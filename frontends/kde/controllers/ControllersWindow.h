/*
 * The Controllers window (F9): the console drawn like the photo -- two hand
 * controllers (disc, 12-key keypad, side fire) and the START / OPTION / SELECT
 * / RESET keys -- the gamepads with their assignments, and Map mode for
 * rebinding any key, disc direction or fire button. Hidden at start; a
 * fixed-size floating window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QWidget>

#include "chfsession.h"

/* Shows the window, or hides it when it is showing. */
void chfControllersToggle(QWidget *parent, chfsession *session);
bool chfControllersVisible();
/* Preferences closed or the session restarted: redraw. */
void chfControllersRefresh();
