/*
 * bindings -- private interface to core/src/bindings.c: the session seeds
 * the table at chfsession_new (process-global storage, like gamepad_sdl.c's
 * pad table, because the machine is a process singleton and there is only
 * ever one session's settings store live at a time). chfsession.h carries
 * the frontend-facing API.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CHF_BINDINGS_H
#define CHF_BINDINGS_H

#include "chfsession.h"

#ifdef __cplusplus
extern "C" {
#endif

struct chfsession;

/* Reloads the table: every slot to its default, then overridden by whatever
 * "bindings" holds in s's settings store. */
void bindings_init(struct chfsession *s);

#ifdef __cplusplus
}
#endif

#endif /* CHF_BINDINGS_H */
