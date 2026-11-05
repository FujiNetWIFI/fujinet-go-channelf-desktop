/*
 * Debugger window (GTK4/libadwaita) over the session's Signetics 2650 / 2637
 * UVI debugger engine, via core/include/chfdebug.h.
 *
 * Tabs, in order: a Prompt (the contract's command set, with tab completion
 * and a command history), the 2650's registers (R0, both R1-R3 sets, PSU and
 * PSL decoded, the 15-bit PC and the IAR, the 8-deep return stack) with a
 * memory viewer over the three address spaces (PROG, UVI, CART), the
 * disassembly with a breakpoint gutter, the Video tab (the 2637 UVI: the
 * frame, the re-render from the registers and the beam-progress view, the
 * layers, objects boxed and indexed, the 16x13/26 character screen, the
 * glyph/UDC sheet, decoded editable registers, collisions, the palette:
 * dbg_video.c), Sound & I/O (the keypads, discs and panel live, the pots,
 * the beeper), breakpoints, the cartridge (its state and the mailbox page)
 * and FujiNet: every FujiBus packet on the cartridge's link, both ways, with
 * hex. A toolbar carries the stepping controls, symbol loading and file
 * saves; the family's keys apply (F5 run/stop, F7 step, F8 step over,
 * Shift+F8 step out, F12 close).
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

#include "../window.h"
#include "dbg_common.h"

#define DISASM_WINDOW 48
#define TRACE_MAX 512
#define MBOX_BASE 0x2B00
#define MBOX_SIZE 0x500
#define MEM_ROWS 16

/* The tabs, in order, by the names CHF_DEBUGGER_TAB takes. */
enum {
    TAB_PROMPT, TAB_CPU, TAB_DISASM, TAB_VIDEO, TAB_IO, TAB_BREAKS, TAB_CART,
    TAB_FUJINET, TAB_COUNT
};
static const struct { const char *name; int tab; } k_tab_names[] = {
    { "prompt", TAB_PROMPT }, { "cpu", TAB_CPU }, { "ram", TAB_CPU },
    { "memory", TAB_CPU }, { "disassembly", TAB_DISASM }, { "video", TAB_VIDEO },
    { "uvi", TAB_VIDEO }, { "sound", TAB_IO }, { "io", TAB_IO },
    { "breakpoints", TAB_BREAKS }, { "cart", TAB_CART }, { "fujinet", TAB_FUJINET },
};

/* The 2650's registers as entries: how many hex digits each shows. R1-R3 are
 * the set RS selects; R1'-R3' the other. */
static const struct { const char *name; int reg; int digits; } k_regs[] = {
    { "R0", CHF_REG_R0, 2 },
    { "R1", CHF_REG_R1, 2 }, { "R2", CHF_REG_R2, 2 }, { "R3", CHF_REG_R3, 2 },
    { "R1'", CHF_REG_R1B, 2 }, { "R2'", CHF_REG_R2B, 2 }, { "R3'", CHF_REG_R3B, 2 },
    { "PC", CHF_REG_PC, 4 }, { "IAR", CHF_REG_IAR, 4 },
    { "PSU", CHF_REG_PSU, 2 }, { "PSL", CHF_REG_PSL, 2 },
    { "SP", CHF_REG_SP, 1 }, { "CC", CHF_REG_CC, 1 },
};
#define NREGS ((int)G_N_ELEMENTS(k_regs))
/* PSU and PSL bits as check buttons */
static const struct { const char *name; int reg; } k_flags[] = {
    { "F", CHF_REG_F }, { "II", CHF_REG_II },
    { "C", CHF_REG_C }, { "COM", CHF_REG_COM }, { "OVF", CHF_REG_OVF },
    { "WC", CHF_REG_WC }, { "RS", CHF_REG_RS }, { "IDC", CHF_REG_IDC },
};
#define NFLAGS ((int)G_N_ELEMENTS(k_flags))
#define NPILLS 4

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

    /* cpu + ram */
    GtkEntry *reg[NREGS];
    GtkWidget *flag[NFLAGS];
    GtkWidget *pill[NPILLS];  /* S (sense), HALT, IRQ line, ILLEGAL */
    GtkEntry *ras[8];         /* the return address stack */
    GtkWidget *ras_label[8];
    GtkWidget *bank_label[2];
    GtkLabel *beam;
    GtkLabel *cpu_info;
    GtkDropDown *mem_space;   /* the memory viewer */
    GtkEntry *mem_addr, *poke_addr, *poke_val;
    GtkTextView *mem_text;
    uint32_t mem_base;
    gboolean syncing;

    /* disassembly */
    GtkTextView *disasm;
    GtkCheckButton *follow_pc;
    GtkEntry *jump;
    int disasm_top;           /* address of the first line (15-bit) */
    uint16_t line_addr[DISASM_WINDOW];
    int line_count;

    /* video */
    DbgVideo *video;

    /* sound & i/o */
    GtkTextView *io_text;
    GtkWidget *keys_area, *sticks_area;
    chfdebug_io io;            /* the last read, for the two drawings */

    /* breakpoints */
    GtkListBox *bp_list;
    GtkDropDown *bp_type;
    GtkEntry *bp_start, *bp_end, *bp_cond;
    GtkLabel *bp_msg;

    /* cart */
    GtkTextView *cart_text, *mbox_text;

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

/* A log (the FujiNet packets) follows its end while its reader is there.
 * New text grows the view only once it is laid out (or its tab first
 * shows), so the end is pinned whenever the view's height changes; whether
 * to follow is decided as text arrives. */
static gboolean following(GtkTextView *view)
{
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(view), "follow")) != 0;
}

static void on_log_resized(GtkAdjustment *adj, gpointer view)
{
    if (following(view))
        gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj) -
                                      gtk_adjustment_get_page_size(adj));
}

static void make_log(GtkTextView *view)
{
    g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
    g_signal_connect(vadj(view), "changed", G_CALLBACK(on_log_resized), view);
}

/* Before new text: follow if the reader is at the end (or never looked). */
static void log_note_reader(GtkTextView *view)
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
static void set_text_at(GtkTextView *view, const char *text, gboolean follow)
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
    set_text_at(view, text, FALSE);
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
    return l;
}

/* A line marked in the accent keeps dark text on either theme. */
static void accent_tag(GtkTextView *view, const char *name)
{
    char rgb[16];
    g_snprintf(rgb, sizeof rgb, "#%06x", CHFSESSION_ACCENT_RGB);
    gtk_text_buffer_create_tag(gtk_text_view_get_buffer(view), name,
                               "paragraph-background", rgb, "foreground", "#1e1a14", NULL);
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
    /* the bronze alone is too dark to see as a line on a dark theme */
    return adw_style_manager_get_dark(adw_style_manager_get_default())
        ? 0xD9C08E : CHFSESSION_ACCENT_DARK_RGB;
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
    if (done) return;
    done = TRUE;
    css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        "entry.chf-hex { padding: 1px 2px; min-height: 0; min-width: 0; }\n"
        "entry.chf-hex > text { min-width: 0; }\n");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

/* ---- the memory viewer ------------------------------------------------------ */

/* 16 rows of 16 bytes from mem_base, in the chosen space, with the ASCII. */
static void refresh_mem(DbgWin *w)
{
    static char text[MEM_ROWS * 96];
    uint8_t bytes[16];
    const int space = (int)gtk_drop_down_get_selected(w->mem_space);
    const uint32_t size = (uint32_t)chfdebug_space_size(w->dbg, space);
    int r, len = 0;

    for (r = 0; r < MEM_ROWS; r++) {
        const uint32_t a = w->mem_base + (uint32_t)r * 16;
        int i;
        len += g_snprintf(text + len, sizeof text - len, "%05X:", a);
        chfdebug_read(w->dbg, space, a, bytes, 16);
        for (i = 0; i < 16; i++)
            len += g_snprintf(text + len, sizeof text - len, a + (uint32_t)i < size
                              ? " %02X" : " --", bytes[i]);
        len += g_snprintf(text + len, sizeof text - len, "  ");
        for (i = 0; i < 16; i++)
            text[len++] = bytes[i] >= 0x20 && bytes[i] < 0x7F ? (char)bytes[i] : '.';
        text[len++] = '\n';
        text[len] = '\0';
    }
    dbg_set_text(w->mem_text, text);
}

/* ---- refresh -------------------------------------------------------------- */

static void refresh_cpu(DbgWin *w)
{
    chfdebug_cpu c;
    char buf[320];
    int i;
    chfdebug_cpu_get(w->dbg, &c);
    {
        const int v[NREGS] = { c.r0, c.r[0][1], c.r[0][2], c.r[0][3],
                               c.r[1][1], c.r[1][2], c.r[1][3], c.pc, c.iar,
                               c.psu, c.psl, c.sp, c.cc };
        for (i = 0; i < NREGS; i++) {
            g_snprintf(buf, sizeof buf, "%0*X", k_regs[i].digits, (unsigned)v[i]);
            dbg_set_entry(w->dbg, w->reg[i], buf);
        }
    }
    w->syncing = TRUE;
    {
        const int f[NFLAGS] = { c.flag, c.ii, c.c, c.com, c.ovf, c.wc, c.rs, c.idc };
        for (i = 0; i < NFLAGS; i++)
            gtk_check_button_set_active(GTK_CHECK_BUTTON(w->flag[i]), f[i] != 0);
    }
    w->syncing = FALSE;
    {
        const int p[NPILLS] = { c.sense, c.halted, c.irq_line, c.illegal_count != 0 };
        for (i = 0; i < NPILLS; i++) {
            if (p[i]) {
                gtk_widget_add_css_class(w->pill[i], "chf-accent");
                gtk_widget_remove_css_class(w->pill[i], "dim-label");
            } else {
                gtk_widget_remove_css_class(w->pill[i], "chf-accent");
                gtk_widget_add_css_class(w->pill[i], "dim-label");
            }
        }
    }
    /* the register set RS selects is marked */
    for (i = 0; i < 2; i++) {
        if (c.rs == i) {
            gtk_widget_add_css_class(w->bank_label[i], "chf-accent");
            gtk_widget_remove_css_class(w->bank_label[i], "dim-label");
        } else {
            gtk_widget_remove_css_class(w->bank_label[i], "chf-accent");
            gtk_widget_add_css_class(w->bank_label[i], "dim-label");
        }
    }
    /* the return stack: SP names the top (it was incremented by the call) */
    for (i = 0; i < 8; i++) {
        g_snprintf(buf, sizeof buf, "%04X", c.ras[i]);
        dbg_set_entry(w->dbg, w->ras[i], buf);
        if (c.sp == i && i > 0) {
            gtk_widget_add_css_class(w->ras_label[i], "chf-accent");
            gtk_widget_remove_css_class(w->ras_label[i], "dim-label");
        } else {
            gtk_widget_remove_css_class(w->ras_label[i], "chf-accent");
            gtk_widget_add_css_class(w->ras_label[i], "dim-label");
        }
    }
    g_snprintf(buf, sizeof buf, "page $%04X   %s%s   illegal opcodes %u%s",
               c.page, c.halted ? "halted, waiting for an interrupt   " : "",
               c.ii ? "interrupts inhibited" : "interrupts enabled", c.illegal_count,
               c.illegal_count ? " (last at $" : "");
    if (c.illegal_count) {
        char at[16];
        g_snprintf(at, sizeof at, "%04X)", c.illegal_pc & 0x7FFF);
        g_strlcat(buf, at, sizeof buf);
    }
    gtk_label_set_text(w->cpu_info, buf);
    g_snprintf(buf, sizeof buf,
               "%s   line %d   dot %d   frame %u   cycle %llu   instruction %llu",
               chf_machine_name(c.machine), c.vpos, c.hpos, c.frame,
               (unsigned long long)c.cycles, (unsigned long long)c.instructions);
    gtk_label_set_text(w->beam, buf);
    refresh_mem(w);
}

static void refresh_disasm(DbgWin *w)
{
    static chfdebug_line lines[DISASM_WINDOW];
    static char text[DISASM_WINDOW * 240];
    int n, pc_line, i, len = 0;
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->disasm);
    GtkTextIter s, e;

    /* Following, the PC sits a third of the way down the window. */
    if (gtk_check_button_get_active(w->follow_pc)) {
        chfdebug_cpu c;
        chfdebug_cpu_get(w->dbg, &c);
        w->disasm_top = chfdebug_row_address(w->dbg, (uint16_t)c.pc, -(DISASM_WINDOW / 3));
    }

    n = chfdebug_disassemble(w->dbg, (uint16_t)w->disasm_top, lines, DISASM_WINDOW, &pc_line);
    w->line_count = n;
    for (i = 0; i < n; i++) {
        w->line_addr[i] = lines[i].address;
        len += g_snprintf(text + len, sizeof text - len, "%c%c %04X  %-9s %-16s %-24s %s%s\n",
                          lines[i].has_breakpoint ? '*' : ' ',
                          lines[i].is_pc ? '>' : ' ',
                          lines[i].address, lines[i].bytes, lines[i].label,
                          lines[i].disasm, lines[i].comment[0] ? "; " : "",
                          lines[i].comment);
    }
    if (n == 0)
        len += g_snprintf(text + len, sizeof text - len, "(no disassembly)\n");
    gtk_text_buffer_set_text(b, text, -1);

    /* the PC line in the accent colour */
    if (pc_line >= 0 && pc_line < n) {
        gtk_text_buffer_get_iter_at_line(b, &s, pc_line);
        gtk_text_buffer_get_iter_at_line(b, &e, pc_line + 1);
        gtk_text_buffer_apply_tag_by_name(b, "pc", &s, &e);
    }
}

/* The console's inputs as the machine has latched them, the pots, the panel
 * and the beeper. */
static void refresh_io(DbgWin *w)
{
    static chfdebug_uvi u;
    chfdebug_io *io = &w->io;
    chfdebug_io before = *io;
    static char text[3072];
    char sticks[2][24], tone[48];
    int i;

    chfdebug_io_get(w->dbg, io);
    chfdebug_uvi_get(w->dbg, &u);
    for (i = 0; i < 2; i++)
        g_snprintf(sticks[i], sizeof sticks[i], "%s%s%s%s%s",
                   io->sticks[i] & 1 ? "U" : "-", io->sticks[i] & 2 ? "R" : "-",
                   io->sticks[i] & 4 ? "D" : "-", io->sticks[i] & 8 ? "L" : "-",
                   io->sticks[i] & 16 ? "F" : "-");
    /* the beeper: a divider of the line rate (MAME's arcadia_sound) */
    if (u.sound_volume && u.sound_tone)
        g_snprintf(tone, sizeof tone, "tone, divider %d", u.sound_pitch);
    else if (u.sound_volume && u.sound_noise)
        g_snprintf(tone, sizeof tone, "noise, divider %d", u.sound_pitch);
    else
        g_snprintf(tone, sizeof tone, "silent");
    g_snprintf(text, sizeof text,
        "%s   %s cartridge\n\n"
        "RESET key     %s\n"
        "SENSE         %d   (the UVI's vblank: line %d of %d)\n"
        "FLAG          %d\n"
        "\n"
        "Keys latched  %07llX   (bit 0-11 pad 1, 12-23 pad 2, 24-26 panel)\n"
        "Pad columns   %02X %02X %02X %02X %02X %02X %02X %02X   ($1900-$1907)\n"
        "Panel $1908   %02X   %s%s%s\n"
        "Discs         1: %s   2: %s   (U R D L F)\n"
        "Pots          1: x $%02X y $%02X   2: x $%02X y $%02X\n"
        "Analog disc   1: %d,%d   2: %d,%d   (0 = the digital directions)\n"
        "\n"
        "Beeper        %s   volume %d   ($18FD pitch %d, $18FE)\n"
        "Audio         %llu samples made\n",
        chf_machine_name(io->machine),
        io->cart_type == 3 ? "FujiNet" : io->cart_type == 2 ? "Golf" : "Standard",
        io->reset_held ? "held" : "-", io->sense, u.line, u.lines, io->flag,
        (unsigned long long)io->keys,
        io->pad[0], io->pad[1], io->pad[2], io->pad[3], io->pad[4], io->pad[5],
        io->pad[6], io->pad[7],
        io->panel, io->panel & 1 ? "START " : "", io->panel & 2 ? "OPTION " : "",
        io->panel & 4 ? "SELECT" : "",
        sticks[0], sticks[1],
        io->pot[0][0], io->pot[0][1], io->pot[1][0], io->pot[1][1],
        io->disc[0][0], io->disc[0][1], io->disc[1][0], io->disc[1][1],
        tone, u.sound_volume, u.sound_pitch,
        (unsigned long long)io->audio_samples);
    dbg_set_text(w->io_text, text);

    if (before.keys != io->keys || before.machine != io->machine ||
        before.reset_held != io->reset_held)
        gtk_widget_queue_draw(w->keys_area);
    if (memcmp(before.sticks, io->sticks, sizeof before.sticks) != 0)
        gtk_widget_queue_draw(w->sticks_area);
}

/* Both keypads and the panel as the console reads them, drawn from the
 * session's layout table: a held key is filled in the accent. */
#define KY_UNIT 44

static void draw_keys(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer ud)
{
    DbgWin *w = ud;
    GtkWidget *wg = GTK_WIDGET(area);
    int n, i;
    float kw, kh;
    const chf_keycap *caps = chfsession_keyboard_layout(w->io.machine, &n, &kw, &kh);
    (void)width; (void)height;

    for (i = 0; i < n; i++) {
        const chf_keycap *c = &caps[i];
        const gboolean held = c->target >= 0 ? (gboolean)((w->io.keys >> c->target) & 1)
                                             : w->io.reset_held != 0;
        const double x = c->x * KY_UNIT + 0.5, y = c->y * KY_UNIT + 0.5;
        const double cw = c->w * KY_UNIT, ch = c->h * KY_UNIT;
        cairo_rectangle(cr, x, y, cw, ch);
        if (held) {
            dbg_set_rgb(cr, CHFSESSION_ACCENT_RGB, 1.0);
            cairo_fill_preserve(cr);
        }
        dbg_set_fg(cr, wg, 0.35);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        if (held) cairo_set_source_rgb(cr, 0.12, 0.1, 0.08);
        else dbg_set_fg(cr, wg, 1.0);
        dbg_draw_centred(cr, wg, c->label, x, y, cw, ch, FALSE);
    }
}

/* Both discs: five lamps each, lit while the console sees the control held. */
#define ST_LAMP_W 58
#define ST_LAMP_H 26

static void draw_sticks(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer ud)
{
    static const char *const names[5] = { "UP", "RIGHT", "DOWN", "LEFT", "FIRE" };
    DbgWin *w = ud;
    GtkWidget *wg = GTK_WIDGET(area);
    int s, a;
    (void)width; (void)height;
    for (s = 0; s < 2; s++) {
        char t[16];
        g_snprintf(t, sizeof t, "Disc %d", s + 1);
        dbg_set_fg(cr, wg, 0.8);
        dbg_draw_centred(cr, wg, t, 0, s * (ST_LAMP_H + 6) , 56, ST_LAMP_H, FALSE);
        for (a = 0; a < 5; a++) {
            const gboolean on = (w->io.sticks[s] >> a) & 1;
            double x = 60 + a * (ST_LAMP_W + 4) + 0.5, y = s * (ST_LAMP_H + 6) + 0.5;
            cairo_rectangle(cr, x, y, ST_LAMP_W, ST_LAMP_H - 1);
            if (on) {
                dbg_set_rgb(cr, CHFSESSION_ACCENT_RGB, 1.0);
                cairo_fill_preserve(cr);
            }
            dbg_set_fg(cr, wg, 0.3);
            cairo_set_line_width(cr, 1);
            cairo_stroke(cr);
            if (on) cairo_set_source_rgb(cr, 0.12, 0.1, 0.08);
            else dbg_set_fg(cr, wg, 0.8);
            dbg_draw_centred(cr, wg, names[a], x, y, ST_LAMP_W, ST_LAMP_H - 1, FALSE);
        }
    }
}

static void refresh_cart(DbgWin *w)
{
    chfdebug_cart c;
    char info[256];
    static char text[4096], mbox[6144];
    int len = 0, i;

    chfdebug_cart_get(w->dbg, &c);
    chfdebug_cart_info(w->dbg, info, sizeof info);
    if (!c.present) {
        dbg_set_text(w->cart_text, info);
        dbg_set_text(w->mbox_text, "(no cartridge)");
        return;
    }
    len += g_snprintf(text + len, sizeof text - len,
        "%s\n\n"
        "Type          %s\n",
        info, c.type_name ? c.type_name : "?");
    if (c.type != 3) {
        len += g_snprintf(text + len, sizeof text - len,
            "ROM           %u bytes\n", c.rom_size);
    } else {
        len += g_snprintf(text + len, sizeof text - len,
            "Link          %s%s\n"
            "Mailbox       %s\n"
            "Live image    %u bytes   CRC-32 %08X   %s\n"
            "Staged        %s   claims the mailbox %s   armed %d   %u bytes   CRC-32 %08X\n"
            "Booted game   %s\n"
            "Swaps         %d\n"
            "Banks         %d   showing %d\n"
            "Boot          state %d   %d%%   error %d\n"
            "Mailbox regs  ACKSEQ $%02X   STATUS $%02X   last error %u   reply cmd $%02X   "
            "RXLEN %u\n"
            "Queue         %u events waiting\n",
            c.link_up ? "up" : "down", c.link_off ? "   (switched off)" : "",
            c.mailbox ? "open (a socket and fujimail)" : "closed (a game runs alone)",
            c.live_size, c.live_crc, c.app_live ? "a FujiNet app" : "a game or CONFIG alone",
            c.staged_app ? "an app" : "no", c.staged_mailbox ? "yes" : "no", c.armed,
            c.staged_size, c.staged_crc,
            c.booted_game ? "yes" : "no", c.swaps, c.nbanks, c.fbank,
            c.boot_state, c.boot_pct, c.boot_err,
            c.ackseq, c.status, c.last_error, c.reply_cmd, c.rxlen, c.queue_depth);
    }
    dbg_set_text(w->cart_text, text);

    len = 0;
    len += g_snprintf(mbox, sizeof mbox,
                      "       0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n");
    /* $2B00-$2FFF as the console reads it, 16 to a row */
    for (i = 0; i < MBOX_SIZE; i++) {
        if (i % 16 == 0)
            len += g_snprintf(mbox + len, sizeof mbox - len, "$%04X: ", MBOX_BASE + i);
        len += g_snprintf(mbox + len, sizeof mbox - len, "%02X ", c.mailbox_page[i]);
        if (i % 16 == 15)
            len += g_snprintf(mbox + len, sizeof mbox - len, "\n");
    }
    dbg_set_text(w->mbox_text, mbox);
}

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
    log_note_reader(w->fuji_text);
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
    set_text_at(w->fuji_text, t->str, force || following(w->fuji_text));
    g_string_free(t, TRUE);
}

static void bp_enable_toggled(GtkCheckButton *b, gpointer ud);
static void bp_remove_clicked(GtkButton *b, gpointer ud);

static const char *bp_type_name(int type)
{
    if (type & CHFDEBUG_BP_EXEC) return "Execute";
    if ((type & (CHFDEBUG_BP_READ | CHFDEBUG_BP_WRITE)) ==
        (CHFDEBUG_BP_READ | CHFDEBUG_BP_WRITE))
        return "Read/Write";
    if (type & CHFDEBUG_BP_READ) return "Read";
    return "Write";
}

static const char *bp_where(const chfdebug_breakpoint *bp, char *buf, int n)
{
    if (bp->type & CHFDEBUG_BP_EXEC)
        g_snprintf(buf, n, "$%04X", bp->start);
    else if (bp->end != bp->start)
        g_snprintf(buf, n, "$%04X-$%04X", bp->start, bp->end);
    else
        g_snprintf(buf, n, "$%04X", bp->start);
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
        GtkWidget *l = gtk_label_new("No breakpoints. Click a disassembly line, or add one below.");
        gtk_widget_add_css_class(l, "dim-label");
        gtk_list_box_append(w->bp_list, l);
        return;
    }
    for (i = 0; i < n; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *en = gtk_check_button_new();
        GtkWidget *rm = gtk_button_new_from_icon_name("user-trash-symbolic");
        GtkWidget *l;
        char text[320], where[48];
        g_snprintf(text, sizeof text, "#%-3d %-10s %-26s hits %-6u %s%s", bps[i].id,
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
    w->stale[tab] = FALSE;
    switch (tab) {
    case TAB_CPU: refresh_cpu(w); break;
    case TAB_DISASM: refresh_disasm(w); break;
    case TAB_VIDEO: dbg_video_refresh(w->video, !chfdebug_is_stopped(w->dbg)); break;
    case TAB_IO: refresh_io(w); break;
    case TAB_BREAKS: refresh_bps(w); break;
    case TAB_CART: refresh_cart(w); break;
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

static void on_reg_activate(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(entry), "reg"));
    long v;
    if (dbg_parse_num(gtk_editable_get_text(GTK_EDITABLE(entry)), &v))
        chfdebug_cpu_set(w->dbg, k_regs[i].reg, (int)v);
    refresh_all(w);
}

static void on_flag_toggled(GtkCheckButton *b, gpointer ud)
{
    DbgWin *w = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "flag"));
    if (w->syncing) return;
    chfdebug_cpu_set(w->dbg, k_flags[i].reg, gtk_check_button_get_active(b) ? 1 : 0);
    refresh_all(w);
}

/* A slot of the return address stack. */
static void on_ras_activate(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(entry), "ras"));
    long v;
    if (dbg_parse_num(gtk_editable_get_text(GTK_EDITABLE(entry)), &v))
        chfdebug_cpu_set(w->dbg, CHF_REG_RAS0 + i, (int)(v & 0x7FFF));
    refresh_all(w);
}

/* The viewer's address box and space: show from there (16-byte aligned). */
static void on_mem_go(GtkWidget *wgt, gpointer ud)
{
    DbgWin *w = ud;
    long a;
    (void)wgt;
    if (parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(w->mem_addr)), &a))
        w->mem_base = (uint32_t)a & ~0xFu;
    refresh_mem(w);
}

static void on_mem_space(GObject *o, GParamSpec *ps, gpointer ud)
{
    DbgWin *w = ud;
    (void)o; (void)ps;
    w->mem_base = 0;
    gtk_editable_set_text(GTK_EDITABLE(w->mem_addr), "0");
    refresh_mem(w);
}

/* A byte poked into the viewer's space (the UVI takes it with its write
 * effects; a cartridge buffer or RAM-like space takes it as data). */
static void on_poke(GtkWidget *wgt, gpointer ud)
{
    DbgWin *w = ud;
    long a, v;
    (void)wgt;
    if (!parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(w->poke_addr)), &a) ||
        !dbg_parse_num(gtk_editable_get_text(GTK_EDITABLE(w->poke_val)), &v))
        return;
    chfdebug_write(w->dbg, (int)gtk_drop_down_get_selected(w->mem_space),
                   (uint32_t)a, (uint8_t)v);
    refresh_all(w);
}

static void on_disasm_click(GtkGestureClick *g, int n, double x, double y, gpointer ud)
{
    DbgWin *w = ud;
    GtkTextIter it;
    int bx, by, line;
    (void)g; (void)n;
    gtk_text_view_window_to_buffer_coords(w->disasm, GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &bx, &by);
    gtk_text_view_get_iter_at_location(w->disasm, &it, bx, by);
    line = gtk_text_iter_get_line(&it);
    if (line < 0 || line >= w->line_count) return;
    chfdebug_breakpoint_toggle(w->dbg, w->line_addr[line]);
    refresh_disasm(w);
    w->stale[TAB_BREAKS] = TRUE;
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
    w->disasm_top = (int)(a & 0x7FFF);
    refresh_disasm(w);
}

static gboolean on_disasm_scroll(GtkEventControllerScroll *c, double dx, double dy, gpointer ud)
{
    DbgWin *w = ud;
    int rows = (int)(dy * 3);
    (void)c; (void)dx;
    if (!rows) rows = dy > 0 ? 1 : -1;
    gtk_check_button_set_active(w->follow_pc, FALSE);
    w->disasm_top = chfdebug_row_address(w->dbg, (uint16_t)w->disasm_top, rows);
    refresh_disasm(w);
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

static void on_bp_add(GtkWidget *wgt, gpointer ud)
{
    DbgWin *w = ud;
    static const int types[4] = { CHFDEBUG_BP_EXEC, CHFDEBUG_BP_READ, CHFDEBUG_BP_WRITE,
                                  CHFDEBUG_BP_READ | CHFDEBUG_BP_WRITE };
    guint sel = gtk_drop_down_get_selected(w->bp_type);
    long a = 0, b = 0;
    int id;
    const char *end = gtk_editable_get_text(GTK_EDITABLE(w->bp_end));
    (void)wgt;
    if (sel >= G_N_ELEMENTS(types)) sel = 0;
    if (!parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(w->bp_start)), &a)) {
        gtk_label_set_text(w->bp_msg, "Not an address or a label");
        return;
    }
    b = a;
    if (end && *end && !parse_addr(w, end, &b)) {
        gtk_label_set_text(w->bp_msg, "The end is not an address or a label");
        return;
    }
    if (b < a) {
        gtk_label_set_text(w->bp_msg, "The end comes before the start");
        return;
    }
    id = chfdebug_breakpoint_add(w->dbg, types[sel], CHF_SPACE_PROG, (uint16_t)a, (uint16_t)b,
                                 gtk_editable_get_text(GTK_EDITABLE(w->bp_cond)));
    if (id < 0) {
        gtk_label_set_text(w->bp_msg, "The condition does not parse, or the range is outside $0000-$7FFF");
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
    gtk_file_dialog_set_title(dlg, "Load Symbols (AS .map / .lst, WLA-DX .sym, AAAA name)");
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

/* <cart>.map/.lst/.sym next to the opened cartridge, no picker needed. */
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

static void on_switch_page(GtkNotebook *nb, GtkWidget *page, guint num, gpointer ud)
{
    DbgWin *w = ud;
    (void)nb; (void)page;
    if (num < TAB_COUNT)
        refresh_tab(w, (int)num);
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
        { "dis", "Disassembly ($0000-$7FFF as code)", "disassembly.asm" },
        { "mailbox", "Mailbox page ($2B00-$2FFF)", "mailbox.bin" },
        { "uvi", "UVI registers (768 bytes)", "uvi.bin" },
        { "font", "Glyph table (64 x 8 bytes)", "font.bin" },
        { "cart", "Cartridge image", "cartridge.bin" },
        { "screen", "Screen (PPM)", "screen.ppm" },
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
    TB("Scanline+1", on_scanline);
    TB("Frame+1", on_frame);
#undef TB

    /* Load Symbols: a file, or (the arrow) the files beside the cartridge */
    sym = adw_split_button_new();
    adw_split_button_set_label(ADW_SPLIT_BUTTON(sym), "Load Symbols\xe2\x80\xa6");
    adw_split_button_set_dropdown_tooltip(ADW_SPLIT_BUTTON(sym), "More ways to load symbols");
    g_signal_connect(sym, "clicked", G_CALLBACK(on_symbols), w);
    sympop = gtk_popover_new();
    beside = gtk_button_new_with_label("Beside the Cartridge (.map, .lst, .sym)");
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
        "Command (help, step, break, bpw, print, mem, poke, uvi, objects, screen, io, cart, "
        "disasm, ...) \xe2\x80\x94 Tab completes, Up/Down recall");
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
    dbg_set_text(w->prompt_out, "Channel F debugger prompt (the Signetics 2650 and the 2637 "
                                "UVI). Type 'help' for every command.\n");
    return dbg_padded(box);
}

static GtkWidget *reg_entry(DbgWin *w, int i)
{
    w->reg[i] = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_max_length(w->reg[i], 4);
    gtk_editable_set_width_chars(GTK_EDITABLE(w->reg[i]), k_regs[i].digits + 1);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(w->reg[i]), k_regs[i].digits + 1);
    gtk_widget_add_css_class(GTK_WIDGET(w->reg[i]), "monospace");
    g_object_set_data(G_OBJECT(w->reg[i]), "reg", GINT_TO_POINTER(i));
    g_signal_connect(w->reg[i], "activate", G_CALLBACK(on_reg_activate), w);
    return GTK_WIDGET(w->reg[i]);
}

/* One row of R1-R3 (index `first` in k_regs) under a label that says which
 * RS selects them. */
static GtkWidget *bank_row(DbgWin *w, int bank, int first)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    int i;

    w->bank_label[bank] = gtk_label_new(bank ? "RS=1" : "RS=0");
    gtk_widget_add_css_class(w->bank_label[bank], "arc-pill");
    gtk_widget_set_size_request(w->bank_label[bank], 52, -1);
    gtk_box_append(GTK_BOX(row), w->bank_label[bank]);
    for (i = 0; i < 3; i++)
        gtk_box_append(GTK_BOX(row), dbg_labeled(k_regs[first + i].name, reg_entry(w, first + i)));
    return row;
}

static GtkWidget *build_cpu(DbgWin *w)
{
    static const char *const pills[NPILLS] = { "S (sense)", "HALT", "IRQ", "ILLEGAL" };
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 32);
    GtkWidget *regs = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *stack = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *row0 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *rowp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *flags = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *pinrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *memtools = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *poketools = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *grid = gtk_grid_new();
    GtkWidget *l, *go, *poke, *memscroll;
    static const char *const spaces[] = { "PROG (CPU view)", "UVI registers", "Cartridge", NULL };
    int i;

    /* R0, then the two sets of R1-R3 */
    gtk_box_append(GTK_BOX(row0), dbg_labeled("R0", reg_entry(w, 0)));
    gtk_box_append(GTK_BOX(regs), row0);
    gtk_box_append(GTK_BOX(regs), bank_row(w, 0, 1));
    gtk_box_append(GTK_BOX(regs), bank_row(w, 1, 4));
    /* PC and IAR, PSU, PSL, SP, CC */
    for (i = 7; i < 9; i++)
        gtk_box_append(GTK_BOX(rowp), dbg_labeled(k_regs[i].name, reg_entry(w, i)));
    for (i = 9; i < NREGS; i++)
        gtk_box_append(GTK_BOX(rowp), dbg_labeled(k_regs[i].name, reg_entry(w, i)));
    gtk_box_append(GTK_BOX(regs), rowp);
    l = gtk_label_new("Flags");
    gtk_widget_add_css_class(l, "dim-label");
    gtk_box_append(GTK_BOX(flags), l);
    for (i = 0; i < NFLAGS; i++) {
        w->flag[i] = gtk_check_button_new_with_label(k_flags[i].name);
        g_object_set_data(G_OBJECT(w->flag[i]), "flag", GINT_TO_POINTER(i));
        g_signal_connect(w->flag[i], "toggled", G_CALLBACK(on_flag_toggled), w);
        gtk_box_append(GTK_BOX(flags), w->flag[i]);
    }
    gtk_box_append(GTK_BOX(regs), flags);
    for (i = 0; i < NPILLS; i++) {
        w->pill[i] = gtk_label_new(pills[i]);
        gtk_widget_add_css_class(w->pill[i], "arc-pill");
        gtk_widget_add_css_class(w->pill[i], "dim-label");
        gtk_box_append(GTK_BOX(pinrow), w->pill[i]);
    }
    gtk_box_append(GTK_BOX(regs), pinrow);
    w->cpu_info = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->cpu_info), "dim-label");
    gtk_label_set_xalign(w->cpu_info, 0);
    gtk_label_set_ellipsize(w->cpu_info, PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(regs), GTK_WIDGET(w->cpu_info));
    gtk_box_append(GTK_BOX(regs), dbg_hint("PSU = S F II . . SP(3); PSL = CC IDC RS WC OVF COM C. "
                                           "Enter in a register or flag writes it; PC is the 15-bit "
                                           "address, IAR its low 13 bits.", 44));

    /* the return stack: a call increments SP and stores the return address
     * at that slot, so slot SP is the top and slots 1..SP are live */
    gtk_box_append(GTK_BOX(stack), dbg_heading("Return address stack"));
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
    for (i = 0; i < 8; i++) {
        char name[8];
        g_snprintf(name, sizeof name, "%d", i);
        w->ras_label[i] = gtk_label_new(name);
        gtk_widget_add_css_class(w->ras_label[i], "arc-pill");
        gtk_widget_add_css_class(w->ras_label[i], "dim-label");
        w->ras[i] = GTK_ENTRY(gtk_entry_new());
        gtk_entry_set_max_length(w->ras[i], 5);
        gtk_editable_set_width_chars(GTK_EDITABLE(w->ras[i]), 5);
        gtk_editable_set_max_width_chars(GTK_EDITABLE(w->ras[i]), 5);
        gtk_widget_add_css_class(GTK_WIDGET(w->ras[i]), "monospace");
        g_object_set_data(G_OBJECT(w->ras[i]), "ras", GINT_TO_POINTER(i));
        g_signal_connect(w->ras[i], "activate", G_CALLBACK(on_ras_activate), w);
        gtk_grid_attach(GTK_GRID(grid), w->ras_label[i], 0, i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(w->ras[i]), 1, i, 1, 1);
    }
    gtk_box_append(GTK_BOX(stack), grid);
    gtk_box_append(GTK_BOX(top), regs);
    gtk_box_append(GTK_BOX(top), stack);

    w->beam = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->beam), "dim-label");
    gtk_label_set_xalign(w->beam, 0);
    gtk_label_set_ellipsize(w->beam, PANGO_ELLIPSIZE_END);

    /* the memory viewer: the console has no RAM, so this is the CPU's view
     * (cartridge, UVI), the UVI's own register file, and the cartridge's
     * buffer */
    w->mem_space = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(spaces));
    g_signal_connect(w->mem_space, "notify::selected", G_CALLBACK(on_mem_space), w);
    w->mem_addr = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->mem_addr, "$1800 or label");
    gtk_editable_set_width_chars(GTK_EDITABLE(w->mem_addr), 14);
    g_signal_connect(w->mem_addr, "activate", G_CALLBACK(on_mem_go), w);
    go = gtk_button_new_with_label("Go");
    g_signal_connect(go, "clicked", G_CALLBACK(on_mem_go), w);
    gtk_box_append(GTK_BOX(memtools), dbg_heading("Memory"));
    gtk_box_append(GTK_BOX(memtools), GTK_WIDGET(w->mem_space));
    gtk_box_append(GTK_BOX(memtools), dbg_labeled("From", GTK_WIDGET(w->mem_addr)));
    gtk_box_append(GTK_BOX(memtools), go);
    w->poke_addr = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->poke_addr, "address");
    gtk_editable_set_width_chars(GTK_EDITABLE(w->poke_addr), 10);
    w->poke_val = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->poke_val, "value");
    gtk_editable_set_width_chars(GTK_EDITABLE(w->poke_val), 6);
    g_signal_connect(w->poke_val, "activate", G_CALLBACK(on_poke), w);
    poke = gtk_button_new_with_label("Poke");
    g_signal_connect(poke, "clicked", G_CALLBACK(on_poke), w);
    gtk_box_append(GTK_BOX(poketools), dbg_labeled("Poke", GTK_WIDGET(w->poke_addr)));
    gtk_box_append(GTK_BOX(poketools), GTK_WIDGET(w->poke_val));
    gtk_box_append(GTK_BOX(poketools), poke);
    gtk_box_append(GTK_BOX(poketools),
                   dbg_hint("The UVI takes a poke with its write effects; ROM ignores it.", 30));
    memscroll = dbg_mono_view(&w->mem_text, FALSE);

    gtk_box_append(GTK_BOX(box), top);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(w->beam));
    gtk_box_append(GTK_BOX(box), memtools);
    gtk_box_append(GTK_BOX(box), memscroll);
    gtk_box_append(GTK_BOX(box), poketools);
    return dbg_padded(box);
}

static GtkWidget *build_disasm(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *scroll = dbg_mono_view(&w->disasm, FALSE);
    GtkGesture *click = gtk_gesture_click_new();
    GtkEventController *scrollc = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    GtkWidget *hint;

    accent_tag(w->disasm, "pc");

    w->follow_pc = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Follow PC"));
    gtk_check_button_set_active(w->follow_pc, TRUE);
    g_signal_connect(w->follow_pc, "toggled", G_CALLBACK(on_follow_toggled), w);
    w->jump = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->jump, "$0400 or label");
    gtk_editable_set_width_chars(GTK_EDITABLE(w->jump), 14);
    g_signal_connect(w->jump, "activate", G_CALLBACK(on_jump), w);
    hint = gtk_label_new("Click a line to toggle its breakpoint; scroll to browse");
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->follow_pc));
    gtk_box_append(GTK_BOX(row), dbg_labeled("Go to", GTK_WIDGET(w->jump)));
    gtk_box_append(GTK_BOX(row), hint);

    g_signal_connect(click, "pressed", G_CALLBACK(on_disasm_click), w);
    gtk_widget_add_controller(GTK_WIDGET(w->disasm), GTK_EVENT_CONTROLLER(click));
    g_signal_connect(scrollc, "scroll", G_CALLBACK(on_disasm_scroll), w);
    gtk_widget_add_controller(GTK_WIDGET(w->disasm), scrollc);
    /* No vertical scrollbar (the wheel walks the disassembly instead), and
     * EXTERNAL rather than NEVER: NEVER would make the window's minimum
     * height all 48 lines. */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_EXTERNAL);

    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), scroll);
    w->disasm_top = 0;
    return dbg_padded(box);
}

/* The text on the left; on the right the keypads and panel, and both discs. */
static GtkWidget *build_io(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    float kw, kh;

    chfsession_keyboard_layout(CHF_MACHINE_ARCADIA, NULL, &kw, &kh);
    w->keys_area = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(w->keys_area), (int)(kw * KY_UNIT) + 2);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(w->keys_area), (int)(kh * KY_UNIT) + 2);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(w->keys_area), draw_keys, w, NULL);
    gtk_widget_set_halign(w->keys_area, GTK_ALIGN_START);

    w->sticks_area = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(w->sticks_area), 60 + 5 * (ST_LAMP_W + 4));
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(w->sticks_area), 2 * (ST_LAMP_H + 6));
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(w->sticks_area), draw_sticks, w, NULL);
    gtk_widget_set_halign(w->sticks_area, GTK_ALIGN_START);

    w->io.machine = CHF_MACHINE_ARCADIA;
    {
        GtkWidget *text = dbg_mono_view(&w->io_text, FALSE);
        gtk_text_view_set_wrap_mode(w->io_text, GTK_WRAP_WORD_CHAR);
        gtk_widget_set_size_request(text, 560, -1);
        gtk_widget_set_hexpand(text, FALSE);
        gtk_box_append(GTK_BOX(box), text);
    }
    gtk_box_append(GTK_BOX(right), dbg_heading("Keypads and panel (as the console latched them)"));
    gtk_box_append(GTK_BOX(right), w->keys_area);
    gtk_box_append(GTK_BOX(right), dbg_hint("Held keys are filled.", 30));
    gtk_box_append(GTK_BOX(right), dbg_heading("Discs"));
    gtk_box_append(GTK_BOX(right), w->sticks_area);
    gtk_box_append(GTK_BOX(box), right);
    return dbg_padded(box);
}

static GtkWidget *build_breaks(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *add, *clear;
    static const char *const types[] = { "Execute", "Read", "Write", "Read/Write", NULL };

    w->bp_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(w->bp_list, GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(w->bp_list));
    gtk_widget_set_vexpand(scroll, TRUE);

    w->bp_type = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(types));
    w->bp_start = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->bp_start, "$0400 / label");
    gtk_editable_set_width_chars(GTK_EDITABLE(w->bp_start), 14);
    w->bp_end = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->bp_end, "(end)");
    gtk_editable_set_width_chars(GTK_EDITABLE(w->bp_end), 8);
    w->bp_cond = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->bp_cond, "condition (r0 == $FF && vpos > 100) \xe2\x80\x94 optional");
    gtk_widget_set_hexpand(GTK_WIDGET(w->bp_cond), TRUE);
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

    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(w->bp_msg));
    return dbg_padded(box);
}

/* The state as text on the left; the mailbox page the console reads on the
 * right. */
static GtkWidget *build_cart(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *mbox = dbg_mono_view(&w->mbox_text, FALSE);
    GtkWidget *text = dbg_mono_view(&w->cart_text, FALSE);

    gtk_text_view_set_wrap_mode(w->cart_text, GTK_WRAP_WORD_CHAR);
    gtk_widget_set_size_request(text, 480, -1);
    gtk_widget_set_hexpand(text, FALSE);
    gtk_widget_set_size_request(mbox, 420, -1);
    gtk_box_append(GTK_BOX(right), dbg_heading("Mailbox page ($2B00-$2FFF) as the console reads it"));
    gtk_box_append(GTK_BOX(right), mbox);
    gtk_box_append(GTK_BOX(box), text);
    gtk_box_append(GTK_BOX(box), right);
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
    make_log(w->fuji_text);

    gtk_box_append(GTK_BOX(right), dbg_heading("The cartridge's mailbox"));
    gtk_box_append(GTK_BOX(right), dbg_mono_view(&w->fuji_state, FALSE));

    gtk_paned_set_start_child(GTK_PANED(paned), left);
    gtk_paned_set_end_child(GTK_PANED(paned), right);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), FALSE);
    gtk_widget_set_size_request(left, 620, -1);
    gtk_widget_set_size_request(right, 320, -1);
    w->fuji_count = -1;
    return dbg_padded(paned);
}

static void on_destroy(GtkWidget *widget, gpointer ud)
{
    DbgWin *w = ud;
    (void)widget;
    if (w->timer) g_source_remove(w->timer);
    chfdebug_detach(w->dbg);
    dbg_video_free(w->video);
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
    GtkWidget *toolbar, *header, *root, *notebook, *overlay, *video;
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

    w->video = dbg_video_new(session, w->dbg, &video);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(root), build_toolbar(w));
    notebook = gtk_notebook_new();
    w->notebook = GTK_NOTEBOOK(notebook);
    gtk_widget_set_vexpand(notebook, TRUE);
    gtk_notebook_append_page(w->notebook, build_prompt(w), gtk_label_new("Prompt"));
    gtk_notebook_append_page(w->notebook, build_cpu(w), gtk_label_new("CPU & RAM"));
    gtk_notebook_append_page(w->notebook, build_disasm(w), gtk_label_new("Disassembly"));
    gtk_notebook_append_page(w->notebook, video, gtk_label_new("Video"));
    gtk_notebook_append_page(w->notebook, build_io(w), gtk_label_new("Sound & I/O"));
    gtk_notebook_append_page(w->notebook, build_breaks(w), gtk_label_new("Breakpoints"));
    gtk_notebook_append_page(w->notebook, build_cart(w), gtk_label_new("Cart"));
    gtk_notebook_append_page(w->notebook, build_fujinet(w), gtk_label_new("FujiNet"));
    gtk_box_append(GTK_BOX(root), notebook);
    tab = tab_from_env();
    if (tab >= 0) gtk_notebook_set_current_page(w->notebook, tab);
    g_signal_connect(notebook, "switch-page", G_CALLBACK(on_switch_page), w);

    overlay = adw_toast_overlay_new();
    w->toasts = ADW_TOAST_OVERLAY(overlay);
    adw_toast_overlay_set_child(w->toasts, root);

    /* the status line -- why the machine stopped -- is the title's subtitle */
    header = adw_header_bar_new();
    w->title = ADW_WINDOW_TITLE(adw_window_title_new("Debugger", ""));
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), GTK_WIDGET(w->title));
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
