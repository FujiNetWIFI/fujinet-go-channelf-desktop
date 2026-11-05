/* f8.h -- the Fairchild F8 (F3850) CPU.
 *
 * Transposed from MAME's src/devices/cpu/f8/f8.cpp (BSD-3-Clause,
 * copyright-holders Juergen Buchmueller -- see COMPLIANCE.md), keeping its
 * structure: one function per ROMC state doing the bus access and charging
 * its cycles (cS = 4, cL = 6 clocks), one function per opcode, the same
 * dispatch and the same trailing opcode fetch. Field names keep MAME's own
 * (minus the m_ prefix).
 *
 * The CPU has no address registers of its own: PC0, PC1, DC0 and DC1 live in
 * every memory chip on the bus and follow the ROMC code each cycle. As in
 * MAME there is one set here, and memory sees flat reads and writes through
 * the callbacks; romc_hook exposes the ROMC stream for traces and for running
 * the firmware's own ROMC decoder in a test.
 *
 * MAME fetches the NEXT opcode at the end of each instruction (ROMC 00), so
 * between instructions the opcode is already on the data bus (dbus) and PC0
 * points one past it: the instruction about to run is at f8_pc(). A debugger
 * that changes PC must refetch (f8_set_pc).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef CHF_F8_H
#define CHF_F8_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* W, the status register */
#define F8_SIGN  0x01        /* 1 = the result was POSITIVE (bit 7 clear) */
#define F8_CARRY 0x02
#define F8_ZERO  0x04
#define F8_OVR   0x08
#define F8_ICB   0x10        /* interrupt control bit */

typedef struct f8 f8_t;

struct f8 {
    uint16_t pc0, pc1;       /* program counter, stack register */
    uint16_t dc0, dc1;       /* data counter, auxiliary data counter */
    uint16_t io;             /* the I/O address latch (ROMC 03 into the low byte, MAME's m_io) */
    uint16_t irq_vector;
    uint8_t a;               /* accumulator */
    uint8_t w;               /* status, F8_* */
    uint8_t is;              /* ISAR, 6 bits */
    uint8_t dbus;            /* the data bus */
    uint8_t r[64];           /* the scratchpad */
    int irq_request;         /* /INTREQ asserted (the Channel F never asserts it) */

    uint64_t cycles;         /* clocks since power-on */
    uint64_t instructions;
    unsigned illegal;        /* illegal opcodes executed (they act as no-ops) */
    int illegal_pc;          /* the last one's address, -1 if none */

    /* The bus. Every callback sees `cycles` as it was at the START of the bus
     * cycle making the access. `romc` is the ROMC state doing it, so a reader
     * can tell a data read (ROMC 02, through DC0) from the instruction stream
     * (00 01 03 0C 0E 11, through PC0). */
    void *ctx;
    uint8_t (*read)(void *ctx, uint16_t addr, uint8_t romc);
    void    (*write)(void *ctx, uint16_t addr, uint8_t data);        /* ROMC 05 */
    uint8_t (*in)(void *ctx, uint8_t port);     /* INS 0/1, IN/INS 4+ (ROMC 1B) */
    void    (*out)(void *ctx, uint8_t port, uint8_t data);    /* OUTS 0/1, OUT/OUTS 4+ (ROMC 1A) */
    /* The interrupt vector a peripheral puts on the bus (ROMC 10/13); NULL =
     * irq_vector. */
    uint16_t (*int_vector)(void *ctx);
    /* After each ROMC state, with the bus as that state left it: NULL normally
     * (tests and traces). */
    void    (*romc_hook)(void *ctx, const f8_t *cpu, uint8_t romc, uint8_t dbus);
};

/* Power on: the registers as MAME's device_start leaves them (all zero),
 * then f8_reset. The callbacks must be set first. */
void f8_power_on(f8_t *c);
/* MAME's device_reset: ROMC 08 (PC1 = PC0, PC0 = 0), clear ICB, fetch the
 * opcode at $0000 (ROMC 00). No cycles are charged, as in MAME. The
 * scratchpad, A, the rest of W and DC0/DC1 are kept. */
void f8_reset(f8_t *c);
/* One instruction: its body, then (unless it is privileged) a pending
 * interrupt's entry, then the next opcode's fetch. Returns the clocks it
 * took. */
int  f8_step(f8_t *c);

/* The instruction about to run. */
static inline uint16_t f8_pc(const f8_t *c)
{
    return (uint16_t)(c->pc0 - 1);
}
/* A debugger's PC change: PC0 = pc + 1 with the opcode at pc fetched (through
 * read, ROMC 00, no cycles charged). */
void f8_set_pc(f8_t *c, uint16_t pc);

/* ---- the disassembler (dasmf8.c, from MAME's f8dasm.cpp) ---- */
#define F8_DASM_CALL    0x1  /* PI, PK: a call (step over runs past it) */
#define F8_DASM_RETURN  0x2  /* POP: a return */
#define F8_DASM_BRANCH  0x4  /* a jump or branch (target in *target) */
#define F8_DASM_COND    0x8  /* ... a conditional one */
/* Disassemble the instruction at `pc` whose bytes are op[0..2] (op[1], op[2]
 * may be past its end). Writes MAME's text ("JMP   $0802", "LR    A,KU") into
 * buf; returns its length in bytes (1-3). *flags gets F8_DASM_*, *target the
 * branch/jump/call target when it has one (else -1). Either may be NULL. */
int dasmf8(char *buf, size_t bufsz, uint16_t pc, const uint8_t op[3], unsigned *flags,
           int *target);

#ifdef __cplusplus
}
#endif

#endif /* CHF_F8_H */
