/**
 * @file    ul_display.c
 * @brief   Display MCU communication — M72-compatible serial protocol
 *
 * Implements the display serial link on USART1 (PB6/PB7, 115200 8N1).
 * TX: sends segment packets with display text
 * RX: receives button state packets from the display MCU
 *
 * The RX path uses a lightweight ring buffer fed by the USART1 RXNE
 * interrupt.  Packet parsing runs in task context via UL_Display_Poll().
 *
 * Copyright (c) 2026 PE Info.  All rights reserved.
 */

#include "ul_display.h"
#include <string.h>

#if (DISP_RX_BUF_SIZE & (DISP_RX_BUF_SIZE - 1U)) != 0U
  #error "DISP_RX_BUF_SIZE must be a power of 2"
#endif

/* ── UART handle (set by UL_Display_Init) ─────────────────────────── */

static UART_HandleTypeDef *_huart;

/* ── RX Ring Buffer ──────────────────────────────────────────────────
 *
 * Written by ISR (single producer), read by UL_Display_Poll (single
 * consumer).  No lock needed — head/tail are updated atomically.
 */

static volatile uint8_t  _rx_buf[DISP_RX_BUF_SIZE];
static volatile uint16_t _rx_head;  /* ISR writes here */
static volatile uint16_t _rx_tail;  /* Poll reads here  */

/* ── Packet Parser State ─────────────────────────────────────────── */

static uint8_t  _pkt_buf[DISP_MAX_PACKET_LEN];
static uint8_t  _pkt_pos;
static uint8_t  _pkt_expected_len;

/* ── Button State ────────────────────────────────────────────────── */

static UL_DispButtons_t _buttons;

/* timeout: if no valid button packet for this many ms, clear buttons */
#define BTN_TIMEOUT_MS  200U

/* ── Hex Helpers ─────────────────────────────────────────────────── */

static uint8_t _hex_val(char c)
{
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    return 0xFF;
}

static uint8_t _hex2_decode(const uint8_t *p)
{
    uint8_t hi = _hex_val((char)p[0]);
    uint8_t lo = _hex_val((char)p[1]);
    if (hi > 0x0F || lo > 0x0F) return 0xFF;
    return (uint8_t)((hi << 4) | lo);
}

static void _hex2_encode(uint8_t *dst, uint8_t val)
{
    static const char hex[] = "0123456789ABCDEF";
    dst[0] = (uint8_t)hex[(val >> 4) & 0x0F];
    dst[1] = (uint8_t)hex[val & 0x0F];
}

static uint8_t _is_hex(uint8_t c)
{
    return (c >= '0' && c <= '9') ||
           (c >= 'A' && c <= 'F') ||
           (c >= 'a' && c <= 'f');
}

/* ── Checksum ────────────────────────────────────────────────────────
 *
 * Sum all bytes in the packet EXCEPT the 2 checksum bytes (positions 5,6).
 * Result is the low 8 bits, encoded as 2 hex digits at positions 5,6.
 */

static uint8_t _calc_checksum(const uint8_t *pkt, uint8_t len)
{
    uint16_t sum = 0;
    for (uint8_t i = 0; i < len; i++) {
        if (i == 5 || i == 6) continue;
        sum += pkt[i];
    }
    return (uint8_t)(sum & 0xFF);
}

static uint8_t _verify_checksum(const uint8_t *pkt, uint8_t len)
{
    uint8_t expected = _calc_checksum(pkt, len);
    uint8_t received = _hex2_decode(&pkt[5]);
    return (received == expected) ? 1U : 0U;
}

/* ── Packet Dispatch ─────────────────────────────────────────────── */

static void _dispatch_packet(const uint8_t *pkt, uint8_t len)
{
    if (!_verify_checksum(pkt, len))
        return;

    uint8_t msg_type = _hex2_decode(&pkt[3]);

    if (msg_type == DISP_TYPE_BUTTON && len == DISP_BTN_PACKET_LEN) {
        uint8_t config = _hex2_decode(&pkt[7]);
        if (config != 0xFF) {
            _buttons.raw = config;
            _buttons.last_rx_tick = HAL_GetTick();
        }
    }
}

/* ── Packet Parser (byte-at-a-time state machine, matches M72) ─── */

static void _parse_byte(uint8_t c)
{
    if (c == DISP_LEAD_CHAR) {
        _pkt_buf[0] = c;
        _pkt_pos = 1;
        _pkt_expected_len = 0;
        return;
    }

    if (_pkt_pos == 0)
        return;

    if (_pkt_pos < DISP_HEADER_LEN) {
        if (!_is_hex(c)) {
            _pkt_pos = 0;
            return;
        }
        _pkt_buf[_pkt_pos++] = c;

        /* After collecting LL (bytes 1,2), decode expected length */
        if (_pkt_pos == 3) {
            _pkt_expected_len = _hex2_decode(&_pkt_buf[1]);
            if (_pkt_expected_len < DISP_HEADER_LEN ||
                _pkt_expected_len > DISP_MAX_PACKET_LEN) {
                _pkt_pos = 0;
                return;
            }
        }
        return;
    }

    if (c == 0) {
        _pkt_pos = 0;
        return;
    }

    _pkt_buf[_pkt_pos++] = c;

    if (_pkt_pos >= _pkt_expected_len) {
        _dispatch_packet(_pkt_buf, _pkt_expected_len);
        _pkt_pos = 0;
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  Public API
 * ══════════════════════════════════════════════════════════════════════ */

void UL_Display_Init(UART_HandleTypeDef *huart)
{
    _huart = huart;
    _rx_head = 0;
    _rx_tail = 0;
    _pkt_pos = 0;
    _pkt_expected_len = 0;

    memset((void *)&_buttons, 0, sizeof(_buttons));

    /* Enable RXNE interrupt (register-level for minimal latency) */
    USART_TypeDef *uart = _huart->Instance;
    uart->CR1 |= USART_CR1_RXNEIE;
}

void UL_Display_UART_IRQHandler(void)
{
    if (_huart == NULL) return;
    USART_TypeDef *uart = _huart->Instance;
    uint32_t isr = uart->ISR;

    if (isr & USART_ISR_RXNE) {
        uint8_t byte = (uint8_t)(uart->RDR & 0xFF);
        uint16_t next = (_rx_head + 1U) & DISP_RX_BUF_MASK;
        if (next != _rx_tail) {
            _rx_buf[_rx_head] = byte;
            _rx_head = next;
        }
    }

    /* Clear error flags to prevent accumulation (only ORE fires on RXNEIE;
       FE/NE won't trigger ISR entry without EIE, but clear defensively) */
    if (isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE)) {
        uart->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NCF;
    }
}

void UL_Display_Poll(void)
{
    /* Drain ring buffer through the packet parser */
    while (_rx_tail != _rx_head) {
        uint8_t byte = _rx_buf[_rx_tail];
        _rx_tail = (_rx_tail + 1U) & DISP_RX_BUF_MASK;
        _parse_byte(byte);
    }

    /* Update button hold counters */
    uint32_t now = HAL_GetTick();
    if ((now - _buttons.last_rx_tick) > BTN_TIMEOUT_MS) {
        _buttons.raw = 0;
        _buttons.scr_count = 0;
        _buttons.inc_count = 0;
        _buttons.dec_count = 0;
    } else {
        if (_buttons.raw & DISP_BTN_SCR) _buttons.scr_count++;
        else _buttons.scr_count = 0;

        if (_buttons.raw & DISP_BTN_INC) _buttons.inc_count++;
        else _buttons.inc_count = 0;

        if (_buttons.raw & DISP_BTN_DEC) _buttons.dec_count++;
        else _buttons.dec_count = 0;
    }
}

void UL_Display_SendSegment(const char *text, uint8_t effect)
{
    if (!_huart) return;

    uint8_t payload_len = (uint8_t)strlen(text);
    if (payload_len > DISP_NUM_DIGITS)
        payload_len = DISP_NUM_DIGITS;

    /* Total packet: $LLTTCCEE + text */
    uint8_t total_len = DISP_HEADER_LEN + 2U + payload_len;
    if (total_len > DISP_MAX_PACKET_LEN)
        return;

    uint8_t pkt[DISP_MAX_PACKET_LEN];
    pkt[0] = (uint8_t)DISP_LEAD_CHAR;
    _hex2_encode(&pkt[1], total_len);
    _hex2_encode(&pkt[3], DISP_TYPE_SEGMENT);
    /* Checksum placeholder (will be filled below) */
    pkt[5] = '0';
    pkt[6] = '0';
    /* Effect byte */
    _hex2_encode(&pkt[7], effect);
    /* Display text */
    memcpy(&pkt[9], text, payload_len);

    /* Calculate and insert checksum */
    uint8_t cksum = _calc_checksum(pkt, total_len);
    _hex2_encode(&pkt[5], cksum);

    /* Transmit (blocking, short packet) */
    HAL_UART_Transmit(_huart, pkt, total_len, 10);
}

void UL_Display_SendText(const char *text)
{
    UL_Display_SendSegment(text, DISP_EFFECT_NORMAL);
}

const UL_DispButtons_t *UL_Display_GetButtons(void)
{
    return &_buttons;
}

uint8_t UL_Display_AutoRepeat(uint32_t count)
{
    if (count == 1) return 1;
    if (count >= 1400 && (count & 0x01) == 0) return 1;
    if (count >= 800  && (count & 0x0F) == 0) return 1;
    if (count >= 200  && (count & 0x3F) == 0) return 1;
    return 0;
}
