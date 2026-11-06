/*
 * The Preferences window -- see prefs.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef CHF_WIN_PREFS_H
#define CHF_WIN_PREFS_H

#include "win_common.h"
#include "chfsession.h"

/* What the window tells the main window. */
typedef struct {
    /* the picture options changed (applied at once) */
    void (*display_changed)(void);
    /* a BIOS import from the window succeeded: the main window follows it */
    void (*roms_changed)(void);
    /* the window closed; dirty = a setting that needs a power cycle changed */
    void (*closed)(int dirty);
} chf_prefs_hooks;

/* Shows the window, or raises it when it is open. */
void chf_prefs_show(HWND parent, chfsession *session, const chf_prefs_hooks *hooks);
/* Closing without applying anything (quitting): the settings are saved. */
void chf_prefs_abandon(void);
/* An import made elsewhere: refresh what the window shows about the ROMs. */
void chf_prefs_roms_changed(void);
/* The gamepad set changed. */
void chf_prefs_gamepads_changed(void);
/* Tab and the arrow keys move between its controls; 1 if the message was used. */
int chf_prefs_pretranslate(MSG *msg);

#endif
