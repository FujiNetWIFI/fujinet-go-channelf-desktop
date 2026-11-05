/*
 * FujiNet Go Channel F -- the GNOME (GTK4/libadwaita) frontend.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <adwaita.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include "chfsession.h"
#include "window.h"

#define APP_ID "online.fujinet.go.channelf.gnome"

static chfsession *g_session;
static char *g_media_arg;
/* The main window. Not gtk_application_get_active_window(): that is the
 * window focused last, which can be the (hidden) controllers or debugger
 * window, and a file opened then must still reach the machine's window. */
static GtkWidget *g_main_window;

/* The installed icons are named after the desktop-entry id. Running straight
 * out of the build tree there is no such icon, so point GTK's icon theme at
 * the in-tree artwork (named for the project, not the id) and use that --
 * otherwise GTK's window-icon lookup fails with a NULL texture on every
 * frame it tries to paint the icon. */
const char *chf_icon_name(void)
{
    static const char *name;
    GtkIconTheme *theme;

    if (name)
        return name;

    name = APP_ID;
    theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
    if (theme && !gtk_icon_theme_has_icon(theme, name)) {
        gtk_icon_theme_add_search_path(theme, CHF_SOURCE_ICON_DIR);
        if (gtk_icon_theme_has_icon(theme, "fujinet-go-channelf"))
            name = "fujinet-go-channelf";
    }
    return name;
}

/* The controllers, debugger and log windows are hidden rather than destroyed
 * and belong to the application (so the shell groups them with it), which
 * means the application would outlive its main window. Closing the main
 * window is quitting. */
static gboolean on_main_close(GtkWindow *win, gpointer user_data)
{
    (void)win;
    g_application_quit(G_APPLICATION(user_data));
    return FALSE;
}

/* SIGTERM (a logout, a service manager) and SIGINT (Ctrl+C in the terminal)
 * quit the way closing the window does, so the session stops the machine
 * and FujiNet and flushes the settings instead of dying mid-write. */
static gboolean on_quit_signal(gpointer user_data)
{
    g_application_quit(G_APPLICATION(user_data));
    return G_SOURCE_CONTINUE;
}

static void on_activate(AdwApplication *app, gpointer user_data)
{
    chfsession_start_opts opts;
    GtkWidget *win;
    (void)user_data;

    /* One window per process: the machine, and the FujiNet cartridge inside
     * it, are process singletons. Raising the existing window is the honest
     * response to a second activation. */
    if (g_main_window) {
        gtk_window_present(GTK_WINDOW(g_main_window));
        return;
    }

    win = chf_window_new(app, g_session);
    g_main_window = win;
    g_object_add_weak_pointer(G_OBJECT(win), (gpointer *)&g_main_window);
    g_signal_connect(win, "close-request", G_CALLBACK(on_main_close), app);

    chfsession_default_opts(g_session, &opts);
    gtk_window_present(GTK_WINDOW(win));
    /* a machine that cannot start says why; Preferences is still there to
     * choose another */
    if (chfsession_start(g_session, &opts) != 0) {
        g_warning("%s", chfsession_last_error(g_session));
        chf_window_message(CHF_WINDOW(win), "The Console Did Not Start",
                          chfsession_last_error(g_session));
    }

    /* A file named on the command line takes the drag-and-drop path: a
     * cartridge is opened (and remembered, as from the menu),
     * anything else goes to FujiNet's SD folder. */
    if (g_media_arg) {
        chf_window_load_media(CHF_WINDOW(win), g_media_arg);
        g_clear_pointer(&g_media_arg, g_free);
    }
}

static void on_open(GApplication *app, GFile **files, gint n_files,
                    const gchar *hint, gpointer user_data)
{
    (void)hint; (void)user_data;
    if (n_files <= 0) {
        g_application_activate(app);
        return;
    }
    /* Already running: the file is for the machine that is up. */
    if (g_main_window) {
        g_autofree char *path = g_file_get_path(files[0]);
        gtk_window_present(GTK_WINDOW(g_main_window));
        if (path)
            chf_window_load_media(CHF_WINDOW(g_main_window), path);
        return;
    }
    g_free(g_media_arg);
    g_media_arg = g_file_get_path(files[0]);
    g_application_activate(app);
}

int main(int argc, char **argv)
{
    AdwApplication *app;
    int status;

    g_session = chfsession_new(NULL);
    if (!g_session) {
        g_printerr("Could not create the session (unusable config/data "
                   "directories?)\n");
        return 1;
    }

    app = adw_application_new(APP_ID, G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(on_open), NULL);
    g_unix_signal_add(SIGTERM, on_quit_signal, app);
    g_unix_signal_add(SIGINT, on_quit_signal, app);

    status = g_application_run(G_APPLICATION(app), argc, argv);

    chfsession_stop(g_session);
    chfsession_free(g_session);
    g_object_unref(app);
    g_free(g_media_arg);
    return status;
}
