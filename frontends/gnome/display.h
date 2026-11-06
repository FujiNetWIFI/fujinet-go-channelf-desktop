/*
 * The emulator display: a GtkWidget that pulls frames from the session on
 * the compositor's own frame clock.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "chfsession.h"

#define CHF_TYPE_DISPLAY (chf_display_get_type())
G_DECLARE_FINAL_TYPE(ChfDisplay, chf_display, CHF, DISPLAY, GtkWidget)

GtkWidget *chf_display_new(chfsession *session);
/* The picture's shape, width : height (chfsession_display_aspect: the
 * television's 4:3, or square pixels). */
void chf_display_set_aspect(ChfDisplay *self, int num, int den);
void chf_display_set_smooth(ChfDisplay *self, gboolean smooth);
