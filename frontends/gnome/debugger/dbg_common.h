/*
 * What the debugger window's files share: the small widget helpers, and the
 * entry points of the parts that live in their own files (the scratchpad grid
 * of the Memory tab, Video, the I/O half of Cart & I/O, Sound, Cart).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>
#include <pango/pangocairo.h>

#include "chfdebug.h"

G_BEGIN_DECLS

/* A monospace text view in a scrolled window (the view is returned in *out). */
GtkWidget *dbg_mono_view(GtkTextView **out, gboolean editable);
/* Replaces a view's text where the reader left it; unchanged text leaves the
 * buffer, the scroll position and any selection alone. */
void dbg_set_text(GtkTextView *view, const char *text);
/* A log that follows its end while its reader is there: dbg_log_make once,
 * dbg_log_note_reader before new text arrives, then dbg_set_text_follow with
 * follow = dbg_log_following(view) (or TRUE to go to the end regardless). */
void dbg_log_make(GtkTextView *view);
void dbg_log_note_reader(GtkTextView *view);
gboolean dbg_log_following(GtkTextView *view);
void dbg_set_text_follow(GtkTextView *view, const char *text, gboolean follow);
GtkWidget *dbg_labeled(const char *text, GtkWidget *child);
GtkWidget *dbg_heading(const char *text);
GtkWidget *dbg_padded(GtkWidget *w);
/* A dim, wrapped hint with a floor under its width (or the window's minimum
 * height is the label wrapped a word to a line). */
GtkWidget *dbg_hint(const char *text, int width_chars);
/* A monospace label (selectable, left aligned). */
GtkWidget *dbg_mono_label(void);
/* The live refresh does not overwrite a value being typed: while the
 * machine runs, a focused entry keeps what is in it. */
void dbg_set_entry(chfdebug *d, GtkEntry *e, const char *text);
/* A hex entry: the width in digits, monospace, no padding to speak of. */
GtkWidget *dbg_hex_entry(int digits);
/* "$0400", "0x400", "#1024" or bare hex. */
int dbg_parse_num(const char *text, long *out);
/* "$40", "0x40" (hex) or bare decimal, for ports, registers and cells. */
int dbg_parse_dec(const char *text, long *out);
/* Sets or clears the accent on a label (a pill); `on` false dims it. */
void dbg_pill(GtkWidget *w, gboolean on);

void dbg_set_rgb(cairo_t *cr, guint32 rgb, double alpha);
/* The theme's text colour, for lines and labels drawn by hand. */
void dbg_set_fg(cairo_t *cr, GtkWidget *w, double alpha);
/* The accent as a line colour: lightened on a dark theme, the darker bronze
 * on a light one. */
guint32 dbg_accent_line_rgb(void);
/* Text at (x, y) in the widget's font, or monospace at `px` pixels (mono);
 * returns the width drawn. */
int dbg_draw_text(cairo_t *cr, GtkWidget *w, const char *text, double x, double y,
                  gboolean mono, double px);
/* Text centred in a cell, in the widget's font (monospace when asked). */
void dbg_draw_centred(cairo_t *cr, GtkWidget *w, const char *text, double x, double y,
                      double cw, double ch, gboolean mono);

/* ---- the tabs in their own files --------------------------------------------- */

/* Each constructor returns its widget in *root. `changed` is called after the
 * part edited the machine (a register, a byte), so the window can refresh. The
 * refresh functions redraw from the machine as it is now; `live` is the
 * machine running. Call them only while the part is on screen. */
typedef void (*DbgChanged)(gpointer user_data);

/* The scratchpad grid and the BIOS K stack (Memory tab, SCRATCH space). */
typedef struct DbgScratch DbgScratch;
DbgScratch *dbg_scratch_new(chfsession *session, chfdebug *dbg, DbgChanged changed, gpointer ud,
                            GtkWidget **root);
void dbg_scratch_refresh(DbgScratch *c, gboolean live);
void dbg_scratch_select(DbgScratch *c, int reg);
void dbg_scratch_free(DbgScratch *c);

/* The Video tab over chfvidtab.h: a page that owns its controller and frees
 * it when destroyed. The first refresh after the page is mapped, or force,
 * redraws everything. */
GtkWidget *dbg_video_new(chfdebug *dbg);
void dbg_video_refresh(GtkWidget *page, gboolean force);

/* The controllers, ports and 2102 (Cart & I/O) and the beeper (Sound). */
typedef struct DbgIo DbgIo;
DbgIo *dbg_io_new(chfsession *session, chfdebug *dbg, GtkWidget **io_root, GtkWidget **sound_root);
void dbg_io_refresh(DbgIo *io, gboolean live);
void dbg_sound_refresh(DbgIo *io, gboolean live);
void dbg_io_free(DbgIo *io);

/* The cartridge, its memory map, the mailbox page and its events. */
typedef struct DbgCart DbgCart;
/* The state and memory map text, and the mailbox page over the events. */
DbgCart *dbg_cart_new(chfsession *session, chfdebug *dbg, GtkWidget **state_root,
                      GtkWidget **mailbox_root);
void dbg_cart_refresh(DbgCart *c, gboolean live);
void dbg_cart_free(DbgCart *c);

G_END_DECLS
