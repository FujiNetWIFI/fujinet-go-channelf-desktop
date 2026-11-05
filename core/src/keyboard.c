/*
 * keyboard -- the on-screen controllers: the key caps as they are printed on
 * the console, and the label every console key has.
 *
 * The Channel F has no keyboard: two 12-key keypads (one per controller,
 * 1 2 3 / 4 5 6 / 7 8 9 / CLEAR 0 ENTER) and three buttons on the console
 * (START, OPTION, SELECT), plus a RESET. The console key index is the
 * bit number the core's chf_host_keys_set takes (chfsession.h):
 *
 *   0-11   player 1 keypad: 1 2 3 4 5 6 7 8 9 CLEAR 0 ENTER
 *   12-23  player 2 keypad: the same
 *   24-26  START, OPTION, SELECT
 *
 * Geometry: units are key caps (a keypad key is 1.0 x 1.0, the gap between
 * caps 0.1 already in the coordinates, so the pitch is 1.1). Player 1's
 * keypad at the left, the console's buttons in the middle, player 2's keypad
 * at the right.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stddef.h>

#include "chfsession.h"

#define GAP   0.1f
#define PITCH (1.0f + GAP)

/* A cap `wd` pitches wide at column `col` of row `row` (both in pitches). */
#define KEY(lbl, tgt, col, row, wd, kind, port) \
    { lbl, tgt, (col) * PITCH, (row) * PITCH, (wd) * PITCH - GAP, 1.0f, kind, port }
#define NRM CHF_KEYCAP_NORMAL
#define FUN CHF_KEYCAP_FUNCTION
#define INP CHF_KEYCAP_INPUT

#define PAD(base, c0, port) \
    KEY("1", (base) + 0, (c0) + 0, 0, 1, NRM, port), \
    KEY("2", (base) + 1, (c0) + 1, 0, 1, NRM, port), \
    KEY("3", (base) + 2, (c0) + 2, 0, 1, NRM, port), \
    KEY("4", (base) + 3, (c0) + 0, 1, 1, NRM, port), \
    KEY("5", (base) + 4, (c0) + 1, 1, 1, NRM, port), \
    KEY("6", (base) + 5, (c0) + 2, 1, 1, NRM, port), \
    KEY("7", (base) + 6, (c0) + 0, 2, 1, NRM, port), \
    KEY("8", (base) + 7, (c0) + 1, 2, 1, NRM, port), \
    KEY("9", (base) + 8, (c0) + 2, 2, 1, NRM, port), \
    KEY("CLEAR", (base) + 9, (c0) + 0, 3, 1, INP, port), \
    KEY("0", (base) + 10, (c0) + 1, 3, 1, NRM, port), \
    KEY("ENTER", (base) + 11, (c0) + 2, 3, 1, INP, port)

static const chf_keycap caps[] = {
    PAD(0, 0.0f, 0),
    /* the console's buttons, stacked in the middle (1.6 pitches wide) */
    KEY("START", CHF_KEY_START, 3.5f, 0.0f, 1.6f, FUN, -1),
    KEY("OPTION", CHF_KEY_OPTION, 3.5f, 1.0f, 1.6f, FUN, -1),
    KEY("SELECT", CHF_KEY_SELECT, 3.5f, 2.0f, 1.6f, FUN, -1),
    KEY("RESET", -1, 3.5f, 3.0f, 1.6f, CHF_KEYCAP_RESET, -1),
    PAD(12, 5.6f, 1),
};

#define PANEL_WIDTH  (8.6f * PITCH - GAP)
#define PANEL_HEIGHT (3 * PITCH + 1.0f)

const chf_keycap *chfsession_keyboard_layout(int machine, int *n, float *width, float *height)
{
    (void)machine;
    if (n) *n = (int)(sizeof caps / sizeof caps[0]);
    if (width) *width = PANEL_WIDTH;
    if (height) *height = PANEL_HEIGHT;
    return caps;
}

/* ---- the label of each console key ------------------------------------------ */

static const char *const key_labels[CHF_KEY_COUNT] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "CLEAR", "0", "ENTER",
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "CLEAR", "0", "ENTER",
    "START", "OPTION", "SELECT",
};

const char *chfsession_key_label(int machine, int key)
{
    (void)machine;
    return key >= 0 && key < CHF_KEY_COUNT ? key_labels[key] : NULL;
}
