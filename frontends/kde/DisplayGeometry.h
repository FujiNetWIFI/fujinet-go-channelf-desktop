/*
 * Where the picture goes inside the display, shared by the GPU and the
 * software display.
 *
 * The frame is 204 x 232 (102 x 58 cells, 2 x 4 pixels each). A television
 * shows it across 4:3; square pixels show the raw buffer (PAL's rows are taller
 * still). chfsession_display_aspect gives the shape for the machine and the
 * "aspect" setting, and the display is told it with setAspect.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QRectF>
#include <QSizeF>

#include "chfsession.h"

inline QRectF displayRect(QSizeF avail, double want)
{
    const double w = avail.width(), h = avail.height();
    double sw, sh;
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }
    return QRectF((w - sw) / 2.0, (h - sh) / 2.0, sw, sh);
}
