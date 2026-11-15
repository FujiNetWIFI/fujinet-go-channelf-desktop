/*
 * The debugger's Video tab (GTK4/cairo): the Channel F's 128 x 64 video RAM,
 * its row palettes and its four I/O ports.
 *
 * Everything that decides what to show lives in the core (chfvidtab.h): this
 * file is widgets. The picture is drawn from the controller's cached buffer
 * (0xFFRRGGBB, drawn as CAIRO_FORMAT_RGB24 with a nearest filter), so a mouse
 * move redraws only the marks. Only the section showing is filled in.
 *
 *   left   the view (Display, Decoded, Beam, Values, Live), the zoom, the
 *          overlays, Break on writes, and the picture; hover inspects a cell,
 *          a click pins it, a double-click breaks on writes to it.
 *   right  six sections: Pixel (the inspected cell), Registers (ports 0, 1, 4
 *          and 5 raw, complemented and decoded), Colours (the four palettes),
 *          Lines (the 64 VRAM rows), Writes (the filtered VRAM write log) and
 *          Timing.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_common.h"

#include <string.h>

#include "chfvidtab.h"

#define NSWATCH 16          /* four palettes of four values */
#define MAX_OVERLAYS 16
#define ACCENT_TEXT "#000000"

typedef struct {
    chfvid_tab *tab;
    GtkDropDown *view, *zoom;
    GtkWidget *brk_top, *brk_pix;
    GtkCheckButton *ov[MAX_OVERLAYS];
    int nov;
    GtkLabel *status, *note;
    GtkDrawingArea *pic;
    GtkWidget *pic_scroll;
    int vp_w, vp_h;
    GtkNotebook *sections;
    GtkLabel *pixel_text, *pixel_note;
    GtkTextView *regs_text, *lines_text, *writes_text;
    char *regs_sig, *lines_sig, *writes_sig;
    GtkCheckButton *wf[5];
    gboolean wf_sync;
    GtkDrawingArea *swatch_box[NSWATCH];
    GtkLabel *swatch_lbl[NSWATCH];
    GtkLabel *timing_text;
    GtkProgressBar *beam;
    gboolean updating, primed, was_mapped;
} Vid;

static GtkWidget *mono_label(void)
{
    GtkWidget *l = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_widget_add_css_class(l, "monospace");
    return l;
}

static GtkWidget *padded_box(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    return dbg_padded(box);
}

static void set_rgb(cairo_t *cr, uint32_t rgb)
{
    cairo_set_source_rgb(cr, ((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0, (rgb & 255) / 255.0);
}

/* ---- the picture ---------------------------------------------------------- */

static void draw_message(cairo_t *cr, int w, int h, const char *msg)
{
    cairo_text_extents_t ext;
    cairo_set_source_rgb(cr, 0.65, 0.65, 0.68);
    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 14);
    cairo_text_extents(cr, msg, &ext);
    cairo_move_to(cr, (w - ext.width) / 2 - ext.x_bearing, (h - ext.height) / 2 - ext.y_bearing);
    cairo_show_text(cr, msg);
    cairo_new_path(cr);
}

/* A cell can be a few device pixels wide: outline it from outside, dark under light. */
static void draw_mark(cairo_t *cr, const int r[4], double red, double green, double blue)
{
    cairo_set_line_width(cr, 3);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.85);
    cairo_rectangle(cr, r[0] - 1.5, r[1] - 1.5, r[2] + 3, r[3] + 3);
    cairo_stroke(cr);
    cairo_set_line_width(cr, 1);
    cairo_set_source_rgb(cr, red, green, blue);
    cairo_rectangle(cr, r[0] - 1.5, r[1] - 1.5, r[2] + 3, r[3] + 3);
    cairo_stroke(cr);
}

static void draw_pic(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer ud)
{
    Vid *v = ud;
    int pw, ph, marks, hover[4] = { 0 }, pin[4] = { 0 };
    const uint32_t *px;
    (void)area;
    cairo_set_source_rgb(cr, 0.07, 0.07, 0.09);
    cairo_paint(cr);
    px = chfvid_tab_picture(v->tab, &pw, &ph);
    if (!px) {
        const char *why = chfvid_tab_why(v->tab);
        draw_message(cr, w, h, why && *why ? why : "Nothing to show");
        return;
    }
    {
        cairo_surface_t *s = cairo_image_surface_create_for_data((unsigned char *)px, CAIRO_FORMAT_RGB24,
                                                                 pw, ph, pw * 4);
        cairo_set_source_surface(cr, s, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
        cairo_paint(cr);
        cairo_surface_destroy(s);
    }
    marks = chfvid_tab_marks(v->tab, hover, pin);
    if (marks & 1) draw_mark(cr, hover, 1, 1, 1);
    if (marks & 2) draw_mark(cr, pin, 1.0, 0.85, 0.1);
}

static void update_pixel_section(Vid *v)
{
    const chfvid_pixel *p = chfvid_tab_pixel(v->tab);
    const gboolean can = chfvid_tab_can_break(v->tab) != 0;
    int index;
    gtk_widget_set_sensitive(v->brk_top, can);
    gtk_widget_set_sensitive(v->brk_pix, can);
    gtk_label_set_text(v->pixel_text, p ? p->text
        : "Move over the picture to inspect a cell.\nClick to pin it (click again to unpin);\n"
          "double-click to break on writes to it.");
    for (index = 0; index < NSWATCH; index++)     /* the swatch the inspected cell uses is outlined */
        gtk_widget_queue_draw(GTK_WIDGET(v->swatch_box[index]));
}

static void do_break_pixel(Vid *v)
{
    const chfvid_pixel *p = chfvid_tab_pixel(v->tab);
    int id = chfvid_tab_break_on_pixel(v->tab);
    char buf[160];
    if (id >= 0 && p)
        g_snprintf(buf, sizeof buf, "Break on writes to row %d col %d", p->row, p->col);
    else
        g_snprintf(buf, sizeof buf, "Nothing to break on (inspect a cell first)");
    gtk_label_set_text(v->note, buf);
    gtk_label_set_text(v->pixel_note, buf);
}

static void on_motion(GtkEventControllerMotion *c, double x, double y, gpointer ud)
{
    Vid *v = ud;
    (void)c;
    if (chfvid_tab_hover(v->tab, (int)x, (int)y)) {
        gtk_widget_queue_draw(GTK_WIDGET(v->pic));
        update_pixel_section(v);
    }
}

static void on_leave(GtkEventControllerMotion *c, gpointer ud)
{
    Vid *v = ud;
    (void)c;
    if (chfvid_tab_hover(v->tab, -1, -1)) {
        gtk_widget_queue_draw(GTK_WIDGET(v->pic));
        update_pixel_section(v);
    }
}

static void on_click(GtkGestureClick *g, int n, double x, double y, gpointer ud)
{
    Vid *v = ud;
    (void)g;
    if (n == 1) {
        chfvid_tab_pin(v->tab, (int)x, (int)y);
    } else if (n == 2) {
        /* the first click of the pair toggled the pin; the second restores it */
        chfvid_tab_hover(v->tab, (int)x, (int)y);
        chfvid_tab_pin(v->tab, (int)x, (int)y);
        do_break_pixel(v);
    }
    gtk_widget_queue_draw(GTK_WIDGET(v->pic));
    update_pixel_section(v);
}

/* ---- text tables ------------------------------------------------------------- */

/* Replace a text view's contents when they (or the highlighted rows) changed;
 * the scroll position stays put. hl[i] == '1' highlights line i. */
static void put_table(GtkTextView *tv, char **sigp, GString *text, const GString *hl)
{
    GString *sig = g_string_new(NULL);
    g_string_append_len(sig, hl->str, (gssize)hl->len);
    g_string_append_c(sig, '|');
    g_string_append_len(sig, text->str, (gssize)text->len);
    if (!*sigp || strcmp(*sigp, sig->str)) {
        GtkTextBuffer *b = gtk_text_view_get_buffer(tv);
        GtkAdjustment *adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(tv));
        double pos = adj ? gtk_adjustment_get_value(adj) : 0;
        char *clean = g_utf8_make_valid(text->str, -1);
        gsize i;
        gtk_text_buffer_set_text(b, clean, -1);
        g_free(clean);
        for (i = 0; i < hl->len; i++) {
            if (hl->str[i] == '1') {
                GtkTextIter a, e;
                gtk_text_buffer_get_iter_at_line(b, &a, (int)i);
                e = a;
                gtk_text_iter_forward_to_line_end(&e);
                gtk_text_buffer_apply_tag_by_name(b, "changed", &a, &e);
            }
        }
        if (adj) gtk_adjustment_set_value(adj, pos);
        g_free(*sigp);
        *sigp = g_strdup(sig->str);
    }
    g_string_free(sig, TRUE);
}

static void fill_regs(Vid *v)
{
    const chfvid_reg *r;
    int n = chfvid_tab_regs(v->tab, &r), i;
    GString *s = g_string_new(NULL), *hl = g_string_new(NULL);
    g_string_append(s, "Register  Raw  ~Raw  Value                   Last write            Meaning\n");
    g_string_append_c(hl, '0');
    for (i = 0; i < n; i++) {
        char raw[8] = "", comp[8] = "";
        if (r[i].port >= 0) {
            g_snprintf(raw, sizeof raw, "$%02X", r[i].raw);
            g_snprintf(comp, sizeof comp, "$%02X", r[i].comp);
        }
        g_string_append_printf(s, "%-9s %-4s %-5s %-23s %-21s %s\n", r[i].name, raw, comp, r[i].value,
                               r[i].last, r[i].meaning);
        g_string_append_c(hl, r[i].age < CHFVID_REG_FADE ? '1' : '0');
    }
    if (!n) g_string_append(s, "The machine is not running.\n");
    put_table(v->regs_text, &v->regs_sig, s, hl);
    g_string_free(s, TRUE);
    g_string_free(hl, TRUE);
}

static void fill_lines(Vid *v)
{
    const chfvid_line_row *r;
    int n = chfvid_tab_lines(v->tab, &r), i;
    GString *s = g_string_new(NULL), *hl = g_string_new(NULL);
    g_string_append(s, "Row  Line  Pal  Background  Writes  PalW  Last writer      Notes\n");
    g_string_append_c(hl, '0');
    for (i = 0; i < n; i++) {
        char writer[40] = "-";
        if (r[i].last_pc >= 0)
            g_snprintf(writer, sizeof writer, "$%04X col %d f%u", r[i].last_pc, r[i].last_col,
                       r[i].last_frame);
        g_string_append_printf(s, "%3d  %4d  %3d  #%06X    %6d  %4d  %-16s %s%s\n", r[i].row,
                               r[i].scanline, r[i].palette, r[i].background & 0xFFFFFF, r[i].writes,
                               r[i].pal_writes, writer, r[i].visible ? "" : "hidden ",
                               r[i].beam ? "<- beam" : "");
        g_string_append_c(hl, (r[i].beam || r[i].pal_writes > 0) ? '1' : '0');
    }
    if (!n) g_string_append(s, "The machine is not running.\n");
    put_table(v->lines_text, &v->lines_sig, s, hl);
    g_string_free(s, TRUE);
    g_string_free(hl, TRUE);
}

static void fill_writes(Vid *v)
{
    const chfdebug_vram_write *w;
    int n = chfvid_tab_writes(v->tab, &w), i;
    GString *s = g_string_new(NULL), *hl = g_string_new(NULL);
    g_string_append_c(hl, '0');
    for (i = 0; i < n; i++) {
        char line[200];
        chfvid_write_text(&w[i], line, sizeof line);
        g_string_append(s, line);
        g_string_append_c(s, '\n');
        g_string_append_c(hl, w[i].value != w[i].old ? '1' : '0');
    }
    if (!n) g_string_append(s, "(no VRAM writes match since the debugger attached)\n");
    else g_string_prepend(s, "Click a line to inspect its cell.\n");
    put_table(v->writes_text, &v->writes_sig, s, hl);
    g_string_free(s, TRUE);
    g_string_free(hl, TRUE);
}

/* A click on a Writes line pins the cell it wrote; on a Lines line, the row's
 * first visible cell. */
static void on_table_click(GtkGestureClick *g, int n, double x, double y, gpointer ud)
{
    Vid *v = ud;
    GtkTextView *tv = GTK_TEXT_VIEW(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g)));
    GtkTextIter it;
    int bx, by, line;
    (void)n;
    gtk_text_view_window_to_buffer_coords(tv, GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &bx, &by);
    gtk_text_view_get_line_at_y(tv, &it, by, NULL);
    line = gtk_text_iter_get_line(&it);
    if (tv == v->writes_text) {
        const chfdebug_vram_write *w;
        const int count = chfvid_tab_writes(v->tab, &w);
        const int index = line - 1;                 /* the hint line comes first */
        if (index < 0 || index >= count) return;
        chfvid_tab_pin_cell(v->tab, w[index].row, w[index].col);
    } else {
        const int row = line - 1;                   /* the header comes first */
        if (row < 0 || row >= CHFDEBUG_VRAM_ROWS) return;
        chfvid_tab_pin_cell(v->tab, row, CHFDEBUG_VIS_COL0);
    }
    gtk_widget_queue_draw(GTK_WIDGET(v->pic));
    update_pixel_section(v);
}

/* ---- colours ------------------------------------------------------------------ */

static void draw_swatch(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer ud)
{
    Vid *v = ud;
    const chfvid_swatch *sw;
    int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(area), "idx"));
    (void)w; (void)h;
    if (chfvid_tab_swatches(v->tab, &sw) <= index) return;
    set_rgb(cr, sw[index].rgb);
    cairo_rectangle(cr, 0, 0, 22, 22);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.8);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, 0.5, 0.5, 21, 21);
    cairo_stroke(cr);
    if (sw[index].inspected) {      /* the inspected cell uses this colour */
        cairo_set_source_rgb(cr, 1, 0.85, 0.1);
        cairo_set_line_width(cr, 2);
        cairo_rectangle(cr, 1, 1, 20, 20);
        cairo_stroke(cr);
    }
}

static void fill_colours(Vid *v)
{
    const chfvid_swatch *sw;
    int n = chfvid_tab_swatches(v->tab, &sw), index;
    for (index = 0; index < NSWATCH; index++) {
        char buf[160];
        if (index < n)
            g_snprintf(buf, sizeof buf, "%-14s v%d %-11s #%06X  %d rows, %ld cells",
                       sw[index].value == 0 ? sw[index].palette_name : "", sw[index].value,
                       sw[index].name, sw[index].rgb & 0xFFFFFF, sw[index].rows, sw[index].cells);
        else
            g_snprintf(buf, sizeof buf, "-");
        gtk_label_set_text(v->swatch_lbl[index], buf);
        gtk_widget_queue_draw(GTK_WIDGET(v->swatch_box[index]));
    }
}

static void fill_timing(Vid *v)
{
    const char *const *lines;
    int n = chfvid_tab_timing(v->tab, &lines), i;
    GString *s = g_string_new(NULL);
    for (i = 0; i < n; i++) {
        const char *tab = strchr(lines[i], '\t');
        if (tab) g_string_append_printf(s, "%-18.*s %s\n", (int)(tab - lines[i]), lines[i], tab + 1);
        else g_string_append_printf(s, "%s\n", lines[i]);
    }
    gtk_label_set_text(v->timing_text, s->len ? s->str : "The machine is not running.");
    g_string_free(s, TRUE);
    gtk_progress_bar_set_fraction(v->beam, chfvid_tab_beam_permille(v->tab) / 1000.0);
}

/* ---- refresh ------------------------------------------------------------------- */

static void fill_section(Vid *v, int sec)
{
    switch (sec) {
    case CHFVID_S_REGISTERS: fill_regs(v); break;
    case CHFVID_S_COLOURS: fill_colours(v); break;
    case CHFVID_S_LINES: fill_lines(v); break;
    case CHFVID_S_WRITES: fill_writes(v); break;
    case CHFVID_S_TIMING: fill_timing(v); break;
    default: update_pixel_section(v); break;
    }
}

static void refresh_view(Vid *v, unsigned what)
{
    if (what & (CHFVID_TAB_PICTURE | CHFVID_TAB_OFF)) {
        int pw = 0, ph = 0;
        if (chfvid_tab_picture(v->tab, &pw, &ph)) {
            gtk_drawing_area_set_content_width(v->pic, pw);
            gtk_drawing_area_set_content_height(v->pic, ph);
        } else {
            gtk_drawing_area_set_content_width(v->pic, v->vp_w > 4 ? v->vp_w : 480);
            gtk_drawing_area_set_content_height(v->pic, v->vp_h > 4 ? v->vp_h : 300);
        }
        gtk_widget_queue_draw(GTK_WIDGET(v->pic));
    }
    if (what & CHFVID_TAB_TABLES) {
        gtk_label_set_text(v->status, chfvid_tab_status(v->tab));
        fill_section(v, gtk_notebook_get_current_page(v->sections));
        if (gtk_notebook_get_current_page(v->sections) != CHFVID_S_PIXEL)
            update_pixel_section(v);    /* the Break on writes buttons follow the cell */
    }
}

static void sync_viewport(Vid *v)
{
    int w = gtk_widget_get_width(v->pic_scroll), h = gtk_widget_get_height(v->pic_scroll);
    if (w > 4 && h > 4 && (w != v->vp_w || h != v->vp_h)) {
        v->vp_w = w;
        v->vp_h = h;
        chfvid_tab_set_viewport(v->tab, w - 2, h - 2);
    }
}

void dbg_video_refresh(GtkWidget *page, gboolean force)
{
    Vid *v = g_object_get_data(G_OBJECT(page), "vid");
    gboolean mapped;
    unsigned what;
    if (!v) return;
    mapped = gtk_widget_get_mapped(page);
    if (mapped && !v->was_mapped) force = TRUE;      /* just shown */
    v->was_mapped = mapped;
    if (!v->primed) force = TRUE;
    v->primed = TRUE;
    sync_viewport(v);
    what = chfvid_tab_refresh(v->tab, force);
    if (force) what |= CHFVID_TAB_PICTURE | CHFVID_TAB_TABLES;
    refresh_view(v, what);
}

/* The picture's room changed (window resized, split dragged): Fit follows it. */
static gboolean on_tick(GtkWidget *w, GdkFrameClock *fc, gpointer ud)
{
    Vid *v = ud;
    int pw = gtk_widget_get_width(w), ph = gtk_widget_get_height(w);
    (void)fc;
    if (pw > 4 && ph > 4 && (pw != v->vp_w || ph != v->vp_h)) {
        sync_viewport(v);
        if (v->primed && chfvid_tab_zoom(v->tab) == 0)
            refresh_view(v, chfvid_tab_refresh(v->tab, TRUE) | CHFVID_TAB_PICTURE);
    }
    return G_SOURCE_CONTINUE;
}

/* ---- controls ------------------------------------------------------------------- */

static void refresh_all(Vid *v)
{
    refresh_view(v, chfvid_tab_refresh(v->tab, TRUE) | CHFVID_TAB_PICTURE | CHFVID_TAB_TABLES);
}

static void on_view(GObject *o, GParamSpec *p, gpointer ud)
{
    Vid *v = ud;
    (void)o; (void)p;
    if (v->updating) return;
    chfvid_tab_set_view(v->tab, (int)gtk_drop_down_get_selected(v->view));
    refresh_all(v);
}

static void on_zoom(GObject *o, GParamSpec *p, gpointer ud)
{
    Vid *v = ud;
    int n;
    const int *z = chfvid_zooms(&n);
    guint i = gtk_drop_down_get_selected(v->zoom);
    (void)o; (void)p;
    if (v->updating || (int)i >= n) return;
    sync_viewport(v);
    chfvid_tab_set_zoom(v->tab, z[i]);
    refresh_all(v);
}

static void on_overlay(GtkCheckButton *b, gpointer ud)
{
    Vid *v = ud;
    unsigned m = 0;
    int n, i;
    const chfvid_overlay_info *d = chfvid_overlays(&n);
    (void)b;
    if (v->updating) return;
    for (i = 0; i < n && i < v->nov; i++)
        if (gtk_check_button_get_active(v->ov[i])) m |= d[i].bit;
    chfvid_tab_set_overlays(v->tab, m);
    refresh_all(v);
}

static void on_unpin(GtkButton *b, gpointer ud)
{
    Vid *v = ud;
    (void)b;
    chfvid_tab_unpin(v->tab);
    gtk_widget_queue_draw(GTK_WIDGET(v->pic));
    update_pixel_section(v);
}

static void on_break(GtkButton *b, gpointer ud)
{
    (void)b;
    do_break_pixel(ud);
}

static void on_write_filter(GtkCheckButton *b, gpointer ud)
{
    Vid *v = ud;
    chfvid_write_filter f;
    (void)b;
    if (v->wf_sync) return;
    f.this_frame = gtk_check_button_get_active(v->wf[0]);
    f.changes_only = gtk_check_button_get_active(v->wf[1]);
    f.palette_cols = gtk_check_button_get_active(v->wf[2]);
    f.inspected_cell = gtk_check_button_get_active(v->wf[3]);
    f.inspected_row = gtk_check_button_get_active(v->wf[4]);
    chfvid_tab_set_write_filter(v->tab, &f);
    refresh_all(v);
}

static void on_write_clear(GtkButton *b, gpointer ud)
{
    Vid *v = ud;
    (void)b;
    chfvid_tab_clear_writes(v->tab);
    refresh_all(v);
}

static void on_section(GtkNotebook *nb, GtkWidget *page, guint n, gpointer ud)
{
    Vid *v = ud;
    (void)nb; (void)page;
    fill_section(v, (int)n);
}

static void on_destroy(GtkWidget *w, gpointer ud)
{
    Vid *v = ud;
    (void)w;
    chfvid_tab_free(v->tab);
    g_free(v->regs_sig);
    g_free(v->lines_sig);
    g_free(v->writes_sig);
    g_free(v);
}

/* A read-only monospace table in a scroller, with the "changed" rows tagged. */
static GtkWidget *table_view(GtkTextView **out, gboolean wrap)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkTextView *tv = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(tv, FALSE);
    gtk_text_view_set_cursor_visible(tv, FALSE);
    gtk_text_view_set_monospace(tv, TRUE);
    gtk_text_view_set_wrap_mode(tv, wrap ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
    gtk_text_buffer_create_tag(gtk_text_view_get_buffer(tv), "changed", "background",
                               "#F2C300", "foreground", ACCENT_TEXT, "weight", 700, NULL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(tv));
    *out = tv;
    return scroll;
}

GtkWidget *dbg_video_new(chfdebug *dbg)
{
    Vid *v = g_new0(Vid, 1);
    GtkWidget *root, *left, *bar, *right, *box, *btns, *b1, *b2, *scroll, *ovbtn, *pop, *ovbox, *grid, *brk;
    GtkEventController *motion;
    GtkGesture *click;
    chfvid_write_filter wfilter;
    const chfvid_overlay_info *od;
    const int *zv;
    const char *views[CHFVID_V_COUNT + 1], *zlabels[24];
    static const char *const wf_text[5] = { "This frame", "Changes only", "Palette columns",
                                            "Inspected cell", "Inspected row" };
    char zbuf[24][8];
    int n, nz, i;

    v->tab = chfvid_tab_new(dbg);

    root = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    g_object_set_data(G_OBJECT(root), "vid", v);
    g_signal_connect(root, "destroy", G_CALLBACK(on_destroy), v);

    /* ---- left: toolbar, status, picture */
    left = padded_box();
    bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    for (i = 0; i < CHFVID_V_COUNT; i++) views[i] = chfvid_view_name(i);
    views[CHFVID_V_COUNT] = NULL;
    v->view = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(views));
    gtk_widget_set_tooltip_text(GTK_WIDGET(v->view),
        "Display: the last frame the console finished, as the television shows it.\n"
        "Decoded: the whole VRAM, hidden cells hatched, each row in its palette.\n"
        "Beam: what a CRT shows: rows the beam has passed are this frame's, the rest last frame's.\n"
        "Values: the whole VRAM's raw 0-3 values as flat false colours.\n"
        "Live: the VRAM as it is now, drawn like Display.");
    zv = chfvid_zooms(&nz);
    if (nz > 23) nz = 23;
    for (i = 0; i < nz; i++) {
        if (zv[i] == 0) g_strlcpy(zbuf[i], "Fit", sizeof zbuf[i]);
        else g_snprintf(zbuf[i], sizeof zbuf[i], "%d\xc3\x97", zv[i]);
        zlabels[i] = zbuf[i];
    }
    zlabels[nz] = NULL;
    v->zoom = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(zlabels));
    gtk_box_append(GTK_BOX(bar), gtk_label_new("View"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(v->view));
    gtk_box_append(GTK_BOX(bar), gtk_label_new("Zoom"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(v->zoom));

    od = chfvid_overlays(&n);
    if (n > MAX_OVERLAYS) n = MAX_OVERLAYS;
    v->nov = n;
    ovbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    for (i = 0; i < n; i++) {
        v->ov[i] = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(od[i].label));
        g_signal_connect(v->ov[i], "toggled", G_CALLBACK(on_overlay), v);
        gtk_box_append(GTK_BOX(ovbox), GTK_WIDGET(v->ov[i]));
    }
    pop = gtk_popover_new();
    gtk_popover_set_child(GTK_POPOVER(pop), ovbox);
    ovbtn = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(ovbtn), "Overlays");
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(ovbtn), pop);
    gtk_box_append(GTK_BOX(bar), ovbtn);

    brk = gtk_button_new_with_label("Break on writes");
    gtk_widget_set_tooltip_text(brk, "Break when the CPU writes the inspected VRAM cell");
    v->brk_top = brk;
    g_signal_connect(brk, "clicked", G_CALLBACK(on_break), v);
    gtk_box_append(GTK_BOX(bar), brk);
    gtk_box_append(GTK_BOX(left), bar);

    v->status = GTK_LABEL(mono_label());
    gtk_label_set_selectable(v->status, FALSE);
    gtk_box_append(GTK_BOX(left), GTK_WIDGET(v->status));

    v->pic = GTK_DRAWING_AREA(gtk_drawing_area_new());
    gtk_drawing_area_set_draw_func(v->pic, draw_pic, v, NULL);
    gtk_drawing_area_set_content_width(v->pic, 480);
    gtk_drawing_area_set_content_height(v->pic, 300);
    gtk_widget_set_halign(GTK_WIDGET(v->pic), GTK_ALIGN_START);
    gtk_widget_set_valign(GTK_WIDGET(v->pic), GTK_ALIGN_START);
    motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), v);
    g_signal_connect(motion, "leave", G_CALLBACK(on_leave), v);
    gtk_widget_add_controller(GTK_WIDGET(v->pic), motion);
    click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_click), v);
    gtk_widget_add_controller(GTK_WIDGET(v->pic), GTK_EVENT_CONTROLLER(click));
    scroll = gtk_scrolled_window_new();
    v->pic_scroll = scroll;
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(v->pic));
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_add_tick_callback(scroll, on_tick, v, NULL);
    gtk_box_append(GTK_BOX(left), scroll);

    v->note = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(v->note, 0);
    gtk_label_set_wrap(v->note, TRUE);
    gtk_box_append(GTK_BOX(left), GTK_WIDGET(v->note));

    /* ---- right: the six sections, in chfvid_section_name order */
    right = gtk_notebook_new();
    v->sections = GTK_NOTEBOOK(right);
    gtk_widget_set_size_request(right, 500, -1);

    /* Pixel */
    box = padded_box();
    v->pixel_text = GTK_LABEL(mono_label());
    gtk_label_set_wrap(v->pixel_text, TRUE);
    gtk_label_set_wrap_mode(v->pixel_text, PANGO_WRAP_WORD_CHAR);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(v->pixel_text));
    v->pixel_note = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(v->pixel_note, 0);
    gtk_label_set_wrap(v->pixel_note, TRUE);
    gtk_label_set_wrap_mode(v->pixel_note, PANGO_WRAP_WORD_CHAR);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(v->pixel_note));
    btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    b1 = gtk_button_new_with_label("Break on writes");
    v->brk_pix = b1;
    b2 = gtk_button_new_with_label("Unpin");
    g_signal_connect(b1, "clicked", G_CALLBACK(on_break), v);
    g_signal_connect(b2, "clicked", G_CALLBACK(on_unpin), v);
    gtk_box_append(GTK_BOX(btns), b1);
    gtk_box_append(GTK_BOX(btns), b2);
    gtk_box_append(GTK_BOX(box), btns);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    gtk_notebook_append_page(v->sections, scroll, gtk_label_new(chfvid_section_name(CHFVID_S_PIXEL)));

    /* Registers */
    scroll = table_view(&v->regs_text, TRUE);
    gtk_notebook_append_page(v->sections, scroll, gtk_label_new(chfvid_section_name(CHFVID_S_REGISTERS)));

    /* Colours: the four palettes of four */
    box = padded_box();
    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 3);
    for (i = 0; i < NSWATCH; i++) {
        v->swatch_box[i] = GTK_DRAWING_AREA(gtk_drawing_area_new());
        gtk_drawing_area_set_content_width(v->swatch_box[i], 22);
        gtk_drawing_area_set_content_height(v->swatch_box[i], 22);
        g_object_set_data(G_OBJECT(v->swatch_box[i]), "idx", GINT_TO_POINTER(i));
        gtk_drawing_area_set_draw_func(v->swatch_box[i], draw_swatch, v, NULL);
        v->swatch_lbl[i] = GTK_LABEL(mono_label());
        gtk_label_set_selectable(v->swatch_lbl[i], FALSE);
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(v->swatch_box[i]), 0, i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(v->swatch_lbl[i]), 1, i, 1, 1);
    }
    gtk_box_append(GTK_BOX(box), grid);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    gtk_notebook_append_page(v->sections, scroll, gtk_label_new(chfvid_section_name(CHFVID_S_COLOURS)));

    /* Lines */
    scroll = table_view(&v->lines_text, FALSE);
    click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_table_click), v);
    gtk_widget_add_controller(GTK_WIDGET(v->lines_text), GTK_EVENT_CONTROLLER(click));
    gtk_notebook_append_page(v->sections, scroll, gtk_label_new(chfvid_section_name(CHFVID_S_LINES)));

    /* Writes: the filter over the log */
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    {
        GtkWidget *filters = gtk_flow_box_new();
        GtkWidget *clear = gtk_button_new_with_label("Clear Log");
        gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(filters), GTK_SELECTION_NONE);
        gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(filters), 3);
        for (i = 0; i < 5; i++) {
            v->wf[i] = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(wf_text[i]));
            g_signal_connect(v->wf[i], "toggled", G_CALLBACK(on_write_filter), v);
            gtk_flow_box_append(GTK_FLOW_BOX(filters), GTK_WIDGET(v->wf[i]));
        }
        g_signal_connect(clear, "clicked", G_CALLBACK(on_write_clear), v);
        gtk_widget_set_halign(clear, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(box), filters);
        gtk_box_append(GTK_BOX(box), clear);
    }
    scroll = table_view(&v->writes_text, FALSE);
    gtk_widget_set_vexpand(scroll, TRUE);
    click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_table_click), v);
    gtk_widget_add_controller(GTK_WIDGET(v->writes_text), GTK_EVENT_CONTROLLER(click));
    gtk_box_append(GTK_BOX(box), scroll);
    gtk_notebook_append_page(v->sections, dbg_padded(box), gtk_label_new(chfvid_section_name(CHFVID_S_WRITES)));

    /* Timing */
    box = padded_box();
    v->beam = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_progress_bar_set_text(v->beam, "beam");
    gtk_progress_bar_set_show_text(v->beam, TRUE);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(v->beam));
    v->timing_text = GTK_LABEL(mono_label());
    gtk_label_set_selectable(v->timing_text, FALSE);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(v->timing_text));
    gtk_notebook_append_page(v->sections, box, gtk_label_new(chfvid_section_name(CHFVID_S_TIMING)));

    gtk_paned_set_start_child(GTK_PANED(root), left);
    gtk_paned_set_end_child(GTK_PANED(root), right);
    gtk_paned_set_resize_start_child(GTK_PANED(root), TRUE);
    gtk_paned_set_shrink_start_child(GTK_PANED(root), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(root), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(root), FALSE);

    /* start from the controller's state (the CHF_DEBUGGER_VIDEO_* variables) */
    v->updating = TRUE;
    v->wf_sync = TRUE;
    gtk_drop_down_set_selected(v->view, (guint)chfvid_tab_view(v->tab));
    for (i = 0; i < nz; i++)
        if (zv[i] == chfvid_tab_zoom(v->tab)) gtk_drop_down_set_selected(v->zoom, (guint)i);
    for (i = 0; i < v->nov; i++)
        gtk_check_button_set_active(v->ov[i], (chfvid_tab_overlays(v->tab) & od[i].bit) != 0);
    chfvid_tab_write_filter(v->tab, &wfilter);
    gtk_check_button_set_active(v->wf[0], wfilter.this_frame != 0);
    gtk_check_button_set_active(v->wf[1], wfilter.changes_only != 0);
    gtk_check_button_set_active(v->wf[2], wfilter.palette_cols != 0);
    gtk_check_button_set_active(v->wf[3], wfilter.inspected_cell != 0);
    gtk_check_button_set_active(v->wf[4], wfilter.inspected_row != 0);
    gtk_notebook_set_current_page(v->sections, chfvid_tab_env_section(v->tab));
    v->wf_sync = FALSE;
    v->updating = FALSE;
    update_pixel_section(v);
    g_signal_connect(v->view, "notify::selected", G_CALLBACK(on_view), v);
    g_signal_connect(v->zoom, "notify::selected", G_CALLBACK(on_zoom), v);
    g_signal_connect(v->sections, "switch-page", G_CALLBACK(on_section), v);
    return root;
}
