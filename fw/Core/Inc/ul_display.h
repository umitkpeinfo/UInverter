/**
 * @file    ul_display.h
 * @brief   Display MCU communication — M72-compatible serial protocol
 *
 * Hardware:
 *   USART1 PB6(TX) / PB7(RX), 115200 8N1
 *   Connects to STM32F100R8 display MCU via PCB edge connector
 *
 * Protocol:
 *   All packets are ASCII with hex-encoded fields:
 *     $LLTTCC[payload]
 *       $  = lead character (0x24)
 *       LL = total packet length in chars (2 hex digits)
 *       TT = message type (2 hex digits): 01=segment, 02=button
 *       CC = checksum (2 hex digits): sum of all bytes minus CC bytes
 *       payload = type-specific data
 *
 * TX (inverter → display):  segment packets  $LLTTCCEE[text]
 *   EE = effect byte (00 = normal)
 *   text = ASCII characters mapped to 14-segment font by display MCU
 *
 * RX (display → inverter):  button packets  $LLTTCCBB
 *   BB = button bitmask (2 hex): bit0=SCR, bit1=INC, bit2=DEC
 *
 * Copyright (c) 2026 PE Info.  All rights reserved.
 */

#ifndef __UL_DISPLAY_H
#define __UL_DISPLAY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f7xx_hal.h"
#include <stdint.h>

/* ── Protocol Constants ──────────────────────────────────────────────── */

#define DISP_LEAD_CHAR        '$'
#define DISP_HEADER_LEN       7U    /* $LLTTCC */
#define DISP_MAX_PACKET_LEN   32U
#define DISP_BAUD_RATE        115200U

#define DISP_TYPE_SEGMENT     0x01U
#define DISP_TYPE_BUTTON      0x02U

#define DISP_BTN_PACKET_LEN   9U    /* $LLTTCCBB */

/* ── Button Bitmask ──────────────────────────────────────────────────── */

#define DISP_BTN_SCR          0x01U  /* Scroll button */
#define DISP_BTN_INC          0x02U  /* Forward / Increment button */
#define DISP_BTN_DEC          0x04U  /* Reverse / Decrement button */

/* ── Display Configuration ───────────────────────────────────────────── */

#define DISP_NUM_DIGITS       6U
#define DISP_EFFECT_NORMAL    0x00U

/** How often the display task sends a frame (ms) */
#define DISP_TX_PERIOD_MS     100U

/** RX ring buffer size (must be power of 2) */
#define DISP_RX_BUF_SIZE      64U
#define DISP_RX_BUF_MASK      (DISP_RX_BUF_SIZE - 1U)

/* ── Button State ────────────────────────────────────────────────────── */

typedef struct {
    volatile uint8_t  raw;             /* latest decoded bitmask */
    volatile uint32_t scr_count;       /* consecutive ticks SCR held */
    volatile uint32_t inc_count;       /* consecutive ticks INC held */
    volatile uint32_t dec_count;       /* consecutive ticks DEC held */
    volatile uint32_t last_rx_tick;    /* HAL_GetTick() of last valid packet */
} UL_DispButtons_t;

/* ── API ─────────────────────────────────────────────────────────────── */

void UL_Display_Init(UART_HandleTypeDef *huart);

/** Call from USART1 IRQ handler — stores byte in ring buffer */
void UL_Display_UART_IRQHandler(void);

/**
 * Call periodically (~5 ms) from an RTOS task.
 * Parses any received button packets and updates button state.
 */
void UL_Display_Poll(void);

/** Send a display string (up to DISP_NUM_DIGITS chars) */
void UL_Display_SendText(const char *text);

/** Send a segment packet with explicit effect byte */
void UL_Display_SendSegment(const char *text, uint8_t effect);

/** Get current button state */
const UL_DispButtons_t *UL_Display_GetButtons(void);

/**
 * Auto-repeat helper (matches M72 acceleration curve).
 * Returns 1 on the ticks where a "click" should fire:
 *   - tick 1:   immediate
 *   - tick 200+: every 64 ticks (~320 ms at 5 ms polling)
 *   - tick 800+: every 16 ticks (~80 ms)
 *   - tick 1400+: every 2 ticks (~10 ms)
 */
uint8_t UL_Display_AutoRepeat(uint32_t count);

#ifdef __cplusplus
}
#endif

#endif /* __UL_DISPLAY_H */
