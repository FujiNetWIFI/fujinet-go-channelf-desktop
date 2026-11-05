/*
 * The FujiNet console log window, and the FujiNet configuration window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "chfsession.h"

G_BEGIN_DECLS

/* Shows (raising an existing one) the FujiNet console log window. */
void chf_fujilog_show(GtkWindow *parent, chfsession *session);
/* Opens the FujiNet web UI in the system browser. Not embedded: the web UI's
 * Google and OneDrive Authorize buttons open the provider's consent page in a
 * new tab, and both providers refuse OAuth from an embedded web view. */
void chf_fujiconfig_show(GtkWindow *parent, chfsession *session);

G_END_DECLS
