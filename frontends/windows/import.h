/*
 * File choosers for cartridges -- see import.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef CHF_WIN_IMPORT_H
#define CHF_WIN_IMPORT_H

#include <windows.h>

#include "chfsession.h"

/* One cartridge (.bin .rom .chf, or a .zip of one) for Open Cartridge; 1 if one was chosen. */
int chf_pick_cart(HWND owner, const char *title, char *path, DWORD pathsz);

/* Asks for cartridge images (several) and copies them onto FujiNet's SD
 * host, then says so in one message box. Returns how many were copied, or
 * -1 when the chooser was cancelled. */
int chf_import_carts(HWND owner, chfsession *session);

#endif
