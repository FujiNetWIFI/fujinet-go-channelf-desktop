/*
 * bios_stub_test -- the app's stand-in BIOS (core/chf/stub.c) held to the contract
 * tools/stub/observe.py measured on the real one (bios_contract.h): the four entry
 * points RESET, clear screen, push K and pop K, each run from the case's pre-state in
 * this app's own F8 core and video, and its post-state compared field by field.
 * Tier 1 (what FujiNet code may depend on) must match; tier 2 (leftovers of the real
 * routine) is only counted and reported.
 *
 * The console here is the least that runs the BIOS: the stand-in at $0000-$07FF, a
 * cartridge from $0800 ($55 and a spin at $0802, the rest $FF), the four port latches
 * and the VRAM of video.c. Then the cartridge handshake CONFIG relies on: $55 at $0800
 * is entered at $0802 with the specified state, the BIOS touching nothing else; any
 * other byte is never entered and ends on an illegal opcode (what parks the app's
 * machine); and a machine with the stand-in boots the FujiNet cartridge's CONFIG.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bios_contract.h"
#include "chf_internal.h"

#define ENTRY_CLEAR 0x00d0
#define ENTRY_PUSH  0x0107
#define ENTRY_POP   0x011e
#define CART_ENTRY  0x0802
#define B           chf_bios_clr_r3_base

typedef struct {
    f8_t cpu;
    cf_video_t vid;
    uint8_t cart[0x10000 - 0x800];   /* $0800-$FFFF */
    unsigned ins, outs;              /* IN / OUT instructions executed */
    unsigned bad_ports;              /* ports other than 0, 1, 4, 5 touched */
    unsigned cart_reads[3];          /* reads of $0800, $0801, and anything from $0802 */
    unsigned data_cart_reads;        /* ... by a data read (ROMC 02) rather than a fetch */
} hm_t;

static uint8_t hm_read(void *ctx, uint16_t a, uint8_t romc)
{
    hm_t *h = ctx;

    if (a < CF_BIOS_SIZE)
        return chf_stub_rom[a];
    h->cart_reads[a < 0x802 ? a - 0x800 : 2]++;
    if (romc == 0x02)
        h->data_cart_reads++;
    return h->cart[a - 0x800];
}

static void hm_write(void *ctx, uint16_t a, uint8_t d)
{
    (void)ctx;
    (void)a;
    (void)d;
}

static uint8_t hm_in(void *ctx, uint8_t port)
{
    hm_t *h = ctx;

    h->ins++;
    if (port != 0 && port != 1 && port != 4 && port != 5) {
        h->bad_ports++;
        return 0;
    }
    return video_read(&h->vid, port, 0, 0, 0);       /* panel and controllers idle */
}

static void hm_out(void *ctx, uint8_t port, uint8_t d)
{
    hm_t *h = ctx;

    h->outs++;
    if (port != 0 && port != 1 && port != 4 && port != 5)
        h->bad_ports++;
    else
        video_write(&h->vid, port, d);
}

/* a console with the cartridge's first byte `sig` and a spin loop at $0802 */
static void hm_init(hm_t *h, uint8_t sig)
{
    memset(h, 0, sizeof *h);
    memset(h->cart, 0xff, sizeof h->cart);
    h->cart[0] = sig;
    h->cart[1] = 0x00;
    h->cart[2] = 0x90;               /* BR to itself */
    h->cart[3] = 0xff;
    video_init(&h->vid);
    h->cpu.ctx = h;
    h->cpu.read = hm_read;
    h->cpu.write = hm_write;
    h->cpu.in = hm_in;
    h->cpu.out = hm_out;
    f8_power_on(&h->cpu);
}

/* The pre-state of a case. The latches go in through the video ports (so the VRAM write
 * registers follow from them), port 0 last, and the VRAM after, since a port-0 strobe writes. */
static void hm_load(hm_t *h, const chf_bios_case *c)
{
    f8_t *cpu = &h->cpu;

    hm_init(h, 0x55);
    video_write(&h->vid, 1, c->latch[1]);
    video_write(&h->vid, 4, c->latch[2]);
    video_write(&h->vid, 5, c->latch[3]);
    video_write(&h->vid, 0, c->latch[0]);
    chf_bios_vram_fill(h->vid.vram, c->vram_kind, c->vram_arg);
    memcpy(cpu->r, c->r, 64);
    cpu->w = (uint8_t)(c->w & 0x1f);
    cpu->is = (uint8_t)(c->isar & 0x3f);
    cpu->dc0 = c->dc0;
    cpu->dc1 = c->dc1;
    cpu->pc1 = c->pc1;
    cpu->a = c->a;
    f8_set_pc(cpu, c->routine == CHF_BC_RESET ? 0x0000
              : c->routine == CHF_BC_CLRSCRN ? ENTRY_CLEAR
              : c->routine == CHF_BC_PUSHK ? ENTRY_PUSH : ENTRY_POP);
}

/* Run until the routine has returned to pc1 (a POP landing there) or, for RESET, the
 * cartridge's entry is next. Returns the instruction count, or -1 if it never got there. */
static long hm_run(hm_t *h, int reset, uint16_t pc1, long limit)
{
    for (long n = 0; n < limit; n++) {
        const uint16_t pc = f8_pc(&h->cpu);
        const int pop = hm_read(h, pc, 0) == 0x1c;

        f8_step(&h->cpu);
        if (reset ? f8_pc(&h->cpu) == CART_ENTRY : pop && f8_pc(&h->cpu) == pc1)
            return n + 1;
    }
    return -1;
}

static uint32_t vram_fnv(const cf_video_t *v)
{
    return chf_bios_vram_fnv(v->vram);
}

/* ---- the contract ---- */

static const char *k_routine[] = { "reset", "clear", "push", "pop" };
static unsigned t1_fail[4], t2_diff[4], ran[4];
static unsigned t2_field[4][8];       /* by field: a is dc0 dc1 pc1 vram w r/latch */
static const char *k_field[] = { "A", "ISAR", "DC0", "DC1", "PC1", "VRAM", "W", "scratchpad/latch" };
static int failures;

static void note(const chf_bios_case *c, int idx, const char *what, long got, long want, int tier)
{
    if (tier == 1) {
        t1_fail[c->routine]++;
        if (t1_fail[c->routine] <= 4)
            printf("FAIL: %s case %d: %s is %lx, the real BIOS leaves %lx\n", k_routine[c->routine], idx, what, got, want);
    } else {
        t2_diff[c->routine]++;
    }
}

static void check_field(const chf_bios_case *c, int idx, unsigned flag, int field, const char *what, long got, long want)
{
    if (got == want)
        return;
    if (c->t1_flags & flag)
        note(c, idx, what, got, want, 1);
    else if (c->t2_flags & flag) {
        note(c, idx, what, got, want, 2);
        t2_field[c->routine][field]++;
    }
}

static void check_case(const chf_bios_case *c, int idx)
{
    static hm_t h;
    char what[32];
    long steps;

    hm_load(&h, c);
    steps = hm_run(&h, c->routine == CHF_BC_RESET, c->pc1, 2000000);
    ran[c->routine]++;
    if (steps < 0) {
        printf("FAIL: %s case %d never returned\n", k_routine[c->routine], idx);
        t1_fail[c->routine]++;
        return;
    }
    check_field(c, idx, CHF_BC_A, 0, "A", h.cpu.a, c->e_a);
    check_field(c, idx, CHF_BC_ISAR, 1, "ISAR", h.cpu.is, c->e_isar);
    check_field(c, idx, CHF_BC_DC0, 2, "DC0", h.cpu.dc0, c->e_dc0);
    check_field(c, idx, CHF_BC_DC1, 3, "DC1", h.cpu.dc1, c->e_dc1);
    check_field(c, idx, CHF_BC_PC1, 4, "PC1", h.cpu.pc1, c->e_pc1);
    check_field(c, idx, CHF_BC_VRAM, 5, "VRAM digest", vram_fnv(&h.vid), c->e_vram_fnv);
    {
        const uint8_t got = h.cpu.w & 0x1f, want = c->e_w;

        if ((got & c->t1_w) != (want & c->t1_w))
            note(c, idx, "W", got, want, 1);
        else if ((got & c->t2_w) != (want & c->t2_w)) {
            note(c, idx, "W", got, want, 2);
            t2_field[c->routine][6]++;
        }
    }
    for (int i = 0; i < 4; i++) {
        const uint8_t got = h.vid.latch[i], want = c->e_latch[i];

        snprintf(what, sizeof what, "port latch %d", i);
        if ((got & c->t1_latch[i]) != (want & c->t1_latch[i]))
            note(c, idx, what, got, want, 1);
        else if ((got & c->t2_latch[i]) != (want & c->t2_latch[i])) {
            note(c, idx, what, got, want, 2);
            t2_field[c->routine][7]++;
        }
    }
    for (int i = 0; i < 64; i++) {
        const uint8_t got = h.cpu.r[i], want = c->e_r[i];

        if (got == want)
            continue;
        snprintf(what, sizeof what, "r%d", i);
        if ((c->t1_r >> i) & 1)
            note(c, idx, what, got, want, 1);
        else if ((c->t2_r >> i) & 1) {
            note(c, idx, what, got, want, 2);
            t2_field[c->routine][7]++;
        }
    }
    /* the routines only ever use ports 0, 1, 4 and 5 */
    if (h.bad_ports) {
        printf("FAIL: %s case %d touched another port\n", k_routine[c->routine], idx);
        t1_fail[c->routine]++;
    }
    if (c->routine == CHF_BC_RESET && (h.ins || h.outs)) {
        printf("FAIL: RESET case %d ran %u IN and %u OUT instructions\n", idx, h.ins, h.outs);
        t1_fail[c->routine]++;
    }
}

static void contract(void)
{
    for (unsigned i = 0; i < CHF_BIOS_NCASES; i++)
        check_case(&chf_bios_cases[i], (int)i);
    for (int r = 0; r < 4; r++) {
        char t2[160] = "";

        for (int f = 0; f < 8; f++)
            if (t2_field[r][f])
                snprintf(t2 + strlen(t2), sizeof t2 - strlen(t2), " %s x%u", k_field[f], t2_field[r][f]);
        printf("%s: %s: %u cases, %u tier-1 mismatches; tier 2 (reported only): %u%s\n", t1_fail[r] ? "FAIL" : "ok",
               k_routine[r], ran[r], t1_fail[r], t2_diff[r], t2[0] ? t2 : "");
        failures += (int)t1_fail[r];
    }
}

/* Clear screen over every r3, from the contract's fixed base state. $00/$40/$80/$C0 are
 * tier 1; the others depend on more of the scratchpad than the contract fixes and are
 * only counted. */
static void clear_r3_sweep(void)
{
    static hm_t h;
    static uint8_t base_vram[CF_VRAM_SIZE];
    unsigned t1_bad = 0, other_same = 0, other = 0;

    chf_bios_vram_fill(base_vram, B.vram_kind, B.vram_arg);
    for (unsigned r3 = 0; r3 < 256; r3++) {
        const int tier1 = (r3 & 0x3f) == 0;

        hm_init(&h, 0x55);
        video_write(&h.vid, 1, B.latch[1]);
        video_write(&h.vid, 4, B.latch[2]);
        video_write(&h.vid, 5, B.latch[3]);
        video_write(&h.vid, 0, B.latch[0]);
        memcpy(h.vid.vram, base_vram, sizeof base_vram);
        memcpy(h.cpu.r, B.r, 64);
        h.cpu.r[3] = (uint8_t)r3;
        h.cpu.w = (uint8_t)(B.w & 0x1f);
        h.cpu.is = (uint8_t)(B.isar & 0x3f);
        h.cpu.dc0 = B.dc0;
        h.cpu.dc1 = B.dc1;
        h.cpu.pc1 = B.pc1;
        h.cpu.a = B.a;
        f8_set_pc(&h.cpu, ENTRY_CLEAR);
        if (hm_run(&h, 0, B.pc1, 2000000) < 0) {
            printf("FAIL: clear screen with r3=$%02x never returned\n", r3);
            t1_bad++;
            continue;
        }
        if (vram_fnv(&h.vid) == chf_bios_clr_r3_fnv[r3][0])
            other_same += !tier1;
        else if (tier1) {
            printf("FAIL: clear screen with r3=$%02x leaves VRAM %08x, the real BIOS %08x\n", r3, vram_fnv(&h.vid),
                   chf_bios_clr_r3_fnv[r3][0]);
            t1_bad++;
        }
        other += !tier1;
    }
    printf("%s: clear screen over r3 = $00 $40 $80 $C0: %u mismatches; the other %u values (tier 3): %u match the real BIOS\n",
           t1_bad ? "FAIL" : "ok", t1_bad, other, other_same);
    failures += (int)t1_bad;
}

/* ---- the cartridge handshake ---- */

static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        failures++;
}

static void handshake(void)
{
    static hm_t h;
    static uint8_t vram0[CF_VRAM_SIZE];
    int all_zero;

    /* power-on, $55: the state CONFIG starts from */
    hm_init(&h, 0x55);
    for (unsigned i = 0; i < CF_VRAM_SIZE; i++)
        h.vid.vram[i] = (uint8_t)((i * 7 + (i >> 5)) & 3);
    memcpy(vram0, h.vid.vram, sizeof vram0);
    video_write(&h.vid, 1, 0x5a);
    video_write(&h.vid, 4, 0xa5);
    video_write(&h.vid, 5, 0xc3);
    check(hm_run(&h, 1, 0, 100000) > 0, "$55 at $0800: the cartridge is entered at $0802");
    all_zero = 1;
    for (int i = 0; i < 64; i++)
        if (h.cpu.r[i] != (i == 59 ? 0x28 : 0))
            all_zero = 0;
    check(all_zero, "  scratchpad all $00 except r59 = $28 (K, Q, H, J zero)");
    check((h.cpu.w & 0x1f) == 0x07, "  W = Z C S, I clear");
    check(h.cpu.a == 0x08 && h.cpu.is == 0x3b && h.cpu.dc0 == 0x0801, "  A = $08, ISAR = $3B, DC0 = $0801");
    check(h.ins == 0 && h.outs == 0, "  no IN and no OUT executed");
    check(h.vid.latch[0] == 0 && h.vid.latch[1] == 0x5a && h.vid.latch[2] == 0xa5 && h.vid.latch[3] == 0xc3,
          "  the port latches untouched");
    check(memcmp(h.vid.vram, vram0, sizeof vram0) == 0, "  the VRAM untouched");
    check(h.cart_reads[0] > 0 && h.cart_reads[1] == 0 && h.data_cart_reads == h.cart_reads[0],
          "  $0800 is read (data) and $0801 never");

    /* the I bit rides through, and a second RESET (JMP $0000 from a cartridge) does the same */
    hm_init(&h, 0x55);
    h.cpu.w = 0x10 | 0x08;
    memset(h.cpu.r, 0xa5, 64);
    f8_set_pc(&h.cpu, 0x0000);
    check(hm_run(&h, 1, 0, 100000) > 0 && (h.cpu.w & 0x1f) == 0x17, "RESET keeps W's I bit: W = I Z C S");

    /* every other byte at $0800: never entered, ends on an illegal opcode below $0800 */
    {
        unsigned entered = 0, no_illegal = 0, io = 0, bad_vram = 0;

        for (unsigned v = 0; v < 256; v++) {
            hm_init(&h, (uint8_t)v);
            if (v == 0x55)
                continue;
            for (int n = 0; n < 5000; n++) {
                if (f8_pc(&h.cpu) >= CART_ENTRY)
                    break;
                f8_step(&h.cpu);
            }
            entered += f8_pc(&h.cpu) >= CART_ENTRY;
            no_illegal += !(h.cpu.illegal && h.cpu.illegal_pc >= 0 && h.cpu.illegal_pc < CF_BIOS_SIZE);
            io += h.ins + h.outs;
            for (unsigned i = 0; i < CF_VRAM_SIZE; i++)
                bad_vram += h.vid.vram[i] != 0;
        }
        check(entered == 0, "the 255 other bytes at $0800: the cartridge is never entered");
        check(no_illegal == 0, "  and each reaches an illegal opcode below $0800 (what parks the app's machine)");
        check(io == 0 && bad_vram == 0, "  with no port traffic and the VRAM untouched");
    }
}

/* ---- a machine with the stand-in as its BIOS boots CONFIG ---- */

static void machine_config(void)
{
    static chf_machine_t m;
    chf_machine_opts_t mo;
    chf_fujinet_t *fj = chf_fujinet_create();
    char why[160];

    if (!fj || chf_fujinet_power_on(fj, NULL, 0, "", true, why, sizeof why) != 0) {
        printf("FAIL: cannot power on the FujiNet cartridge: %s\n", why);
        failures++;
        return;
    }
    memset(&mo, 0, sizeof mo);
    mo.machine = CHF_MACHINE_CHANNELF;
    mo.bios_id = -1;
    mo.cart = chf_fujinet_cart(fj);
    mo.fujinet = fj;
    chf_machine_init(&m, &mo);
    chf_machine_check_cart(&m);
    for (int f = 0; f < 200; f++)
        chf_machine_run_frame(&m);
    check(!m.parked && !m.have_bios, "the stand-in console boots the FujiNet cartridge's CONFIG and does not park");
    check(f8_pc(&m.cpu) >= 0x0800 && m.cpu.illegal == 0, "  the F8 is running the cartridge, with no illegal opcode");
    {
        unsigned lit = 0;

        for (unsigned i = 0; i < CF_VRAM_SIZE; i++)
            lit += m.vid.vram[i] != 0;
        check(lit > 100, "  and CONFIG has drawn on the screen");
    }
    chf_fujinet_destroy(fj);
}

int main(void)
{
    check(chf_stub_rom[0] != 0xff, "the stand-in is built (stub.c)");
    contract();
    clear_r3_sweep();
    handshake();
    machine_config();
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
