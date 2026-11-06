/*
 * Debugger window (GTK4/libadwaita) over the session's Fairchild F8 / Channel F
 * debugger engine, via core/include/chfdebug.h.
 *
 * Tabs, in order: a Prompt (the contract's command set, with tab completion
 * and a command history); CPU & RAM (dbg_cpu.c: A, W, ISAR in octal, PC0/PC1
 * and the data counters, J H K Q, the scratchpad as an 8 x 8 octal grid, the
 * BIOS K stack, and a hex view over the five spaces); the disassembly with a
 * breakpoint gutter; Video (dbg_video.c: the six views with their overlays,
 * the cell readout, the row palettes, the ports and the VRAM write log);
 * Sound & I/O (dbg_io.c); Breakpoints (all seven kinds); Cart (dbg_cart.c); and
 * FujiNet: every FujiBus packet on the cartridge's link, both ways, with hex. A
 * toolbar carries the stepping controls, symbol loading and file saves; the
 * family's keys apply (F5 run/stop, F7 step, F8 step over, Shift+F8 step out,
 * F12 close, F1 Help). The header bar's Help button opens the topic of the tab
 * that shows.
 *
 * The engine exists only while this window is showing: showing it attaches
 * (which stops the machine, as on every sibling), hiding it detaches and
 * lets the machine run on.
 *
 * Idle cost: the window polls the engine's generation counter on a short
 * timer that runs only while the window is on screen, and refreshes only
 * the tab that shows (the others are marked stale and catch up when
 * selected). A stopped machine costs nothing; a running one shows live
 * values twice a second, the Video tab's picture ten times. Every chfdebug
 * call waits for the frame in progress (the host's run lock), so looking at a
 * running machine is safe.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <stdlib.h>
#include <string.h>

#include "../help/help_window.h"
#include "../window.h"
#include "dbg_common.h"

#define DISASM_MAX 160
#define TRACE_MAX 512
#define GUTTER 26

/* The tabs, in order, by the names CHF_DEBUGGER_TAB takes. */
enum {
    TAB_PROMPT, TAB_CPU, TAB_DISASM, TAB_VIDEO, TAB_IO, TAB_BREAKS, TAB_CART,
    TAB_FUJINET, TAB_COUNT
};
static const struct { const char *name; int tab; } k_tab_names[] = {
    { "prompt", TAB_PROMPT }, { "cpu", TAB_CPU }, { "ram", TAB_CPU },
    { "memory", TAB_CPU }, { "disassembly", TAB_DISASM }, { "video", TAB_VIDEO },
    { "vram", TAB_VIDEO }, { "sound", TAB_IO }, { "io", TAB_IO },
    { "breakpoints", TAB_BREAKS }, { "cart", TAB_CART }, { "fujinet", TAB_FUJINET },
};
/* Help's contexts (chfhelp.h), one per tab */
static const char *const k_tab_contexts[TAB_COUNT] = {
    "debugger/prompt", "debugger/cpu", "debugger/disassembly", "debugger/video",
    "debugger/io", "debugger/breakpoints", "debugger/cart", "debugger/fujinet",
};

/* The breakpoint kinds the form offers, with the space each one's range is in. */
static const struct { const char *name; int type; int space; const char *hint; } k_bp_kinds[] = {
    { "Execute", CHFDEBUG_BP_EXEC, CHF_SPACE_PROG, "$0802 or a label" },
    { "Read", CHFDEBUG_BP_READ, CHF_SPACE_PROG, "$8000 or a label" },
    { "Write", CHFDEBUG_BP_WRITE, CHF_SPACE_PROG, "$8000 or a label" },
    { "Port in", CHFDEBUG_BP_IN, CHF_SPACE_IO, "port, e.g. 1 or $01" },
    { "Port out", CHFDEBUG_BP_OUT, CHF_SPACE_IO, "port, e.g. 5 or $05" },
    { "VRAM write", CHFDEBUG_BP_VRAM, CHF_SPACE_VRAM, "row,col e.g. 12,40" },
    { "Scratchpad write", CHFDEBUG_BP_SCRATCH, CHF_SPACE_SCRATCH, "register, e.g. 9 or 40" },
};
#define NBPKINDS ((int)G_N_ELEMENTS(k_bp_kinds))

typedef struct {
    GtkWindow *win;
    chfsession *session;
    chfdebug *dbg;
    unsigned seen_generation;
    gboolean was_stopped;
    int running_ticks;
    guint timer;
    gboolean stale[TAB_COUNT];

    AdwWindowTitle *title;    /* "Debugger", with the stop reason under it */
    GtkButton *run_btn;
    AdwToastOverlay *toasts;
    GtkNotebook *notebook;

    /* prompt */
    GtkTextView *prompt_out;
    GtkEntry *prompt_in;
    GtkScrolledWindow *prompt_scroll;
    GPtrArray *history;
    guint history_pos;

    /* cpu + ram, video, sound & i/o, cart */
    DbgCpu *cpu;
    DbgVideo *video;
    DbgIo *io;
    DbgCart *cart;

    /* disassembly */
    GtkWidget *dis_area;
    GtkCheckButton *follow_pc;
    GtkEntry *jump;
    chfdebug_line dis_lines[DISASM_MAX];
    int dis_count;
    int dis_rows;             /* lines that fit */
    int dis_top;              /* address of the first line */
    int dis_sel;              /* the selected line's address, -1 none */
    double dis_lh, dis_cw;    /* a line's height and a character's width */
    gboolean dis_metrics;

    /* breakpoints */
    GtkListBox *bp_list;
    GtkDropDown *bp_type;
    GtkEntry *bp_start, *bp_end, *bp_cond;
    GtkLabel *bp_msg;

    /* fujinet */
    GtkLabel *fuji_note;
    GtkTextView *fuji_text, *fuji_state;
    uint64_t fuji_last;
    int fuji_count;
} DbgWin;

static DbgWin *g_win;

/* ---- helpers -------------------------------------------------------------- */

GtkWidget *dbg_mono_view(GtkTextView **out, gboolean editable)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), editable);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), editable);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);
    *out = GTK_TEXT_VIEW(view);
    return scroll;
}

static GtkAdjustment *vadj(GtkTextView *view)
{
    GtkWidget *parent = gtk_widget_get_parent(GTK_WIDGET(view));
    return GTK_IS_SCROLLED_WINDOW(parent)
        ? gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(parent)) : NULL;
}

/* A log (the FujiNet packets, the VRAM writes, the I/O accesses) follows its
 * end while its reader is there. New text grows the view only once it is laid
 * out (or its tab first shows), so the end is pinned whenever the view's
 * height changes; whether to follow is decided as text arrives. */
gboolean dbg_log_following(GtkTextView *view)
{
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(view), "follow")) != 0;
}

static void on_log_resized(GtkAdjustment *adj, gpointer view)
{
    if (dbg_log_following(view))
        gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj) -
                                      gtk_adjustment_get_page_size(adj));
}

void dbg_log_make(GtkTextView *view)
{
    g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
    g_signal_connect(vadj(view), "changed", G_CALLBACK(on_log_resized), view);
}

/* Before new text: follow if the reader is at the end (or never looked). */
void dbg_log_note_reader(GtkTextView *view)
{
    GtkAdjustment *adj = vadj(view);
    g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(
        gtk_adjustment_get_page_size(adj) <= 0.0 ||
        gtk_adjustment_get_value(adj) + gtk_adjustment_get_page_size(adj) >=
            gtk_adjustment_get_upper(adj) - 2.0));
}

/* Replaces a view's text where the reader left it: a refresh (a step, or
 * the live values twice a second) must not throw a scrolled dump back to its
 * top. The top line is noted and scrolled back to once the new text is laid
 * out (scroll_to_mark waits for that). A log that is following goes to its
 * end instead. */
void dbg_set_text_follow(GtkTextView *view, const char *text, gboolean follow)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(view);
    GtkAdjustment *adj = vadj(view);
    GtkTextIter it;
    int top = 0;

    if (!follow && adj && gtk_adjustment_get_value(adj) > 0.0) {
        gtk_text_view_get_line_at_y(view, &it, (int)gtk_adjustment_get_value(adj), NULL);
        top = gtk_text_iter_get_line(&it);
    }
    {
        /* unchanged text leaves the buffer, the scroll position and any
         * selection alone */
        GtkTextIter s, e;
        char *old;
        gboolean same;

        gtk_text_buffer_get_bounds(b, &s, &e);
        old = gtk_text_buffer_get_text(b, &s, &e, TRUE);
        same = strcmp(old, text) == 0;
        g_free(old);
        if (same)
            return;
    }
    gtk_text_buffer_set_text(b, text, -1);
    if (follow) {
        g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
        if (adj) on_log_resized(adj, view);
        return;
    }
    if (top > 0) {
        GtkTextMark *mark;
        gtk_text_buffer_get_iter_at_line(b, &it, top);
        mark = gtk_text_buffer_create_mark(b, NULL, &it, TRUE);
        gtk_text_view_scroll_to_mark(view, mark, 0.0, TRUE, 0.0, 0.0);
        gtk_text_buffer_delete_mark(b, mark);
    }
}

void dbg_set_text(GtkTextView *view, const char *text)
{
    dbg_set_text_follow(view, text, FALSE);
}

static void append_text(GtkTextView *view, GtkScrolledWindow *scroll, const char *text)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(view);
    GtkTextIter end;
    GtkAdjustment *adj;
    gtk_text_buffer_get_end_iter(b, &end);
    gtk_text_buffer_insert(b, &end, text, -1);
    adj = gtk_scrolled_window_get_vadjustment(scroll);
    if (adj) gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj));
}

int dbg_parse_num(const char *text, long *out)
{
    char *end;
    long v;
    while (*text == ' ') text++;
    if (*text == '$') v = strtol(text + 1, &end, 16);
    else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) v = strtol(text + 2, &end, 16);
    else if (*text == '#') v = strtol(text + 1, &end, 10);
    else v = strtol(text, &end, 16);
    if (end == text || *end) return 0;
    *out = v;
    return 1;
}

int dbg_parse_dec(const char *text, long *out)
{
    char *end;
    long v;
    while (*text == ' ') text++;
    if (*text == '$') v = strtol(text + 1, &end, 16);
    else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) v = strtol(text + 2, &end, 16);
    else if (*text == 'r' || *text == 'R') v = strtol(text + 1, &end, 10);
    else v = strtol(text, &end, 10);
    if (end == text || *end) return 0;
    *out = v;
    return 1;
}

/* A label or a number, as typed into an address box. */
static int parse_addr(DbgWin *w, const char *text, long *out)
{
    int a = chfdebug_label_address(w->dbg, text);
    if (a >= 0) { *out = a; return 1; }
    return dbg_parse_num(text, out);
}

void dbg_set_entry(chfdebug *d, GtkEntry *e, const char *text)
{
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(e));
    GtkWidget *focus = root ? gtk_root_get_focus(root) : NULL;
    if (focus && !chfdebug_is_stopped(d) &&
        (focus == GTK_WIDGET(e) || gtk_widget_is_ancestor(focus, GTK_WIDGET(e))))
        return;
    if (strcmp(gtk_editable_get_text(GTK_EDITABLE(e)), text) != 0)
        gtk_editable_set_text(GTK_EDITABLE(e), text);
}

GtkWidget *dbg_hex_entry(int digits)
{
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(e), digits + 2);
    gtk_editable_set_width_chars(GTK_EDITABLE(e), digits + 1);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(e), digits + 1);
    gtk_widget_add_css_class(e, "monospace");
    return e;
}

void dbg_pill(GtkWidget *w, gboolean on)
{
    if (on) {
        gtk_widget_add_css_class(w, "chf-accent");
        gtk_widget_remove_css_class(w, "dim-label");
    } else {
        gtk_widget_remove_css_class(w, "chf-accent");
        gtk_widget_add_css_class(w, "dim-label");
    }
}

GtkWidget *dbg_labeled(const char *text, GtkWidget *child)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "dim-label");
    gtk_box_append(GTK_BOX(box), l);
    gtk_box_append(GTK_BOX(box), child);
    /* beside a wrapped hint, an entry keeps its own height */
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    return box;
}

GtkWidget *dbg_heading(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "heading");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    return l;
}

GtkWidget *dbg_padded(GtkWidget *w)
{
    gtk_widget_set_margin_top(w, 8);
    gtk_widget_set_margin_bottom(w, 8);
    gtk_widget_set_margin_start(w, 8);
    gtk_widget_set_margin_end(w, 8);
    return w;
}

GtkWidget *dbg_hint(const char *text, int width_chars)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "dim-label");
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_width_chars(GTK_LABEL(l), width_chars);
    gtk_label_set_max_width_chars(GTK_LABEL(l), width_chars * 2);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    return l;
}

GtkWidget *dbg_mono_label(void)
{
    GtkWidget *l = gtk_label_new("");
    gtk_widget_add_css_class(l, "monospace");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_yalign(GTK_LABEL(l), 0);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_widget_set_focusable(l, FALSE);       /* no caret; the mouse still selects */
    return l;
}

void dbg_set_rgb(cairo_t *cr, guint32 rgb, double alpha)
{
    cairo_set_source_rgba(cr, ((rgb >> 16) & 0xFF) / 255.0, ((rgb >> 8) & 0xFF) / 255.0,
                          (rgb & 0xFF) / 255.0, alpha);
}

void dbg_set_fg(cairo_t *cr, GtkWidget *w, double alpha)
{
    GdkRGBA c;
    gtk_widget_get_color(w, &c);
    cairo_set_source_rgba(cr, c.red, c.green, c.blue, c.alpha * alpha);
}

guint32 dbg_accent_line_rgb(void)
{
    /* the yellow is a line colour on a dark theme; the bronze is on a light one */
    return adw_style_manager_get_dark(adw_style_manager_get_default())
        ? CHFSESSION_ACCENT_RGB : CHFSESSION_ACCENT_DARK_RGB;
}

int dbg_draw_text(cairo_t *cr, GtkWidget *w, const char *text, double x, double y,
                  gboolean mono, double px)
{
    PangoLayout *layout = gtk_widget_create_pango_layout(w, text);
    int tw, th;
    if (mono) {
        PangoFontDescription *fd = pango_font_description_new();
        pango_font_description_set_family(fd, "Monospace");
        pango_font_description_set_absolute_size(fd, (px > 0 ? px : 13) * PANGO_SCALE);
        pango_layout_set_font_description(layout, fd);
        pango_font_description_free(fd);
    } else if (px > 0) {
        PangoFontDescription *fd = pango_font_description_new();
        pango_font_description_set_absolute_size(fd, px * PANGO_SCALE);
        pango_layout_set_font_description(layout, fd);
        pango_font_description_free(fd);
    }
    pango_layout_get_pixel_size(layout, &tw, &th);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
    cairo_new_path(cr);         /* the move_to would join the next arc with a line */
    g_object_unref(layout);
    return tw;
}

void dbg_draw_centred(cairo_t *cr, GtkWidget *w, const char *text, double x, double y,
                      double cw, double ch, gboolean mono)
{
    PangoLayout *layout = gtk_widget_create_pango_layout(w, text);
    int tw, th;
    if (mono) {
        PangoFontDescription *fd = pango_font_description_from_string("Monospace 10");
        pango_layout_set_font_description(layout, fd);
        pango_font_description_free(fd);
    }
    pango_layout_get_pixel_size(layout, &tw, &th);
    cairo_move_to(cr, x + (cw - tw) / 2.0, y + (ch - th) / 2.0);
    pango_cairo_show_layout(cr, layout);
    cairo_new_path(cr);         /* the move_to would join the next arc with a line */
    g_object_unref(layout);
}

/* Messages from Load Symbols and Save land in the prompt's transcript and,
 * since the prompt may not be the tab showing, in a toast. */
static void prompt_say(DbgWin *w, const char *msg)
{
    char line[700];
    g_snprintf(line, sizeof line, "%s\n", msg);
    append_text(w->prompt_out, w->prompt_scroll, line);
    adw_toast_overlay_add_toast(w->toasts, adw_toast_new(msg));
}

static void install_css(void)
{
    static gboolean done;
    GtkCssProvider *css;
    char text[256];
    if (done) return;
    done = TRUE;
    css = gtk_css_provider_new();
    g_snprintf(text, sizeof text,
        "entry.chf-hex { padding: 1px 2px; min-height: 0; min-width: 0; }\n"
        "entry.chf-hex > text { min-width: 0; }\n"
        "levelbar.chf-meter block.filled { background-color: #%06x; }\n",
        CHFSESSION_ACCENT_RGB);
    gtk_css_provider_load_from_string(css, text);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

/* ---- disassembly: the lines, a breakpoint gutter and the PC in the accent ---- */

static void dis_measure(DbgWin *w)
{
    PangoLayout *l;
    PangoFontDescription *fd;
    int tw, th;
    if (w->dis_metrics) return;
    l = gtk_widget_create_pango_layout(w->dis_area, "0000000000");
    fd = pango_font_description_new();
    pango_font_description_set_family(fd, "Monospace");
    pango_font_description_set_absolute_size(fd, 13 * PANGO_SCALE);
    pango_layout_set_font_description(l, fd);
    pango_layout_get_pixel_size(l, &tw, &th);
    w->dis_cw = tw / 10.0;
    w->dis_lh = th + 3;
    w->dis_metrics = TRUE;
    pango_font_description_free(fd);
    g_object_unref(l);
}

static void refresh_disasm(DbgWin *w)
{
    int pc_line, rows;

    dis_measure(w);
    rows = w->dis_rows > 4 ? w->dis_rows : 30;
    if (rows > DISASM_MAX) rows = DISASM_MAX;
    /* Following, the PC sits a third of the way down the window. */
    if (gtk_check_button_get_active(w->follow_pc)) {
        chfdebug_cpu c;
        chfdebug_cpu_get(w->dbg, &c);
        if (c.pc >= 0) {
            const int top = chfdebug_row_address(w->dbg, (uint16_t)c.pc, -(rows / 3));
            /* the rows before the start of the address space are not the end of it */
            w->dis_top = (top & 0xFFFF) > c.pc ? 0 : top;
        }
    }
    w->dis_count = chfdebug_disassemble(w->dbg, (uint16_t)w->dis_top, w->dis_lines, rows, &pc_line);
    gtk_widget_queue_draw(w->dis_area);
}

static void draw_disasm(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer ud)
{
    DbgWin *w = ud;
    GtkWidget *wg = GTK_WIDGET(area);
    const guint32 line_rgb = dbg_accent_line_rgb();
    int i;
    (void)height;

    dis_measure(w);
    for (i = 0; i < w->dis_count; i++) {
        const chfdebug_line *l = &w->dis_lines[i];
        const double y = i * w->dis_lh;
        const double tx = GUTTER + 6;
        const gboolean sel = w->dis_sel == l->address;
        char text[160];
        double x;

        if (l->is_pc) {
            dbg_set_rgb(cr, CHFSESSION_ACCENT_RGB, 1.0);
            cairo_rectangle(cr, 0, y, width, w->dis_lh);
            cairo_fill(cr);
        } else if (sel) {
            dbg_set_fg(cr, wg, 0.14);
            cairo_rectangle(cr, 0, y, width, w->dis_lh);
            cairo_fill(cr);
        }
        /* the gutter: a breakpoint is a dot, the PC a triangle */
        if (l->has_breakpoint) {
            cairo_arc(cr, GUTTER / 2.0 - 2, y + w->dis_lh / 2, 5.5, 0, 2 * G_PI);
            dbg_set_rgb(cr, 0xD8362B, 1.0);
            cairo_fill_preserve(cr);
            dbg_set_rgb(cr, 0x000000, 0.4);
            cairo_set_line_width(cr, 1);
            cairo_stroke(cr);
        }
        if (l->is_pc) {
            cairo_move_to(cr, GUTTER - 10, y + w->dis_lh / 2 - 4);
            cairo_line_to(cr, GUTTER - 3, y + w->dis_lh / 2);
            cairo_line_to(cr, GUTTER - 10, y + w->dis_lh / 2 + 4);
            cairo_close_path(cr);
            dbg_set_rgb(cr, 0x1E1A14, 1.0);
            cairo_fill(cr);
        }
        if (l->is_pc) dbg_set_rgb(cr, 0x1E1A14, 1.0);
        else dbg_set_fg(cr, wg, 1.0);
        g_snprintf(text, sizeof text, "%04X  %-8s %-14s %s", l->address, l->bytes,
                   l->label, l->disasm);
        x = tx;
        x += dbg_draw_text(cr, wg, text, x, y + 1, TRUE, 13);
        if (l->comment[0]) {
            if (l->is_pc) dbg_set_rgb(cr, 0x1E1A14, 0.7);
            else dbg_set_fg(cr, wg, 0.55);
            g_snprintf(text, sizeof text, "  ; %s", l->comment);
            dbg_draw_text(cr, wg, text, x, y + 1, TRUE, 13);
        }
    }
    (void)line_rgb;
    if (w->dis_count == 0) {
        dbg_set_fg(cr, wg, 0.6);
        dbg_draw_text(cr, wg, "(no disassembly)", GUTTER + 6, 4, TRUE, 13);
    }
    /* the gutter's edge */
    dbg_set_fg(cr, wg, 0.15);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, GUTTER + 0.5, 0);
    cairo_line_to(cr, GUTTER + 0.5, height);
    cairo_stroke(cr);
}

static void on_disasm_resize(GtkDrawingArea *area, int width, int height, gpointer ud)
{
    DbgWin *w = ud;
    int rows;
    (void)area; (void)width;
    dis_measure(w);
    rows = (int)(height / w->dis_lh) + 1;
    if (rows != w->dis_rows) {
        w->dis_rows = rows;
        refresh_disasm(w);
    }
}

static int dis_line_at(DbgWin *w, double y)
{
    int i = (int)(y / w->dis_lh);
    return i >= 0 && i < w->dis_count ? i : -1;
}

static void dis_toggle(DbgWin *w, int i)
{
    chfdebug_breakpoint_toggle(w->dbg, w->dis_lines[i].address);
    refresh_disasm(w);
    w->stale[TAB_BREAKS] = TRUE;
}

/* A click in the gutter toggles the line's breakpoint, anywhere else selects
 * the line; a double-click toggles too. */
static void on_disasm_click(GtkGestureClick *g, int n, double x, double y, gpointer ud)
{
    DbgWin *w = ud;
    const int i = dis_line_at(w, y);
    (void)g;
    if (i < 0) return;
    w->dis_sel = w->dis_lines[i].address;
    if (n == 2 || x < GUTTER)
        dis_toggle(w, i);
    else
        gtk_widget_queue_draw(w->dis_area);
}

static void on_follow_toggled(GtkCheckButton *b, gpointer ud)
{
    (void)b;
    refresh_disasm(ud);
}

static void on_jump(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    long a;
    if (!parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(entry)), &a)) return;
    gtk_check_button_set_active(w->follow_pc, FALSE);
    w->dis_top = (int)(a & 0xFFFF);
    w->dis_sel = w->dis_top;
    refresh_disasm(w);
}

static gboolean on_disasm_scroll(GtkEventControllerScroll *c, double dx, double dy, gpointer ud)
{
    DbgWin *w = ud;
    int rows = (int)(dy * 3);
    (void)c; (void)dx;
    if (!rows) rows = dy > 0 ? 1 : -1;
    gtk_check_button_set_active(w->follow_pc, FALSE);
    w->dis_top = chfdebug_row_address(w->dbg, (uint16_t)w->dis_top, rows);
    refresh_disasm(w);
    return TRUE;
}

/* ---- refresh -------------------------------------------------------------- */

static void refresh_fujinet(DbgWin *w, gboolean force)
{
    static chfdebug_fuji_packet p[TRACE_MAX];
    static char state[2048];
    char note[200], status[128];
    GString *t;
    int n = chfdebug_fujinet_trace(w->dbg, p, TRACE_MAX), i;

    if (chfsession_fujinet_external(w->session)) {
        chfsession_cart_status(w->session, status, sizeof status);
        g_snprintf(note, sizeof note, "External FujiNet at %s. Cartridge: %s.",
                   chfsession_boip_endpoint(w->session), status);
    } else if (!chfsession_fujinet_running(w->session)) {
        g_strlcpy(note, "FujiNet is off (Preferences \xe2\x96\xb8 FujiNet).", sizeof note);
    } else {
        chfsession_cart_status(w->session, status, sizeof status);
        g_snprintf(note, sizeof note, "FujiNet is on. Cartridge: %s.", status);
    }
    gtk_label_set_text(w->fuji_note, note);
    chfdebug_fujinet_state(w->dbg, state, sizeof state);
    dbg_set_text(w->fuji_state, state);

    if (!force && n == w->fuji_count && (n == 0 || p[n - 1].serial == w->fuji_last))
        return;
    w->fuji_count = n;
    w->fuji_last = n > 0 ? p[n - 1].serial : 0;
    dbg_log_note_reader(w->fuji_text);
    t = g_string_new(NULL);
    for (i = 0; i < n; i++) {
        int k;
        g_string_append_printf(t, "%s%s", i ? "\n" : "", p[i].text);
        if (p[i].repeat > 1) g_string_append_printf(t, " \xc3\x97%u", p[i].repeat);
        g_string_append(t, "\n     ");
        for (k = 0; k < p[i].nbytes && k < (int)sizeof p[i].data; k++)
            g_string_append_printf(t, "%02X ", p[i].data[k]);
        if (p[i].nbytes > (int)sizeof p[i].data) g_string_append(t, "\xe2\x80\xa6");
    }
    if (n == 0)
        g_string_append(t, "(no FujiBus packets since the trace was cleared)");
    dbg_set_text_follow(w->fuji_text, t->str, force || dbg_log_following(w->fuji_text));
    g_string_free(t, TRUE);
}

static void bp_enable_toggled(GtkCheckButton *b, gpointer ud);
static void bp_remove_clicked(GtkButton *b, gpointer ud);

static const char *bp_type_name(int type)
{
    int i;
    for (i = 0; i < NBPKINDS; i++)
        if (type & k_bp_kinds[i].type)
            return k_bp_kinds[i].name;
    return "?";
}

static void bp_cell(char *dst, int n, unsigned v)
{
    g_snprintf(dst, n, "r%u,c%u", v / CHFDEBUG_VRAM_COLS, v % CHFDEBUG_VRAM_COLS);
}

/* The range as the kind's own units: addresses, ports, row,col cells, registers. */
static const char *bp_where(const chfdebug_breakpoint *bp, char *buf, int n)
{
    char a[24], b[24];
    if (bp->type & (CHFDEBUG_BP_EXEC | CHFDEBUG_BP_READ | CHFDEBUG_BP_WRITE)) {
        g_snprintf(a, sizeof a, "$%04X", bp->start);
        g_snprintf(b, sizeof b, "$%04X", bp->end);
    } else if (bp->type & (CHFDEBUG_BP_IN | CHFDEBUG_BP_OUT)) {
        g_snprintf(a, sizeof a, "port $%02X", bp->start);
        g_snprintf(b, sizeof b, "$%02X", bp->end);
    } else if (bp->type & CHFDEBUG_BP_VRAM) {
        bp_cell(a, sizeof a, bp->start);
        bp_cell(b, sizeof b, bp->end);
    } else {
        g_snprintf(a, sizeof a, "r%u", bp->start);
        g_snprintf(b, sizeof b, "r%u", bp->end);
    }
    if (bp->end != bp->start && !(bp->type & CHFDEBUG_BP_EXEC))
        g_snprintf(buf, n, "%s-%s", a, b);
    else
        g_snprintf(buf, n, "%s", a);
    return buf;
}

static void refresh_bps(DbgWin *w)
{
    chfdebug_breakpoint bps[128];
    GtkWidget *child;
    int n = chfdebug_breakpoint_list(w->dbg, bps, 128), i;

    while ((child = gtk_widget_get_first_child(GTK_WIDGET(w->bp_list))) != NULL)
        gtk_list_box_remove(w->bp_list, child);
    if (n == 0) {
        GtkWidget *l = gtk_label_new("No breakpoints. Click a disassembly line's margin, or add one below.");
        gtk_widget_add_css_class(l, "dim-label");
        gtk_list_box_append(w->bp_list, l);
        return;
    }
    for (i = 0; i < n; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *en = gtk_check_button_new();
        GtkWidget *rm = gtk_button_new_from_icon_name("user-trash-symbolic");
        GtkWidget *l;
        char text[360], where[64];
        g_snprintf(text, sizeof text, "#%-3d %-17s %-24s hits %-6u %s%s", bps[i].id,
                   bp_type_name(bps[i].type), bp_where(&bps[i], where, sizeof where),
                   bps[i].hits, bps[i].condition[0] ? "if " : "", bps[i].condition);
        l = gtk_label_new(text);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_widget_set_hexpand(l, TRUE);
        gtk_widget_add_css_class(l, "monospace");
        gtk_check_button_set_active(GTK_CHECK_BUTTON(en), bps[i].enabled != 0);
        gtk_widget_set_tooltip_text(en, "Enabled");
        g_object_set_data(G_OBJECT(en), "bp", GINT_TO_POINTER(bps[i].id));
        g_object_set_data(G_OBJECT(rm), "bp", GINT_TO_POINTER(bps[i].id));
        g_signal_connect(en, "toggled", G_CALLBACK(bp_enable_toggled), w);
        g_signal_connect(rm, "clicked", G_CALLBACK(bp_remove_clicked), w);
        gtk_widget_add_css_class(rm, "flat");
        gtk_widget_set_tooltip_text(rm, "Remove");
        gtk_box_append(GTK_BOX(row), en);
        gtk_box_append(GTK_BOX(row), l);
        gtk_box_append(GTK_BOX(row), rm);
        gtk_list_box_append(w->bp_list, row);
    }
}

static void refresh_status(DbgWin *w)
{
    char reason[160], text[256];
    int addr;
    gboolean stopped = chfdebug_is_stopped(w->dbg) != 0;
    chfdebug_stop_reason(w->dbg, reason, sizeof reason, &addr);
    if (stopped)
        g_snprintf(text, sizeof text, "Stopped%s%s", reason[0] ? ": " : "", reason);
    else
        g_snprintf(text, sizeof text, "Running");
    adw_window_title_set_subtitle(w->title, text);
    gtk_button_set_label(w->run_btn, stopped ? "Run (F5)" : "Stop (F5)");
    if (stopped) gtk_widget_add_css_class(GTK_WIDGET(w->run_btn), "chf-accent");
    else gtk_widget_remove_css_class(GTK_WIDGET(w->run_btn), "chf-accent");
}

/* One tab, from the machine as it is now. Only the tab that shows is ever
 * refreshed; the others catch up when selected (stale). */
static void refresh_tab(DbgWin *w, int tab)
{
    const gboolean live = !chfdebug_is_stopped(w->dbg);
    w->stale[tab] = FALSE;
    switch (tab) {
    case TAB_CPU: dbg_cpu_refresh(w->cpu, live); break;
    case TAB_DISASM: refresh_disasm(w); break;
    case TAB_VIDEO: dbg_video_refresh(w->video, live); break;
    case TAB_IO: dbg_io_refresh(w->io, live); break;
    case TAB_BREAKS: refresh_bps(w); break;
    case TAB_CART: dbg_cart_refresh(w->cart, live); break;
    case TAB_FUJINET: refresh_fujinet(w, FALSE); break;
    default: break;
    }
}

static void refresh_current(DbgWin *w)
{
    refresh_tab(w, gtk_notebook_get_current_page(w->notebook));
}

/* Something changed (a step, a command, an edit): the status and the tab
 * that shows now; every other tab is stale until selected. */
static void refresh_all(DbgWin *w)
{
    int t;
    refresh_status(w);
    for (t = 0; t < TAB_COUNT; t++)
        w->stale[t] = TRUE;
    refresh_current(w);
}

static void on_tab_changed(gpointer ud)
{
    refresh_all(ud);
}

static void attach_and_refresh(DbgWin *w);

static gboolean tick_refresh(gpointer ud)
{
    DbgWin *w = ud;
    unsigned gen;
    gboolean stopped;
    /* Opened before the session started (CHF_OPEN_DEBUGGER), or showing
     * across a session restart: the engine comes up with the machine. */
    if (!chfdebug_is_attached(w->dbg) && chfsession_is_running(w->session)) {
        attach_and_refresh(w);
        return G_SOURCE_CONTINUE;
    }
    gen = chfdebug_generation(w->dbg);
    stopped = chfdebug_is_stopped(w->dbg) != 0;
    if (gen != w->seen_generation || stopped != w->was_stopped) {
        w->seen_generation = gen;
        w->was_stopped = stopped;
        refresh_all(w);
    } else if (!stopped) {
        /* live: the Video tab's picture ten times a second, every other
         * tab twice */
        const int tab = gtk_notebook_get_current_page(w->notebook);
        if (tab == TAB_VIDEO) {
            refresh_tab(w, tab);
        } else if (++w->running_ticks >= 5) {
            w->running_ticks = 0;
            refresh_status(w);
            refresh_tab(w, tab);
        }
    }
    return G_SOURCE_CONTINUE;
}

/* ---- handlers ------------------------------------------------------------- */

static void on_run(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    if (chfdebug_is_stopped(w->dbg)) chfdebug_resume(w->dbg);
    else chfdebug_stop(w->dbg);
    refresh_all(w);
}

static void on_step(GtkButton *b, gpointer ud) { (void)b; chfdebug_step(((DbgWin *)ud)->dbg); refresh_all(ud); }
static void on_step_over(GtkButton *b, gpointer ud) { (void)b; chfdebug_step_over(((DbgWin *)ud)->dbg); refresh_all(ud); }
static void on_step_out(GtkButton *b, gpointer ud) { (void)b; chfdebug_step_out(((DbgWin *)ud)->dbg); refresh_all(ud); }
static void on_scanline(GtkButton *b, gpointer ud) { (void)b; chfdebug_scanline(((DbgWin *)ud)->dbg, 1); refresh_all(ud); }
static void on_frame(GtkButton *b, gpointer ud) { (void)b; chfdebug_frame(((DbgWin *)ud)->dbg, 1); refresh_all(ud); }

/* Run to the line selected in the Disassembly. */
static void on_run_to(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    if (w->dis_sel < 0) {
        adw_toast_overlay_add_toast(w->toasts,
            adw_toast_new("Select a line in the Disassembly first"));
        return;
    }
    chfdebug_run_to(w->dbg, (uint16_t)w->dis_sel);
    refresh_all(w);
}

static void on_prompt(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    static char out[65536];
    const char *cmd = gtk_editable_get_text(GTK_EDITABLE(entry));
    char line[512];
    if (!cmd || !*cmd) return;
    /* the history keeps each command once in a row */
    if (w->history->len == 0 ||
        strcmp(g_ptr_array_index(w->history, w->history->len - 1), cmd) != 0)
        g_ptr_array_add(w->history, g_strdup(cmd));
    w->history_pos = w->history->len;
    g_snprintf(line, sizeof line, "> %s\n", cmd);
    append_text(w->prompt_out, w->prompt_scroll, line);
    chfdebug_command(w->dbg, cmd, out, sizeof out);
    append_text(w->prompt_out, w->prompt_scroll, out);
    if (out[0] && out[strlen(out) - 1] != '\n')
        append_text(w->prompt_out, w->prompt_scroll, "\n");
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
    refresh_all(w);
}

static void history_show(DbgWin *w)
{
    const char *text = w->history_pos < w->history->len
                           ? g_ptr_array_index(w->history, w->history_pos) : "";
    gtk_editable_set_text(GTK_EDITABLE(w->prompt_in), text);
    gtk_editable_set_position(GTK_EDITABLE(w->prompt_in), -1);
}

/* Tab completes the current word against the commands, the registers and
 * the labels; several matches are listed instead. Up and Down walk the
 * command history. */
static gboolean on_prompt_key(GtkEventControllerKey *c, guint keyval, guint code,
                              GdkModifierType st, gpointer ud)
{
    DbgWin *w = ud;
    char comps[4096];
    const char *text;
    const char *word;
    int n;
    (void)c; (void)code; (void)st;
    if (keyval == GDK_KEY_Up || keyval == GDK_KEY_KP_Up) {
        if (w->history_pos > 0) {
            w->history_pos--;
            history_show(w);
        }
        return TRUE;
    }
    if (keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down) {
        if (w->history_pos < w->history->len) {
            w->history_pos++;
            history_show(w);
        }
        return TRUE;
    }
    if (keyval != GDK_KEY_Tab) return FALSE;
    text = gtk_editable_get_text(GTK_EDITABLE(w->prompt_in));
    word = strrchr(text, ' ');
    word = word ? word + 1 : text;
    n = chfdebug_completions(w->dbg, word, comps, sizeof comps);
    if (n == 1) {
        char merged[600];
        char *nl = strchr(comps, '\n');
        if (nl) *nl = '\0';
        g_snprintf(merged, sizeof merged, "%.*s%s ", (int)(word - text), text, comps);
        gtk_editable_set_text(GTK_EDITABLE(w->prompt_in), merged);
        gtk_editable_set_position(GTK_EDITABLE(w->prompt_in), -1);
    } else if (n > 1) {
        append_text(w->prompt_out, w->prompt_scroll, comps);
        if (comps[0] && comps[strlen(comps) - 1] != '\n')
            append_text(w->prompt_out, w->prompt_scroll, "\n");
    }
    return TRUE;
}

static void bp_enable_toggled(GtkCheckButton *b, gpointer ud)
{
    DbgWin *w = ud;
    chfdebug_breakpoint_enable(w->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "bp")),
                               gtk_check_button_get_active(b));
    refresh_disasm(w);
}

static void bp_remove_clicked(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    chfdebug_breakpoint_remove(w->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "bp")));
    refresh_all(w);
}

/* One end of a breakpoint's range, in the kind's own units. */
static int bp_parse(DbgWin *w, int kind, const char *text, long *out)
{
    long row, col;
    char *comma;

    switch (k_bp_kinds[kind].space) {
    case CHF_SPACE_PROG:
        return parse_addr(w, text, out);
    case CHF_SPACE_VRAM:
        /* row,col, or a bare cell number */
        if ((comma = strchr(text, ',')) != NULL) {
            g_autofree char *r = g_strndup(text, (gsize)(comma - text));
            if (!dbg_parse_dec(r, &row) || !dbg_parse_dec(comma + 1, &col) ||
                row < 0 || row >= CHFDEBUG_VRAM_ROWS || col < 0 || col >= CHFDEBUG_VRAM_COLS)
                return 0;
            *out = row * CHFDEBUG_VRAM_COLS + col;
            return 1;
        }
        return dbg_parse_dec(text, out);
    default:
        return dbg_parse_dec(text, out);
    }
}

static void on_bp_type_changed(GObject *o, GParamSpec *ps, gpointer ud)
{
    DbgWin *w = ud;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(o));
    (void)ps;
    if (sel < (guint)NBPKINDS)
        gtk_entry_set_placeholder_text(w->bp_start, k_bp_kinds[sel].hint);
}

static void on_bp_add(GtkWidget *wgt, gpointer ud)
{
    DbgWin *w = ud;
    guint sel = gtk_drop_down_get_selected(w->bp_type);
    long a = 0, b = 0;
    int id;
    const char *end = gtk_editable_get_text(GTK_EDITABLE(w->bp_end));
    (void)wgt;
    if (sel >= (guint)NBPKINDS) sel = 0;
    if (!bp_parse(w, (int)sel, gtk_editable_get_text(GTK_EDITABLE(w->bp_start)), &a)) {
        gtk_label_set_text(w->bp_msg, "Not a valid start for this kind of breakpoint");
        return;
    }
    b = a;
    if (end && *end && !bp_parse(w, (int)sel, end, &b)) {
        gtk_label_set_text(w->bp_msg, "The end is not valid");
        return;
    }
    if (b < a) {
        gtk_label_set_text(w->bp_msg, "The end comes before the start");
        return;
    }
    if (a < 0 || b > 0xFFFF) {
        gtk_label_set_text(w->bp_msg, "Out of range");
        return;
    }
    id = chfdebug_breakpoint_add(w->dbg, k_bp_kinds[sel].type, k_bp_kinds[sel].space,
                                 (uint16_t)a, (uint16_t)b,
                                 gtk_editable_get_text(GTK_EDITABLE(w->bp_cond)));
    if (id < 0) {
        gtk_label_set_text(w->bp_msg, "The condition does not parse, or the range is outside its space");
        return;
    }
    gtk_label_set_text(w->bp_msg, "");
    gtk_editable_set_text(GTK_EDITABLE(w->bp_start), "");
    gtk_editable_set_text(GTK_EDITABLE(w->bp_end), "");
    gtk_editable_set_text(GTK_EDITABLE(w->bp_cond), "");
    refresh_all(w);
}

static void on_bp_clear(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    chfdebug_breakpoint_clear(w->dbg);
    refresh_all(w);
}

static void load_symbols(DbgWin *w, const char *path)
{
    char msg[512];
    chfdebug_load_symbols(w->dbg, path, msg, sizeof msg);
    prompt_say(w, msg);
    refresh_all(w);
}

static void on_symbols_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    DbgWin *w = ud;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    if (!file) return;
    path = g_file_get_path(file);
    if (!path) return;
    load_symbols(w, path);
}

static void on_symbols(GtkWidget *b, gpointer ud)
{
    DbgWin *w = ud;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *syms = gtk_file_filter_new();
    GtkFileFilter *all = gtk_file_filter_new();
    const char *cart = chfsession_cart_path(w->session);
    (void)b;

    gtk_file_filter_set_name(syms, "Symbol files (*.map, *.lst, *.sym, *.txt)");
    gtk_file_filter_add_suffix(syms, "map");
    gtk_file_filter_add_suffix(syms, "lst");
    gtk_file_filter_add_suffix(syms, "sym");
    gtk_file_filter_add_suffix(syms, "txt");
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, syms);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_title(dlg, "Load Symbols (AS .map / .lst, or AAAA name)");
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    /* a program's symbols usually sit beside it */
    if (cart && *cart) {
        g_autofree char *dir = g_path_get_dirname(cart);
        g_autoptr(GFile) folder = g_file_new_for_path(dir);
        gtk_file_dialog_set_initial_folder(dlg, folder);
    }
    gtk_file_dialog_open(dlg, w->win, NULL, on_symbols_chosen, w);
    g_object_unref(syms);
    g_object_unref(all);
    g_object_unref(filters);
    g_object_unref(dlg);
}

/* <cart>.map/.lst next to the opened cartridge, no picker needed. */
static void on_symbols_beside(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
    load_symbols(w, NULL);
}

static void on_save_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    DbgWin *w = ud;
    g_autoptr(GFile) file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    const char *kind = g_object_get_data(src, "kind");
    char msg[512];
    if (!file) return;
    path = g_file_get_path(file);
    if (!path) return;
    chfdebug_save(w->dbg, kind, path, msg, sizeof msg);
    prompt_say(w, msg);
}

static void on_save(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    const char *kind = g_object_get_data(G_OBJECT(b), "kind");
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_POPOVER);
    char title[80];
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
    g_snprintf(title, sizeof title, "Save %s", (const char *)g_object_get_data(G_OBJECT(b), "title"));
    gtk_file_dialog_set_title(dlg, title);
    gtk_file_dialog_set_initial_name(dlg, g_object_get_data(G_OBJECT(b), "file"));
    g_object_set_data(G_OBJECT(dlg), "kind", (gpointer)kind);
    gtk_file_dialog_save(dlg, w->win, NULL, on_save_chosen, w);
    g_object_unref(dlg);
}

static void on_fuji_clear(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    chfdebug_fujinet_trace_clear(w->dbg);
    refresh_fujinet(w, TRUE);
}

/* Each tab has its own Help topic; the header bar's button follows it. */
static void on_switch_page(GtkNotebook *nb, GtkWidget *page, guint num, gpointer ud)
{
    DbgWin *w = ud;
    (void)nb; (void)page;
    if (num < TAB_COUNT) {
        chf_help_set_context(w->win, k_tab_contexts[num]);
        refresh_tab(w, (int)num);
    }
}

static void hide_window(DbgWin *w)
{
    chfdebug_detach(w->dbg);
    gtk_widget_set_visible(GTK_WIDGET(w->win), FALSE);
}

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint code,
                       GdkModifierType st, gpointer ud)
{
    DbgWin *w = ud;
    (void)c; (void)code;
    switch (keyval) {
    case GDK_KEY_F1: chf_help_for(w->win); return TRUE;
    case GDK_KEY_F5: on_run(NULL, w); return TRUE;
    case GDK_KEY_F7: on_step(NULL, w); return TRUE;
    case GDK_KEY_F8:
        if (st & GDK_SHIFT_MASK) on_step_out(NULL, w); else on_step_over(NULL, w);
        return TRUE;
    case GDK_KEY_F12: hide_window(w); return TRUE;
    default: return FALSE;
    }
}

static gboolean on_close(GtkWindow *win, gpointer ud)
{
    (void)win;
    hide_window(ud);
    return TRUE;
}

/* The refresh timer runs only while the window is on screen. */
static void on_window_map(GtkWidget *widget, gpointer ud)
{
    DbgWin *w = ud;
    (void)widget;
    if (!w->timer)
        w->timer = g_timeout_add(100, tick_refresh, w);
}

static void on_window_unmap(GtkWidget *widget, gpointer ud)
{
    DbgWin *w = ud;
    (void)widget;
    if (w->timer) {
        g_source_remove(w->timer);
        w->timer = 0;
    }
}

/* ---- construction --------------------------------------------------------- */

static GtkWidget *build_toolbar(DbgWin *w)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *b, *sym, *sympop, *beside, *save, *pop, *popbox;
    static const struct { const char *kind, *title, *file; } saves[] = {
        { "dis", "Disassembly ($0000-$47FF as code)", "disassembly.asm" },
        { "mailbox", "Mailbox ($F800-$FFFF)", "mailbox.bin" },
        { "vram", "Video RAM (8192 bytes)", "vram.bin" },
        { "scratch", "Scratchpad (64 bytes)", "scratch.bin" },
        { "cart", "Cartridge image", "cartridge.bin" },
        { "screen", "Screen (PPM)", "screen.ppm" },
        { "vramshot", "Whole VRAM view (PPM)", "vram.ppm" },
        { "regs", "Registers (text)", "registers.txt" } };
    unsigned i;

    gtk_widget_set_margin_start(bar, 8);
    gtk_widget_set_margin_end(bar, 8);
    gtk_widget_set_margin_top(bar, 6);
    gtk_widget_set_margin_bottom(bar, 6);
#define TB(label, cb) do { b = gtk_button_new_with_label(label); \
    g_signal_connect(b, "clicked", G_CALLBACK(cb), w); gtk_box_append(GTK_BOX(bar), b); } while (0)
    w->run_btn = GTK_BUTTON(gtk_button_new_with_label("Stop (F5)"));
    g_signal_connect(w->run_btn, "clicked", G_CALLBACK(on_run), w);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->run_btn));
    TB("Step (F7)", on_step);
    TB("Step Over (F8)", on_step_over);
    TB("Step Out (\xe2\x87\xa7""F8)", on_step_out);
    TB("Frame", on_frame);
    TB("Scanline", on_scanline);
    TB("Run to Cursor", on_run_to);
#undef TB

    /* Load Symbols: a file, or (the arrow) the files beside the cartridge */
    sym = adw_split_button_new();
    adw_split_button_set_label(ADW_SPLIT_BUTTON(sym), "Symbols\xe2\x80\xa6");
    adw_split_button_set_dropdown_tooltip(ADW_SPLIT_BUTTON(sym), "More ways to load symbols");
    g_signal_connect(sym, "clicked", G_CALLBACK(on_symbols), w);
    sympop = gtk_popover_new();
    beside = gtk_button_new_with_label("Beside the Cartridge (.map, .lst)");
    gtk_widget_add_css_class(beside, "flat");
    g_signal_connect(beside, "clicked", G_CALLBACK(on_symbols_beside), w);
    gtk_popover_set_child(GTK_POPOVER(sympop), beside);
    adw_split_button_set_popover(ADW_SPLIT_BUTTON(sym), GTK_POPOVER(sympop));
    gtk_box_append(GTK_BOX(bar), sym);

    save = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(save), "Save\xe2\x80\xa6");
    pop = gtk_popover_new();
    popbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    for (i = 0; i < G_N_ELEMENTS(saves); i++) {
        GtkWidget *sb = gtk_button_new_with_label(saves[i].title);
        gtk_widget_add_css_class(sb, "flat");
        g_object_set_data(G_OBJECT(sb), "kind", (gpointer)saves[i].kind);
        g_object_set_data(G_OBJECT(sb), "title", (gpointer)saves[i].title);
        g_object_set_data(G_OBJECT(sb), "file", (gpointer)saves[i].file);
        g_signal_connect(sb, "clicked", G_CALLBACK(on_save), w);
        gtk_box_append(GTK_BOX(popbox), sb);
    }
    gtk_popover_set_child(GTK_POPOVER(pop), popbox);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(save), pop);
    gtk_box_append(GTK_BOX(bar), save);
    return bar;
}

static GtkWidget *build_prompt(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *scroll = dbg_mono_view(&w->prompt_out, FALSE);
    GtkEventController *keys;

    w->prompt_scroll = GTK_SCROLLED_WINDOW(scroll);
    gtk_text_view_set_wrap_mode(w->prompt_out, GTK_WRAP_WORD_CHAR);
    w->prompt_in = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->prompt_in,
        "Command (help, step, break, bpv, print, mem, poke, scratch, kstack, vram, pal, ports, "
        "cart, disasm, ...) \xe2\x80\x94 Tab completes, Up/Down recall");
    gtk_widget_set_hexpand(GTK_WIDGET(w->prompt_in), TRUE);
    g_signal_connect(w->prompt_in, "activate", G_CALLBACK(on_prompt), w);
    /* ahead of the entry's own text handling, so Tab and Up/Down are ours */
    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_prompt_key), w);
    gtk_widget_add_controller(GTK_WIDGET(w->prompt_in), keys);
    w->history = g_ptr_array_new_with_free_func(g_free);

    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(w->prompt_in));
    dbg_set_text(w->prompt_out, "Channel F debugger prompt (the Fairchild F8 / F3850, the 128 x 64 "
                                "video RAM and the FujiNet cartridge). Type 'help' for every command.\n");
    return dbg_padded(box);
}

static GtkWidget *build_disasm(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkGesture *click = gtk_gesture_click_new();
    GtkEventController *scrollc = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    GtkWidget *hint, *frame;

    w->follow_pc = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Follow PC"));
    gtk_check_button_set_active(w->follow_pc, TRUE);
    g_signal_connect(w->follow_pc, "toggled", G_CALLBACK(on_follow_toggled), w);
    w->jump = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->jump, "$0802 or label");
    gtk_editable_set_width_chars(GTK_EDITABLE(w->jump), 14);
    g_signal_connect(w->jump, "activate", G_CALLBACK(on_jump), w);
    hint = gtk_label_new("Click the margin or double-click a line for a breakpoint; scroll to browse");
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_label_set_ellipsize(GTK_LABEL(hint), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(hint), 1);
    gtk_widget_set_hexpand(hint, TRUE);
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->follow_pc));
    gtk_box_append(GTK_BOX(row), dbg_labeled("Go to", GTK_WIDGET(w->jump)));
    gtk_box_append(GTK_BOX(row), hint);

    /* a drawing area of any height: the lines that fit are the ones read */
    w->dis_area = gtk_drawing_area_new();
    w->dis_sel = -1;
    gtk_widget_set_hexpand(w->dis_area, TRUE);
    gtk_widget_set_vexpand(w->dis_area, TRUE);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(w->dis_area), 120);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(w->dis_area), draw_disasm, w, NULL);
    g_signal_connect(w->dis_area, "resize", G_CALLBACK(on_disasm_resize), w);
    g_signal_connect(click, "pressed", G_CALLBACK(on_disasm_click), w);
    gtk_widget_add_controller(w->dis_area, GTK_EVENT_CONTROLLER(click));
    g_signal_connect(scrollc, "scroll", G_CALLBACK(on_disasm_scroll), w);
    gtk_widget_add_controller(w->dis_area, scrollc);
    frame = gtk_frame_new(NULL);
    gtk_frame_set_child(GTK_FRAME(frame), w->dis_area);
    gtk_widget_set_vexpand(frame, TRUE);

    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), frame);
    w->dis_top = 0;
    return dbg_padded(box);
}

static GtkWidget *build_breaks(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *add, *clear;
    GtkStringList *kinds = gtk_string_list_new(NULL);
    int i;

    for (i = 0; i < NBPKINDS; i++)
        gtk_string_list_append(kinds, k_bp_kinds[i].name);
    w->bp_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(w->bp_list, GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(w->bp_list));
    gtk_widget_set_vexpand(scroll, TRUE);

    w->bp_type = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(kinds), NULL));
    w->bp_start = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->bp_start, k_bp_kinds[0].hint);
    gtk_editable_set_width_chars(GTK_EDITABLE(w->bp_start), 18);
    w->bp_end = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->bp_end, "(end)");
    gtk_editable_set_width_chars(GTK_EDITABLE(w->bp_end), 9);
    w->bp_cond = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->bp_cond, "condition (a == $FF && vpos > 100) \xe2\x80\x94 optional");
    gtk_widget_set_hexpand(GTK_WIDGET(w->bp_cond), TRUE);
    g_signal_connect(w->bp_type, "notify::selected", G_CALLBACK(on_bp_type_changed), w);
    g_signal_connect(w->bp_start, "activate", G_CALLBACK(on_bp_add), w);
    g_signal_connect(w->bp_end, "activate", G_CALLBACK(on_bp_add), w);
    g_signal_connect(w->bp_cond, "activate", G_CALLBACK(on_bp_add), w);
    add = gtk_button_new_with_label("Add");
    g_signal_connect(add, "clicked", G_CALLBACK(on_bp_add), w);
    clear = gtk_button_new_with_label("Clear all");
    g_signal_connect(clear, "clicked", G_CALLBACK(on_bp_clear), w);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_type));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_start));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_end));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_cond));
    gtk_box_append(GTK_BOX(row), add);
    gtk_box_append(GTK_BOX(row), clear);
    w->bp_msg = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->bp_msg), "chf-accent-text");
    gtk_label_set_xalign(w->bp_msg, 0);
    g_object_unref(kinds);

    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(w->bp_msg));
    return dbg_padded(box);
}

/* The packets left (a note and Clear over them), the cartridge's mailbox right. */
static GtkWidget *build_fujinet(DbgWin *w)
{
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *clear = gtk_button_new_with_label("Clear");

    w->fuji_note = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->fuji_note), "dim-label");
    gtk_label_set_wrap(w->fuji_note, TRUE);
    gtk_label_set_width_chars(w->fuji_note, 30);
    gtk_label_set_xalign(w->fuji_note, 0);
    gtk_widget_set_hexpand(GTK_WIDGET(w->fuji_note), TRUE);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_fuji_clear), w);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->fuji_note));
    gtk_box_append(GTK_BOX(row), clear);
    gtk_box_append(GTK_BOX(left), dbg_heading("FujiBus packets (SLIP-decoded)"));
    gtk_box_append(GTK_BOX(left), row);
    gtk_box_append(GTK_BOX(left), dbg_mono_view(&w->fuji_text, FALSE));
    dbg_log_make(w->fuji_text);

    gtk_box_append(GTK_BOX(right), dbg_heading("The cartridge's mailbox"));
    gtk_box_append(GTK_BOX(right), dbg_mono_view(&w->fuji_state, FALSE));
    gtk_text_view_set_wrap_mode(w->fuji_state, GTK_WRAP_WORD_CHAR);

    gtk_paned_set_start_child(GTK_PANED(paned), left);
    gtk_paned_set_end_child(GTK_PANED(paned), right);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), FALSE);
    gtk_widget_set_size_request(left, 560, -1);
    gtk_widget_set_size_request(right, 460, -1);
    w->fuji_count = -1;
    return dbg_padded(paned);
}

static void on_destroy(GtkWidget *widget, gpointer ud)
{
    DbgWin *w = ud;
    (void)widget;
    if (w->timer) g_source_remove(w->timer);
    chfdebug_detach(w->dbg);
    dbg_cpu_free(w->cpu);
    dbg_video_free(w->video);
    dbg_io_free(w->io);
    dbg_cart_free(w->cart);
    if (w->history) g_ptr_array_unref(w->history);
    if (g_win == w) g_win = NULL;
    g_free(w);
}

static void attach_and_refresh(DbgWin *w)
{
    chfdebug_attach(w->dbg);     /* stops the machine */
    w->seen_generation = chfdebug_generation(w->dbg);
    w->was_stopped = chfdebug_is_stopped(w->dbg) != 0;
    refresh_all(w);
}

/* CHF_DEBUGGER_TAB: a page number, or a tab's name (or the start of one). */
static int tab_from_env(void)
{
    const char *tab = g_getenv("CHF_DEBUGGER_TAB");
    char *end;
    long n;
    unsigned i;
    if (!tab || !*tab) return -1;
    n = strtol(tab, &end, 10);
    if (end != tab && *end == '\0')
        return (n >= 0 && n < TAB_COUNT) ? (int)n : -1;
    for (i = 0; i < G_N_ELEMENTS(k_tab_names); i++)
        if (g_ascii_strncasecmp(tab, k_tab_names[i].name, strlen(tab)) == 0)
            return k_tab_names[i].tab;
    return -1;
}

void chf_debugger_show(GtkWindow *parent, chfsession *session)
{
    DbgWin *w;
    GtkWidget *toolbar, *header, *root, *notebook, *overlay, *cpu, *video, *io, *cart;
    GtkEventController *keys;
    int tab;

    if (g_win) {
        gtk_window_present(g_win->win);
        attach_and_refresh(g_win);
        return;
    }
    install_css();
    w = g_new0(DbgWin, 1);
    w->session = session;
    w->dbg = chfsession_debugger(session);
    g_win = w;

    w->win = GTK_WINDOW(adw_window_new());
    gtk_window_set_title(w->win, "Debugger");
    gtk_window_set_default_size(w->win, 1400, 880);
    gtk_window_set_transient_for(w->win, parent);
    gtk_window_set_application(w->win, gtk_window_get_application(parent));
    gtk_window_set_destroy_with_parent(w->win, TRUE);
    g_signal_connect(w->win, "close-request", G_CALLBACK(on_close), w);
    g_signal_connect(w->win, "destroy", G_CALLBACK(on_destroy), w);
    g_signal_connect(w->win, "map", G_CALLBACK(on_window_map), w);
    g_signal_connect(w->win, "unmap", G_CALLBACK(on_window_unmap), w);

    w->cpu = dbg_cpu_new(session, w->dbg, on_tab_changed, w, &cpu);
    w->video = dbg_video_new(session, w->dbg, on_tab_changed, w, &video);
    w->io = dbg_io_new(session, w->dbg, &io);
    w->cart = dbg_cart_new(session, w->dbg, &cart);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(root), build_toolbar(w));
    notebook = gtk_notebook_new();
    w->notebook = GTK_NOTEBOOK(notebook);
    gtk_widget_set_vexpand(notebook, TRUE);
    gtk_notebook_append_page(w->notebook, build_prompt(w), gtk_label_new("Prompt"));
    gtk_notebook_append_page(w->notebook, cpu, gtk_label_new("CPU & RAM"));
    gtk_notebook_append_page(w->notebook, build_disasm(w), gtk_label_new("Disassembly"));
    gtk_notebook_append_page(w->notebook, video, gtk_label_new("Video"));
    gtk_notebook_append_page(w->notebook, io, gtk_label_new("Sound & I/O"));
    gtk_notebook_append_page(w->notebook, build_breaks(w), gtk_label_new("Breakpoints"));
    gtk_notebook_append_page(w->notebook, cart, gtk_label_new("Cart"));
    gtk_notebook_append_page(w->notebook, build_fujinet(w), gtk_label_new("FujiNet"));
    gtk_box_append(GTK_BOX(root), notebook);
    tab = tab_from_env();
    if (tab >= 0) gtk_notebook_set_current_page(w->notebook, tab);
    chf_help_set_context(w->win, k_tab_contexts[tab >= 0 ? tab : 0]);
    g_signal_connect(notebook, "switch-page", G_CALLBACK(on_switch_page), w);

    overlay = adw_toast_overlay_new();
    w->toasts = ADW_TOAST_OVERLAY(overlay);
    adw_toast_overlay_set_child(w->toasts, root);

    /* the status line -- why the machine stopped -- is the title's subtitle */
    header = adw_header_bar_new();
    w->title = ADW_WINDOW_TITLE(adw_window_title_new("Debugger", ""));
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), GTK_WIDGET(w->title));
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), chf_help_button(NULL));
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), overlay);
    adw_window_set_content(ADW_WINDOW(w->win), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), w);
    gtk_widget_add_controller(GTK_WIDGET(w->win), keys);

    attach_and_refresh(w);
    gtk_window_present(w->win);
}

void chf_debugger_toggle(GtkWindow *parent, chfsession *session)
{
    if (g_win && gtk_widget_get_visible(GTK_WIDGET(g_win->win)))
        hide_window(g_win);
    else
        chf_debugger_show(parent, session);
}
