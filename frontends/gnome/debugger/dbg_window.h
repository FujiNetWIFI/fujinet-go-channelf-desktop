/*
 * Debugger window for the GNOME frontend.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "chfsession.h"

G_BEGIN_DECLS

/* Shows (creating on first use) the debugger window for the session.
 * Showing attaches the debugger engine, which stops the machine. */
void chf_debugger_show(GtkWindow *parent, chfsession *session);
/* F12: shows it, or hides it (detaching, so the machine runs on). */
void chf_debugger_toggle(GtkWindow *parent, chfsession *session);

G_END_DECLS
