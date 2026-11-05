/*
 * Where the picture goes inside the display, shared by the GPU and the
 * software display.
 *
 * The 2637 UVI's frame is 192 x 262 (128 pixels of playfield plus 32 of border
 * each side, 262 lines). MAME declares no screen aspect, so it shows the frame
 * across a 4:3 screen: a pixel is about 1.8 times as wide as it is tall. That
 * is the "TV Aspect" picture and the default.
 * Square pixels show the raw buffer at 192 : 262 (a tall, narrow picture).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QRectF>
#include <QSizeF>

#include "chfsession.h"

inline QRectF displayRect(QSizeF avail, bool tvAspect)
{
    const double want = tvAspect ? 4.0 / 3.0
                                 : (double)CHFSESSION_FB_WIDTH / (double)CHFSESSION_FB_HEIGHT;
    const double w = avail.width(), h = avail.height();
    double sw, sh;
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }
    return QRectF((w - sw) / 2.0, (h - sh) / 2.0, sw, sh);
}
