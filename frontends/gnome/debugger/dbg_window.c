/*
 * Debugger window (GTK4/libadwaita) over the session's Fairchild F8 / Channel F
 * debugger engine, via core/include/chfdebug.h: the family's twelve-tab window.
 *
 * Tabs (CHF_DEBUGGER_TAB takes the number, or the start of the name):
 *   0 Console          the contract's command set, Tab completion, history
 *   1 Disassembly      breakpoint gutter (red enabled, hollow grey disabled),
 *                      follow-PC, Go to, Run to Cursor
 *   2 Memory           five spaces (PROG, SCRATCH, VRAM, CART, IO) with Go To
 *                      (a range is highlighted), region labels, search (hex or
 *                      text) and poke; the SCRATCH space adds the 8 x 8 octal
 *                      scratchpad grid (dbg_scratch.c) and the BIOS K stack
 *   3 Breakpoints      why it stopped; execute, read, write, access, port
 *                      in/out, VRAM and scratchpad writes, conditions, hits
 *   4 Watches          addresses, labels and ranges in any space, live values
 *   5 Cart & I/O       the cartridge, the memory map, the mailbox page and its
 *                      events (dbg_cart.c); controllers, the console, ports 0,
 *                      1, 4 and 5 and the 2102 latch (dbg_io.c)
 *   6 Trace            the instruction ring, live while the machine runs
 *   7 History          the same ring, read when the machine stops
 *   8 Video            dbg_video.c over chfvidtab.h
 *   9 Sound            the beeper, its level and the sound-mode timeline
 *  10 Symbols          every known symbol, filtered; a row opens the disassembly
 *  11 FujiNet          every FujiBus packet on the cartridge's link, with hex
 * The registers (A, W and its flags, ISAR in octal, PC, PC0, PC1, DC0, DC1,
 * r59 the K-stack pointer; J H K Q are in the scratchpad grid) sit under the
 * tabs on every tab and are editable while the machine is stopped. Any tab
 * can be popped out into a window of its own (Pop Out, then Dock; closing that
 * window docks it); one tab always stays in the main window. A toolbar carries
 * the stepping controls, breakpoints, symbols and file saves; the family's
 * keys apply (F5 run/stop, F7 step, F8 step over, Shift+F8 step out, Shift+F7
 * frame, F9 breakpoint on the selected line, F12 close, F1 Help).
 *
 * The engine exists only while this window is showing: showing it attaches
 * (which stops the machine, as on every sibling), hiding it detaches and
 * lets the machine run on.
 *
 * The window polls the engine's generation counter on a short timer and
 * refreshes only the tabs in front, and only when something changed, so a
 * stopped machine costs nothing and a running one shows live values twice a
 * second (the Video tab ten times). Every new stop (chfdebug_stop_serial)
 * brings the disassembly back to the PC and the trace and history to their
 * newest line. Environment hooks read at creation: CHF_DEBUGGER_TAB,
 * CHF_WATCH, CHF_WATCH_BREAK, CHF_MEM_GOTO, CHF_HISTORY, CHF_POPOUT (and the
 * CHF_DEBUGGER_VIDEO_* ones the Video tab reads).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <stdlib.h>
#include <string.h>

#include "../help/help_window.h"
#include "../window.h"
#include "chfmemtools.h"
#include "dbg_common.h"

#define DISASM_MAX 160
#define GUTTER 26
#define MEM_ROWS 32
#define MEM_PAGE (MEM_ROWS * 16)
#define POKE_MAX 64
#define TRACE_MAX 512
#define HIST_SHOWN 2000   /* lines on a Trace or History page */
#define HIST_READ 65536   /* the ring */
#define SYMBOLS_SHOWN 2000
#define NSPACES 5

/* The tabs, in order; CHF_DEBUGGER_TAB takes a number or the start of a name. */
enum {
    TAB_CONSOLE, TAB_DISASM, TAB_MEMORY, TAB_BREAKS, TAB_WATCHES, TAB_CART, TAB_TRACE,
    TAB_HISTORY, TAB_VIDEO, TAB_SOUND, TAB_SYMBOLS, TAB_FUJINET, NTABS
};
static const char *const k_tab_names[NTABS] = {
    "Console", "Disassembly", "Memory", "Breakpoints", "Watches", "Cart & I/O", "Trace",
    "History", "Video", "Sound", "Symbols", "FujiNet",
};
/* The old tabs' names, still taken by CHF_DEBUGGER_TAB */
static const struct { const char *name; int tab; int space; } k_tab_aliases[] = {
    { "prompt", TAB_CONSOLE, -1 }, { "cpu", TAB_MEMORY, CHF_SPACE_SCRATCH },
    { "ram", TAB_MEMORY, -1 }, { "scratch", TAB_MEMORY, CHF_SPACE_SCRATCH },
    { "io", TAB_CART, -1 }, { "i/o", TAB_CART, -1 }, { "cart", TAB_CART, -1 },
    { "vram", TAB_VIDEO, -1 },
};
/* Help's contexts (data/help/help.md), one per tab */
static const char *const k_tab_contexts[NTABS] = {
    "debugger/console", "debugger/disassembly", "debugger/memory", "debugger/breakpoints",
    "debugger/watches", "debugger/cart", "debugger/trace", "debugger/history",
    "debugger/video", "debugger/sound", "debugger/symbols", "debugger/fujinet",
};

/* The address spaces of the Memory tab, in the drop-down's order */
static const int k_spaces[NSPACES] = {
    CHF_SPACE_PROG, CHF_SPACE_SCRATCH, CHF_SPACE_VRAM, CHF_SPACE_CART, CHF_SPACE_IO };
static const char *const k_space_labels[NSPACES + 1] = {
    "PROG (CPU view)", "Scratchpad", "VRAM", "Cartridge", "I/O ports", NULL };

/* The breakpoint kinds the form offers, with the space each one's range is in. */
static const struct { const char *name; int type; int space; const char *hint; } k_bp_kinds[] = {
    { "Execute", CHFDEBUG_BP_EXEC, CHF_SPACE_PROG, "$0802 or a label" },
    { "Read", CHFDEBUG_BP_READ, CHF_SPACE_PROG, "$8000 or a label" },
    { "Write", CHFDEBUG_BP_WRITE, CHF_SPACE_PROG, "$8000 or a label" },
    { "Access", CHFDEBUG_BP_ACCESS, CHF_SPACE_PROG, "$8000 or a label (read or write)" },
    { "Port in", CHFDEBUG_BP_IN, CHF_SPACE_IO, "port, e.g. 1 or $01" },
    { "Port out", CHFDEBUG_BP_OUT, CHF_SPACE_IO, "port, e.g. 5 or $05" },
    { "VRAM write", CHFDEBUG_BP_VRAM, CHF_SPACE_VRAM, "row,col e.g. 12,40" },
    { "Scratchpad write", CHFDEBUG_BP_SCRATCH, CHF_SPACE_SCRATCH, "register, e.g. 9 or 40" },
};
#define NBPKINDS ((int)G_N_ELEMENTS(k_bp_kinds))

/* The register strip: name, the engine's register, the digits shown */
enum { R_PC, R_A, R_W, R_ISAR, R_PC0, R_PC1, R_DC0, R_DC1, R_K, R_COUNT };
static const struct { const char *name; int reg; int digits; const char *tip; } k_regs[R_COUNT] = {
    { "PC", CHF_REG_PC, 4, "The instruction about to run (PC0 is one past it: the CPU has "
                           "fetched its opcode)" },
    { "A", CHF_REG_A, 2, "The accumulator" },
    { "W", CHF_REG_W, 2, "Status: ICB O Z C S, S set meaning positive" },
    { "ISAR", CHF_REG_ISAR, 3, "The scratchpad pointer, in octal as upper.lower (6 bits)" },
    { "PC0", CHF_REG_PC0, 4, "The program counter as the bus sees it" },
    { "PC1", CHF_REG_PC1, 4, "The one return address (PI copies PC0 to PC1, POP back)" },
    { "DC0", CHF_REG_DC0, 4, "The data counter" },
    { "DC1", CHF_REG_DC1, 4, "The alternate data counter (XDC swaps)" },
    { "r59", CHF_REG_R0 + 59, 2, "The BIOS K-stack pointer (J H K Q are in the scratchpad grid)" },
};
/* W's bits as check buttons */
static const struct { const char *name; const char *tip; int reg; } k_flags[] = {
    { "S", "Sign: set when the result was positive (the F8's S is inverted)", CHF_REG_SIGN },
    { "C", "Carry", CHF_REG_CARRY },
    { "Z", "Zero", CHF_REG_ZERO },
    { "O", "Overflow", CHF_REG_OVERFLOW },
    { "ICB", "Interrupt control bit (the Channel F never interrupts)", CHF_REG_ICB },
};
#define NFLAGS ((int)G_N_ELEMENTS(k_flags))

typedef struct {
    GtkWindow *win;
    chfsession *session;
    chfdebug *dbg;
    unsigned seen_generation;
    gboolean was_stopped;
    gboolean closing;         /* the window is being destroyed: touch nothing */
    int running_ticks;
    guint timer;

    AdwWindowTitle *title;    /* "Debugger", with the stop reason under it */
    GtkButton *run_btn;
    AdwToastOverlay *toasts;
    GtkNotebook *notebook;

    /* tabs: the page widget of each, and the window it is popped out into */
    GtkWidget *pages[NTABS];
    GtkWindow *detached[NTABS];
    uint64_t seen_serial;     /* chfdebug_stop_serial last acted on */

    /* console */
    GtkTextView *console_out;
    GtkEntry *console_in;
    GPtrArray *history;
    guint history_pos;

    /* the registers strip */
    GtkEntry *reg[R_COUNT];
    GtkWidget *flag[NFLAGS];
    GtkLabel *k_depth, *info;
    GtkWidget *pill_illegal;
    gboolean syncing;         /* the flag boxes are being set from the engine */
    gboolean force_entries;   /* a refresh may overwrite a focused entry */

    /* memory */
    GtkDropDown *mem_space;
    GtkTextView *mem_view;
    GtkEntry *mem_goto, *mem_write;
    uint32_t mem_base[NSPACES];
    gboolean mem_hl;          /* a range is highlighted (Go to, a search hit, a watch) */
    int mem_hl_space;
    uint32_t mem_hl_lo, mem_hl_hi;
    GtkDropDown *srch_kind;
    GtkEntry *srch_entry;
    GtkLabel *srch_note;
    GtkWidget *scratch_box;
    DbgScratch *scratch;

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
    GtkEntry *break_at;       /* the toolbar's "Break at" box */
    GtkLabel *bp_msg;
    GtkLabel *bp_stop;        /* "Stopped: ..." / "Running." */
    GString *bp_sig;          /* what the list shows, to skip a needless rebuild */

    /* watches */
    GtkDropDown *wt_space;
    GtkEntry *wt_entry;
    GtkListBox *wt_list;
    GPtrArray *wt_labels;     /* a GtkLabel per row, rewritten in place */
    unsigned wt_gen;
    gboolean wt_built;

    /* cart & i/o, sound */
    DbgCart *cart;
    DbgIo *io;

    /* symbols */
    GtkEntry *sym_filter;
    GtkListBox *sym_list;
    GtkLabel *sym_note;
    GString *sym_sig;         /* what the list shows, to skip a needless rebuild */

    /* trace (0) and history (1): the same ring, two views */
    GtkToggleButton *h_rec[2];
    GtkDropDown *h_filter[2];
    GtkEntry *h_range[2];
    GtkLabel *h_note[2];
    GtkTextView *h_text[2];
    uint64_t h_sig[2];        /* what each page last drew; ~0 forces a redraw */
    gboolean h_sync;          /* the Record toggles are being set from the engine */
    int trace_ticks;

    /* fujinet */
    GtkLabel *fuji_note;
    GtkTextView *fuji_text, *fuji_state;
    uint64_t fuji_last;
    int fuji_count;
} DbgWin;

static DbgWin *g_win;

/* ---- helpers (shared with the other files) ------------------------------- */

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
    /* tied to the view: the adjustment can outlive it while the window dies */
    g_signal_connect_object(vadj(view), "changed", G_CALLBACK(on_log_resized), view, 0);
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
    char *valid = g_utf8_make_valid(text, -1);
    text = valid;

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
        if (same) {
            g_free(valid);
            if (follow) {
                g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
                if (adj) on_log_resized(adj, view);
            }
            return;
        }
    }
    gtk_text_buffer_set_text(b, text, -1);
    g_free(valid);
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

/* ---- window-level helpers --------------------------------------------------- */

/* Engine text for a text buffer: valid UTF-8, control bytes dropped. */
static char *clean_text(const char *text)
{
    char *v = g_utf8_make_valid(text, -1), *s, *d;
    for (s = d = v; *s; s++)
        if ((unsigned char)*s >= 0x20 || *s == '\n' || *s == '\t') *d++ = *s;
    *d = '\0';
    return v;
}

/* Text added to a log (the console's transcript): it follows its end while
 * its reader is there. */
static void append_text(GtkTextView *view, const char *text, gboolean force_follow)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(view);
    GtkTextIter end;
    char *clean = clean_text(text);
    dbg_log_note_reader(view);
    if (force_follow)
        g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
    gtk_text_buffer_get_end_iter(b, &end);
    gtk_text_buffer_insert(b, &end, clean, -1);
    g_free(clean);
}

/* An address box: a label (as the engine does, a name wins over a hex
 * literal), bare hex, or any expression the console takes ($0802+4, pc,
 * label+2). 0 with the reason in err. */
static int parse_addr(DbgWin *w, const char *text, long *out, char *err, int errsz)
{
    char buf[128];
    long v;
    int a;
    uint32_t range_lo, range_hi;
    g_strlcpy(buf, text, sizeof buf);
    g_strstrip(buf);
    if (!buf[0]) {
        g_strlcpy(err, "Enter an address, a label or an expression", errsz);
        return 0;
    }
    a = chfdebug_label_address(w->dbg, buf);
    if (a >= 0) { *out = a & 0xFFFF; return 1; }
    if (dbg_parse_num(buf, &v)) {
        if (v < 0 || v > 0xFFFF) {
            g_strlcpy(err, "Out of range (0 to $FFFF)", errsz);
            return 0;
        }
        *out = v;
        return 1;
    }
    /* the range parser takes any expression the console does (label+3, p[ptr], pc) */
    if (!chfsym_parse_range(w->dbg, CHF_SPACE_PROG, buf, &range_lo, &range_hi)) {
        g_strlcpy(err, "Not an address or a label", errsz);
        return 0;
    }
    *out = range_lo;
    return 1;
}

/* "vram:10,20", "scratch:r12": a space name (or the start of one) and a colon
 * before the text, for the capture variables. No prefix: the default space. */
static const char *split_space(const char *text, int dflt, int *space)
{
    static const char *const names[NSPACES] = { "prog", "scratch", "vram", "cart", "io" };
    const char *colon = strchr(text, ':');
    int i;
    *space = dflt;
    if (!colon || colon == text || colon - text > 8) return text;
    for (i = 0; i < NSPACES; i++)
        if (g_ascii_strncasecmp(text, names[i], (gsize)(colon - text)) == 0) {
            *space = k_spaces[i];
            return colon + 1;
        }
    return text;
}

static int space_index(int space)
{
    int i;
    for (i = 0; i < NSPACES; i++)
        if (k_spaces[i] == space) return i;
    return 0;
}

/* The tab text of a space when it is the Memory tab's: how many hex digits
 * its addresses take. */
static int space_digits(chfdebug *d, int space)
{
    const int size = chfdebug_space_size(d, space);
    return size > 0x10000 ? 5 : size > 0x100 ? 4 : 2;
}

static void notice(DbgWin *w, const char *msg)
{
    char line[700];
    g_snprintf(line, sizeof line, "%s\n", msg);
    append_text(w->console_out, line, FALSE);
    adw_toast_overlay_add_toast(w->toasts, adw_toast_new(msg));
}

static void toast(DbgWin *w, const char *msg)
{
    adw_toast_overlay_add_toast(w->toasts, adw_toast_new(msg));
}

/* The step controls wait a moment for the engine to come to rest, so a
 * step does not flash "Running" before the status catches up. */
static void settle(DbgWin *w, gboolean want_stopped)
{
    int i;
    for (i = 0; i < 20 && (chfdebug_is_stopped(w->dbg) != 0) != want_stopped; i++)
        g_usleep(2000);
}

static void install_css(void)
{
    static gboolean done;
    GtkCssProvider *css;
    char text[512];
    if (done) return;
    done = TRUE;
    css = gtk_css_provider_new();
    g_snprintf(text, sizeof text,
        "entry.chf-hex { padding: 1px 2px; min-height: 0; min-width: 0; }\n"
        "entry.chf-hex > text { min-width: 0; }\n"
        "levelbar.chf-meter block.filled { background-color: #%06x; }\n"
        "button.chf-toggle:checked, togglebutton.chf-toggle:checked {"
        " background-image: none; background-color: #%06x; color: #1e1a14; }\n",
        CHFSESSION_ACCENT_RGB, CHFSESSION_ACCENT_RGB);
    gtk_css_provider_load_from_string(css, text);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

/* A refresh does not overwrite a value being typed: a focused entry whose
 * text is not what was last put there keeps it, until Enter (force_entries)
 * says to take the engine's. */
static void set_entry(DbgWin *w, GtkEntry *e, const char *text)
{
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(e));
    GtkWidget *focus = root ? gtk_root_get_focus(root) : NULL;
    const char *shown = g_object_get_data(G_OBJECT(e), "shown");
    if (focus && !w->force_entries && shown &&
        (focus == GTK_WIDGET(e) || gtk_widget_is_ancestor(focus, GTK_WIDGET(e))) &&
        strcmp(gtk_editable_get_text(GTK_EDITABLE(e)), shown) != 0)
        return;
    gtk_editable_set_text(GTK_EDITABLE(e), text);
    g_object_set_data_full(G_OBJECT(e), "shown", g_strdup(text), g_free);
}

/* ---- tabs, and tabs popped out into windows of their own -------------------- */

static void refresh_now(DbgWin *w);

static int page_tab(GtkWidget *page)
{
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(page), "tab"));
}

/* The tab showing in the main window (-1 if the notebook is empty). */
static int current_tab(DbgWin *w)
{
    GtkWidget *page = gtk_notebook_get_nth_page(w->notebook,
                                                gtk_notebook_get_current_page(w->notebook));
    return page ? page_tab(page) : -1;
}

/* In front of the user: the current page, or a tab popped out. */
static gboolean tab_visible(DbgWin *w, int tab)
{
    return w->detached[tab] != NULL || current_tab(w) == tab;
}

static void show_tab(DbgWin *w, int tab)
{
    if (w->detached[tab])
        gtk_window_present(w->detached[tab]);
    else
        gtk_notebook_set_current_page(w->notebook,
                                      gtk_notebook_page_num(w->notebook, w->pages[tab]));
}

/* ---- the register strip --------------------------------------------------------- */

static void refresh_cpu(DbgWin *w)
{
    chfdebug_cpu c;
    char v[R_COUNT][12], buf[400];
    int i;

    chfdebug_cpu_get(w->dbg, &c);
    if (c.pc >= 0) g_snprintf(v[R_PC], sizeof v[0], "%04X", c.pc);
    else g_strlcpy(v[R_PC], "----", sizeof v[0]);
    g_snprintf(v[R_A], sizeof v[0], "%02X", c.a);
    g_snprintf(v[R_W], sizeof v[0], "%02X", c.w);
    g_snprintf(v[R_ISAR], sizeof v[0], "%o.%o", (c.isar >> 3) & 7, c.isar & 7);
    g_snprintf(v[R_PC0], sizeof v[0], "%04X", c.pc0);
    g_snprintf(v[R_PC1], sizeof v[0], "%04X", c.pc1);
    g_snprintf(v[R_DC0], sizeof v[0], "%04X", c.dc0);
    g_snprintf(v[R_DC1], sizeof v[0], "%04X", c.dc1);
    g_snprintf(v[R_K], sizeof v[0], "%02X", c.r[59]);
    for (i = 0; i < R_COUNT; i++)
        set_entry(w, w->reg[i], v[i]);

    w->syncing = TRUE;
    {
        const int f[NFLAGS] = { c.sign, c.carry, c.zero, c.overflow, c.icb };
        for (i = 0; i < NFLAGS; i++)
            gtk_check_button_set_active(GTK_CHECK_BUTTON(w->flag[i]), f[i] != 0);
    }
    w->syncing = FALSE;

    if (c.k_depth >= 0) g_snprintf(buf, sizeof buf, "K depth %d", c.k_depth);
    else g_snprintf(buf, sizeof buf, "K stack unused");
    gtk_label_set_text(w->k_depth, buf);

    gtk_widget_set_visible(w->pill_illegal, c.illegal_count != 0);
    dbg_pill(w->pill_illegal, TRUE);
    g_snprintf(buf, sizeof buf,
               "%s   BIOS %s   line %d   cycle %d   frame %u   clocks %llu   instructions %llu",
               chf_machine_name(c.machine),
               c.bios == CHF_ROM_SL31253 ? "sl31253" : c.bios == CHF_ROM_SL90025 ? "sl90025"
               : "stand-in", c.vpos, c.hpos, c.frame, (unsigned long long)c.cycles,
               (unsigned long long)c.instructions);
    if (c.illegal_count) {
        char at[60];
        g_snprintf(at, sizeof at, "   illegal %u (last $%04X)", c.illegal_count, c.illegal_pc & 0xFFFF);
        g_strlcat(buf, at, sizeof buf);
    }
    gtk_label_set_text(w->info, buf);
}

static void on_reg_activate(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    const int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(entry), "reg"));
    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    long v;

    if (!chfdebug_is_stopped(w->dbg)) {
        toast(w, "Stop the machine to edit the registers");
    } else if (i == R_ISAR) {
        /* octal, "u.l" or "ul" */
        char digits[8];
        int n = 0;
        const char *p;
        for (p = text; *p && n < 6; p++)
            if (*p >= '0' && *p <= '7') digits[n++] = *p;
        digits[n] = '\0';
        if (n > 0)
            chfdebug_cpu_set(w->dbg, CHF_REG_ISAR, (int)(strtol(digits, NULL, 8) & 0x3F));
    } else if (dbg_parse_num(text, &v)) {
        chfdebug_cpu_set(w->dbg, k_regs[i].reg, (int)v);
    }
    /* Enter ends the edit: show the engine's value, or put the old one back */
    w->force_entries = TRUE;
    refresh_now(w);
    w->force_entries = FALSE;
}

static void on_flag_toggled(GtkCheckButton *b, gpointer ud)
{
    DbgWin *w = ud;
    const int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "flag"));
    if (w->syncing) return;
    if (!chfdebug_is_stopped(w->dbg)) {
        toast(w, "Stop the machine to edit the flags");
    } else {
        chfdebug_cpu_set(w->dbg, k_flags[i].reg, gtk_check_button_get_active(b) ? 1 : 0);
    }
    w->force_entries = TRUE;
    refresh_now(w);
    w->force_entries = FALSE;
}

/* ---- memory ----------------------------------------------------------------------- */

static int mem_space_now(DbgWin *w)
{
    const guint sel = gtk_drop_down_get_selected(w->mem_space);
    return sel < NSPACES ? k_spaces[sel] : CHF_SPACE_PROG;
}

/* The dump: a row is the address, 16 bytes, then the text. The highlighted
 * range gets the accent background on its bytes (hex and text), with the gap
 * between two highlighted bytes filled so it reads as a bar. */
static void refresh_mem(DbgWin *w)
{
    uint8_t data[MEM_PAGE];
    chfdebug_region map[32];
    char region[64];
    GString *str = g_string_new(NULL);
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->mem_view);
    const int space = mem_space_now(w);
    const uint32_t size = (uint32_t)chfdebug_space_size(w->dbg, space);
    const uint32_t base = w->mem_base[space_index(space)];
    const int digits = space_digits(w->dbg, space);
    const int regions = space == CHF_SPACE_PROG ? chfdebug_memory_map(w->dbg, map, 32) : 0;
    int row, col, rows;

    memset(data, 0, sizeof data);
    if (size == 0) {
        dbg_set_text(w->mem_view, "(no machine: nothing to show)\n");
        g_string_free(str, TRUE);
        return;
    }
    rows = (int)((size - base + 15) / 16);
    if (rows > MEM_ROWS) rows = MEM_ROWS;
    chfdebug_read(w->dbg, space, base, data, rows * 16);
    g_string_append_printf(str, "%*s  0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n", digits, "");
    for (row = 0; row < rows; row++) {
        const uint32_t a = base + (uint32_t)row * 16;
        g_string_append_printf(str, "%0*X  ", digits, a);
        for (col = 0; col < 16; col++) {
            if (a + (uint32_t)col < size)
                g_string_append_printf(str, "%02X ", data[row * 16 + col]);
            else
                g_string_append(str, "-- ");
        }
        g_string_append_c(str, ' ');
        for (col = 0; col < 16; col++) {
            uint8_t ch = data[row * 16 + col];
            g_string_append_c(str, (a + (uint32_t)col < size && ch >= 0x20 && ch <= 0x7E)
                                       ? (char)ch : '.');
        }
        if (space == CHF_SPACE_PROG) {
            /* the hardware answering at the row's first byte, from one map fetch */
            chfmem_region_name(map, regions, (uint16_t)a, region, sizeof region);
            if (region[0]) g_string_append_printf(str, "  %s", region);
        } else if (space == CHF_SPACE_VRAM) {
            g_string_append_printf(str, "  row %d col %d-%d", (int)(a / CHFDEBUG_VRAM_COLS),
                                   (int)(a % CHFDEBUG_VRAM_COLS), (int)(a % CHFDEBUG_VRAM_COLS) + 15);
        } else if (space == CHF_SPACE_SCRATCH) {
            g_string_append_printf(str, "  r%d-r%d", (int)a, (int)a + 15);
        } else if (space == CHF_SPACE_IO) {
            g_string_append_printf(str, "  ports $%02X-$%02X", (int)a, (int)a + 15);
        }
        g_string_append_c(str, '\n');
    }
    dbg_set_text(w->mem_view, str->str);
    g_string_free(str, TRUE);

    {   /* unchanged text keeps its tags: clear the old highlight first */
        GtkTextIter s, e;
        gtk_text_buffer_get_bounds(b, &s, &e);
        gtk_text_buffer_remove_tag_by_name(b, "mark", &s, &e);
    }
    if (w->mem_hl && w->mem_hl_space == space) {
        uint32_t a;
        for (a = base; a < base + (uint32_t)rows * 16; a++) {
            GtkTextIter s, e;
            int line, off;
            gboolean next;
            if (a < w->mem_hl_lo || a > w->mem_hl_hi) continue;
            line = (int)(a - base) / 16 + 1;
            col = (int)(a - base) % 16;
            next = a < w->mem_hl_hi && col < 15;
            gtk_text_buffer_get_iter_at_line_offset(b, &s, line, digits + 2 + col * 3);
            gtk_text_buffer_get_iter_at_line_offset(b, &e, line, digits + 2 + col * 3 + (next ? 3 : 2));
            gtk_text_buffer_apply_tag_by_name(b, "mark", &s, &e);
            off = digits + 2 + 48 + 1 + col;
            gtk_text_buffer_get_iter_at_line_offset(b, &s, line, off);
            gtk_text_buffer_get_iter_at_line_offset(b, &e, line, off + 1);
            gtk_text_buffer_apply_tag_by_name(b, "mark", &s, &e);
        }
    }
    if (space == CHF_SPACE_SCRATCH)
        dbg_scratch_refresh(w->scratch, !chfdebug_is_stopped(w->dbg));
}

static void mem_set_base(DbgWin *w, uint32_t base)
{
    const int space = mem_space_now(w);
    const long size = chfdebug_space_size(w->dbg, space);
    long max = size - MEM_PAGE;
    long b = (long)(int32_t)base;
    if (max < 0) max = 0;
    if (b < 0) b = 0;
    if (b > max) b = max;
    w->mem_base[space_index(space)] = (uint32_t)b & ~0xFu;
    refresh_mem(w);
}

static void on_mem_space(GObject *o, GParamSpec *ps, gpointer ud)
{
    DbgWin *w = ud;
    (void)o; (void)ps;
    gtk_widget_set_visible(w->scratch_box, mem_space_now(w) == CHF_SPACE_SCRATCH);
    refresh_mem(w);
}

/* A range in a space, highlighted, with a little context above it. */
static void show_in_memory_space(DbgWin *w, int space, uint32_t lo, uint32_t hi)
{
    w->mem_hl = TRUE;
    w->mem_hl_space = space;
    w->mem_hl_lo = lo;
    w->mem_hl_hi = hi;
    gtk_drop_down_set_selected(w->mem_space, (guint)space_index(space));
    gtk_widget_set_visible(w->scratch_box, space == CHF_SPACE_SCRATCH);
    if (space == CHF_SPACE_SCRATCH && lo < 64)
        dbg_scratch_select(w->scratch, (int)lo);
    mem_set_base(w, (lo & ~0xFu) >= 4 * 16 ? (lo & ~0xFu) - 4 * 16 : 0);
}

/* Go to: an address, a label, an expression or a range, in the space showing. */
static void on_mem_goto(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    uint32_t lo, hi;
    const int space = mem_space_now(w);
    if (!chfsym_parse_range(w->dbg, space, gtk_editable_get_text(GTK_EDITABLE(entry)), &lo, &hi)) {
        toast(w, "Not an address, a label or a range in this space");
        return;
    }
    show_in_memory_space(w, space, lo, hi);
}

/* "address value...": bytes written into the space showing. */
static void on_mem_write(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    char **parts = g_strsplit_set(gtk_editable_get_text(GTK_EDITABLE(entry)), " \t:=,", -1);
    uint8_t bytes[POKE_MAX];
    uint32_t lo = 0, hi = 0;
    long v;
    int n = 0, i;
    char *addr = NULL;
    const int space = mem_space_now(w);
    for (i = 0; parts[i]; i++) {
        if (!*parts[i]) continue;
        if (!addr) addr = parts[i];
        else if (n < POKE_MAX && dbg_parse_num(parts[i], &v) && v >= 0 && v <= 255)
            bytes[n++] = (uint8_t)v;
        else { n = -1; break; }
    }
    if (addr && n > 0 && chfsym_parse_range(w->dbg, space, addr, &lo, &hi)) {
        for (i = 0; i < n; i++)
            chfdebug_write(w->dbg, space, lo + (uint32_t)i, bytes[i]);
        gtk_editable_set_text(GTK_EDITABLE(entry), "");
        refresh_now(w);
    } else {
        toast(w, "Poke takes an address and hex bytes, e.g. 8010 41 42");
    }
    g_strfreev(parts);
}

/* Find Next / Find Previous: from the highlight (else the top of the page). */
static void on_search(GtkWidget *button, gpointer ud)
{
    DbgWin *w = ud;
    const int backward = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "back"));
    const int space = mem_space_now(w);
    uint8_t pat[CHFSEARCH_MAX], mask;
    int n, flags, r;
    uint32_t from, found = 0, base = w->mem_base[space_index(space)];
    const uint32_t size = (uint32_t)chfdebug_space_size(w->dbg, space);

    if (!chfsearch_parse((int)gtk_drop_down_get_selected(w->srch_kind),
                         gtk_editable_get_text(GTK_EDITABLE(w->srch_entry)),
                         pat, &n, &mask, &flags)) {
        gtk_label_set_text(w->srch_note, "Type hex bytes (F8 00) or some text");
        return;
    }
    if (!chfdebug_is_stopped(w->dbg)) {
        gtk_label_set_text(w->srch_note, "Stop the machine first");
        return;
    }
    if (w->mem_hl && w->mem_hl_space == space)
        from = w->mem_hl_lo;
    else
        from = backward ? base : (base > 0 ? base - 1 : (size > 0 ? size - 1 : 0));
    r = chfdebug_search_next(w->dbg, space, pat, n, mask, flags, from, backward, &found);
    if (r > 0) {
        char msg[64];
        g_snprintf(msg, sizeof msg, "Found at $%0*X", space_digits(w->dbg, space), found);
        show_in_memory_space(w, space, found, found + (uint32_t)n - 1);
        gtk_label_set_text(w->srch_note, msg);
    } else if (r == 0) {
        gtk_label_set_text(w->srch_note, "Not found");
    } else {
        gtk_label_set_text(w->srch_note, "Stop the machine first");
    }
}

static void on_search_activate(GtkEntry *entry, gpointer ud)
{
    on_search(g_object_get_data(G_OBJECT(entry), "next-button"), ud);
}

static void on_mem_page(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    const int space = mem_space_now(w);
    const long delta = (long)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "delta")) * MEM_PAGE;
    mem_set_base(w, (uint32_t)((long)w->mem_base[space_index(space)] + delta));
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
        } else if (l->bp_disabled) {
            cairo_arc(cr, GUTTER / 2.0 - 2, y + w->dis_lh / 2, 5, 0, 2 * G_PI);
            dbg_set_rgb(cr, 0x8A8A8A, 1.0);
            cairo_set_line_width(cr, 1.5);
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
    refresh_now(w);
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
    char err[128] = "";
    long a;
    if (!parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(entry)), &a, err, sizeof err)) {
        toast(w, err);
        return;
    }
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

/* The packet log is rebuilt only when it changed: a new packet, or a
 * repeat (which moves the newest serial on). */
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

/* "Stopped: breakpoint ... at $0802", "Stopped at $0802" or "Running". */
static void stop_line(DbgWin *w, char *line, int size)
{
    char reason[200];
    int addr;
    if (chfdebug_is_stopped(w->dbg)) {
        gboolean plain, restated;
        chfdebug_stop_reason(w->dbg, reason, sizeof reason, &addr);
        plain = !reason[0] || strcmp(reason, "stopped") == 0;
        restated = g_str_has_prefix(reason, "stopped ");   /* "stopped at $0802" */
        if (restated)
            g_snprintf(line, size, "S%s", reason + 1);
        else
            g_snprintf(line, size, "Stopped%s%s", plain ? "" : ": ", plain ? "" : reason);
        if (addr >= 0 && !strchr(reason, '$'))
            g_snprintf(line + strlen(line), size - (int)strlen(line), " at $%04X", addr);
    } else {
        g_snprintf(line, size, "Running");
    }
}

static void refresh_bps(DbgWin *w)
{
    static chfdebug_breakpoint bps[128];
    GtkWidget *child;
    GString *sig = g_string_new(NULL);
    int n = chfdebug_breakpoint_list(w->dbg, bps, 128), i;

    {
        char line[400];
        stop_line(w, line, sizeof line);
        if (!chfdebug_is_stopped(w->dbg)) g_strlcat(line, ".", sizeof line);
        gtk_label_set_text(w->bp_stop, line);
    }

    /* the list is rebuilt only when what it shows changed (hit counts move
     * while the machine runs), so a click on a row is not pulled out from
     * under the pointer */
    for (i = 0; i < n; i++)
        g_string_append_printf(sig, "%d/%d/%u-%u/%d/%u/%s\n", bps[i].id, bps[i].type,
                               bps[i].start, bps[i].end, bps[i].enabled, bps[i].hits,
                               bps[i].condition);
    if (w->bp_sig && g_string_equal(sig, w->bp_sig)) {
        g_string_free(sig, TRUE);
        return;
    }
    if (w->bp_sig) g_string_free(w->bp_sig, TRUE);
    w->bp_sig = sig;

    while ((child = gtk_widget_get_first_child(GTK_WIDGET(w->bp_list))) != NULL)
        gtk_list_box_remove(w->bp_list, child);
    if (n == 0) {
        GtkWidget *l = gtk_label_new("No breakpoints. Click a disassembly line's margin (or press F9 "
                                     "on a selected line), or add one below.");
        gtk_widget_add_css_class(l, "dim-label");
        gtk_list_box_append(w->bp_list, l);
        return;
    }
    for (i = 0; i < n; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *en = gtk_check_button_new();
        GtkWidget *rm = gtk_button_new_from_icon_name("user-trash-symbolic");
        GtkWidget *l;
        char text[420], desc[300];
        chfbp_describe(w->dbg, &bps[i], desc, sizeof desc);
        g_snprintf(text, sizeof text, "#%-3d %s    hits %u", bps[i].id, desc, bps[i].hits);
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

/* ---- symbols ------------------------------------------------------------------ */

/* A row opens the disassembly at its address; a port opens the I/O space. */
static void on_sym_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer ud)
{
    DbgWin *w = ud;
    (void)box;
    if (!row) return;
    if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "port"))) {
        const uint32_t port = (uint32_t)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "addr"));
        show_in_memory_space(w, CHF_SPACE_IO, port, port);
        show_tab(w, TAB_MEMORY);
        return;
    }
    gtk_check_button_set_active(w->follow_pc, FALSE);
    w->dis_top = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "addr"));
    show_tab(w, TAB_DISASM);
    refresh_disasm(w);
}

/* The index order is unstable (loading, clearing and lookups reorder it), so
 * the list is fetched through the filter every time and rebuilt only when
 * what it would show changed. */
static void refresh_symbols(DbgWin *w)
{
    static int indices[SYMBOLS_SHOWN + 1];
    static uint16_t addresses[SYMBOLS_SHOWN];
    static int banks[SYMBOLS_SHOWN];
    static char names[SYMBOLS_SHOWN][64];
    GString *sig = g_string_new(NULL);
    const char *filter = gtk_editable_get_text(GTK_EDITABLE(w->sym_filter));
    const int total = chfdebug_symbols_count(w->dbg);
    const int matched = chfdebug_symbols_filter(w->dbg, filter, indices, SYMBOLS_SHOWN + 1);
    const int shown = matched > SYMBOLS_SHOWN ? SYMBOLS_SHOWN : matched;
    int i;
    char note[160];

    for (i = 0; i < shown; i++) {
        if (!chfdebug_symbols_get(w->dbg, indices[i], &addresses[i], names[i],
                                  sizeof names[i], &banks[i])) {
            addresses[i] = 0;
            names[i][0] = '\0';
            banks[i] = CHFDEBUG_SYM_ANY;
        }
        g_string_append_printf(sig, "%04X/%d/%s\n", addresses[i], banks[i], names[i]);
    }
    g_string_append_printf(sig, "%d/%d", matched, total);
    if (w->sym_sig && g_string_equal(sig, w->sym_sig)) {
        g_string_free(sig, TRUE);
        return;
    }
    if (w->sym_sig) g_string_free(w->sym_sig, TRUE);
    w->sym_sig = sig;

    gtk_list_box_remove_all(w->sym_list);
    for (i = 0; i < shown; i++) {
        GtkWidget *row_label;
        GtkWidget *row = gtk_list_box_row_new();
        char text[160];
        const gboolean port = banks[i] == CHFDEBUG_SYM_PORT;
        if (port)
            g_snprintf(text, sizeof text, "port $%02X  %s", addresses[i], names[i]);
        else if (banks[i] >= 0)
            g_snprintf(text, sizeof text, "$%04X  %-32s bank %d", addresses[i], names[i], banks[i]);
        else
            g_snprintf(text, sizeof text, "$%04X  %s", addresses[i], names[i]);
        row_label = gtk_label_new(text);
        gtk_label_set_xalign(GTK_LABEL(row_label), 0);
        gtk_widget_add_css_class(row_label, "monospace");
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), row_label);
        g_object_set_data(G_OBJECT(row), "addr", GINT_TO_POINTER(addresses[i]));
        g_object_set_data(G_OBJECT(row), "port", GINT_TO_POINTER(port));
        gtk_list_box_append(w->sym_list, row);
    }
    if (matched > SYMBOLS_SHOWN)
        g_snprintf(note, sizeof note, "%d of %d symbols match; the first %d are shown "
                   "(narrow the filter)", matched, total, SYMBOLS_SHOWN);
    else
        g_snprintf(note, sizeof note, "%d of %d symbol%s", matched, total, total == 1 ? "" : "s");
    gtk_label_set_text(w->sym_note, note);
}

/* a new filter, typed or cleared: show it now */
static void on_sym_filter_changed(GtkEditable *editable, gpointer ud)
{
    (void)editable;
    refresh_symbols(ud);
}

/* ---- watches ------------------------------------------------------------------ */

static void on_wt_remove(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    chfwatch_remove(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "wt-idx")));
    refresh_now(w);
}

static void on_wt_memory(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    const int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "wt-idx"));
    uint32_t lo, hi;
    if (!chfwatch_range(w->dbg, index, &lo, &hi)) {
        toast(w, "That watch does not resolve to an address yet");
        return;
    }
    show_in_memory_space(w, chfwatch_space(index), lo, hi);
    show_tab(w, TAB_MEMORY);
}

/* Break when the watched bytes change: a write whose value differs from the
 * one it replaced, in the space the watch is in. */
static void break_on_change(DbgWin *w, int space, uint32_t lo, uint32_t hi)
{
    int type, id;
    const char *cond = "value != old";
    switch (space) {
    case CHF_SPACE_PROG: type = CHFDEBUG_BP_WRITE; break;
    case CHF_SPACE_VRAM: type = CHFDEBUG_BP_VRAM; break;
    case CHF_SPACE_IO: type = CHFDEBUG_BP_OUT; break;
    case CHF_SPACE_SCRATCH: type = CHFDEBUG_BP_SCRATCH; cond = ""; break;   /* already a change */
    default:
        toast(w, "The cartridge's own memory cannot break; watch the PROG address instead");
        return;
    }
    id = chfdebug_breakpoint_add(w->dbg, type, space, (uint16_t)lo, (uint16_t)hi, cond);
    toast(w, id < 0 ? "The breakpoint was refused" : "Breaking when it changes");
}

static void on_wt_break(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    const int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "wt-idx"));
    uint32_t lo, hi;
    if (!chfwatch_range(w->dbg, index, &lo, &hi)) {
        toast(w, "That watch does not resolve to an address yet");
        return;
    }
    break_on_change(w, chfwatch_space(index), lo, hi);
    refresh_now(w);
}

static void wt_rebuild(DbgWin *w)
{
    const int n = chfwatch_count();
    int i;

    gtk_list_box_remove_all(w->wt_list);
    g_ptr_array_set_size(w->wt_labels, 0);
    if (n == 0) {
        GtkWidget *l = gtk_label_new("No watches. Type an address, a label, label+3, p[ptr] "
                                     "or a range (8000-80FF) above, in the space chosen.");
        gtk_widget_add_css_class(l, "dim-label");
        gtk_list_box_append(w->wt_list, l);
    }
    for (i = 0; i < n; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *label = gtk_label_new(NULL);
        static const struct { const char *text, *tip; GCallback cb; } btns[] = {
            { "Memory", "Show it in the Memory tab", G_CALLBACK(on_wt_memory) },
            { "Break on change", "Stop when a write changes it", G_CALLBACK(on_wt_break) },
            { "Remove", "Forget this watch", G_CALLBACK(on_wt_remove) },
        };
        size_t k;
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_widget_set_hexpand(label, TRUE);
        gtk_widget_add_css_class(label, "monospace");
        gtk_box_append(GTK_BOX(row), label);
        for (k = 0; k < G_N_ELEMENTS(btns); k++) {
            GtkWidget *b = gtk_button_new_with_label(btns[k].text);
            gtk_widget_add_css_class(b, "flat");
            gtk_widget_set_tooltip_text(b, btns[k].tip);
            g_object_set_data(G_OBJECT(b), "wt-idx", GINT_TO_POINTER(i));
            g_signal_connect(b, "clicked", btns[k].cb, w);
            gtk_box_append(GTK_BOX(row), b);
        }
        gtk_list_box_append(w->wt_list, row);
        g_ptr_array_add(w->wt_labels, label);
    }
}

/* The rows are rebuilt only when the list changed; the values are written
 * into the labels in place, so a click on a row's button is never pulled
 * from under the pointer. */
static void refresh_watches(DbgWin *w)
{
    const unsigned gen = chfwatch_generation();
    int i;
    if (!w->wt_built || gen != w->wt_gen || chfwatch_count() != (int)w->wt_labels->len) {
        wt_rebuild(w);
        w->wt_gen = gen;
        w->wt_built = TRUE;
    }
    for (i = 0; i < (int)w->wt_labels->len; i++) {
        char line[240];
        chfwatch_format(w->dbg, i, line, sizeof line);
        gtk_label_set_text(g_ptr_array_index(w->wt_labels, i), line);
    }
}

static void on_wt_add(GtkWidget *wgt, gpointer ud)
{
    DbgWin *w = ud;
    const guint sel = gtk_drop_down_get_selected(w->wt_space);
    (void)wgt;
    if (chfwatch_add(sel < NSPACES ? k_spaces[sel] : CHF_SPACE_PROG,
                     gtk_editable_get_text(GTK_EDITABLE(w->wt_entry))) < 0) {
        toast(w, "Type an address, a label, an expression or a range");
        return;
    }
    gtk_editable_set_text(GTK_EDITABLE(w->wt_entry), "");
    refresh_now(w);
}

/* ---- trace and history -------------------------------------------------------- */

/* Both tabs read the one ring. Trace (0) follows it while the machine runs;
 * History (1) is read once the machine has stopped. The newest HIST_SHOWN
 * matches are shown, oldest first. */
static void refresh_hist(DbgWin *w, int idx, gboolean force)
{
    static chfdebug_trace buf[HIST_READ];
    static int hit[HIST_SHOWN];
    const gboolean on = chfdebug_trace_enabled(w->dbg) != 0;
    const gboolean stopped = chfdebug_is_stopped(w->dbg) != 0;
    const int count = chfdebug_trace_count(w->dbg);
    uint32_t lo = 0, hi = 0xFFFF;
    int filter = CHFDEBUG_TRACE_ALL, n, i, shown = 0, matched = 0;
    uint64_t sig;
    chfdebug_trace newest;
    char note[200];
    GString *t;

    w->h_sync = TRUE;       /* the Record toggles follow the engine */
    gtk_toggle_button_set_active(w->h_rec[0], on);
    gtk_toggle_button_set_active(w->h_rec[1], on);
    w->h_sync = FALSE;

    if (idx == 1 && !stopped) {
        gtk_label_set_text(w->h_note[1], "Stop the machine to read its history.");
        w->h_sig[1] = ~(uint64_t)0;
        return;
    }
    memset(&newest, 0, sizeof newest);
    chfdebug_trace_read(w->dbg, &newest, 1);
    sig = ((uint64_t)count << 40) ^ newest.cycles ^ (on ? 1 : 0);
    if (!force && sig == w->h_sig[idx]) return;

    switch (gtk_drop_down_get_selected(w->h_filter[idx])) {
    case 1: filter = CHFDEBUG_TRACE_BRANCHES; break;
    case 2:
        if (!chfsym_parse_range(w->dbg, CHF_SPACE_PROG,
                                gtk_editable_get_text(GTK_EDITABLE(w->h_range[idx])), &lo, &hi)) {
            gtk_label_set_text(w->h_note[idx], "Type a range: 0802-08FF, a label, label..label");
            return;
        }
        filter = CHFDEBUG_TRACE_RANGE;
        break;
    default: break;
    }
    w->h_sig[idx] = sig;
    n = chfdebug_trace_read(w->dbg, buf, filter == CHFDEBUG_TRACE_ALL ? HIST_SHOWN : HIST_READ);
    for (i = 0; i < n; i++)             /* newest first: take the newest matches */
        if (chfdebug_trace_match(&buf[i], filter, (uint16_t)lo, (uint16_t)hi)) {
            matched++;
            if (shown < HIST_SHOWN) hit[shown++] = i;
        }
    t = g_string_new(NULL);
    if (shown == 0)
        g_string_append(t, on ? "(nothing recorded yet: let the machine run)"
                              : "Recording is off. Press Record, then let the machine run.");
    for (i = shown - 1; i >= 0; i--) {  /* oldest first */
        char line[256];
        chfdebug_trace_format(&buf[hit[i]], line, sizeof line);
        g_string_append(t, line);
        g_string_append_c(t, '\n');
    }
    dbg_set_text_follow(w->h_text[idx], t->str, TRUE);
    g_string_free(t, TRUE);
    g_snprintf(note, sizeof note, "%s, %d instruction%s%s", on ? "recording" : "recording off",
               count, count == 1 ? "" : "s",
               matched > shown ? ", newest 2000 shown" : "");
    gtk_label_set_text(w->h_note[idx], note);
}

static void refresh_status(DbgWin *w)
{
    char text[400];
    const gboolean stopped = chfdebug_is_stopped(w->dbg) != 0;
    stop_line(w, text, sizeof text);
    adw_window_title_set_subtitle(w->title, text);
    gtk_button_set_label(w->run_btn, stopped ? "Run (F5)" : "Stop (F5)");
    if (stopped) gtk_widget_add_css_class(GTK_WIDGET(w->run_btn), "chf-accent");
    else gtk_widget_remove_css_class(GTK_WIDGET(w->run_btn), "chf-accent");
}

/* Only the tabs in front of the user are brought up to date (the one showing,
 * and any popped out); the others catch up when their tab is switched to. */
static void refresh_tab(DbgWin *w, int tab)
{
    const gboolean live = !chfdebug_is_stopped(w->dbg);
    if (w->closing) return;
    switch (tab) {
    case TAB_DISASM: refresh_disasm(w); break;
    case TAB_MEMORY: refresh_mem(w); break;
    case TAB_BREAKS: refresh_bps(w); break;
    case TAB_WATCHES: refresh_watches(w); break;
    case TAB_CART: dbg_cart_refresh(w->cart, live); dbg_io_refresh(w->io, live); break;
    case TAB_SOUND: dbg_sound_refresh(w->io, live); break;
    case TAB_SYMBOLS: refresh_symbols(w); break;
    case TAB_TRACE: refresh_hist(w, 0, FALSE); break;
    case TAB_HISTORY: refresh_hist(w, 1, FALSE); break;
    case TAB_VIDEO: dbg_video_refresh(w->pages[TAB_VIDEO], FALSE); break;
    case TAB_FUJINET: refresh_fujinet(w, FALSE); break;
    default: break;
    }
}

/* The status, the registers and the tabs in front, and the generation they
 * now reflect. */
static void refresh_now(DbgWin *w)
{
    int t;
    if (w->closing) return;
    w->seen_generation = chfdebug_generation(w->dbg);
    w->was_stopped = chfdebug_is_stopped(w->dbg) != 0;
    refresh_status(w);
    refresh_cpu(w);
    for (t = 0; t < NTABS; t++)
        if (tab_visible(w, t)) refresh_tab(w, t);
}

static void attach_and_refresh(DbgWin *w);

static GtkAdjustment *vadj_of(GtkTextView *view)
{
    GtkWidget *parent = gtk_widget_get_parent(GTK_WIDGET(view));
    return GTK_IS_SCROLLED_WINDOW(parent)
        ? gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(parent)) : NULL;
}

/* A new stop brings every view to the current spot: the disassembly to the PC
 * (done by the caller), the dump back to its top row, and the trace and
 * history to their newest line (they redraw and follow their end). */
static void recentre_views(DbgWin *w)
{
    GtkAdjustment *adj = vadj_of(w->mem_view);
    int index;
    if (adj) gtk_adjustment_set_value(adj, 0);
    for (index = 0; index < 2; index++) {
        w->h_sig[index] = ~(uint64_t)0;
        g_object_set_data(G_OBJECT(w->h_text[index]), "follow", GINT_TO_POINTER(1));
    }
    refresh_now(w);
}

static gboolean tick_refresh(gpointer ud)
{
    DbgWin *w = ud;
    unsigned gen;
    gboolean stopped;
    if (!gtk_widget_get_visible(GTK_WIDGET(w->win))) return G_SOURCE_CONTINUE;
    /* Opened before the session started (CHF_OPEN_DEBUGGER), or showing
     * across a session restart: the engine comes up with the machine. */
    if (!chfdebug_is_attached(w->dbg) && chfsession_is_running(w->session)) {
        attach_and_refresh(w);
        return G_SOURCE_CONTINUE;
    }
    gen = chfdebug_generation(w->dbg);
    stopped = chfdebug_is_stopped(w->dbg) != 0;
    {   /* a new stop brings the disassembly back to the PC, wherever it was scrolled */
        const uint64_t serial = chfdebug_stop_serial(w->dbg);
        if (serial != w->seen_serial) {
            w->seen_serial = serial;
            if (stopped && !gtk_check_button_get_active(w->follow_pc))
                gtk_check_button_set_active(w->follow_pc, TRUE);
            w->running_ticks = 0;
            recentre_views(w);
            return G_SOURCE_CONTINUE;
        }
    }
    if (!stopped && tab_visible(w, TAB_TRACE) && ++w->trace_ticks >= 4) {
        w->trace_ticks = 0;     /* the trace follows the ring while the machine runs */
        refresh_hist(w, 0, FALSE);
    }
    if (gen != w->seen_generation || stopped != w->was_stopped) {
        w->running_ticks = 0;
        refresh_now(w);
    } else if (!stopped && tab_visible(w, TAB_VIDEO)) {
        /* the Video tab's picture ten times a second */
        dbg_video_refresh(w->pages[TAB_VIDEO], FALSE);
        if (++w->running_ticks >= 5) {
            w->running_ticks = 0;
            refresh_now(w);
        }
    } else if (!stopped && ++w->running_ticks >= 5) {
        /* live values twice a second while the machine runs */
        w->running_ticks = 0;
        refresh_now(w);
    }
    return G_SOURCE_CONTINUE;
}

/* The refresh timer runs only while the window is mapped. */
static void on_win_map(GtkWidget *widget, gpointer ud)
{
    DbgWin *w = ud;
    (void)widget;
    if (!w->timer && !w->closing)
        w->timer = g_timeout_add(100, tick_refresh, w);
}

static void on_win_unmap(GtkWidget *widget, gpointer ud)
{
    DbgWin *w = ud;
    (void)widget;
    if (w->timer) g_source_remove(w->timer);
    w->timer = 0;
}

/* ---- handlers ------------------------------------------------------------- */

static void on_run(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    if (chfdebug_is_stopped(w->dbg)) {
        chfdebug_resume(w->dbg);
    } else {
        chfdebug_stop(w->dbg);
        settle(w, TRUE);
    }
    refresh_now(w);
}

/* A step: the machine is stopped, runs a moment, stops again. */
#define STEP_HANDLER(name, call) \
    static void name(GtkButton *b, gpointer ud) \
    { \
        DbgWin *w = ud; \
        (void)b; \
        call; \
        settle(w, TRUE); \
        refresh_now(w); \
    }
STEP_HANDLER(on_step, chfdebug_step(w->dbg))
STEP_HANDLER(on_step_over, chfdebug_step_over(w->dbg))
STEP_HANDLER(on_step_out, chfdebug_step_out(w->dbg))
STEP_HANDLER(on_scanline, chfdebug_scanline(w->dbg, 1))
STEP_HANDLER(on_frame, chfdebug_frame(w->dbg, 1))
#undef STEP_HANDLER

/* Run to the line selected in the Disassembly. */
static void on_run_to(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    if (w->dis_sel < 0) {
        toast(w, "Select a line in the Disassembly first");
        return;
    }
    chfdebug_run_to(w->dbg, (uint16_t)w->dis_sel);
    settle(w, TRUE);
    refresh_now(w);
}

/* F9: an execute breakpoint on the selected line. */
static void toggle_selected(DbgWin *w)
{
    if (w->dis_sel < 0) {
        toast(w, "Select a disassembly line first");
        return;
    }
    chfdebug_breakpoint_toggle(w->dbg, (uint16_t)w->dis_sel);
    refresh_now(w);
}

static void on_console(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    static char out[65536];
    const char *cmd = gtk_editable_get_text(GTK_EDITABLE(entry));
    char line[512];
    gboolean was_stopped = chfdebug_is_stopped(w->dbg) != 0;
    if (!cmd || !*cmd) return;
    /* the history keeps each command once in a row */
    if (w->history->len == 0 ||
        strcmp(g_ptr_array_index(w->history, w->history->len - 1), cmd) != 0)
        g_ptr_array_add(w->history, g_strdup(cmd));
    w->history_pos = w->history->len;
    g_snprintf(line, sizeof line, "> %s\n", cmd);
    append_text(w->console_out, line, TRUE);
    out[0] = '\0';
    chfdebug_command(w->dbg, cmd, out, sizeof out);
    append_text(w->console_out, out, TRUE);
    if (out[0] && out[strlen(out) - 1] != '\n')
        append_text(w->console_out, "\n", TRUE);
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
    if (was_stopped) settle(w, TRUE);
    refresh_now(w);
}

static void history_show(DbgWin *w)
{
    const char *text = w->history_pos < w->history->len
                           ? g_ptr_array_index(w->history, w->history_pos) : "";
    gtk_editable_set_text(GTK_EDITABLE(w->console_in), text);
    gtk_editable_set_position(GTK_EDITABLE(w->console_in), -1);
}

/* Tab completes the current word against the commands, the registers and
 * the labels; several matches are listed instead. Up and Down walk the
 * command history. */
static gboolean on_console_key(GtkEventControllerKey *c, guint keyval, guint code,
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
    text = gtk_editable_get_text(GTK_EDITABLE(w->console_in));
    word = strrchr(text, ' ');
    word = word ? word + 1 : text;
    comps[0] = '\0';
    n = chfdebug_completions(w->dbg, word, comps, sizeof comps);
    if (n == 1) {
        char merged[600];
        char *nl = strchr(comps, '\n');
        if (nl) *nl = '\0';
        g_snprintf(merged, sizeof merged, "%.*s%s ", (int)(word - text), text, comps);
        gtk_editable_set_text(GTK_EDITABLE(w->console_in), merged);
        gtk_editable_set_position(GTK_EDITABLE(w->console_in), -1);
    } else if (n > 1) {
        append_text(w->console_out, comps, FALSE);
        if (comps[0] && comps[strlen(comps) - 1] != '\n')
            append_text(w->console_out, "\n", FALSE);
    }
    return TRUE;
}

static void bp_enable_toggled(GtkCheckButton *b, gpointer ud)
{
    DbgWin *w = ud;
    chfdebug_breakpoint_enable(w->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "bp")),
                               gtk_check_button_get_active(b));
    refresh_now(w);
}

static void bp_remove_clicked(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    chfdebug_breakpoint_remove(w->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "bp")));
    refresh_now(w);
}

/* One end of a breakpoint's range, in the kind's own units. */
static int bp_parse(DbgWin *w, int kind, const char *text, long *out)
{
    long row, col;
    char *comma;
    char err[128];

    switch (k_bp_kinds[kind].space) {
    case CHF_SPACE_PROG:
        return parse_addr(w, text, out, err, sizeof err);
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

/* A refused Add: said under the form, and as a toast so it is not missed. */
static void bp_error(DbgWin *w, const char *msg)
{
    gtk_label_set_text(w->bp_msg, msg);
    if (msg[0]) toast(w, msg);
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
        bp_error(w, "Not a valid start for this kind of breakpoint");
        return;
    }
    b = a;
    if (end && *end && !bp_parse(w, (int)sel, end, &b)) {
        bp_error(w, "The end is not valid");
        return;
    }
    if (b < a) {
        bp_error(w, "The end comes before the start");
        return;
    }
    if (a < 0 || b > 0xFFFF) {
        bp_error(w, "Out of range");
        return;
    }
    id = chfdebug_breakpoint_add(w->dbg, k_bp_kinds[sel].type, k_bp_kinds[sel].space,
                                 (uint16_t)a, (uint16_t)b,
                                 gtk_editable_get_text(GTK_EDITABLE(w->bp_cond)));
    if (id < 0) {
        bp_error(w, "The condition does not parse, or the range is outside its space");
        return;
    }
    bp_error(w, "");
    gtk_editable_set_text(GTK_EDITABLE(w->bp_start), "");
    gtk_editable_set_text(GTK_EDITABLE(w->bp_end), "");
    gtk_editable_set_text(GTK_EDITABLE(w->bp_cond), "");
    refresh_now(w);
}

/* "Break at": a symbol or a hex address, then Enter, sets an execute
 * breakpoint there (once). */
static void on_break_at(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    char err[128] = "";
    long address;
    if (!parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(entry)), &address, err, sizeof err)) {
        toast(w, err);
        return;
    }
    if (!chfdebug_breakpoint_check(w->dbg, (uint16_t)address))
        chfdebug_breakpoint_add(w->dbg, CHFDEBUG_BP_EXEC, CHF_SPACE_PROG, (uint16_t)address,
                                (uint16_t)address, "");
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
    refresh_now(w);
}

static void on_bp_clear(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    chfdebug_breakpoint_clear(w->dbg);
    refresh_now(w);
}

static void on_bp_clear_hits(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    chfdebug_breakpoint_clear_hits(w->dbg);
    refresh_now(w);
}

static void popdown_of(GtkWidget *b)
{
    GtkWidget *pop = gtk_widget_get_ancestor(b, GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
}

static void load_symbols(DbgWin *w, const char *path)
{
    char msg[512] = "";
    chfdebug_load_symbols(w->dbg, path, msg, sizeof msg);
    notice(w, msg);
    refresh_now(w);
}

static void on_clear_symbols(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    chfdebug_clear_symbols(w->dbg);
    notice(w, "Symbols cleared (the built-ins stay)");
    refresh_now(w);
}

static void on_symbols_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    DbgWin *w = ud;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    if (!file || w->closing) return;
    path = g_file_get_path(file);
    if (path) load_symbols(w, path);
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
    popdown_of(GTK_WIDGET(b));
    load_symbols(ud, NULL);
}

static void on_save_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    DbgWin *w = ud;
    g_autoptr(GFile) file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    const char *kind = g_object_get_data(src, "kind");
    char msg[512] = "";
    if (!file || w->closing) return;
    path = g_file_get_path(file);
    if (!path) return;
    chfdebug_save(w->dbg, kind, path, msg, sizeof msg);
    notice(w, msg);
}

static void on_save(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    const char *kind = g_object_get_data(G_OBJECT(b), "kind");
    char title[80];
    popdown_of(GTK_WIDGET(b));
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

/* The notebook also switches pages as it is taken apart with the window
 * (main window closing), before the window's "destroy": only a mapped window
 * has a tab to refresh, a hidden one refreshes when it is shown. */
static void on_switch_page(GtkNotebook *nb, GtkWidget *page, guint num, gpointer ud)
{
    DbgWin *w = ud;
    (void)nb; (void)num;
    if (w->closing) return;
    chf_help_set_context(w->win, k_tab_contexts[page_tab(page)]);
    if (gtk_widget_get_mapped(GTK_WIDGET(w->win)))
        refresh_tab(w, page_tab(page));
}

/* Record on either page is the one recorder. */
static void on_hist_rec(GtkToggleButton *b, gpointer ud)
{
    DbgWin *w = ud;
    if (w->h_sync) return;
    chfdebug_trace_enable(w->dbg, gtk_toggle_button_get_active(b));
    w->h_sig[0] = w->h_sig[1] = ~(uint64_t)0;
    refresh_now(w);
}

static void on_hist_clear(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    chfdebug_trace_clear(w->dbg);
    w->h_sig[0] = w->h_sig[1] = ~(uint64_t)0;
    refresh_now(w);
}

static void on_hist_changed(GObject *o, GParamSpec *p, gpointer ud)
{
    DbgWin *w = ud;
    (void)o; (void)p;
    w->h_sig[0] = w->h_sig[1] = ~(uint64_t)0;
    refresh_now(w);
}

static void on_hist_activate(GtkEntry *e, gpointer ud)
{
    on_hist_changed(NULL, NULL, ud);
    (void)e;
}

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint code,
                       GdkModifierType st, gpointer ud);

/* Puts a popped-out tab back in the notebook, at its place in the order. */
static void dock_tab(DbgWin *w, int tab, gboolean destroy)
{
    GtkWindow *win = w->detached[tab];
    GtkWidget *page = w->pages[tab], *vbox;
    int pos = 0, t;

    if (!win) return;
    vbox = g_object_get_data(G_OBJECT(win), "vbox");
    g_object_ref(page);
    gtk_box_remove(GTK_BOX(vbox), page);
    w->detached[tab] = NULL;
    for (t = 0; t < tab; t++)
        if (!w->detached[t]) pos++;
    gtk_notebook_insert_page(w->notebook, page, gtk_label_new(k_tab_names[tab]), pos);
    g_object_unref(page);
    gtk_notebook_set_current_page(w->notebook, pos);
    if (destroy) gtk_window_destroy(win);
    if (!w->closing) refresh_now(w);
}

static void on_dock_button(GtkButton *b, gpointer ud)
{
    dock_tab(ud, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "tab")), TRUE);
}

static gboolean on_detached_close(GtkWindow *win, gpointer ud)
{
    dock_tab(ud, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(win), "tab")), FALSE);
    return FALSE;     /* let it close */
}

/* Any tab can move to a window of its own; one always stays. */
static void pop_out_tab(DbgWin *w, int tab)
{
    GtkWidget *page, *vbox, *header, *dock;
    GtkWindow *win;
    GtkEventController *keys;
    char title[96];

    if (tab < 0 || tab >= NTABS || w->detached[tab]) return;
    if (gtk_notebook_get_n_pages(w->notebook) <= 1) {
        toast(w, "One tab always stays in the debugger");
        return;
    }
    page = w->pages[tab];
    g_object_ref(page);
    gtk_notebook_remove_page(w->notebook, gtk_notebook_page_num(w->notebook, page));

    win = GTK_WINDOW(adw_window_new());
    g_snprintf(title, sizeof title, "Channel F Debugger - %s", k_tab_names[tab]);
    gtk_window_set_title(win, title);
    gtk_window_set_icon_name(win, chf_icon_name());
    gtk_window_set_default_size(win, 800, 560);
    gtk_window_set_transient_for(win, w->win);
    header = adw_header_bar_new();
    dock = gtk_button_new_with_label("Dock");
    gtk_widget_set_tooltip_text(dock, "Put this tab back in the debugger");
    g_object_set_data(G_OBJECT(dock), "tab", GINT_TO_POINTER(tab));
    g_signal_connect(dock, "clicked", G_CALLBACK(on_dock_button), w);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), dock);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), chf_help_button(NULL));
    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(vbox), header);
    gtk_box_append(GTK_BOX(vbox), page);
    gtk_widget_set_vexpand(page, TRUE);
    adw_window_set_content(ADW_WINDOW(win), vbox);
    g_object_unref(page);
    g_object_set_data(G_OBJECT(win), "vbox", vbox);
    g_object_set_data(G_OBJECT(win), "tab", GINT_TO_POINTER(tab));
    chf_help_set_context(win, k_tab_contexts[tab]);
    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), w);
    gtk_widget_add_controller(GTK_WIDGET(win), keys);
    g_signal_connect(win, "close-request", G_CALLBACK(on_detached_close), w);
    w->detached[tab] = win;
    gtk_window_present(win);
    refresh_now(w);
}

static void on_pop_out(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    pop_out_tab(w, current_tab(w));
}

/* Closing hides: the machine runs on, and the window is back on F12. */
static void hide_window(DbgWin *w)
{
    int t;
    for (t = 0; t < NTABS; t++)
        dock_tab(w, t, TRUE);       /* popped-out tabs go home with the window */
    chfdebug_detach(w->dbg);
    gtk_widget_set_visible(GTK_WIDGET(w->win), FALSE);
}

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint code,
                       GdkModifierType st, gpointer ud)
{
    DbgWin *w = ud;
    GtkWidget *src = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(c));
    (void)code;
    switch (keyval) {
    case GDK_KEY_F1: chf_help_for(GTK_IS_WINDOW(src) ? GTK_WINDOW(src) : w->win); return TRUE;
    case GDK_KEY_F5: on_run(NULL, w); return TRUE;
    case GDK_KEY_F7:
        if (st & GDK_SHIFT_MASK) on_frame(NULL, w); else on_step(NULL, w);
        return TRUE;
    case GDK_KEY_F8:
        if (st & GDK_SHIFT_MASK) on_step_out(NULL, w); else on_step_over(NULL, w);
        return TRUE;
    case GDK_KEY_F9: toggle_selected(w); return TRUE;
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

/* ---- construction --------------------------------------------------------- */

static GtkWidget *build_toolbar(DbgWin *w)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *stepping = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *tools = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
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
    g_signal_connect(b, "clicked", G_CALLBACK(cb), w); gtk_box_append(GTK_BOX(stepping), b); } while (0)
    w->run_btn = GTK_BUTTON(gtk_button_new_with_label("Stop (F5)"));
    g_signal_connect(w->run_btn, "clicked", G_CALLBACK(on_run), w);
    gtk_box_append(GTK_BOX(stepping), GTK_WIDGET(w->run_btn));
    TB("Step (F7)", on_step);
    TB("Step Over (F8)", on_step_over);
    TB("Step Out (\xe2\x87\xa7""F8)", on_step_out);
    TB("Frame (\xe2\x87\xa7""F7)", on_frame);
    TB("Scanline", on_scanline);
    TB("Run to Cursor", on_run_to);
    TB("Clear Breakpoints", on_bp_clear);
#undef TB

    /* Break at: a symbol or a hex address, then Enter */
    w->break_at = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->break_at, "symbol or hex, Enter");
    gtk_editable_set_width_chars(GTK_EDITABLE(w->break_at), 20);
    gtk_widget_set_tooltip_text(GTK_WIDGET(w->break_at),
                                "Set an execute breakpoint at a symbol or a hex address");
    g_signal_connect(w->break_at, "activate", G_CALLBACK(on_break_at), w);
    gtk_box_append(GTK_BOX(tools), dbg_labeled("Break at", GTK_WIDGET(w->break_at)));

    /* Load Symbols: a file, or (the arrow) the files beside the cartridge */
    sym = adw_split_button_new();
    adw_split_button_set_label(ADW_SPLIT_BUTTON(sym), "Symbols\xe2\x80\xa6");
    adw_split_button_set_dropdown_tooltip(ADW_SPLIT_BUTTON(sym), "More ways to load symbols");
    g_signal_connect(sym, "clicked", G_CALLBACK(on_symbols), w);
    sympop = gtk_popover_new();
    beside = gtk_button_new_with_label("Beside the Cartridge (.map, .lst, .sym)");
    gtk_widget_add_css_class(beside, "flat");
    g_signal_connect(beside, "clicked", G_CALLBACK(on_symbols_beside), w);
    gtk_popover_set_child(GTK_POPOVER(sympop), beside);
    adw_split_button_set_popover(ADW_SPLIT_BUTTON(sym), GTK_POPOVER(sympop));
    gtk_box_append(GTK_BOX(tools), sym);

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
    gtk_box_append(GTK_BOX(tools), save);

    b = gtk_button_new_with_label("Pop Out");
    gtk_widget_set_tooltip_text(b, "Move the tab showing into a window of its own");
    gtk_widget_set_halign(b, GTK_ALIGN_END);
    gtk_widget_set_hexpand(b, TRUE);
    g_signal_connect(b, "clicked", G_CALLBACK(on_pop_out), w);
    gtk_box_append(GTK_BOX(tools), b);
    gtk_box_append(GTK_BOX(bar), stepping);
    gtk_box_append(GTK_BOX(bar), tools);
    return bar;
}

static GtkWidget *build_console(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *scroll = dbg_mono_view(&w->console_out, FALSE);
    GtkEventController *keys;

    gtk_text_view_set_wrap_mode(w->console_out, GTK_WRAP_WORD_CHAR);
    dbg_log_make(w->console_out);
    w->console_in = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->console_in,
        "Command (help, step, break, bpa, bpv, watch, find, print, mem, poke, scratch, kstack, vram, "
        "video, cart, ...) \xe2\x80\x94 Tab completes, Up/Down recall");
    gtk_widget_set_hexpand(GTK_WIDGET(w->console_in), TRUE);
    g_signal_connect(w->console_in, "activate", G_CALLBACK(on_console), w);
    /* ahead of the entry's own text handling, so Tab and Up/Down are ours */
    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_console_key), w);
    gtk_widget_add_controller(GTK_WIDGET(w->console_in), keys);
    w->history = g_ptr_array_new_with_free_func(g_free);

    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(w->console_in));
    dbg_set_text_follow(w->console_out,
        "Channel F debugger console (the Fairchild F8 / F3850, the 128 x 64 video RAM and the "
        "FujiNet cartridge). Type 'help' for every command. Numbers: $0802, 0x802, 802h, o17 "
        "(octal).\n", TRUE);
    return dbg_padded(box);
}

/* Under the notebook, on every tab: A, W and its flags, ISAR in octal, PC, PC0,
 * PC1, DC0, DC1 and r59 (Enter sets one while the machine is stopped), the
 * K-stack depth, and where the beam is. */
static GtkWidget *reg_entry(DbgWin *w, int i)
{
    w->reg[i] = GTK_ENTRY(dbg_hex_entry(k_regs[i].digits));
    gtk_widget_set_tooltip_text(GTK_WIDGET(w->reg[i]), k_regs[i].tip);
    g_object_set_data(G_OBJECT(w->reg[i]), "reg", GINT_TO_POINTER(i));
    g_signal_connect(w->reg[i], "activate", G_CALLBACK(on_reg_activate), w);
    return GTK_WIDGET(w->reg[i]);
}

static GtkWidget *build_regs(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *row2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *row3 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    int i;

    for (i = R_PC; i <= R_ISAR; i++) {
        gtk_box_append(GTK_BOX(row1), dbg_labeled(k_regs[i].name, reg_entry(w, i)));
        if (i == R_W) {
            int f;
            for (f = 0; f < NFLAGS; f++) {
                w->flag[f] = gtk_check_button_new_with_label(k_flags[f].name);
                gtk_widget_set_tooltip_text(w->flag[f], k_flags[f].tip);
                g_object_set_data(G_OBJECT(w->flag[f]), "flag", GINT_TO_POINTER(f));
                g_signal_connect(w->flag[f], "toggled", G_CALLBACK(on_flag_toggled), w);
                gtk_box_append(GTK_BOX(row1), w->flag[f]);
            }
        }
    }
    for (i = R_PC0; i <= R_K; i++)
        gtk_box_append(GTK_BOX(row2), dbg_labeled(k_regs[i].name, reg_entry(w, i)));
    w->k_depth = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->k_depth), "dim-label");
    gtk_box_append(GTK_BOX(row2), GTK_WIDGET(w->k_depth));
    w->pill_illegal = gtk_label_new("ILLEGAL");
    gtk_widget_add_css_class(w->pill_illegal, "chf-pill");
    gtk_widget_set_tooltip_text(w->pill_illegal, "An illegal opcode has been executed");
    dbg_pill(w->pill_illegal, TRUE);
    gtk_box_append(GTK_BOX(row2), w->pill_illegal);

    w->info = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->info), "dim-label");
    gtk_label_set_xalign(w->info, 0);
    gtk_label_set_ellipsize(w->info, PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(GTK_WIDGET(w->info), TRUE);
    gtk_box_append(GTK_BOX(row3), GTK_WIDGET(w->info));

    gtk_box_append(GTK_BOX(box), row1);
    gtk_box_append(GTK_BOX(box), row2);
    gtk_box_append(GTK_BOX(box), row3);
    return dbg_padded(box);
}

/* Find Next / Previous over the dump, with a note for what happened. */
static GtkWidget *build_search_bar(DbgWin *w)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *next = gtk_button_new_with_label("Find Next");
    GtkWidget *prev = gtk_button_new_with_label("Find Previous");
    static const char *const kinds[] = { "Hex", "Text", NULL };

    g_object_set_data(G_OBJECT(prev), "back", GINT_TO_POINTER(1));
    g_signal_connect(next, "clicked", G_CALLBACK(on_search), w);
    g_signal_connect(prev, "clicked", G_CALLBACK(on_search), w);
    w->srch_kind = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(kinds));
    gtk_widget_set_tooltip_text(GTK_WIDGET(w->srch_kind),
                                "Hex bytes (F8 00) or text (one byte per character, any case)");
    w->srch_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->srch_entry, "search this space: F8 00, or text");
    gtk_widget_set_hexpand(GTK_WIDGET(w->srch_entry), TRUE);
    g_object_set_data(G_OBJECT(w->srch_entry), "next-button", next);
    g_signal_connect(w->srch_entry, "activate", G_CALLBACK(on_search_activate), w);
    w->srch_note = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->srch_note), "dim-label");
    gtk_label_set_ellipsize(w->srch_note, PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(w->srch_note, 18);
    gtk_label_set_max_width_chars(w->srch_note, 18);
    gtk_box_append(GTK_BOX(bar), gtk_label_new("Search"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->srch_kind));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->srch_entry));
    gtk_box_append(GTK_BOX(bar), next);
    gtk_box_append(GTK_BOX(bar), prev);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->srch_note));
    return bar;
}

static void on_scratch_changed(gpointer ud)
{
    refresh_now(ud);
}

static GtkWidget *build_memory(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *mem = dbg_mono_view(&w->mem_view, FALSE);
    GtkWidget *edit = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *up, *down, *scratch_root;

    w->mem_space = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(k_space_labels));
    g_signal_connect(w->mem_space, "notify::selected", G_CALLBACK(on_mem_space), w);
    w->mem_goto = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_width_chars(GTK_EDITABLE(w->mem_goto), 16);
    gtk_entry_set_placeholder_text(w->mem_goto, "$0802 / label / lo-hi");
    gtk_widget_set_tooltip_text(GTK_WIDGET(w->mem_goto),
        "An address, a label, an expression or a range in the space showing "
        "(r12 or j/h/k/q in the scratchpad, row,col in the VRAM, a port or its name in I/O)");
    g_signal_connect(w->mem_goto, "activate", G_CALLBACK(on_mem_goto), w);
    w->mem_write = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_width_chars(GTK_EDITABLE(w->mem_write), 14);
    gtk_entry_set_placeholder_text(w->mem_write, "8010 41 42");
    gtk_widget_set_tooltip_text(GTK_WIDGET(w->mem_write),
        "Address, then hex bytes, into the space showing: the cartridge's RAM, a VRAM cell (0-3), "
        "a register, a port's latch. ROM and the BIOS ignore it.");
    g_signal_connect(w->mem_write, "activate", G_CALLBACK(on_mem_write), w);
    up = gtk_button_new_with_label("Page Up");
    down = gtk_button_new_with_label("Page Down");
    g_object_set_data(G_OBJECT(up), "delta", GINT_TO_POINTER(-1));
    g_object_set_data(G_OBJECT(down), "delta", GINT_TO_POINTER(1));
    g_signal_connect(up, "clicked", G_CALLBACK(on_mem_page), w);
    g_signal_connect(down, "clicked", G_CALLBACK(on_mem_page), w);
    gtk_box_append(GTK_BOX(edit), GTK_WIDGET(w->mem_space));
    gtk_box_append(GTK_BOX(edit), dbg_labeled("Go to", GTK_WIDGET(w->mem_goto)));
    gtk_box_append(GTK_BOX(edit), up);
    gtk_box_append(GTK_BOX(edit), down);
    gtk_box_append(GTK_BOX(edit), dbg_labeled("Poke (address bytes)", GTK_WIDGET(w->mem_write)));

    /* the scratchpad's grid and the BIOS K stack, over the dump of that space */
    w->scratch = dbg_scratch_new(w->session, w->dbg, on_scratch_changed, w, &scratch_root);
    w->scratch_box = scratch_root;
    gtk_widget_set_visible(w->scratch_box, FALSE);

    gtk_box_append(GTK_BOX(box), edit);
    gtk_box_append(GTK_BOX(box), build_search_bar(w));
    gtk_box_append(GTK_BOX(box), w->scratch_box);
    gtk_box_append(GTK_BOX(box), mem);
    /* the highlight covers a few bytes, not a paragraph */
    gtk_text_buffer_create_tag(gtk_text_view_get_buffer(w->mem_view), "mark",
                               "background", "#F2C300", "foreground", "#1e1a14", NULL);
    w->mem_hl = FALSE;
    {
        GtkWidget *scroll = gtk_scrolled_window_new();
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), dbg_padded(box));
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC,
                                       GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), FALSE);
        gtk_widget_set_size_request(mem, -1, 150);
        return scroll;
    }
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
    gtk_entry_set_placeholder_text(w->jump, "$0802 / label / expression");
    gtk_editable_set_width_chars(GTK_EDITABLE(w->jump), 22);
    g_signal_connect(w->jump, "activate", G_CALLBACK(on_jump), w);
    hint = gtk_label_new("Click the margin or double-click a line for a breakpoint (F9 on the "
                         "selected line); scroll to browse");
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
    GtkWidget *add, *clear, *hits, *head, *hint;
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
    gtk_entry_set_placeholder_text(w->bp_cond,
        "condition (a == $FF && value != old) \xe2\x80\x94 optional");
    gtk_widget_set_hexpand(GTK_WIDGET(w->bp_cond), TRUE);
    g_signal_connect(w->bp_type, "notify::selected", G_CALLBACK(on_bp_type_changed), w);
    g_signal_connect(w->bp_start, "activate", G_CALLBACK(on_bp_add), w);
    g_signal_connect(w->bp_end, "activate", G_CALLBACK(on_bp_add), w);
    g_signal_connect(w->bp_cond, "activate", G_CALLBACK(on_bp_add), w);
    add = gtk_button_new_with_label("Add");
    g_signal_connect(add, "clicked", G_CALLBACK(on_bp_add), w);
    clear = gtk_button_new_with_label("Clear All");
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
    gtk_label_set_wrap(w->bp_msg, TRUE);
    hint = dbg_hint("A VRAM breakpoint takes a cell as row,col (decimal) and stops after the "
                    "strobe that wrote it; a scratchpad one a register number and stops when "
                    "it changes; a port one a port number. A condition is an expression like "
                    "the console's (a, isar, pc0, p[$8000]); `value`, `addr`, `row`, `col`, "
                    "`port` and `old` describe the access.", 60);
    g_object_unref(kinds);

    w->bp_stop = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(w->bp_stop, 0);
    gtk_label_set_ellipsize(w->bp_stop, PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(GTK_WIDGET(w->bp_stop), TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(w->bp_stop), "heading");
    hits = gtk_button_new_with_label("Clear Hits");
    gtk_widget_set_tooltip_text(hits, "Zero every hit count");
    g_signal_connect(hits, "clicked", G_CALLBACK(on_bp_clear_hits), w);
    head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(head), GTK_WIDGET(w->bp_stop));
    gtk_box_append(GTK_BOX(head), hits);

    gtk_box_append(GTK_BOX(box), head);
    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(w->bp_msg));
    gtk_box_append(GTK_BOX(box), hint);
    return dbg_padded(box);
}

static GtkWidget *build_watches(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *form = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *add;

    w->wt_labels = g_ptr_array_new();
    w->wt_space = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(k_space_labels));
    gtk_widget_set_tooltip_text(GTK_WIDGET(w->wt_space), "The space the watch is in");
    w->wt_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->wt_entry, "address, label, label+3, p[ptr], 8000-80FF");
    gtk_widget_set_hexpand(GTK_WIDGET(w->wt_entry), TRUE);
    g_signal_connect(w->wt_entry, "activate", G_CALLBACK(on_wt_add), w);
    add = gtk_button_new_with_label("Add");
    g_signal_connect(add, "clicked", G_CALLBACK(on_wt_add), w);
    gtk_box_append(GTK_BOX(form), gtk_label_new("Watch"));
    gtk_box_append(GTK_BOX(form), GTK_WIDGET(w->wt_space));
    gtk_box_append(GTK_BOX(form), GTK_WIDGET(w->wt_entry));
    gtk_box_append(GTK_BOX(form), add);

    w->wt_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(w->wt_list, GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(w->wt_list));
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(box), form);
    gtk_box_append(GTK_BOX(box), scroll);
    return dbg_padded(box);
}

/* The cartridge's state and the memory map on the left; the console's
 * controllers, ports and the 2102, then the cartridge's mailbox page and
 * events on the right, in a scroller. */
static GtkWidget *build_cart(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *state_root, *mailbox_root, *io_root, *sound_root;

    w->cart = dbg_cart_new(w->session, w->dbg, &state_root, &mailbox_root);
    w->io = dbg_io_new(w->session, w->dbg, &io_root, &sound_root);
    w->pages[TAB_SOUND] = sound_root;
    gtk_box_append(GTK_BOX(right), io_root);
    gtk_box_append(GTK_BOX(right), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(right), mailbox_root);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), right);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(box), state_root);
    gtk_box_append(GTK_BOX(box), scroll);
    return dbg_padded(box);
}

/* A filter over every known symbol, built-in and loaded; a row opens the
 * disassembly at its address. */
static GtkWidget *build_symbols(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *load, *clear;

    w->sym_filter = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->sym_filter, "filter by name");
    gtk_widget_set_hexpand(GTK_WIDGET(w->sym_filter), TRUE);
    g_signal_connect(w->sym_filter, "changed", G_CALLBACK(on_sym_filter_changed), w);
    w->sym_note = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->sym_note), "dim-label");
    load = gtk_button_new_with_label("Load\xe2\x80\xa6");
    gtk_widget_set_tooltip_text(load, "Load a symbol file (AS .map or .lst, or AAAA name)");
    g_signal_connect(load, "clicked", G_CALLBACK(on_symbols), w);
    clear = gtk_button_new_with_label("Clear");
    gtk_widget_set_tooltip_text(clear, "Forget every loaded symbol and label; the built-ins stay");
    g_signal_connect(clear, "clicked", G_CALLBACK(on_clear_symbols), w);
    gtk_box_append(GTK_BOX(bar), gtk_label_new("Filter"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->sym_filter));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->sym_note));
    gtk_box_append(GTK_BOX(bar), load);
    gtk_box_append(GTK_BOX(bar), clear);

    w->sym_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(w->sym_list, GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(w->sym_list, FALSE);
    g_signal_connect(w->sym_list, "row-activated", G_CALLBACK(on_sym_row_activated), w);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(w->sym_list));
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(box), bar);
    gtk_box_append(GTK_BOX(box), scroll);
    return dbg_padded(box);
}

/* Trace (idx 0) and History (idx 1): the same controls over the one ring. */
static GtkWidget *build_hist(DbgWin *w, int idx)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = dbg_mono_view(&w->h_text[idx], FALSE);
    GtkWidget *clear = gtk_button_new_with_label("Clear");
    static const char *const filters[] = { "All instructions", "Branches only", "In range", NULL };

    w->h_rec[idx] = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Record"));
    gtk_widget_add_css_class(GTK_WIDGET(w->h_rec[idx]), "chf-toggle");
    gtk_widget_set_tooltip_text(GTK_WIDGET(w->h_rec[idx]),
                                "Record every instruction the CPU runs (the machine runs slower)");
    g_signal_connect(w->h_rec[idx], "toggled", G_CALLBACK(on_hist_rec), w);
    w->h_filter[idx] = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(filters));
    g_signal_connect(w->h_filter[idx], "notify::selected", G_CALLBACK(on_hist_changed), w);
    w->h_range[idx] = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->h_range[idx], "range: 0802-08FF or a label");
    g_signal_connect(w->h_range[idx], "activate", G_CALLBACK(on_hist_activate), w);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_hist_clear), w);
    w->h_note[idx] = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->h_note[idx]), "dim-label");
    gtk_widget_set_hexpand(GTK_WIDGET(w->h_note[idx]), TRUE);
    gtk_label_set_xalign(w->h_note[idx], 1.0f);
    dbg_log_make(w->h_text[idx]);
    w->h_sig[idx] = ~(uint64_t)0;

    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->h_rec[idx]));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->h_filter[idx]));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->h_range[idx]));
    gtk_box_append(GTK_BOX(bar), clear);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->h_note[idx]));
    gtk_box_append(GTK_BOX(box), bar);
    gtk_box_append(GTK_BOX(box), scroll);
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

/* The state outlives the window: the widgets go before the window's
 * "destroy" and can still call back (the notebook's page switches), so it is
 * freed only once the window object itself is gone. */
static void on_win_finalized(gpointer data, GObject *where)
{
    DbgWin *w = data;
    (void)where;
    if (w->history) g_ptr_array_unref(w->history);
    if (w->bp_sig) g_string_free(w->bp_sig, TRUE);
    if (w->wt_labels) g_ptr_array_unref(w->wt_labels);
    if (w->sym_sig) g_string_free(w->sym_sig, TRUE);
    dbg_scratch_free(w->scratch);
    dbg_io_free(w->io);
    dbg_cart_free(w->cart);
    g_free(w);
}

static void on_destroy(GtkWidget *widget, gpointer ud)
{
    DbgWin *w = ud;
    (void)widget;
    w->closing = TRUE;
    if (w->timer) g_source_remove(w->timer);
    w->timer = 0;
    chfdebug_detach(w->dbg);
    if (g_win == w) g_win = NULL;
}

static void attach_and_refresh(DbgWin *w)
{
    chfdebug_attach(w->dbg);    /* stops the machine, within a frame */
    settle(w, TRUE);
    w->running_ticks = 0;
    if (w->bp_sig) { g_string_free(w->bp_sig, TRUE); w->bp_sig = NULL; }
    refresh_now(w);
}

/* A tab from an environment variable (CHF_DEBUGGER_TAB, CHF_POPOUT): a
 * number, or a tab's name or the start of one, any case; the old tabs' names
 * still work. *space is the Memory space the name implies, or -1. */
static int tab_from_env(const char *var, int *space)
{
    const char *tab = g_getenv(var);
    char *end;
    long n;
    unsigned i;
    if (space) *space = -1;
    if (!tab || !*tab) return -1;
    n = strtol(tab, &end, 10);
    if (end != tab && *end == '\0')
        return (n >= 0 && n < NTABS) ? (int)n : -1;
    for (i = 0; i < G_N_ELEMENTS(k_tab_aliases); i++)
        if (g_ascii_strcasecmp(tab, k_tab_aliases[i].name) == 0) {
            if (space) *space = k_tab_aliases[i].space;
            return k_tab_aliases[i].tab;
        }
    for (i = 0; i < NTABS; i++)
        if (g_ascii_strncasecmp(tab, k_tab_names[i], strlen(tab)) == 0)
            return (int)i;
    return -1;
}

void chf_debugger_show(GtkWindow *parent, chfsession *session)
{
    DbgWin *w;
    GtkWidget *toolbar, *header, *root, *notebook, *overlay;
    GtkEventController *keys;
    int tab, space, i;

    if (g_win) {
        gboolean shown = gtk_widget_get_visible(GTK_WIDGET(g_win->win));
        gtk_window_present(g_win->win);
        if (!shown) attach_and_refresh(g_win);
        return;
    }
    install_css();
    w = g_new0(DbgWin, 1);
    w->session = session;
    w->dbg = chfsession_debugger(session);
    g_win = w;

    w->win = GTK_WINDOW(adw_window_new());
    g_object_weak_ref(G_OBJECT(w->win), on_win_finalized, w);
    gtk_window_set_title(w->win, "Debugger");
    gtk_window_set_icon_name(w->win, chf_icon_name());
    gtk_window_set_default_size(w->win, 1300, 860);
    gtk_window_set_transient_for(w->win, parent);
    gtk_window_set_application(w->win, gtk_window_get_application(parent));
    gtk_window_set_destroy_with_parent(w->win, TRUE);
    g_signal_connect(w->win, "close-request", G_CALLBACK(on_close), w);
    g_signal_connect(w->win, "destroy", G_CALLBACK(on_destroy), w);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(root), build_toolbar(w));
    notebook = gtk_notebook_new();
    w->notebook = GTK_NOTEBOOK(notebook);
    gtk_widget_set_vexpand(notebook, TRUE);
    w->pages[TAB_CONSOLE] = build_console(w);
    w->pages[TAB_DISASM] = build_disasm(w);
    w->pages[TAB_MEMORY] = build_memory(w);
    w->pages[TAB_BREAKS] = build_breaks(w);
    w->pages[TAB_WATCHES] = build_watches(w);
    w->pages[TAB_CART] = build_cart(w);          /* also makes the Sound page */
    w->pages[TAB_TRACE] = build_hist(w, 0);
    w->pages[TAB_HISTORY] = build_hist(w, 1);
    w->pages[TAB_VIDEO] = dbg_video_new(w->dbg);
    w->pages[TAB_SYMBOLS] = build_symbols(w);
    w->pages[TAB_FUJINET] = build_fujinet(w);
    for (i = 0; i < NTABS; i++) {
        g_object_set_data(G_OBJECT(w->pages[i]), "tab", GINT_TO_POINTER(i));
        gtk_notebook_append_page(w->notebook, w->pages[i], gtk_label_new(k_tab_names[i]));
    }
    gtk_box_append(GTK_BOX(root), notebook);
    gtk_box_append(GTK_BOX(root), build_regs(w));
    tab = tab_from_env("CHF_DEBUGGER_TAB", &space);
    if (tab >= 0) gtk_notebook_set_current_page(w->notebook, tab);
    if (space >= 0) {
        gtk_drop_down_set_selected(w->mem_space, (guint)space_index(space));
        gtk_widget_set_visible(w->scratch_box, space == CHF_SPACE_SCRATCH);
    }
    g_signal_connect(notebook, "switch-page", G_CALLBACK(on_switch_page), w);
    chf_help_set_context(w->win, k_tab_contexts[current_tab(w)]);

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

    /* captures: a watch, a break-on-change, a Go to, recording, a popped-out tab */
    {
        const char *wt = g_getenv("CHF_WATCH"), *wb = g_getenv("CHF_WATCH_BREAK"),
                   *mg = g_getenv("CHF_MEM_GOTO");
        uint32_t lo, hi;
        int sp;
        if (wt && *wt) {        /* a comma-separated list, each "space:text" or just text */
            char **entries = g_strsplit(wt, ",", -1);
            int entry;
            for (entry = 0; entries[entry]; entry++)
                if (*g_strstrip(entries[entry])) {
                    const char *text = split_space(entries[entry], CHF_SPACE_PROG, &sp);
                    chfwatch_add(sp, text);
                }
            g_strfreev(entries);
        }
        if (wb && *wb) {
            const char *text = split_space(wb, CHF_SPACE_PROG, &sp);
            if (chfsym_parse_range(w->dbg, sp, text, &lo, &hi)) break_on_change(w, sp, lo, hi);
        }
        if (mg && *mg) {
            const char *text = split_space(mg, CHF_SPACE_PROG, &sp);
            if (chfsym_parse_range(w->dbg, sp, text, &lo, &hi)) show_in_memory_space(w, sp, lo, hi);
        }
        if (g_getenv("CHF_HISTORY")) chfdebug_trace_enable(w->dbg, 1);
    }
    attach_and_refresh(w);
    g_signal_connect(w->win, "map", G_CALLBACK(on_win_map), w);
    g_signal_connect(w->win, "unmap", G_CALLBACK(on_win_unmap), w);
    gtk_window_present(w->win);
    if (current_tab(w) == TAB_CONSOLE)
        gtk_widget_grab_focus(GTK_WIDGET(w->console_in));
    tab = tab_from_env("CHF_POPOUT", NULL);
    if (tab >= 0) pop_out_tab(w, tab);
}

/* F12 and the menu item: show it, or hide it if it is showing. */
void chf_debugger_toggle(GtkWindow *parent, chfsession *session)
{
    if (g_win && gtk_widget_get_visible(GTK_WIDGET(g_win->win)))
        hide_window(g_win);
    else
        chf_debugger_show(parent, session);
}
