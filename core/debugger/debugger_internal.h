/*
 * debugger_internal -- the engine's side of the session glue, the prompt's
 * view of the engine, and what debugger.c shares with uviview.c. Not
 * installed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CHF_DEBUGGER_INTERNAL_H
#define CHF_DEBUGGER_INTERNAL_H

#include "chfdebug.h"
#include "host.h"
#include "chf_internal.h"
#include "symbols.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An engine for the running host, with no session: what chfdebug_get() makes
 * for a session, and what the tests make directly. */
chfdebug *chfdebug_create(void);
void chfdebug_free(chfdebug *d);
/* The machine is about to go away (a power cycle or a stop): resume and
 * unhook, remembering whether a window held the engine. */
void chfdebug_power_down(chfdebug *d);
/* A new machine is up: hook it again if a window held the engine (which
 * stops it at its first instruction, so a power cycle can be debugged from
 * the reset vector). */
void chfdebug_power_up(chfdebug *d);
/* The opened cartridge, for symbols next to it ("" for CONFIG). */
void chfdebug_set_cart_path(chfdebug *d, const char *path);
/* Feed the FujiNet trace from the transport, from now on. */
void chfdebug_fujinet_install(chfdebug *d);

/* For the prompt (prompt.c). */
symtab *chfdebug_symbols(chfdebug *d);
/* Evaluate an expression in the machine's context; 0 or -1 with err. */
int chfdebug_eval(chfdebug *d, const char *text, long *out, char *err, int errsz);
/* Step n instructions (the prompt's `step n`). */
void chfdebug_step_n(chfdebug *d, int n);
/* Register names the evaluator knows, for completion. */
extern const char *const chfdebug_reg_names[];

/* The running machine, or NULL when the host is not running. Every use is
 * under chf_host_lock (or on the machine thread). */
chf_machine_t *chfdebug_machine(void);
/* Where the beam is: the UVI's line and the pixel in it. */
void chfdebug_beam(chf_machine_t *m, int *vpos, int *hpos);

/* uviview.c: the prompt's views of the chip. Each takes the host lock. */
int chfdebug_text_uvi(char *dst, int dstsz);
int chfdebug_text_objects(char *dst, int dstsz);
int chfdebug_text_screen(char *dst, int dstsz);

#ifdef __cplusplus
}
#endif

#endif /* CHF_DEBUGGER_INTERNAL_H */
