/*
 * expr -- the debugger's expression evaluator: breakpoint conditions,
 * print, set, and every address argument the prompt takes.
 *
 *   numbers     $C000  0xC000  0C000h  49152  %1010  o17 (octal)
 *   names       whatever the context resolves: registers, flags, labels,
 *               vpos/hpos/frame/cycles, and value/addr (row/col, port) in a
 *               breakpoint condition
 *   memory      [expr] a PROG-space byte, and one per address space:
 *               p[expr] PROG, s[expr] the scratchpad, v[expr] the VRAM
 *               (row * 128 + col), c[expr] the cartridge's memory, io[expr]
 *               a port -- all side-effect free
 *   operators   C's, with C's precedence: unary - ! ~, * / %, + -, << >>,
 *               < <= > >=, == !=, &, ^, |, &&, ||
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CHF_EXPR_H
#define CHF_EXPR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* A name to a value; return 1 if known. */
    int (*name)(void *user, const char *name, long *out);
    /* A byte of memory, without side effects. space is the chfdebug_space
     * (0 PROG, 1 SCRATCH, 2 VRAM, 3 CART, 4 IO). */
    uint8_t (*read8)(void *user, int space, uint32_t addr);
    void *user;
} expr_ctx;

/* Evaluate; 0 and *out on success, -1 with a message otherwise. With a NULL
 * context, names evaluate to 0 and memory reads to 0 -- a syntax check. */
int expr_eval(const char *text, const expr_ctx *ctx, long *out, char *err, int errsz);

#ifdef __cplusplus
}
#endif

#endif /* CHF_EXPR_H */
