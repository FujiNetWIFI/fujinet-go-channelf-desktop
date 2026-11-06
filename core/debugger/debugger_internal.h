/*
 * debugger_internal -- the engine's side of the session glue, the prompt's
 * view of the engine, and what debugger.c shares with vidview.c. Not
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
/* The ROMC stream (`romc on|off|n`): the last n bus cycles, newest last. */
typedef struct {
    uint64_t cycles;
    uint16_t pc0;
    uint8_t romc, dbus;
} chfdebug_romc;
void chfdebug_romc_enable(chfdebug *d, int on);
int  chfdebug_romc_enabled(chfdebug *d);
int  chfdebug_romc_read(chfdebug *d, chfdebug_romc *out, int max);

/* The running machine, or NULL when the host is not running. Every use is
 * under chf_host_lock (or on the machine thread). */
chf_machine_t *chfdebug_machine(void);
/* The instruction about to run (the CPU has fetched its opcode: PC0 - 1). */
uint16_t chfdebug_machine_pc(const chf_machine_t *m);

/* vidview.c: the prompt's views of the chip and the console. Each takes the
 * host lock. */
int chfdebug_text_ports(char *dst, int dstsz);
int chfdebug_text_video(char *dst, int dstsz);
int chfdebug_text_pal(char *dst, int dstsz);
int chfdebug_text_sound(chfdebug *d, char *dst, int dstsz);
int chfdebug_text_input(char *dst, int dstsz);
/* `vram [row]`: one row of values with its palette, or the visible picture. */
int chfdebug_text_vram(int row, char *dst, int dstsz);
int chfdebug_text_scratch(char *dst, int dstsz);
int chfdebug_text_kstack(char *dst, int dstsz);
int chfdebug_text_bios(char *dst, int dstsz);
/* Fix up the newest VRAM-log entry and its cell with the PC of the instruction
 * (the machine logs PC0 - 1, one short for a two-byte OUT). Under the run lock. */
void chfdebug_vram_log_fix_pc(chf_machine_t *m, unsigned offs, uint16_t pc);

#ifdef __cplusplus
}
#endif

#endif /* CHF_DEBUGGER_INTERNAL_H */
