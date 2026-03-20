/**
 * @file    ul_drivers.c
 * @brief   UltraLogic R1 — 6-channel SVPWM (3 complementary pairs)
 *
 * 60 Hz output, 5 kHz switching, 2-level voltage-source inverter.
 * Min-max (zero-sequence) injection for full SVPWM utilisation.
 *
 * Copyright (c) 2026 PE Info.  All rights reserved.
 */

#include "ul_drivers.h"
#include "main.h"

extern TIM_HandleTypeDef htim1;
extern ADC_HandleTypeDef hadc3;

/* ── Angle accumulator ───────────────────────────────────────────────── */

#define ANGLE_FULL       36000000U
#define ANGLE_TO_IDX     100000U

static uint32_t angle_accum;
static uint32_t angle_step;

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
 *  Dead-time DTG calculation — pure 32-bit, ceiling-rounded
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * STM32 TIM1 DTG[7:0] encoding (RM0410 §25.4.18):
 *   0xxxxxxx → DT =         DTG[6:0]       ×  t_DTS
 *   10xxxxxx → DT = (64  + DTG[5:0]) × 2  ×  t_DTS
 *   110xxxxx → DT = (32  + DTG[4:0]) × 8  ×  t_DTS
 *   111xxxxx → DT = (32  + DTG[4:0]) × 16 ×  t_DTS
 *
 * All intermediate divisions use ceiling so the encoded dead-time
 * is always >= the requested value (safe for shoot-through prevention).
 *
 * dt_ticks = ceil(deadtime_ns × TIM_CLK / 10^9)
 *          = ceil(deadtime_ns × ceil(TIM_CLK / 10^6) / 10^3)   [32-bit]
 */
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
 *  Helpers
 * ══════════════════════════════════════════════════════════════════════ */

static uint32_t _tim1_clk_hz = SVPWM_TIM_CLK;

static uint32_t _get_tim1_clk(void)
{
    RCC_ClkInitTypeDef clk;
    uint32_t lat;
    HAL_RCC_GetClockConfig(&clk, &lat);
    uint32_t pclk2 = HAL_RCC_GetPCLK2Freq();
    return (clk.APB2CLKDivider == RCC_HCLK_DIV1) ? pclk2 : (2U * pclk2);
}

static void _recalc_params(void)
{
    uint32_t arr = _tim1_clk_hz / (2U * svpwm_sw_freq);
    uint32_t step = (uint32_t)((uint64_t)svpwm_out_freq * 10U * 3600000ULL
                               / svpwm_sw_freq);
    if (step == 0) step = 1;

    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    __HAL_TIM_SET_AUTORELOAD(&htim1, arr);
    angle_step = step;
    __set_PRIMASK(primask);
}

static volatile uint32_t _dbg_bdtr;
static volatile uint32_t _dbg_ccer;
static volatile uint8_t  _dbg_dtg;

static volatile uint8_t _test_mode = 0;

/* ══════════════════════════════════════════════════════════════════════
 *  Init: start 6-ch complementary PWM + update ISR
 *
 *  ALL BDTR / CCER / MOE writes are DIRECT REGISTER operations —
 *  no HAL calls — to eliminate HAL channel-state and lock issues
 *  that could silently skip the dead-time or output-enable writes.
 *
 *  The entire sequence runs with interrupts disabled to prevent
 *  preemption during the transition from stopped → configured → running.
 * ══════════════════════════════════════════════════════════════════════ */

void UL_SVPWM_Init(void)
{
    TIM_TypeDef *tim = htim1.Instance;

    _tim1_clk_hz = _get_tim1_clk();

    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    angle_accum = 0;

    /* 1. Force safe state: stop counter, disable outputs, disable ISR */
    tim->CR1  &= ~TIM_CR1_CEN;
    tim->BDTR &= ~TIM_BDTR_MOE;
    tim->DIER &= ~TIM_DIER_UIE;
    __DSB();

    /* 2. Clear all pending flags while outputs are in safe state */
    tim->SR = 0U;

    /* 3. RCR = 1 → one update event per full PWM cycle.
     *    In center-aligned mode, RCR=0 fires at BOTH overflow and
     *    underflow (2× per period), doubling the ISR rate and
     *    output frequency.  RCR=1 fires once per period.           */
    tim->RCR = 1;

    /* 4. Recalculate ARR + angle_step ─────────────────────────────── */
    _recalc_params();

    /* 5. Set initial compare values ───────────────────────────────── */
    uint32_t arr = tim->ARR;
    if (_test_mode) {
        tim->CCR1 = arr / 2U;
        tim->CCR2 = 0;
        tim->CCR3 = 0;
    } else {
        tim->CCR1 = arr / 2U;
        tim->CCR2 = arr / 2U;
        tim->CCR3 = arr / 2U;
    }

    /* 6. BDTR — direct register write (bypasses HAL entirely) ─────── */
    uint8_t dtg = _calc_dtg(_tim1_clk_hz, SVPWM_DEADTIME_NS);
    _dbg_dtg = dtg;

    uint32_t bdtr_val = (uint32_t)dtg
                      | TIM_BDTR_OSSR
                      | TIM_BDTR_OSSI;
    tim->BDTR = bdtr_val;
    __DSB();

    if ((tim->BDTR & TIM_BDTR_DTG) != dtg)
    {
        tim->BDTR = bdtr_val;
        __DSB();
    }

    /* 7. Enable output channels in CCER (direct register write) ───── */
    if (_test_mode) {
        tim->CCER = TIM_CCER_CC1E | TIM_CCER_CC1NE;
    } else {
        tim->CCER = TIM_CCER_CC1E  | TIM_CCER_CC1NE
                  | TIM_CCER_CC2E  | TIM_CCER_CC2NE
                  | TIM_CCER_CC3E  | TIM_CCER_CC3NE;
    }

    /* 8. Configure update interrupt ───────────────────────────────── */
    HAL_NVIC_SetPriority(TIM1_UP_TIM10_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(TIM1_UP_TIM10_IRQn);
    if (!_test_mode)
        tim->DIER |= TIM_DIER_UIE;

    /* 9. Force update to load shadow registers, clear UIF ─────────── */
    tim->EGR = TIM_EGR_UG;
    tim->SR  = 0U;
    __DSB();

    /* 10. Enable MOE, then start counter ──────────────────────────── */
    tim->BDTR |= TIM_BDTR_MOE;
    tim->CR1  |= TIM_CR1_CEN;

    __set_PRIMASK(primask);

    /* 11. Verify: read back registers ─────────────────────────────── */
    _dbg_bdtr = tim->BDTR;
    _dbg_ccer = tim->CCER;
}

/* ══════════════════════════════════════════════════════════════════════
 *  SVPWM Enable / Disable — controls MOE (Main Output Enable)
 *
 *  Disable clears MOE → all outputs go to IDLE (safe state per OSSI).
 *  Timer and ISR keep running so re-enable is instantaneous.
 * ══════════════════════════════════════════════════════════════════════ */

static volatile uint8_t _svpwm_enabled = 1;

void UL_SVPWM_Enable(void)
{
    TIM_TypeDef *tim = htim1.Instance;
    _svpwm_enabled = 1;
    tim->BDTR |= TIM_BDTR_MOE;
}

void UL_SVPWM_Disable(void)
{
    TIM_TypeDef *tim = htim1.Instance;
    _svpwm_enabled = 0;
    tim->BDTR &= ~TIM_BDTR_MOE;
}

uint8_t UL_SVPWM_IsEnabled(void)
{
    return _svpwm_enabled;
}

/* ══════════════════════════════════════════════════════════════════════
 *  SVPWM ISR — called at switching frequency from TIM1 update
 * ══════════════════════════════════════════════════════════════════════ */

void UL_SVPWM_ISR(void)
{
    if (_test_mode) return;
    uint16_t idx_u = (uint16_t)(angle_accum / ANGLE_TO_IDX) % 360U;
    uint16_t idx_v = (idx_u + 120U) % 360U;
    uint16_t idx_w = (idx_u + 240U) % 360U;

    uint16_t su = sine_lut[idx_u];
    uint16_t sv = sine_lut[idx_v];
    uint16_t sw = sine_lut[idx_w];

    /* Min-max (zero-sequence) injection for SVPWM */
    uint16_t mx = su, mn = su;
    if (sv > mx) mx = sv;  if (sv < mn) mn = sv;
    if (sw > mx) mx = sw;  if (sw < mn) mn = sw;
    int16_t ofs = 5000 - (int16_t)((mx + mn) >> 1);

    /* Scale by modulation index and map to timer ARR */
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

    angle_accum = (angle_accum + angle_step) % ANGLE_FULL;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Runtime setters (called from CDC command parser)
 * ══════════════════════════════════════════════════════════════════════ */

void UL_SVPWM_SetOutFreq(uint32_t freq_hz)
{
    if (freq_hz == 0 || freq_hz > 400) return;
    svpwm_out_freq = freq_hz;
    _recalc_params();
}

void UL_SVPWM_SetSwFreq(uint32_t freq_hz)
{
    if (freq_hz < 1000 || freq_hz > 20000) return;
    svpwm_sw_freq = freq_hz;
    _recalc_params();
}

void UL_SVPWM_SetModIndex(uint32_t mod_permille)
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
 *  Register readback (debug)
 * ══════════════════════════════════════════════════════════════════════ */

uint32_t UL_SVPWM_ReadBDTR(void) { return htim1.Instance->BDTR; }
uint32_t UL_SVPWM_ReadCCER(void) { return htim1.Instance->CCER; }
uint32_t UL_SVPWM_ReadCR1(void)  { return htim1.Instance->CR1;  }

/* ══════════════════════════════════════════════════════════════════════
 *  ADC — DC Bus Voltage Monitor (VBUS_MON on ADC3_IN6 / PF8)
 *
 *  Software channel switching: reconfigure ADC3 to channel 6, do a
 *  single polled conversion, then return.  Safe to call from any
 *  RTOS task (not ISR).
 * ══════════════════════════════════════════════════════════════════════ */

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
    return _read_adc3_channel(ADC_CHANNEL_6);
}

float UL_ReadVbusMon(void)
{
    uint32_t adc16 = (uint32_t)UL_ReadVbusMon_Raw() << 4;
    float vbus = VBUS_M_OFFSET + (float)adc16 * VBUS_M_GAIN;
    if (vbus < 0.0f) vbus = 0.0f;
    return vbus;
}

/* ══════════════════════════════════════════════════════════════════════
 *  DC Bus Pre-charge State Machine
 *
 *  On Start():
 *    1. Relay stays OPEN — cap charges through inrush resistors R1-R3
 *    2. Monitor V_bus until it stabilises (dV < CHG_STABLE_DV for
 *       CHG_STABLE_N consecutive ticks)
 *    3. Close relay, enter VERIFY — confirm bus didn't collapse
 *    4. Transition to RUNNING — bus is ready, relay stays closed
 *
 *  Over-voltage and timeout checks run in every active state.
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

/* ── DS1232 Watchdog Heartbeat ───────────────────────────────────────── */

void UL_Heartbeat_Toggle(void)
{
    HAL_GPIO_TogglePin(SOL_CPU_GPIO_Port, SOL_CPU_Pin);
}

