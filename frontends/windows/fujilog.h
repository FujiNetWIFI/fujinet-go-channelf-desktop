/*
 * The FujiNet console log window -- see fujilog.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef CHF_WIN_FUJILOG_H
#define CHF_WIN_FUJILOG_H

#include "win_common.h"
#include "chfsession.h"

/* Shows (raising an existing one) the FujiNet console log window: the
 * in-process runtime's captured output, refreshed once a second while the
 * window is open. */
void chf_fujilog_show(HWND parent, chfsession *session);

#endif
