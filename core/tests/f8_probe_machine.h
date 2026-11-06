/*
 * f8_probe_machine.h -- the Channel F as the probe cartridges of
 * tools/ab/f8_probes.py see it, for the F8 tests (f8_ops_test, romc_trace_test,
 * romc_shadow_test): MAME's channelf driver with a "chess" cartridge, minus
 * the BIOS (which a test cannot ship).
 *
 *   $0000-$07FF   the BIOS: never read by a probe (counted as a hazard)
 *   $0800-$FFFF   the image, except
 *   $2800-$2FFF   2K of cart RAM
 *   past the image the open bus reads $FF
 *
 * I/O (channelf.cpp's port_N_r / port_N_w): ports 0, 1, 4 and 5 are latches,
 * written by OUT/OUTS and read back as (~external | latch) with the controllers
 * and the panel idle; port 0 bit 6 gates the controllers' lines onto ports 1
 * and 4. Every other port reads 0 and ignores writes. (The video RAM write on
 * port 0 bit 5 and the sound on port 5 are not modelled: a probe cannot read
 * them.)
 *
 * A row of the trace is the state before an instruction:
 *   pc a w is pc1 dc0 dc1 r0..r15 s1 s2 cycles
 * exactly as tools/ab/f8_trace.lua prints it from MAME (cycles counted from the
 * probe's START).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef F8_PROBE_MACHINE_H
#define F8_PROBE_MACHINE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "f8.h"

#define PM_BASE 0x0800
#define PM_ENTRY 0x0802         /* where the BIOS jumps to */

typedef struct {
    uint8_t rom[0x10000 - PM_BASE];
    size_t rom_size;
    uint8_t ram[0x800];
    uint8_t latch[4];           /* ports 0 1 4 5 */
    unsigned hazards;
    uint16_t hazard_addr;
    int quiet;                  /* reads of the BIOS are not hazards (the reset fetch) */
    int stub;                   /* $0000 holds JMP $0802, the BIOS's whole job for a probe */
} pm_t;

static inline uint8_t pm_read(void *ctx, uint16_t a, uint8_t romc)
{
    pm_t *m = ctx;
    (void)romc;
    if (a < PM_BASE) {
        if (m->stub && a < 3)
            return (uint8_t[]){ 0x29, 0x08, 0x02 }[a];
        if (!m->quiet) {
            m->hazards++;
            m->hazard_addr = a;
        }
        return 0xff;
    }
    if (a >= 0x2800 && a < 0x3000)
        return m->ram[a - 0x2800];
    return (size_t)(a - PM_BASE) < m->rom_size ? m->rom[a - PM_BASE] : 0xff;
}

static inline void pm_write(void *ctx, uint16_t a, uint8_t d)
{
    pm_t *m = ctx;
    if (a >= 0x2800 && a < 0x3000)
        m->ram[a - 0x2800] = d;
}

static inline uint8_t pm_in(void *ctx, uint8_t port)
{
    pm_t *m = ctx;
    const int gate = (m->latch[0] & 0x40) != 0;
    switch (port) {
    case 0: return (uint8_t)(0x0f | m->latch[0]);                   /* panel idle: bits 4-7 read as 1 */
    case 1: return (uint8_t)((gate ? 0x3f : 0xff) | m->latch[1]);   /* right controller idle */
    case 4: return (uint8_t)((gate ? 0x00 : 0xff) | m->latch[2]);   /* left controller idle */
    case 5: return m->latch[3];
    default: return 0;
    }
}

static inline void pm_out(void *ctx, uint8_t port, uint8_t d)
{
    pm_t *m = ctx;
    switch (port) {
    case 0: m->latch[0] = d; break;
    case 1: m->latch[1] = d; break;
    case 4: m->latch[2] = d; break;
    case 5: m->latch[3] = d; break;
    default: break;
    }
}

/* A fresh machine on a CPU with the image copied to $0800. With `stub` the CPU
 * is reset into a BIOS of one instruction, JMP $0802, so the whole run is the
 * bus traffic a cartridge would see from power-on; without it the CPU is parked
 * on the cartridge's entry as the BIOS leaves it (PC at $0802; everything else
 * is the probe's business). */
static inline void pm_setup(pm_t *m, f8_t *c, const uint8_t *img, size_t size, int stub)
{
    memset(m, 0, sizeof *m);
    memset(c, 0, sizeof *c);
    memcpy(m->rom, img, size);
    m->rom_size = size;
    m->stub = stub;
    c->ctx = m;
    c->read = pm_read;
    c->write = pm_write;
    c->in = pm_in;
    c->out = pm_out;
    m->quiet = 1;
    f8_power_on(c);
    m->quiet = 0;
    if (!stub)
        f8_set_pc(c, PM_ENTRY);
}

/* base64 -> bytes (the golden headers keep the probe images that way); returns the length */
static inline size_t pm_b64(const char *s, uint8_t *out)
{
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = 0;
    unsigned acc = 0;
    int bits = 0;
    for (; *s && *s != '='; s++) {
        const char *p = strchr(tab, *s);
        if (!p)
            continue;
        acc = (acc << 6) | (unsigned)(p - tab);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return n;
}

/* ---- trace rows ---- */

static inline void pm_row(char *line, size_t n, const f8_t *c, uint64_t base)
{
    unsigned s1 = 0, s2 = 0;
    char regs[33];
    for (unsigned i = 0; i < 48; i++) {
        s1 += (i + 1) * c->r[16 + i];
        s2 += (i * i + 3) * c->r[16 + i];
    }
    for (unsigned i = 0; i < 16; i++)
        snprintf(regs + 2 * i, 3, "%02X", c->r[i]);
    snprintf(line, n, "%04X %02X %02X %02X %04X %04X %04X %s %08X %08X %llu\n", f8_pc(c) & 0xffffu, c->a,
             c->w & 0x1f, c->is & 0x3f, c->pc1, c->dc0, c->dc1, regs, s1, s2,
             (unsigned long long)(c->cycles - base));
}

#define PM_FNV_OFFSET 0xCBF29CE484222325ULL
#define PM_FNV_PRIME 0x100000001B3ULL

static inline uint64_t pm_fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *s = p;
    while (n--)
        h = (h ^ *s++) * PM_FNV_PRIME;
    return h;
}

#endif /* F8_PROBE_MACHINE_H */
