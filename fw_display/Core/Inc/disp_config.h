/**
 * @file    disp_config.h
 * @brief   Ultra Display R1 — hardware pin mapping and constants
 *
 * MCU:  STM32F100R8T7B (LQFP64), 8 MHz HSE crystal, 24 MHz SYSCLK
 *
 * 14-segment display: 6 digits, multiplexed at 1 kHz per digit
 *   Segments A-M : PC0-PC12  (active HIGH, NPN transistor drivers)
 *   Segment  N   : PB8
 *   Segment  DP  : PB9
 *   Digits 0-5   : PA0-PA5   (active HIGH, Darlington pairs)
 *
 * 3 buttons (active LOW, external 10K pull-ups):
 *   SW1 Scroll  : PB0
 *   SW2 Forward : PB1
 *   SW3 Reverse : PB2
 *
 * USART1 (PA9=TX, PA10=RX) to main logic board, 115200 8N1
 *
 * Pin mapping derived from 17B209.5 schematic.
 *
 * Copyright (c) 2026 PE Info.  All rights reserved.
 */

#ifndef __DISP_CONFIG_H
#define __DISP_CONFIG_H

#include "stm32f1xx_hal.h"

/* ── 14-Segment Pin Mapping ──────────────────────────────────────────
 *
 * Segments A-N + DP mapped to GPIO pins.
 * The segment encoding order defines how bits in a uint16_t map to
 * physical segments.  Bit 0 = SEG_A, Bit 14 = SEG_DP.
 *
 * These MUST be updated to match your actual PCB routing.
 * The port and pin for each segment are specified below.
 */

/* Segment GPIO — PC0..PC12 + PB8..PB9 */
#define SEG_A_PORT    GPIOC
#define SEG_A_PIN     GPIO_PIN_0
#define SEG_B_PORT    GPIOC
#define SEG_B_PIN     GPIO_PIN_1
#define SEG_C_PORT    GPIOC
#define SEG_C_PIN     GPIO_PIN_2
#define SEG_D_PORT    GPIOC
#define SEG_D_PIN     GPIO_PIN_3
#define SEG_E_PORT    GPIOC
#define SEG_E_PIN     GPIO_PIN_4
#define SEG_F_PORT    GPIOC
#define SEG_F_PIN     GPIO_PIN_5
#define SEG_G1_PORT   GPIOC
#define SEG_G1_PIN    GPIO_PIN_6
#define SEG_G2_PORT   GPIOC
#define SEG_G2_PIN    GPIO_PIN_7
#define SEG_H_PORT    GPIOC
#define SEG_H_PIN     GPIO_PIN_8
#define SEG_J_PORT    GPIOC
#define SEG_J_PIN     GPIO_PIN_9
#define SEG_K_PORT    GPIOC
#define SEG_K_PIN     GPIO_PIN_10
#define SEG_L_PORT    GPIOC
#define SEG_L_PIN     GPIO_PIN_11
#define SEG_M_PORT    GPIOC
#define SEG_M_PIN     GPIO_PIN_12
#define SEG_N_PORT    GPIOB
#define SEG_N_PIN     GPIO_PIN_8
#define SEG_DP_PORT   GPIOB
#define SEG_DP_PIN    GPIO_PIN_9

#define SEG_COUNT     15U

/* Digit select GPIO — PA0..PA5 based on schematic */
#define DIG0_PORT     GPIOA
#define DIG0_PIN      GPIO_PIN_0
#define DIG1_PORT     GPIOA
#define DIG1_PIN      GPIO_PIN_1
#define DIG2_PORT     GPIOA
#define DIG2_PIN      GPIO_PIN_2
#define DIG3_PORT     GPIOA
#define DIG3_PIN      GPIO_PIN_3
#define DIG4_PORT     GPIOA
#define DIG4_PIN      GPIO_PIN_4
#define DIG5_PORT     GPIOA
#define DIG5_PIN      GPIO_PIN_5

#define DIG_COUNT     6U

/* Button GPIO (active LOW with external 10K pull-ups) */
#define BTN_SCR_PORT  GPIOB
#define BTN_SCR_PIN   GPIO_PIN_0
#define BTN_INC_PORT  GPIOB
#define BTN_INC_PIN   GPIO_PIN_1
#define BTN_DEC_PORT  GPIOB
#define BTN_DEC_PIN   GPIO_PIN_2

/* Button bitmask values (must match M72 protocol) */
#define BTN_MASK_SCR  0x01U
#define BTN_MASK_INC  0x02U
#define BTN_MASK_DEC  0x04U

/* ── Display Timing ──────────────────────────────────────────────── */

/** Multiplex refresh rate per digit (Hz).  6 digits × 1 kHz = 6 kHz ISR */
#define DISP_MUX_FREQ_HZ    1000U

/** Button scan interval (in multiplex ticks) */
#define BTN_SCAN_INTERVAL    50U

/** Button TX interval (in multiplex ticks) — send packet every ~50 ms */
#define BTN_TX_INTERVAL      50U

/* ── Serial Protocol (must match inverter-side ul_display.h) ─────── */

#define PROTO_LEAD_CHAR      '$'
#define PROTO_TYPE_SEGMENT    0x01U
#define PROTO_TYPE_BUTTON     0x02U
#define PROTO_BAUD            115200U
#define PROTO_MAX_PACKET      32U
#define PROTO_HEADER_LEN      7U

#endif /* __DISP_CONFIG_H */
