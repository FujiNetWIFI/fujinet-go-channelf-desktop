/*
 * SettingsDialog -- the Preferences dialog, and the Import BIOS flow it
 * shares with the main window.
 *
 * Every row applies as it is changed: the picture, the volume, the controls
 * and the gamepad assignments take effect at once, and the machine, BIOS,
 * audio, gamepad and FujiNet rows are applied by power-cycling the console
 * when the dialog closes (run() says so).
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
 * changed (or a BIOS was imported), for the caller to restart the session. */
bool run(QWidget *parent, chfsession *session);

/* Import BIOS: the user's own dumps from `path` or, when that is empty, from
 * a file chooser (several files at once, zips included) or, with `folder`,
 * from a folder picked with one. Shows what the session says happened, whole,
 * for every input. Returns how many ROMs were imported, or -1 when the
 * chooser was cancelled. Takes effect at the next power cycle. */
int importRoms(QWidget *parent, chfsession *session, bool folder, const QString &path = QString());

} // namespace SettingsDialog
