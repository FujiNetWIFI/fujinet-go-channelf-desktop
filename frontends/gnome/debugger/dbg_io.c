/*
 * The debugger's Sound & I/O tab: what the console's inputs and its beeper are
 * doing.
 *
 *   left   both hand controllers' eight contacts and the console's panel, as
 *          the machine latched them at the frame (a held contact is a lit
 *          lamp), with the bytes the CPU reads from ports 0, 1 and 4 (active
 *          low); the beeper's mode, frequency, level meter and the forced-on
 *          interval after a change; for a Maze or Hangman cartridge, the 2102
 *          RAM's port and address latch.
 *   right  the sound mode changes (port 5 bits 7-6) and every port access, each
 *          with the PC that made it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_common.h"

#include <string.h>

#define IO_MAX 400
#define SND_MAX 120
#define LAMP_W 74
#define LAMP_H 26
#define LAMP_GAP 4
#define LABEL_W 130

struct DbgIo {
    chfsession *session;
    chfdebug *dbg;
    chfdebug_io io;
    chfdebug_video vid;

    GtkWidget *lamps;
    GtkLabel *ports_text, *sound_text, *ram_text;
    GtkWidget *meter;
    GtkTextView *snd_log, *io_log;
    uint64_t last_io_cycle, last_snd_cycle;
    int last_io_count, last_snd_count;
    uint64_t last_io_sig, last_snd_sig;
};

static const char *const k_snd_name[4] = { "off", "1 kHz", "500 Hz", "120 Hz" };

/* ---- the lamps ------------------------------------------------------------------ */

static void lamp(cairo_t *cr, GtkWidget *wg, double x, double y, const char *label, gboolean on)
{
    cairo_rectangle(cr, x + 0.5, y + 0.5, LAMP_W - 1, LAMP_H - 1);
    if (on) {
        dbg_set_rgb(cr, CHFSESSION_ACCENT_RGB, 1.0);
        cairo_fill_preserve(cr);
    }
    dbg_set_fg(cr, wg, 0.3);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    if (on) dbg_set_rgb(cr, 0x1E1A14, 1.0);
    else dbg_set_fg(cr, wg, 0.8);
    dbg_draw_centred(cr, wg, label, x, y, LAMP_W, LAMP_H, FALSE);
}

static void draw_lamps(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer ud)
{
    static const char *const acts[CHF_ACT_PER_PORT] = {
        "RIGHT", "LEFT", "BACK", "FORWARD", "CCW \xe2\x86\xba", "CW \xe2\x86\xbb", "PULL", "PUSH" };
    static const char *const keys[CHF_KEY_COUNT] = { "1 TIME", "2 MODE", "3 HOLD", "4 START" };
    DbgIo *io = ud;
    GtkWidget *wg = GTK_WIDGET(area);
    int row, a;
    (void)width; (void)height;

    for (row = 0; row < 2; row++) {
        /* Player 1 is the right controller (port 1), Player 2 the left (port 4) */
        const double y = row * (LAMP_H + 8);
        char t[48];
        dbg_set_fg(cr, wg, 0.9);
        g_snprintf(t, sizeof t, "Player %d \xc2\xb7 %s", row + 1, row ? "left" : "right");
        dbg_draw_text(cr, wg, t, 0, y + 4, FALSE, 13);
        for (a = 0; a < CHF_ACT_PER_PORT; a++)
            lamp(cr, wg, LABEL_W + a * (LAMP_W + LAMP_GAP), y, acts[a],
                 (io->io.controller[row] >> a) & 1);
    }
    {
        const double y = 2 * (LAMP_H + 8);
        dbg_set_fg(cr, wg, 0.9);
        dbg_draw_text(cr, wg, "Console", 0, y + 4, FALSE, 13);
        for (a = 0; a < CHF_KEY_COUNT; a++)
            lamp(cr, wg, LABEL_W + a * (LAMP_W + LAMP_GAP), y, keys[a], (io->io.panel >> a) & 1);
        lamp(cr, wg, LABEL_W + 4 * (LAMP_W + LAMP_GAP), y, "RESET", io->io.reset_held != 0);
    }
}

/* ---- logs ----------------------------------------------------------------------------- */

static void refresh_snd_log(DbgIo *io)
{
    static chfdebug_sound_event ev[SND_MAX];
    GString *s;
    uint64_t sig;
    const int n = chfdebug_sound_events(io->dbg, ev, SND_MAX);
    int i;

    sig = n ? ev[n - 1].cycle ^ ((uint64_t)n << 48) : 0;
    if (n == io->last_snd_count && sig == io->last_snd_sig)
        return;
    io->last_snd_count = n;
    io->last_snd_sig = sig;
    s = g_string_new(NULL);
    for (i = 0; i < n; i++)
        g_string_append_printf(s, "frame %-6u  pc $%04X  %s \xe2\x86\x92 %s\n", ev[i].frame, ev[i].pc,
                               k_snd_name[ev[i].old_mode & 3], k_snd_name[ev[i].mode & 3]);
    if (n == 0)
        g_string_append(s, "(no sound mode changes since the debugger attached)\n");
    dbg_log_note_reader(io->snd_log);
    dbg_set_text_follow(io->snd_log, s->str, dbg_log_following(io->snd_log));
    g_string_free(s, TRUE);
}

/* What a port write means, for the four video ports. */
static void port_note(const chfdebug_io_event *e, char *dst, int n)
{
    dst[0] = '\0';
    if (!e->is_write)
        return;
    switch (e->port) {
    case 0: g_snprintf(dst, n, "strobe %s, controllers %s, panel latch $%X",
                       (e->value & 0x20) ? "up" : "down", (e->value & 0x40) ? "off" : "read",
                       e->value & 0xF); break;
    case 1: g_snprintf(dst, n, "colour value %d", (~e->value >> 6) & 3); break;
    case 4: g_snprintf(dst, n, "column %d", ~e->value & 0x7F); break;
    case 5: g_snprintf(dst, n, "row %d, sound %s", ~e->value & 0x3F, k_snd_name[(e->value >> 6) & 3]); break;
    default: break;
    }
}

static void refresh_io_log(DbgIo *io)
{
    static chfdebug_io_event ev[IO_MAX];
    GString *s;
    uint64_t sig;
    const int n = chfdebug_io_events(io->dbg, ev, IO_MAX);
    int i;

    sig = n ? ev[n - 1].cycle ^ ((uint64_t)n << 48) : 0;
    if (n == io->last_io_count && sig == io->last_io_sig)
        return;
    io->last_io_count = n;
    io->last_io_sig = sig;
    s = g_string_new(NULL);
    for (i = 0; i < n; i++) {
        char note[96];
        port_note(&ev[i], note, sizeof note);
        g_string_append_printf(s, "frame %-6u  pc $%04X  %-3s port %02X = $%02X  %s\n", ev[i].frame,
                               ev[i].pc, ev[i].is_write ? "OUT" : "IN", ev[i].port, ev[i].value, note);
    }
    if (n == 0)
        g_string_append(s, "(no port accesses since the debugger attached)\n");
    dbg_log_note_reader(io->io_log);
    dbg_set_text_follow(io->io_log, s->str, dbg_log_following(io->io_log));
    g_string_free(s, TRUE);
}

static void on_io_clear(GtkButton *b, gpointer ud)
{
    DbgIo *io = ud;
    (void)b;
    chfdebug_io_events_clear(io->dbg);
    io->last_io_count = -1;
    refresh_io_log(io);
}

/* ---- refresh ------------------------------------------------------------------------- */

void dbg_io_refresh(DbgIo *io, gboolean live)
{
    char text[1200];
    const chfdebug_io *i;
    int len = 0;
    (void)live;

    chfdebug_io_get(io->dbg, &io->io);
    chfdebug_video_get(io->dbg, &io->vid);
    i = &io->io;
    gtk_widget_queue_draw(io->lamps);

    /* the bytes the CPU reads, active low: a held contact reads 0 */
    len += g_snprintf(text + len, sizeof text - len,
        "port 0   $%02X   panel (bits 0-3, a pressed button reads 0)\n"
        "port 1   $%02X   Player 1, the right controller %s\n"
        "port 4   $%02X   Player 2, the left controller %s\n"
        "The controllers are read while port 0 bit 6 is clear (%s now).",
        io->vid.in0, io->vid.in1, io->vid.controllers_enabled ? "" : "(off)",
        io->vid.in4, io->vid.controllers_enabled ? "" : "(off)",
        io->vid.controllers_enabled ? "on" : "off");
    gtk_label_set_text(io->ports_text, text);

    len = 0;
    len += g_snprintf(text + len, sizeof text - len,
        "Mode          %s   (port 5 bits 7-6 = %d)\n"
        "Level         %d of 32767%s\n"
        "Changes       %u since power-on\n"
        "Audio         %llu samples made\n",
        k_snd_name[i->sound_mode & 3], i->sound_mode, i->sound_level,
        i->sound_forced ? "   (forced on: about 2 ms after a change)" : "",
        i->sound_changes, (unsigned long long)i->audio_samples);
    gtk_label_set_text(io->sound_text, text);
    gtk_level_bar_set_value(GTK_LEVEL_BAR(io->meter), i->sound_level);

    if (i->has_2102)
        g_snprintf(text, sizeof text,
            "2102 RAM       ports $%02X (data) and $%02X (address)   latched address $%03X   bit %d",
            i->ram_port, i->ram_port + 1, i->ram_addr, i->ram_bit);
    else
        g_snprintf(text, sizeof text, "2102 RAM       none (Maze and Hangman carry one)");
    gtk_label_set_text(io->ram_text, text);

    refresh_snd_log(io);
    refresh_io_log(io);
}

/* ---- construction -------------------------------------------------------------------- */

static GtkWidget *mono_text(GtkLabel **out)
{
    GtkWidget *l = gtk_label_new("");
    gtk_widget_add_css_class(l, "monospace");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_yalign(GTK_LABEL(l), 0);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_width_chars(GTK_LABEL(l), 60);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 72);
    *out = GTK_LABEL(l);
    return l;
}

DbgIo *dbg_io_new(chfsession *session, chfdebug *dbg, GtkWidget **root)
{
    DbgIo *io = g_new0(DbgIo, 1);
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *iorow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *clear = gtk_button_new_with_label("Clear");
    GtkWidget *snd_scroll, *io_scroll, *lscroll;

    io->session = session;
    io->dbg = dbg;
    io->last_io_count = io->last_snd_count = -1;

    io->lamps = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(io->lamps),
                                       LABEL_W + 8 * (LAMP_W + LAMP_GAP));
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(io->lamps), 3 * (LAMP_H + 8));
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(io->lamps), draw_lamps, io, NULL);
    gtk_widget_set_halign(io->lamps, GTK_ALIGN_START);

    io->meter = gtk_level_bar_new_for_interval(0, 32767);
    gtk_level_bar_set_mode(GTK_LEVEL_BAR(io->meter), GTK_LEVEL_BAR_MODE_CONTINUOUS);
    gtk_widget_add_css_class(io->meter, "chf-meter");
    gtk_widget_set_size_request(io->meter, 360, 10);
    gtk_widget_set_halign(io->meter, GTK_ALIGN_START);

    gtk_box_append(GTK_BOX(left), dbg_heading("Controllers and console (as the machine latched them)"));
    gtk_box_append(GTK_BOX(left), io->lamps);
    gtk_box_append(GTK_BOX(left), mono_text(&io->ports_text));
    gtk_box_append(GTK_BOX(left), dbg_heading("Sound"));
    gtk_box_append(GTK_BOX(left), mono_text(&io->sound_text));
    gtk_box_append(GTK_BOX(left), io->meter);
    gtk_box_append(GTK_BOX(left), dbg_heading("2102 RAM"));
    gtk_box_append(GTK_BOX(left), mono_text(&io->ram_text));
    lscroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(lscroll), dbg_padded(left));
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(lscroll), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(lscroll, 780, -1);

    snd_scroll = dbg_mono_view(&io->snd_log, FALSE);
    dbg_log_make(io->snd_log);
    gtk_widget_set_size_request(snd_scroll, -1, 130);
    gtk_widget_set_vexpand(snd_scroll, FALSE);
    io_scroll = dbg_mono_view(&io->io_log, FALSE);
    gtk_text_view_set_wrap_mode(io->io_log, GTK_WRAP_WORD_CHAR);
    dbg_log_make(io->io_log);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_io_clear), io);
    gtk_box_append(GTK_BOX(iorow), dbg_heading("Port accesses"));
    gtk_box_append(GTK_BOX(iorow), clear);
    gtk_box_append(GTK_BOX(right), dbg_heading("Sound mode changes"));
    gtk_box_append(GTK_BOX(right), snd_scroll);
    gtk_box_append(GTK_BOX(right), iorow);
    gtk_box_append(GTK_BOX(right), io_scroll);
    gtk_widget_set_hexpand(right, TRUE);

    gtk_box_append(GTK_BOX(outer), lscroll);
    gtk_box_append(GTK_BOX(outer), right);
    *root = dbg_padded(outer);
    return io;
}

void dbg_io_free(DbgIo *io)
{
    g_free(io);
}
