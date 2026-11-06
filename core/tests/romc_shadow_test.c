/*
 * romc_shadow_test -- the cartridge firmware's own ROMC decoder, driven by this
 * core's bus.
 *
 * A Videocart has no address bus: five ROMC lines name what every memory device
 * does each cycle, and the cartridge keeps its own PC0/PC1/DC0/DC1 by following
 * them. The firmware's decoder is chf_bus_cycle() in channelf_cart.h (staged
 * into core/chf/fuji-generated/ from pico/channelf/firmware). This test feeds
 * it, state by state through romc_hook, the stream the F8 core makes running
 * every probe cartridge of tools/ab/f8_probes.py from power-on, and holds it to
 *
 *   - the CPU's registers: after every ROMC state the decoder's shadow PC0, PC1,
 *     DC0 and DC1 equal the core's;
 *   - the bus: the cartridge drives exactly where the F8 guide says it must
 *     (an independent predicate, as the firmware's own host test has), and what
 *     it drives is what the core put on the bus.
 *
 * The 32 ROMC states: the probes reach 27. The interrupt entry (0F, 10, 13) is
 * run here by hand (MAME's console never raises one); 1E and 1F, PC0 on the
 * bus, are in the F8 guide and in MAME but no instruction emits them.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "channelf_cart.h"
#include "f8.h"
#include "f8_probe_machine.h"
#include "f8_golden.h"

#define MAX_REPORT 10

typedef struct {
    pm_t m;
    chf_bus_t b;
    chf_mem_t mem;
    uint8_t ram[0x800];          /* the decoder's own copy of the cart RAM */
    unsigned long cycles, drove, bad;
    unsigned long hist[32];
    const char *name;
} rig_t;

/* What the cart must drive this cycle, from the F8 spec wording in MAME's ROMC
 * comments, independently of channelf_cart.h. Returns 1 and fills *want when
 * the cart must drive. */
static int expect_drive(uint8_t romc, const chf_bus_t *b, const chf_mem_t *m, uint8_t *want)
{
    switch (romc) {
    /* "the device whose address space includes the contents of PC0 must place on the
     * data bus the [op code / memory word] addressed by PC0" */
    case 0x00: case 0x01: case 0x03: case 0x0C: case 0x0E: case 0x11:
        if (b->pc0 < CHF_ROM_BASE)
            return 0;
        *want = chf_read(m, b->pc0);
        return 1;
    case 0x02:
        if (b->dc0 < CHF_ROM_BASE)
            return 0;
        *want = chf_read(m, b->dc0);
        return 1;
    case 0x06:
        if (b->dc0 < CHF_ROM_BASE)
            return 0;
        *want = (uint8_t)(b->dc0 >> 8);
        return 1;
    case 0x09:
        if (b->dc0 < CHF_ROM_BASE)
            return 0;
        *want = (uint8_t)b->dc0;
        return 1;
    case 0x07:
        if (b->pc1 < CHF_ROM_BASE)
            return 0;
        *want = (uint8_t)(b->pc1 >> 8);
        return 1;
    case 0x0B:
        if (b->pc1 < CHF_ROM_BASE)
            return 0;
        *want = (uint8_t)b->pc1;
        return 1;
    case 0x1E:
        if (b->pc0 < CHF_ROM_BASE)
            return 0;
        *want = (uint8_t)b->pc0;
        return 1;
    case 0x1F:
        if (b->pc0 < CHF_ROM_BASE)
            return 0;
        *want = (uint8_t)(b->pc0 >> 8);
        return 1;
    default:
        return 0;                /* CPU-sourced, or a port the cart does not claim */
    }
}

static void report(rig_t *r, const char *fmt, ...)
{
    va_list ap;
    if (r->bad < MAX_REPORT) {
        va_start(ap, fmt);
        fprintf(stderr, "romc_shadow: %s cycle %lu: ", r->name, r->cycles);
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
        va_end(ap);
    }
    r->bad++;
}

static void shadow(void *ctx, const f8_t *c, uint8_t romc, uint8_t dbus)
{
    rig_t *r = ctx;
    const chf_bus_t before = r->b;
    uint8_t want = 0;
    const int want_drive = expect_drive(romc, &before, &r->mem, &want);
    bool drive = false;
    uint8_t dval = 0;

    r->hist[romc & 31]++;
    chf_bus_cycle(&r->b, &r->mem, romc, dbus, &drive, &dval);
    if (drive)
        r->drove++;
    if ((int)drive != want_drive)
        report(r, "ROMC %02X: cart %s the bus, spec says it %s (PC0=%04X DC0=%04X PC1=%04X)", romc,
               drive ? "drove" : "left", want_drive ? "must drive" : "must not", before.pc0, before.dc0, before.pc1);
    else if (drive && dval != want)
        report(r, "ROMC %02X: cart drove $%02X, spec says $%02X", romc, dval, want);
    if (drive && dval != dbus)
        report(r, "ROMC %02X: cart drove $%02X, the bus had $%02X", romc, dval, dbus);
    if (r->b.pc0 != c->pc0 || r->b.pc1 != c->pc1 || r->b.dc0 != c->dc0 || r->b.dc1 != c->dc1) {
        report(r, "ROMC %02X: cart PC0=%04X PC1=%04X DC0=%04X DC1=%04X, cpu PC0=%04X PC1=%04X DC0=%04X DC1=%04X", romc,
               r->b.pc0, r->b.pc1, r->b.dc0, r->b.dc1, c->pc0, c->pc1, c->dc0, c->dc1);
        r->b.pc0 = c->pc0;       /* resync, or one slip floods the log */
        r->b.pc1 = c->pc1;
        r->b.dc0 = c->dc0;
        r->b.dc1 = c->dc1;
    }
    r->cycles++;
}

/* the CPU's context is the rig, so the machine's callbacks unwrap it */
static uint8_t rig_read(void *ctx, uint16_t a, uint8_t romc) { return pm_read(&((rig_t *)ctx)->m, a, romc); }
static void rig_write(void *ctx, uint16_t a, uint8_t d) { pm_write(&((rig_t *)ctx)->m, a, d); }
static uint8_t rig_in(void *ctx, uint8_t p) { return pm_in(&((rig_t *)ctx)->m, p); }
static void rig_out(void *ctx, uint8_t p, uint8_t d) { pm_out(&((rig_t *)ctx)->m, p, d); }

static void rig_init(rig_t *r, f8_t *cpu, const char *name, const uint8_t *img, size_t size)
{
    memset(r, 0, sizeof *r);
    memset(cpu, 0, sizeof *cpu);
    r->name = name;
    memcpy(r->m.rom, img, size);
    r->m.rom_size = size;
    r->m.stub = 1;
    r->mem.rom = r->m.rom;
    r->mem.rom_size = (uint32_t)size;
    r->mem.ram = r->ram;
    r->mem.ram_base = 0x2800;
    r->mem.ram_size = 0x800;
    cpu->ctx = r;
    cpu->read = rig_read;
    cpu->write = rig_write;
    cpu->in = rig_in;
    cpu->out = rig_out;
    cpu->romc_hook = shadow;
    r->m.quiet = 1;
    f8_power_on(cpu);            /* the decoder sees the reset from the first state */
    r->m.quiet = 0;
}

static unsigned long g_hist[32], g_bad, g_cycles, g_drove;

static void collect(const rig_t *r)
{
    for (int i = 0; i < 32; i++)
        g_hist[i] += r->hist[i];
    g_bad += r->bad;
    g_cycles += r->cycles;
    g_drove += r->drove;
}

static uint16_t vector_0a40(void *ctx)
{
    (void)ctx;
    return 0x0A40;
}

/* an interrupt entry, which no console probe can make: EI, then a request */
static void irq_scenario(void)
{
    static rig_t r;
    static f8_t cpu;
    static uint8_t img[0x2000];

    memset(img, 0x2b, sizeof img);
    /* $0802: EI; NOP; LI 5; NOP ... and at the vector $0A40: LIS 7; DI; NOP; NOP */
    memcpy(img + 0x0002, (uint8_t[]){ 0x1b, 0x2b, 0x20, 0x05, 0x2b, 0x2b, 0x2b }, 7);
    memcpy(img + 0x0240, (uint8_t[]){ 0x77, 0x1a, 0x2b, 0x2b, 0x2b }, 5);
    rig_init(&r, &cpu, "irq", img, sizeof img);
    cpu.int_vector = vector_0a40;
    cpu.irq_request = 1;
    for (int i = 0; i < 40; i++)
        f8_step(&cpu);
    if (cpu.irq_vector != 0x0A40)
        report(&r, "the vector callback was not used (%04X)", cpu.irq_vector);
    collect(&r);
}

int main(void)
{
    static uint8_t img[0x10000];
    static rig_t r;
    static f8_t cpu;
    int failures = 0;

    for (unsigned i = 0; i < sizeof f8_probes / sizeof f8_probes[0]; i++) {
        memset(img, 0x2b, f8_probes[i].size);
        for (unsigned j = 0; j < f8_probes[i].nseg; j++)
            pm_b64(f8_probes[i].seg[j].b64, img + f8_probes[i].seg[j].addr);
        rig_init(&r, &cpu, f8_probes[i].name, img, f8_probes[i].size);
        unsigned long guard = 0;
        while (f8_pc(&cpu) != f8_probes[i].done && guard++ < 50000000UL)
            f8_step(&cpu);
        if (f8_pc(&cpu) != f8_probes[i].done) {
            printf("%s: never reached DONE\n", f8_probes[i].name);
            failures++;
        }
        printf("%-8s %9lu ROMC cycles, %9lu driven by the cart, %lu divergences\n", f8_probes[i].name, r.cycles,
               r.drove, r.bad);
        collect(&r);
    }

    int probes_seen = 0;
    for (int i = 0; i < 32; i++)
        probes_seen += g_hist[i] != 0;
    irq_scenario();

    int seen = 0;
    printf("%lu cycles in all, %lu driven by the cart; states:", g_cycles, g_drove);
    for (int i = 0; i < 32; i++)
        if (g_hist[i]) {
            printf(" %02X=%lu", i, g_hist[i]);
            seen++;
        }
    printf("\n%d of the 32 ROMC states reached (%d by the probes, the interrupt entry adds %d)\n", seen, probes_seen,
           seen - probes_seen);
    if (probes_seen != 27 || seen != 30) {
        printf("FAIL: expected 27 + 3 states, 1E and 1F never\n");
        failures++;
    }
    if (g_hist[0x1E] || g_hist[0x1F]) {
        printf("FAIL: an instruction put PC0 on the bus\n");
        failures++;
    }
    if (g_bad) {
        printf("FAIL: %lu divergences\n", g_bad);
        failures++;
    }
    if (!failures)
        printf("PASS\n");
    return failures ? 1 : 0;
}
