/*
 * The Win32 debugger window over the 2650 / UVI debugger engine
 * (chfdebug.h): eight tabs -- Prompt, CPU & RAM, Disassembly, Video,
 * Sound & I/O, Breakpoints, Cart, FujiNet.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <windows.h>

#include "chfsession.h"

/* Show: created on first use, hidden (and detached, so the machine runs on)
 * when closed. Showing it attaches the engine, which stops the machine, as
 * the other frontends do, so there is something to look at. */
void chf_debugger_show(HWND parent, chfsession *session);

/* F12: show, or hide and let the machine run. */
void chf_debugger_toggle(HWND parent, chfsession *session);
int  chf_debugger_visible(void);

/* Accelerators (F5/F7/F8/Shift+F8/F12) for the main loop's pretranslate
 * hook. Returns 1 if the message was consumed. */
int chf_debugger_pretranslate(MSG *msg);
