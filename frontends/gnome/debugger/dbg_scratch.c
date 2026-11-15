/*
 * The scratchpad, for the Memory tab when the SCRATCH space is selected: the
 * F8's 64 registers as they stand.
 *
 *   grid     the 64 bytes as an 8 x 8 grid, addressed in octal as ISAR
 *            addresses them (the row is the upper digit, the column the
 *            lower), each cell named by its register number (r9 is J, r10 and
 *            r11 H, r12 and r13 K, r14 and r15 Q), ISAR's cell outlined, the
 *            cells that changed in the accent and the BIOS K stack's band
 *            (r40-r59) shaded. Click a cell to edit it.
 *   K stack  the saved returns of the BIOS's PUSH K / POP K, oldest first.
 *
 * The CPU registers themselves live in the window's strip.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_common.h"

#include <string.h>

#define CELL_W 58
#define CELL_H 38
#define HEAD_W 22
#define HEAD_H 20
#define KSTACK_FIRST 40
#define KSTACK_LAST 59

struct DbgScratch {
    chfsession *session;
    chfdebug *dbg;
    DbgChanged changed;
    gpointer ud;

    chfdebug_cpu cur;
    uint8_t base[64];           /* the scratchpad as it was before the last change */
    gboolean have_base;
    unsigned seen_gen;

    GtkWidget *grid;
    int sel;                    /* the selected register, or -1 */
    GtkLabel *cell_name;
    GtkEntry *cell_val;
    GtkLabel *kstack;
};

/* ---- names ------------------------------------------------------------------ */

/* A scratchpad register's name: its number, and J H K Q where it has one. */
static void reg_name(int r, char *dst, int n)
{
    static const char *const special[7] = { "J", "HU", "HL", "KU", "KL", "QU", "QL" };
    if (r >= 9 && r <= 15)
        g_snprintf(dst, n, "r%d %s", r, special[r - 9]);
    else
        g_snprintf(dst, n, "r%d", r);
}

/* ---- the scratchpad grid ----------------------------------------------------- */

#define GRID_W (HEAD_W + 8 * CELL_W + 2)
#define GRID_H (HEAD_H + 8 * CELL_H + 2)

static void draw_grid(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer ud)
{
    DbgScratch *c = ud;
    GtkWidget *wg = GTK_WIDGET(area);
    const guint32 line = dbg_accent_line_rgb();
    int r, col;
    (void)width; (void)height;

    cairo_set_line_width(cr, 1);
    for (col = 0; col < 8; col++) {
        char t[4];
        g_snprintf(t, sizeof t, "%d", col);
        dbg_set_fg(cr, wg, 0.55);
        dbg_draw_centred(cr, wg, t, HEAD_W + col * CELL_W, 0, CELL_W, HEAD_H, TRUE);
    }
    for (r = 0; r < 8; r++) {
        char t[4];
        g_snprintf(t, sizeof t, "%d", r);
        dbg_set_fg(cr, wg, 0.55);
        dbg_draw_centred(cr, wg, t, 0, HEAD_H + r * CELL_H, HEAD_W, CELL_H, TRUE);
    }
    for (r = 0; r < 64; r++) {
        const double x = HEAD_W + (r % 8) * CELL_W + 0.5, y = HEAD_H + (r / 8) * CELL_H + 0.5;
        const gboolean changed = c->have_base && c->cur.r[r] != c->base[r];
        const gboolean band = r >= KSTACK_FIRST && r <= KSTACK_LAST;
        char name[16], val[8];

        if (changed) {
            dbg_set_rgb(cr, CHFSESSION_ACCENT_RGB, 1.0);
            cairo_rectangle(cr, x, y, CELL_W - 1, CELL_H - 1);
            cairo_fill(cr);
        } else if (band) {
            dbg_set_fg(cr, wg, 0.10);
            cairo_rectangle(cr, x, y, CELL_W - 1, CELL_H - 1);
            cairo_fill(cr);
        }
        dbg_set_fg(cr, wg, 0.22);
        cairo_rectangle(cr, x, y, CELL_W - 1, CELL_H - 1);
        cairo_stroke(cr);

        reg_name(r, name, sizeof name);
        if (changed) dbg_set_rgb(cr, 0x1E1A14, 0.75);
        else dbg_set_fg(cr, wg, 0.6);
        dbg_draw_text(cr, wg, name, x + 4, y + 2, FALSE, 10);
        g_snprintf(val, sizeof val, "%02X", c->cur.r[r]);
        if (changed) dbg_set_rgb(cr, 0x1E1A14, 1.0);
        else dbg_set_fg(cr, wg, 1.0);
        dbg_draw_centred(cr, wg, val, x, y + 12, CELL_W - 1, CELL_H - 14, TRUE);
    }
    /* the selected cell, then ISAR's on top of it */
    if (c->sel >= 0) {
        dbg_set_fg(cr, wg, 0.9);
        cairo_set_line_width(cr, 1);
        cairo_rectangle(cr, HEAD_W + (c->sel % 8) * CELL_W + 2.5,
                        HEAD_H + (c->sel / 8) * CELL_H + 2.5, CELL_W - 6, CELL_H - 6);
        cairo_stroke(cr);
    }
    r = c->cur.isar & 0x3F;
    dbg_set_rgb(cr, line, 1.0);
    cairo_set_line_width(cr, 2.5);
    cairo_rectangle(cr, HEAD_W + (r % 8) * CELL_W + 1.5, HEAD_H + (r / 8) * CELL_H + 1.5,
                    CELL_W - 3, CELL_H - 3);
    cairo_stroke(cr);
}

static void cell_select(DbgScratch *c, int r)
{
    char name[16], text[40], val[8];
    c->sel = r;
    if (r < 0) {
        gtk_label_set_text(c->cell_name, "Click a cell to edit it");
        gtk_widget_set_sensitive(GTK_WIDGET(c->cell_val), FALSE);
    } else {
        reg_name(r, name, sizeof name);
        g_snprintf(text, sizeof text, "%s  (ISAR %o.%o)", name, r >> 3, r & 7);
        gtk_label_set_text(c->cell_name, text);
        g_snprintf(val, sizeof val, "%02X", c->cur.r[r]);
        gtk_editable_set_text(GTK_EDITABLE(c->cell_val), val);
        gtk_widget_set_sensitive(GTK_WIDGET(c->cell_val), TRUE);
    }
    gtk_widget_queue_draw(c->grid);
}

static void on_grid_click(GtkGestureClick *g, int n, double x, double y, gpointer ud)
{
    DbgScratch *c = ud;
    const int col = (int)((x - HEAD_W) / CELL_W), row = (int)((y - HEAD_H) / CELL_H);
    (void)g; (void)n;
    if (x < HEAD_W || y < HEAD_H || col < 0 || col > 7 || row < 0 || row > 7)
        return;
    cell_select(c, row * 8 + col);
}

static void on_cell_val(GtkEntry *e, gpointer ud)
{
    DbgScratch *c = ud;
    long v;
    (void)e;
    if (c->sel >= 0 && dbg_parse_num(gtk_editable_get_text(GTK_EDITABLE(c->cell_val)), &v)) {
        chfdebug_cpu_set(c->dbg, CHF_REG_R0 + c->sel, (int)(v & 0xFF));
        if (c->changed) c->changed(c->ud);
    }
}

/* ---- refresh -------------------------------------------------------------------- */

void dbg_scratch_refresh(DbgScratch *c, gboolean live)
{
    chfdebug_cpu cpu;
    const unsigned gen = chfdebug_generation(c->dbg);
    int i;

    chfdebug_cpu_get(c->dbg, &cpu);
    /* the cells that "changed" are against the last time the machine moved: a
     * step or a command (the generation), or the last refresh of a running one */
    if (!c->have_base) {
        memcpy(c->base, cpu.r, sizeof c->base);
        c->have_base = TRUE;
        c->seen_gen = gen;
    } else if (live || gen != c->seen_gen) {
        memcpy(c->base, c->cur.r, sizeof c->base);
        c->seen_gen = gen;
    }
    c->cur = cpu;

    /* the BIOS K stack, oldest return first */
    {
        GString *s = g_string_new(NULL);
        if (cpu.k_depth < 0) {
            g_string_append_printf(s, "r59 = $%02X is not a K-stack pointer\n(PUSH K / POP K are not in use)",
                                   cpu.r[59]);
        } else {
            g_string_append_printf(s, "depth %d   r59 = $%02X\n", cpu.k_depth, cpu.r[59]);
            if (cpu.k_depth == 0)
                g_string_append(s, "(empty)");
            for (i = 0; i < cpu.k_depth && i < 10; i++)
                g_string_append_printf(s, "%2d: $%04X%s\n", i + 1, cpu.k_stack[i],
                                       i == cpu.k_depth - 1 ? "   (top)" : "");
        }
        gtk_label_set_text(c->kstack, s->str);
        g_string_free(s, TRUE);
    }

    gtk_widget_queue_draw(c->grid);
    if (c->sel >= 0 && !gtk_widget_has_focus(GTK_WIDGET(c->cell_val))) {
        char val[8];
        g_snprintf(val, sizeof val, "%02X", cpu.r[c->sel]);
        dbg_set_entry(c->dbg, c->cell_val, val);
    }
}

/* Select a register in the grid from outside (a Go To in the SCRATCH space). */
void dbg_scratch_select(DbgScratch *c, int reg)
{
    cell_select(c, reg >= 0 && reg < 64 ? reg : -1);
}

/* ---- construction --------------------------------------------------------------- */

static GtkWidget *build_scratch(DbgScratch *c)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *edit = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkGesture *click = gtk_gesture_click_new();

    c->grid = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(c->grid), GRID_W);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(c->grid), GRID_H);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(c->grid), draw_grid, c, NULL);
    gtk_widget_set_halign(c->grid, GTK_ALIGN_START);
    g_signal_connect(click, "pressed", G_CALLBACK(on_grid_click), c);
    gtk_widget_add_controller(c->grid, GTK_EVENT_CONTROLLER(click));

    c->cell_name = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(c->cell_name, 0);
    gtk_widget_set_size_request(GTK_WIDGET(c->cell_name), 190, -1);
    c->cell_val = GTK_ENTRY(dbg_hex_entry(2));
    g_signal_connect(c->cell_val, "activate", G_CALLBACK(on_cell_val), c);
    gtk_box_append(GTK_BOX(edit), GTK_WIDGET(c->cell_name));
    gtk_box_append(GTK_BOX(edit), GTK_WIDGET(c->cell_val));

    gtk_box_append(GTK_BOX(box), dbg_heading("Scratchpad (ISAR addresses, octal: row . column)"));
    gtk_box_append(GTK_BOX(box), c->grid);
    gtk_box_append(GTK_BOX(box), edit);
    gtk_box_append(GTK_BOX(box),
        dbg_hint("ISAR's cell is outlined, changed cells are yellow, the BIOS K stack "
                 "(r40-r59) is shaded.", 40));
    cell_select(c, -1);
    return box;
}

static GtkWidget *build_kstack(DbgScratch *c)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *frame = gtk_frame_new(NULL);

    c->kstack = GTK_LABEL(dbg_mono_label());
    gtk_widget_set_margin_top(GTK_WIDGET(c->kstack), 6);
    gtk_widget_set_margin_bottom(GTK_WIDGET(c->kstack), 6);
    gtk_widget_set_margin_start(GTK_WIDGET(c->kstack), 8);
    gtk_widget_set_margin_end(GTK_WIDGET(c->kstack), 8);
    gtk_label_set_width_chars(c->kstack, 26);
    gtk_frame_set_child(GTK_FRAME(frame), GTK_WIDGET(c->kstack));
    gtk_box_append(GTK_BOX(box), dbg_heading("K stack (the BIOS's PUSH K / POP K)"));
    gtk_box_append(GTK_BOX(box), frame);
    return box;
}

DbgScratch *dbg_scratch_new(chfsession *session, chfdebug *dbg, DbgChanged changed, gpointer ud,
                            GtkWidget **root)
{
    DbgScratch *c = g_new0(DbgScratch, 1);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 28);

    c->session = session;
    c->dbg = dbg;
    c->changed = changed;
    c->ud = ud;
    c->sel = -1;
    gtk_box_append(GTK_BOX(box), build_scratch(c));
    gtk_box_append(GTK_BOX(box), build_kstack(c));
    *root = box;
    return c;
}

void dbg_scratch_free(DbgScratch *c)
{
    g_free(c);
}
