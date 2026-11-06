/*
 * Preferences dialog for the GNOME frontend.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "chfsession.h"
#include "window.h"

G_BEGIN_DECLS

/* Shows the preferences dialog. The right stick, the volume, the aspect and
 * smooth scaling apply live; the machine, the BIOS and the host options
 * (FujiNet, audio, gamepads) are read when the machine powers up, so the
 * dialog power-cycles the session on close if one of those changed. */
void chf_prefs_show(ChfWindow *parent, chfsession *session,
                    void (*restart)(ChfWindow *parent));

G_END_DECLS
