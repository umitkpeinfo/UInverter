/**
 * @file    disp_font.h
 * @brief   14-segment font table for ASCII characters
 *
 * Each entry is a uint16_t where each bit maps to a segment:
 *
 *        A
 *      ─────
 *    F│╲H│J╱│B         Bit mapping:
 *     │  ╲│╱ │          0  = A     8  = H
 *      G1─┼─G2          1  = B     9  = J
 *     │  ╱│╲ │          2  = C    10  = K
 *    E│╱N│M╲│C          3  = D    11  = L
 *      ─────  .         4  = E    12  = M
 *        D    DP        5  = F    13  = N
 *                        6  = G1  14  = DP
 *                        7  = G2
 *
 * Copyright (c) 2026 PE Info.  All rights reserved.
 */

#ifndef __DISP_FONT_H
#define __DISP_FONT_H

#include <stdint.h>

#define SEG_A   (1U << 0)
#define SEG_B   (1U << 1)
#define SEG_C   (1U << 2)
#define SEG_D   (1U << 3)
#define SEG_E   (1U << 4)
#define SEG_F   (1U << 5)
#define SEG_G1  (1U << 6)
#define SEG_G2  (1U << 7)
#define SEG_H   (1U << 8)
#define SEG_J   (1U << 9)
#define SEG_K   (1U << 10)
#define SEG_L   (1U << 11)
#define SEG_M   (1U << 12)
#define SEG_N   (1U << 13)
#define SEG_DP_BIT (1U << 14)

/**
 * Look up the 14-segment pattern for an ASCII character.
 * Returns 0 for unsupported characters (blank).
 */
static inline uint16_t font14_lookup(char c)
{
    /* Printable ASCII range: 0x20 (space) through 0x5F (_) covers
     * digits, uppercase letters, and common punctuation. */
    static const uint16_t font_table[64] = {
        /* 0x20 ' ' */ 0,
        /* 0x21 '!' */ SEG_B | SEG_C | SEG_DP_BIT,
        /* 0x22 '"' */ SEG_F | SEG_J,
        /* 0x23 '#' */ SEG_B | SEG_C | SEG_D | SEG_G1 | SEG_G2 | SEG_J | SEG_M,
        /* 0x24 '$' */ SEG_A | SEG_C | SEG_D | SEG_F | SEG_G1 | SEG_G2 | SEG_J | SEG_M,
        /* 0x25 '%' */ SEG_A | SEG_D | SEG_F | SEG_C | SEG_K | SEG_N,
        /* 0x26 '&' */ SEG_A | SEG_D | SEG_E | SEG_F | SEG_G1 | SEG_H | SEG_K | SEG_N,
        /* 0x27 ''' */ SEG_J,
        /* 0x28 '(' */ SEG_K | SEG_N,
        /* 0x29 ')' */ SEG_H | SEG_L,
        /* 0x2A '*' */ SEG_G1 | SEG_G2 | SEG_H | SEG_J | SEG_K | SEG_L | SEG_M | SEG_N,
        /* 0x2B '+' */ SEG_G1 | SEG_G2 | SEG_J | SEG_M,
        /* 0x2C ',' */ SEG_N,
        /* 0x2D '-' */ SEG_G1 | SEG_G2,
        /* 0x2E '.' */ SEG_DP_BIT,
        /* 0x2F '/' */ SEG_K | SEG_N,
        /* 0x30 '0' */ SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F | SEG_K | SEG_N,
        /* 0x31 '1' */ SEG_B | SEG_C,
        /* 0x32 '2' */ SEG_A | SEG_B | SEG_D | SEG_E | SEG_G1 | SEG_G2,
        /* 0x33 '3' */ SEG_A | SEG_B | SEG_C | SEG_D | SEG_G2,
        /* 0x34 '4' */ SEG_B | SEG_C | SEG_F | SEG_G1 | SEG_G2,
        /* 0x35 '5' */ SEG_A | SEG_C | SEG_D | SEG_F | SEG_G1 | SEG_G2,
        /* 0x36 '6' */ SEG_A | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G1 | SEG_G2,
        /* 0x37 '7' */ SEG_A | SEG_B | SEG_C,
        /* 0x38 '8' */ SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G1 | SEG_G2,
        /* 0x39 '9' */ SEG_A | SEG_B | SEG_C | SEG_D | SEG_F | SEG_G1 | SEG_G2,
        /* 0x3A ':' */ SEG_J | SEG_M,
        /* 0x3B ';' */ SEG_J | SEG_N,
        /* 0x3C '<' */ SEG_K | SEG_N,
        /* 0x3D '=' */ SEG_D | SEG_G1 | SEG_G2,
        /* 0x3E '>' */ SEG_H | SEG_L,
        /* 0x3F '?' */ SEG_A | SEG_B | SEG_G2 | SEG_M,
        /* 0x40 '@' */ SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_G2 | SEG_J,
        /* 0x41 'A' */ SEG_A | SEG_B | SEG_C | SEG_E | SEG_F | SEG_G1 | SEG_G2,
        /* 0x42 'B' */ SEG_A | SEG_B | SEG_C | SEG_D | SEG_G2 | SEG_J | SEG_M,
        /* 0x43 'C' */ SEG_A | SEG_D | SEG_E | SEG_F,
        /* 0x44 'D' */ SEG_A | SEG_B | SEG_C | SEG_D | SEG_J | SEG_M,
        /* 0x45 'E' */ SEG_A | SEG_D | SEG_E | SEG_F | SEG_G1,
        /* 0x46 'F' */ SEG_A | SEG_E | SEG_F | SEG_G1,
        /* 0x47 'G' */ SEG_A | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G2,
        /* 0x48 'H' */ SEG_B | SEG_C | SEG_E | SEG_F | SEG_G1 | SEG_G2,
        /* 0x49 'I' */ SEG_A | SEG_D | SEG_J | SEG_M,
        /* 0x4A 'J' */ SEG_B | SEG_C | SEG_D | SEG_E,
        /* 0x4B 'K' */ SEG_E | SEG_F | SEG_G1 | SEG_K | SEG_N,
        /* 0x4C 'L' */ SEG_D | SEG_E | SEG_F,
        /* 0x4D 'M' */ SEG_B | SEG_C | SEG_E | SEG_F | SEG_H | SEG_K,
        /* 0x4E 'N' */ SEG_B | SEG_C | SEG_E | SEG_F | SEG_H | SEG_N,
        /* 0x4F 'O' */ SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F,
        /* 0x50 'P' */ SEG_A | SEG_B | SEG_E | SEG_F | SEG_G1 | SEG_G2,
        /* 0x51 'Q' */ SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F | SEG_N,
        /* 0x52 'R' */ SEG_A | SEG_B | SEG_E | SEG_F | SEG_G1 | SEG_G2 | SEG_N,
        /* 0x53 'S' */ SEG_A | SEG_C | SEG_D | SEG_F | SEG_G1 | SEG_G2,
        /* 0x54 'T' */ SEG_A | SEG_J | SEG_M,
        /* 0x55 'U' */ SEG_B | SEG_C | SEG_D | SEG_E | SEG_F,
        /* 0x56 'V' */ SEG_E | SEG_F | SEG_K | SEG_L,
        /* 0x57 'W' */ SEG_B | SEG_C | SEG_E | SEG_F | SEG_L | SEG_N,
        /* 0x58 'X' */ SEG_H | SEG_K | SEG_L | SEG_N,
        /* 0x59 'Y' */ SEG_H | SEG_K | SEG_M,
        /* 0x5A 'Z' */ SEG_A | SEG_D | SEG_K | SEG_L,
        /* 0x5B '[' */ SEG_A | SEG_D | SEG_E | SEG_F,
        /* 0x5C '\' */ SEG_H | SEG_N,
        /* 0x5D ']' */ SEG_A | SEG_B | SEG_C | SEG_D,
        /* 0x5E '^' */ SEG_L | SEG_N,
        /* 0x5F '_' */ SEG_D,
    };

    if (c >= 'a' && c <= 'z')
        c = (char)(c - ('a' - 'A'));

    if (c < 0x20 || c > 0x5F)
        return 0;

    return font_table[(uint8_t)c - 0x20];
}

#endif /* __DISP_FONT_H */
