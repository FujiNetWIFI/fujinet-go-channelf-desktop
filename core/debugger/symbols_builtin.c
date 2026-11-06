/*
 * symbols_builtin.c -- the labels every session starts with: the BIOS's four
 * entry points the FujiNet programs use (and no other address inside it), the
 * cartridge's signature, entry and APPBANK window, a Videocart's RAM and bank
 * latch, the FujiNet cartridge's arena and mailbox as the console sees it
 * ($8000-$FFFF), and the console's port names.
 *
 * The mailbox labels are built from the staged fuji_mailbox.h's own macros
 * (the cartridge firmware's header, staged verbatim), so they cannot drift
 * from the protocol the cartridge speaks; the console address of an arena
 * offset is FN_ARENA_BASE + offset. The hotspot ranges (REGSEL, APPBANK_SEL,
 * REGDATA, TXDATA) get their one-byte-per-register names from
 * chf_mailbox_name() when a dump asks. The port names are entries of the
 * SYM_BANK_PORT kind: they resolve by name and never label a disassembly line.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "symbols.h"
#include "fuji_mailbox.h"

/* an arena offset, as the console addresses it */
#define CONSOLE(off) ((uint16_t)(FN_ARENA_BASE + (off)))

void symtab_add_builtins(symtab *t)
{
    /* The BIOS entry points, the cartridge's handshake and the Videocart slots. */
    static const struct { uint16_t addr; const char *name; } fixed[] = {
        { 0x0000, "RESET" },
        { 0x00D0, "CLRSCRN" },
        { 0x0107, "PUSHK" },
        { 0x011E, "POPK" },
        { FN_ROM_BASE,                "CART_SIG" },          /* $55 */
        { FN_ROM_BASE + 2,            "CART_ENTRY" },
        { FN_ROM_BASE + FN_ROM_CLAIM, "FUJI_CLAIM" },
        { FN_APP_WIN,                 "APPBANK_WIN" },
        { 0x2800, "VCART_RAM" },                              /* chess, multi */
        { 0x3000, "VCART_BANK" },                             /* the multicarts' bank latch */
    };
    /* The cartridge's arena and published page, as the console reads it. */
    static const struct { uint16_t addr; const char *name; } mailbox[] = {
        { CONSOLE(0),               "CART_RAM" },
        { CONSOLE(FN_R_DATA),       "REPLY" },
        { CONSOLE(FN_R_ACKSEQ),     "ACKSEQ" },
        { CONSOLE(FN_R_STATUS),     "STATUS" },
        { CONSOLE(FN_R_ERR),        "ERR" },
        { CONSOLE(FN_R_REPLY_CMD),  "REPLY_CMD" },
        { CONSOLE(FN_R_RXLEN_LO),   "RXLEN" },
        { CONSOLE(FN_R_RXLEN_HI),   "RXLEN_HI" },
        { CONSOLE(FN_R_BOOT_STATE), "BOOT_STATE" },
        { CONSOLE(FN_R_BOOT_PCT),   "BOOT_PCT" },
        { CONSOLE(FN_R_BOOT_ERR),   "BOOT_ERR" },
        { CONSOLE(FN_R_MAGIC0),     "MAGIC" },
        { CONSOLE(FN_R_MAGIC1),     "MAGIC1" },
        { CONSOLE(FN_R_PROTO_VER),  "PROTO_VER" },
        { CONSOLE(FN_R_TICK_LO),    "TICK" },
        { CONSOLE(FN_R_TICK_LO + 1), "TICK_HI" },
        { CONSOLE(FN_H_REGSEL),     "REGSEL" },
        { CONSOLE(FN_H_REGSEL + FN_HOT_BANK), "APPBANK_SEL" },
        { CONSOLE(FN_H_REGSEL + FN_HOT_SWAP), "SWAP" },
        { CONSOLE(FN_H_REGDATA),    "REGDATA" },
        { CONSOLE(FN_H_DATA),       "TXDATA" },
    };
    /* The ports (not PROG addresses): the console's four, and the two 2102
     * Videocarts' pairs. */
    static const struct { uint8_t port; const char *name; } ports[] = {
        { 0x00, "PANEL" },
        { 0x01, "RIGHT_C" },
        { 0x01, "COLOR" },
        { 0x04, "LEFT_C" },
        { 0x04, "COLUMN" },
        { 0x05, "ROW_SOUND" },
        { 0x20, "RAM20" },
        { 0x21, "RAM21" },
        { 0x24, "RAM24" },
        { 0x25, "RAM25" },
    };

    for (unsigned i = 0; i < sizeof fixed / sizeof fixed[0]; i++)
        symtab_add(t, fixed[i].addr, SYM_BANK_ANY, fixed[i].name, 1);
    for (unsigned i = 0; i < sizeof mailbox / sizeof mailbox[0]; i++)
        symtab_add(t, mailbox[i].addr, SYM_BANK_ANY, mailbox[i].name, 1);
    for (unsigned i = 0; i < sizeof ports / sizeof ports[0]; i++)
        symtab_add(t, ports[i].port, SYM_BANK_PORT, ports[i].name, 1);
}
