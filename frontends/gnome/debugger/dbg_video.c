/*
 * The debugger's Video tab: the 2637 UVI.
 *
 * The tab shows everything the chip knows, from one snapshot
 * (chfdebug_uvi_get) per refresh:
 *
 *   left   the picture at 2x, nearest neighbour: the frame the machine
 *          finished, the CURRENT registers re-rendered with chosen layers and
 *          overlays, or the frame as far as the beam has got; the objects
 *          boxed (by the core) and indexed (here); with the pointer over it,
 *          what draws there (chfdebug_uvi_hit_test): the object or the
 *          character cell, named and outlined;
 *   right  five sections: Objects (the four, each with its position, size,
 *          colour, UDC, bitmap and what it has hit), Screen (the 16 x 13/26
 *          character screen with its colour bits and block-graphics cells,
 *          drawn from the chip's glyph table), Registers (the ones that are
 *          not plain RAM: $18F0-$18FF, the keypad inputs, $19F8-$19FF,
 *          decoded and editable), Control (the screen geometry, colours,
 *          collision flags and the beam) and Sheets (the glyph table with the
 *          UDCs, the objects' shapes, the palette).
 *
 * Idle cost: the caller refreshes this tab only while it shows, and only the
 * section that shows is redrawn. The picture is rendered by the core into one
 * reused buffer and a GPU texture is built only when its pixels changed (a
 * stopped machine costs nothing, a running one at most ten uploads a
 * second); an object's bitmap is fetched only when its registers changed.
 *
 * The picture is shown with SQUARE pixels (192 x 262 at 2x): the debugger
 * wants every pixel of the frame exactly, not the television's 4:3.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_common.h"

#include <math.h>
#include <string.h>

#define FRAME_W CHFSESSION_FB_WIDTH
#define FRAME_H CHFSESSION_FB_HEIGHT
#define SCALE 2                 /* the picture's pixels: nearest, as the core scales */
#define UVI_XPOS 32             /* the playfield's left edge in the frame */

enum { VIEW_FRAME, VIEW_RENDER, VIEW_PROGRESS };
enum { SEC_OBJECTS, SEC_SCREEN, SEC_REGS, SEC_CONTROL, SEC_SHEETS, SEC_COUNT };

#define NLAYERS 2
#define NFLAGS 4
/* the checkboxes' order and what each means to the core */
static const struct { const char *name; unsigned bit; } k_layers[NLAYERS] = {
    { "Characters", CHFDEBUG_UVI_L_CHARS }, { "Objects", CHFDEBUG_UVI_L_OBJECTS },
};
static const struct { const char *name; unsigned bit; } k_flags[NFLAGS] = {
    { "Object boxes", CHFDEBUG_UVI_F_BOXES }, { "Character grid", CHFDEBUG_UVI_F_GRID },
    { "Collision highlight", CHFDEBUG_UVI_F_COLLIDE }, { "Beam cross-hair", CHFDEBUG_UVI_F_BEAM },
};
/* the boxes' colours (red, green, blue, orange), as the core draws them */
static const guint32 k_obj_colour[4] = { 0xFF4040, 0x40E040, 0x5080FF, 0xFFA030 };

/* The registers that are not plain RAM, as offsets into the register file
 * ($1800 + offset): $18F0-$18FF, $1900-$1908, $19F8-$19FF. */
static const struct { unsigned first, last; } k_reg_ranges[] = {
    { 0x0F0, 0x0FF }, { 0x100, 0x108 }, { 0x1F8, 0x1FF },
};
#define NREGROWS (16 + 9 + 8)

/* An object's row in the tables: its bitmap and its text, and the registers
 * the bitmap was last fetched for. */
typedef struct {
    GtkPicture *pic;
    GtkWidget *label;
    uint8_t sig[16];
    gboolean have_sig;
} ObjRow;

/* A fixed-size image drawn with cairo (image_area_new below). */
typedef struct {
    GtkWidget *widget;
    uint32_t *px;
    int w, h;
} ImageArea;

struct DbgVideo {
    chfsession *session;
    chfdebug *dbg;
    chfdebug_uvi snap;
    chfdebug_uvi_cell cells[CHFDEBUG_UVI_COLS * CHFDEBUG_UVI_MAX_ROWS];
    int nrows;                  /* of the character screen: 13 or 26 */
    int tick;                   /* live refreshes since the sections last redrew */

    /* the picture */
    GtkDropDown *view;
    GtkCheckButton *layer[NLAYERS], *flag[NFLAGS];
    GtkPicture *pic;
    GtkWidget *overlay_area;
    GtkLabel *hover_label;
    uint32_t *buf, *prev;       /* the rendered view, and the one on the GPU */
    size_t cap;                 /* pixels each holds */
    int pw, ph;
    int mx, my;                 /* the pointer, in frame pixels */
    gboolean over;
    chfdebug_hit hit[4];
    int nhit;

    GtkNotebook *sections;

    /* objects */
    ObjRow obj[4];

    /* screen */
    GtkWidget *screen_area;
    GtkLabel *screen_text;
    uint32_t palette[8];
    gboolean have_palette;
    int screen_sel;             /* the cell last clicked / hovered, -1 none */

    /* registers */
    GtkLabel *reg_name[NREGROWS], *reg_text[NREGROWS];
    GtkEntry *reg_val[NREGROWS];
    unsigned reg_off[NREGROWS];
    uint8_t reg_shown[NREGROWS];
    gboolean reg_built;

    /* control */
    GtkLabel *control_text;

    /* sheets */
    ImageArea *sheet_font, *sheet_objects, *sheet_palette;
};

/* ---- images ------------------------------------------------------------------- */

/* ---- images ------------------------------------------------------------------- */

static void set_picture(GtkPicture *pic, const uint32_t *px, int w, int h)
{
    GBytes *bytes = g_bytes_new(px, (gsize)w * (gsize)h * 4);
    GdkTexture *tex = gdk_memory_texture_new(w, h, GDK_MEMORY_B8G8R8X8, bytes, (gsize)w * 4);
    gtk_picture_set_paintable(pic, GDK_PAINTABLE(tex));
    gtk_widget_set_size_request(GTK_WIDGET(pic), w, h);
    g_object_unref(tex);
    g_bytes_unref(bytes);
}

/* A fixed-size image drawn with cairo, for the pictures whose size varies:
 * a GtkPicture asked for its height at a width answers with the aspect
 * ratio's, which stretches it when its column is wider than the image. A
 * drawing area is exactly the size it is given. */

static void image_area_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer ud)
{
    ImageArea *ia = ud;
    cairo_surface_t *surf;
    (void)area; (void)width; (void)height;
    if (!ia->px) return;
    surf = cairo_image_surface_create_for_data((unsigned char *)ia->px, CAIRO_FORMAT_RGB24,
                                               ia->w, ia->h, ia->w * 4);
    cairo_set_source_surface(cr, surf, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
    cairo_surface_destroy(surf);
}

static void image_area_free(gpointer p)
{
    ImageArea *ia = p;
    g_free(ia->px);
    g_free(ia);
}

static ImageArea *image_area_new(void)
{
    ImageArea *ia = g_new0(ImageArea, 1);
    ia->widget = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(ia->widget), image_area_draw, ia, NULL);
    gtk_widget_set_halign(ia->widget, GTK_ALIGN_START);
    gtk_widget_set_valign(ia->widget, GTK_ALIGN_START);
    g_object_set_data_full(G_OBJECT(ia->widget), "image-area", ia, image_area_free);
    return ia;
}

static void image_area_set(ImageArea *ia, const uint32_t *px, int w, int h)
{
    if (ia->w != w || ia->h != h) {
        g_free(ia->px);
        ia->px = g_new(uint32_t, (gsize)w * (gsize)h);
        ia->w = w;
        ia->h = h;
        gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(ia->widget), w);
        gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(ia->widget), h);
    }
    memcpy(ia->px, px, (gsize)w * (gsize)h * 4);
    gtk_widget_queue_draw(ia->widget);
}

static GtkPicture *new_picture(void)
{
    GtkPicture *pic = GTK_PICTURE(gtk_picture_new());
    gtk_picture_set_can_shrink(pic, FALSE);
    gtk_widget_set_halign(GTK_WIDGET(pic), GTK_ALIGN_START);
    gtk_widget_set_valign(GTK_WIDGET(pic), GTK_ALIGN_START);
    return pic;
}

/* The core's view `kind` into the shared buffer, and onto `pic` when its
 * pixels differ from what `prev` (if any) last showed. */
static gboolean show_view(DbgVideo *v, GtkPicture *pic, ImageArea *area, int kind,
                          unsigned layers, unsigned flags, int scale, gboolean cache)
{
    int w = 0, h = 0;
    if (!chfdebug_uvi_view(v->dbg, kind, layers, flags, scale, v->buf, v->cap, &w, &h) ||
        w <= 0 || h <= 0 || (size_t)w * (size_t)h > v->cap)
        return FALSE;
    if (cache) {
        if (v->pw == w && v->ph == h &&
            memcmp(v->buf, v->prev, (size_t)w * (size_t)h * 4) == 0)
            return FALSE;
        memcpy(v->prev, v->buf, (size_t)w * (size_t)h * 4);
        v->pw = w;
        v->ph = h;
    }
    if (pic)
        set_picture(pic, v->buf, w, h);
    else
        image_area_set(area, v->buf, w, h);
    return TRUE;
}

/* The eight colours, sampled from the palette sheet (one cell each in a row). */
static void fetch_palette(DbgVideo *v)
{
    int w = 0, h = 0, cell, k;
    if (v->have_palette) return;
    if (!chfdebug_uvi_view(v->dbg, CHFDEBUG_UVI_PALETTE, 0, 0, 1, v->buf, v->cap, &w, &h) ||
        w < 8 || h < 1)
        return;
    cell = w / 8;
    for (k = 0; k < 8; k++)
        v->palette[k] = v->buf[(size_t)(h / 2) * w + k * cell + cell / 2] & 0xFFFFFF;
    v->have_palette = TRUE;
}

/* ---- the picture ----------------------------------------------------------------- */

static unsigned layer_mask(DbgVideo *v)
{
    unsigned m = 0;
    int i;
    for (i = 0; i < NLAYERS; i++)
        if (gtk_check_button_get_active(v->layer[i]))
            m |= k_layers[i].bit;
    return m;
}

static unsigned flag_mask(DbgVideo *v)
{
    unsigned m = 0;
    int i;
    for (i = 0; i < NFLAGS; i++)
        if (gtk_check_button_get_active(v->flag[i]))
            m |= k_flags[i].bit;
    return m;
}

static int view_now(DbgVideo *v)
{
    guint sel = gtk_drop_down_get_selected(v->view);
    return sel == VIEW_RENDER || sel == VIEW_PROGRESS ? (int)sel : VIEW_FRAME;
}

/* Only the re-render takes the layers; the overlays apply to all three. */
static void sync_sensitivity(DbgVideo *v)
{
    const gboolean render = view_now(v) == VIEW_RENDER;
    int i;
    for (i = 0; i < NLAYERS; i++)
        gtk_widget_set_sensitive(GTK_WIDGET(v->layer[i]), render);
}

static void picture_update(DbgVideo *v)
{
    const int view = view_now(v);
    const int core_kind = view == VIEW_RENDER ? CHFDEBUG_UVI_RENDER
                        : view == VIEW_PROGRESS ? CHFDEBUG_UVI_PROGRESS : CHFDEBUG_UVI_FRAME;

    if (show_view(v, v->pic, NULL, core_kind, layer_mask(v), flag_mask(v), SCALE, TRUE) ||
        (flag_mask(v) & CHFDEBUG_UVI_F_BOXES))
        gtk_widget_queue_draw(v->overlay_area);
}

/* The rectangle of a character cell, in frame pixels. */
static void cell_rect(const DbgVideo *v, int index, int *x, int *y, int *w, int *h)
{
    const int row = index / CHFDEBUG_UVI_COLS, col = index % CHFDEBUG_UVI_COLS;
    *x = UVI_XPOS + v->snap.shift + col * 8;
    *y = v->snap.first_line + row * v->snap.cell_height;
    *w = 8;
    *h = v->snap.cell_height;
}

static void draw_overlay(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer ud)
{
    DbgVideo *v = ud;
    GtkWidget *wg = GTK_WIDGET(area);
    int i;
    (void)width; (void)height;

    /* the core boxes the objects; the index goes on the corner of each box */
    if (flag_mask(v) & CHFDEBUG_UVI_F_BOXES) {
        for (i = 0; i < 4; i++) {
            const chfdebug_uvi_object *o = &v->snap.obj[i];
            char t[4];
            if (!o->visible) continue;
            g_snprintf(t, sizeof t, "%d", i);
            dbg_set_rgb(cr, 0x000000, 0.65);
            cairo_rectangle(cr, o->px * SCALE, o->py * SCALE - 13, 12, 12);
            cairo_fill(cr);
            dbg_set_rgb(cr, k_obj_colour[i], 1.0);
            dbg_draw_centred(cr, wg, t, o->px * SCALE, o->py * SCALE - 13, 12, 12, TRUE);
        }
    }
    if (v->over && v->nhit > 0) {
        int x = 0, y = 0, w = 0, h = 0;
        const int k = v->hit[0].kind, idx = v->hit[0].index;
        if (k == CHFDEBUG_UVI_HIT_OBJECT && idx >= 0 && idx < 4) {
            x = v->snap.obj[idx].px; y = v->snap.obj[idx].py;
            w = v->snap.obj[idx].width; h = v->snap.obj[idx].height;
        } else if (k == CHFDEBUG_UVI_HIT_CELL) {
            cell_rect(v, idx, &x, &y, &w, &h);
        } else {
            return;
        }
        dbg_set_rgb(cr, CHFSESSION_ACCENT_RGB, 0.25);
        cairo_rectangle(cr, x * SCALE, y * SCALE, w * SCALE, h * SCALE);
        cairo_fill_preserve(cr);
        dbg_set_rgb(cr, dbg_accent_line_rgb(), 1.0);
        cairo_set_line_width(cr, 2);
        cairo_stroke(cr);
    }
}

/* One cell, described: "Cell 4,7: code $2A glyph $2A fg cyan bg black". */
static void describe_cell(const DbgVideo *v, int idx, char *dst, int n)
{
    const chfdebug_uvi_cell *c = &v->cells[idx];
    g_snprintf(dst, (gsize)n, "Cell %d,%d: code $%02X glyph $%02X %s  fg %s  bg %s",
               idx / CHFDEBUG_UVI_COLS, idx % CHFDEBUG_UVI_COLS, c->code, c->glyph,
               c->graphics ? "(block graphics)" : c->glyph >= 0x38 ? "(UDC)" : "",
               chfdebug_uvi_colour_name(c->fg), chfdebug_uvi_colour_name(c->bg));
}

/* "Object 1: x=.. y=..", for what is under the pointer. */
static void describe_hit(DbgVideo *v, char *dst, int n)
{
    char more[40] = "";
    if (!v->over) {
        g_snprintf(dst, (gsize)n, "Move the pointer over the picture to see what draws there");
        return;
    }
    if (v->nhit <= 0) {
        g_snprintf(dst, (gsize)n, "Pointer x=%d y=%d: nothing but the background", v->mx, v->my);
        return;
    }
    if (v->nhit > 1)
        g_snprintf(more, sizeof more, "   (+%d under it)", v->nhit - 1);
    if (v->hit[0].kind == CHFDEBUG_UVI_HIT_OBJECT) {
        const chfdebug_uvi_object *o = &v->snap.obj[v->hit[0].index & 3];
        g_snprintf(dst, (gsize)n, "Object %d: x=%d y=%d colour=%s %s UDC $%02X   pointer %d,%d%s",
                   o->index, o->x, o->y, chfdebug_uvi_colour_name(o->colour),
                   o->double_height ? "8x16" : "8x8", o->udc, v->mx, v->my, more);
    } else {
        char cell[160];
        describe_cell(v, v->hit[0].index, cell, sizeof cell);
        g_snprintf(dst, (gsize)n, "%s   pointer %d,%d%s", cell, v->mx, v->my, more);
    }
}

static void hover_update(DbgVideo *v, double x, double y, gboolean over)
{
    char text[260];
    chfdebug_hit old = v->hit[0];
    const int oldn = v->nhit, oldx = v->mx, oldy = v->my;
    const gboolean was = v->over;

    v->over = over;
    if (over) {
        v->mx = CLAMP((int)x / SCALE, 0, FRAME_W - 1);
        v->my = CLAMP((int)y / SCALE, 0, FRAME_H - 1);
        if (was && v->mx == oldx && v->my == oldy)
            return;
        chfdebug_uvi_get(v->dbg, &v->snap);
        v->nrows = chfdebug_uvi_cells(v->dbg, v->cells);
        v->nhit = chfdebug_uvi_hit_test(v->dbg, v->mx, v->my, v->hit, 4);
        if (v->nhit < 0) v->nhit = 0;
    } else {
        v->nhit = 0;
    }
    describe_hit(v, text, sizeof text);
    gtk_label_set_text(v->hover_label, text);
    if (oldn != v->nhit || (v->nhit > 0 && (old.kind != v->hit[0].kind || old.index != v->hit[0].index)) ||
        was != over)
        gtk_widget_queue_draw(v->overlay_area);
}

static void on_motion(GtkEventControllerMotion *c, double x, double y, gpointer ud)
{
    (void)c;
    hover_update(ud, x, y, TRUE);
}

static void on_leave(GtkEventControllerMotion *c, gpointer ud)
{
    (void)c;
    hover_update(ud, 0, 0, FALSE);
}

static void on_view_changed(GObject *o, GParamSpec *ps, gpointer ud)
{
    DbgVideo *v = ud;
    (void)o; (void)ps;
    sync_sensitivity(v);
    v->pw = 0;                  /* a different picture: not comparable */
    picture_update(v);
}

static void on_option_toggled(GtkCheckButton *b, gpointer ud)
{
    (void)b;
    picture_update(ud);
}

/* ---- objects ------------------------------------------------------------------------ */

#define OBJ_BOX 72              /* the cell an object's bitmap sits in */
#define OBJ_SCALE 4             /* one shape pixel as 4 x 4 */

static ObjRow *obj_row_new(GtkWidget *parent, ObjRow *r)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *cell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    r->pic = new_picture();
    gtk_widget_set_halign(GTK_WIDGET(r->pic), GTK_ALIGN_CENTER);
    gtk_widget_set_valign(GTK_WIDGET(r->pic), GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(cell, OBJ_BOX, OBJ_BOX);
    gtk_widget_set_valign(cell, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(cell), GTK_WIDGET(r->pic));
    r->label = dbg_mono_label();
    gtk_label_set_selectable(GTK_LABEL(r->label), FALSE);
    gtk_label_set_wrap(GTK_LABEL(r->label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(r->label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_width_chars(GTK_LABEL(r->label), 60);
    gtk_label_set_max_width_chars(GTK_LABEL(r->label), 70);
    gtk_box_append(GTK_BOX(row), cell);
    gtk_box_append(GTK_BOX(row), r->label);
    gtk_box_append(GTK_BOX(parent), row);
    return r;
}

/* One object's bitmap, fetched only when the registers it draws from
 * changed. */
static void obj_bitmap(DbgVideo *v, ObjRow *r, int index, const uint8_t *sig, int nsig)
{
    int w = 0, h = 0;
    uint8_t cur[16] = { 0 };
    memcpy(cur, sig, (size_t)nsig);
    if (r->have_sig && memcmp(cur, r->sig, sizeof cur) == 0)
        return;
    if (!chfdebug_uvi_thing(v->dbg, CHFDEBUG_UVI_OBJECT, index, OBJ_SCALE, 0x00202020, v->buf,
                            v->cap, &w, &h) || w <= 0 || h <= 0)
        return;
    set_picture(r->pic, v->buf, w, h);
    memcpy(r->sig, cur, sizeof cur);
    r->have_sig = TRUE;
}

static void refresh_objects(DbgVideo *v)
{
    const chfdebug_uvi *s = &v->snap;
    char text[400];
    int i;

    for (i = 0; i < 4; i++) {
        const chfdebug_uvi_object *o = &s->obj[i];
        char hits[80] = "";
        int k;
        if (o->hits_chars)
            g_strlcat(hits, "  hit characters", sizeof hits);
        for (k = 0; k < 4; k++)
            if ((o->hits_objects >> k) & 1) {
                char b[24];
                g_snprintf(b, sizeof b, "  hit obj %d", k);
                g_strlcat(hits, b, sizeof hits);
            }
        g_snprintf(text, sizeof text,
            "OBJ%d  x=%-4d y=%-4d  %-7s  %s  UDC $%02X  %s%s\n"
            "      regs X $%02X Y $%02X   shape %02X %02X %02X %02X %02X %02X %02X %02X",
            i, o->x, o->y, chfdebug_uvi_colour_name(o->colour),
            o->double_height ? "8x16" : "8x8 ", o->udc,
            o->visible ? "visible" : "off screen", hits,
            o->reg_x, o->reg_y, o->shape[0], o->shape[1], o->shape[2], o->shape[3],
            o->shape[4], o->shape[5], o->shape[6], o->shape[7]);
        gtk_label_set_text(GTK_LABEL(v->obj[i].label), text);
        {
            uint8_t sig[12] = { o->reg_x, o->reg_y, (uint8_t)o->colour,
                                (uint8_t)o->double_height };
            memcpy(sig + 4, o->shape, 8);
            obj_bitmap(v, &v->obj[i], i, sig, sizeof sig);
        }
    }
}

static GtkWidget *build_objects(DbgVideo *v)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    int i;

    gtk_box_append(GTK_BOX(box), dbg_heading("The four objects (8 x 8, or 8 x 16 with the size flag)"));
    for (i = 0; i < 4; i++)
        obj_row_new(box, &v->obj[i]);
    gtk_box_append(GTK_BOX(box), dbg_hint("Each object is drawn from the user-defined character "
                                          "$38 + its index ($1980-$19BF); the outline colours in "
                                          "the picture are red, green, blue and orange for 0-3.", 44));
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    return scroll;
}

/* ---- the character screen ----------------------------------------------------------------- */

#define SCR_CELL 24             /* one character cell, 3 x the glyph's pixels */
#define SCR_PAD 22

/* The block-graphics shapes (the chip's "rectangle" table): each of the six
 * bits lights one 2 x 3 pixel-wide block, bits 0-2 across the top four rows,
 * bits 3-5 across the bottom four. */
static void rectangle_shape(unsigned code, uint8_t out[8])
{
    uint8_t top = 0, bot = 0;
    int i;
    if (code & 1)    top |= 0x03;
    if (code & 2)    top |= 0x1c;
    if (code & 4)    top |= 0xe0;
    if (code & 8)    bot |= 0x03;
    if (code & 0x10) bot |= 0x1c;
    if (code & 0x20) bot |= 0xe0;
    for (i = 0; i < 4; i++) { out[i] = top; out[4 + i] = bot; }
}

static void draw_screen(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer ud)
{
    DbgVideo *v = ud;
    GtkWidget *wg = GTK_WIDGET(area);
    const int rows = v->nrows > 0 ? v->nrows : 13;
    int r, c, i;
    (void)width; (void)height;

    for (c = 0; c < CHFDEBUG_UVI_COLS; c++) {
        char t[4];
        g_snprintf(t, sizeof t, "%X", c);
        dbg_set_fg(cr, wg, 0.6);
        dbg_draw_centred(cr, wg, t, SCR_PAD + c * SCR_CELL, 0, SCR_CELL, SCR_PAD - 2, TRUE);
    }
    for (r = 0; r < rows; r++) {
        char t[4];
        g_snprintf(t, sizeof t, "%d", r);
        dbg_set_fg(cr, wg, 0.6);
        dbg_draw_centred(cr, wg, t, 0, SCR_PAD + r * SCR_CELL, SCR_PAD - 2, SCR_CELL, TRUE);
        for (c = 0; c < CHFDEBUG_UVI_COLS; c++) {
            const chfdebug_uvi_cell *cell = &v->cells[r * CHFDEBUG_UVI_COLS + c];
            const double x = SCR_PAD + c * SCR_CELL, y = SCR_PAD + r * SCR_CELL;
            uint8_t shape[8];
            int row, bit;
            const double px = SCR_CELL / 8.0;

            if (v->have_palette)
                dbg_set_rgb(cr, v->palette[cell->bg & 7], 1.0);
            else
                dbg_set_rgb(cr, 0x000000, 1.0);
            cairo_rectangle(cr, x, y, SCR_CELL, SCR_CELL);
            cairo_fill(cr);
            if (cell->graphics)
                rectangle_shape(cell->code & 0x3f, shape);
            else
                memcpy(shape, v->snap.chars[cell->glyph & 0x3f], 8);
            if (v->have_palette)
                dbg_set_rgb(cr, v->palette[cell->fg & 7], 1.0);
            else
                dbg_set_rgb(cr, 0xFFFFFF, 1.0);
            for (row = 0; row < 8; row++)
                for (bit = 0; bit < 8; bit++)
                    if (shape[row] & (0x80 >> bit))
                        cairo_rectangle(cr, x + bit * px, y + row * px, px, px);
            cairo_fill(cr);
        }
    }
    /* the cell grid, and the selected cell outlined */
    dbg_set_fg(cr, wg, 0.25);
    cairo_set_line_width(cr, 1);
    for (i = 0; i <= CHFDEBUG_UVI_COLS; i++) {
        cairo_move_to(cr, SCR_PAD + i * SCR_CELL + 0.5, SCR_PAD);
        cairo_line_to(cr, SCR_PAD + i * SCR_CELL + 0.5, SCR_PAD + rows * SCR_CELL);
    }
    for (i = 0; i <= rows; i++) {
        cairo_move_to(cr, SCR_PAD, SCR_PAD + i * SCR_CELL + 0.5);
        cairo_line_to(cr, SCR_PAD + CHFDEBUG_UVI_COLS * SCR_CELL, SCR_PAD + i * SCR_CELL + 0.5);
    }
    cairo_stroke(cr);
    if (v->screen_sel >= 0 && v->screen_sel < rows * CHFDEBUG_UVI_COLS) {
        dbg_set_rgb(cr, dbg_accent_line_rgb(), 1.0);
        cairo_set_line_width(cr, 2);
        cairo_rectangle(cr, SCR_PAD + (v->screen_sel % CHFDEBUG_UVI_COLS) * SCR_CELL + 1,
                        SCR_PAD + (v->screen_sel / CHFDEBUG_UVI_COLS) * SCR_CELL + 1,
                        SCR_CELL - 2, SCR_CELL - 2);
        cairo_stroke(cr);
    }
}

static void screen_select(DbgVideo *v, int idx)
{
    char text[200];
    v->screen_sel = idx;
    if (idx >= 0 && idx < v->nrows * CHFDEBUG_UVI_COLS)
        describe_cell(v, idx, text, sizeof text);
    else
        g_snprintf(text, sizeof text, "Click a cell to see its code and colours");
    gtk_label_set_text(v->screen_text, text);
    gtk_widget_queue_draw(v->screen_area);
}

static void on_screen_click(GtkGestureClick *g, int n, double x, double y, gpointer ud)
{
    DbgVideo *v = ud;
    const int c = (int)floor((x - SCR_PAD) / SCR_CELL), r = (int)floor((y - SCR_PAD) / SCR_CELL);
    (void)g; (void)n;
    if (c < 0 || c >= CHFDEBUG_UVI_COLS || r < 0 || r >= v->nrows) return;
    screen_select(v, r * CHFDEBUG_UVI_COLS + c);
}

static void refresh_screen(DbgVideo *v)
{
    char t[200];
    int h;
    fetch_palette(v);
    v->nrows = chfdebug_uvi_cells(v->dbg, v->cells);
    h = SCR_PAD + (v->nrows > 0 ? v->nrows : 13) * SCR_CELL + 2;
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(v->screen_area), h);
    if (v->screen_sel >= v->nrows * CHFDEBUG_UVI_COLS)
        v->screen_sel = -1;
    if (v->screen_sel >= 0)
        describe_cell(v, v->screen_sel, t, sizeof t);
    else
        g_snprintf(t, sizeof t, "Click a cell to see its code and colours");
    gtk_label_set_text(v->screen_text, t);
    gtk_widget_queue_draw(v->screen_area);
}

static GtkWidget *build_screen(DbgVideo *v)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkGesture *click = gtk_gesture_click_new();

    v->screen_sel = -1;
    v->screen_area = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(v->screen_area),
                                       SCR_PAD + CHFDEBUG_UVI_COLS * SCR_CELL + 2);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(v->screen_area),
                                        SCR_PAD + 13 * SCR_CELL + 2);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(v->screen_area), draw_screen, v, NULL);
    gtk_widget_set_halign(v->screen_area, GTK_ALIGN_START);
    g_signal_connect(click, "pressed", G_CALLBACK(on_screen_click), v);
    gtk_widget_add_controller(v->screen_area, GTK_EVENT_CONTROLLER(click));
    v->screen_text = GTK_LABEL(dbg_mono_label());
    gtk_label_set_selectable(v->screen_text, FALSE);
    gtk_box_append(GTK_BOX(box), dbg_heading("The character screen: 16 columns x 13 rows (26 with two screens)"));
    gtk_box_append(GTK_BOX(box), v->screen_area);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(v->screen_text));
    gtk_box_append(GTK_BOX(box), dbg_hint("Each cell in the colours the chip gives it: the "
                                          "character colour from its own colour bits (multicolor) "
                                          "or the palette, on the screen colour. Codes $C0/$40 "
                                          "switch block graphics on and off within a row; "
                                          "glyphs $38-$3F are the UDCs.", 44));
    dbg_padded(box);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    return scroll;
}

/* ---- the registers ------------------------------------------------------------------------ */

static void on_reg_activate(GtkEntry *entry, gpointer ud)
{
    DbgVideo *v = ud;
    unsigned off = (unsigned)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(entry), "reg"));
    long val;
    if (dbg_parse_num(gtk_editable_get_text(GTK_EDITABLE(entry)), &val)) {
        gtk_widget_remove_css_class(GTK_WIDGET(entry), "error");
        chfdebug_uvi_write(v->dbg, off, (uint8_t)val);
        dbg_video_refresh(v, FALSE);
    } else {
        gtk_widget_add_css_class(GTK_WIDGET(entry), "error");
    }
}

static GtkWidget *build_regs(DbgVideo *v)
{
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    int i, n = 0;
    unsigned a, g;

    for (g = 0; g < G_N_ELEMENTS(k_reg_ranges); g++)
        for (a = k_reg_ranges[g].first; a <= k_reg_ranges[g].last && n < NREGROWS; a++, n++)
            v->reg_off[n] = a;
    for (i = 0; i < n; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *addr, *name, *val, *text;
        char ab[8];
        g_snprintf(ab, sizeof ab, "$%04X", 0x1800 + v->reg_off[i]);
        addr = gtk_label_new(ab);
        gtk_widget_add_css_class(addr, "monospace");
        gtk_widget_add_css_class(addr, "dim-label");
        name = gtk_label_new("");
        gtk_widget_add_css_class(name, "monospace");
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_label_set_width_chars(GTK_LABEL(name), 12);
        gtk_label_set_max_width_chars(GTK_LABEL(name), 12);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        val = gtk_entry_new();
        gtk_entry_set_max_length(GTK_ENTRY(val), 2);
        gtk_editable_set_width_chars(GTK_EDITABLE(val), 2);
        gtk_editable_set_max_width_chars(GTK_EDITABLE(val), 2);
        gtk_widget_add_css_class(val, "monospace");
        gtk_widget_add_css_class(val, "chf-hex");
        g_object_set_data(G_OBJECT(val), "reg", GINT_TO_POINTER((int)v->reg_off[i]));
        g_signal_connect(val, "activate", G_CALLBACK(on_reg_activate), v);
        text = gtk_label_new("");
        gtk_widget_add_css_class(text, "monospace");
        gtk_label_set_xalign(GTK_LABEL(text), 0);
        gtk_widget_set_hexpand(text, TRUE);
        gtk_label_set_width_chars(GTK_LABEL(text), 24);
        gtk_label_set_max_width_chars(GTK_LABEL(text), 40);
        gtk_label_set_ellipsize(GTK_LABEL(text), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(row), addr);
        gtk_box_append(GTK_BOX(row), name);
        gtk_box_append(GTK_BOX(row), val);
        gtk_box_append(GTK_BOX(row), text);
        gtk_box_append(GTK_BOX(list), row);
        v->reg_name[i] = GTK_LABEL(name);
        v->reg_val[i] = GTK_ENTRY(val);
        v->reg_text[i] = GTK_LABEL(text);
    }
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(outer), dbg_hint("The UVI's registers: $18F0-$18FF (object positions, "
                                            "sound, screen geometry), $1900-$1908 (the keypads and "
                                            "panel, read-only), $19F8-$19FF (colours, collisions, "
                                            "pots). The screen RAM and the UDCs are in the Screen "
                                            "and Sheets sections and the CPU tab's memory viewer. "
                                            "Enter in a value writes it, with the chip's write "
                                            "effects; a running program may write it again.", 44));
    gtk_box_append(GTK_BOX(outer), scroll);
    return dbg_padded(outer);
}

/* Values from the snapshot's raw file; a decoded line only for the ones whose
 * value changed (and the ones that depend on the beam or the keys), because
 * each description is a call into the running machine. */
static void refresh_regs(DbgVideo *v)
{
    int i;
    for (i = 0; i < NREGROWS; i++) {
        const unsigned off = v->reg_off[i];
        const uint8_t val = v->snap.reg[off];
        const gboolean live = off == 0x0FF || (off >= 0x100 && off <= 0x108) ||
                              off == 0x1FC || off == 0x1FD || off == 0x1FE || off == 0x1FF;
        char hex[4];

        if (!v->reg_built || v->reg_shown[i] != val || live) {
            char name[64], text[160];
            chfdebug_uvi_describe(v->dbg, off, name, sizeof name, text, sizeof text);
            if (!v->reg_built)
                gtk_label_set_text(v->reg_name[i], name);
            gtk_label_set_text(v->reg_text[i], text);
            gtk_widget_set_tooltip_text(GTK_WIDGET(v->reg_text[i]), text);
            v->reg_shown[i] = val;
        }
        g_snprintf(hex, sizeof hex, "%02X", val);
        dbg_set_entry(v->dbg, v->reg_val[i], hex);
    }
    v->reg_built = TRUE;
}

/* ---- control, collisions and the beam --------------------------------------------------------- */

static void refresh_control(DbgVideo *v)
{
    static const char *const pair[6] = { "0-1", "0-2", "0-3", "1-2", "1-3", "2-3" };
    const chfdebug_uvi *s = &v->snap;
    GString *t = g_string_new(NULL);
    int b;

    g_string_append_printf(t,
        "SCREEN\n"
        "  rows          %d%s, cells 8 x %d\n"
        "  first line    %d ($18FC)     fine scroll %d pixels ($18FE bits 5-7)\n"
        "  multicolor    %s   (colour bits per character, $18FD bit 7)\n"
        "  block graphics at the top of the screen  %s   ($19F8 bit 7)\n"
        "  pots read     %s   ($19F9 bit 6)\n\n",
        s->rows, s->two_screens ? " (two screens)" : "", s->cell_height, s->first_line, s->shift,
        s->multicolor ? "on" : "off", s->block_graphics ? "on" : "off",
        s->pot_y ? "the X axes" : "the Y axes (multiplexed)");
    g_string_append_printf(t,
        "COLOURS\n"
        "  screen        %d %s   character %d %s       ($19F9)\n"
        "  alternate     screen %d %s   character %d %s       ($19F8)\n\n",
        s->screen_colour, chfdebug_uvi_colour_name(s->screen_colour),
        s->char_colour, chfdebug_uvi_colour_name(s->char_colour),
        s->alt_screen_colour, chfdebug_uvi_colour_name(s->alt_screen_colour),
        s->alt_char_colour, chfdebug_uvi_colour_name(s->alt_char_colour));
    g_string_append_printf(t,
        "COLLISIONS (a 0 bit is a hit; reading clears them)\n"
        "  objects x characters  $19FC = $%02X   ", s->coll_chars);
    for (b = 0; b < 4; b++)
        g_string_append_printf(t, "obj%d %s  ", b, (s->coll_chars >> b) & 1 ? "-" : "HIT");
    g_string_append_printf(t, "\n  objects x objects     $19FD = $%02X\n    ", s->coll_objects);
    for (b = 0; b < 6; b++)
        g_string_append_printf(t, "%s %s  ", pair[b], (s->coll_objects >> b) & 1 ? "-" : "HIT");
    g_string_append_printf(t,
        "\n\nBEAM\n"
        "  line          %d of %d   character row %d ($18FF)   vblank (SENSE) %s\n\n",
        s->line, s->lines, s->charline, s->vblank ? "yes" : "no");
    g_string_append_printf(t,
        "SOUND (the beeper; the Sound & I/O tab decodes it live)\n"
        "  pitch divider %d ($18FD bits 0-6)   volume %d   tone %s   noise %s ($18FE)\n",
        s->sound_pitch, s->sound_volume, s->sound_tone ? "on" : "off",
        s->sound_noise ? "on" : "off");
    gtk_label_set_text(v->control_text, t->str);
    g_string_free(t, TRUE);
}

/* ---- sheets --------------------------------------------------------------------------------------------- */

static void refresh_sheets(DbgVideo *v)
{
    show_view(v, NULL, v->sheet_font, CHFDEBUG_UVI_FONT, 0, 0, 3, FALSE);
    show_view(v, NULL, v->sheet_objects, CHFDEBUG_UVI_OBJECTS, 0, 0, 3, FALSE);
    show_view(v, NULL, v->sheet_palette, CHFDEBUG_UVI_PALETTE, 0, 0, 2, FALSE);
}

static GtkWidget *build_sheets(DbgVideo *v)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *scroll = gtk_scrolled_window_new();

    v->sheet_font = image_area_new();
    v->sheet_objects = image_area_new();
    v->sheet_palette = image_area_new();
    gtk_box_append(GTK_BOX(box), dbg_heading("Glyph table: 64 characters, 16 x 4 ($38-$3F, the last "
                                             "eight, are the UDCs)"));
    gtk_box_append(GTK_BOX(box), v->sheet_font->widget);
    gtk_box_append(GTK_BOX(box), dbg_heading("The four objects' images, in their colours"));
    gtk_box_append(GTK_BOX(box), v->sheet_objects->widget);
    gtk_box_append(GTK_BOX(box), dbg_heading("Palette: the UVI's 8 colours"));
    gtk_box_append(GTK_BOX(box), v->sheet_palette->widget);
    dbg_padded(box);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    return scroll;
}

/* ---- the tab ------------------------------------------------------------------------------------------------- */

static void refresh_section(DbgVideo *v, int sec)
{
    switch (sec) {
    case SEC_OBJECTS: refresh_objects(v); break;
    case SEC_SCREEN: refresh_screen(v); break;
    case SEC_REGS: refresh_regs(v); break;
    case SEC_CONTROL: refresh_control(v); break;
    case SEC_SHEETS: refresh_sheets(v); break;
    default: break;
    }
}

static void on_section_switch(GtkNotebook *nb, GtkWidget *page, guint num, gpointer ud)
{
    DbgVideo *v = ud;
    (void)nb; (void)page;
    chfdebug_uvi_get(v->dbg, &v->snap);
    refresh_section(v, (int)num);
}

void dbg_video_refresh(DbgVideo *v, gboolean live)
{
    chfdebug_uvi_get(v->dbg, &v->snap);
    picture_update(v);
    /* while the machine runs, the sections look at the registers a few times
     * a second; stopped, every call is a change worth showing */
    if (live && ++v->tick < 5)
        return;
    v->tick = 0;
    refresh_section(v, gtk_notebook_get_current_page(v->sections));
}

static GtkWidget *check(const char *label, gboolean active, GCallback cb, gpointer ud)
{
    GtkWidget *c = gtk_check_button_new_with_label(label);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(c), active);
    g_signal_connect(c, "toggled", cb, ud);
    return c;
}

DbgVideo *dbg_video_new(chfsession *session, chfdebug *dbg, GtkWidget **root)
{
    static const char *const views[] = { "Frame (what the display shows)",
                                         "Rendered from the registers",
                                         "Frame in progress, to the beam", NULL };
    DbgVideo *v = g_new0(DbgVideo, 1);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row, *overlay, *layers, *flags, *lscroll;
    GtkEventController *motion;
    int i;

    v->session = session;
    v->dbg = dbg;
    v->cap = (size_t)CHFDEBUG_UVI_VIEW_MAX_PIXELS * SCALE * SCALE;
    v->buf = g_new0(uint32_t, v->cap);
    v->prev = g_new0(uint32_t, v->cap);

    /* the view and its layers and overlays */
    v->view = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(views));
    g_signal_connect(v->view, "notify::selected", G_CALLBACK(on_view_changed), v);
    row = dbg_labeled("View", GTK_WIDGET(v->view));
    layers = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(layers), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(layers), 4);
    for (i = 0; i < NLAYERS; i++) {
        v->layer[i] = GTK_CHECK_BUTTON(check(k_layers[i].name, TRUE, G_CALLBACK(on_option_toggled), v));
        gtk_flow_box_append(GTK_FLOW_BOX(layers), GTK_WIDGET(v->layer[i]));
    }
    flags = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flags), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flags), 4);
    for (i = 0; i < NFLAGS; i++) {
        v->flag[i] = GTK_CHECK_BUTTON(check(k_flags[i].name, i == 0, G_CALLBACK(on_option_toggled), v));
        gtk_flow_box_append(GTK_FLOW_BOX(flags), GTK_WIDGET(v->flag[i]));
    }

    /* the picture, with the overlay that indexes and outlines over it */
    v->pic = new_picture();
    gtk_widget_set_size_request(GTK_WIDGET(v->pic), FRAME_W * SCALE, FRAME_H * SCALE);
    v->overlay_area = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(v->overlay_area), draw_overlay, v, NULL);
    gtk_widget_set_can_target(v->overlay_area, FALSE);
    overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay), GTK_WIDGET(v->pic));
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), v->overlay_area);
    gtk_widget_set_halign(overlay, GTK_ALIGN_START);
    gtk_widget_set_valign(overlay, GTK_ALIGN_START);
    motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), v);
    g_signal_connect(motion, "leave", G_CALLBACK(on_leave), v);
    gtk_widget_add_controller(overlay, motion);

    v->hover_label = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(v->hover_label), "monospace");
    gtk_label_set_xalign(v->hover_label, 0);
    gtk_label_set_ellipsize(v->hover_label, PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(v->hover_label, 1);
    gtk_label_set_text(v->hover_label, "Move the pointer over the picture to see what draws there");

    gtk_box_append(GTK_BOX(left), row);
    gtk_box_append(GTK_BOX(left), layers);
    gtk_box_append(GTK_BOX(left), flags);
    gtk_box_append(GTK_BOX(left), overlay);
    gtk_box_append(GTK_BOX(left), GTK_WIDGET(v->hover_label));
    gtk_widget_set_size_request(left, FRAME_W * SCALE, -1);
    gtk_widget_set_hexpand(left, FALSE);

    /* the sections */
    v->sections = GTK_NOTEBOOK(gtk_notebook_new());
    gtk_notebook_set_scrollable(v->sections, TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(v->sections), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(v->sections), TRUE);
    gtk_notebook_append_page(v->sections, build_objects(v), gtk_label_new("Objects"));
    gtk_notebook_append_page(v->sections, build_screen(v), gtk_label_new("Screen"));
    gtk_notebook_append_page(v->sections, build_regs(v), gtk_label_new("Registers"));
    {
        GtkWidget *scroll = gtk_scrolled_window_new();
        v->control_text = GTK_LABEL(dbg_mono_label());
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), dbg_padded(GTK_WIDGET(v->control_text)));
        gtk_notebook_append_page(v->sections, scroll, gtk_label_new("Control"));
    }
    gtk_notebook_append_page(v->sections, build_sheets(v), gtk_label_new("Sheets"));
    g_signal_connect(v->sections, "switch-page", G_CALLBACK(on_section_switch), v);

    /* the picture cannot shrink, so the column scrolls rather than hold the
     * window to its height on a small screen */
    lscroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(lscroll), left);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(lscroll), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(lscroll), TRUE);
    gtk_widget_set_hexpand(lscroll, FALSE);
    gtk_box_append(GTK_BOX(box), lscroll);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(v->sections));
    sync_sensitivity(v);
    *root = dbg_padded(box);
    return v;
}

void dbg_video_free(DbgVideo *v)
{
    if (!v) return;
    g_free(v->buf);
    g_free(v->prev);
    g_free(v);
}
