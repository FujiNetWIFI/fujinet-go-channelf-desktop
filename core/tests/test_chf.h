/*
 * test_arc.h -- what the session-level tests share: the Channel F's character
 * screen read as text, the tests' own 2650 cartridge programs, and a peek into
 * the running machine, all through the host (core/chf/host.h) so a test needs
 * nothing of the debugger.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef CHF_TEST_ARC_H
#define CHF_TEST_ARC_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "host.h"

/* The UVI's glyph codes as ASCII (the charset is not ASCII: $10-$19 are the
 * digits, $1A-$33 A-Z, $34-$37 . , + $; $01-$0F are graphics). */
static inline char test_glyph(uint8_t code)
{
    code &= 0x3f;
    if (code == 0)
        return ' ';
    if (code >= 0x10 && code <= 0x19)
        return (char)('0' + code - 0x10);
    if (code >= 0x1a && code <= 0x33)
        return (char)('A' + code - 0x1a);
    switch (code) {
    case 0x34: return '.';
    case 0x35: return ',';
    case 0x36: return '+';
    case 0x37: return '$';
    default:   return '#';
    }
}

/* Row r (0-12) of the screen RAM as 16 characters. *lit is set when the row is
 * drawn on the highlight (CONFIG draws the selected row with bit 7 of its
 * letters clear). */
static inline void test_screen_row(int r, char line[17], int *lit)
{
    chf_machine_t *m;

    chf_host_lock();
    m = chf_host_machine();
    if (lit)
        *lit = 0;
    for (int c = 0; c < 16; c++) {
        const uint8_t b = chf_machine_peek(m, CHF_PEEK_UVI, (uint32_t)(r * 16 + c));

        line[c] = test_glyph(b);
        if (lit && line[c] >= 'A' && line[c] <= 'Z' && !(b & 0x80))
            *lit = 1;
    }
    line[16] = '\0';
    chf_host_unlock();
}

/* the row holding `text`, or -1 */
static inline int test_find_row(const char *text)
{
    char line[17];

    for (int r = 0; r < 13; r++) {
        test_screen_row(r, line, NULL);
        if (strstr(line, text))
            return r;
    }
    return -1;
}

static inline int test_bar_row(void)
{
    char line[17];
    int lit;

    for (int r = 1; r < 11; r++) {
        test_screen_row(r, line, &lit);
        if (lit)
            return r;
    }
    return -1;
}

static inline void test_dump_screen(void)
{
    char line[17];

    for (int r = 0; r < 13; r++) {
        test_screen_row(r, line, NULL);
        printf("  %2d|%s|\n", r, line);
    }
}

/* a byte of the UVI's RAM ($18D0 + n) while the machine runs */
static inline uint8_t test_uvi_ram(unsigned off)
{
    uint8_t v;

    chf_host_lock();
    v = chf_machine_peek(chf_host_machine(), CHF_PEEK_UVI, off);
    chf_host_unlock();
    return v;
}
#define TEST_COUNTER 0xd0

/* The tests' own cartridge: an 8K image whose program counts in the UVI's RAM
 * at $18D0 forever. `claim` stamps "FUJI" at the claim offset, making it a
 * FujiNet app that keeps the mailbox; without it, a plain game.
 *      CPSL WC|C ; LODA,R0 $18D0 ; ADDI,R0 1 ; STRA,R0 $18D0 ; BCTA,UN $0000 */
static inline void test_counter_image(uint8_t img[8192], int claim)
{
    static const uint8_t code[] = {
        0x75, 0x09, 0x0c, 0x18, 0xd0, 0x84, 0x01, 0xcc, 0x18, 0xd0, 0x1f, 0x00, 0x00,
    };

    memset(img, 0xff, 8192);
    memcpy(img, code, sizeof code);
    if (claim)
        memcpy(img + 0x1cfc, "FUJI", 4);
}

static inline void test_write_file(const char *path, const uint8_t *data, size_t n)
{
    FILE *f = fopen(path, "wb");

    if (f) {
        fwrite(data, 1, n, f);
        fclose(f);
    }
}

static inline void test_sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

    nanosleep(&ts, NULL);
}

#endif
