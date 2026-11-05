/*
 * The Controllers window -- the Channel F console drawn like the photo: both
 * hand controllers (12-key gold keypad, disc, side FIRE button) and the
 * START / OPTION / SELECT / RESET panel, held keys lit, the gamepads' player
 * assignments, and Map mode for rebinding any control to a key or a gamepad
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
gboolean chf_controllers_visible(void);
