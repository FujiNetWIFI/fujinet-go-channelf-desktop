/*
 * The FujiNet console log window and the FujiNet configuration window, and
 * the accent colour helpers every window shares.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QColor>
#include <QString>
#include <QWidget>

extern "C" {
#include "chfsession.h"
}

/* Shows (raising an existing one) the FujiNet console log window. */
void fujinet_log_show(QWidget *parent, chfsession *session);
/* Opens the FujiNet web UI in the system browser. Not embedded: the web UI's
 * Google and OneDrive Authorize buttons open the provider's consent page in a
 * new tab, and both providers refuse OAuth from an embedded web view. */
void fujinet_config_show(QWidget *parent, chfsession *session);

/* The accent colour: the Videocarts' yellow. It carries DARK text (a held
 * key, the debugger's current line, the Map target). */
QColor chfAccentColor();
/* Its darker variant: outlines and text in the accent on the light themes. */
QColor chfAccentDarkColor();
/* The text on the accent. */
QColor chfAccentInk();
/* The accent for TEXT (and text-like marks: the status dot, a breakpoint
 * dot): the yellow on a dark theme, the darker variant on a light one, where
 * the yellow is too pale to read as a mark. */
QColor chfAccentTextColor(const QWidget *w);
/* A style sheet that fills a button (or a label) in the accent with dark
 * text. */
QString chfAccentFill();
