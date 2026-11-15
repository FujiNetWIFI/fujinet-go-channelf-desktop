/*
 * The debugger window's two Channel F-only hand-drawn widgets (Win32): the
 * scratchpad grid and the controller / console-panel lamps. Each is a small
 * window class that paints the state dbg_window.c last gave it, double
 * buffered; neither reads the engine on its own.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <windows.h>

#include "chfdebug.h"

/* Sent to the scratch grid's parent when a cell is clicked: wParam the register 0-63. */
#define WM_DBG_SCRATCH_PICK (WM_APP + 6)

void dbg_widgets_register(HINSTANCE inst);

/* The 8 x 8 octal grid of the 64 registers: the ISAR cell outlined, cells in
 * `changed` (a bit each) in the accent, the BIOS K stack (r40-r59) shaded. */
HWND dbg_scratch_create(HWND parent, int id);
void dbg_scratch_update(HWND grid, const chfdebug_cpu *cpu, uint64_t changed, HFONT mono, int mono_w, int mono_h, int dpi);
void dbg_scratch_size(int mono_w, int mono_h, int dpi, int *w, int *h);

/* The two hand controllers' eight contacts and the console's buttons, as the
 * console latched them, with what ports 0, 1 and 4 read. */
HWND dbg_lamps_create(HWND parent, int id);
void dbg_lamps_update(HWND lamps, const chfdebug_io *io, const chfdebug_video *vid, HFONT mono, int mono_w, int mono_h, int dpi);
void dbg_lamps_size(int mono_w, int mono_h, int dpi, int *w, int *h);

/* The name the BIOS gives a scratchpad register ("J", "Hu"), "" for none. */
const char *dbg_reg_name(int reg);
