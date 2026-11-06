/*
 * f8_irq_test -- what MAME's Channel F cannot exercise of the F8 core, and its
 * API: the interrupt entry, the privileged instructions that never take one,
 * reset and f8_set_pc semantics, when each callback sees `cycles`, the clocks
 * every instruction class takes, illegal opcodes, unwired callbacks.
 *
 * The probes of tools/ab/f8_probes.py (f8_ops_test) hold the core to MAME
 * instruction for instruction; the console never asserts /INTREQ and its BIOS
 * never uses the timer, so the expected values here are worked out from MAME's
 * f8.cpp (execute_run's freeze cycle, ROMC_0F, ROMC_13) and checked by hand.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "f8.h"

static int g_fail;

#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            if (g_fail < 40) { \
                printf("FAIL line %d: ", __LINE__); \
                printf(__VA_ARGS__); \
                printf("\n"); \
            } \
            g_fail++; \
        } \
    } while (0)

/* ---- a flat machine that logs every access ---- */
enum { EV_READ, EV_WRITE, EV_IN, EV_OUT, EV_HOOK, EV_VECTOR };

typedef struct {
    int kind;
    uint16_t addr;          /* address or port */
    uint8_t romc, data;
    uint64_t cycles;
    uint16_t pc0, pc1, dc0, dc1;
} ev_t;

typedef struct {
    uint8_t mem[0x10000];
    uint8_t port_in;        /* what IN reads */
    ev_t log[512];
    int n;
    f8_t *cpu;
} bus_t;

static void note(bus_t *b, int kind, unsigned addr, int romc, int data)
{
    if (b->n < 512) {
        ev_t *e = &b->log[b->n++];
        e->kind = kind;
        e->addr = (uint16_t)addr;
        e->romc = (uint8_t)romc;
        e->data = (uint8_t)data;
        e->cycles = b->cpu->cycles;
    }
}

static uint8_t bus_read(void *ctx, uint16_t a, uint8_t romc)
{
    bus_t *b = ctx;
    note(b, EV_READ, a, romc, b->mem[a]);
    return b->mem[a];
}
static void bus_write(void *ctx, uint16_t a, uint8_t d)
{
    bus_t *b = ctx;
    note(b, EV_WRITE, a, 5, d);
    b->mem[a] = d;
}
static uint8_t bus_in(void *ctx, uint8_t p)
{
    bus_t *b = ctx;
    note(b, EV_IN, p, 0x1b, b->port_in);
    return b->port_in;
}
static void bus_out(void *ctx, uint8_t p, uint8_t d)
{
    note(ctx, EV_OUT, p, 0x1a, d);
}
static void bus_hook(void *ctx, const f8_t *c, uint8_t romc, uint8_t dbus)
{
    bus_t *b = ctx;
    note(b, EV_HOOK, 0, romc, dbus);
    if (b->n) {
        ev_t *e = &b->log[b->n - 1];
        e->pc0 = c->pc0;
        e->pc1 = c->pc1;
        e->dc0 = c->dc0;
        e->dc1 = c->dc1;
        e->cycles = c->cycles;
    }
}
static uint16_t bus_vector(void *ctx)
{
    note(ctx, EV_VECTOR, 0, 0x0f, 0);
    return 0x0A40;
}

static void setup(bus_t *b, f8_t *c, int hook)
{
    memset(b, 0, sizeof *b);
    memset(b->mem, 0x2b, sizeof b->mem);        /* NOP */
    memset(c, 0, sizeof *c);
    b->cpu = c;
    c->ctx = b;
    c->read = bus_read;
    c->write = bus_write;
    c->in = bus_in;
    c->out = bus_out;
    if (hook)
        c->romc_hook = bus_hook;
    f8_power_on(c);
    b->n = 0;
}

/* put an instruction at `pc` and park the CPU on it */
static void at(bus_t *b, f8_t *c, unsigned pc, const uint8_t *bytes, int n)
{
    memcpy(b->mem + pc, bytes, (size_t)n);
    f8_set_pc(c, (uint16_t)pc);
    b->n = 0;
}

/* the k-th ROMC hook event of the log */
static const ev_t *hook_n(const bus_t *b, int k)
{
    for (int i = 0; i < b->n; i++)
        if (b->log[i].kind == EV_HOOK && k-- == 0)
            return &b->log[i];
    return NULL;
}

/* the ROMC states the hook saw, in order, as a string of two-digit hex pairs */
static const char *states(const bus_t *b)
{
    static char s[2048];
    char *p = s;
    *p = 0;
    for (int i = 0; i < b->n; i++)
        if (b->log[i].kind == EV_HOOK)
            p += sprintf(p, "%s%02X", p == s ? "" : " ", b->log[i].romc);
    return s;
}

/* ---- tests ---- */

static void test_constants(void)
{
    CHECK(F8_SIGN == 1 && F8_CARRY == 2 && F8_ZERO == 4 && F8_OVR == 8 && F8_ICB == 16, "W bit values");
}

static void test_power_on_reset(void)
{
    static bus_t b;
    static f8_t c;

    memset(&b, 0, sizeof b);
    memset(b.mem, 0x2b, sizeof b.mem);
    b.mem[0] = 0x70;                                  /* CLR */
    memset(&c, 0xa5, sizeof c);                       /* power-on must not trust the struct */
    b.cpu = &c;
    c.ctx = &b;
    c.read = bus_read;
    c.write = bus_write;
    c.in = bus_in;
    c.out = bus_out;
    c.int_vector = NULL;
    c.romc_hook = bus_hook;
    f8_power_on(&c);
    CHECK(c.pc0 == 1 && c.pc1 == 0 && c.dc0 == 0 && c.dc1 == 0, "power-on: pc0 %04X pc1 %04X dc %04X %04X", c.pc0, c.pc1, c.dc0, c.dc1);
    CHECK(c.a == 0 && c.w == 0 && c.is == 0 && c.io == 0 && c.irq_vector == 0 && c.irq_request == 0, "power-on registers");
    CHECK(c.dbus == 0x70 && f8_pc(&c) == 0, "power-on fetched the opcode at 0: dbus %02X pc %04X", c.dbus, f8_pc(&c));
    CHECK(c.cycles == 0 && c.instructions == 0, "power-on charges nothing: %llu", (unsigned long long)c.cycles);
    CHECK(c.illegal == 0 && c.illegal_pc == -1, "illegal counters");
    {
        int zero = 1;
        for (int i = 0; i < 64; i++)
            zero &= c.r[i] == 0;
        CHECK(zero, "scratchpad cleared");
    }
    CHECK(strcmp(states(&b), "08 00") == 0, "reset's states: %s", states(&b));
    CHECK(hook_n(&b, 0)->cycles == 0 && hook_n(&b, 1)->cycles == 0, "hook sees cycles 0 at both reset states");
    CHECK(hook_n(&b, 0)->pc0 == 0 && hook_n(&b, 1)->pc0 == 1, "hook sees the post-state: PC0 %04X, %04X",
          hook_n(&b, 0)->pc0, hook_n(&b, 1)->pc0);
    CHECK(b.n == 3 && b.log[1].kind == EV_READ && b.log[1].addr == 0 && b.log[1].romc == 0, "the fetch went through read");

    /* f8_reset keeps everything but PC0/PC1/ICB/dbus and charges nothing */
    c.a = 5;
    c.w = 0x1f;
    c.is = 9;
    c.r[3] = 7;
    c.dc0 = 0x1234;
    c.dc1 = 0x5678;
    c.pc0 = 0x4000;
    c.cycles = 1000;
    c.instructions = 77;
    b.mem[0] = 0x2b;
    b.n = 0;
    f8_reset(&c);
    CHECK(c.pc1 == 0x4000 && c.pc0 == 1, "reset: PC1 takes the old PC0 (%04X), PC0 restarts (%04X)", c.pc1, c.pc0);
    CHECK(c.w == 0x0f, "reset clears ICB only: W %02X", c.w);
    CHECK(c.a == 5 && c.is == 9 && c.r[3] == 7 && c.dc0 == 0x1234 && c.dc1 == 0x5678, "reset keeps A, ISAR, the scratchpad, DC0/DC1");
    CHECK(c.cycles == 1000 && c.instructions == 77, "reset charges nothing: %llu", (unsigned long long)c.cycles);
    CHECK(c.dbus == 0x2b, "reset fetched the opcode at 0 (%02X)", c.dbus);
    CHECK(strcmp(states(&b), "08 00") == 0, "reset's states: %s", states(&b));
    CHECK(hook_n(&b, 0)->cycles == 1000 && hook_n(&b, 1)->cycles == 1000, "reset's hook sees the cycles unchanged: %llu",
          (unsigned long long)hook_n(&b, 0)->cycles);
}

static void test_set_pc(void)
{
    static bus_t b;
    static f8_t c;

    setup(&b, &c, 1);
    c.cycles = 500;
    b.mem[0x1234] = 0x77;           /* LIS 7 */
    f8_set_pc(&c, 0x1234);
    CHECK(c.pc0 == 0x1235 && c.dbus == 0x77 && f8_pc(&c) == 0x1234, "set_pc: PC0 %04X dbus %02X", c.pc0, c.dbus);
    CHECK(c.cycles == 500, "set_pc charges nothing");
    CHECK(b.n == 1 && b.log[0].kind == EV_READ && b.log[0].addr == 0x1234 && b.log[0].romc == 0x00,
          "set_pc: one read of the opcode, ROMC 00 (n=%d)", b.n);
    CHECK(strcmp(states(&b), "") == 0, "set_pc does not call the hook");
    f8_step(&c);
    CHECK(c.a == 7, "the fetched opcode ran (A=%02X)", c.a);
    f8_set_pc(&c, 0xffff);
    CHECK(c.pc0 == 0x0000 && f8_pc(&c) == 0xffff, "set_pc at $FFFF wraps PC0 to 0: %04X", c.pc0);
}

/* clocks of a whole instruction (body + the trailing fetch) */
static void test_clocks(void)
{
    static const struct { const char *name; uint8_t b[3]; int n; uint8_t w; int is; int clocks; } t[] = {
        { "NOP", { 0x2b }, 1, 0, 0, 4 },
        { "LR A,KU", { 0x00 }, 1, 0, 0, 4 },
        { "LR A,IS", { 0x0a }, 1, 0, 0, 4 },
        { "SR 1", { 0x12 }, 1, 0, 0, 4 },
        { "INC", { 0x1f }, 1, 0, 0, 4 },
        { "LI", { 0x20, 5 }, 2, 0, 0, 10 },
        { "AI", { 0x24, 5 }, 2, 0, 0, 10 },
        { "IN", { 0x26, 4 }, 2, 0, 0, 16 },
        { "OUT", { 0x27, 4 }, 2, 0, 0, 16 },
        { "PI", { 0x28, 0x0a, 0x40 }, 3, 0, 0, 26 },
        { "JMP", { 0x29, 0x0a, 0x40 }, 3, 0, 0, 22 },
        { "DCI", { 0x2a, 0x12, 0x34 }, 3, 0, 0, 24 },
        { "XDC", { 0x2c }, 1, 0, 0, 8 },
        { "DS R0", { 0x30 }, 1, 0, 0, 6 },
        { "DS ISAR", { 0x3c }, 1, 0, 0, 6 },
        { "illegal 3F (a long fetch too)", { 0x3f }, 1, 0, 0, 6 },
        { "illegal 2D", { 0x2d }, 1, 0, 0, 4 },
        { "LM", { 0x16 }, 1, 0, 0, 10 },
        { "ST", { 0x17 }, 1, 0, 0, 10 },
        { "AM", { 0x88 }, 1, 0, 0, 10 },
        { "AMD", { 0x89 }, 1, 0, 0, 10 },
        { "ADC", { 0x8e }, 1, 0, 0, 10 },
        { "LR K,P", { 0x08 }, 1, 0, 0, 16 },
        { "LR P,K", { 0x09 }, 1, 0, 0, 16 },
        { "PK", { 0x0c }, 1, 0, 0, 16 },
        { "LR P0,Q", { 0x0d }, 1, 0, 0, 16 },
        { "LR Q,DC", { 0x0e }, 1, 0, 0, 16 },
        { "LR DC,Q", { 0x0f }, 1, 0, 0, 16 },
        { "POP", { 0x1c }, 1, 0, 0, 8 },
        { "EI", { 0x1b }, 1, 0, 0, 8 },
        { "DI", { 0x1a }, 1, 0, 0, 8 },
        { "LR W,J", { 0x1d }, 1, 0, 0, 8 },
        { "ASD R0", { 0xd0 }, 1, 0, 0, 8 },
        { "ASD ISAR", { 0xdc }, 1, 0, 0, 8 },
        { "BR (taken)", { 0x90, 0x10 }, 2, 0, 0, 14 },
        { "BT 1 (not taken)", { 0x81, 0x10 }, 2, 0, 0, 12 },
        { "BT 1 (taken)", { 0x81, 0x10 }, 2, F8_SIGN, 0, 14 },
        { "BF 1 (taken)", { 0x91, 0x10 }, 2, 0, 0, 14 },
        { "BF 1 (not taken)", { 0x91, 0x10 }, 2, F8_SIGN, 0, 12 },
        { "BR7 (taken)", { 0x8f, 0x10 }, 2, 0, 0, 10 },
        { "BR7 (not taken)", { 0x8f, 0x10 }, 2, 0, 7, 8 },
        { "INS 0", { 0xa0 }, 1, 0, 0, 8 },
        { "INS 1", { 0xa1 }, 1, 0, 0, 8 },
        { "INS 4", { 0xa4 }, 1, 0, 0, 16 },
        { "OUTS 0", { 0xb0 }, 1, 0, 0, 8 },
        { "OUTS 1", { 0xb1 }, 1, 0, 0, 8 },
        { "OUTS 4 (privileged)", { 0xb4 }, 1, 0, 0, 16 },
    };
    static bus_t b;
    static f8_t c;

    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        setup(&b, &c, 0);
        c.w = t[i].w;
        c.is = (uint8_t)t[i].is;
        c.dc0 = 0x3000;
        at(&b, &c, 0x1000, t[i].b, t[i].n);
        const uint64_t before = c.cycles;
        const int took = f8_step(&c);
        CHECK(took == t[i].clocks, "%s: f8_step returned %d, expected %d", t[i].name, took, t[i].clocks);
        CHECK(c.cycles - before == (unsigned)t[i].clocks, "%s: cycles advanced by %llu", t[i].name,
              (unsigned long long)(c.cycles - before));
        CHECK(c.instructions == 1, "%s: instructions %llu", t[i].name, (unsigned long long)c.instructions);
    }
    printf("clocks: %u instruction classes\n", (unsigned)(sizeof t / sizeof t[0]));
}

/* what each callback sees of `cycles`, and the hook's states, access by access */
static void test_callbacks(void)
{
    static bus_t b;
    static f8_t c;

    /* LI: the operand fetch (ROMC 03) at 0, the trailing opcode fetch (00) after its 6 clocks */
    setup(&b, &c, 1);
    at(&b, &c, 0x1000, (uint8_t[]){ 0x20, 0x05 }, 2);
    c.cycles = 100;
    f8_step(&c);
    CHECK(strcmp(states(&b), "03 00") == 0, "LI states: %s", states(&b));
    CHECK(b.n == 4 && b.log[0].kind == EV_READ && b.log[0].addr == 0x1001 && b.log[0].romc == 0x03 && b.log[0].cycles == 100,
          "LI operand read: addr %04X romc %02X at %llu", b.log[0].addr, b.log[0].romc, (unsigned long long)b.log[0].cycles);
    CHECK(b.log[1].kind == EV_HOOK && b.log[1].cycles == 100 && b.log[1].pc0 == 0x1002,
          "the hook sees the state's cycles before they are charged: %llu", (unsigned long long)b.log[1].cycles);
    CHECK(b.log[2].kind == EV_READ && b.log[2].addr == 0x1002 && b.log[2].romc == 0x00 && b.log[2].cycles == 106,
          "trailing fetch: addr %04X romc %02X at %llu", b.log[2].addr, b.log[2].romc, (unsigned long long)b.log[2].cycles);
    CHECK(c.a == 5 && c.cycles == 110, "LI result: A %02X cycles %llu", c.a, (unsigned long long)c.cycles);

    /* IN 4: operand fetch at 0, the port read in ROMC 1B at 6, the trailing fetch at 12 */
    setup(&b, &c, 1);
    b.port_in = 0x6b;
    at(&b, &c, 0x1000, (uint8_t[]){ 0x26, 0x04 }, 2);
    f8_step(&c);
    CHECK(strcmp(states(&b), "03 1B 00") == 0, "IN states: %s", states(&b));
    {
        int found = 0;
        for (int i = 0; i < b.n; i++)
            if (b.log[i].kind == EV_IN) {
                found++;
                CHECK(b.log[i].addr == 4 && b.log[i].cycles == 6, "IN: port %02X at %llu", b.log[i].addr,
                      (unsigned long long)b.log[i].cycles);
            }
        CHECK(found == 1, "IN: one port read");
    }
    CHECK(c.a == 0x6b && c.io == 4, "IN result: A %02X io %04X", c.a, c.io);

    /* OUT 5: the port write is ROMC 1A, at 6 */
    setup(&b, &c, 1);
    c.a = 0x3c;
    at(&b, &c, 0x1000, (uint8_t[]){ 0x27, 0x05 }, 2);
    f8_step(&c);
    CHECK(strcmp(states(&b), "03 1A 00") == 0, "OUT states: %s", states(&b));
    for (int i = 0; i < b.n; i++)
        if (b.log[i].kind == EV_OUT)
            CHECK(b.log[i].addr == 5 && b.log[i].data == 0x3c && b.log[i].cycles == 6, "OUT: port %02X data %02X at %llu",
                  b.log[i].addr, b.log[i].data, (unsigned long long)b.log[i].cycles);

    /* INS 0 / OUTS 1: ROMC 1C and a direct access with no 1A/1B state of its own: the port
     * is touched after the 1C's four clocks */
    setup(&b, &c, 1);
    b.port_in = 0x11;
    at(&b, &c, 0x1000, (uint8_t[]){ 0xa0 }, 1);
    f8_step(&c);
    CHECK(strcmp(states(&b), "1C 00") == 0, "INS 0 states: %s", states(&b));
    for (int i = 0; i < b.n; i++)
        if (b.log[i].kind == EV_IN)
            CHECK(b.log[i].addr == 0 && b.log[i].cycles == 4, "INS 0: port %u at %llu", b.log[i].addr,
                  (unsigned long long)b.log[i].cycles);
    CHECK(c.a == 0x11 && c.io == 0, "INS 0 leaves io alone: A %02X io %04X", c.a, c.io);
    setup(&b, &c, 1);
    c.a = 0x77;
    at(&b, &c, 0x1000, (uint8_t[]){ 0xb1 }, 1);
    f8_step(&c);
    CHECK(strcmp(states(&b), "1C 00") == 0, "OUTS 1 states: %s", states(&b));
    for (int i = 0; i < b.n; i++)
        if (b.log[i].kind == EV_OUT)
            CHECK(b.log[i].addr == 1 && b.log[i].data == 0x77 && b.log[i].cycles == 4, "OUTS 1: port %u at %llu", b.log[i].addr,
                  (unsigned long long)b.log[i].cycles);

    /* INS 4 / OUTS 4 go through 1B / 1A */
    setup(&b, &c, 1);
    at(&b, &c, 0x1000, (uint8_t[]){ 0xa4 }, 1);
    f8_step(&c);
    CHECK(strcmp(states(&b), "1C 1B 00") == 0, "INS 4 states: %s", states(&b));
    setup(&b, &c, 1);
    at(&b, &c, 0x1000, (uint8_t[]){ 0xb4 }, 1);
    f8_step(&c);
    CHECK(strcmp(states(&b), "1C 1A 00") == 0, "OUTS 4 states: %s", states(&b));

    /* ST, LM: the data read is ROMC 02 through DC0, the write ROMC 05, both at the start of their state */
    setup(&b, &c, 1);
    c.a = 0x99;
    c.dc0 = 0x2000;
    at(&b, &c, 0x1000, (uint8_t[]){ 0x17 }, 1);
    f8_step(&c);
    CHECK(b.mem[0x2000] == 0x99 && c.dc0 == 0x2001, "ST stored and stepped DC0");
    CHECK(strcmp(states(&b), "05 00") == 0, "ST states: %s", states(&b));
    CHECK(b.log[0].kind == EV_WRITE && b.log[0].addr == 0x2000 && b.log[0].cycles == 0, "ST write at cycle 0");
    setup(&b, &c, 1);
    b.mem[0x2000] = 0x5a;
    c.dc0 = 0x2000;
    at(&b, &c, 0x1000, (uint8_t[]){ 0x16 }, 1);
    f8_step(&c);
    CHECK(c.a == 0x5a && c.dc0 == 0x2001, "LM");
    CHECK(b.log[0].kind == EV_READ && b.log[0].romc == 0x02 && b.log[0].addr == 0x2000, "LM reads through ROMC 02");

    /* DCI: 11 03 0E 03, the second pair short */
    setup(&b, &c, 1);
    at(&b, &c, 0x1000, (uint8_t[]){ 0x2a, 0x12, 0x34 }, 3);
    f8_step(&c);
    CHECK(strcmp(states(&b), "11 03 0E 03 00") == 0, "DCI states: %s", states(&b));
    CHECK(c.dc0 == 0x1234, "DCI: DC0 %04X", c.dc0);
    {
        unsigned want_cycles[5] = { 0, 6, 10, 16, 20 }, k = 0;
        for (int i = 0; i < b.n; i++)
            if (b.log[i].kind == EV_READ)
                CHECK(k < 5 && b.log[i].cycles == want_cycles[k++], "DCI read %u at %llu", k,
                      (unsigned long long)b.log[i].cycles);
    }

    /* JMP: the hook's records, with the registers each state left */
    setup(&b, &c, 1);
    c.a = 0x55;
    at(&b, &c, 0x1000, (uint8_t[]){ 0x29, 0x0a, 0x40 }, 3);
    f8_step(&c);
    CHECK(strcmp(states(&b), "03 0C 14 00") == 0, "JMP states: %s", states(&b));
    CHECK(c.a == 0x0a && f8_pc(&c) == 0x0a40, "JMP leaves A = the target's high byte: A %02X pc %04X", c.a, f8_pc(&c));
    {
        const ev_t *h[4];
        int k = 0;
        for (int i = 0; i < b.n && k < 4; i++)
            if (b.log[i].kind == EV_HOOK)
                h[k++] = &b.log[i];
        CHECK(h[0]->data == 0x0a && h[0]->pc0 == 0x1002, "ROMC 03: dbus %02X PC0 %04X", h[0]->data, h[0]->pc0);
        CHECK(h[1]->data == 0x40 && h[1]->pc0 == 0x1040, "ROMC 0C: dbus %02X PC0 %04X", h[1]->data, h[1]->pc0);
        CHECK(h[2]->data == 0x0a && h[2]->pc0 == 0x0a40, "ROMC 14: dbus %02X PC0 %04X", h[2]->data, h[2]->pc0);
        CHECK(h[3]->pc0 == 0x0a41, "ROMC 00: PC0 %04X", h[3]->pc0);
    }

    /* PI: the same, and PC1 is the instruction after it */
    setup(&b, &c, 1);
    at(&b, &c, 0x1000, (uint8_t[]){ 0x28, 0x0a, 0x40 }, 3);
    f8_step(&c);
    CHECK(strcmp(states(&b), "03 0D 0C 14 00") == 0, "PI states: %s", states(&b));
    CHECK(c.pc1 == 0x1003 && f8_pc(&c) == 0x0a40 && c.a == 0x0a, "PI: PC1 %04X pc %04X A %02X", c.pc1, f8_pc(&c), c.a);
}

static void test_semantics(void)
{
    static bus_t b;
    static f8_t c;

    /* ISAR++ / ISAR-- move only the low three bits */
    setup(&b, &c, 0);
    c.is = 0x0f;
    c.r[0x0f] = 0xaa;
    at(&b, &c, 0x1000, (uint8_t[]){ 0x4d }, 1);        /* LR A,I */
    f8_step(&c);
    CHECK(c.a == 0xaa && c.is == 0x08, "LR A,ISAR++ from 0x0F wraps in the row: IS %02X", c.is);
    c.is = 0x08;
    at(&b, &c, 0x1000, (uint8_t[]){ 0x4e }, 1);        /* LR A,D */
    f8_step(&c);
    CHECK(c.is == 0x0f, "LR A,ISAR-- from 0x08 wraps in the row: IS %02X", c.is);
    c.a = 0xff;
    at(&b, &c, 0x1000, (uint8_t[]){ 0x0b }, 1);        /* LR IS,A */
    f8_step(&c);
    CHECK(c.is == 0x3f, "LR IS,A keeps 6 bits: %02X", c.is);

    /* BR7 tests ISARL != 7 */
    setup(&b, &c, 0);
    c.is = 0x07;
    at(&b, &c, 0x1000, (uint8_t[]){ 0x8f, 0x10 }, 2);
    f8_step(&c);
    CHECK(f8_pc(&c) == 0x1002, "BR7 falls through at ISARL 7: %04X", f8_pc(&c));
    c.is = 0x06;
    at(&b, &c, 0x1000, (uint8_t[]){ 0x8f, 0x10 }, 2);
    f8_step(&c);
    CHECK(f8_pc(&c) == 0x1011, "BR7 branches otherwise: %04X (offset from the offset byte)", f8_pc(&c));

    /* W's S flag is set for a POSITIVE result */
    setup(&b, &c, 0);
    c.a = 0x7f;
    at(&b, &c, 0x1000, (uint8_t[]){ 0x24, 0x00 }, 2);   /* AI 0 */
    f8_step(&c);
    CHECK((c.w & F8_SIGN) && !(c.w & F8_ZERO), "AI 0 of $7F: S set (positive), W %02X", c.w);
    c.a = 0x80;
    at(&b, &c, 0x1000, (uint8_t[]){ 0x24, 0x00 }, 2);
    f8_step(&c);
    CHECK(!(c.w & F8_SIGN), "AI 0 of $80: S clear (negative), W %02X", c.w);
    c.a = 0x00;
    at(&b, &c, 0x1000, (uint8_t[]){ 0x24, 0x00 }, 2);
    f8_step(&c);
    CHECK((c.w & (F8_SIGN | F8_ZERO)) == (F8_SIGN | F8_ZERO), "AI 0 of 0: Z and S set, W %02X", c.w);
    c.a = 0x7f;
    at(&b, &c, 0x1000, (uint8_t[]){ 0x24, 0x01 }, 2);   /* signed overflow */
    f8_step(&c);
    CHECK((c.w & F8_OVR) && !(c.w & F8_CARRY), "AI 1 of $7F overflows: W %02X", c.w);

    /* decimal adjust (the F8 guide's recipe: add $66 first) */
    setup(&b, &c, 0);
    c.a = 0x12 + 0x66;
    c.r[0] = 0x29;
    at(&b, &c, 0x1000, (uint8_t[]){ 0xd0 }, 1);         /* ASD R0 */
    f8_step(&c);
    CHECK(c.a == 0x41, "12 + 29 = %02X (decimal)", c.a);
    c.a = 0x99 + 0x66;
    c.r[0] = 0x01;
    at(&b, &c, 0x1000, (uint8_t[]){ 0xd0 }, 1);
    f8_step(&c);
    CHECK(c.a == 0x00 && (c.w & F8_CARRY), "99 + 01 = %02X with carry (W %02X)", c.a, c.w);
    c.a = 0x25 + 0x66;
    c.r[0] = 0x19;
    at(&b, &c, 0x1000, (uint8_t[]){ 0xd0 }, 1);
    f8_step(&c);
    CHECK(c.a == 0x44, "25 + 19 = %02X", c.a);

    /* ADC adds the data bus (A) to DC0 as a signed byte */
    setup(&b, &c, 0);
    c.dc0 = 0x2000;
    c.a = 0xfe;
    at(&b, &c, 0x1000, (uint8_t[]){ 0x8e }, 1);
    f8_step(&c);
    CHECK(c.dc0 == 0x1ffe, "ADC of $FE is -2: DC0 %04X", c.dc0);
    c.a = 0x7f;
    at(&b, &c, 0x1000, (uint8_t[]){ 0x8e }, 1);
    f8_step(&c);
    CHECK(c.dc0 == 0x207d, "ADC of $7F is +127: DC0 %04X", c.dc0);

    /* the relative branch is a signed byte added to the address of the offset byte, across a page */
    setup(&b, &c, 0);
    at(&b, &c, 0x10fe, (uint8_t[]){ 0x90, 0x10 }, 2);
    f8_step(&c);
    CHECK(f8_pc(&c) == 0x110f, "BR across a page: %04X", f8_pc(&c));
    at(&b, &c, 0x1102, (uint8_t[]){ 0x90, 0xf0 }, 2);
    f8_step(&c);
    CHECK(f8_pc(&c) == 0x10f3, "BR backwards across a page: %04X", f8_pc(&c));

    /* DI / EI / LR W,J / LR J,W */
    setup(&b, &c, 0);
    at(&b, &c, 0x1000, (uint8_t[]){ 0x1b }, 1);
    f8_step(&c);
    CHECK(c.w & F8_ICB, "EI sets ICB");
    at(&b, &c, 0x1000, (uint8_t[]){ 0x1a }, 1);
    f8_step(&c);
    CHECK(!(c.w & F8_ICB), "DI clears ICB");
    c.r[9] = 0xff;
    at(&b, &c, 0x1000, (uint8_t[]){ 0x1d }, 1);
    f8_step(&c);
    CHECK(c.w == 0x1f, "LR W,J takes 5 bits: W %02X", c.w);
    c.w = 0x15;
    at(&b, &c, 0x1000, (uint8_t[]){ 0x1e }, 1);
    f8_step(&c);
    CHECK(c.r[9] == 0x15, "LR J,W");

    /* PK, LR P0,Q, POP, LR K,P, LR P,K */
    setup(&b, &c, 0);
    c.r[12] = 0x23;
    c.r[13] = 0x45;
    at(&b, &c, 0x1000, (uint8_t[]){ 0x0c }, 1);          /* PK: PC0 = K, PC1 = the next instruction */
    f8_step(&c);
    CHECK(f8_pc(&c) == 0x2345 && c.pc1 == 0x1001, "PK: pc %04X PC1 %04X", f8_pc(&c), c.pc1);
    at(&b, &c, 0x1000, (uint8_t[]){ 0x1c }, 1);          /* POP: PC0 = PC1 */
    c.pc1 = 0x3456;
    f8_step(&c);
    CHECK(f8_pc(&c) == 0x3456, "POP: pc %04X", f8_pc(&c));
    c.pc1 = 0x1234;
    at(&b, &c, 0x1000, (uint8_t[]){ 0x08 }, 1);          /* LR K,P */
    f8_step(&c);
    CHECK(c.r[12] == 0x12 && c.r[13] == 0x34, "LR K,P");
    c.r[12] = 0x56;
    c.r[13] = 0x78;
    at(&b, &c, 0x1000, (uint8_t[]){ 0x09 }, 1);          /* LR P,K */
    f8_step(&c);
    CHECK(c.pc1 == 0x5678, "LR P,K: PC1 %04X", c.pc1);
    c.r[14] = 0x9a;
    c.r[15] = 0xbc;
    at(&b, &c, 0x1000, (uint8_t[]){ 0x0d }, 1);          /* LR P0,Q */
    f8_step(&c);
    CHECK(f8_pc(&c) == 0x9abc, "LR P0,Q: pc %04X", f8_pc(&c));
    c.dc0 = 0xdef0;
    at(&b, &c, 0x1000, (uint8_t[]){ 0x0e }, 1);          /* LR Q,DC */
    f8_step(&c);
    CHECK(c.r[14] == 0xde && c.r[15] == 0xf0, "LR Q,DC");
    c.dc0 = 0;
    at(&b, &c, 0x1000, (uint8_t[]){ 0x0f }, 1);          /* LR DC,Q */
    f8_step(&c);
    CHECK(c.dc0 == 0xdef0, "LR DC,Q");

    /* XDC */
    c.dc0 = 0x1111;
    c.dc1 = 0x2222;
    at(&b, &c, 0x1000, (uint8_t[]){ 0x2c }, 1);
    f8_step(&c);
    CHECK(c.dc0 == 0x2222 && c.dc1 == 0x1111, "XDC");
}

static void test_illegal(void)
{
    static const uint8_t bad[] = { 0x2d, 0x2e, 0x2f, 0x3f, 0x4f, 0x5f, 0xa2, 0xa3, 0xb2, 0xb3, 0xcf, 0xdf, 0xef, 0xff };
    static bus_t b;
    static f8_t c;

    for (int op = 0; op < 256; op++) {
        int illegal = 0;
        for (unsigned i = 0; i < sizeof bad; i++)
            illegal |= bad[i] == op;
        setup(&b, &c, 0);
        c.a = 0x12;
        c.w = 0x0a;
        c.is = 0x15;
        c.dc0 = 0x3000;
        c.dc1 = 0x4000;
        c.r[5] = 0x77;
        at(&b, &c, 0x1234, (uint8_t[]){ (uint8_t)op }, 1);
        f8_step(&c);
        CHECK((c.illegal == 1) == illegal, "opcode %02X: illegal count %u", op, c.illegal);
        if (illegal) {
            CHECK(c.illegal_pc == 0x1234, "opcode %02X: illegal_pc %d", op, c.illegal_pc);
            CHECK(c.a == 0x12 && c.w == 0x0a && c.is == 0x15 && c.dc0 == 0x3000 && c.dc1 == 0x4000 && c.r[5] == 0x77,
                  "opcode %02X changed state", op);
            CHECK(f8_pc(&c) == 0x1235, "opcode %02X: pc %04X", op, f8_pc(&c));
        } else {
            CHECK(c.illegal_pc == -1, "opcode %02X: illegal_pc %d", op, c.illegal_pc);
        }
    }
    printf("illegal: 14 opcodes are no-ops and counted, 242 are not\n");
}

/* the interrupt entry, and who is not allowed to take one */
static void test_interrupt(void)
{
    static bus_t b;
    static f8_t c;

    /* NOP with ICB set and /INTREQ asserted: freeze cycle 10, 1C (long), 0F, 13, then the fetch */
    setup(&b, &c, 1);
    c.w = F8_ICB | F8_CARRY;
    c.irq_request = 1;
    c.irq_vector = 0x0B50;                             /* int_vector is NULL: this is used */
    at(&b, &c, 0x1000, (uint8_t[]){ 0x2b }, 1);
    const int took = f8_step(&c);
    CHECK(strcmp(states(&b), "10 1C 0F 13 00") == 0, "interrupt states: %s", states(&b));
    CHECK(c.pc1 == 0x1001, "the return address is the next instruction: PC1 %04X", c.pc1);
    CHECK(f8_pc(&c) == 0x0B50 && c.pc0 == 0x0B51, "vector taken: pc %04X", f8_pc(&c));
    CHECK(!(c.w & F8_ICB) && (c.w & F8_CARRY), "ICB cleared, the rest of W kept: %02X", c.w);
    CHECK(took == 4 + 24, "NOP 4 + the entry's four long states 24 (%d)", took);
    {
        /* the vector's two bytes arrive in 0F (low) and 13 (high); the fetch is at the vector */
        const ev_t *h[5];
        int k = 0;
        for (int i = 0; i < b.n && k < 5; i++)
            if (b.log[i].kind == EV_HOOK)
                h[k++] = &b.log[i];
        CHECK(h[2]->data == 0x50 && h[2]->pc0 == 0x1050 && h[2]->pc1 == 0x1001, "ROMC 0F: dbus %02X PC0 %04X PC1 %04X", h[2]->data,
              h[2]->pc0, h[2]->pc1);
        CHECK(h[3]->data == 0x0B && h[3]->pc0 == 0x0B50, "ROMC 13: dbus %02X PC0 %04X", h[3]->data, h[3]->pc0);
        CHECK(h[4]->pc0 == 0x0B51, "fetch at the vector: PC0 %04X", h[4]->pc0);
        CHECK(h[0]->cycles == 0 && h[1]->cycles == 6 && h[2]->cycles == 12 && h[3]->cycles == 18 && h[4]->cycles == 24,
              "entry states start at 0 6 12 18, the fetch at 24: %llu %llu %llu %llu %llu", (unsigned long long)h[0]->cycles,
              (unsigned long long)h[1]->cycles, (unsigned long long)h[2]->cycles, (unsigned long long)h[3]->cycles,
              (unsigned long long)h[4]->cycles);
    }

    /* the interrupting device is asked for its vector in ROMC 0F */
    setup(&b, &c, 1);
    c.int_vector = bus_vector;
    c.w = F8_ICB;
    c.irq_request = 1;
    at(&b, &c, 0x1000, (uint8_t[]){ 0x2b }, 1);
    f8_step(&c);
    CHECK(f8_pc(&c) == 0x0A40 && c.irq_vector == 0x0A40, "int_vector callback: pc %04X", f8_pc(&c));
    {
        int calls = 0, after_1c = 0;
        for (int i = 0; i < b.n; i++) {
            if (b.log[i].kind == EV_VECTOR)
                calls++;
            if (b.log[i].kind == EV_VECTOR && i > 0 && b.log[i - 1].kind == EV_HOOK && b.log[i - 1].romc == 0x1c)
                after_1c = 1;
        }
        CHECK(calls == 1 && after_1c, "int_vector called once, right after ROMC 1C (%d)", calls);
    }

    /* no request, or ICB clear: no interrupt */
    setup(&b, &c, 1);
    c.w = F8_ICB;
    at(&b, &c, 0x1000, (uint8_t[]){ 0x2b }, 1);
    f8_step(&c);
    CHECK(strcmp(states(&b), "00") == 0, "no request, no interrupt: %s", states(&b));
    setup(&b, &c, 1);
    c.irq_request = 1;
    at(&b, &c, 0x1000, (uint8_t[]){ 0x2b }, 1);
    f8_step(&c);
    CHECK(strcmp(states(&b), "00") == 0, "ICB clear, no interrupt: %s", states(&b));

    /* the privileged instructions never take one; DI clears ICB before the test; every other opcode does */
    unsigned priv = 0, took_irq = 0;
    for (int op = 0; op < 256; op++) {
        const int privileged = op == 0x0c || op == 0x1b || op == 0x1c || op == 0x1d || op == 0x27 || op == 0x28 ||
                               op == 0x29 || (op >= 0xb4 && op <= 0xbf);
        setup(&b, &c, 1);
        c.w = F8_ICB;
        c.irq_request = 1;
        c.r[9] = F8_ICB;                               /* LR W,J keeps ICB */
        c.dc0 = 0x3000;
        at(&b, &c, 0x1000, (uint8_t[]){ (uint8_t)op, 0x04, 0x05 }, 3);
        f8_step(&c);
        const char *s = states(&b);
        const int taken = strstr(s, "0F") != NULL;
        const int expect = !privileged && op != 0x1a;
        CHECK(taken == expect, "opcode %02X: interrupt %s (states %s)", op, taken ? "taken" : "not taken", s);
        if (privileged)
            priv++;
        else if (taken)
            took_irq++;
    }
    CHECK(priv == 19, "%u privileged opcodes", priv);

    /* EI is privileged: the instruction after it runs before the request is taken */
    setup(&b, &c, 1);
    c.irq_request = 1;
    at(&b, &c, 0x1000, (uint8_t[]){ 0x1b, 0x71 }, 2);   /* EI; LIS 1 */
    f8_step(&c);
    CHECK(strcmp(states(&b), "1C 00") == 0 && (c.w & F8_ICB), "EI: no interrupt yet (%s)", states(&b));
    b.n = 0;
    f8_step(&c);
    CHECK(c.a == 1 && c.pc1 == 0x1002 && f8_pc(&c) == 0 && strstr(states(&b), "0F"), "LIS 1 runs, then the interrupt (pc %04X PC1 %04X)",
          f8_pc(&c), c.pc1);
    printf("interrupt: entry sequence, vector callback, %u privileged opcodes exempt, %u others take it\n", priv, took_irq);
}

static void test_unwired(void)
{
    static bus_t b;
    static f8_t c;

    /* no callbacks at all: the bus floats high; opcode $FF is illegal, so the CPU slides along */
    memset(&c, 0, sizeof c);
    f8_power_on(&c);
    CHECK(c.dbus == 0xff && c.illegal == 0, "unwired reset reads $FF");
    f8_step(&c);
    f8_step(&c);
    CHECK(c.illegal == 2 && c.illegal_pc == 1 && f8_pc(&c) == 2, "illegal: %u, last at %d, pc %04X", c.illegal, c.illegal_pc, f8_pc(&c));

    /* read wired, ports not: IN reads $FF, OUT is dropped, a write is dropped */
    setup(&b, &c, 0);
    c.in = NULL;
    c.out = NULL;
    c.write = NULL;
    at(&b, &c, 0x1000, (uint8_t[]){ 0x26, 0x04, 0x27, 0x04, 0x17 }, 5);
    f8_step(&c);
    CHECK(c.a == 0xff, "IN of an unwired port reads $FF: %02X", c.a);
    f8_step(&c);
    f8_step(&c);
    CHECK(c.cycles > 0, "no crash on unwired OUT/ST");
    printf("unwired: reads float to $FF, writes vanish\n");
}

int main(void)
{
    test_constants();
    test_power_on_reset();
    test_set_pc();
    test_clocks();
    test_callbacks();
    test_semantics();
    test_illegal();
    test_interrupt();
    test_unwired();
    if (g_fail) {
        printf("FAIL: %d\n", g_fail);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
