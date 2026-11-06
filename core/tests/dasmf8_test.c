/*
 * dasmf8_test -- the F8 disassembler against MAME's own, and against the core.
 *
 *   - dasmf8_golden.h (tools/ab/gen_dasmf8_golden.sh: MAME's f8dasm.cpp compiled
 *     standalone): all 256 opcodes' text and length at sixteen pc / operand
 *     triples, MAME's step flags mapped to F8_DASM_*, branch targets;
 *   - the disassembler against the core: exactly the opcodes it prints as data
 *     ("DC") are the ones the core counts as illegal, an instruction that falls
 *     through advances the PC by the printed length, and a jump, call or branch
 *     lands where the target says;
 *   - the one place it differs from MAME on purpose: a branch near either end of
 *     memory wraps in 16 bits (MAME prints five or eight digits).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "f8.h"
#include "dasmf8_golden.h"

static int g_fail;

#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            if (g_fail < 30) { \
                printf("FAIL: "); \
                printf(__VA_ARGS__); \
                printf("\n"); \
            } \
            g_fail++; \
        } \
    } while (0)

/* ---- a flat machine for running single instructions ---- */
typedef struct {
    uint8_t mem[0x10000];
} flat_t;

static uint8_t flat_read(void *ctx, uint16_t a, uint8_t romc)
{
    (void)romc;
    return ((flat_t *)ctx)->mem[a];
}
static void flat_write(void *ctx, uint16_t a, uint8_t d) { ((flat_t *)ctx)->mem[a] = d; }
static uint8_t flat_in(void *ctx, uint8_t p) { (void)ctx; (void)p; return 0; }
static void flat_out(void *ctx, uint8_t p, uint8_t d) { (void)ctx; (void)p; (void)d; }

static void flat_cpu(f8_t *c, flat_t *m)
{
    memset(c, 0, sizeof *c);
    c->ctx = m;
    c->read = flat_read;
    c->write = flat_write;
    c->in = flat_in;
    c->out = flat_out;
    f8_power_on(c);
}

/* the target the golden text prints (the last H'xxxx') */
static int text_target(const char *t)
{
    const char *p = strrchr(t, 'H');
    unsigned v;
    if (!p || p[1] != '\'' || strlen(p) != 7 || sscanf(p, "H'%4x'", &v) != 1)
        return -1;
    return (int)v;
}

static void test_golden(void)
{
    int checked = 0;
    for (int op = 0; op < 256; op++) {
        const int nt = dasmf8_golden[op].len > 1 ? DASMF8_NV : 1;
        const unsigned mf = dasmf8_golden[op].flags;       /* MAME: 8 supported, 4 out, 2 over, 1 cond */
        unsigned want = 0;
        if ((mf & 2) || op == 0x0c)
            want |= F8_DASM_CALL;                          /* PK is a step out in MAME, a call here */
        if ((mf & 4) && op != 0x0c)
            want |= F8_DASM_RETURN;
        if (mf & 1)
            want |= F8_DASM_BRANCH | F8_DASM_COND;
        if (op == 0x29 || op == 0x90)
            want |= F8_DASM_BRANCH;
        CHECK(mf & 8, "%02X: MAME says flags unsupported", op);
        CHECK(dasmf8_length((uint8_t)op) == dasmf8_golden[op].len, "%02X: dasmf8_length %d, MAME %d", op,
              dasmf8_length((uint8_t)op), dasmf8_golden[op].len);
        CHECK(dasmf8_op_flags((uint8_t)op) == want, "%02X: dasmf8_op_flags %x, want %x", op, dasmf8_op_flags((uint8_t)op), want);
        for (int v = 0; v < nt; v++) {
            const uint8_t bytes[3] = { (uint8_t)op, dasmf8_variants[v].b1, dasmf8_variants[v].b2 };
            char buf[64];
            unsigned fl = 99;
            int tgt = 99;
            int len = dasmf8(buf, sizeof buf, dasmf8_variants[v].pc, bytes, &fl, &tgt);
            int want_tgt = -1;
            if (op == 0x28 || op == 0x29)
                want_tgt = dasmf8_variants[v].b1 << 8 | dasmf8_variants[v].b2;
            else if ((want & F8_DASM_BRANCH))
                want_tgt = text_target(dasmf8_golden[op].text[v]);
            CHECK(len == dasmf8_golden[op].len, "%02X @%04X: length %d, MAME %d", op, dasmf8_variants[v].pc, len,
                  dasmf8_golden[op].len);
            CHECK(strcmp(buf, dasmf8_golden[op].text[v]) == 0, "%02X @%04X: \"%s\", MAME \"%s\"", op,
                  dasmf8_variants[v].pc, buf, dasmf8_golden[op].text[v]);
            CHECK(fl == want, "%02X: flags %x, want %x", op, fl, want);
            CHECK(tgt == want_tgt, "%02X @%04X: target %d, want %d", op, dasmf8_variants[v].pc, tgt, want_tgt);
            checked++;
        }
    }
    printf("golden: %d opcode/operand/pc combinations against MAME's text, length, flags, targets\n", checked);
}

static void test_core(void)
{
    static flat_t m;
    static f8_t c;
    int illegal_seen = 0, fell = 0, jumped = 0, cond = 0;

    for (int op = 0; op < 256; op++) {
        const uint8_t bytes[3] = { (uint8_t)op, 0x04, 0x5a };
        char buf[64];
        unsigned fl;
        int tgt;
        const unsigned pc = 0x1000;
        const int len = dasmf8(buf, sizeof buf, pc, bytes, &fl, &tgt);
        const int is_data = strncmp(buf, "DC ", 3) == 0;

        for (int w = 0; w < 32; w++) {
            for (int is = 0; is < 64; is += 7) {
                unsigned long landed[2] = { 0, 0 };
                (void)landed;
                memset(&m, 0x2b, sizeof m);
                m.mem[pc] = (uint8_t)op;
                m.mem[pc + 1] = bytes[1];
                m.mem[pc + 2] = bytes[2];
                flat_cpu(&c, &m);
                c.w = (uint8_t)w;
                c.is = (uint8_t)is;
                c.r[12] = 0x23;         /* K and Q: where PK and LR P0,Q go */
                c.r[13] = 0x45;
                c.r[14] = 0x23;
                c.r[15] = 0x45;
                c.pc1 = 0x2345;
                c.dc0 = 0x3000;
                f8_set_pc(&c, (uint16_t)pc);
                const unsigned before = c.illegal;
                f8_step(&c);
                CHECK(is_data == (c.illegal != before), "%02X: printed \"%s\" but the core %s", op, buf,
                      c.illegal != before ? "counts it illegal" : "executes it");
                if (c.illegal != before) {
                    illegal_seen++;
                    CHECK(f8_pc(&c) == pc + 1, "%02X: illegal opcode moved the PC to %04X", op, f8_pc(&c));
                    continue;
                }
                const unsigned after = f8_pc(&c);
                if (!(fl & (F8_DASM_CALL | F8_DASM_RETURN | F8_DASM_BRANCH)) && op != 0x0d) {
                    CHECK(after == pc + (unsigned)len, "%02X \"%s\": PC %04X, %d bytes long", op, buf, after, len);
                    fell++;
                } else if (fl & F8_DASM_COND) {
                    CHECK(after == (unsigned)tgt || after == pc + (unsigned)len, "%02X \"%s\": PC %04X is neither %04X nor %04X",
                          op, buf, after, tgt, (unsigned)(pc + len));
                    cond++;
                } else if (tgt >= 0) {
                    CHECK(after == (unsigned)tgt, "%02X \"%s\": PC %04X, target %04X", op, buf, after, tgt);
                    jumped++;
                } else if (op == 0x0c || op == 0x0d) {
                    CHECK(after == 0x2345, "%02X \"%s\": PC %04X", op, buf, after);      /* from K, from Q */
                } else if (op == 0x1c) {
                    CHECK(after == 0x2345, "%02X \"%s\": PC %04X", op, buf, after);      /* from PC1 */
                }
            }
        }
    }
    CHECK(illegal_seen > 0, "no illegal opcode ran");
    printf("core: the disassembler agrees with the core on %d fall-throughs, %d jumps, %d conditional branches, and on the illegal opcodes\n",
           fell, jumped, cond);
}

/* a branch whose target is outside $0000-$FFFF: MAME's text has five or eight digits, here it wraps */
static void test_wrap(void)
{
    static flat_t m;
    static f8_t c;
    struct { unsigned pc; uint8_t off; const char *text; int tgt; } t[] = {
        { 0x0010, 0xe0, "BR   H'FFF1'", 0xfff1 },
        { 0xfff0, 0x7f, "BR   H'0070'", 0x0070 },
        { 0x0000, 0xff, "BR   H'0000'", 0x0000 },
        { 0xffff, 0x00, "BR   H'0000'", 0x0000 },
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        const uint8_t bytes[3] = { 0x90, t[i].off, 0 };
        char buf[64];
        int tgt;
        dasmf8(buf, sizeof buf, (uint16_t)t[i].pc, bytes, NULL, &tgt);
        CHECK(strcmp(buf, t[i].text) == 0, "BR at %04X: \"%s\", want \"%s\"", t[i].pc, buf, t[i].text);
        CHECK(tgt == t[i].tgt, "BR at %04X: target %04X, want %04X", t[i].pc, tgt, t[i].tgt);
        /* and the core really goes there */
        memset(&m, 0x2b, sizeof m);
        m.mem[t[i].pc] = 0x90;
        m.mem[(t[i].pc + 1) & 0xffff] = t[i].off;
        flat_cpu(&c, &m);
        f8_set_pc(&c, (uint16_t)t[i].pc);
        f8_step(&c);
        CHECK(f8_pc(&c) == (unsigned)t[i].tgt, "BR at %04X: the core went to %04X", t[i].pc, f8_pc(&c));
    }
    printf("wrap: branches near the ends of memory wrap in 16 bits, as the core does\n");
}

static void test_api(void)
{
    const uint8_t bytes[3] = { 0x29, 0x08, 0x02 };
    char small[8];
    char buf[64];

    CHECK(dasmf8(buf, sizeof buf, 0x0100, bytes, NULL, NULL) == 3, "JMP is three bytes long");
    CHECK(strcmp(buf, "JMP  H'0802'") == 0, "JMP text \"%s\"", buf);
    CHECK(dasmf8(small, sizeof small, 0x0100, bytes, NULL, NULL) == 3, "a short buffer does not change the length");
    CHECK(strcmp(small, "JMP  H'") == 0, "a short buffer is cut and terminated: \"%s\"", small);
    CHECK(dasmf8(NULL, 0, 0x0100, bytes, NULL, NULL) == 3, "no buffer only measures");
    printf("api: NULL outputs, truncation\n");
}

int main(void)
{
    test_golden();
    test_core();
    test_wrap();
    test_api();
    if (g_fail) {
        printf("FAIL: %d\n", g_fail);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
