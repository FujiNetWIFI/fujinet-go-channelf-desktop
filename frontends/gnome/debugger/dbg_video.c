/*
 * The debugger's Video tab: the Channel F's 128 x 64 video RAM, its row
 * palettes and its four I/O ports.
 *
 *   left   the picture in one of the six views (the frame the display shows,
 *          the live VRAM, the CRT's beam, the whole VRAM, its raw values, the
 *          four palettes) at a chosen scale, nearest neighbour, with the
 *          overlays the core draws (visible area, grid, palette columns, beam
 *          line, write cursor, write heat, changed cells, rows whose palette
 *          changed). Beside it a 64-row palette strip aligned with the
 *          picture's rows: each row's background and palette number, a tick on
 *          the rows whose palette changed since the last frame. Under it the
 *          readout of the cell under the pointer; a click pins one, a
 *          double-click breaks on writes to it. The pinned cell's value,
 *          palette, pen, colour, write count and last writer (PC, frame, line,
 *          the value it replaced) are shown in full.
 *   right  three pages: Palettes (the 4 x 4 table with RGB values and how many
 *          rows use each), Ports (0, 1, 4 and 5 raw, complemented and decoded,
 *          and as the CPU would read them now, with the timing) and VRAM writes
 *          (every strobe since the debugger attached, with filters).
 *
 * Idle cost: the caller refreshes this tab only while it shows, and only the
 * page that shows is redrawn. The picture is rendered by the core into one
 * reused buffer and redrawn only when its pixels changed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_common.h"

#include <math.h>
#include <string.h>

#define CELL_W 2                /* a VRAM cell is 2 x 4 pixels at scale 1 */
#define CELL_H 4
#define MAX_SCALE 4
#define STRIP_W 30
#define WRITES_MAX 600

enum { SEC_PALETTES, SEC_PORTS, SEC_WRITES, SEC_COUNT };

#define NOVERLAYS 8
static const struct { const char *name; const char *tip; unsigned bit; gboolean def; } k_overlays[NOVERLAYS] = {
    { "Visible area", "The part the television shows (VRAM and Values views)", CHFDEBUG_F_VISIBLE, TRUE },
    { "Grid", "Cell borders (scale 2 and up)", CHFDEBUG_F_GRID, FALSE },
    { "Palette columns", "Columns 125-127, which pick each row's palette", CHFDEBUG_F_PALCOLS, TRUE },
    { "Beam line", "Where the beam is now", CHFDEBUG_F_BEAM, FALSE },
    { "Write cursor", "The cell the next strobe writes (ports 4 and 5)", CHFDEBUG_F_CURSOR, FALSE },
    { "Write heat", "Cells written this frame, brighter the more", CHFDEBUG_F_HEAT, FALSE },
    { "Changed cells", "Cells that changed since the last frame", CHFDEBUG_F_DIRTY, FALSE },
    { "Palette rows", "A tick at the left of rows whose palette changed", CHFDEBUG_F_PALROWS, FALSE },
};
static const char *const k_view_names[] = {
    "Frame", "Live", "Beam", "VRAM", "Values", "Palettes", NULL };

struct DbgVideo {
    chfsession *session;
    chfdebug *dbg;
    DbgChanged changed;
    gpointer ud;
    chfdebug_video vid;
    int tick;                   /* live refreshes since the log last redrew */

    /* the picture */
    GtkDropDown *view, *scale;
    GtkCheckButton *flag[NOVERLAYS];
    GtkWidget *pic, *strip;
    uint32_t *buf, *prev;       /* the rendered view, and the one drawn */
    size_t cap;
    int pw, ph;
    gboolean have_pic;
    int hover_row, hover_col;   /* -1 none */
    int pin_row, pin_col;
    GtkLabel *hover_label, *pin_label;

    /* the row palettes, this frame and the last */
    uint8_t cur_pal[CHFDEBUG_VRAM_ROWS], prev_pal[CHFDEBUG_VRAM_ROWS];
    uint32_t pal_frame;
    gboolean have_pal;

    /* pages */
    GtkNotebook *sections;
    GtkWidget *pal_area;
    GtkLabel *ports_text;
    GtkTextView *log_text;
    GtkLabel *log_note;
    GtkSpinButton *row0, *row1, *col0, *col1;
    GtkWidget *f_changes, *f_frame, *f_palcols;
};

/* ---- the picture's geometry ----------------------------------------------------------- */

static int view_now(DbgVideo *v)
{
    return (int)gtk_drop_down_get_selected(v->view);
}

static int scale_now(DbgVideo *v)
{
    return (int)gtk_drop_down_get_selected(v->scale) + 1;
}

static gboolean view_is_whole(int view)
{
    return view == CHFDEBUG_VIEW_VRAM || view == CHFDEBUG_VIEW_VALUES;
}

static gboolean view_is_picture(int view)
{
    return view == CHFDEBUG_VIEW_FRAME || view == CHFDEBUG_VIEW_LIVE || view == CHFDEBUG_VIEW_BEAM;
}

/* The pixels a cell takes in the view at `scale`; FALSE when it is not in it. */
static gboolean cell_rect(int view, int scale, int row, int col, double *x, double *y,
                          double *w, double *h)
{
    int cx, cy;
    if (view_is_whole(view)) {
        cx = col; cy = row;
    } else if (view_is_picture(view)) {
        cx = col - CHFDEBUG_VIS_COL0; cy = row - CHFDEBUG_VIS_ROW0;
        if (cx < 0 || cx >= CHFDEBUG_VIS_COLS || cy < 0 || cy >= CHFDEBUG_VIS_ROWS)
            return FALSE;
    } else {
        return FALSE;
    }
    *x = cx * CELL_W * scale;
    *y = cy * CELL_H * scale;
    *w = CELL_W * scale;
    *h = CELL_H * scale;
    return TRUE;
}

static unsigned overlay_mask(DbgVideo *v)
{
    unsigned m = 0;
    int i;
    for (i = 0; i < NOVERLAYS; i++)
        if (gtk_check_button_get_active(v->flag[i]))
            m |= k_overlays[i].bit;
    return m;
}

/* ---- drawing ------------------------------------------------------------------------- */

static void draw_pic(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer ud)
{
    DbgVideo *v = ud;
    GtkWidget *wg = GTK_WIDGET(area);
    const int view = view_now(v), scale = scale_now(v);
    double x, y, w, h;
    (void)width; (void)height;

    if (v->have_pic && v->pw > 0 && v->ph > 0) {
        cairo_surface_t *surf = cairo_image_surface_create_for_data(
            (unsigned char *)v->buf, CAIRO_FORMAT_RGB24, v->pw, v->ph, v->pw * 4);
        cairo_set_source_surface(cr, surf, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
        cairo_paint(cr);
        cairo_surface_destroy(surf);
    }
    cairo_set_line_width(cr, 1);
    if (v->hover_row >= 0 && cell_rect(view, scale, v->hover_row, v->hover_col, &x, &y, &w, &h)) {
        dbg_set_rgb(cr, 0x000000, 0.8);
        cairo_rectangle(cr, x - 0.5, y - 0.5, w + 1, h + 1);
        cairo_stroke(cr);
        dbg_set_rgb(cr, 0xFFFFFF, 0.9);
        cairo_rectangle(cr, x + 0.5, y + 0.5, w - 1, h - 1);
        cairo_stroke(cr);
    }
    if (v->pin_row >= 0 && cell_rect(view, scale, v->pin_row, v->pin_col, &x, &y, &w, &h)) {
        dbg_set_rgb(cr, CHFSESSION_ACCENT_RGB, 1.0);
        cairo_set_line_width(cr, 2);
        cairo_rectangle(cr, x - 1, y - 1, w + 2, h + 2);
        cairo_stroke(cr);
        dbg_set_rgb(cr, 0x000000, 0.7);
        cairo_set_line_width(cr, 1);
        cairo_rectangle(cr, x - 2.5, y - 2.5, w + 5, h + 5);
        cairo_stroke(cr);
    }
    (void)wg;
}

/* The row strip: one band per VRAM row, the height the picture gives it, so a
 * band sits beside its row. */
static void draw_strip(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer ud)
{
    DbgVideo *v = ud;
    GtkWidget *wg = GTK_WIDGET(area);
    const int view = view_now(v), scale = scale_now(v);
    const int first = view_is_picture(view) ? CHFDEBUG_VIS_ROW0 : 0;
    const int last = view_is_picture(view) ? CHFDEBUG_VIS_ROW0 + CHFDEBUG_VIS_ROWS : CHFDEBUG_VRAM_ROWS;
    const double rh = CELL_H * scale;
    int r;
    (void)width; (void)height;

    if (view == CHFDEBUG_VIEW_PALETTES) {
        dbg_set_fg(cr, wg, 0.5);
        dbg_draw_text(cr, wg, "rows", 2, 2, FALSE, 11);
        return;
    }
    for (r = first; r < last; r++) {
        const double y = (r - first) * rh;
        const gboolean changed = v->have_pal && v->prev_pal[r] != v->cur_pal[r];
        const guint32 bg = v->vid.row_background[r] & 0xFFFFFF;
        const int lum = (int)(((bg >> 16) & 0xFF) * 30 + ((bg >> 8) & 0xFF) * 59 + (bg & 0xFF) * 11) / 100;
        char t[4];

        dbg_set_rgb(cr, bg, 1.0);
        cairo_rectangle(cr, 8, y, STRIP_W - 8, rh);
        cairo_fill(cr);
        if (v->hover_row == r) {
            dbg_set_rgb(cr, CHFSESSION_ACCENT_RGB, 1.0);
            cairo_rectangle(cr, 8.5, y + 0.5, STRIP_W - 9, rh - 1);
            cairo_set_line_width(cr, 1);
            cairo_stroke(cr);
        }
        if (rh >= 12) {
            g_snprintf(t, sizeof t, "%d", v->cur_pal[r]);
            if (lum > 128) dbg_set_rgb(cr, 0x000000, 0.8); else dbg_set_rgb(cr, 0xFFFFFF, 0.9);
            dbg_draw_centred(cr, wg, t, 8, y, STRIP_W - 8, rh, TRUE);
        }
        if (changed) {
            dbg_set_rgb(cr, dbg_accent_line_rgb(), 1.0);
            cairo_move_to(cr, 0, y);
            cairo_line_to(cr, 7, y + rh / 2);
            cairo_line_to(cr, 0, y + rh);
            cairo_close_path(cr);
            cairo_fill(cr);
        }
    }
}

/* The four palettes: each value's colour, RGB and name, and how many rows use it. */
#define PAL_LABEL_W 150
#define PAL_SW_W 92
#define PAL_ROW_H 74

static void draw_palettes(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer ud)
{
    DbgVideo *v = ud;
    GtkWidget *wg = GTK_WIDGET(area);
    int p, val;
    (void)width; (void)height;

    for (p = 0; p < 4; p++) {
        const double y = 6 + p * PAL_ROW_H;
        char t[64];

        dbg_set_fg(cr, wg, 1.0);
        g_snprintf(t, sizeof t, "Palette %d", p);
        dbg_draw_text(cr, wg, t, 4, y + 4, FALSE, 14);
        dbg_set_fg(cr, wg, 0.6);
        dbg_draw_text(cr, wg, chfdebug_palette_name(p), 4, y + 24, FALSE, 12);
        g_snprintf(t, sizeof t, "%d row%s", v->vid.palette_rows[p],
                   v->vid.palette_rows[p] == 1 ? "" : "s");
        dbg_set_rgb(cr, v->vid.palette_rows[p] ? dbg_accent_line_rgb() : 0x808080, 1.0);
        dbg_draw_text(cr, wg, t, 4, y + 42, FALSE, 12);
        for (val = 0; val < 4; val++) {
            const int pen = chfdebug_palette_pen(p, val);
            const guint32 rgb = chfdebug_colour_rgb(pen) & 0xFFFFFF;
            const double x = PAL_LABEL_W + val * (PAL_SW_W + 6);
            const int lum = (int)(((rgb >> 16) & 0xFF) * 30 + ((rgb >> 8) & 0xFF) * 59 + (rgb & 0xFF) * 11) / 100;

            dbg_set_rgb(cr, rgb, 1.0);
            cairo_rectangle(cr, x, y, PAL_SW_W, 40);
            cairo_fill(cr);
            dbg_set_fg(cr, wg, 0.3);
            cairo_set_line_width(cr, 1);
            cairo_rectangle(cr, x + 0.5, y + 0.5, PAL_SW_W - 1, 39);
            cairo_stroke(cr);
            g_snprintf(t, sizeof t, "value %d", val);
            if (lum > 128) dbg_set_rgb(cr, 0x000000, 0.75); else dbg_set_rgb(cr, 0xFFFFFF, 0.9);
            dbg_draw_centred(cr, wg, t, x, y, PAL_SW_W, 22, FALSE);
            g_snprintf(t, sizeof t, "#%06X", rgb);
            dbg_draw_centred(cr, wg, t, x, y + 18, PAL_SW_W, 20, TRUE);
            dbg_set_fg(cr, wg, 0.75);
            dbg_draw_centred(cr, wg, chfdebug_colour_name(pen), x, y + 41, PAL_SW_W, 16, FALSE);
        }
    }
}

/* ---- the cell readout ------------------------------------------------------------------ */

static void cell_line(DbgVideo *v, const chfdebug_vram_cell *c, GString *s, gboolean full)
{
    g_string_append_printf(s, "row %d, col %d  (%s%s%s)", c->row, c->col,
                           c->visible ? "visible" : "hidden", c->hidden_why ? ": " : "",
                           c->hidden_why ? c->hidden_why : "");
    g_string_append_printf(s, "\nvalue %d   palette %d (%s)   pen %s   <span foreground=\"#%06x\" "
                           "background=\"#%06x\">  </span> #%06X", c->value, c->palette,
                           chfdebug_palette_name(c->palette), chfdebug_colour_name(c->pen),
                           c->rgb & 0xFFFFFF, c->rgb & 0xFFFFFF, c->rgb & 0xFFFFFF);
    if (!full)
        return;
    g_string_append_printf(s, "\nwritten %u time%s since the debugger attached", c->writes,
                           c->writes == 1 ? "" : "s");
    if (c->last_pc >= 0)
        g_string_append_printf(s, "\nlast write by PC $%04X, frame %u, line %d, clock %llu: %d \xe2\x86\x92 %d",
                               c->last_pc, c->last_frame, c->last_line,
                               (unsigned long long)c->last_cycle, c->last_old, c->value);
    else
        g_string_append(s, "\nnever written");
    (void)v;
}

static void update_labels(DbgVideo *v)
{
    chfdebug_vram_cell c;
    GString *s = g_string_new(NULL);

    if (v->hover_row >= 0 && chfdebug_vram_cell_get(v->dbg, v->hover_row, v->hover_col, &c) == 0) {
        g_string_append_printf(s, "row %d, col %d: value %d, palette %d, %s #%06X", c.row, c.col,
                               c.value, c.palette, chfdebug_colour_name(c.pen), c.rgb & 0xFFFFFF);
        if (!c.visible)
            g_string_append_printf(s, " (hidden: %s)", c.hidden_why ? c.hidden_why : "?");
    } else {
        g_string_append(s, "Point at a cell; click to pin it, double-click to break on writes to it");
    }
    gtk_label_set_text(v->hover_label, s->str);
    g_string_truncate(s, 0);
    if (v->pin_row >= 0 && chfdebug_vram_cell_get(v->dbg, v->pin_row, v->pin_col, &c) == 0) {
        cell_line(v, &c, s, TRUE);
        gtk_label_set_markup(v->pin_label, s->str);
    } else {
        gtk_label_set_text(v->pin_label, "No cell pinned");
    }
    g_string_free(s, TRUE);
}

static gboolean pic_cell(DbgVideo *v, double x, double y, int *row, int *col)
{
    return chfdebug_video_hit(v->dbg, view_now(v), (int)x, (int)y, scale_now(v), row, col) >= 0;
}

static void on_pic_motion(GtkEventControllerMotion *c, double x, double y, gpointer ud)
{
    DbgVideo *v = ud;
    int row = -1, col = -1;
    (void)c;
    if (!pic_cell(v, x, y, &row, &col))
        row = col = -1;
    if (row != v->hover_row || col != v->hover_col) {
        v->hover_row = row;
        v->hover_col = col;
        update_labels(v);
        gtk_widget_queue_draw(v->pic);
        gtk_widget_queue_draw(v->strip);
    }
}

static void on_pic_leave(GtkEventControllerMotion *c, gpointer ud)
{
    DbgVideo *v = ud;
    (void)c;
    v->hover_row = v->hover_col = -1;
    update_labels(v);
    gtk_widget_queue_draw(v->pic);
    gtk_widget_queue_draw(v->strip);
}

/* A click pins the cell; a double-click breaks on writes to it. */
static void on_pic_click(GtkGestureClick *g, int n, double x, double y, gpointer ud)
{
    DbgVideo *v = ud;
    int row, col;
    (void)g;
    if (!pic_cell(v, x, y, &row, &col))
        return;
    v->pin_row = row;
    v->pin_col = col;
    if (n >= 2) {
        const unsigned cell = (unsigned)(row * CHFDEBUG_VRAM_COLS + col);
        chfdebug_breakpoint_add(v->dbg, CHFDEBUG_BP_VRAM, CHF_SPACE_VRAM, (uint16_t)cell,
                                (uint16_t)cell, "");
        if (v->changed) v->changed(v->ud);
    }
    update_labels(v);
    gtk_widget_queue_draw(v->pic);
}

/* ---- refreshing the picture ----------------------------------------------------------- */

static void size_pic(DbgVideo *v, int w, int h)
{
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(v->pic), w);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(v->pic), h);
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(v->strip), STRIP_W);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(v->strip), h);
}

static void picture_update(DbgVideo *v)
{
    const int view = view_now(v), scale = scale_now(v);
    int w = 0, h = 0;
    gboolean ok;

    ok = chfdebug_video_view(v->dbg, view, overlay_mask(v), scale, v->buf, v->cap, &w, &h);
    if (!ok || w <= 0 || h <= 0 || (size_t)w * (size_t)h > v->cap) {
        if (v->have_pic) {
            v->have_pic = FALSE;
            gtk_widget_queue_draw(v->pic);
        }
        return;
    }
    if (v->have_pic && v->pw == w && v->ph == h &&
        memcmp(v->buf, v->prev, (size_t)w * (size_t)h * 4) == 0)
        return;
    memcpy(v->prev, v->buf, (size_t)w * (size_t)h * 4);
    if (v->pw != w || v->ph != h) {
        v->pw = w;
        v->ph = h;
        size_pic(v, w, h);
    }
    v->have_pic = TRUE;
    gtk_widget_queue_draw(v->pic);
}

static void on_overlay_toggled(GtkCheckButton *b, gpointer ud)
{
    (void)b;
    picture_update(ud);
}

static void on_view_changed(GObject *o, GParamSpec *ps, gpointer ud)
{
    DbgVideo *v = ud;
    const gboolean palettes = view_now(v) == CHFDEBUG_VIEW_PALETTES;
    int i;
    (void)o; (void)ps;
    v->have_pic = FALSE;
    v->hover_row = v->hover_col = -1;
    v->pin_row = v->pin_col = -1;
    for (i = 0; i < NOVERLAYS; i++)
        gtk_widget_set_sensitive(GTK_WIDGET(v->flag[i]), !palettes);
    picture_update(v);
    update_labels(v);
    gtk_widget_queue_draw(v->strip);
}

/* ---- the pages ----------------------------------------------------------------------- */

static void refresh_ports(DbgVideo *v)
{
    const chfdebug_video *d = &v->vid;
    static const char *const snd[4] = { "off", "1 kHz", "500 Hz", "120 Hz" };
    char text[1800], dec[4][96];
    int len = 0, i;
    const uint8_t raw[4] = { d->port0, d->port1, d->port4, d->port5 };
    const uint8_t rd[4] = { d->in0, d->in1, d->in4, d->in5 };
    const int num[4] = { 0, 1, 4, 5 };

    g_snprintf(dec[0], sizeof dec[0], "strobe %s, controllers %s, panel latch $%X",
               d->strobe ? "UP" : "down", d->controllers_enabled ? "read" : "off", d->panel_latch);
    g_snprintf(dec[1], sizeof dec[1], "colour value %d (the next strobe writes it)", d->value);
    g_snprintf(dec[2], sizeof dec[2], "column %d ($%02X)", d->col, d->col);
    g_snprintf(dec[3], sizeof dec[3], "row %d ($%02X), sound %s", d->row, d->row, snd[d->sound_mode & 3]);
    len += g_snprintf(text + len, sizeof text - len, "port  raw   ~raw  %-50s CPU reads now\n", "decoded");
    for (i = 0; i < 4; i++)
        len += g_snprintf(text + len, sizeof text - len, "  %d   $%02X   $%02X  %-50s $%02X\n",
                          num[i], raw[i], (uint8_t)~raw[i], dec[i], rd[i]);
    len += g_snprintf(text + len, sizeof text - len,
        "\nThe CPU writes a pixel by putting the colour (complemented) on port 1 bits 7-6,\n"
        "the column on port 4 bits 6-0 and the row on port 5 bits 5-0, then strobing\n"
        "port 0 bit 5. Port 0 bit 6 gates the hand controllers.\n\n");
    len += g_snprintf(text + len, sizeof text - len,
        "beam        line %d of %d, cycle %d of %d   frame %u   %s\n"
        "strobes     %u this frame so far, %u in the last whole one (%u changed a cell)\n"
        "palettes    rows by palette:  0:%d  1:%d  2:%d  3:%d\n",
        d->line, d->lines, d->cycle_in_line, d->cycles_per_line, d->frame,
        d->is_pal ? "PAL" : "NTSC", d->strobes_frame, d->strobes_last, d->changes_last,
        d->palette_rows[0], d->palette_rows[1], d->palette_rows[2], d->palette_rows[3]);
    gtk_label_set_text(v->ports_text, text);
}

static void refresh_writes(DbgVideo *v)
{
    static chfdebug_vram_write w[WRITES_MAX];
    chfdebug_vram_filter f;
    GString *s = g_string_new(NULL);
    char note[160];
    int n, i;

    f.row0 = gtk_spin_button_get_value_as_int(v->row0);
    f.row1 = gtk_spin_button_get_value_as_int(v->row1);
    f.col0 = gtk_spin_button_get_value_as_int(v->col0);
    f.col1 = gtk_spin_button_get_value_as_int(v->col1);
    f.changes_only = gtk_check_button_get_active(GTK_CHECK_BUTTON(v->f_changes));
    f.this_frame = gtk_check_button_get_active(GTK_CHECK_BUTTON(v->f_frame));
    f.palette_cols = gtk_check_button_get_active(GTK_CHECK_BUTTON(v->f_palcols));
    n = chfdebug_vram_writes(v->dbg, &f, w, WRITES_MAX);

    for (i = 0; i < n; i++)
        g_string_append_printf(s, "%7u  %4d  %8llu  $%04X  r%-2d c%-3d     %d \xe2\x86\x92 %d%s\n",
                               w[i].frame, w[i].line, (unsigned long long)w[i].cycle, w[i].pc,
                               w[i].row, w[i].col, w[i].old, w[i].value,
                               w[i].old == w[i].value ? "   (same)" : "");
    if (n == 0)
        g_string_append(s, "(no matching writes)\n");
    dbg_log_note_reader(v->log_text);
    dbg_set_text_follow(v->log_text, s->str, dbg_log_following(v->log_text));
    g_string_free(s, TRUE);
    g_snprintf(note, sizeof note, "%d shown   strobes: %u this frame, %u last frame (%u changed a cell)",
               n, v->vid.strobes_frame, v->vid.strobes_last, v->vid.changes_last);
    gtk_label_set_text(v->log_note, note);
}

static void on_filter_changed(gpointer w, gpointer ud)
{
    (void)w;
    refresh_writes(ud);
}

static void on_log_clear(GtkButton *b, gpointer ud)
{
    DbgVideo *v = ud;
    (void)b;
    chfdebug_vram_writes_clear(v->dbg);
    refresh_writes(v);
}

static void on_page_switched(GtkNotebook *nb, GtkWidget *page, guint num, gpointer ud)
{
    DbgVideo *v = ud;
    (void)nb; (void)page;
    if (num == SEC_WRITES) refresh_writes(v);
    else if (num == SEC_PORTS) refresh_ports(v);
    else gtk_widget_queue_draw(v->pal_area);
}

void dbg_video_refresh(DbgVideo *v, gboolean live)
{
    chfdebug_video_get(v->dbg, &v->vid);
    /* the palettes the rows had a frame ago, for the ticks */
    if (!v->have_pal) {
        memcpy(v->cur_pal, v->vid.row_palette, sizeof v->cur_pal);
        memcpy(v->prev_pal, v->cur_pal, sizeof v->prev_pal);
        v->pal_frame = v->vid.frame;
        v->have_pal = TRUE;
    } else if (v->vid.frame != v->pal_frame) {
        memcpy(v->prev_pal, v->cur_pal, sizeof v->prev_pal);
        memcpy(v->cur_pal, v->vid.row_palette, sizeof v->cur_pal);
        v->pal_frame = v->vid.frame;
    } else {
        memcpy(v->cur_pal, v->vid.row_palette, sizeof v->cur_pal);
    }
    picture_update(v);
    update_labels(v);
    gtk_widget_queue_draw(v->strip);
    switch (gtk_notebook_get_current_page(v->sections)) {
    case SEC_PALETTES: gtk_widget_queue_draw(v->pal_area); break;
    case SEC_PORTS: refresh_ports(v); break;
    case SEC_WRITES:
        /* a running machine's log is rebuilt twice a second, not ten times */
        if (!live || ++v->tick >= 5) {
            v->tick = 0;
            refresh_writes(v);
        }
        break;
    default: break;
    }
}

/* ---- construction -------------------------------------------------------------------- */

static GtkWidget *spin(int lo, int hi, int val, DbgVideo *v)
{
    GtkWidget *s = gtk_spin_button_new_with_range(lo, hi, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s), val);
    gtk_editable_set_width_chars(GTK_EDITABLE(s), 4);
    g_signal_connect(s, "value-changed", G_CALLBACK(on_filter_changed), v);
    return s;
}

static GtkWidget *build_writes(DbgVideo *v)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *r1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *r2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *clear = gtk_button_new_with_label("Clear");
    GtkWidget *scroll = dbg_mono_view(&v->log_text, FALSE);

    v->row0 = GTK_SPIN_BUTTON(spin(0, 63, 0, v));
    v->row1 = GTK_SPIN_BUTTON(spin(0, 63, 63, v));
    v->col0 = GTK_SPIN_BUTTON(spin(0, 127, 0, v));
    v->col1 = GTK_SPIN_BUTTON(spin(0, 127, 127, v));
    v->f_changes = gtk_check_button_new_with_label("Changes only");
    v->f_frame = gtk_check_button_new_with_label("This frame");
    v->f_palcols = gtk_check_button_new_with_label("Palette columns");
    g_signal_connect(v->f_changes, "toggled", G_CALLBACK(on_filter_changed), v);
    g_signal_connect(v->f_frame, "toggled", G_CALLBACK(on_filter_changed), v);
    g_signal_connect(v->f_palcols, "toggled", G_CALLBACK(on_filter_changed), v);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_log_clear), v);

    gtk_box_append(GTK_BOX(r1), dbg_labeled("Rows", GTK_WIDGET(v->row0)));
    gtk_box_append(GTK_BOX(r1), dbg_labeled("to", GTK_WIDGET(v->row1)));
    gtk_box_append(GTK_BOX(r1), dbg_labeled("Columns", GTK_WIDGET(v->col0)));
    gtk_box_append(GTK_BOX(r1), dbg_labeled("to", GTK_WIDGET(v->col1)));
    gtk_box_append(GTK_BOX(r2), v->f_changes);
    gtk_box_append(GTK_BOX(r2), v->f_frame);
    gtk_box_append(GTK_BOX(r2), v->f_palcols);
    gtk_box_append(GTK_BOX(r2), clear);
    v->log_note = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(v->log_note), "dim-label");
    gtk_label_set_xalign(v->log_note, 0);
    gtk_label_set_ellipsize(v->log_note, PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(v->log_note, 1);
    gtk_widget_set_hexpand(GTK_WIDGET(v->log_note), TRUE);
    dbg_log_make(v->log_text);

    gtk_box_append(GTK_BOX(box), r1);
    gtk_box_append(GTK_BOX(box), r2);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(v->log_note));
    {
        GtkWidget *head = gtk_label_new("  frame  line     clock   pc     cell       old \xe2\x86\x92 new");
        gtk_widget_add_css_class(head, "monospace");
        gtk_widget_add_css_class(head, "dim-label");
        gtk_label_set_xalign(GTK_LABEL(head), 0);
        gtk_box_append(GTK_BOX(box), head);
    }
    gtk_box_append(GTK_BOX(box), scroll);
    return dbg_padded(box);
}

static GtkWidget *build_ports(DbgVideo *v)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();

    v->ports_text = GTK_LABEL(dbg_mono_label());
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(v->ports_text));
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), dbg_padded(box));
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    return scroll;
}

static GtkWidget *build_palettes(DbgVideo *v)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    v->pal_area = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(v->pal_area),
                                       PAL_LABEL_W + 4 * (PAL_SW_W + 6));
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(v->pal_area), 4 * PAL_ROW_H + 8);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(v->pal_area), draw_palettes, v, NULL);
    gtk_widget_set_halign(v->pal_area, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(box), v->pal_area);
    gtk_box_append(GTK_BOX(box),
        dbg_hint("A row's palette is (column 126 & 2) | (column 125 >> 1): the value in each "
                 "palette column picks the backgrounds (0 black and white, 1 light blue, "
                 "2 light grey, 3 light green), and the other three values are blue, red and "
                 "green on all of them.", 46));
    return dbg_padded(box);
}

DbgVideo *dbg_video_new(chfsession *session, chfdebug *dbg, DbgChanged changed, gpointer ud,
                        GtkWidget **root)
{
    DbgVideo *v = g_new0(DbgVideo, 1);
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *flags = gtk_flow_box_new();
    GtkWidget *picrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *picscroll = gtk_scrolled_window_new();
    GtkWidget *readout = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkStringList *scales = gtk_string_list_new(NULL);
    GtkEventController *motion;
    GtkGesture *click;
    const char *sec;
    int i;

    v->session = session;
    v->dbg = dbg;
    v->changed = changed;
    v->ud = ud;
    v->hover_row = v->hover_col = v->pin_row = v->pin_col = -1;
    v->cap = (size_t)CHFDEBUG_VIEW_MAX_PIXELS * MAX_SCALE * MAX_SCALE;
    v->buf = g_new(uint32_t, v->cap);
    v->prev = g_new(uint32_t, v->cap);

    /* the picture's controls */
    v->view = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(k_view_names));
    for (i = 1; i <= MAX_SCALE; i++) {
        char t[8];
        g_snprintf(t, sizeof t, "%d\xc3\x97", i);
        gtk_string_list_append(scales, t);
    }
    v->scale = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(scales), NULL));
    gtk_drop_down_set_selected(v->scale, 1);
    gtk_box_append(GTK_BOX(controls), dbg_labeled("View", GTK_WIDGET(v->view)));
    gtk_box_append(GTK_BOX(controls), dbg_labeled("Scale", GTK_WIDGET(v->scale)));
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flags), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flags), 4);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(flags), 2);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flags), 10);
    for (i = 0; i < NOVERLAYS; i++) {
        v->flag[i] = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(k_overlays[i].name));
        gtk_check_button_set_active(v->flag[i], k_overlays[i].def);
        gtk_widget_set_tooltip_text(GTK_WIDGET(v->flag[i]), k_overlays[i].tip);
        g_signal_connect(v->flag[i], "toggled", G_CALLBACK(on_overlay_toggled), v);
        gtk_flow_box_append(GTK_FLOW_BOX(flags), GTK_WIDGET(v->flag[i]));
    }
    g_signal_connect(v->view, "notify::selected", G_CALLBACK(on_view_changed), v);
    g_signal_connect(v->scale, "notify::selected", G_CALLBACK(on_view_changed), v);

    /* the picture, with the row strip beside it */
    v->pic = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(v->pic), draw_pic, v, NULL);
    gtk_widget_set_halign(v->pic, GTK_ALIGN_START);
    gtk_widget_set_valign(v->pic, GTK_ALIGN_START);
    v->strip = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(v->strip), draw_strip, v, NULL);
    gtk_widget_set_halign(v->strip, GTK_ALIGN_START);
    gtk_widget_set_valign(v->strip, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text(v->strip,
        "Each VRAM row's palette (from columns 125 and 126): its background and number; "
        "a tick marks a row whose palette changed since the last frame");
    size_pic(v, 408, 464);
    motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_pic_motion), v);
    g_signal_connect(motion, "leave", G_CALLBACK(on_pic_leave), v);
    gtk_widget_add_controller(v->pic, motion);
    click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_pic_click), v);
    gtk_widget_add_controller(v->pic, GTK_EVENT_CONTROLLER(click));
    gtk_box_append(GTK_BOX(picrow), v->strip);
    gtk_box_append(GTK_BOX(picrow), v->pic);
    gtk_widget_set_margin_top(picrow, 4);
    gtk_widget_set_margin_start(picrow, 4);
    gtk_widget_set_margin_end(picrow, 4);
    gtk_widget_set_margin_bottom(picrow, 4);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(picscroll), picrow);
    gtk_widget_set_vexpand(picscroll, TRUE);

    /* the readouts under it */
    v->hover_label = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(v->hover_label, 0);
    gtk_label_set_ellipsize(v->hover_label, PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(v->hover_label, 1);
    gtk_widget_add_css_class(GTK_WIDGET(v->hover_label), "dim-label");
    v->pin_label = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(v->pin_label, 0);
    gtk_label_set_yalign(v->pin_label, 0);
    gtk_label_set_wrap(v->pin_label, TRUE);
    gtk_label_set_width_chars(v->pin_label, 50);
    gtk_label_set_max_width_chars(v->pin_label, 62);
    gtk_label_set_selectable(v->pin_label, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(v->pin_label), "monospace");
    gtk_box_append(GTK_BOX(readout), dbg_heading("Cell"));
    gtk_box_append(GTK_BOX(readout), GTK_WIDGET(v->hover_label));
    gtk_box_append(GTK_BOX(readout), GTK_WIDGET(v->pin_label));

    gtk_box_append(GTK_BOX(left), controls);
    gtk_box_append(GTK_BOX(left), flags);
    gtk_box_append(GTK_BOX(left), picscroll);
    gtk_box_append(GTK_BOX(left), readout);
    gtk_widget_set_size_request(left, 540, -1);

    /* the pages */
    v->sections = GTK_NOTEBOOK(gtk_notebook_new());
    gtk_notebook_append_page(v->sections, build_palettes(v), gtk_label_new("Palettes"));
    gtk_notebook_append_page(v->sections, build_ports(v), gtk_label_new("Ports"));
    gtk_notebook_append_page(v->sections, build_writes(v), gtk_label_new("VRAM writes"));
    gtk_widget_set_vexpand(GTK_WIDGET(v->sections), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(v->sections), TRUE);
    gtk_box_append(GTK_BOX(right), GTK_WIDGET(v->sections));
    gtk_widget_set_hexpand(right, TRUE);

    /* CHF_DEBUGGER_SECTION opens a page: palettes, ports or writes */
    sec = g_getenv("CHF_DEBUGGER_SECTION");
    if (sec && *sec) {
        if (g_ascii_strncasecmp(sec, "ports", strlen(sec)) == 0)
            gtk_notebook_set_current_page(v->sections, SEC_PORTS);
        else if (g_ascii_strncasecmp(sec, "writes", strlen(sec)) == 0 ||
                 g_ascii_strncasecmp(sec, "log", strlen(sec)) == 0)
            gtk_notebook_set_current_page(v->sections, SEC_WRITES);
    }
    g_signal_connect(v->sections, "switch-page", G_CALLBACK(on_page_switched), v);

    gtk_box_append(GTK_BOX(outer), left);
    gtk_box_append(GTK_BOX(outer), right);
    g_object_unref(scales);
    *root = dbg_padded(outer);
    return v;
}

void dbg_video_free(DbgVideo *v)
{
    if (!v) return;
    g_free(v->buf);
    g_free(v->prev);
    g_free(v);
}
