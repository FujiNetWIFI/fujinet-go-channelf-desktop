/*
 * symbols -- the debugger's label tables: built-in names for the 2650's
 * reset vector, the UVI's registers and the FujiNet cartridge's mailbox,
 * plus whatever symbol files the user loads.
 *
 * Labels name PROG addresses (15 bits). A label belongs to an address, not to
 * a bank: every label matches any bank. The bank field stays (the family's
 * table format, and a WLA-DX "BB:AAAA" file still reads).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CHF_SYMBOLS_H
#define CHF_SYMBOLS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYM_NAME_MAX 48
#define SYM_BANK_ANY (-1)

typedef struct {
    uint16_t addr;
    int16_t bank;            /* 16K ROM bank, or SYM_BANK_ANY */
    uint8_t builtin;
    char name[SYM_NAME_MAX];
} sym_entry;

typedef struct {
    sym_entry *v;
    int n, cap;
    int sorted;
} symtab;

void symtab_init(symtab *t);           /* empty, then the built-ins */
void symtab_free(symtab *t);
void symtab_clear_user(symtab *t);     /* drop every loaded label */
int  symtab_add(symtab *t, uint16_t addr, int bank, const char *name, int builtin);
/* Remove the user labels at addr (any bank). */
void symtab_remove_at(symtab *t, uint16_t addr);

/* The label at addr, or NULL. `bank` is the 16K ROM bank mapped at addr now
 * (SYM_BANK_ANY when unknown or not cartridge memory). */
const char *symtab_lookup(symtab *t, uint16_t addr, int bank);
/* The nearest label at or below addr within `max_offset`, for "label+n". */
const char *symtab_lookup_near(symtab *t, uint16_t addr, int bank, int max_offset,
                               uint16_t *offset);
/* Address of a name (case-insensitive), or -1. */
int  symtab_find(symtab *t, const char *name);

/* Load a symbol file, detecting its format; returns labels added (>= 0) or
 * -1 with a message. */
int  symtab_load(symtab *t, const char *path, char *msg, int msgsz);

/* For completion: the i-th label name, NULL past the end. */
const char *symtab_name_at(symtab *t, int i);

/* The name of a console address inside the FujiNet cartridge's mailbox
 * ($2B00-$2FFF) that the plain labels do not cover -- the hotspot ranges
 * ("FN_H_REGSEL+05", "FN_HOT_BANK+03") -- or NULL. The returned text is in a
 * static buffer, valid until the next call. */
const char *chf_mailbox_name(uint16_t addr);

/* symbols_builtin.c */
void symtab_add_builtins(symtab *t);

#ifdef __cplusplus
}
#endif

#endif /* CHF_SYMBOLS_H */
