/*
 * test_chf.h -- what the tests share: the tests' own BIOS and cartridge programs
 * (F8 machine code written out by hand, so nothing needs assembling and nothing
 * is anybody's ROM), a way to build a machine on them, and a peek into the
 * running host's machine, all through core/chf's own headers so a test needs
 * nothing of the debugger or the session.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef CHF_TEST_CHF_H
#define CHF_TEST_CHF_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "host.h"
#include "fuji_mailbox.h"       /* the cartridge firmware's own layout, for the claim and the status page */

/* The tests' BIOS: a 2K image whose reset vector jumps to the cartridge's entry,
 * $0802 (`JMP $0802`), the rest unused ($00). Not the console's BIOS, which
 * would also clear the scratchpad and set up the K stack. */
static inline void test_bios(uint8_t lo[CF_BIOS_HALF], uint8_t hi[CF_BIOS_HALF])
{
    memset(lo, 0, CF_BIOS_HALF);
    memset(hi, 0, CF_BIOS_HALF);
    lo[0] = 0x29;               /* JMP $0802 */
    lo[1] = 0x08;
    lo[2] = 0x02;
}

/* A cartridge whose program is `code` at $0802 (the $55 signature and the
 * skipped byte before it), padded with $FF to `size`. With `claim` it is a
 * FujiNet app: "FUJI" at the claim offset (size must then be 16K). */
static inline void test_cart(uint8_t *img, uint32_t size, const uint8_t *code, size_t n, int claim)
{
    memset(img, 0xff, size);
    img[0] = 0x55;
    img[1] = 0x00;
    memcpy(img + 2, code, n);
    if (claim)
        memcpy(img + FN_ROM_CLAIM, FN_R_CLAIM_SIG, FN_R_CLAIM_LEN);
}

/* BR to itself, the idle loop: `90 FF` */
#define T_SPIN 0x90, 0xff

/* A FujiNet app that counts, byte at the arena's first cell ($8000):
 *     loop: DCI $8000 ; LM ; AI 1 ; DCI $8000 ; ST ; BR loop */
static inline void test_counter_image(uint8_t img[16384], int claim)
{
    static const uint8_t code[] = {
        0x2a, 0x80, 0x00, 0x16, 0x24, 0x01, 0x2a, 0x80, 0x00, 0x17, 0x90, 0xf5,
    };

    test_cart(img, 16384, code, sizeof code, claim);
}
#define TEST_COUNTER_ADDR 0x8000

/* The console's bytes from the running host, through the debugger's own peek
 * (no side effects), under the run lock. */
static inline uint8_t test_peek(uint16_t addr)
{
    uint8_t v;

    chf_host_lock();
    v = chf_machine_peek(chf_host_machine(), CHF_PEEK_PROG, addr);
    chf_host_unlock();
    return v;
}

/* the 16 bytes of the arena's mailbox a string search reads: does `text` occur in
 * the reply window ($F800-$FBFF)? */
static inline int test_reply_has(const char *text)
{
    const size_t n = strlen(text);

    for (unsigned a = 0xf800; a + n <= 0xfc00; a++) {
        size_t k = 0;

        while (k < n && test_peek((uint16_t)(a + k)) == (uint8_t)text[k])
            k++;
        if (k == n)
            return 1;
    }
    return 0;
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

/* a machine on the tests' BIOS and a cartridge in the slot, as chf_headless builds one */
typedef struct {
    chf_machine_t m;
    chf_cart_t cart;
    uint8_t lo[CF_BIOS_HALF], hi[CF_BIOS_HALF];
} test_rig;

static inline int test_rig_init(test_rig *r, int machine, int cart_type, const uint8_t *img, uint32_t size)
{
    chf_machine_opts_t mo;
    const char *err = NULL;

    memset(r, 0, sizeof *r);
    test_bios(r->lo, r->hi);
    if (chf_cart_init(&r->cart, cart_type, img, size, &err) != 0)
        return -1;
    memset(&mo, 0, sizeof mo);
    mo.machine = machine;
    mo.bios_lo = r->lo;
    mo.bios_hi = r->hi;
    mo.bios_id = CHF_ROM_SL31253;
    mo.cart = &r->cart;
    chf_machine_init(&r->m, &mo);
    return 0;
}

#endif
