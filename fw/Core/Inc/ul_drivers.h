/**
 * @file    ul_drivers.h
 * @brief   UltraLogic R1 — 3-phase SVPWM inverter driver
 *
 * TIM1 complementary PWM outputs (PE8-PE13):
 *   TIM1_CH1N PE8   FCU+     TIM1_CH1  PE9   FCU-
 *   TIM1_CH2N PE10  FCV+     TIM1_CH2  PE11  FCV-
 *   TIM1_CH3N PE12  FCW+     TIM1_CH3  PE13  FCW-
 *
 * TIM1 BKIN:
 *   PE15 — hardware overcurrent fault input (active LOW, CUR_TRIP circuit)
 *   Normal: HIGH (external pull-up R215).  Fault: optocoupler U12 pulls LOW.
 *
 * Copyright (c) 2026 PE Info.  All rights reserved.
 */

#ifndef __UL_DRIVERS_H
#define __UL_DRIVERS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f7xx_hal.h"
#include <stdint.h>

/* ── Firmware Info ────────────────────────────────────────────────────── */

#define FW_VERSION_STR     "4.1.0"
#define FW_HW_REVISION     "R1"
#define FW_BUILD_DATE      __DATE__
#define FW_BUILD_TIME      __TIME__
#define FW_PRODUCT_NAME    "UltraLogic"
#define FW_COMPANY         "PE Info"
#define FW_AUTHOR          "Umit Kayacik"

/* ── SVPWM Configuration ─────────────────────────────────────────────
 *
 *  SYSCLK        = 216 MHz  (HSE 25 MHz, PLLN=432, PLLP=2, overdrive)
 *  APB2          = 108 MHz  (SYSCLK/2)
 *  TIM1 input    = 216 MHz  (2×APB2, since APB2 divider != 1)
 *  TIM1 counter  = 216 MHz  (PSC=0, no prescaler)
 *
 *  SVPWM_TIM_CLK is the TIM1 INPUT clock (before prescaler).
 *  Used for dead-time DTG calculation (DTG uses CK_INT, not prescaled).
 *  ARR calculation uses counter clock = SVPWM_TIM_CLK / (PSC+1).
 */

#define SVPWM_LUT_SIZE         360U
#define SVPWM_DEADTIME_NS      2000U
#define SVPWM_TIM_CLK          216000000U

#define SVPWM_DEF_OUT_FREQ_HZ  60U
#define SVPWM_DEF_SW_FREQ_HZ   2000U
#define SVPWM_DEF_MOD_INDEX    850U      /* per-mille (850 = 85.0%) */

/* ── Fault System ────────────────────────────────────────────────────── */

#define FAULT_NONE          0x0000U
#define FAULT_OVERCURRENT   0x0001U  /* F1 — BKIN hw trip on PE15, or SW _cur_trip_stuck() via PD10 */
#define FAULT_OVERVOLTAGE   0x0002U  /* F5 — bus OV, 3 consecutive ISR readings */
#define FAULT_UNDERVOLTAGE  0x0004U  /* bus UV during RUN */
#define FAULT_BUS_COLLAPSE  0x0008U  /* bus UV during READY or RUN (below VBUS_UV_TRIP_V) */
#define FAULT_PRECHARGE     0x0010U  /* precharge timeout/failure, or ADC injected-init error */

uint16_t UL_Fault_Get(void);
uint8_t  UL_Fault_IsTripped(void);
void     UL_Fault_Set(uint16_t mask, uint8_t diag);
void     UL_Fault_Clear(void);

/* ── Diagnostic Code System ──────────────────────────────────────────
 *
 *  Each fault site pushes a specific diagnostic code into a ring buffer.
 *  Codes 1-49 are faults (require reset), 50-99 are warnings.
 *  The display board shows "F-XX" and the PC app shows full descriptions.
 *
 *  History buffer stores the last DIAG_HISTORY_LEN entries with timestamps
 *  for post-mortem analysis via GET:DIAG.
 */

#define DIAG_NONE                0U

#define DIAG_F_OVERCURRENT_HW   1U   /* BKIN hardware trip on PE15           */
#define DIAG_F_OVERCURRENT_SW   2U   /* CUR_TRIP stuck LOW on PD10          */
#define DIAG_F_OVERVOLTAGE      3U   /* Bus > VBUS_OV_TRIP_V (3 readings)   */
#define DIAG_F_UNDERVOLTAGE     4U   /* Bus < VBUS_UV_TRIP_V during RUN     */
#define DIAG_F_BUS_COLLAPSE     5U   /* Bus < VBUS_UV_TRIP_V during READY   */
#define DIAG_F_PRECHG_TIMEOUT   6U   /* Precharge sequence timed out        */
#define DIAG_F_PRECHG_COLLAPSE  7U   /* Bus collapsed after relay close     */
#define DIAG_F_NO_CHARGE        8U   /* No voltage rise during precharge    */
#define DIAG_F_ADC_INIT         9U   /* ADC injected channel init failed    */
#define DIAG_F_PRECHG_OV      10U   /* Overvoltage during precharge        */

#define DIAG_W_REGEN_ACTIVE    50U   /* Brake chopper engaged               */

#define DIAG_IS_FAULT(c)       ((c) >= 1U && (c) <= 49U)

#define DIAG_HISTORY_LEN        8U

typedef struct {
    uint8_t  code;
    uint32_t tick_ms;
} DiagEntry_t;

uint8_t            UL_Diag_GetCode(void);
const DiagEntry_t *UL_Diag_GetHistory(uint8_t *count_out);
void               UL_Diag_Clear(void);

/* ── Drive State Machine ─────────────────────────────────────────────
 *
 *  IDLE → PRECHARGE → READY → RUN → STOPPING → IDLE
 *    |        |          |       |        |
 *    +--------+----------+-------+--------+--> FAULT --> (clear) --> IDLE
 */

typedef enum {
    DRV_STATE_IDLE,
    DRV_STATE_PRECHARGE,
    DRV_STATE_READY,
    DRV_STATE_RUN,
    DRV_STATE_STOPPING,
    DRV_STATE_FAULT
} DrvState_t;

DrvState_t UL_Drive_GetState(void);
void       UL_Drive_Start(void);
void       UL_Drive_Run(void);
void       UL_Drive_Stop(void);
void       UL_Drive_Reset(void);
void       UL_Drive_Tick(void);

/* ── SVPWM API ────────────────────────────────────────────────────────── */

void    UL_SVPWM_Init(void);
void    UL_SVPWM_ISR(void);
void    UL_SVPWM_Enable(void);
void    UL_SVPWM_Disable(void);
uint8_t UL_SVPWM_IsEnabled(void);

void UL_SVPWM_SetOutFreq(uint32_t freq_hz);
void UL_SVPWM_SetSwFreq(uint32_t freq_hz);
void UL_SVPWM_SetModIndex(uint32_t mod_permille);
void UL_SVPWM_SetTestMode(uint8_t enable);

void    UL_ChargeSwitch(uint8_t enable);
uint8_t UL_ChargeSwitch_State(void);

uint32_t UL_SVPWM_ReadBDTR(void);
uint32_t UL_SVPWM_ReadCCER(void);
uint32_t UL_SVPWM_ReadCR1(void);

/* ── ADC — DC Bus Voltage Monitor (inverted flyback sense) ────────────
 *
 *  Topology:
 *    V_bus → flyback XFMR → inverted aux winding (V_8v ≈ -V_bus / K)
 *    V_8v  → R-divider biased by +3.3V → V_mon (ADC input)
 *
 *  Combined (V_bus from ADC):
 *    V_bus = VBUS_M_OFFSET + adc16 × VBUS_M_GAIN   (GAIN is negative)
 *
 *  where adc16 = 12-bit raw << 4  (boosted to 16-bit range)
 */
#define VBUS_XFMR_K         16.95f
#define VBUS_DIV_A           0.03710f
#define VBUS_DIV_B           3.178f
#define VBUS_ADC_VREF        3.3f
#define VBUS_ADC_RANGE16     65536.0f

#define VBUS_M_OFFSET  (VBUS_XFMR_K * VBUS_DIV_B / VBUS_DIV_A)
#define VBUS_M_GAIN    (-(VBUS_XFMR_K * VBUS_ADC_VREF) \
                        / (VBUS_DIV_A * VBUS_ADC_RANGE16))

uint16_t UL_ReadVbusMon_Raw(void);
float    UL_ReadVbusMon(void);

/* ── Current Sensing — HCPL-7510 + shunt resistor ─────────────────────
 *
 *  Signal chain per channel:
 *    Motor current → 2×5 mΩ parallel (2.5 mΩ) → HCPL-7510 → ADC
 *
 *  HCPL-7510 transfer (datasheet Note 2):
 *    VOUT = VREF/2 + V_shunt × (VREF / 512 mV)
 *    where V_shunt = I × R_SHUNT
 *
 *  Solving for I from 12-bit ADC raw:
 *    I = raw × HALL_A_PER_LSB - HALL_OFFSET_A
 */
#define HALL_R_SHUNT       0.0025f     /* 2 × 5 mΩ parallel              */
#define HALL_OPTO_VREF     4.0f        /* HCPL-7510 VREF pin (adjust if differs) */
#define HALL_ADC_VREF      3.3f
#define HALL_ADC_COUNTS    4096.0f

#define HALL_OPTO_GAIN     (HALL_OPTO_VREF / 0.512f)
#define HALL_OPTO_VZERO    (HALL_OPTO_VREF / 2.0f)
#define HALL_SENSITIVITY   (HALL_R_SHUNT * HALL_OPTO_GAIN)   /* V/A at VOUT */

#define HALL_A_PER_LSB     (HALL_ADC_VREF / (HALL_ADC_COUNTS * HALL_SENSITIVITY))
#define HALL_OFFSET_A      (HALL_OPTO_VZERO / HALL_SENSITIVITY)

/* ── ISR Measurements (updated every switching cycle) ──────────────────
 *
 * Written atomically per-member in the SVPWM ISR (single 16/32-bit stores).
 * Readers may see a mix of cycle N and N+1 across members; this is acceptable
 * for telemetry.  Safety-critical decisions (OV/UV) use v_bus alone (atomic).
 */

typedef struct {
    volatile uint16_t shunt1_raw;  /* Phase U current ADC raw (ADC1 IN5) */
    volatile uint16_t shunt3_raw;  /* Phase W current ADC raw (ADC3 IN7) */
    volatile uint16_t vbus_raw;    /* DC bus voltage  ADC raw (ADC3 IN6) */
    volatile float    v_bus;       /* DC bus voltage in volts             */
    volatile float    i_u;         /* Phase U current in amps             */
    volatile float    i_w;         /* Phase W current in amps             */
} UL_Meas_t;

const UL_Meas_t *UL_Meas_Get(void);

void UL_ADC_InjectInit(void);

/* ── Bus Voltage Protection Thresholds ───────────────────────────────── */

#define VBUS_OV_TRIP_V       420.0f   /* 1.27× 325V nominal → OV trip (F5) */
#define VBUS_REGEN_ON_V      395.0f   /* 1.20× nominal → brake chopper ON */
#define VBUS_REGEN_OFF_V     380.0f   /* 1.15× nominal → brake chopper OFF */
#define VBUS_UV_TRIP_V       30.0f    /* undervoltage trip during RUN */

/* ── DC Bus Pre-charge State Machine ─────────────────────────────────
 *
 *  Automatic sequence:
 *    IDLE → PRECHARGE → VERIFY → RUNNING
 *           (relay open,         (relay closed,
 *            cap charges          bus verified)
 *            through R1-R3)
 *
 *  Any state except IDLE can transition to FAULT on error.
 *  FAULT holds relay open until explicitly cleared.
 */

typedef enum {
    CHG_STATE_IDLE,
    CHG_STATE_PRECHARGE,
    CHG_STATE_VERIFY,
    CHG_STATE_RUNNING,
    CHG_STATE_FAULT
} ChgState_t;

typedef enum {
    CHG_FAULT_NONE,
    CHG_FAULT_TIMEOUT,
    CHG_FAULT_OVERVOLTAGE,
    CHG_FAULT_BUS_COLLAPSE,
    CHG_FAULT_NO_CHARGE
} ChgFault_t;

#define CHG_TICK_MS           50U
#define CHG_TIMEOUT_MS        5000U
#define CHG_MIN_V             30.0f
#define CHG_STABLE_DV         5.0f
#define CHG_STABLE_N          6U        /* 6 × 50 ms = 300 ms stable */
#define CHG_VERIFY_MS         200U
#define CHG_COLLAPSE_V        20.0f
#define CHG_OV_V              420.0f

void        UL_Charge_Start(void);
void        UL_Charge_Stop(void);
void        UL_Charge_ClearFault(void);
void        UL_Charge_Tick(void);
ChgState_t  UL_Charge_GetState(void);
ChgFault_t  UL_Charge_GetFault(void);
float       UL_Charge_GetVbus(void);
uint8_t     UL_Charge_BusReady(void);

/* ── DS1232 Watchdog Heartbeat (SOL_CPU on PG7) ──────────────────────
 *
 *  The DS1232 MicroMonitor requires a periodic falling edge on its
 *  ST (strobe) pin.  Call UL_Heartbeat_Toggle() every HEARTBEAT_PERIOD_MS.
 */

#define HEARTBEAT_PERIOD_MS    50U
#define DRV_TELEMETRY_MS       1000U   /* $DRV periodic telemetry interval */

void UL_Heartbeat_Toggle(void);

/* ── Dynamic Braking (Regen) — brake chopper on PD13 / PD14 ──────────
 *
 *  Hysteresis control called from the SVPWM ISR:
 *    v_bus > VBUS_REGEN_ON_V  → BRK_EN + BRK_ON asserted
 *    v_bus < VBUS_REGEN_OFF_V → BRK_EN + BRK_ON released
 */

void UL_Regen_Service(float v_bus);

/* ── BKIN ISR entry point (called from stm32f7xx_it.c) ───────────────── */

void UL_BKIN_IRQHandler(void);

#ifdef __cplusplus
}
#endif

#endif /* __UL_DRIVERS_H */
