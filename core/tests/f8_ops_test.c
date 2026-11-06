/*
 * f8_ops_test -- the F8 core against MAME's, on the probe cartridges of
 * tools/ab/f8_probes.py, in the model of the Channel F of f8_probe_machine.h.
 *
 *   f8_ops_test                           the ctest: every probe in f8_golden.h is
 *                                         run and its trace (row count, FNV-1a
 *                                         hash of every row) compared with MAME's
 *   f8_ops_test --run CART START DONE [MAXROWS]
 *                                         print the trace of one cartridge image,
 *                                         one row before every instruction from
 *                                         the first at START to the first at DONE
 *                                         (tools/ab/f8_ab.py diffs it with MAME's)
 *   f8_ops_test --coverage CART START DONE
 *                                         the opcodes the cartridge executes
 *   f8_ops_test --accesses CART START DONE
 *                                         every I/O port access and every access to the cart
 *                                         RAM ($2800-$2FFF) the cartridge makes, with the
 *                                         core's clock count at the callback (f8_ab.py
 *                                         --timing holds it to MAME's)
 *
 * A row is the state before the instruction (f8_probe_machine.h).
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
    size_t rows;
    int done;
    unsigned hazards;
    uint16_t hazard_addr;
    uint64_t hash;
    unsigned char seen[256];
    unsigned illegal;
} result_t;

/* run the image from the BIOS's entry; rows are recorded from the first
 * instruction at `start` to the first at `done`, as the Lua trace is cut */
static void run(const uint8_t *img, size_t size, unsigned start, unsigned done, size_t maxrows, FILE *out,
                result_t *res)
{
    static pm_t m;
    static f8_t cpu;
    char line[256];
    uint64_t base = 0;
    int on = 0;

    memset(res, 0, sizeof *res);
    res->hash = PM_FNV_OFFSET;
    pm_setup(&m, &cpu, img, size, 0);
    for (unsigned long guard = 0; guard < 50000000UL; guard++) {
        const unsigned pc = f8_pc(&cpu);
        if (!on && pc == start) {
            on = 1;
            base = cpu.cycles;
        }
        if (on) {
            pm_row(line, sizeof line, &cpu, base);
            res->hash = pm_fnv(res->hash, line, strlen(line));
            if (out)
                fputs(line, out);
            res->rows++;
            if (pc == done) {
                res->done = 1;
                break;
            }
            if (res->rows >= maxrows)
                break;
            res->seen[cpu.dbus] = 1;
        }
        f8_step(&cpu);
    }
    res->hazards = m.hazards;
    res->hazard_addr = m.hazard_addr;
    res->illegal = cpu.illegal;
}

/* the callbacks' view of time: each port access and cart RAM access, "IR|IW|MR|MW addr data cycles"
 * with the cycles counted from START, as tools/ab/f8_trace.lua taps them in MAME */
typedef struct {
    pm_t m;
    f8_t cpu;
    uint64_t base;
    int on;
} tap_t;

static void tap_log(tap_t *t, const char *kind, unsigned addr, unsigned data)
{
    if (t->on)
        printf("%s %04X %02X %llu\n", kind, addr, data, (unsigned long long)(t->cpu.cycles - t->base));
}
static uint8_t tap_read(void *ctx, uint16_t a, uint8_t romc)
{
    tap_t *t = ctx;
    const uint8_t v = pm_read(&t->m, a, romc);
    if (a >= 0x2800 && a < 0x3000)
        tap_log(t, "MR", a, v);
    return v;
}
static void tap_write(void *ctx, uint16_t a, uint8_t d)
{
    tap_t *t = ctx;
    pm_write(&t->m, a, d);
    if (a >= 0x2800 && a < 0x3000)
        tap_log(t, "MW", a, d);
}
static uint8_t tap_in(void *ctx, uint8_t p)
{
    tap_t *t = ctx;
    const uint8_t v = pm_in(&t->m, p);
    tap_log(t, "IR", p, v);
    return v;
}
static void tap_out(void *ctx, uint8_t p, uint8_t d)
{
    tap_t *t = ctx;
    pm_out(&t->m, p, d);
    tap_log(t, "IW", p, d);
}

static int run_accesses(const uint8_t *img, size_t size, unsigned start, unsigned done)
{
    static tap_t t;

    memset(&t, 0, sizeof t);
    pm_setup(&t.m, &t.cpu, img, size, 0);
    t.cpu.ctx = &t;
    t.cpu.read = tap_read;
    t.cpu.write = tap_write;
    t.cpu.in = tap_in;
    t.cpu.out = tap_out;
    for (unsigned long guard = 0; guard < 50000000UL; guard++) {
        const unsigned pc = f8_pc(&t.cpu);
        if (!t.on && pc == start) {
            t.on = 1;
            t.base = t.cpu.cycles;
        }
        if (pc == done)
            return 0;
        f8_step(&t.cpu);
    }
    return 1;
}

/* the image of a cartridge file or golden entry, padded the way the Python side pads it */
static uint8_t *load_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *p = malloc(0x10000);
    if (!f || !p) {
        perror(path);
        exit(2);
    }
    *size = fread(p, 1, 0x10000, f);
    fclose(f);
    return p;
}

#ifdef F8_GOLDEN
#include "f8_golden.h"
#endif

int main(int argc, char **argv)
{
    result_t res;

    if (argc >= 5 && strcmp(argv[1], "--accesses") == 0) {
        size_t size;
        uint8_t *img = load_file(argv[2], &size);
        return run_accesses(img, size, (unsigned)strtoul(argv[3], NULL, 16), (unsigned)strtoul(argv[4], NULL, 16));
    }
    if (argc >= 5 && (strcmp(argv[1], "--run") == 0 || strcmp(argv[1], "--coverage") == 0)) {
        const int cov = strcmp(argv[1], "--coverage") == 0;
        size_t size;
        uint8_t *img = load_file(argv[2], &size);
        const unsigned start = (unsigned)strtoul(argv[3], NULL, 16);
        const unsigned done = (unsigned)strtoul(argv[4], NULL, 16);
        size_t maxrows = argc > 5 ? (size_t)strtoull(argv[5], NULL, 0) : 50000000;
        run(img, size, start, done, maxrows, cov ? NULL : stdout, &res);
        fprintf(stderr, "rows %zu done %d hazards %u (last $%04X) illegal executed %u\n", res.rows, res.done,
                res.hazards, res.hazard_addr, res.illegal);
        if (cov) {
            int n = 0;
            for (int i = 0; i < 256; i++)
                if (res.seen[i]) {
                    printf("%02X ", i);
                    n++;
                }
            printf("\n%d opcodes\n", n);
        }
        return res.done ? 0 : 1;
    }
#ifdef F8_GOLDEN
    {
        int failures = 0;
        size_t rows_total = 0;
        static uint8_t seen[256];
        for (unsigned i = 0; i < sizeof f8_probes / sizeof f8_probes[0]; i++) {
            static uint8_t img[0x10000];
            memset(img, 0x2b, f8_probes[i].size);
            for (unsigned j = 0; j < f8_probes[i].nseg; j++)
                if (pm_b64(f8_probes[i].seg[j].b64, img + f8_probes[i].seg[j].addr) != f8_probes[i].seg[j].len) {
                    printf("%s: segment %u is not %u bytes\n", f8_probes[i].name, j, f8_probes[i].seg[j].len);
                    return 1;
                }
            run(img, f8_probes[i].size, f8_probes[i].start, f8_probes[i].done, (size_t)f8_probes[i].rows + 16, NULL, &res);
            int ok = res.done && res.rows == f8_probes[i].rows && res.hash == f8_probes[i].hash &&
                     res.hazards == 0;
            printf("%-8s %8zu rows  hash %016llX  %s\n", f8_probes[i].name, res.rows, (unsigned long long)res.hash,
                   ok ? "ok" : "FAIL");
            if (!ok) {
                printf("  expected %u rows, hash %016llX; done %d hazards %u\n", f8_probes[i].rows,
                       (unsigned long long)f8_probes[i].hash, res.done, res.hazards);
                failures++;
            }
            rows_total += res.rows;
            for (int k = 0; k < 256; k++)
                seen[k] |= res.seen[k];
        }
        int n = 0;
        for (int k = 0; k < 256; k++)
            n += seen[k] != 0;
        printf("%zu rows in all, %d of 256 opcodes executed\n", rows_total, n);
        if (n != 256) {
            printf("FAIL: the probes do not execute every opcode\n");
            failures++;
        }
        return failures ? 1 : 0;
    }
#else
    fprintf(stderr, "usage: %s --run CART START DONE [MAXROWS] | --coverage|--accesses CART START DONE\n", argv[0]);
    return 2;
#endif
}
