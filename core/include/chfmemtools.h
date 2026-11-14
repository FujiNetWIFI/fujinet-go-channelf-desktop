/*
 * Watches, range parsing, memory-search queries and breakpoint descriptions,
 * kept in the core so the debugger windows only lay them out. Every call
 * takes the address SPACE it means (chfdebug_space): the Memory tab has five
 * (PROG, SCRATCH, VRAM, CART, IO), and a watch, a Go To, a Find or a
 * highlight belongs to the space that was selected when it was made.
 *
 * Addresses by space (a number the text means, in the console's own units):
 *   PROG     $0000-$FFFF   a bare 1-4 hex-digit token ("0810") is hex unless it
 *                          names a label; labels, $hex, 0x, 800h, %bin, o17
 *                          octal and expressions work as in the console
 *   SCRATCH  r0-r63        "r12", "j" "h" "k" "q" "hu" "hl" "ku" "kl" "qu"
 *                          "ql" name registers; a bare number is DECIMAL
 *                          ("59"); $3B and o73 work
 *   VRAM     0-8191        row * 128 + col; "row,col" ("10,20") works too; a
 *                          bare token is hex
 *   CART     0-$1FFFF      an offset into the cartridge's memory; bare hex
 *   IO       0-255         a port; bare hex; port names (PANEL, ROW_SOUND)
 *
 * A watch is text the user typed (an address, a label, an expression, or a
 * range low-high / low..high) shown with its live value. It is stored as text and
 * resolved every time it is shown, so loading symbols later makes a label
 * start resolving.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef CHF_MEMTOOLS_H
#define CHF_MEMTOOLS_H

#include <stdint.h>

#include "chfdebug.h"

#ifdef __cplusplus
extern "C" {
#endif

/* "low..high", "low-high" (plain numbers or labels on both sides), or one
 * expression, in `space`. Returns 1 and fills low and high (inclusive,
 * low <= high), 0 if the text means nothing or lies outside the space's
 * nominal range (PROG 64K, SCRATCH 64, VRAM 8192, CART 128K, IO 256). */
int chfsym_parse_range(chfdebug *d, int space, const char *text, uint32_t *low,
                       uint32_t *high);

/* ---- watches ---- */
#define CHFWATCH_MAX 64
/* The process-wide list, like the symbol tables: it outlives the window. */
int  chfwatch_count(void);
/* Returns the new index, or -1 if empty / full. A watch that does not resolve
 * yet is kept (it shows "?"). */
int  chfwatch_add(int space, const char *text);
void chfwatch_remove(int index);
void chfwatch_clear(void);
const char *chfwatch_text(int index);
/* The space a watch was made in (CHF_SPACE_PROG for a bad index). */
int  chfwatch_space(int index);
unsigned chfwatch_generation(void);
/* "score  $0810  $42 (66)"; other spaces say which ("r12  scratch r12  $05
 * (5)"). While the machine runs the value is "-"; a range shows its first 8
 * bytes. Returns the length. */
int  chfwatch_format(chfdebug *d, int index, char *dst, int dstsz);
/* The range a watch covers (for "show in Memory"; its space is
 * chfwatch_space). 1 if it resolves. */
int  chfwatch_range(chfdebug *d, int index, uint32_t *low, uint32_t *high);

/* ---- memory search ---- */
enum { CHFSEARCH_HEX = 0, CHFSEARCH_TEXT };
#define CHFSEARCH_MAX 32
/* Hex: byte pairs, optional "$" / "0x" / spaces. Text: one byte per
 * character, case-insensitive. Fills pat, n, mask (0xFF) and flags for
 * chfdebug_search. Returns 1, or 0 for empty / malformed text. */
int chfsearch_parse(int kind, const char *text, uint8_t *pat, int *n,
                    uint8_t *mask, int *flags);

/* ---- breakpoints ---- */
/* "Write  $0810 (score)  if value != old"; "In/Out  2" (a port); "VRAM  row 10
 * col 20 to row 12 col 40"; "Scratch  r12-r15". A label only names a PROG
 * address. */
int chfbp_describe(chfdebug *d, const chfdebug_breakpoint *b, char *dst,
                   int dstsz);

/* ---- memory map (PROG space) ---- */
/* The name of the region of `map` (from chfdebug_memory_map) holding addr
 * ("Cartridge RAM (3853)", "Multicart bank (bank 0/3)"), for labelling Memory
 * rows. Fetch the map once per refresh and look rows up in it. Returns the
 * length, 0 and "" when no region covers addr. */
int chfmem_region_name(const chfdebug_region *map, int count, uint16_t addr,
                       char *dst, int dstsz);

/* The space's short name for tabs and text: "PROG", "SCRATCH", "VRAM", "CART",
 * "IO" ("?" for a bad number). */
const char *chfmem_space_name(int space);

#ifdef __cplusplus
}
#endif

#endif /* CHF_MEMTOOLS_H */
