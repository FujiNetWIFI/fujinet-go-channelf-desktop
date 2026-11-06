/*
 * File and folder choosers for cartridges and the BIOS -- see import.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef CHF_WIN_IMPORT_H
#define CHF_WIN_IMPORT_H

#include <windows.h>

#include "chfsession.h"

/* One cartridge (.bin .chf, or a .zip of one) for Open Cartridge; 1 if one was chosen. */
int chf_pick_cart(HWND owner, const char *title, char *path, DWORD pathsz);

/* Asks for BIOS files (several, zips included) or a folder, imports each,
 * and says in one message box what came of it. Returns how many ROM items
 * were imported (0: nothing recognised), or -1 when the chooser was
 * cancelled. */
int chf_import_bios(HWND owner, chfsession *session, int folder);

/* Imports dropped or named files and folders the same way, one message box
 * for the lot. Returns how many ROM items came in. */
int chf_import_roms(HWND owner, chfsession *session, const char *const *paths, int n);

/* Asks for cartridge images (several) and copies them onto FujiNet's SD
 * host, then says so in one message box. Returns how many were copied, or
 * -1 when the chooser was cancelled. The chooser and the boxes have a Help
 * button, which sends the owner the registered message chf_import_help_msg(). */
int chf_import_carts(HWND owner, chfsession *session);

/* The message the Help button of those windows sends their owner. */
UINT chf_import_help_msg(void);

/* Opens FujiNet's SD folder in Explorer (made first if it is missing). */
void chf_open_sd_folder(HWND owner, chfsession *session);

#endif
