/**
 * @file    ul_drivers.h
 * @brief   UltraLogic R1 — 6-channel SVPWM driver (3 complementary pairs)
 *
 * TIM1 complementary PWM outputs:
 *   TIM1_CH1N PE8   FCU+     TIM1_CH1  PE9   FCU-
 *   TIM1_CH2N PE10  FCV+     TIM1_CH2  PE11  FCV-
 *   TIM1_CH3N PE12  FCW+     TIM1_CH3  PE13  FCW-
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

#define FW_VERSION_STR     "3.0.0"
#define FW_HW_REVISION     "R1"
#define FW_BUILD_DATE      __DATE__
#define FW_BUILD_TIME      __TIME__
#define FW_PRODUCT_NAME    "UltraLogic"
#define FW_COMPANY         "PE Info"
#define FW_AUTHOR          "Umit Kayacik"

/* ── SVPWM Configuration ─────────────────────────────────────────────── */

#define SVPWM_LUT_SIZE         360U
#define SVPWM_DEADTIME_NS      1000U
#define SVPWM_TIM_CLK          216000000U

/* Default values (applied at init) */
#define SVPWM_DEF_OUT_FREQ_HZ  60U
#define SVPWM_DEF_SW_FREQ_HZ   5000U
#define SVPWM_DEF_MOD_INDEX    850U      /* per-mille (850 = 85.0%) */

/* ── API ──────────────────────────────────────────────────────────────── */

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
 *  Divider transfer function:
 *    V_mon = VBUS_DIV_A × V_8v + VBUS_DIV_B
 *
 *  Combined (V_bus from ADC):
 *    V_bus = VBUS_M_OFFSET + adc16 × VBUS_M_GAIN   (GAIN is negative)
 *
 *  where adc16 = 12-bit raw << 4  (boosted to 16-bit range)
 *
 *  VBUS_XFMR_K calibrated from measurement:
 *    117.6 VAC → V_bus = 166.3 V (rectified), RAW ≈ 3494
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

/* ── DS1232 Watchdog Heartbeat (SOL_CPU on PG6) ──────────────────────
 *
 *  The DS1232 MicroMonitor requires a periodic falling edge on its
 *  ST (strobe) pin.  If no transition arrives before the timeout
 *  (set by TD pin: 150 ms / 600 ms / 1200 ms), the DS1232 asserts
 *  a hardware reset.
 *
 *  UL_Heartbeat_Toggle() simply toggles PG6, producing alternating
 *  rising/falling edges.  Call it from a periodic RTOS task at an
 *  interval well below the DS1232 timeout (e.g. every 50 ms).
 */

#define HEARTBEAT_PERIOD_MS    50U

void UL_Heartbeat_Toggle(void);

#ifdef __cplusplus
}
#endif

#endif /* __UL_DRIVERS_H */
