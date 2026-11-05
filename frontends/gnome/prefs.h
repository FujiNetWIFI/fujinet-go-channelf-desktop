/*
 * Preferences dialog for the GNOME frontend.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "chfsession.h"

G_BEGIN_DECLS

typedef struct _ArcWindow ChfWindow;

/* Shows the preferences dialog. The analog switch, the volume and the aspect
 * apply live; the machine and the host options (FujiNet, audio,
 * gamepads) are read when the machine powers up, so the dialog power-cycles
 * the session on close if one of those changed. */
void chf_prefs_show(ChfWindow *parent, chfsession *session,
                    void (*restart)(ChfWindow *parent));

G_END_DECLS
