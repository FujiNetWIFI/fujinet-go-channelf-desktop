/*
 * romc_trace_test -- the core's ROMC stream against MAME's, on the probe
 * cartridges of tools/ab/f8_probes.py.
 *
 * MAME's F8 core, grafted to write one 10-byte record per ROMC state
 * (F8_ROMC_TRACE: romc, dbus, PC0, PC1, DC0, DC1, little-endian, the registers
 * as the state left them), is what the firmware's cartridge decoder was written
 * against. romc_hook gives this core the same stream; this test holds the two
 * to each other:
 *
 *   romc_trace_test                       the ctest: each probe's records from the fetch of
 *                                         its START opcode (ROMC 00 with PC0 = START + 1) to
 *                                         the fetch of its DONE opcode, counted and hashed
 *                                         (FNV-1a over the bytes) against romc_golden.h
 *   romc_trace_test --run CART START DONE the same records, raw, on stdout
 *                                         (tools/ab/romc_ab.py diffs them with MAME's)
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "f8.h"
#include "f8_probe_machine.h"

typedef struct {
    unsigned start, done;
    int on, finished;
    unsigned long n;
    uint64_t hash;
    unsigned long hist[32];
    FILE *out;
} tap_t;

static void tap(void *ctx, const f8_t *c, uint8_t romc, uint8_t dbus)
{
    tap_t *t = ctx;
    uint8_t rec[10];

    if (t->finished)
        return;
    if (!t->on) {
        if (romc != 0x00 || c->pc0 != ((t->start + 1) & 0xffff))
            return;
        t->on = 1;
    }
    rec[0] = romc;
    rec[1] = dbus;
    rec[2] = (uint8_t)c->pc0;
    rec[3] = (uint8_t)(c->pc0 >> 8);
    rec[4] = (uint8_t)c->pc1;
    rec[5] = (uint8_t)(c->pc1 >> 8);
    rec[6] = (uint8_t)c->dc0;
    rec[7] = (uint8_t)(c->dc0 >> 8);
    rec[8] = (uint8_t)c->dc1;
    rec[9] = (uint8_t)(c->dc1 >> 8);
    t->hash = pm_fnv(t->hash, rec, sizeof rec);
    t->n++;
    t->hist[romc & 31]++;
    if (t->out)
        fwrite(rec, 1, sizeof rec, t->out);
    if (romc == 0x00 && c->pc0 == ((t->done + 1) & 0xffff))
        t->finished = 1;
}

/* the CPU's own context is the machine; the tap rides beside it, so wrap read/write */
typedef struct {
    pm_t m;
    tap_t t;
} rig_t;

static uint8_t rig_read(void *ctx, uint16_t a, uint8_t romc) { return pm_read(&((rig_t *)ctx)->m, a, romc); }
static void rig_write(void *ctx, uint16_t a, uint8_t d) { pm_write(&((rig_t *)ctx)->m, a, d); }
static uint8_t rig_in(void *ctx, uint8_t p) { return pm_in(&((rig_t *)ctx)->m, p); }
static void rig_out(void *ctx, uint8_t p, uint8_t d) { pm_out(&((rig_t *)ctx)->m, p, d); }
static void rig_tap(void *ctx, const f8_t *c, uint8_t romc, uint8_t dbus) { tap(&((rig_t *)ctx)->t, c, romc, dbus); }

static int run(const uint8_t *img, size_t size, unsigned start, unsigned done, FILE *out, tap_t *res,
               unsigned *hazards)
{
    static rig_t r;
    static f8_t cpu;

    memset(&r, 0, sizeof r);
    memset(&cpu, 0, sizeof cpu);
    memcpy(r.m.rom, img, size);
    r.m.rom_size = size;
    r.m.stub = 1;
    r.t.start = start;
    r.t.done = done;
    r.t.hash = PM_FNV_OFFSET;
    r.t.out = out;
    cpu.ctx = &r;
    cpu.read = rig_read;
    cpu.write = rig_write;
    cpu.in = rig_in;
    cpu.out = rig_out;
    cpu.romc_hook = rig_tap;
    r.m.quiet = 1;
    f8_power_on(&cpu);          /* the reset's own ROMC 08 and 00 reach the tap */
    r.m.quiet = 0;
    for (unsigned long guard = 0; !r.t.finished && guard < 50000000UL; guard++)
        f8_step(&cpu);
    *res = r.t;
    *hazards = r.m.hazards;
    return r.t.finished;
}

#ifdef ROMC_GOLDEN
#include "f8_golden.h"
#include "romc_golden.h"
#endif

int main(int argc, char **argv)
{
    tap_t res;
    unsigned hazards;

    if (argc >= 5 && strcmp(argv[1], "--run") == 0) {
        FILE *f = fopen(argv[2], "rb");
        static uint8_t img[0x10000];
        size_t size;
        int ok;
        if (!f) {
            perror(argv[2]);
            return 2;
        }
        size = fread(img, 1, sizeof img, f);
        fclose(f);
        ok = run(img, size, (unsigned)strtoul(argv[3], NULL, 16), (unsigned)strtoul(argv[4], NULL, 16), stdout, &res,
                 &hazards);
        fprintf(stderr, "records %lu finished %d hazards %u\n", res.n, ok, hazards);
        return ok ? 0 : 1;
    }
#ifdef ROMC_GOLDEN
    {
        int failures = 0;
        unsigned long total = 0, hist[32] = { 0 };
        for (unsigned i = 0; i < sizeof romc_probes / sizeof romc_probes[0]; i++) {
            static uint8_t img[0x10000];
            const char *name = romc_probes[i].name;
            unsigned k;
            for (k = 0; k < sizeof f8_probes / sizeof f8_probes[0]; k++)
                if (strcmp(f8_probes[k].name, name) == 0)
                    break;
            if (k == sizeof f8_probes / sizeof f8_probes[0]) {
                printf("%s: no such probe in f8_golden.h\n", name);
                failures++;
                continue;
            }
            memset(img, 0x2b, f8_probes[k].size);
            for (unsigned j = 0; j < f8_probes[k].nseg; j++)
                pm_b64(f8_probes[k].seg[j].b64, img + f8_probes[k].seg[j].addr);
            int ok = run(img, f8_probes[k].size, f8_probes[k].start, f8_probes[k].done, NULL, &res, &hazards);
            ok = ok && res.n == romc_probes[i].records && res.hash == romc_probes[i].hash && hazards == 0;
            printf("%-8s %9lu records  hash %016llX  %s\n", name, res.n, (unsigned long long)res.hash,
                   ok ? "ok" : "FAIL");
            if (!ok) {
                printf("  expected %u records, hash %016llX; hazards %u\n", romc_probes[i].records,
                       (unsigned long long)romc_probes[i].hash, hazards);
                failures++;
            }
            total += res.n;
            for (int s = 0; s < 32; s++)
                hist[s] += res.hist[s];
        }
        int seen = 0;
        printf("%lu records in all; states:", total);
        for (int s = 0; s < 32; s++)
            if (hist[s]) {
                printf(" %02X=%lu", s, hist[s]);
                seen++;
            }
        printf("\n%d of the 32 ROMC states reached\n", seen);
        /* MAME's console never raises an interrupt (0F 10 13), no instruction puts PC0
         * on the bus (1E 1F), and the reset's ROMC 08 comes before every window */
        if (seen != 26 || hist[0x08]) {
            printf("FAIL: expected 26 states\n");
            failures++;
        }
        return failures ? 1 : 0;
    }
#else
    fprintf(stderr, "usage: %s --run CART START DONE\n", argv[0]);
    return 2;
#endif
}
