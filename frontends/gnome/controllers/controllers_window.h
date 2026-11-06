/*
 * The Controllers window -- the Channel F console drawn as a player sees it:
 * the panel (RESET, 1 TIME, 2 MODE, 3 HOLD, 4 START) across the top and both
 * hand controllers below it, each as two views of its knob (from above with
 * eight direction wedges and the twist arrows, from the side with PULL and
 * PUSH). Held controls light up in the accent whoever holds them, the pointer
 * works the knobs, and Map mode rebinds any control to a key or a gamepad
 * button.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "chfsession.h"

/* Toggles visibility: shows the singleton window (creating it on first
 * call), or hides it if already showing. `parent` is the main window; it is
 * only used the first time, to set the transient-for relationship, and keys
 * typed in this window go through it. */
void chf_controllers_toggle(GtkWindow *parent, chfsession *session);
/* Shows it (or raises it). */
void chf_controllers_show(GtkWindow *parent, chfsession *session);
gboolean chf_controllers_visible(void);
