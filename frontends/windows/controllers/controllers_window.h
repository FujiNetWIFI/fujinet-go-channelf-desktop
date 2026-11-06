/*
 * The Win32 Controllers window (F9): the Channel F console drawn as a player
 * sees it -- its button panel and both hand controllers as two views of the
 * knob -- the gamepads with their assignments, and Map mode, in a fixed-size
 * floating tool window. Hidden at start.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <windows.h>

#include "chfsession.h"

/* Show / hide (F9). Created on first use, hidden not destroyed after. */
void chf_controllers_window_toggle(HWND parent, chfsession *session);
int  chf_controllers_window_visible(void);

/* The gamepad set or the machine changed: refresh. */
void chf_controllers_window_refresh(void);

/* Map mode is armed: the next host key is a binding, not typing. The main
 * window gives its keys here first; 1 if consumed. */
int chf_controllers_map_key(UINT msg, WPARAM vk, LPARAM lp);
