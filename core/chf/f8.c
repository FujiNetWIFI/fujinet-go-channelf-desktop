/* f8.c -- the Fairchild F8 (F3850), see f8.h.
 *
 * Transposed from MAME's src/devices/cpu/f8/f8.cpp (BSD-3-Clause, copyright-
 * holders Juergen Buchmueller -- see COMPLIANCE.md). The structure is MAME's:
 * one romc_XX() per ROMC state (the access, then 4 or 6 clocks), one f8_xxx()
 * per opcode named as MAME names it, the same 256-way dispatch, then the
 * privileged-op test, the interrupt freeze cycle and the trailing fetch of the
 * next opcode. Only the C++ plumbing changed: m_x is c->x, the address spaces
 * are the callbacks, the debugger and the interrupt-acknowledge hook are gone,
 * and the ROMC trace MAME's graft prints is romc_hook.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <string.h>

#include "f8.h"

/* cycle (short/long) */
enum { CS = 4, CL = 6 };

/* the bus: a missing callback is an undriven bus (reads $FF, writes vanish) */
static inline uint8_t mem_read(f8_t *c, uint16_t addr, uint8_t romc)
{
    return c->read ? c->read(c->ctx, addr, romc) : 0xff;
}
static inline void mem_write(f8_t *c, uint16_t addr, uint8_t data)
{
    if (c->write)
        c->write(c->ctx, addr, data);
}
static inline uint8_t io_read(f8_t *c, uint8_t port)
{
    return c->in ? c->in(c->ctx, port) : 0xff;
}
static inline void io_write(f8_t *c, uint8_t port, uint8_t data)
{
    if (c->out)
        c->out(c->ctx, port, data);
}

/* MAME takes the cycles with m_icount after the access, so every callback and
 * the hook see `cycles` as it was at the start of the bus cycle */
static inline void burn(f8_t *c, int n)
{
    c->cycles += (unsigned)n;
}

#define TRACE(c, romc) \
    do { \
        if ((c)->romc_hook) \
            (c)->romc_hook((c)->ctx, (c), (romc), (c)->dbus); \
    } while (0)

/* clear all flags */
static inline void clr_ozcs(f8_t *c)
{
    c->w &= (uint8_t)~(F8_OVR | F8_ZERO | F8_CARRY | F8_SIGN);
}

/* set sign and zero flags (note: the S flag is complementary) */
static inline void set_sz(f8_t *c, uint8_t n)
{
    if (n == 0)
        c->w |= F8_ZERO;
    if (~n & 0x80)
        c->w |= F8_SIGN;
}

/* set overflow and carry flags */
static inline uint8_t do_add(f8_t *c, uint8_t n, uint8_t m, uint8_t carry)
{
    uint16_t r = (uint16_t)(n + m + carry);
    if (r & 0x100)
        c->w |= F8_CARRY;
    if ((n ^ r) & (m ^ r) & 0x80)
        c->w |= F8_OVR;
    return (uint8_t)(r & 0xff);
}

/* decimal add helper. From the F8 Guide To Programming's description of AMD:
 * the binary sum of the augend and $66 is formed before AMD is called; the
 * addend is added to it and the carry and intermediate carry recorded; a
 * factor depending on them is added (none/none $AA, none/ic $A0, c/none $0A,
 * both $00), any carry from the low digit being suppressed; the status flags
 * are those of the binary add, before the factor. */
static uint8_t do_add_decimal(f8_t *c, uint8_t augend, uint8_t addend)
{
    uint8_t tmp = (uint8_t)(augend + addend);

    uint8_t hc = 0; /* high order carry */
    uint8_t ic = 0; /* low order carry */

    if (((augend + addend) & 0xff0) > 0xf0)
        hc = 1;
    if ((augend & 0x0f) + (addend & 0x0f) > 0x0F)
        ic = 1;

    clr_ozcs(c);
    do_add(c, augend, addend, 0);
    set_sz(c, tmp);

    if (hc == 0 && ic == 0)
        tmp = (uint8_t)(((tmp + 0xa0) & 0xf0) + ((tmp + 0x0a) & 0x0f));
    if (hc == 0 && ic == 1)
        tmp = (uint8_t)(((tmp + 0xa0) & 0xf0) + (tmp & 0x0f));
    if (hc == 1 && ic == 0)
        tmp = (uint8_t)((tmp & 0xf0) + ((tmp + 0x0a) & 0x0f));

    return tmp;
}

/* ISAR++ / ISAR--: only the low three bits (the column) move, the row stays */
static inline void is_inc(f8_t *c)
{
    c->is = (uint8_t)((c->is & 0x38) | ((c->is + 1) & 0x07));
}
static inline void is_dec(f8_t *c)
{
    c->is = (uint8_t)((c->is & 0x38) | ((c->is - 1) & 0x07));
}

/* ---- ROMC states (the F8 has no address bus: five ROMC lines name what
 * every memory device does this cycle; there is one PC0/PC1/DC0/DC1 here) ---- */

/* 00: instruction fetch: dbus = (PC0), PC0++ */
static void romc_00(f8_t *c, int insttim)
{
    c->dbus = mem_read(c, c->pc0, 0x00);
    c->pc0++;
    TRACE(c, 0x00);
    burn(c, insttim);
}

/* 01: dbus = (PC0), PC0 += (int8_t)dbus (the relative branch) */
static void romc_01(f8_t *c)
{
    c->dbus = mem_read(c, c->pc0, 0x01);
    c->pc0 = (uint16_t)(c->pc0 + (int8_t)c->dbus);
    TRACE(c, 0x01);
    burn(c, CL);
}

/* 02: dbus = (DC0), DC0++ */
static void romc_02(f8_t *c)
{
    c->dbus = mem_read(c, c->dc0, 0x02);
    c->dc0++;
    TRACE(c, 0x02);
    burn(c, CL);
}

/* 03: operand fetch: as 00, and the byte is also the I/O address latch */
static void romc_03(f8_t *c, int insttim)
{
    c->dbus = c->io = mem_read(c, c->pc0, 0x03);
    c->pc0++;
    TRACE(c, 0x03);
    burn(c, insttim);
}

/* 04: PC0 = PC1 */
static void romc_04(f8_t *c)
{
    c->pc0 = c->pc1;
    TRACE(c, 0x04);
    burn(c, CS);
}

/* 05: (DC0) = dbus, DC0++ */
static void romc_05(f8_t *c)
{
    mem_write(c, c->dc0, c->dbus);
    c->dc0++;
    TRACE(c, 0x05);
    burn(c, CL);
}

/* 06: dbus = DC0 high */
static void romc_06(f8_t *c)
{
    c->dbus = c->dc0 >> 8;
    TRACE(c, 0x06);
    burn(c, CL);
}

/* 07: dbus = PC1 high */
static void romc_07(f8_t *c)
{
    c->dbus = c->pc1 >> 8;
    TRACE(c, 0x07);
    burn(c, CL);
}

/* 08: PC1 = PC0, dbus = 0, PC0 = 0 (this system's ROMC 08 vector, both bytes) */
static void romc_08(f8_t *c)
{
    c->pc1 = c->pc0;
    c->dbus = 0;
    c->pc0 = 0;
    TRACE(c, 0x08);
    burn(c, CL);
}

/* 09: dbus = DC0 low */
static void romc_09(f8_t *c)
{
    c->dbus = c->dc0 & 0xff;
    TRACE(c, 0x09);
    burn(c, CL);
}

/* 0A: DC0 += (int8_t)dbus */
static void romc_0a(f8_t *c)
{
    c->dc0 = (uint16_t)(c->dc0 + (int8_t)c->dbus);
    TRACE(c, 0x0A);
    burn(c, CL);
}

/* 0B: dbus = PC1 low */
static void romc_0b(f8_t *c)
{
    c->dbus = c->pc1 & 0xff;
    TRACE(c, 0x0B);
    burn(c, CL);
}

/* 0C: dbus = (PC0), PC0 low = dbus */
static void romc_0c(f8_t *c)
{
    c->dbus = mem_read(c, c->pc0, 0x0C);
    c->pc0 = (c->pc0 & 0xff00) | c->dbus;
    TRACE(c, 0x0C);
    burn(c, CL);
}

/* 0D: PC1 = PC0 + 1 */
static void romc_0d(f8_t *c)
{
    c->pc1 = c->pc0 + 1;
    TRACE(c, 0x0D);
    burn(c, CS);
}

/* 0E: dbus = (PC0), DC0 low = dbus */
static void romc_0e(f8_t *c)
{
    c->dbus = mem_read(c, c->pc0, 0x0E);
    c->dc0 = (c->dc0 & 0xff00) | c->dbus;
    TRACE(c, 0x0E);
    burn(c, CL);
}

/* 0F: interrupt vector low: PC1 = PC0, PC0 low = vector low */
static void romc_0f(f8_t *c)
{
    if (c->int_vector)
        c->irq_vector = c->int_vector(c->ctx);
    c->dbus = c->irq_vector & 0x00ff;
    c->pc1 = c->pc0;
    c->pc0 = (c->pc0 & 0xff00) | c->dbus;
    TRACE(c, 0x0F);
    burn(c, CL);
}

/* 10: inhibit changes to the interrupt priority logic (no state) */
static void romc_10(f8_t *c)
{
    TRACE(c, 0x10);
    burn(c, CL);
}

/* 11: dbus = (PC0), DC0 high = dbus */
static void romc_11(f8_t *c)
{
    c->dbus = mem_read(c, c->pc0, 0x11);
    c->dc0 = (uint16_t)((c->dc0 & 0x00ff) | (c->dbus << 8));
    TRACE(c, 0x11);
    burn(c, CL);
}

/* 12: PC1 = PC0, PC0 low = dbus */
static void romc_12(f8_t *c)
{
    c->pc1 = c->pc0;
    c->pc0 = (c->pc0 & 0xff00) | c->dbus;
    TRACE(c, 0x12);
    burn(c, CL);
}

/* 13: interrupt vector high: PC0 high = vector high, ICB cleared */
static void romc_13(f8_t *c)
{
    c->dbus = c->irq_vector >> 8;
    c->pc0 = (uint16_t)((c->pc0 & 0x00ff) | (c->dbus << 8));
    c->w &= (uint8_t)~F8_ICB;
    TRACE(c, 0x13);
    burn(c, CL);
}

/* 14: PC0 high = dbus */
static void romc_14(f8_t *c)
{
    c->pc0 = (uint16_t)((c->pc0 & 0x00ff) | (c->dbus << 8));
    TRACE(c, 0x14);
    burn(c, CL);
}

/* 15: PC1 high = dbus */
static void romc_15(f8_t *c)
{
    c->pc1 = (uint16_t)((c->pc1 & 0x00ff) | (c->dbus << 8));
    TRACE(c, 0x15);
    burn(c, CL);
}

/* 16: DC0 high = dbus */
static void romc_16(f8_t *c)
{
    c->dc0 = (uint16_t)((c->dc0 & 0x00ff) | (c->dbus << 8));
    TRACE(c, 0x16);
    burn(c, CL);
}

/* 17: PC0 low = dbus */
static void romc_17(f8_t *c)
{
    c->pc0 = (c->pc0 & 0xff00) | c->dbus;
    TRACE(c, 0x17);
    burn(c, CL);
}

/* 18: PC1 low = dbus */
static void romc_18(f8_t *c)
{
    c->pc1 = (c->pc1 & 0xff00) | c->dbus;
    TRACE(c, 0x18);
    burn(c, CL);
}

/* 19: DC0 low = dbus */
static void romc_19(f8_t *c)
{
    c->dc0 = (c->dc0 & 0xff00) | c->dbus;
    TRACE(c, 0x19);
    burn(c, CL);
}

/* 1A: I/O write: port (io) = dbus */
static void romc_1a(f8_t *c)
{
    io_write(c, (uint8_t)c->io, c->dbus);
    TRACE(c, 0x1A);
    burn(c, CL);
}

/* 1B: I/O read: dbus = port (io) */
static void romc_1b(f8_t *c)
{
    c->dbus = io_read(c, (uint8_t)c->io);
    TRACE(c, 0x1B);
    burn(c, CL);
}

/* 1C: no bus activity */
static void romc_1c(f8_t *c, int insttim)
{
    TRACE(c, 0x1C);
    burn(c, insttim);
}

/* 1D: swap DC0 and DC1 */
static void romc_1d(f8_t *c)
{
    uint16_t tmp = c->dc0;
    c->dc0 = c->dc1;
    c->dc1 = tmp;
    TRACE(c, 0x1D);
    burn(c, CS);
}

/* ROMC 1E and 1F (PC0 low / high onto the bus) are in MAME but no instruction
 * emits them, and nothing here does either. */

/* ---- opcodes ---- */

static void illegal(f8_t *c)
{
    c->illegal++;
    c->illegal_pc = (uint16_t)(c->pc0 - 1);
}

/* 0000 0000  LR A,KU */
static void f8_lr_a_ku(f8_t *c)
{
    c->a = c->r[12];
}

/* 0000 0001  LR A,KL */
static void f8_lr_a_kl(f8_t *c)
{
    c->a = c->r[13];
}

/* 0000 0010  LR A,QU */
static void f8_lr_a_qu(f8_t *c)
{
    c->a = c->r[14];
}

/* 0000 0011  LR A,QL */
static void f8_lr_a_ql(f8_t *c)
{
    c->a = c->r[15];
}

/* 0000 0100  LR KU,A */
static void f8_lr_ku_a(f8_t *c)
{
    c->r[12] = c->a;
}

/* 0000 0101  LR KL,A */
static void f8_lr_kl_a(f8_t *c)
{
    c->r[13] = c->a;
}

/* 0000 0110  LR QU,A */
static void f8_lr_qu_a(f8_t *c)
{
    c->r[14] = c->a;
}

/* 0000 0111  LR QL,A */
static void f8_lr_ql_a(f8_t *c)
{
    c->r[15] = c->a;
}

/* 0000 1000  LR K,P */
static void f8_lr_k_p(f8_t *c)
{
    romc_07(c);
    c->r[12] = c->dbus;
    romc_0b(c);
    c->r[13] = c->dbus;
}

/* 0000 1001  LR P,K */
static void f8_lr_p_k(f8_t *c)
{
    c->dbus = c->r[12];
    romc_15(c);
    c->dbus = c->r[13];
    romc_18(c);
}

/* 0000 1010  LR A,IS */
static void f8_lr_a_is(f8_t *c)
{
    c->a = c->is;
}

/* 0000 1011  LR IS,A */
static void f8_lr_is_a(f8_t *c)
{
    c->is = (uint8_t)(c->a & 0x3f);
}

/* 0000 1100  PK */
static void f8_pk(f8_t *c)
{
    c->dbus = c->r[13];
    romc_12(c);
    c->dbus = c->r[12];
    romc_14(c);
}

/* 0000 1101  LR P0,Q */
static void f8_lr_p0_q(f8_t *c)
{
    c->dbus = c->r[15];
    romc_17(c);
    c->dbus = c->r[14];
    romc_14(c);
}

/* 0000 1110  LR Q,DC */
static void f8_lr_q_dc(f8_t *c)
{
    romc_06(c);
    c->r[14] = c->dbus;
    romc_09(c);
    c->r[15] = c->dbus;
}

/* 0000 1111  LR DC,Q */
static void f8_lr_dc_q(f8_t *c)
{
    c->dbus = c->r[14];
    romc_16(c);
    c->dbus = c->r[15];
    romc_19(c);
}

/* 0001 0000  LR DC,H */
static void f8_lr_dc_h(f8_t *c)
{
    c->dbus = c->r[10];
    romc_16(c);
    c->dbus = c->r[11];
    romc_19(c);
}

/* 0001 0001  LR H,DC */
static void f8_lr_h_dc(f8_t *c)
{
    romc_06(c);
    c->r[10] = c->dbus;
    romc_09(c);
    c->r[11] = c->dbus;
}

/* 0001 0010  SR 1  (O Z C S: 0 x 0 1) */
static void f8_sr_1(f8_t *c)
{
    c->a >>= 1;
    clr_ozcs(c);
    set_sz(c, c->a);
}

/* 0001 0011  SL 1  (O Z C S: 0 x 0 x) */
static void f8_sl_1(f8_t *c)
{
    c->a = (uint8_t)(c->a << 1);
    clr_ozcs(c);
    set_sz(c, c->a);
}

/* 0001 0100  SR 4  (O Z C S: 0 x 0 1) */
static void f8_sr_4(f8_t *c)
{
    c->a >>= 4;
    clr_ozcs(c);
    set_sz(c, c->a);
}

/* 0001 0101  SL 4  (O Z C S: 0 x 0 x) */
static void f8_sl_4(f8_t *c)
{
    c->a = (uint8_t)(c->a << 4);
    clr_ozcs(c);
    set_sz(c, c->a);
}

/* 0001 0110  LM */
static void f8_lm(f8_t *c)
{
    romc_02(c);
    c->a = c->dbus;
}

/* 0001 0111  ST */
static void f8_st(f8_t *c)
{
    c->dbus = c->a;
    romc_05(c);
}

/* 0001 1000  COM  (O Z C S: 0 x 0 x) */
static void f8_com(f8_t *c)
{
    c->a = (uint8_t)~c->a;
    clr_ozcs(c);
    set_sz(c, c->a);
}

/* 0001 1001  LNK  (O Z C S: x x x x) */
static void f8_lnk(f8_t *c)
{
    int carry = (c->w & F8_CARRY) != 0;
    clr_ozcs(c);
    if (carry)
        c->a = do_add(c, c->a, 1, 0);
    set_sz(c, c->a);
}

/* 0001 1010  DI */
static void f8_di(f8_t *c)
{
    romc_1c(c, CS);
    c->w &= (uint8_t)~F8_ICB;
}

/* 0001 1011  EI */
static void f8_ei(f8_t *c)
{
    romc_1c(c, CS);
    c->w |= F8_ICB;
}

/* 0001 1100  POP */
static void f8_pop(f8_t *c)
{
    romc_04(c);
}

/* 0001 1101  LR W,J  (O Z C S: x x x x) */
static void f8_lr_w_j(f8_t *c)
{
    romc_1c(c, CS);
    c->w = (uint8_t)(c->r[9] & 0x1f);
}

/* 0001 1110  LR J,W */
static void f8_lr_j_w(f8_t *c)
{
    c->r[9] = c->w;
}

/* 0001 1111  INC  (O Z C S: x x x x) */
static void f8_inc(f8_t *c)
{
    clr_ozcs(c);
    c->a = do_add(c, c->a, 1, 0);
    set_sz(c, c->a);
}

/* 0010 0000   aaaa aaaa  LI aa */
static void f8_li(f8_t *c)
{
    romc_03(c, CL);
    c->a = c->dbus;
}

/* 0010 0001   aaaa aaaa  NI aa  (O Z C S: 0 x 0 x) */
static void f8_ni(f8_t *c)
{
    romc_03(c, CL);
    clr_ozcs(c);
    c->a &= c->dbus;
    set_sz(c, c->a);
}

/* 0010 0010   aaaa aaaa  OI aa  (O Z C S: 0 x 0 x) */
static void f8_oi(f8_t *c)
{
    romc_03(c, CL);
    clr_ozcs(c);
    c->a |= c->dbus;
    set_sz(c, c->a);
}

/* 0010 0011   aaaa aaaa  XI aa  (O Z C S: 0 x 0 x) */
static void f8_xi(f8_t *c)
{
    romc_03(c, CL);
    clr_ozcs(c);
    c->a ^= c->dbus;
    set_sz(c, c->a);
}

/* 0010 0100   aaaa aaaa  AI aa  (O Z C S: x x x x) */
static void f8_ai(f8_t *c)
{
    romc_03(c, CL);
    clr_ozcs(c);
    c->a = do_add(c, c->a, c->dbus, 0);
    set_sz(c, c->a);
}

/* 0010 0101   aaaa aaaa  CI aa  (O Z C S: x x x x) */
static void f8_ci(f8_t *c)
{
    romc_03(c, CL);
    clr_ozcs(c);
    set_sz(c, do_add(c, (uint8_t)~c->a, c->dbus, 1));
}

/* 0010 0110   aaaa aaaa  IN aa  (O Z C S: 0 x 0 x) */
static void f8_in(f8_t *c)
{
    romc_03(c, CL);
    clr_ozcs(c);
    romc_1b(c);
    c->a = c->dbus;
    set_sz(c, c->a);
}

/* 0010 0111   aaaa aaaa  OUT aa */
static void f8_out(f8_t *c)
{
    romc_03(c, CL);
    c->dbus = c->a;
    romc_1a(c);
}

/* 0010 1000   iiii iiii   jjjj jjjj  PI iijj */
static void f8_pi(f8_t *c)
{
    romc_03(c, CL);
    c->a = c->dbus;
    romc_0d(c);
    romc_0c(c);
    c->dbus = c->a;
    romc_14(c);
}

/* 0010 1001   iiii iiii   jjjj jjjj  JMP iijj */
static void f8_jmp(f8_t *c)
{
    romc_03(c, CL);
    c->a = c->dbus;
    romc_0c(c);
    c->dbus = c->a;
    romc_14(c);
}

/* 0010 1010   iiii iiii   jjjj jjjj  DCI iijj */
static void f8_dci(f8_t *c)
{
    romc_11(c);
    romc_03(c, CS);
    romc_0e(c);
    romc_03(c, CS);
}

/* 0010 1011  NOP */
static void f8_nop(f8_t *c)
{
    (void)c;
}

/* 0010 1100  XDC */
static void f8_xdc(f8_t *c)
{
    romc_1d(c);
}

/* 0011 rrrr  DS r  (O Z C S: x x x x) */
static void f8_ds_r(f8_t *c, int r)
{
    clr_ozcs(c);
    uint8_t d = do_add(c, c->r[r], 0xff, 0);
    c->r[r] = d;
    set_sz(c, d);
}

/* 0011 1100  DS ISAR  (O Z C S: x x x x) */
static void f8_ds_isar(f8_t *c)
{
    clr_ozcs(c);
    uint8_t d = do_add(c, c->r[c->is & 63], 0xff, 0);
    c->r[c->is & 63] = d;
    set_sz(c, d);
}

/* 0011 1101  DS ISAR++  (O Z C S: x x x x) */
static void f8_ds_isar_i(f8_t *c)
{
    clr_ozcs(c);
    uint8_t d = do_add(c, c->r[c->is & 63], 0xff, 0);
    c->r[c->is & 63] = d;
    set_sz(c, d);
    is_inc(c);
}

/* 0011 1110  DS ISAR--  (O Z C S: x x x x) */
static void f8_ds_isar_d(f8_t *c)
{
    clr_ozcs(c);
    uint8_t d = do_add(c, c->r[c->is & 63], 0xff, 0);
    c->r[c->is & 63] = d;
    set_sz(c, d);
    is_dec(c);
}

/* 0100 rrrr  LR A,r */
static void f8_lr_a_r(f8_t *c, int r)
{
    c->a = c->r[r];
}

/* 0100 1100  LR A,ISAR */
static void f8_lr_a_isar(f8_t *c)
{
    c->a = c->r[c->is & 63];
}

/* 0100 1101  LR A,ISAR++ */
static void f8_lr_a_isar_i(f8_t *c)
{
    c->a = c->r[c->is & 63];
    is_inc(c);
}

/* 0100 1110  LR A,ISAR-- */
static void f8_lr_a_isar_d(f8_t *c)
{
    c->a = c->r[c->is & 63];
    is_dec(c);
}

/* 0101 rrrr  LR r,A */
static void f8_lr_r_a(f8_t *c, int r)
{
    c->r[r] = c->a;
}

/* 0101 1100  LR ISAR,A */
static void f8_lr_isar_a(f8_t *c)
{
    c->r[c->is & 63] = c->a;
}

/* 0101 1101  LR ISAR++,A */
static void f8_lr_isar_i_a(f8_t *c)
{
    c->r[c->is & 63] = c->a;
    is_inc(c);
}

/* 0101 1110  LR ISAR--,A */
static void f8_lr_isar_d_a(f8_t *c)
{
    c->r[c->is & 63] = c->a;
    is_dec(c);
}

/* 0110 0eee  LISU e */
static void f8_lisu(f8_t *c, int e)
{
    c->is = (uint8_t)((c->is & 0x07) | e);
}

/* 0110 1eee  LISL e */
static void f8_lisl(f8_t *c, int e)
{
    c->is = (uint8_t)((c->is & 0x38) | e);
}

/* 0111 iiii  LIS i */
static void f8_lis(f8_t *c, int i)
{
    c->a = (uint8_t)i;
}

/* 1000 0eee   aaaa aaaa  BT e,aa */
static void f8_bt(f8_t *c, int e)
{
    romc_1c(c, CS);
    if (c->w & e)
        romc_01(c); /* take the relative branch */
    else
        romc_03(c, CS); /* just read the argument on the data bus */
}

/* 1000 1000  AM  (O Z C S: x x x x) */
static void f8_am(f8_t *c)
{
    romc_02(c);
    clr_ozcs(c);
    c->a = do_add(c, c->a, c->dbus, 0);
    set_sz(c, c->a);
}

/* 1000 1001  AMD  (O Z C S: x x x x) */
static void f8_amd(f8_t *c)
{
    romc_02(c);
    c->a = do_add_decimal(c, c->a, c->dbus);
}

/* 1000 1010  NM  (O Z C S: 0 x 0 x) */
static void f8_nm(f8_t *c)
{
    romc_02(c);
    clr_ozcs(c);
    c->a &= c->dbus;
    set_sz(c, c->a);
}

/* 1000 1011  OM  (O Z C S: 0 x 0 x) */
static void f8_om(f8_t *c)
{
    romc_02(c);
    clr_ozcs(c);
    c->a |= c->dbus;
    set_sz(c, c->a);
}

/* 1000 1100  XM  (O Z C S: 0 x 0 x) */
static void f8_xm(f8_t *c)
{
    romc_02(c);
    clr_ozcs(c);
    c->a ^= c->dbus;
    set_sz(c, c->a);
}

/* 1000 1101  CM  (O Z C S: x x x x) */
static void f8_cm(f8_t *c)
{
    romc_02(c);
    clr_ozcs(c);
    set_sz(c, do_add(c, (uint8_t)~c->a, c->dbus, 1));
}

/* 1000 1110  ADC */
static void f8_adc(f8_t *c)
{
    c->dbus = c->a;
    romc_0a(c); /* add data bus value to DC0 */
}

/* 1000 1111  BR7 */
static void f8_br7(f8_t *c)
{
    if ((c->is & 7) == 7)
        romc_03(c, CS); /* just read the argument on the data bus */
    else
        romc_01(c); /* take the relative branch */
}

/* 1001 tttt   aaaa aaaa  BF t,aa */
static void f8_bf(f8_t *c, int t)
{
    romc_1c(c, CS);
    if (c->w & t)
        romc_03(c, CS); /* just read the argument on the data bus */
    else
        romc_01(c); /* take the relative branch */
}

/* 1010 000n  INS n (n = 0-1)  (O Z C S: 0 x 0 x) */
static void f8_ins_0(f8_t *c, int n)
{
    romc_1c(c, CS);
    clr_ozcs(c);
    c->a = io_read(c, (uint8_t)n);
    set_sz(c, c->a);
}

/* 1010 nnnn  INS n (n = 4-F)  (O Z C S: 0 x 0 x) */
static void f8_ins_1(f8_t *c, int n)
{
    romc_1c(c, CL);
    c->io = (uint16_t)n;
    romc_1b(c);
    clr_ozcs(c);
    c->a = c->dbus;
    set_sz(c, c->a);
}

/* 1011 000n  OUTS n (n = 0-1) */
static void f8_outs_0(f8_t *c, int n)
{
    romc_1c(c, CS);
    io_write(c, (uint8_t)n, c->a);
}

/* 1011 nnnn  OUTS n (n = 4-F) */
static void f8_outs_1(f8_t *c, int n)
{
    romc_1c(c, CL);
    c->io = (uint16_t)n;
    c->dbus = c->a;
    romc_1a(c);
}

/* 1100 rrrr  AS r  (O Z C S: x x x x) */
static void f8_as(f8_t *c, int r)
{
    clr_ozcs(c);
    c->a = do_add(c, c->a, c->r[r], 0);
    set_sz(c, c->a);
}

/* 1100 1100  AS ISAR  (O Z C S: x x x x) */
static void f8_as_isar(f8_t *c)
{
    clr_ozcs(c);
    c->a = do_add(c, c->a, c->r[c->is & 63], 0);
    set_sz(c, c->a);
}

/* 1100 1101  AS ISAR++  (O Z C S: x x x x) */
static void f8_as_isar_i(f8_t *c)
{
    clr_ozcs(c);
    c->a = do_add(c, c->a, c->r[c->is & 63], 0);
    set_sz(c, c->a);
    is_inc(c);
}

/* 1100 1110  AS ISAR--  (O Z C S: x x x x) */
static void f8_as_isar_d(f8_t *c)
{
    clr_ozcs(c);
    c->a = do_add(c, c->a, c->r[c->is & 63], 0);
    set_sz(c, c->a);
    is_dec(c);
}

/* 1101 rrrr  ASD r  (O Z C S: x x x x) */
static void f8_asd(f8_t *c, int r)
{
    romc_1c(c, CS);
    c->a = do_add_decimal(c, c->a, c->r[r]);
}

/* 1101 1100  ASD ISAR  (O Z C S: x x x x) */
static void f8_asd_isar(f8_t *c)
{
    romc_1c(c, CS);
    c->a = do_add_decimal(c, c->a, c->r[c->is & 63]);
}

/* 1101 1101  ASD ISAR++  (O Z C S: x x x x) */
static void f8_asd_isar_i(f8_t *c)
{
    romc_1c(c, CS);
    c->a = do_add_decimal(c, c->a, c->r[c->is & 63]);
    is_inc(c);
}

/* 1101 1110  ASD ISAR--  (O Z C S: x x x x) */
static void f8_asd_isar_d(f8_t *c)
{
    romc_1c(c, CS);
    c->a = do_add_decimal(c, c->a, c->r[c->is & 63]);
    is_dec(c);
}

/* 1110 rrrr  XS r  (O Z C S: 0 x 0 x) */
static void f8_xs(f8_t *c, int r)
{
    clr_ozcs(c);
    c->a ^= c->r[r];
    set_sz(c, c->a);
}

/* 1110 1100  XS ISAR  (O Z C S: 0 x 0 x) */
static void f8_xs_isar(f8_t *c)
{
    clr_ozcs(c);
    c->a ^= c->r[c->is & 63];
    set_sz(c, c->a);
}

/* 1110 1101  XS ISAR++  (O Z C S: 0 x 0 x) */
static void f8_xs_isar_i(f8_t *c)
{
    clr_ozcs(c);
    c->a ^= c->r[c->is & 63];
    set_sz(c, c->a);
    is_inc(c);
}

/* 1110 1110  XS ISAR--  (O Z C S: 0 x 0 x) */
static void f8_xs_isar_d(f8_t *c)
{
    clr_ozcs(c);
    c->a ^= c->r[c->is & 63];
    set_sz(c, c->a);
    is_dec(c);
}

/* 1111 rrrr  NS r  (O Z C S: 0 x 0 x) */
static void f8_ns(f8_t *c, int r)
{
    clr_ozcs(c);
    c->a &= c->r[r];
    set_sz(c, c->a);
}

/* 1111 1100  NS ISAR  (O Z C S: 0 x 0 x) */
static void f8_ns_isar(f8_t *c)
{
    clr_ozcs(c);
    c->a &= c->r[c->is & 63];
    set_sz(c, c->a);
}

/* 1111 1101  NS ISAR++  (O Z C S: 0 x 0 x) */
static void f8_ns_isar_i(f8_t *c)
{
    clr_ozcs(c);
    c->a &= c->r[c->is & 63];
    set_sz(c, c->a);
    is_inc(c);
}

/* 1111 1110  NS ISAR--  (O Z C S: 0 x 0 x) */
static void f8_ns_isar_d(f8_t *c)
{
    clr_ozcs(c);
    c->a &= c->r[c->is & 63];
    set_sz(c, c->a);
    is_dec(c);
}


/* ---- execution ---- */

int f8_step(f8_t *c)
{
    const uint64_t start = c->cycles;
    const uint8_t op = c->dbus;

    c->instructions++;

    switch (op) {
    case 0x00: f8_lr_a_ku(c); break;
    case 0x01: f8_lr_a_kl(c); break;
    case 0x02: f8_lr_a_qu(c); break;
    case 0x03: f8_lr_a_ql(c); break;
    case 0x04: f8_lr_ku_a(c); break;
    case 0x05: f8_lr_kl_a(c); break;
    case 0x06: f8_lr_qu_a(c); break;
    case 0x07: f8_lr_ql_a(c); break;
    case 0x08: f8_lr_k_p(c); break;
    case 0x09: f8_lr_p_k(c); break;
    case 0x0a: f8_lr_a_is(c); break;
    case 0x0b: f8_lr_is_a(c); break;
    case 0x0c: f8_pk(c); break;
    case 0x0d: f8_lr_p0_q(c); break;
    case 0x0e: f8_lr_q_dc(c); break;
    case 0x0f: f8_lr_dc_q(c); break;

    case 0x10: f8_lr_dc_h(c); break;
    case 0x11: f8_lr_h_dc(c); break;
    case 0x12: f8_sr_1(c); break;
    case 0x13: f8_sl_1(c); break;
    case 0x14: f8_sr_4(c); break;
    case 0x15: f8_sl_4(c); break;
    case 0x16: f8_lm(c); break;
    case 0x17: f8_st(c); break;
    case 0x18: f8_com(c); break;
    case 0x19: f8_lnk(c); break;
    case 0x1a: f8_di(c); break;
    case 0x1b: f8_ei(c); break;
    case 0x1c: f8_pop(c); break;
    case 0x1d: f8_lr_w_j(c); break;
    case 0x1e: f8_lr_j_w(c); break;
    case 0x1f: f8_inc(c); break;

    case 0x20: f8_li(c); break;
    case 0x21: f8_ni(c); break;
    case 0x22: f8_oi(c); break;
    case 0x23: f8_xi(c); break;
    case 0x24: f8_ai(c); break;
    case 0x25: f8_ci(c); break;
    case 0x26: f8_in(c); break;
    case 0x27: f8_out(c); break;
    case 0x28: f8_pi(c); break;
    case 0x29: f8_jmp(c); break;
    case 0x2a: f8_dci(c); break;
    case 0x2b: f8_nop(c); break;
    case 0x2c: f8_xdc(c); break;
    case 0x2d: illegal(c); break;
    case 0x2e: illegal(c); break;
    case 0x2f: illegal(c); break;

    case 0x30: f8_ds_r(c, 0); break;
    case 0x31: f8_ds_r(c, 1); break;
    case 0x32: f8_ds_r(c, 2); break;
    case 0x33: f8_ds_r(c, 3); break;
    case 0x34: f8_ds_r(c, 4); break;
    case 0x35: f8_ds_r(c, 5); break;
    case 0x36: f8_ds_r(c, 6); break;
    case 0x37: f8_ds_r(c, 7); break;
    case 0x38: f8_ds_r(c, 8); break;
    case 0x39: f8_ds_r(c, 9); break;
    case 0x3a: f8_ds_r(c, 10); break;
    case 0x3b: f8_ds_r(c, 11); break;
    case 0x3c: f8_ds_isar(c); break;
    case 0x3d: f8_ds_isar_i(c); break;
    case 0x3e: f8_ds_isar_d(c); break;
    case 0x3f: illegal(c); break;

    case 0x40: f8_lr_a_r(c, 0); break;
    case 0x41: f8_lr_a_r(c, 1); break;
    case 0x42: f8_lr_a_r(c, 2); break;
    case 0x43: f8_lr_a_r(c, 3); break;
    case 0x44: f8_lr_a_r(c, 4); break;
    case 0x45: f8_lr_a_r(c, 5); break;
    case 0x46: f8_lr_a_r(c, 6); break;
    case 0x47: f8_lr_a_r(c, 7); break;
    case 0x48: f8_lr_a_r(c, 8); break;
    case 0x49: f8_lr_a_r(c, 9); break;
    case 0x4a: f8_lr_a_r(c, 10); break;
    case 0x4b: f8_lr_a_r(c, 11); break;
    case 0x4c: f8_lr_a_isar(c); break;
    case 0x4d: f8_lr_a_isar_i(c); break;
    case 0x4e: f8_lr_a_isar_d(c); break;
    case 0x4f: illegal(c); break;

    case 0x50: f8_lr_r_a(c, 0); break;
    case 0x51: f8_lr_r_a(c, 1); break;
    case 0x52: f8_lr_r_a(c, 2); break;
    case 0x53: f8_lr_r_a(c, 3); break;
    case 0x54: f8_lr_r_a(c, 4); break;
    case 0x55: f8_lr_r_a(c, 5); break;
    case 0x56: f8_lr_r_a(c, 6); break;
    case 0x57: f8_lr_r_a(c, 7); break;
    case 0x58: f8_lr_r_a(c, 8); break;
    case 0x59: f8_lr_r_a(c, 9); break;
    case 0x5a: f8_lr_r_a(c, 10); break;
    case 0x5b: f8_lr_r_a(c, 11); break;
    case 0x5c: f8_lr_isar_a(c); break;
    case 0x5d: f8_lr_isar_i_a(c); break;
    case 0x5e: f8_lr_isar_d_a(c); break;
    case 0x5f: illegal(c); break;

    case 0x60: f8_lisu(c, 0x00); break;
    case 0x61: f8_lisu(c, 0x08); break;
    case 0x62: f8_lisu(c, 0x10); break;
    case 0x63: f8_lisu(c, 0x18); break;
    case 0x64: f8_lisu(c, 0x20); break;
    case 0x65: f8_lisu(c, 0x28); break;
    case 0x66: f8_lisu(c, 0x30); break;
    case 0x67: f8_lisu(c, 0x38); break;
    case 0x68: f8_lisl(c, 0x00); break;
    case 0x69: f8_lisl(c, 0x01); break;
    case 0x6a: f8_lisl(c, 0x02); break;
    case 0x6b: f8_lisl(c, 0x03); break;
    case 0x6c: f8_lisl(c, 0x04); break;
    case 0x6d: f8_lisl(c, 0x05); break;
    case 0x6e: f8_lisl(c, 0x06); break;
    case 0x6f: f8_lisl(c, 0x07); break;

    case 0x70: f8_lis(c, 0x0); break;
    case 0x71: f8_lis(c, 0x1); break;
    case 0x72: f8_lis(c, 0x2); break;
    case 0x73: f8_lis(c, 0x3); break;
    case 0x74: f8_lis(c, 0x4); break;
    case 0x75: f8_lis(c, 0x5); break;
    case 0x76: f8_lis(c, 0x6); break;
    case 0x77: f8_lis(c, 0x7); break;
    case 0x78: f8_lis(c, 0x8); break;
    case 0x79: f8_lis(c, 0x9); break;
    case 0x7a: f8_lis(c, 0xa); break;
    case 0x7b: f8_lis(c, 0xb); break;
    case 0x7c: f8_lis(c, 0xc); break;
    case 0x7d: f8_lis(c, 0xd); break;
    case 0x7e: f8_lis(c, 0xe); break;
    case 0x7f: f8_lis(c, 0xf); break;

    case 0x80: f8_bt(c, 0); break;
    case 0x81: f8_bt(c, 1); break;
    case 0x82: f8_bt(c, 2); break;
    case 0x83: f8_bt(c, 3); break;
    case 0x84: f8_bt(c, 4); break;
    case 0x85: f8_bt(c, 5); break;
    case 0x86: f8_bt(c, 6); break;
    case 0x87: f8_bt(c, 7); break;
    case 0x88: f8_am(c); break;
    case 0x89: f8_amd(c); break;
    case 0x8a: f8_nm(c); break;
    case 0x8b: f8_om(c); break;
    case 0x8c: f8_xm(c); break;
    case 0x8d: f8_cm(c); break;
    case 0x8e: f8_adc(c); break;
    case 0x8f: f8_br7(c); break;

    case 0x90: f8_bf(c, 0x0); break;
    case 0x91: f8_bf(c, 0x1); break;
    case 0x92: f8_bf(c, 0x2); break;
    case 0x93: f8_bf(c, 0x3); break;
    case 0x94: f8_bf(c, 0x4); break;
    case 0x95: f8_bf(c, 0x5); break;
    case 0x96: f8_bf(c, 0x6); break;
    case 0x97: f8_bf(c, 0x7); break;
    case 0x98: f8_bf(c, 0x8); break;
    case 0x99: f8_bf(c, 0x9); break;
    case 0x9a: f8_bf(c, 0xa); break;
    case 0x9b: f8_bf(c, 0xb); break;
    case 0x9c: f8_bf(c, 0xc); break;
    case 0x9d: f8_bf(c, 0xd); break;
    case 0x9e: f8_bf(c, 0xe); break;
    case 0x9f: f8_bf(c, 0xf); break;

    case 0xa0: f8_ins_0(c, 0x0); break;
    case 0xa1: f8_ins_0(c, 0x1); break;
    case 0xa2: illegal(c); break;
    case 0xa3: illegal(c); break;
    case 0xa4: f8_ins_1(c, 0x4); break;
    case 0xa5: f8_ins_1(c, 0x5); break;
    case 0xa6: f8_ins_1(c, 0x6); break;
    case 0xa7: f8_ins_1(c, 0x7); break;
    case 0xa8: f8_ins_1(c, 0x8); break;
    case 0xa9: f8_ins_1(c, 0x9); break;
    case 0xaa: f8_ins_1(c, 0xa); break;
    case 0xab: f8_ins_1(c, 0xb); break;
    case 0xac: f8_ins_1(c, 0xc); break;
    case 0xad: f8_ins_1(c, 0xd); break;
    case 0xae: f8_ins_1(c, 0xe); break;
    case 0xaf: f8_ins_1(c, 0xf); break;

    case 0xb0: f8_outs_0(c, 0x0); break;
    case 0xb1: f8_outs_0(c, 0x1); break;
    case 0xb2: illegal(c); break;
    case 0xb3: illegal(c); break;
    case 0xb4: f8_outs_1(c, 0x4); break;
    case 0xb5: f8_outs_1(c, 0x5); break;
    case 0xb6: f8_outs_1(c, 0x6); break;
    case 0xb7: f8_outs_1(c, 0x7); break;
    case 0xb8: f8_outs_1(c, 0x8); break;
    case 0xb9: f8_outs_1(c, 0x9); break;
    case 0xba: f8_outs_1(c, 0xa); break;
    case 0xbb: f8_outs_1(c, 0xb); break;
    case 0xbc: f8_outs_1(c, 0xc); break;
    case 0xbd: f8_outs_1(c, 0xd); break;
    case 0xbe: f8_outs_1(c, 0xe); break;
    case 0xbf: f8_outs_1(c, 0xf); break;

    case 0xc0: f8_as(c, 0x0); break;
    case 0xc1: f8_as(c, 0x1); break;
    case 0xc2: f8_as(c, 0x2); break;
    case 0xc3: f8_as(c, 0x3); break;
    case 0xc4: f8_as(c, 0x4); break;
    case 0xc5: f8_as(c, 0x5); break;
    case 0xc6: f8_as(c, 0x6); break;
    case 0xc7: f8_as(c, 0x7); break;
    case 0xc8: f8_as(c, 0x8); break;
    case 0xc9: f8_as(c, 0x9); break;
    case 0xca: f8_as(c, 0xa); break;
    case 0xcb: f8_as(c, 0xb); break;
    case 0xcc: f8_as_isar(c); break;
    case 0xcd: f8_as_isar_i(c); break;
    case 0xce: f8_as_isar_d(c); break;
    case 0xcf: illegal(c); break;

    case 0xd0: f8_asd(c, 0x0); break;
    case 0xd1: f8_asd(c, 0x1); break;
    case 0xd2: f8_asd(c, 0x2); break;
    case 0xd3: f8_asd(c, 0x3); break;
    case 0xd4: f8_asd(c, 0x4); break;
    case 0xd5: f8_asd(c, 0x5); break;
    case 0xd6: f8_asd(c, 0x6); break;
    case 0xd7: f8_asd(c, 0x7); break;
    case 0xd8: f8_asd(c, 0x8); break;
    case 0xd9: f8_asd(c, 0x9); break;
    case 0xda: f8_asd(c, 0xa); break;
    case 0xdb: f8_asd(c, 0xb); break;
    case 0xdc: f8_asd_isar(c); break;
    case 0xdd: f8_asd_isar_i(c); break;
    case 0xde: f8_asd_isar_d(c); break;
    case 0xdf: illegal(c); break;

    case 0xe0: f8_xs(c, 0x0); break;
    case 0xe1: f8_xs(c, 0x1); break;
    case 0xe2: f8_xs(c, 0x2); break;
    case 0xe3: f8_xs(c, 0x3); break;
    case 0xe4: f8_xs(c, 0x4); break;
    case 0xe5: f8_xs(c, 0x5); break;
    case 0xe6: f8_xs(c, 0x6); break;
    case 0xe7: f8_xs(c, 0x7); break;
    case 0xe8: f8_xs(c, 0x8); break;
    case 0xe9: f8_xs(c, 0x9); break;
    case 0xea: f8_xs(c, 0xa); break;
    case 0xeb: f8_xs(c, 0xb); break;
    case 0xec: f8_xs_isar(c); break;
    case 0xed: f8_xs_isar_i(c); break;
    case 0xee: f8_xs_isar_d(c); break;
    case 0xef: illegal(c); break;

    case 0xf0: f8_ns(c, 0x0); break;
    case 0xf1: f8_ns(c, 0x1); break;
    case 0xf2: f8_ns(c, 0x2); break;
    case 0xf3: f8_ns(c, 0x3); break;
    case 0xf4: f8_ns(c, 0x4); break;
    case 0xf5: f8_ns(c, 0x5); break;
    case 0xf6: f8_ns(c, 0x6); break;
    case 0xf7: f8_ns(c, 0x7); break;
    case 0xf8: f8_ns(c, 0x8); break;
    case 0xf9: f8_ns(c, 0x9); break;
    case 0xfa: f8_ns(c, 0xa); break;
    case 0xfb: f8_ns(c, 0xb); break;
    case 0xfc: f8_ns_isar(c); break;
    case 0xfd: f8_ns_isar_i(c); break;
    case 0xfe: f8_ns_isar_d(c); break;
    case 0xff: illegal(c); break;
    }
    switch (op) {
    case 0x0c: case 0x1b: case 0x1c: case 0x1d:
    case 0x27: case 0x28: case 0x29:
    case 0xb4: case 0xb5: case 0xb6: case 0xb7:
    case 0xb8: case 0xb9: case 0xba: case 0xbb:
    case 0xbc: case 0xbd: case 0xbe: case 0xbf:
        /* don't handle irq after privileged instruction */
        romc_00(c, CS);
        break;

    default:
        /* 'freeze cycle' for handling interrupts */
        if ((c->w & F8_ICB) && c->irq_request) {
            romc_10(c);
            romc_1c(c, CL);
            romc_0f(c);
            romc_13(c);
        }

        /* fetch next instruction (DS is long cycle) */
        if (op >= 0x30 && op <= 0x3f)
            romc_00(c, CL);
        else
            romc_00(c, CS);
        break;
    }

    return (int)(c->cycles - start);
}

/* ---- the host's side ---- */

void f8_power_on(f8_t *c)
{
    c->pc0 = c->pc1 = c->dc0 = c->dc1 = 0;
    c->io = 0;
    c->irq_vector = 0;
    c->a = c->w = c->is = c->dbus = 0;
    memset(c->r, 0, sizeof c->r);
    c->irq_request = 0;
    c->cycles = 0;
    c->instructions = 0;
    c->illegal = 0;
    c->illegal_pc = -1;
    f8_reset(c);
}

void f8_reset(f8_t *c)
{
    /* MAME's device_reset runs before the scheduler loads m_icount, so what
     * it spends is lost: nothing is charged */
    const uint64_t t = c->cycles;

    /* save PC0 to PC1 and reset PC0, and clear ICB */
    romc_08(c);
    c->cycles = t;
    c->w &= (uint8_t)~F8_ICB;

    /* fetch the first opcode */
    romc_00(c, CS);
    c->cycles = t;
}

void f8_set_pc(f8_t *c, uint16_t pc)
{
    c->pc0 = pc;
    c->dbus = mem_read(c, c->pc0, 0x00);
    c->pc0++;
}
