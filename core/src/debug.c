/*
 * debug.c -- the session's side of the debugger: one engine per session
 * (core/debugger/debugger.c), told when a power cycle takes the machine
 * away and brings a new one up, and fed FujiNet's traffic from the start.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "session_internal.h"
#include "debugger_internal.h"

chfdebug *chfdebug_get(chfsession *s)
{
    if (!s)
        return NULL;
    if (!s->debugger)
        s->debugger = chfdebug_create();
    return s->debugger;
}

/* The FujiNet trace is always on: it is cheap, and it is most useful about
 * what happened before anyone thought to look. */
void chfdebug_trace_fujinet(struct chfsession *s)
{
    chfdebug_fujinet_install(chfdebug_get(s));
}

void chfdebug_before_power_cycle(struct chfsession *s)
{
    if (s->debugger)
        chfdebug_power_down(s->debugger);
}

void chfdebug_after_power_cycle(struct chfsession *s)
{
    if (!s->debugger)
        return;
    chfdebug_set_cart_path(s->debugger, s->cart_path);
    chfdebug_power_up(s->debugger);
}

void chfdebug_destroy(struct chfsession *s)
{
    chfdebug_free(s->debugger);
    s->debugger = NULL;
}
