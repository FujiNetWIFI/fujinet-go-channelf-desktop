/*
 * What the main window needs from its display, so the GPU display and the
 * software fallback are interchangeable.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

class QWidget;

class DisplayView {
public:
    virtual ~DisplayView() = default;
    virtual QWidget *widget() = 0;
    /* The shape of the picture, width : height (chfsession_display_aspect:
     * the TV's 4:3, or square pixels for the machine). */
    virtual void setAspect(int num, int den) = 0;
    virtual void setSmooth(bool smooth) = 0;
};
