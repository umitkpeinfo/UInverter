/**
 * @file    ul_drivers.c
 * @brief   UltraLogic R1 — 3-phase SVPWM inverter driver
 *
 * Hardware configuration (MX-aligned):
 *   SYSCLK  = 216 MHz  (HSE 25 MHz, PLLN=432, PLLP=2, overdrive enabled)
 *   APB2    = 108 MHz  (SYSCLK/2)
 *   TIM1    = 216 MHz input  (2×APB2), PSC=0 → 216 MHz counter, 2.0 µs dead time
 *   BKIN    = PE15, active LOW (CUR_TRIP goes LOW on overcurrent)
 *   Gate driver polarity: active LOW (HIGH = IGBT OFF)
 *   Idle state: OCx/OCxN = SET (HIGH) → all IGBTs OFF when MOE cleared
 *
 * Safety architecture:
 *   - MOE is OFF at boot; only UL_SVPWM_Enable() can set it after precharge
 *   - Any fault immediately clears MOE, zeros CCR, opens charge relay
 *   - BKIN hardware kills PWM in one TIM1 clock cycle (~6 ns)
 *   - Overvoltage requires 3 consecutive ISR readings before trip (noise filter)
 *   - No USB command can directly enable MOE (must go through state machine)
 *
 * Copyright (c) 2026 PE Info.  All rights reserved.
 */

#include "ul_drivers.h"
#include "main.h"

extern TIM_HandleTypeDef htim1;
extern ADC_HandleTypeDef hadc1;
extern ADC_HandleTypeDef hadc3;

/* ── Output enable flag (set only by UL_SVPWM_Enable, cleared on any fault) ─ */

static volatile uint8_t _svpwm_enabled = 0;

/* ── Drive state — declared early so BKIN ISR can set FAULT atomically ────── */

static volatile DrvState_t drv_state = DRV_STATE_IDLE;

/* ── Angle accumulator ───────────────────────────────────────────────── */

/**
 * Electrical angle is tracked in milli-centi-degrees (0..35,999,999)
 * for integer-only accumulation without drift.
 *   360° × 100,000 = 36,000,000 counts per revolution
 *   LUT index = angle_accum / 100,000  →  0..359
 */
#define ANGLE_FULL       36000000U
#define ANGLE_TO_IDX     100000U

static uint32_t angle_accum;
static uint32_t angle_step;  /* increment per ISR, set by _recalc_params() */

static volatile uint32_t svpwm_out_freq  = SVPWM_DEF_OUT_FREQ_HZ;
static volatile uint32_t svpwm_sw_freq   = SVPWM_DEF_SW_FREQ_HZ;
static volatile uint32_t svpwm_mod_index = SVPWM_DEF_MOD_INDEX;

/* ── Sine LUT: 360 entries, 0-10000 range (5000 = zero crossing) ───── */

static const uint16_t sine_lut[SVPWM_LUT_SIZE] = {
    5000, 5087, 5174, 5262, 5349, 5436, 5523, 5609, 5696, 5782, 5868, 5954,
    6040, 6125, 6210, 6294, 6378, 6462, 6545, 6628, 6710, 6792, 6873, 6954,
    7034, 7113, 7192, 7270, 7347, 7424, 7500, 7575, 7650, 7723, 7796, 7868,
    7939, 8009, 8078, 8147, 8214, 8280, 8346, 8410, 8473, 8536, 8597, 8657,
    8716, 8774, 8830, 8886, 8940, 8993, 9045, 9096, 9145, 9193, 9240, 9286,
    9330, 9373, 9415, 9455, 9494, 9532, 9568, 9603, 9636, 9668, 9698, 9728,
    9755, 9782, 9806, 9830, 9851, 9872, 9891, 9908, 9924, 9938, 9951, 9963,
    9973, 9981, 9988, 9993, 9997, 9999,10000, 9999, 9997, 9993, 9988, 9981,
    9973, 9963, 9951, 9938, 9924, 9908, 9891, 9872, 9851, 9830, 9806, 9782,
    9755, 9728, 9698, 9668, 9636, 9603, 9568, 9532, 9494, 9455, 9415, 9373,
    9330, 9286, 9240, 9193, 9145, 9096, 9045, 8993, 8940, 8886, 8830, 8774,
    8716, 8657, 8597, 8536, 8473, 8410, 8346, 8280, 8214, 8147, 8078, 8009,
    7939, 7868, 7796, 7723, 7650, 7575, 7500, 7424, 7347, 7270, 7192, 7113,
    7034, 6954, 6873, 6792, 6710, 6628, 6545, 6462, 6378, 6294, 6210, 6125,
    6040, 5954, 5868, 5782, 5696, 5609, 5523, 5436, 5349, 5262, 5174, 5087,
    5000, 4913, 4826, 4738, 4651, 4564, 4477, 4391, 4304, 4218, 4132, 4046,
    3960, 3875, 3790, 3706, 3622, 3538, 3455, 3372, 3290, 3208, 3127, 3046,
    2966, 2887, 2808, 2730, 2653, 2576, 2500, 2425, 2350, 2277, 2204, 2132,
    2061, 1991, 1922, 1853, 1786, 1720, 1654, 1590, 1527, 1464, 1403, 1343,
    1284, 1226, 1170, 1114, 1060, 1007,  955,  904,  855,  807,  760,  714,
     670,  627,  585,  545,  506,  468,  432,  397,  364,  332,  302,  272,
     245,  218,  194,  170,  149,  128,  109,   92,   76,   62,   49,   37,
      27,   19,   12,    7,    3,    1,    0,    1,    3,    7,   12,   19,
      27,   37,   49,   62,   76,   92,  109,  128,  149,  170,  194,  218,
     245,  272,  302,  332,  364,  397,  432,  468,  506,  545,  585,  627,
     670,  714,  760,  807,  855,  904,  955, 1007, 1060, 1114, 1170, 1226,
    1284, 1343, 1403, 1464, 1527, 1590, 1654, 1720, 1786, 1853, 1922, 1991,
    2061, 2132, 2204, 2277, 2350, 2425, 2500, 2576, 2653, 2730, 2808, 2887,
    2966, 3046, 3127, 3208, 3290, 3372, 3455, 3538, 3622, 3706, 3790, 3875,
    3960, 4046, 4132, 4218, 4304, 4391, 4477, 4564, 4651, 4738, 4826, 4913
};

/* ══════════════════════════════════════════════════════════════════════
 *  Fault System
 * ══════════════════════════════════════════════════════════════════════ */

static volatile uint16_t fault_flags = FAULT_NONE;

uint16_t UL_Fault_Get(void)       { return fault_flags; }
uint8_t  UL_Fault_IsTripped(void) { return fault_flags != FAULT_NONE ? 1U : 0U; }

/**
 * Latch one or more fault bits and force a safe shutdown:
 *   1. Atomically OR mask into fault_flags (interrupt-safe against BKIN ISR)
 *   2. Clear MOE → idle-state outputs (SET = HIGH → IGBTs OFF)
 *   3. Zero CCR so no duty is queued for the next cycle
 *   4. Open the charge relay to isolate the DC bus
 *   5. Transition drv_state to FAULT immediately (no tick delay)
 */
void UL_Fault_Set(uint16_t mask)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    fault_flags |= mask;
    __set_PRIMASK(primask);

    TIM_TypeDef *tim = htim1.Instance;
    tim->BDTR &= ~TIM_BDTR_MOE;
    __DSB();
    tim->CCR1 = 0;
    tim->CCR2 = 0;
    tim->CCR3 = 0;
    _svpwm_enabled = 0;

    UL_ChargeSwitch(0);
    drv_state = DRV_STATE_FAULT;
}

void UL_Fault_Clear(void)
{
    if (fault_flags == FAULT_NONE) return;

    TIM_TypeDef *tim = htim1.Instance;
    tim->BDTR &= ~TIM_BDTR_MOE;
    tim->CCR1 = 0;
    tim->CCR2 = 0;
    tim->CCR3 = 0;
    _svpwm_enabled = 0;

    fault_flags = FAULT_NONE;
}

/* ══════════════════════════════════════════════════════════════════════
 *  BKIN ISR entry — called from TIM1_BRK_TIM9_IRQHandler
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * Called from TIM1_BRK_TIM9_IRQHandler when BKIN fires (PE15 goes LOW).
 * MOE is already cleared by hardware at this point (~5 ns); we zero CCR,
 * open the charge relay, latch the fault flag, and transition drv_state
 * so the full software safe-state converges immediately (no 50 ms gap).
 */
void UL_BKIN_IRQHandler(void)
{
    TIM_TypeDef *tim = htim1.Instance;
    tim->CCR1 = 0;
    tim->CCR2 = 0;
    tim->CCR3 = 0;
    _svpwm_enabled = 0;

    UL_ChargeSwitch(0);

    fault_flags |= FAULT_OVERCURRENT;
    drv_state = DRV_STATE_FAULT;
}

/* ══════════════════════════════════════════════════════════════════════
 *  ISR Measurements — updated every switching cycle
 * ══════════════════════════════════════════════════════════════════════ */

static UL_Meas_t isr_meas;

const UL_Meas_t *UL_Meas_Get(void) { return &isr_meas; }

static float _vbus_from_raw(uint16_t raw)
{
    uint32_t adc16 = (uint32_t)raw << 4;
    float v = VBUS_M_OFFSET + (float)adc16 * VBUS_M_GAIN;
    return (v < 0.0f) ? 0.0f : v;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Dynamic Braking (Regen) — BRK_ON / BRK_EN on PD13 / PD14
 * ══════════════════════════════════════════════════════════════════════ */

static volatile uint8_t regen_active = 0;

void UL_Regen_Service(float v_bus)
{
    if (v_bus > VBUS_REGEN_ON_V) {
        if (!regen_active) {
            HAL_GPIO_WritePin(BRK_EN_GPIO_Port, BRK_EN_Pin, GPIO_PIN_SET);
            HAL_GPIO_WritePin(BRK_ON_GPIO_Port, BRK_ON_Pin, GPIO_PIN_SET);
            regen_active = 1;
        }
    } else if (v_bus < VBUS_REGEN_OFF_V) {
        if (regen_active) {
            HAL_GPIO_WritePin(BRK_ON_GPIO_Port, BRK_ON_Pin, GPIO_PIN_RESET);
            HAL_GPIO_WritePin(BRK_EN_GPIO_Port, BRK_EN_Pin, GPIO_PIN_RESET);
            regen_active = 0;
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  Dead-time DTG calculation — pure 32-bit, ceiling-rounded
 *
 *  STM32 TIM1 DTG[7:0] encoding (RM0410 §25.4.18):
 *    0xxxxxxx → DT =         DTG[6:0]       ×  t_DTS
 *    10xxxxxx → DT = (64  + DTG[5:0]) × 2  ×  t_DTS
 *    110xxxxx → DT = (32  + DTG[4:0]) × 8  ×  t_DTS
 *    111xxxxx → DT = (32  + DTG[4:0]) × 16 ×  t_DTS
 *
 *  tim_clk_hz = CK_INT (before prescaler), NOT the counter clock.
 * ══════════════════════════════════════════════════════════════════════ */

static uint8_t _calc_dtg(uint32_t tim_clk_hz, uint32_t deadtime_ns)
{
    uint32_t clk_mhz  = (tim_clk_hz + 999999U) / 1000000U;
    uint32_t dt_ticks  = (deadtime_ns * clk_mhz + 999U) / 1000U;

    if (dt_ticks <= 127U)
        return (uint8_t)dt_ticks;

    if (dt_ticks <= 254U)
    {
        uint32_t n = (dt_ticks + 1U) / 2U;
        if (n < 64U)  n = 64U;
        if (n > 127U) n = 127U;
        return (uint8_t)(0x80U | ((n - 64U) & 0x3FU));
    }
    if (dt_ticks <= 504U)
    {
        uint32_t n = (dt_ticks + 7U) / 8U;
        if (n < 32U) n = 32U;
        if (n > 63U) n = 63U;
        return (uint8_t)(0xC0U | ((n - 32U) & 0x1FU));
    }
    if (dt_ticks <= 1008U)
    {
        uint32_t n = (dt_ticks + 15U) / 16U;
        if (n < 32U) n = 32U;
        if (n > 63U) n = 63U;
        return (uint8_t)(0xE0U | ((n - 32U) & 0x1FU));
    }
    return 0xFFU;
}

/* ══════════════════════════════════════════════════════════════════════
 *  TIM1 Clock Detection & Parameter Calculation
 * ══════════════════════════════════════════════════════════════════════ */

static uint32_t _tim1_clk_hz = SVPWM_TIM_CLK;

/** Derive TIM1 input clock from the RCC configuration at runtime. */
static uint32_t _get_tim1_clk(void)
{
    RCC_ClkInitTypeDef clk;
    uint32_t lat;
    HAL_RCC_GetClockConfig(&clk, &lat);
    uint32_t pclk2 = HAL_RCC_GetPCLK2Freq();
    return (clk.APB2CLKDivider == RCC_HCLK_DIV1) ? pclk2 : (2U * pclk2);
}

/**
 * Recalculate ARR and angle_step from current output/switching frequencies.
 * Called whenever f_out or f_sw changes, or during init.
 *
 * ARR = counter_clock / (2 × f_sw)     (center-aligned → 2× triangle)
 * angle_step = f_out × 36,000,000 / f_sw  (split multiply to avoid u32 overflow)
 */
static void _recalc_params(void)
{
    uint32_t psc = htim1.Instance->PSC;
    uint32_t cnt_clk = _tim1_clk_hz / (psc + 1U);
    uint32_t arr = cnt_clk / (2U * svpwm_sw_freq);
    uint32_t step = (uint32_t)((uint64_t)svpwm_out_freq * 10U * 3600000ULL
                               / svpwm_sw_freq);
    if (step == 0) step = 1;

    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    __HAL_TIM_SET_AUTORELOAD(&htim1, arr);
    angle_step = step;
    __set_PRIMASK(primask);
}

static volatile uint8_t _test_mode = 0;

/* Snapshots captured after UL_SVPWM_Init() for debugger inspection */
static volatile uint32_t _dbg_bdtr;
static volatile uint32_t _dbg_ccer;
static volatile uint8_t  _dbg_dtg;

/* ══════════════════════════════════════════════════════════════════════
 *  ADC Injected Channel Setup
 *
 *  ADC1 injected: channel 5 (PA5 / SHUNT1_AN) — phase U current
 *  ADC3 injected: channel 7 (PF9 / SHUNT3_AN) — phase W current
 *                 channel 6 (PF8 / VBUS_MON)   — DC bus voltage
 *
 *  Trigger: TIM1 TRGO (UPDATE event, once per PWM period)
 * ══════════════════════════════════════════════════════════════════════ */

void UL_ADC_InjectInit(void)
{
    ADC_InjectionConfTypeDef sInjConfig = {0};
    HAL_StatusTypeDef rc;

    sInjConfig.InjectedNbrOfConversion    = 1;
    sInjConfig.InjectedDiscontinuousConvMode = DISABLE;
    sInjConfig.AutoInjectedConv           = DISABLE;
    sInjConfig.ExternalTrigInjecConv      = ADC_EXTERNALTRIGINJECCONV_T1_TRGO;
    sInjConfig.ExternalTrigInjecConvEdge  = ADC_EXTERNALTRIGINJECCONVEDGE_RISING;

    sInjConfig.InjectedChannel      = ADC_CHANNEL_5;
    sInjConfig.InjectedRank         = ADC_INJECTED_RANK_1;
    sInjConfig.InjectedSamplingTime = ADC_SAMPLETIME_15CYCLES;
    sInjConfig.InjectedOffset       = 0;
    rc = HAL_ADCEx_InjectedConfigChannel(&hadc1, &sInjConfig);
    if (rc != HAL_OK) { UL_Fault_Set(FAULT_PRECHARGE); return; }

    sInjConfig.InjectedNbrOfConversion = 2;
    sInjConfig.InjectedChannel      = ADC_CHANNEL_7;
    sInjConfig.InjectedRank         = ADC_INJECTED_RANK_1;
    sInjConfig.InjectedSamplingTime = ADC_SAMPLETIME_15CYCLES;
    sInjConfig.InjectedOffset       = 0;
    rc = HAL_ADCEx_InjectedConfigChannel(&hadc3, &sInjConfig);
    if (rc != HAL_OK) { UL_Fault_Set(FAULT_PRECHARGE); return; }

    sInjConfig.InjectedChannel      = ADC_CHANNEL_6;
    sInjConfig.InjectedRank         = ADC_INJECTED_RANK_2;
    rc = HAL_ADCEx_InjectedConfigChannel(&hadc3, &sInjConfig);
    if (rc != HAL_OK) { UL_Fault_Set(FAULT_PRECHARGE); return; }

    rc = HAL_ADCEx_InjectedStart(&hadc1);
    if (rc != HAL_OK) { UL_Fault_Set(FAULT_PRECHARGE); return; }

    rc = HAL_ADCEx_InjectedStart(&hadc3);
    if (rc != HAL_OK) { UL_Fault_Set(FAULT_PRECHARGE); return; }
}

/* ══════════════════════════════════════════════════════════════════════
 *  Init: start 6-ch complementary PWM + update ISR
 *
 *  ALL BDTR / CCER / MOE writes are DIRECT REGISTER operations.
 *  The entire sequence runs with interrupts disabled.
 * ══════════════════════════════════════════════════════════════════════ */

void UL_SVPWM_Init(void)
{
    TIM_TypeDef *tim = htim1.Instance;

    _tim1_clk_hz = _get_tim1_clk();

    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    angle_accum = 0;

    /* 1. Force safe state */
    tim->CR1  &= ~TIM_CR1_CEN;
    tim->BDTR &= ~TIM_BDTR_MOE;
    tim->DIER &= ~TIM_DIER_UIE;
    __DSB();

    /* 2. Clear all pending flags */
    tim->SR = 0U;

    /* 3. RCR = 1 → one update per full PWM cycle */
    tim->RCR = 1;

    /* 4. Recalculate ARR + angle_step */
    _recalc_params();

    /* 5. Set initial compare values — ALL ZERO (outputs OFF) */
    tim->CCR1 = 0;
    tim->CCR2 = 0;
    tim->CCR3 = 0;

    /* 6. BDTR — dead time + break enable (read-modify-write to preserve BKF)
     *    BKP=0 (active LOW, matching CUR_TRIP hardware)
     *    BKE=1 (hardware break enabled)
     *    OSSR=1 (off-state in RUN: outputs driven to idle level)
     *    OSSI=1 (off-state in IDLE: outputs driven to idle level)
     *    BKF is set by HAL_TIMEx_ConfigBreakDeadTime (filter = 4) and must
     *    be preserved — a full register write would clear it to 0.
     *    MOE is NOT set here — see UL_SVPWM_Enable() */
    uint8_t dtg = _calc_dtg(_tim1_clk_hz, SVPWM_DEADTIME_NS);
    _dbg_dtg = dtg;

    {
        uint32_t bdtr = tim->BDTR;
        bdtr &= ~(TIM_BDTR_DTG_Msk | TIM_BDTR_MOE);
        bdtr |= (uint32_t)dtg
              | TIM_BDTR_OSSR
              | TIM_BDTR_OSSI
              | TIM_BDTR_BKE;
        tim->BDTR = bdtr;
        __DSB();
    }

    /* 7. Enable output channels in CCER (Mode 2 + active-LOW polarity) */
    if (_test_mode) {
        tim->CCER = TIM_CCER_CC1E  | TIM_CCER_CC1P
                  | TIM_CCER_CC1NE | TIM_CCER_CC1NP;
    } else {
        tim->CCER = TIM_CCER_CC1E  | TIM_CCER_CC1P
                  | TIM_CCER_CC1NE | TIM_CCER_CC1NP
                  | TIM_CCER_CC2E  | TIM_CCER_CC2P
                  | TIM_CCER_CC2NE | TIM_CCER_CC2NP
                  | TIM_CCER_CC3E  | TIM_CCER_CC3P
                  | TIM_CCER_CC3NE | TIM_CCER_CC3NP;
    }

    /* 8. Configure update interrupt (SVPWM ISR) */
    HAL_NVIC_SetPriority(TIM1_UP_TIM10_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(TIM1_UP_TIM10_IRQn);
    if (!_test_mode)
        tim->DIER |= TIM_DIER_UIE;

    /* 9. Configure BKIN interrupt (priority 0 — highest) */
    HAL_NVIC_SetPriority(TIM1_BRK_TIM9_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(TIM1_BRK_TIM9_IRQn);
    tim->DIER |= TIM_DIER_BIE;

    /* 10. Force update to load shadow registers, clear flags */
    tim->EGR = TIM_EGR_UG;
    tim->SR  = 0U;
    __DSB();

    /* 11. Start counter, but keep MOE OFF — outputs remain disabled.
     *     MOE is only set by UL_SVPWM_Enable() when the drive state
     *     machine reaches RUN after a successful precharge. */
    tim->CR1  |= TIM_CR1_CEN;

    __set_PRIMASK(primask);

    _dbg_bdtr = tim->BDTR;
    _dbg_ccer = tim->CCER;
}

/* ══════════════════════════════════════════════════════════════════════
 *  SVPWM Enable / Disable — controls MOE (Main Output Enable)
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * Check if CUR_TRIP is stuck active (PD10 = BRK_CUR_CPU, active LOW).
 * Reads the pin multiple times — if it never reads HIGH, the trip line
 * is latched or shorted and we must not enable MOE.
 * Mirrors the M72 reference firmware's 10-read secondary check.
 */
static uint8_t _cur_trip_stuck(void)
{
    for (int i = 0; i < 10; i++) {
        if (HAL_GPIO_ReadPin(BRK_CUR_CPU_GPIO_Port, BRK_CUR_CPU_Pin)
            == GPIO_PIN_SET)
            return 0;
    }
    return 1;
}

/**
 * Enable SVPWM output — the ONLY place MOE is set.
 * Pre-loads CCR to 50% duty (neutral voltage) so the first PWM cycle
 * does not produce a current spike before the ISR computes real duties.
 *
 * Refuses to enable if CUR_TRIP (PD10) is stuck low — sets F1 instead.
 */
void UL_SVPWM_Enable(void)
{
    if (UL_Fault_IsTripped()) return;

    if (_cur_trip_stuck()) {
        UL_Fault_Set(FAULT_OVERCURRENT);
        return;
    }

    TIM_TypeDef *tim = htim1.Instance;

    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    uint32_t arr = tim->ARR;
    tim->CCR1 = arr / 2U;
    tim->CCR2 = arr / 2U;
    tim->CCR3 = arr / 2U;
    angle_accum = 0;

    tim->EGR = TIM_EGR_UG;
    tim->SR  = 0U;
    __DSB();

    _svpwm_enabled = 1;
    tim->BDTR |= TIM_BDTR_MOE;

    __set_PRIMASK(primask);
}

void UL_SVPWM_Disable(void)
{
    TIM_TypeDef *tim = htim1.Instance;

    tim->BDTR &= ~TIM_BDTR_MOE;
    __DSB();

    tim->CCR1 = 0;
    tim->CCR2 = 0;
    tim->CCR3 = 0;
    _svpwm_enabled = 0;
}

uint8_t UL_SVPWM_IsEnabled(void)
{
    return _svpwm_enabled;
}

/* ══════════════════════════════════════════════════════════════════════
 *  SVPWM ISR — called at switching frequency from TIM1 update
 *
 *  Reads ADC injected results from previous trigger, performs
 *  bus voltage protection and regen, then computes new duty cycles.
 * ══════════════════════════════════════════════════════════════════════ */

void UL_SVPWM_ISR(void)
{
    /* --- Read ADC injected results (from previous trigger) --- */
    uint16_t s1 = (uint16_t)(ADC1->JDR1 & 0xFFFU);
    uint16_t s3 = (uint16_t)(ADC3->JDR1 & 0xFFFU);

    isr_meas.shunt1_raw = s1;
    isr_meas.shunt3_raw = s3;
    isr_meas.vbus_raw   = (uint16_t)(ADC3->JDR2 & 0xFFFU);
    isr_meas.v_bus      = _vbus_from_raw(isr_meas.vbus_raw);
    isr_meas.i_u        = (float)s1 * HALL_A_PER_LSB - HALL_OFFSET_A;
    isr_meas.i_w        = (float)s3 * HALL_A_PER_LSB - HALL_OFFSET_A;

    /* --- Bus voltage protection (3 consecutive readings, matching M72) --- */
    {
        static uint8_t ov_trip_count = 0;
        if (isr_meas.v_bus > VBUS_OV_TRIP_V) {
            if (++ov_trip_count >= 3) {
                UL_Fault_Set(FAULT_OVERVOLTAGE);
                return;
            }
        } else {
            ov_trip_count = 0;
        }
    }

    /* --- Dynamic braking (regen) --- */
    UL_Regen_Service(isr_meas.v_bus);

    /* --- SVPWM duty calculation (sine LUT + min-max zero-sequence injection) ---
     *
     * 1. Look up three 120°-spaced sine values (0..10000, mid=5000)
     * 2. Inject zero-sequence: offset = 5000 − (max+min)/2
     *    This centers the waveforms and extends linear range to 2/√3 ≈ 1.155
     * 3. Apply modulation index (per-mille) around the 5000 midpoint
     * 4. Scale to CCR counts:  duty = result × ARR / 10000
     */
    if (!_svpwm_enabled || _test_mode) return;

    uint16_t idx_u = (uint16_t)(angle_accum / ANGLE_TO_IDX) % 360U;
    uint16_t idx_v = (idx_u + 120U) % 360U;
    uint16_t idx_w = (idx_u + 240U) % 360U;

    uint16_t su = sine_lut[idx_u];
    uint16_t sv = sine_lut[idx_v];
    uint16_t sw = sine_lut[idx_w];

    uint16_t mx = su, mn = su;
    if (sv > mx) mx = sv;  if (sv < mn) mn = sv;
    if (sw > mx) mx = sw;  if (sw < mn) mn = sw;
    int16_t ofs = 5000 - (int16_t)((mx + mn) >> 1);

    int32_t m   = (int32_t)svpwm_mod_index;
    uint32_t arr = __HAL_TIM_GET_AUTORELOAD(&htim1);

    int32_t ru = 5000 + ((int32_t)su + ofs - 5000) * m / 1000;
    int32_t rv = 5000 + ((int32_t)sv + ofs - 5000) * m / 1000;
    int32_t rw = 5000 + ((int32_t)sw + ofs - 5000) * m / 1000;
    if (ru < 0) ru = 0; if (ru > 10000) ru = 10000;
    if (rv < 0) rv = 0; if (rv > 10000) rv = 10000;
    if (rw < 0) rw = 0; if (rw > 10000) rw = 10000;

    uint32_t du = (uint32_t)ru * arr / 10000U;
    uint32_t dv = (uint32_t)rv * arr / 10000U;
    uint32_t dw = (uint32_t)rw * arr / 10000U;

    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, du);
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, dv);
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_3, dw);

    angle_accum += angle_step;
    if (angle_accum >= ANGLE_FULL)
        angle_accum -= ANGLE_FULL;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Runtime setters (called from CDC command parser)
 * ══════════════════════════════════════════════════════════════════════ */

void UL_SVPWM_SetOutFreq(uint32_t freq_hz)   /* 1..400 Hz */
{
    if (freq_hz == 0 || freq_hz > 400) return;
    svpwm_out_freq = freq_hz;
    _recalc_params();
}

void UL_SVPWM_SetSwFreq(uint32_t freq_hz)    /* 1..16 kHz (FP15R12W1T4 thermal limit) */
{
    if (freq_hz < 1000 || freq_hz > 16000) return;
    svpwm_sw_freq = freq_hz;
    _recalc_params();
}

void UL_SVPWM_SetModIndex(uint32_t mod_permille) /* 0..1155 (max with SVPWM) */
{
    if (mod_permille > 1155) return;
    svpwm_mod_index = mod_permille;
}

void UL_SVPWM_SetTestMode(uint8_t enable)
{
    _test_mode = (enable != 0) ? 1 : 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  DC Bus Pre-charge
 * ══════════════════════════════════════════════════════════════════════ */

void UL_ChargeSwitch(uint8_t enable)
{
    HAL_GPIO_WritePin(CHG_OUT_CPU_GPIO_Port, CHG_OUT_CPU_Pin,
                      enable ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

uint8_t UL_ChargeSwitch_State(void)
{
    return (uint8_t)HAL_GPIO_ReadPin(CHG_OUT_CPU_GPIO_Port, CHG_OUT_CPU_Pin);
}

/* ══════════════════════════════════════════════════════════════════════
 *  TIM1 Register Readback — used by GET:REG USB command and telemetry
 * ══════════════════════════════════════════════════════════════════════ */

uint32_t UL_SVPWM_ReadBDTR(void) { return htim1.Instance->BDTR; }
uint32_t UL_SVPWM_ReadCCER(void) { return htim1.Instance->CCER; }
uint32_t UL_SVPWM_ReadCR1(void)  { return htim1.Instance->CR1;  }

/* ══════════════════════════════════════════════════════════════════════
 *  ADC — DC Bus Voltage Monitor
 *
 *  Primary path: ISR-cached isr_meas values (updated at f_sw).
 *  Fallback: polled ADC3 regular conversion, used only during startup
 *  before the first TIM1 update event triggers injected sampling.
 * ══════════════════════════════════════════════════════════════════════ */

/** Polled single-shot read — startup fallback only, not ISR-safe. */
static uint16_t _read_adc3_channel(uint32_t channel)
{
    ADC_ChannelConfTypeDef cfg = {0};
    cfg.Channel     = channel;
    cfg.Rank        = ADC_REGULAR_RANK_1;
    cfg.SamplingTime = ADC_SAMPLETIME_56CYCLES;

    if (HAL_ADC_ConfigChannel(&hadc3, &cfg) != HAL_OK)
        return 0;

    HAL_ADC_Start(&hadc3);
    if (HAL_ADC_PollForConversion(&hadc3, 10) != HAL_OK)
    {
        HAL_ADC_Stop(&hadc3);
        return 0;
    }
    uint16_t val = (uint16_t)HAL_ADC_GetValue(&hadc3);
    HAL_ADC_Stop(&hadc3);
    return val;
}

uint16_t UL_ReadVbusMon_Raw(void)
{
    if (isr_meas.vbus_raw != 0)
        return isr_meas.vbus_raw;
    return _read_adc3_channel(ADC_CHANNEL_6);
}

float UL_ReadVbusMon(void)
{
    if (isr_meas.v_bus > 0.0f)
        return isr_meas.v_bus;
    uint32_t adc16 = (uint32_t)UL_ReadVbusMon_Raw() << 4;
    float vbus = VBUS_M_OFFSET + (float)adc16 * VBUS_M_GAIN;
    if (vbus < 0.0f) vbus = 0.0f;
    return vbus;
}

/* ══════════════════════════════════════════════════════════════════════
 *  DC Bus Pre-charge State Machine
 * ══════════════════════════════════════════════════════════════════════ */

static volatile ChgState_t  chg_state      = CHG_STATE_IDLE;
static volatile ChgFault_t  chg_fault      = CHG_FAULT_NONE;
static          float        chg_vbus_last  = 0.0f;
static          float        chg_vbus_prev  = 0.0f;
static          float        chg_vbus_close = 0.0f;
static          uint32_t     chg_elapsed_ms = 0;
static          uint32_t     chg_stable_n   = 0;

static float _fabsf(float x) { return x < 0.0f ? -x : x; }

void UL_Charge_Start(void)
{
    UL_ChargeSwitch(0);
    chg_vbus_prev  = 0.0f;
    chg_vbus_close = 0.0f;
    chg_elapsed_ms = 0;
    chg_stable_n   = 0;
    chg_fault      = CHG_FAULT_NONE;
    chg_state      = CHG_STATE_PRECHARGE;
}

void UL_Charge_Stop(void)
{
    UL_ChargeSwitch(0);
    chg_state = CHG_STATE_IDLE;
    chg_fault = CHG_FAULT_NONE;
}

void UL_Charge_ClearFault(void)
{
    if (chg_state == CHG_STATE_FAULT) {
        UL_ChargeSwitch(0);
        chg_state = CHG_STATE_IDLE;
        chg_fault = CHG_FAULT_NONE;
    }
}

void UL_Charge_Tick(void)
{
    float vbus    = UL_ReadVbusMon();
    chg_vbus_last = vbus;

    if (chg_state != CHG_STATE_IDLE && chg_state != CHG_STATE_FAULT) {
        if (vbus > CHG_OV_V) {
            UL_ChargeSwitch(0);
            chg_fault = CHG_FAULT_OVERVOLTAGE;
            chg_state = CHG_STATE_FAULT;
            return;
        }
    }

    switch (chg_state) {

    case CHG_STATE_IDLE:
        break;

    case CHG_STATE_PRECHARGE:
        chg_elapsed_ms += CHG_TICK_MS;

        if (chg_elapsed_ms > CHG_TIMEOUT_MS) {
            chg_fault = (vbus < CHG_MIN_V)
                        ? CHG_FAULT_NO_CHARGE
                        : CHG_FAULT_TIMEOUT;
            chg_state = CHG_STATE_FAULT;
            return;
        }

        if (vbus >= CHG_MIN_V) {
            if (_fabsf(vbus - chg_vbus_prev) < CHG_STABLE_DV)
                chg_stable_n++;
            else
                chg_stable_n = 0;
        }
        chg_vbus_prev = vbus;

        if (chg_stable_n >= CHG_STABLE_N) {
            UL_ChargeSwitch(1);
            chg_vbus_close = vbus;
            chg_elapsed_ms = 0;
            chg_state      = CHG_STATE_VERIFY;
        }
        break;

    case CHG_STATE_VERIFY:
        chg_elapsed_ms += CHG_TICK_MS;

        if (vbus < chg_vbus_close - CHG_COLLAPSE_V) {
            UL_ChargeSwitch(0);
            chg_fault = CHG_FAULT_BUS_COLLAPSE;
            chg_state = CHG_STATE_FAULT;
            return;
        }

        if (chg_elapsed_ms >= CHG_VERIFY_MS) {
            chg_state = CHG_STATE_RUNNING;
        }
        break;

    case CHG_STATE_RUNNING:
        if (vbus < CHG_MIN_V) {
            UL_ChargeSwitch(0);
            chg_fault = CHG_FAULT_BUS_COLLAPSE;
            chg_state = CHG_STATE_FAULT;
        }
        break;

    case CHG_STATE_FAULT:
        break;
    }
}

ChgState_t  UL_Charge_GetState(void) { return chg_state;     }
ChgFault_t  UL_Charge_GetFault(void) { return chg_fault;     }
float       UL_Charge_GetVbus(void)  { return chg_vbus_last; }
uint8_t     UL_Charge_BusReady(void) { return chg_state == CHG_STATE_RUNNING ? 1U : 0U; }

/* ══════════════════════════════════════════════════════════════════════
 *  Drive State Machine
 *
 *  IDLE → PRECHARGE → READY → RUN → STOPPING → IDLE
 *                                                 ↑
 *  Any state → FAULT → (clear) ───────────────────┘
 * ══════════════════════════════════════════════════════════════════════ */

DrvState_t UL_Drive_GetState(void) { return drv_state; }

void UL_Drive_Start(void)
{
    if (drv_state != DRV_STATE_IDLE) return;
    if (UL_Fault_IsTripped()) return;
    UL_Charge_Start();
    drv_state = DRV_STATE_PRECHARGE;
}

void UL_Drive_Run(void)
{
    if (drv_state != DRV_STATE_READY) return;
    if (UL_Fault_IsTripped()) {
        drv_state = DRV_STATE_FAULT;
        return;
    }
    UL_SVPWM_Enable();
    if (UL_Fault_IsTripped()) {
        drv_state = DRV_STATE_FAULT;
        return;
    }
    drv_state = DRV_STATE_RUN;
}

void UL_Drive_Stop(void)
{
    if (drv_state == DRV_STATE_IDLE || drv_state == DRV_STATE_FAULT) return;

    UL_SVPWM_Disable();
    UL_ChargeSwitch(0);
    UL_Charge_Stop();
    regen_active = 0;
    HAL_GPIO_WritePin(BRK_ON_GPIO_Port, BRK_ON_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(BRK_EN_GPIO_Port, BRK_EN_Pin, GPIO_PIN_RESET);
    drv_state = DRV_STATE_IDLE;
}

void UL_Drive_Reset(void)
{
    UL_SVPWM_Disable();
    UL_ChargeSwitch(0);
    UL_Charge_Stop();
    UL_Fault_Clear();
    regen_active = 0;
    HAL_GPIO_WritePin(BRK_ON_GPIO_Port, BRK_ON_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(BRK_EN_GPIO_Port, BRK_EN_Pin, GPIO_PIN_RESET);
    drv_state = DRV_STATE_IDLE;
}

void UL_Drive_Tick(void)
{
    if (UL_Fault_IsTripped() && drv_state != DRV_STATE_FAULT) {
        UL_SVPWM_Disable();
        UL_ChargeSwitch(0);
        drv_state = DRV_STATE_FAULT;
        return;
    }

    switch (drv_state) {

    case DRV_STATE_IDLE:
        break;

    case DRV_STATE_PRECHARGE:
        UL_Charge_Tick();
        if (UL_Charge_GetState() == CHG_STATE_RUNNING) {
            drv_state = DRV_STATE_READY;
        } else if (UL_Charge_GetState() == CHG_STATE_FAULT) {
            UL_Fault_Set(FAULT_PRECHARGE);
            drv_state = DRV_STATE_FAULT;
        }
        break;

    case DRV_STATE_READY:
        if (isr_meas.v_bus < VBUS_UV_TRIP_V) {
            UL_Fault_Set(FAULT_BUS_COLLAPSE);
        }
        break;

    case DRV_STATE_RUN:
        if (isr_meas.v_bus < VBUS_UV_TRIP_V) {
            UL_Fault_Set(FAULT_UNDERVOLTAGE);
        }
        if (_cur_trip_stuck()) {
            UL_Fault_Set(FAULT_OVERCURRENT);
        }
        break;

    case DRV_STATE_STOPPING:
        UL_SVPWM_Disable();
        UL_ChargeSwitch(0);
        UL_Charge_Stop();
        drv_state = DRV_STATE_IDLE;
        break;

    case DRV_STATE_FAULT:
        break;
    }
}

/* ── DS1232 Watchdog Heartbeat ───────────────────────────────────────── */

void UL_Heartbeat_Toggle(void)
{
    HAL_GPIO_TogglePin(SOL_CPU_GPIO_Port, SOL_CPU_Pin);
}
