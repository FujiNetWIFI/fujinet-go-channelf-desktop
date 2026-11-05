/*
 * SettingsDialog -- the Preferences dialog.
 *
 * Every row applies as it is changed: the picture, the volume, the analog
 * sticks and the gamepad assignments take effect at once, and the machine,
 * audio, gamepad and FujiNet rows are applied by power-cycling
 * the console when the dialog closes (run() says so).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QString>
#include <QWidget>

extern "C" {
#include "chfsession.h"
}

namespace SettingsDialog {

/* Runs the dialog. Returns true when something that needs a power cycle
 * changed, for the caller to restart the session. */
bool run(QWidget *parent, chfsession *session);

} // namespace SettingsDialog
