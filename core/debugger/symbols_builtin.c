/*
 * symbols_builtin.c -- the labels every session starts with: the 2650's reset
 * vector, the UVI's registers ($1800-$1AFF, named as the Channel F
 * programming notes and MAME's driver do), and the FujiNet cartridge's
 * mailbox as the console sees it ($2B00-$2FFF).
 *
 * The mailbox labels are built from the staged fuji_mailbox.h's own macros
 * (the cartridge firmware's header, staged verbatim), so they cannot drift
 * from the protocol the cartridge speaks; the cartridge's image offsets are
 * the console addresses less $1000 (A12 selects the cartridge). The hotspot
 * ranges ($2D00 REGSEL, $2E00 REGDATA, $2F00 DATA) get their one-byte-per-
 * register names from chf_mailbox_name() when a dump asks.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "symbols.h"
#include "fuji_mailbox.h"

/* an image offset of the cartridge, as the console addresses it */
#define CONSOLE(off) ((uint16_t)((off) + 0x1000))

void symtab_add_builtins(symtab *t)
{
    /* The 2650 and the UVI. */
    static const struct { uint16_t addr; const char *name; } uvi[] = {
        { 0x0000, "reset" },
        { 0x1800, "UVI_SCR1" },           /* upper screen, 13 rows of 16 */
        { 0x18D0, "UVI_RAM1" },
        { 0x18F0, "UVI_OBJ0_Y" },
        { 0x18F1, "UVI_OBJ0_X" },
        { 0x18F2, "UVI_OBJ1_Y" },
        { 0x18F3, "UVI_OBJ1_X" },
        { 0x18F4, "UVI_OBJ2_Y" },
        { 0x18F5, "UVI_OBJ2_X" },
        { 0x18F6, "UVI_OBJ3_Y" },
        { 0x18F7, "UVI_OBJ3_X" },
        { 0x18F8, "UVI_RAM2" },
        { 0x18FC, "UVI_VPOS" },
        { 0x18FD, "UVI_SOUND1" },
        { 0x18FE, "UVI_SOUND2" },
        { 0x18FF, "UVI_CHARLINE" },
        { 0x1900, "UVI_PAD1A" },
        { 0x1904, "UVI_PAD2A" },
        { 0x1908, "UVI_PANEL" },
        { 0x1980, "UVI_UDC38" },          /* the user-defined characters; 38-3B are the objects */
        { 0x1988, "UVI_UDC39" },
        { 0x1990, "UVI_UDC3A" },
        { 0x1998, "UVI_UDC3B" },
        { 0x19A0, "UVI_UDC3C" },
        { 0x19A8, "UVI_UDC3D" },
        { 0x19B0, "UVI_UDC3E" },
        { 0x19B8, "UVI_UDC3F" },
        { 0x19F8, "UVI_PAL0" },
        { 0x19F9, "UVI_PAL1" },
        { 0x19FA, "UVI_PAL2" },
        { 0x19FB, "UVI_PAL3" },
        { 0x19FC, "UVI_COLL_CHARS" },
        { 0x19FD, "UVI_COLL_OBJECTS" },
        { 0x19FE, "UVI_POT2" },
        { 0x19FF, "UVI_POT1" },
        { 0x1A00, "UVI_SCR2" },           /* lower screen, 13 rows of 16 */
        { 0x1AD0, "UVI_RAM3" },
    };
    /* The cartridge's published page, as the console reads it. */
    static const struct { uint16_t addr; const char *name; } mailbox[] = {
        { CONSOLE(FN_R_DATA),       "FN_R_DATA" },
        { CONSOLE(FN_R_ACKSEQ),     "FN_R_ACKSEQ" },
        { CONSOLE(FN_R_STATUS),     "FN_R_STATUS" },
        { CONSOLE(FN_R_ERR),        "FN_R_ERR" },
        { CONSOLE(FN_R_REPLY_CMD),  "FN_R_REPLY_CMD" },
        { CONSOLE(FN_R_RXLEN_LO),   "FN_R_RXLEN_LO" },
        { CONSOLE(FN_R_RXLEN_HI),   "FN_R_RXLEN_HI" },
        { CONSOLE(FN_R_BOOT_STATE), "FN_R_BOOT_STATE" },
        { CONSOLE(FN_R_BOOT_PCT),   "FN_R_BOOT_PCT" },
        { CONSOLE(FN_R_BOOT_ERR),   "FN_R_BOOT_ERR" },
        { CONSOLE(FN_R_MAGIC0),     "FN_R_MAGIC0" },
        { CONSOLE(FN_R_MAGIC1),     "FN_R_MAGIC1" },
        { CONSOLE(FN_R_PROTO_VER),  "FN_R_PROTO_VER" },
        { CONSOLE(FN_R_SLICE_ECHO), "FN_R_SLICE_ECHO" },
        { CONSOLE(FN_R_TICK_LO),    "FN_R_TICK_LO" },
        { CONSOLE(FN_R_TICK_HI),    "FN_R_TICK_HI" },
        { CONSOLE(FN_R_CLAIM),      "FN_R_CLAIM" },
        { CONSOLE(FN_H_REGSEL),     "FN_H_REGSEL" },
        { CONSOLE(FN_H_REGSEL + FN_HOT_BANK), "FN_HOT_BANK" },
        { CONSOLE(FN_H_REGSEL + FN_HOT_SWAP), "FN_HOT_SWAP" },
        { CONSOLE(FN_H_REGDATA),    "FN_H_REGDATA" },
        { CONSOLE(FN_H_DATA),       "FN_H_DATA" },
    };

    for (unsigned i = 0; i < sizeof uvi / sizeof uvi[0]; i++)
        symtab_add(t, uvi[i].addr, SYM_BANK_ANY, uvi[i].name, 1);
    for (unsigned i = 0; i < sizeof mailbox / sizeof mailbox[0]; i++)
        symtab_add(t, mailbox[i].addr, SYM_BANK_ANY, mailbox[i].name, 1);
}
