/*
 * The main application window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "chfsession.h"

#define CHF_TYPE_WINDOW (chf_window_get_type())
G_DECLARE_FINAL_TYPE(ChfWindow, chf_window, CHF, WINDOW,
                     AdwApplicationWindow)

GtkWidget *chf_window_new(AdwApplication *app, chfsession *session);
/* main.c: the icon name to use (the installed id, or the in-tree art). */
const char *chf_icon_name(void);
void chf_window_toast(ChfWindow *self, const char *text);
/* A message the user has to read (a toast would vanish before the reason
 * was read): an alert with an OK button. */
void chf_window_message(ChfWindow *self, const char *heading, const char *body);
/* The display's shape: 0 = the television's 4:3, 1 = square pixels. Also
 * follows the machine (the PAL pair's square pixels are a different shape). */
void chf_window_apply_aspect(ChfWindow *self, int aspect);
/* Smooth (bilinear) scaling of the picture, or the sharp pixels. */
void chf_window_apply_smooth(ChfWindow *self, gboolean smooth);
/* A dropped or command-line file: a BIOS dump is imported, a cartridge is
 * copied to the cartridge folder and run, anything else goes to FujiNet's SD
 * folder. */
void chf_window_load_media(ChfWindow *self, const char *path);
/* A system action (CHF_SYSACT_*) from any window's keys, with the main
 * window's feedback: Reset to CONFIG toasts, Debugger Stop opens the
 * debugger. */
void chf_window_run_sysaction(ChfWindow *self, int sysact);
/* A key from any of the app's windows, handled exactly as the main window
 * handles its own: F9/F11/F12, the system actions, then the machine's keys.
 * Returns whether it was used. keycode is the GDK hardware keycode. */
gboolean chf_window_key(ChfWindow *self, guint keyval, guint keycode,
                       GdkModifierType state, gboolean down);
/* Focus left one of the app's windows: let go of every key the machine
 * holds (and forget which system-action keys were down). */
void chf_window_release_keys(ChfWindow *self);

/* Import BIOS: a chooser for files (several, zips allowed) or one folder,
 * then one result dialog over `over` (the window when NULL). `apply`
 * power-cycles the console when the import supplied a BIOS that was missing
 * (or, with `retry`, a cartridge the BIOS rule refused, opens it again).
 * `done` gets the number of items imported, or -1 if the chooser was
 * cancelled. */
void chf_window_import_bios(ChfWindow *self, GtkWidget *over, gboolean folder,
                            gboolean apply, const char *retry,
                            void (*done)(int imported, gpointer user_data),
                            gpointer user_data);
/* The Help context the main window shows now: "main", "nobios" while the
 * Needs BIOS message is up, or "prefs/<page>" in Preferences. */
const char *chf_window_help_context(ChfWindow *self);

/* The accent colour (CHFSESSION_ACCENT_RGB, the Videocart yellow) as CSS
 * classes every window can use: ".chf-accent" paints a widget's background in
 * it (with dark text), ".chf-accent-text" its label (a tint of it on a dark
 * theme, the darker bronze on a light one), and ".chf-pill" makes a label a
 * small rounded indicator. Installed once by the main window. */
void chf_install_accent_css(void);
