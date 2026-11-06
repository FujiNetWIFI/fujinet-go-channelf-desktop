/*
 * The debugger's CPU & RAM tab: the F8 as it stands.
 *
 *   registers  PC (the instruction about to run), PC0 and PC1 (the program
 *              counter as the bus sees it and its one return address), DC0
 *              and DC1, the data bus, A, W as its flags (S labelled positive,
 *              since the F8 sets it when the result is), ISAR in octal as
 *              upper.lower, and J, H, K and Q, with the clocks, the beam and
 *              the illegal-opcode count. Every field is editable while the
 *              machine is stopped.
 *   scratchpad the 64 bytes as an 8 x 8 grid, addressed in octal as ISAR
 *              addresses them (the row is the upper digit, the column the
 *              lower), each cell named by its register number (r9 is J, r10
 *              and r11 H, r12 and r13 K, r14 and r15 Q), ISAR's cell
 *              outlined, the cells that changed in the accent and the BIOS K
 *              stack's band (r40-r59) shaded. Click a cell to edit it.
 *   K stack    the saved returns of the BIOS's PUSH K / POP K, oldest first.
 *   memory     a hex view over the five spaces (PROG, scratchpad, VRAM, the
 *              cartridge and the I/O ports) with a poke.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_common.h"

#include <stdlib.h>
#include <string.h>

#define MEM_ROWS 16
#define CELL_W 58
#define CELL_H 38
#define HEAD_W 22
#define HEAD_H 20
#define KSTACK_FIRST 40
#define KSTACK_LAST 59

/* the registers as entries */
enum { R_PC, R_PC0, R_PC1, R_DC0, R_DC1, R_DBUS, R_A, R_W, R_ISAR, R_J, R_H, R_K, R_Q, R_COUNT };
static const struct { const char *name; int reg; int digits; } k_regs[R_COUNT] = {
    { "PC", CHF_REG_PC, 4 }, { "PC0", CHF_REG_PC0, 4 }, { "PC1", CHF_REG_PC1, 4 },
    { "DC0", CHF_REG_DC0, 4 }, { "DC1", CHF_REG_DC1, 4 }, { "bus", -1, 2 },
    { "A", CHF_REG_A, 2 }, { "W", CHF_REG_W, 2 }, { "ISAR", CHF_REG_ISAR, 3 },
    { "J", CHF_REG_J, 2 }, { "H", CHF_REG_H, 4 }, { "K", CHF_REG_K, 4 }, { "Q", CHF_REG_Q, 4 },
};
/* W's bits as check buttons */
static const struct { const char *name; const char *tip; int reg; } k_flags[] = {
    { "S (positive)", "Sign: set when the result was positive (the F8's S is inverted)", CHF_REG_SIGN },
    { "C", "Carry", CHF_REG_CARRY },
    { "Z", "Zero", CHF_REG_ZERO },
    { "O", "Overflow", CHF_REG_OVERFLOW },
    { "ICB", "Interrupt control bit (the Channel F never interrupts)", CHF_REG_ICB },
};
#define NFLAGS ((int)G_N_ELEMENTS(k_flags))

struct DbgCpu {
    chfsession *session;
    chfdebug *dbg;
    DbgChanged changed;
    gpointer ud;
    gboolean syncing;

    chfdebug_cpu cur;
    uint8_t base[64];           /* the scratchpad as it was before the last change */
    gboolean have_base;
    unsigned seen_gen;

    GtkEntry *reg[R_COUNT];
    GtkWidget *flag[NFLAGS];
    GtkWidget *pill_illegal;
    GtkLabel *info, *a_note;

    GtkWidget *grid;
    int sel;                    /* the selected register, or -1 */
    GtkLabel *cell_name;
    GtkEntry *cell_val;
    GtkLabel *kstack;

    GtkDropDown *mem_space;
    GtkEntry *mem_addr, *poke_addr, *poke_val;
    GtkTextView *mem_text;
    uint32_t mem_base;
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
    DbgCpu *c = ud;
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

static void cell_select(DbgCpu *c, int r)
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
    DbgCpu *c = ud;
    const int col = (int)((x - HEAD_W) / CELL_W), row = (int)((y - HEAD_H) / CELL_H);
    (void)g; (void)n;
    if (x < HEAD_W || y < HEAD_H || col < 0 || col > 7 || row < 0 || row > 7)
        return;
    cell_select(c, row * 8 + col);
}

static void on_cell_val(GtkEntry *e, gpointer ud)
{
    DbgCpu *c = ud;
    long v;
    (void)e;
    if (c->sel >= 0 && dbg_parse_num(gtk_editable_get_text(GTK_EDITABLE(c->cell_val)), &v)) {
        chfdebug_cpu_set(c->dbg, CHF_REG_R0 + c->sel, (int)(v & 0xFF));
        if (c->changed) c->changed(c->ud);
    }
}

/* ---- the registers ------------------------------------------------------------ */

static void on_reg_activate(GtkEntry *entry, gpointer ud)
{
    DbgCpu *c = ud;
    const int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(entry), "reg"));
    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    long v;

    if (i == R_ISAR) {
        /* octal, "u.l" or "ul" */
        char digits[8];
        int n = 0;
        const char *p;
        for (p = text; *p && n < 6; p++)
            if (*p >= '0' && *p <= '7') digits[n++] = *p;
        digits[n] = '\0';
        if (n > 0)
            chfdebug_cpu_set(c->dbg, CHF_REG_ISAR, (int)(strtol(digits, NULL, 8) & 0x3F));
    } else if (k_regs[i].reg >= 0 && dbg_parse_num(text, &v)) {
        chfdebug_cpu_set(c->dbg, k_regs[i].reg, (int)v);
    }
    if (c->changed) c->changed(c->ud);
}

static void on_flag_toggled(GtkCheckButton *b, gpointer ud)
{
    DbgCpu *c = ud;
    const int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "flag"));
    if (c->syncing) return;
    chfdebug_cpu_set(c->dbg, k_flags[i].reg, gtk_check_button_get_active(b) ? 1 : 0);
    if (c->changed) c->changed(c->ud);
}

static GtkWidget *reg_entry(DbgCpu *c, int i)
{
    c->reg[i] = GTK_ENTRY(dbg_hex_entry(k_regs[i].digits));
    g_object_set_data(G_OBJECT(c->reg[i]), "reg", GINT_TO_POINTER(i));
    if (k_regs[i].reg >= 0)
        g_signal_connect(c->reg[i], "activate", G_CALLBACK(on_reg_activate), c);
    else
        gtk_editable_set_editable(GTK_EDITABLE(c->reg[i]), FALSE);
    if (i == R_ISAR)
        gtk_widget_set_tooltip_text(GTK_WIDGET(c->reg[i]),
            "The scratchpad pointer, in octal as upper.lower (6 bits)");
    return GTK_WIDGET(c->reg[i]);
}

/* ---- the memory viewer ---------------------------------------------------------- */

static void refresh_mem(DbgCpu *c)
{
    static char text[MEM_ROWS * 110];
    uint8_t bytes[16];
    const int space = (int)gtk_drop_down_get_selected(c->mem_space);
    const uint32_t size = (uint32_t)chfdebug_space_size(c->dbg, space);
    const int wide = size > 0x10000 ? 5 : size > 0x100 ? 4 : 2;
    int r, len = 0;

    for (r = 0; r < MEM_ROWS; r++) {
        const uint32_t a = c->mem_base + (uint32_t)r * 16;
        int i;
        len += g_snprintf(text + len, sizeof text - len, "%0*X:", wide, a);
        chfdebug_read(c->dbg, space, a, bytes, 16);
        for (i = 0; i < 16; i++)
            len += g_snprintf(text + len, sizeof text - len, a + (uint32_t)i < size
                              ? " %02X" : " --", bytes[i]);
        len += g_snprintf(text + len, sizeof text - len, "  ");
        for (i = 0; i < 16; i++)
            text[len++] = bytes[i] >= 0x20 && bytes[i] < 0x7F ? (char)bytes[i] : '.';
        if (space == CHF_SPACE_VRAM)        /* a line is 16 columns of one row */
            len += g_snprintf(text + len, sizeof text - len, "  row %d col %d-%d",
                              (int)(a / CHFDEBUG_VRAM_COLS), (int)(a % CHFDEBUG_VRAM_COLS),
                              (int)(a % CHFDEBUG_VRAM_COLS) + 15);
        else if (space == CHF_SPACE_SCRATCH)
            len += g_snprintf(text + len, sizeof text - len, "  r%d-r%d", (int)a, (int)a + 15);
        text[len++] = '\n';
        text[len] = '\0';
    }
    dbg_set_text(c->mem_text, text);
}

static int parse_label_or_num(DbgCpu *c, const char *text, long *out)
{
    int a = chfdebug_label_address(c->dbg, text);
    if (a >= 0) { *out = a; return 1; }
    return dbg_parse_num(text, out);
}

/* The viewer's address box and space: show from there (16-byte aligned). */
static void on_mem_go(GtkWidget *wgt, gpointer ud)
{
    DbgCpu *c = ud;
    long a;
    (void)wgt;
    if (parse_label_or_num(c, gtk_editable_get_text(GTK_EDITABLE(c->mem_addr)), &a))
        c->mem_base = (uint32_t)a & ~0xFu;
    refresh_mem(c);
}

static void on_mem_space(GObject *o, GParamSpec *ps, gpointer ud)
{
    DbgCpu *c = ud;
    (void)o; (void)ps;
    c->mem_base = 0;
    gtk_editable_set_text(GTK_EDITABLE(c->mem_addr), "0");
    refresh_mem(c);
}

/* A byte poked into the viewer's space (the bus takes it with its write
 * effects; ROM and the BIOS ignore it). */
static void on_poke(GtkWidget *wgt, gpointer ud)
{
    DbgCpu *c = ud;
    long a, v;
    (void)wgt;
    if (!parse_label_or_num(c, gtk_editable_get_text(GTK_EDITABLE(c->poke_addr)), &a) ||
        !dbg_parse_num(gtk_editable_get_text(GTK_EDITABLE(c->poke_val)), &v))
        return;
    chfdebug_write(c->dbg, (int)gtk_drop_down_get_selected(c->mem_space),
                   (uint32_t)a, (uint8_t)v);
    if (c->changed) c->changed(c->ud);
}

/* ---- refresh -------------------------------------------------------------------- */

void dbg_cpu_refresh(DbgCpu *c, gboolean live)
{
    chfdebug_cpu cpu;
    char buf[400];
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

    {
        char v[R_COUNT][12];
        if (cpu.pc >= 0) g_snprintf(v[R_PC], sizeof v[0], "%04X", cpu.pc);
        else g_strlcpy(v[R_PC], "----", sizeof v[0]);
        g_snprintf(v[R_PC0], sizeof v[0], "%04X", cpu.pc0);
        g_snprintf(v[R_PC1], sizeof v[0], "%04X", cpu.pc1);
        g_snprintf(v[R_DC0], sizeof v[0], "%04X", cpu.dc0);
        g_snprintf(v[R_DC1], sizeof v[0], "%04X", cpu.dc1);
        g_snprintf(v[R_DBUS], sizeof v[0], "%02X", cpu.dbus);
        g_snprintf(v[R_A], sizeof v[0], "%02X", cpu.a);
        g_snprintf(v[R_W], sizeof v[0], "%02X", cpu.w);
        g_snprintf(v[R_ISAR], sizeof v[0], "%o.%o", (cpu.isar >> 3) & 7, cpu.isar & 7);
        g_snprintf(v[R_J], sizeof v[0], "%02X", cpu.j_h_k_q[0] & 0xFF);
        g_snprintf(v[R_H], sizeof v[0], "%04X", cpu.j_h_k_q[1]);
        g_snprintf(v[R_K], sizeof v[0], "%04X", cpu.j_h_k_q[2]);
        g_snprintf(v[R_Q], sizeof v[0], "%04X", cpu.j_h_k_q[3]);
        for (i = 0; i < R_COUNT; i++)
            dbg_set_entry(c->dbg, c->reg[i], v[i]);
    }
    c->syncing = TRUE;
    {
        const int f[NFLAGS] = { cpu.sign, cpu.carry, cpu.zero, cpu.overflow, cpu.icb };
        for (i = 0; i < NFLAGS; i++)
            gtk_check_button_set_active(GTK_CHECK_BUTTON(c->flag[i]), f[i] != 0);
    }
    c->syncing = FALSE;

    g_snprintf(buf, sizeof buf, "%%%c%c%c%c%c%c%c%c   %u",
               cpu.a & 0x80 ? '1' : '0', cpu.a & 0x40 ? '1' : '0', cpu.a & 0x20 ? '1' : '0',
               cpu.a & 0x10 ? '1' : '0', cpu.a & 8 ? '1' : '0', cpu.a & 4 ? '1' : '0',
               cpu.a & 2 ? '1' : '0', cpu.a & 1 ? '1' : '0', cpu.a);
    gtk_label_set_text(c->a_note, buf);

    gtk_widget_set_visible(c->pill_illegal, cpu.illegal_count != 0);
    dbg_pill(c->pill_illegal, TRUE);
    g_snprintf(buf, sizeof buf,
               "%s   BIOS %s   line %d  cycle %d   frame %u   clocks %llu   instructions %llu%s",
               chf_machine_name(cpu.machine),
               cpu.bios == CHF_ROM_SL31253 ? "sl31253" : cpu.bios == CHF_ROM_SL90025 ? "sl90025"
               : "stand-in", cpu.vpos, cpu.hpos, cpu.frame,
               (unsigned long long)cpu.cycles, (unsigned long long)cpu.instructions,
               cpu.illegal_count ? "   illegal opcodes " : "");
    if (cpu.illegal_count) {
        char at[40];
        g_snprintf(at, sizeof at, "%u (last at $%04X)", cpu.illegal_count, cpu.illegal_pc & 0xFFFF);
        g_strlcat(buf, at, sizeof buf);
    }
    gtk_label_set_text(c->info, buf);

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
    refresh_mem(c);
}

/* ---- construction --------------------------------------------------------------- */

static GtkWidget *build_registers(DbgCpu *c)
{
    GtkWidget *regs = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *row1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *row2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *row3 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *flags = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    int i;

    gtk_box_append(GTK_BOX(regs), dbg_heading("Registers"));
    for (i = R_PC; i <= R_DBUS; i++)
        gtk_box_append(GTK_BOX(row1), dbg_labeled(k_regs[i].name, reg_entry(c, i)));
    gtk_box_append(GTK_BOX(regs), row1);

    gtk_box_append(GTK_BOX(row2), dbg_labeled("A", reg_entry(c, R_A)));
    c->a_note = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(c->a_note), "monospace");
    gtk_widget_add_css_class(GTK_WIDGET(c->a_note), "dim-label");
    gtk_label_set_width_chars(c->a_note, 17);
    gtk_label_set_xalign(c->a_note, 0);
    gtk_box_append(GTK_BOX(row2), GTK_WIDGET(c->a_note));
    gtk_box_append(GTK_BOX(row2), dbg_labeled("ISAR", reg_entry(c, R_ISAR)));
    gtk_box_append(GTK_BOX(regs), row2);

    gtk_box_append(GTK_BOX(flags), dbg_labeled("W", reg_entry(c, R_W)));
    for (i = 0; i < NFLAGS; i++) {
        c->flag[i] = gtk_check_button_new_with_label(k_flags[i].name);
        gtk_widget_set_tooltip_text(c->flag[i], k_flags[i].tip);
        g_object_set_data(G_OBJECT(c->flag[i]), "flag", GINT_TO_POINTER(i));
        g_signal_connect(c->flag[i], "toggled", G_CALLBACK(on_flag_toggled), c);
        gtk_box_append(GTK_BOX(flags), c->flag[i]);
    }
    gtk_box_append(GTK_BOX(regs), flags);

    for (i = R_J; i <= R_Q; i++)
        gtk_box_append(GTK_BOX(row3), dbg_labeled(k_regs[i].name, reg_entry(c, i)));
    gtk_box_append(GTK_BOX(regs), row3);

    c->pill_illegal = gtk_label_new("ILLEGAL");
    gtk_widget_add_css_class(c->pill_illegal, "chf-pill");
    gtk_widget_set_halign(c->pill_illegal, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text(c->pill_illegal, "An illegal opcode has been executed");
    dbg_pill(c->pill_illegal, FALSE);
    gtk_box_append(GTK_BOX(regs), c->pill_illegal);

    c->info = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(c->info), "dim-label");
    gtk_label_set_xalign(c->info, 0);
    gtk_label_set_wrap(c->info, TRUE);
    gtk_label_set_width_chars(c->info, 44);
    gtk_label_set_max_width_chars(c->info, 60);
    gtk_box_append(GTK_BOX(regs), GTK_WIDGET(c->info));
    gtk_box_append(GTK_BOX(regs),
        dbg_hint("PC is the instruction about to run (PC0 is one past it: the CPU has "
                 "fetched its opcode). W = ICB O Z C S, S set meaning positive. Enter in a "
                 "field writes it while the machine is stopped.", 44));
    return regs;
}

static GtkWidget *build_scratch(DbgCpu *c)
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

static GtkWidget *build_kstack(DbgCpu *c)
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

DbgCpu *dbg_cpu_new(chfsession *session, chfdebug *dbg, DbgChanged changed, gpointer ud,
                    GtkWidget **root)
{
    DbgCpu *c = g_new0(DbgCpu, 1);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 28);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    GtkWidget *memtools = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *poketools = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *go, *poke, *memscroll, *scroll;
    static const char *const spaces[] = { "PROG (CPU view)", "Scratchpad", "VRAM",
                                          "Cartridge", "I/O ports", NULL };

    c->session = session;
    c->dbg = dbg;
    c->changed = changed;
    c->ud = ud;
    c->sel = -1;

    gtk_box_append(GTK_BOX(left), build_registers(c));
    gtk_box_append(GTK_BOX(left), build_kstack(c));
    gtk_box_append(GTK_BOX(top), left);
    gtk_box_append(GTK_BOX(top), build_scratch(c));

    c->mem_space = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(spaces));
    g_signal_connect(c->mem_space, "notify::selected", G_CALLBACK(on_mem_space), c);
    c->mem_addr = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(c->mem_addr, "$0800 or label");
    gtk_editable_set_width_chars(GTK_EDITABLE(c->mem_addr), 14);
    g_signal_connect(c->mem_addr, "activate", G_CALLBACK(on_mem_go), c);
    go = gtk_button_new_with_label("Go");
    g_signal_connect(go, "clicked", G_CALLBACK(on_mem_go), c);
    gtk_box_append(GTK_BOX(memtools), dbg_heading("Memory"));
    gtk_box_append(GTK_BOX(memtools), GTK_WIDGET(c->mem_space));
    gtk_box_append(GTK_BOX(memtools), dbg_labeled("From", GTK_WIDGET(c->mem_addr)));
    gtk_box_append(GTK_BOX(memtools), go);

    c->poke_addr = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(c->poke_addr, "address");
    gtk_editable_set_width_chars(GTK_EDITABLE(c->poke_addr), 10);
    c->poke_val = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(c->poke_val, "value");
    gtk_editable_set_width_chars(GTK_EDITABLE(c->poke_val), 6);
    g_signal_connect(c->poke_val, "activate", G_CALLBACK(on_poke), c);
    poke = gtk_button_new_with_label("Poke");
    g_signal_connect(poke, "clicked", G_CALLBACK(on_poke), c);
    gtk_box_append(GTK_BOX(poketools), dbg_labeled("Poke", GTK_WIDGET(c->poke_addr)));
    gtk_box_append(GTK_BOX(poketools), GTK_WIDGET(c->poke_val));
    gtk_box_append(GTK_BOX(poketools), poke);
    gtk_box_append(GTK_BOX(poketools),
        dbg_hint("Into the chosen space: the cartridge's RAM, a VRAM cell (0-3), a register, "
                 "a port's latch. ROM and the BIOS ignore it.", 40));
    memscroll = dbg_mono_view(&c->mem_text, FALSE);
    gtk_widget_set_size_request(memscroll, -1, 170);

    gtk_box_append(GTK_BOX(box), top);
    gtk_box_append(GTK_BOX(box), memtools);
    gtk_box_append(GTK_BOX(box), memscroll);
    gtk_box_append(GTK_BOX(box), poketools);

    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), dbg_padded(box));
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    *root = scroll;
    return c;
}

void dbg_cpu_free(DbgCpu *c)
{
    g_free(c);
}
