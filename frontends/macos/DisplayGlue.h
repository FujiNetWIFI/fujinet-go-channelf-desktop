/*
 * DisplayGlue.h -- what DisplayView.m needs to know about THIS machine: the
 * session's functions, the frame size and the shape of the picture.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdatomic.h>

#include "chfsession.h"

#define FN_VIEW      ChfDisplayView
#define FN_SESSION   chfsession
#define FN_FB_W      CHFSESSION_FB_WIDTH
#define FN_FB_H      CHFSESSION_FB_HEIGHT
#define FN_INITIAL_W 699
#define FN_INITIAL_H 524

/* setMachine:squarePixels: (DisplayExtras.inc). */
#define FN_EXTRA_METHODS "DisplayExtras.inc"

/* The picture's width:height, set by setMachine:squarePixels:. */
static _Atomic int s_fn_aspect_num = 4;
static _Atomic int s_fn_aspect_den = 3;

static inline int fn_machine_hz(FN_SESSION *s) { return chfsession_refresh_rate(s); }

static inline void fn_notify_vsync(FN_SESSION *s, int64_t ns)
{
    chfsession_notify_vsync(s, ns);
}

/* Every frame is the whole 204 x 232; nothing to measure. */
static inline int fn_pull_frame(FN_SESSION *s, uint32_t *dst, int *lines, uint64_t *serial)
{
    *lines = FN_FB_H;
    return chfsession_copy_frame(s, dst, serial);
}

/* The machine and the "aspect" setting say which shape; the TV flag is not
 * used. */
static inline double fn_picture_aspect(FN_SESSION *s, int lines, BOOL tv)
{
    (void)s; (void)lines; (void)tv;
    return (double)atomic_load(&s_fn_aspect_num) / (double)atomic_load(&s_fn_aspect_den);
}
