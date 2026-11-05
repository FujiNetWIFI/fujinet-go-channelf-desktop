/*
 * The Controllers window -- the Channel F console drawn like the photo: a
 * charcoal case with its wood strip, the two hand controllers (each a 12-key
 * gold keypad, the gold disc and the side FIRE button) with the console's
 * START / OPTION / SELECT / RESET between them. The keypads and the panel are
 * drawn from the session's layout table (chfsession_keyboard_layout), so
 * every frontend shows the same caps.
 *
 * Every control is pressed on the raw button press and released on the
 * raw release (a GtkGestureDrag, not a click: a click gesture gives up when
 * the pointer wobbles), because the console samples its keypads and discs
 * once per frame -- a press present only for the instant of a click falls
 * between frames and a polling program never sees it. A key on screen is HELD
 * for as long as the mouse button is down, exactly like the real one, and
 * dragging off a cap lets it go. The disc is dragged: press anywhere on it and
 * move the pointer, and the directions follow it (up to two at once, the
 * diagonals), the knob showing where the pot is. Losing the focus, hiding and
 * rebuilding let go of whatever the mouse holds here (only that: the main
 * window releases its own keys).
 *
 * What the console sees held -- host keyboard, gamepads and this window
 * together (chfsession_key_held, chfsession_buttons_held) -- is painted in the
 * accent colour (the keypads' gold, lightened), and the disc knob follows the
 * pot: the gamepad's analog stick when there is one
 * (chfsession_disc_get), else the held directions. The window doubles as an
 * input tester for a new gamepad.
 *
 * Keys typed with the window active go through the main window
 * (chf_window_key), so the console, F9 and the shortcuts behave the same
 * whichever window has the focus.
 *
 * FIXED SIZE, FLOATING: the drawn parts have fixed sizes (key caps at a fixed
 * number of pixels per key unit), labels ellipsize rather than grow, so
 * neither Map mode nor a long gamepad name resizes the window, and the window
 * is not resizable, so tiling compositors float it. It is hidden at startup
 * (F9 or the View menu shows it).
 *
 * SINGLETON, hidden rather than destroyed, so a remap survives closing it.
 * Its timers run only while it is on screen.
 *
 * MAP MODE: the Map button arms a two-step rebind -- click a key cap, a disc
 * direction, a FIRE button or a console button to pick the target, then press
 * a keyboard key OR a gamepad button. While armed the same gestures pick the
 * target instead of pressing, every control shows the key bound to it, and
 * typed keys are not typed into the console; once a target is picked the
 * other controls are greyed. Rebinding steals the key from whatever held it.
 * Escape cancels. Defaults puts every binding back.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controllers_window.h"

#include "../window.h"

#include <math.h>
#include <pango/pangocairo.h>
#include <string.h>

#define CASE_PAD 18             /* the charcoal case around everything */
#define WOOD_H 24               /* the wood strip along the top */
#define UNIT 38                 /* pixels per key unit */
#define DISC_R1 (1.55 * UNIT)   /* the disc's outer radius */
#define DISC_R0 (0.5 * UNIT)    /* ... and the dome's */
#define FIRE_W UNIT
#define FIRE_H (1.7 * UNIT)
#define FIRE_GAP 10
#define STRIP_BW 214
#define STRIP_GAP 12
#define STRIP_H 34
#define MAX_ITEMS 80
#define MAX_PADS 8

/* the disc's directional pads: the angle each spans, as drawn and as hit */
#define PAD_DRAW_SPAN (38.0 * G_PI / 180.0)
#define PAD_HIT_SPAN (45.0 * G_PI / 180.0)

enum { PANE_CONSOLE, PANE_STRIP, PANE_COUNT };
enum { SH_RECT, SH_WEDGE };
/* kinds beyond chf_keycap_kind */
enum { K_FIRE = 100, K_PAD, K_DISC, K_BUTTON };

typedef struct {
    int shape;
    int kind;                   /* chf_keycap_kind, or K_* */
    int target;                 /* what a press drives and Map rebinds */
    int port;                   /* a disc's pad / fire / the disc itself */
    const char *label;
    double x, y, w, h;          /* SH_RECT; a wedge or disc: x, y the centre,
                                   w the inner radius, h the outer */
    double a;                   /* SH_WEDGE: the pad's centre angle */
    gboolean lit;               /* what the console sees held */
} Item;

typedef struct {
    GtkWidget *area;
    Item items[MAX_ITEMS];
    int n;
    int pressed;                /* the item the mouse holds, or -1 */
    int hover;
} Pane;

/* ---- singleton state ------------------------------------------------------ */

static GtkWidget *g_window;
static GtkWindow *g_parent;     /* the ChfWindow: typed keys go through it */
static chfsession *g_session;
static AdwWindowTitle *g_title;
static Pane g_pane[PANE_COUNT];

/* The disc the mouse is dragging, and the directions it holds. */
static struct {
    gboolean active;
    int port;
    unsigned mask;              /* bit per chf_action */
} g_drag = { FALSE, 0, 0 };
static int g_last_disc[2][2];   /* the analog deflection last painted */

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 waiting for a
 * key or pad button to bind to that target. */
static int g_map_state = -2;
static GtkWidget *g_map_button;
static GtkWidget *g_map_hint;
static GtkWidget *g_show_bindings;
static guint g_capture_timer;
static guint g_swallow_code;    /* the key Map took: its repeats and release too */
static guint g_held_timer;
static guint g_pad_timer;

static GtkWidget *g_pad_box;
static GtkWidget *g_pad_port[MAX_PADS];     /* each pad's "drives player N" */
static int g_npad_rows;
static unsigned g_pad_generation = (unsigned)-1;

/* what the console was built for */
static int g_built_machine = -1;

static void refresh_all(void);

/* ---- small helpers ---------------------------------------------------------- */

static void rgb(cairo_t *cr, guint32 c)
{
    cairo_set_source_rgb(cr, ((c >> 16) & 0xFF) / 255.0, ((c >> 8) & 0xFF) / 255.0,
                         (c & 0xFF) / 255.0);
}

static guint32 mix(guint32 a, guint32 b, double t)
{
    guint32 out = 0;
    int s;
    for (s = 16; s >= 0; s -= 8) {
        double ca = (a >> s) & 0xFF, cb = (b >> s) & 0xFF;
        out |= (guint32)(ca + (cb - ca) * t + 0.5) << s;
    }
    return out;
}

static void rrect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

/* Text centred in a box, in an absolute pixel size, ellipsized to the box. */
static void legend(cairo_t *cr, GtkWidget *w, const char *text, double px, gboolean bold,
                   guint32 colour, double x, double y, double bw, double bh)
{
    PangoLayout *l = gtk_widget_create_pango_layout(w, text);
    PangoFontDescription *fd = pango_font_description_new();
    int tw, th;

    pango_font_description_set_family(fd, "Sans");
    pango_font_description_set_weight(fd, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_font_description_set_absolute_size(fd, px * PANGO_SCALE);
    pango_layout_set_font_description(l, fd);
    pango_layout_set_width(l, (int)(bw * PANGO_SCALE));
    pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
    pango_layout_set_alignment(l, PANGO_ALIGN_CENTER);
    pango_layout_get_pixel_size(l, &tw, &th);
    rgb(cr, colour);
    cairo_move_to(cr, x, y + (bh - th) / 2.0);
    pango_cairo_show_layout(cr, l);
    /* the move_to above is still the current point, and the next arc would
     * join it with a line */
    cairo_new_path(cr);
    pango_font_description_free(fd);
    g_object_unref(l);
}

/* A host key's name as a small legend: the long ones shortened to fit a cap. */
static void legend_name(guint32 keysym, char *dst, int dstsz)
{
    char name[32];
    chfsession_keysym_name(keysym, name, sizeof name);
    if (g_str_has_prefix(name, "Keypad "))
        g_snprintf(dst, dstsz, "KP %s", name + 7);
    else if (g_str_has_prefix(name, "Left "))
        g_snprintf(dst, dstsz, "L %s", name + 5);
    else if (g_str_has_prefix(name, "Right "))
        g_snprintf(dst, dstsz, "R %s", name + 6);
    else if (!strcmp(name, "Backspace"))
        g_snprintf(dst, dstsz, "BkSp");
    else if (!strcmp(name, "Escape"))
        g_snprintf(dst, dstsz, "Esc");
    else
        g_snprintf(dst, dstsz, "%s", name);
}

static gboolean is_sysact(int target)
{
    return target >= CHF_TARGET_SYSACT(0);
}

/* What the console sees of an item held, from any source. */
static gboolean item_held(const Item *it, const unsigned held[2])
{
    int t = it->target;
    if (t < 0)
        return FALSE;
    if (t < 2 * CHF_ACT_PER_PORT)
        return ((held[t / CHF_ACT_PER_PORT] >> (t % CHF_ACT_PER_PORT)) & 1) != 0;
    if (t < CHF_TARGET_SWITCH(0))
        return chfsession_key_held(g_session, t - CHF_TARGET_KEY(0)) != 0;
    if (t < CHF_TARGET_SYSACT(0))
        return chfsession_switch_held(g_session, t - CHF_TARGET_SWITCH(0)) != 0;
    return FALSE;
}

static gboolean showing_bindings(void)
{
    return g_map_state != -2 ||
           gtk_check_button_get_active(GTK_CHECK_BUTTON(g_show_bindings));
}


/* ---- drawing ------------------------------------------------------------------- */

#define COL_CASE_TOP 0x3C3C3F
#define COL_CASE_BOT 0x232325
#define COL_CASE_EDGE 0x0C0C0D
#define COL_GOLD CHFSESSION_ACCENT_RGB
#define COL_GOLD_DARK CHFSESSION_ACCENT_DARK_RGB
#define COL_GOLD_LIGHT 0xEBD6A6
#define COL_LEGEND 0x1E1A14
#define COL_FUNC 0x3A3A3E
#define COL_FUNC_SIDE 0x121214
#define COL_RESET 0xB5322A
#define COL_RESET_SIDE 0x5E120E
#define COL_RESET_HELD 0xFF6A5C

typedef struct {
    gboolean held, selected, dim, hot;
} ItemState;

static ItemState item_state(const Pane *p, int i)
{
    const Item *it = &p->items[i];
    ItemState st;
    st.held = it->lit || p->pressed == i;
    st.selected = g_map_state >= 0 && g_map_state == it->target;
    st.dim = g_map_state >= 0 && !st.selected;
    st.hot = p->hover == i && !st.held;
    return st;
}

/* The binding legend for a target, or "" when none or not shown. */
static void bound_text(int target, char *dst, int dstsz)
{
    chf_binding b;
    dst[0] = '\0';
    if (!showing_bindings()) return;
    b = chfsession_binding_get(g_session, target);
    if (b.keysym)
        legend_name(b.keysym, dst, dstsz);
    else
        g_snprintf(dst, dstsz, "\xe2\x80\x94");
}

static void rgba_rgb(cairo_pattern_t *g, double off, guint32 c)
{
    cairo_pattern_add_color_stop_rgb(g, off, ((c >> 16) & 0xFF) / 255.0,
                                     ((c >> 8) & 0xFF) / 255.0, (c & 0xFF) / 255.0);
}

static void draw_cap(cairo_t *cr, GtkWidget *w, const Pane *p, int i)
{
    const Item *it = &p->items[i];
    const ItemState st = item_state(p, i);
    guint32 face = COL_GOLD, side = COL_GOLD_DARK, ink = COL_LEGEND;
    double fh = it->h - 3, ly = it->y, lh = fh;
    char bound[40];
    const int units = (int)g_utf8_strlen(it->label, -1);
    double px;

    if (it->kind == CHF_KEYCAP_INPUT) {
        face = mix(COL_GOLD, 0x000000, 0.14);
    } else if (it->kind == CHF_KEYCAP_FUNCTION) {
        face = COL_FUNC; side = COL_FUNC_SIDE; ink = COL_GOLD_LIGHT;
    } else if (it->kind == CHF_KEYCAP_RESET) {
        face = COL_RESET; side = COL_RESET_SIDE; ink = 0xFFFFFF;
    }
    if (st.held || st.selected) {
        face = it->kind == CHF_KEYCAP_RESET ? COL_RESET_HELD : COL_GOLD_LIGHT;
        side = it->kind == CHF_KEYCAP_RESET ? COL_RESET : COL_GOLD;
        ink = COL_LEGEND;
    } else if (st.hot) {
        face = mix(face, 0xFFFFFF, 0.18);
    }

    if (st.dim) cairo_push_group(cr);
    rrect(cr, it->x, it->y + 3, it->w, it->h - 3, 5);
    rgb(cr, side);
    cairo_fill(cr);
    rrect(cr, it->x, it->y, it->w, fh, 5);
    rgb(cr, face);
    cairo_fill_preserve(cr);
    rgb(cr, st.held || st.selected ? 0xFFFFFF : mix(face, 0x000000, 0.45));
    cairo_set_line_width(cr, st.held || st.selected ? 2 : 1);
    cairo_stroke(cr);

    bound_text(it->target, bound, sizeof bound);
    if (bound[0]) {
        lh = fh * 0.62;
        legend(cr, w, bound, MAX(7.0, UNIT * 0.2), FALSE,
               mix(ink, face, 0.35), it->x, it->y + lh - 2, it->w, fh - lh);
    }
    px = units <= 1 ? UNIT * 0.40 : units <= 3 ? UNIT * 0.28 : UNIT * 0.22;
    legend(cr, w, it->label, px, TRUE, ink, it->x, ly, it->w, lh);
    if (st.dim) {
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, 0.4);
    }
}

/* The wood strip along the top of the case: a brown gradient with a few
 * wandering grain lines. */
static void draw_wood(cairo_t *cr, double x, double y, double w, double h)
{
    cairo_pattern_t *g = cairo_pattern_create_linear(0, y, 0, y + h);
    int i;
    rgba_rgb(g, 0, 0x9A6B3C);
    rgba_rgb(g, 0.5, 0x7C5028);
    rgba_rgb(g, 1, 0x5E3A1B);
    rrect(cr, x, y, w, h, 4);
    cairo_set_source(cr, g);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(g);
    cairo_save(cr);
    cairo_clip(cr);
    cairo_set_line_width(cr, 0.8);
    for (i = 0; i < 9; i++) {
        double yy = y + 2.5 + i * (h - 5) / 8.0, xx;
        rgb(cr, i % 2 ? 0x6A4322 : 0xA77A4A);
        cairo_move_to(cr, x, yy);
        for (xx = x; xx <= x + w; xx += 8)
            cairo_line_to(cr, xx, yy + 1.6 * sin(xx * 0.045 + i * 1.7));
        cairo_stroke(cr);
    }
    cairo_restore(cr);
    rrect(cr, x, y, w, h, 4);
    rgb(cr, 0x2A1809);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
}

/* The disc: a gold plate, four direction pads and a dome that follows the
 * pot. */
static void draw_disc(cairo_t *cr, GtkWidget *w, const Pane *p, int port)
{
    double cx = 0, cy = 0, r0 = DISC_R0, r1 = DISC_R1, kx = 0, ky = 0;
    int ax, ay, i;
    gboolean any = FALSE, analog;
    char bound[40];

    for (i = 0; i < p->n; i++)
        if (p->items[i].kind == K_DISC && p->items[i].port == port) {
            cx = p->items[i].x; cy = p->items[i].y; r1 = p->items[i].w; any = TRUE;
        }
    if (!any) return;

    /* the well and the plate */
    cairo_arc(cr, cx, cy, r1 + 5, 0, 2 * G_PI);
    rgb(cr, 0x111112);
    cairo_fill(cr);
    cairo_arc(cr, cx, cy, r1 + 1, 0, 2 * G_PI);
    rgb(cr, COL_GOLD_DARK);
    cairo_fill(cr);

    for (i = 0; i < p->n; i++) {
        const Item *it = &p->items[i];
        const ItemState st = item_state(p, i);
        const double mid = (it->w + it->h) / 2;
        const double px = cx + cos(it->a) * mid, py = cy + sin(it->a) * mid;
        guint32 face, ink = COL_LEGEND;

        if (it->kind != K_PAD || it->port != port) continue;
        face = st.held || st.selected ? COL_GOLD_LIGHT
             : st.hot ? mix(COL_GOLD, 0xFFFFFF, 0.2) : COL_GOLD;
        if (st.dim) cairo_push_group(cr);
        cairo_arc(cr, cx, cy, it->h, it->a - PAD_DRAW_SPAN, it->a + PAD_DRAW_SPAN);
        cairo_arc_negative(cr, cx, cy, it->w, it->a + PAD_DRAW_SPAN, it->a - PAD_DRAW_SPAN);
        cairo_close_path(cr);
        rgb(cr, face);
        cairo_fill_preserve(cr);
        rgb(cr, st.held || st.selected ? 0xFFFFFF : mix(face, 0x000000, 0.45));
        cairo_set_line_width(cr, st.held || st.selected ? 2 : 1);
        cairo_stroke(cr);
        bound_text(it->target, bound, sizeof bound);
        if (bound[0]) {
            legend(cr, w, bound, 9, TRUE, ink, px - 20, py - 9, 40, 18);
        } else {
            /* an arrow pointing the way the pad pushes */
            const double ux = cos(it->a), uy = sin(it->a);
            cairo_move_to(cr, px + ux * 6, py + uy * 6);
            cairo_line_to(cr, px - ux * 4 - uy * 6, py - uy * 4 + ux * 6);
            cairo_line_to(cr, px - ux * 4 + uy * 6, py - uy * 4 - ux * 6);
            cairo_close_path(cr);
            rgb(cr, ink);
            cairo_fill(cr);
        }
        if (st.dim) {
            cairo_pop_group_to_source(cr);
            cairo_paint_with_alpha(cr, 0.4);
        }
    }

    /* the dome follows the pot: the analog deflection, else the held pads */
    chfsession_disc_get(g_session, port, &ax, &ay);
    analog = ax != 0 || ay != 0;
    if (analog) {
        kx = ax / 127.0; ky = ay / 127.0;
    } else {
        for (i = 0; i < p->n; i++)
            if (p->items[i].kind == K_PAD && p->items[i].port == port &&
                (p->items[i].lit || (g_drag.active && g_drag.port == port &&
                 ((g_drag.mask >> (p->items[i].target % CHF_ACT_PER_PORT)) & 1)))) {
                kx += cos(p->items[i].a);
                ky += sin(p->items[i].a);
            }
        if (kx != 0 && ky != 0) { kx *= 0.7071; ky *= 0.7071; }
    }
    kx *= r0 * 0.9; ky *= r0 * 0.9;
    cairo_arc(cr, cx + kx, cy + ky + 2, r0 + 1, 0, 2 * G_PI);
    rgb(cr, 0x050505);
    cairo_fill(cr);
    cairo_arc(cr, cx + kx, cy + ky, r0, 0, 2 * G_PI);
    rgb(cr, 0x2C2C30);
    cairo_fill(cr);
    cairo_arc(cr, cx + kx - r0 * 0.28, cy + ky - r0 * 0.28, r0 * 0.34, 0, 2 * G_PI);
    rgb(cr, 0x55555B);
    cairo_fill(cr);
}

/* The FIRE button on the controller's side. */
static void draw_fire(cairo_t *cr, GtkWidget *w, const Pane *p, int i)
{
    const Item *it = &p->items[i];
    const ItemState st = item_state(p, i);
    const gboolean on = st.held || st.selected;
    char bound[40];

    if (st.dim) cairo_push_group(cr);
    rrect(cr, it->x, it->y + 3, it->w, it->h - 3, 8);
    rgb(cr, COL_FUNC_SIDE);
    cairo_fill(cr);
    rrect(cr, it->x, it->y, it->w, it->h - 3, 8);
    rgb(cr, on ? COL_GOLD_LIGHT : st.hot ? mix(COL_FUNC, 0xFFFFFF, 0.15) : COL_FUNC);
    cairo_fill_preserve(cr);
    rgb(cr, on ? 0xFFFFFF : COL_GOLD);
    cairo_set_line_width(cr, on ? 2 : 1.5);
    cairo_stroke(cr);
    bound_text(it->target, bound, sizeof bound);
    if (bound[0]) {
        legend(cr, w, "FIRE", 10, TRUE, on ? COL_LEGEND : COL_GOLD_LIGHT, it->x,
               it->y + 6, it->w, 16);
        legend(cr, w, bound, 9, FALSE, on ? COL_LEGEND : COL_GOLD, it->x,
               it->y + it->h / 2, it->w, 16);
    } else {
        legend(cr, w, "FIRE", 11, TRUE, on ? COL_LEGEND : COL_GOLD_LIGHT, it->x, it->y,
               it->w, it->h - 3);
    }
    if (st.dim) {
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, 0.4);
    }
}

static void draw_console(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer ud)
{
    Pane *p = ud;
    GtkWidget *widget = GTK_WIDGET(area);
    cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, h);
    int i, port;
    (void)area;

    /* the case */
    rgba_rgb(g, 0, COL_CASE_TOP);
    rgba_rgb(g, 1, COL_CASE_BOT);
    rrect(cr, 0.5, 0.5, w - 1, h - 1, 16);
    cairo_set_source(cr, g);
    cairo_fill_preserve(cr);
    rgb(cr, COL_CASE_EDGE);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    cairo_pattern_destroy(g);
    /* the lit top edge of the moulding */
    cairo_move_to(cr, 18, 2.5);
    cairo_line_to(cr, w - 18, 2.5);
    rgb(cr, 0x58585C);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);

    draw_wood(cr, CASE_PAD, CASE_PAD, w - 2 * CASE_PAD, WOOD_H);
    legend(cr, widget, "CHANNELF 2001", 11, TRUE, COL_GOLD, w / 2.0 - 70,
           CASE_PAD + WOOD_H + 6 + 4.3 * UNIT + 22, 140, 16);

    for (port = 0; port < 2; port++)
        draw_disc(cr, widget, p, port);
    for (i = 0; i < p->n; i++) {
        if (p->items[i].kind == K_FIRE)
            draw_fire(cr, widget, p, i);
        else if (p->items[i].kind != K_PAD && p->items[i].kind != K_DISC)
            draw_cap(cr, widget, p, i);
    }
}

/* The console buttons under the case. */
static void draw_strip(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer ud)
{
    Pane *p = ud;
    int i;
    (void)w; (void)h;

    for (i = 0; i < p->n; i++) {
        const Item *it = &p->items[i];
        const ItemState st = item_state(p, i);
        guint32 face = st.held || st.selected ? CHFSESSION_ACCENT_RGB
                     : st.hot ? 0x4A4A52 : 0x38383E;
        char bound[40], text[120];

        if (st.dim) cairo_push_group(cr);
        rrect(cr, it->x, it->y + 3, it->w, it->h - 3, 7);
        rgb(cr, st.held || st.selected ? CHFSESSION_ACCENT_DARK_RGB : 0x1A1A1D);
        cairo_fill(cr);
        rrect(cr, it->x, it->y, it->w, it->h - 3, 7);
        rgb(cr, face);
        cairo_fill(cr);
        bound_text(it->target, bound, sizeof bound);
        if (bound[0])
            g_snprintf(text, sizeof text, "%s  \xc2\xb7  %s", it->label, bound);
        else
            g_snprintf(text, sizeof text, "%s", it->label);
        legend(cr, GTK_WIDGET(area), text, 12, FALSE, 0xFFFFFF, it->x + 4, it->y, it->w - 8,
               it->h - 3);
        if (st.dim) {
            cairo_pop_group_to_source(cr);
            cairo_paint_with_alpha(cr, 0.4);
        }
    }
}

/* ---- hit testing -------------------------------------------------------------------- */

static gboolean item_hit(const Item *it, double x, double y)
{
    if (it->kind == K_DISC)
        return FALSE;           /* the disc as a whole is dragged, not mapped */
    if (it->shape == SH_RECT)
        return x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h;
    {
        const double dx = x - it->x, dy = y - it->y;
        const double r = sqrt(dx * dx + dy * dy);
        double d;
        if (r < it->w || r > it->h) return FALSE;
        d = remainder(atan2(dy, dx) - it->a, 2 * G_PI);
        return fabs(d) <= PAD_HIT_SPAN;
    }
}

static int pane_hit(const Pane *p, double x, double y)
{
    int i;
    for (i = p->n - 1; i >= 0; i--)
        if (item_hit(&p->items[i], x, y))
            return i;
    return -1;
}

/* The disc under the point (any part of it, the dome included), or -1. */
static int disc_at(const Pane *p, double x, double y)
{
    int i;
    for (i = 0; i < p->n; i++) {
        const Item *it = &p->items[i];
        if (it->kind == K_DISC && hypot(x - it->x, y - it->y) <= it->w)
            return it->port;
    }
    return -1;
}

/* The pointer's place on a disc is the analog deflection: the offset from the
 * centre over the disc's radius, clamped to the rim, with a small dead zone,
 * so a corner is a real diagonal and the pots follow the pointer. */
static void disc_follow(const Pane *p, double x, double y)
{
    double cx = 0, cy = 0, r = DISC_R1, dx, dy, len;
    int i, ax = 0, ay = 0;

    for (i = 0; i < p->n; i++)
        if (p->items[i].kind == K_DISC && p->items[i].port == g_drag.port) {
            cx = p->items[i].x; cy = p->items[i].y; r = p->items[i].w;
        }
    dx = (x - cx) / r;
    dy = (y - cy) / r;
    len = hypot(dx, dy);
    if (len > 1.0) { dx /= len; dy /= len; len = 1.0; }
    if (len > 0.12) {
        ax = (int)lround(dx * 127.0);
        ay = (int)lround(dy * 127.0);
    }
    chfsession_disc_set(g_session, g_drag.port, ax, ay);
    gtk_widget_queue_draw(g_pane[PANE_CONSOLE].area);
}

static void disc_release(void)
{
    if (!g_drag.active) return;
    chfsession_disc_set(g_session, g_drag.port, 0, 0);
    g_drag.active = FALSE;
    g_drag.mask = 0;
    if (g_pane[PANE_CONSOLE].area)
        gtk_widget_queue_draw(g_pane[PANE_CONSOLE].area);
}

/* ---- pressing ------------------------------------------------------------- */

static void run_sysaction(int sa)
{
    if (g_parent)
        chf_window_run_sysaction(CHF_WINDOW(g_parent), sa);
    else
        chfsession_sysaction(g_session, sa);
}

/* Lets go of what the mouse holds in one pane. A system action fires on
 * release, like a real button: pressing and dragging off should not
 * power-cycle the cartridge. */
static void pane_release(Pane *p, gboolean activate)
{
    const Item *it;
    if (p->pressed < 0) return;
    it = &p->items[p->pressed];
    p->pressed = -1;
    if (is_sysact(it->target)) {
        if (activate) run_sysaction(it->target - CHF_TARGET_SYSACT(0));
    } else {
        chfsession_press(g_session, it->target, 0);
    }
    gtk_widget_queue_draw(p->area);
}

static void mouse_release(void)
{
    int i;
    for (i = 0; i < PANE_COUNT; i++)
        pane_release(&g_pane[i], FALSE);
    disc_release();
}

static void stop_capture(void)
{
    if (g_capture_timer) {
        g_source_remove(g_capture_timer);
        g_capture_timer = 0;
    }
    chfsession_gamepad_capture_cancel(g_session);
}

static void say_bound(const char *what, const char *stolen)
{
    char msg[220];
    if (stolen[0])
        g_snprintf(msg, sizeof msg, "Bound %s (was %s)", what, stolen);
    else
        g_snprintf(msg, sizeof msg, "Bound %s", what);
    gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
}

static void set_map_state(int state);

static gboolean capture_tick(gpointer d)
{
    int button;
    (void)d;
    if (g_map_state < 0) { g_capture_timer = 0; return G_SOURCE_REMOVE; }
    if (chfsession_gamepad_capture_poll(g_session, &button)) {
        char stolen[128];
        chfsession_binding_set_button(g_session, g_map_state, button, stolen, sizeof stolen);
        g_capture_timer = 0;
        set_map_state(-1);     /* stay armed: remapping several in a row is normal */
        say_bound(chf_pad_button_name(button), stolen);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void set_map_state(int state)
{
    g_map_state = state;
    if (state == -2) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Map");
        gtk_widget_remove_css_class(g_map_button, "chf-accent");
        gtk_label_set_text(GTK_LABEL(g_map_hint),
                           "Hold a key or control with the mouse. Map rebinds one.");
    } else if (state == -1) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Done");
        gtk_widget_add_css_class(g_map_button, "chf-accent");
        gtk_label_set_text(GTK_LABEL(g_map_hint), "Click a key or control to remap it");
    } else {
        char msg[160];
        mouse_release();
        g_snprintf(msg, sizeof msg, "Press a key or a gamepad button for %s (Esc cancels)",
                   chf_target_name(state));
        gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
        chfsession_gamepad_capture_begin(g_session);
        if (!g_capture_timer)
            g_capture_timer = g_timeout_add(50, capture_tick, NULL);
    }
    refresh_all();
}


static void on_drag_begin(GtkGestureDrag *g, double x, double y, gpointer ud)
{
    Pane *p = ud;
    int i, port;
    const Item *it;
    (void)g;
    if (g_map_state == -2 && (port = disc_at(p, x, y)) >= 0) {
        mouse_release();
        g_drag.active = TRUE;
        g_drag.port = port;
        g_drag.mask = 0;
        disc_follow(p, x, y);
        return;
    }
    i = pane_hit(p, x, y);
    if (i < 0) return;
    it = &p->items[i];
    if (g_map_state != -2) {
        /* armed: picks (or changes) the target */
        set_map_state(it->target);
        return;
    }
    mouse_release();
    p->pressed = i;
    if (!is_sysact(it->target))
        chfsession_press(g_session, it->target, 1);
    gtk_widget_queue_draw(p->area);
}

static void on_drag_update(GtkGestureDrag *g, double ox, double oy, gpointer ud)
{
    double sx, sy;
    if (!g_drag.active) return;
    gtk_gesture_drag_get_start_point(g, &sx, &sy);
    disc_follow(ud, sx + ox, sy + oy);
}

static void on_drag_end(GtkGestureDrag *g, double ox, double oy, gpointer ud)
{
    (void)g; (void)ox; (void)oy;
    disc_release();
    pane_release(ud, TRUE);
}

static void on_cancel(GtkGesture *g, GdkEventSequence *seq, gpointer ud)
{
    (void)g; (void)seq;
    disc_release();
    pane_release(ud, FALSE);
}

/* Dragging off the cap lets it go. */
static void on_motion(GtkEventControllerMotion *c, double x, double y, gpointer ud)
{
    Pane *p = ud;
    int i = pane_hit(p, x, y);
    (void)c;
    if (p->pressed >= 0 && i != p->pressed)
        pane_release(p, FALSE);
    if (i != p->hover) {
        p->hover = i;
        gtk_widget_queue_draw(p->area);
    }
}

static void on_leave(GtkEventControllerMotion *c, gpointer ud)
{
    Pane *p = ud;
    (void)c;
    pane_release(p, FALSE);
    if (p->hover >= 0) {
        p->hover = -1;
        gtk_widget_queue_draw(p->area);
    }
}


static gboolean on_query_tooltip(GtkWidget *w, int x, int y, gboolean kbd,
                                 GtkTooltip *tip, gpointer ud)
{
    Pane *p = ud;
    int i = kbd ? -1 : pane_hit(p, x, y);
    chf_binding b;
    char key[32], text[260], name[80];
    (void)w;
    if (i < 0) return FALSE;
    b = chfsession_binding_get(g_session, p->items[i].target);
    chfsession_keysym_name(b.keysym, key, sizeof key);
    g_strlcpy(name, chf_target_name(p->items[i].target), sizeof name);
    g_snprintf(text, sizeof text, "%s\nKey: %s    Gamepad: %s", name,
               key[0] ? key : "none",
               b.button != CHF_PAD_BTN_NONE ? chf_pad_button_name(b.button) : "none");
    gtk_tooltip_set_text(tip, text);
    return TRUE;
}


static void pane_init(Pane *p, int port, GtkDrawingAreaDrawFunc draw)
{
    GtkGesture *drag = gtk_gesture_drag_new();
    GtkEventController *motion = gtk_event_controller_motion_new();

    p->area = gtk_drawing_area_new();
    (void)port;
    p->pressed = p->hover = -1;
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(p->area), draw, p, NULL);
    gtk_widget_set_halign(p->area, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(p->area, GTK_ALIGN_CENTER);
    /* Not focusable: clicking a key must not steal focus. */
    gtk_widget_set_focusable(p->area, FALSE);
    gtk_widget_set_has_tooltip(p->area, TRUE);
    g_signal_connect(p->area, "query-tooltip", G_CALLBACK(on_query_tooltip), p);

    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), p);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), p);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), p);
    g_signal_connect(drag, "cancel", G_CALLBACK(on_cancel), p);
    gtk_widget_add_controller(p->area, GTK_EVENT_CONTROLLER(drag));
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), p);
    g_signal_connect(motion, "leave", G_CALLBACK(on_leave), p);
    gtk_widget_add_controller(p->area, motion);
}

/* ---- building the controls ------------------------------------------------------------ */

static Item *pane_add(Pane *p, int shape, int kind, int target, const char *label)
{
    Item *it;
    if (p->n >= MAX_ITEMS) return NULL;
    it = &p->items[p->n++];
    memset(it, 0, sizeof *it);
    it->shape = shape;
    it->kind = kind;
    it->target = target;
    it->label = label;
    return it;
}

static int machine_now(void)
{
    int m = chfsession_get_int(g_session, "machine", CHF_MACHINE_ARCADIA);
    return m >= 0 && m < CHF_MACHINE_COUNT ? m : CHF_MACHINE_ARCADIA;
}

/* The console: the layout table's keypads and panel, then under each keypad
 * its controller's disc, with the FIRE button on the outer side. */
static void build_console(void)
{
    static const double angles[CHF_ACT_FIRE] = { -G_PI / 2, 0, G_PI / 2, G_PI };
    Pane *p = &g_pane[PANE_CONSOLE];
    float kw, kh;
    int n, i, port, act;
    const chf_keycap *caps = chfsession_keyboard_layout(g_built_machine, &n, &kw, &kh);
    const double x0 = CASE_PAD + FIRE_W + FIRE_GAP;
    const double y0 = CASE_PAD + WOOD_H + 12;
    const double ytop = y0 + kh * UNIT + 14;
    double lo[2] = { 1e9, 1e9 }, hi[2] = { -1e9, -1e9 };
    Item *it;

    p->n = 0;
    p->pressed = p->hover = -1;
    for (i = 0; i < n; i++) {
        const chf_keycap *c = &caps[i];
        it = pane_add(p, SH_RECT, c->kind,
                      c->target >= 0 ? CHF_TARGET_KEY(c->target)
                                     : CHF_TARGET_SWITCH(CHF_SW_RESET), c->label);
        if (!it) break;
        it->x = x0 + c->x * UNIT;
        it->y = y0 + c->y * UNIT;
        it->w = c->w * UNIT;
        it->h = c->h * UNIT;
        if (c->port >= 0 && c->port < 2) {
            lo[c->port] = MIN(lo[c->port], it->x);
            hi[c->port] = MAX(hi[c->port], it->x + it->w);
        }
    }
    for (port = 0; port < 2; port++) {
        const double cx = (lo[port] + hi[port]) / 2, cy = ytop + DISC_R1 + 5;
        it = pane_add(p, SH_WEDGE, K_DISC, -1, "");
        it->port = port;
        it->x = cx; it->y = cy; it->w = DISC_R1;
        for (act = CHF_ACT_UP; act < CHF_ACT_FIRE; act++) {
            it = pane_add(p, SH_WEDGE, K_PAD, CHF_TARGET_PORT(port, act), "");
            it->port = port;
            it->a = angles[act];
            it->x = cx; it->y = cy; it->w = DISC_R0 + 4; it->h = DISC_R1;
        }
        it = pane_add(p, SH_RECT, K_FIRE, CHF_TARGET_PORT(port, CHF_ACT_FIRE), "FIRE");
        it->port = port;
        it->w = FIRE_W; it->h = FIRE_H;
        it->x = port == 0 ? lo[0] - FIRE_GAP - FIRE_W : hi[1] + FIRE_GAP;
        it->y = cy - FIRE_H / 2;
    }
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(p->area),
                                       (int)(kw * UNIT + 2 * x0));
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(p->area),
                                        (int)(ytop + 2 * DISC_R1 + 10 + CASE_PAD));
}

static void build_strip(void)
{
    Pane *p = &g_pane[PANE_STRIP];
    const int bw = STRIP_BW;
    Item *it;

    p->n = 0;
    p->pressed = p->hover = -1;
    it = pane_add(p, SH_RECT, K_BUTTON, CHF_TARGET_SYSACT(CHF_SYSACT_RESET_CONFIG),
                  "Reset to CONFIG");
    it->x = 0; it->y = 0; it->w = bw; it->h = STRIP_H;
    it = pane_add(p, SH_RECT, K_BUTTON, CHF_TARGET_SYSACT(CHF_SYSACT_DEBUG_STOP),
                  "Stop in Debugger");
    it->x = bw + STRIP_GAP; it->y = 0; it->w = bw; it->h = STRIP_H;
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(p->area), 2 * bw + STRIP_GAP);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(p->area), STRIP_H);
}

static void rebuild(void)
{
    mouse_release();
    g_built_machine = machine_now();
    build_console();
    build_strip();
    adw_window_title_set_subtitle(g_title, chf_machine_name(g_built_machine));
    refresh_all();
}

static void refresh_all(void)
{
    int i;
    for (i = 0; i < PANE_COUNT; i++)
        if (g_pane[i].area)
            gtk_widget_queue_draw(g_pane[i].area);
}

/* What the console sees held, painted in the accent. Redraws only what
 * changed (the disc knob too: the analog deflection moves it with no pad
 * changing). */
static gboolean held_tick(gpointer d)
{
    unsigned held[2];
    int i, k, port;
    gboolean knob = FALSE;
    (void)d;
    if (!g_window || !gtk_widget_get_visible(g_window))
        return G_SOURCE_CONTINUE;
    held[0] = chfsession_buttons_held(g_session, 0);
    held[1] = chfsession_buttons_held(g_session, 1);
    for (port = 0; port < 2; port++) {
        int ax, ay;
        chfsession_disc_get(g_session, port, &ax, &ay);
        if (ax != g_last_disc[port][0] || ay != g_last_disc[port][1]) {
            g_last_disc[port][0] = ax;
            g_last_disc[port][1] = ay;
            knob = TRUE;
        }
    }
    for (i = 0; i < PANE_COUNT; i++) {
        Pane *p = &g_pane[i];
        gboolean changed = i == PANE_CONSOLE && knob;
        for (k = 0; k < p->n; k++) {
            gboolean on = item_held(&p->items[k], held);
            if (on != p->items[k].lit) {
                p->items[k].lit = on;
                changed = TRUE;
            }
        }
        if (changed)
            gtk_widget_queue_draw(p->area);
    }
    return G_SOURCE_CONTINUE;
}

/* ---- gamepads ------------------------------------------------------------- */

static void refresh_pad_ports(void)
{
    int i;
    for (i = 0; i < g_npad_rows; i++) {
        int effective = chfsession_gamepad_effective_port(g_session, i);
        char text[32];
        if (effective >= 0) g_snprintf(text, sizeof text, "drives player %d", effective + 1);
        else g_snprintf(text, sizeof text, "drives nothing");
        if (strcmp(gtk_label_get_text(GTK_LABEL(g_pad_port[i])), text) != 0)
            gtk_label_set_text(GTK_LABEL(g_pad_port[i]), text);
    }
}

static void on_assign_changed(GObject *o, GParamSpec *ps, gpointer user_data)
{
    int idx = GPOINTER_TO_INT(user_data);
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(o));
    (void)ps;
    /* 0 = automatic (connection order), 1-2 = that player */
    chfsession_gamepad_assign(g_session, idx, sel == 0 ? -1 : (int)sel - 1);
    refresh_pad_ports();
}

/* Rebuilt whenever a pad comes or goes (the session's generation counter):
 * one row per pad, its name, the player it drives and the player it is set to. */
static void refresh_pads(void)
{
    static const char *const choices[] = { "Automatic", "Player 1", "Player 2", NULL };
    GtkWidget *child;
    int n, i;

    while ((child = gtk_widget_get_first_child(g_pad_box)) != NULL)
        gtk_box_remove(GTK_BOX(g_pad_box), child);
    g_npad_rows = 0;

    n = chfsession_gamepad_count(g_session);
    if (n == 0) {
        GtkWidget *none = gtk_label_new("No gamepads connected");
        gtk_widget_add_css_class(none, "dim-label");
        gtk_box_append(GTK_BOX(g_pad_box), none);
        return;
    }
    for (i = 0; i < n && i < MAX_PADS; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *name, *dd, *eff;
        char text[128];
        int assign = chfsession_gamepad_assignment(g_session, i);

        chfsession_gamepad_name(g_session, i, text, sizeof text);
        name = gtk_label_new(text);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(name), 1);
        gtk_widget_set_tooltip_text(name, text);
        dd = gtk_drop_down_new_from_strings(choices);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(dd),
                                   assign < 0 || assign > 1 ? 0 : (guint)assign + 1);
        gtk_widget_set_focus_on_click(dd, FALSE);
        g_signal_connect(dd, "notify::selected", G_CALLBACK(on_assign_changed), GINT_TO_POINTER(i));
        eff = gtk_label_new("");
        gtk_widget_add_css_class(eff, "dim-label");
        gtk_box_append(GTK_BOX(row), name);
        gtk_box_append(GTK_BOX(row), eff);
        gtk_box_append(GTK_BOX(row), dd);
        gtk_box_append(GTK_BOX(g_pad_box), row);
        g_pad_port[g_npad_rows++] = eff;
    }
    refresh_pad_ports();
}

/* Hot-plugged pads, auto-assignment changes, and the machine changed in
 * Preferences. */
static gboolean pad_tick(gpointer d)
{
    unsigned gen;
    (void)d;
    if (!g_window || !gtk_widget_get_visible(g_window)) return G_SOURCE_CONTINUE;
    gen = chfsession_gamepad_generation(g_session);
    if (gen != g_pad_generation) {
        g_pad_generation = gen;
        refresh_pads();
    } else {
        refresh_pad_ports();
    }
    if (machine_now() != g_built_machine)
        rebuild();
    return G_SOURCE_CONTINUE;
}

/* ---- keyboard ------------------------------------------------------------- */

static gboolean on_key(guint keyval, guint code, GdkModifierType st, gboolean down)
{
    /* an open drop-down (a gamepad's stick) has the arrows and Return */
    GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(g_window));
    if (focus && gtk_widget_get_ancestor(focus, GTK_TYPE_POPOVER))
        return FALSE;

    if (g_swallow_code && code == g_swallow_code) {
        if (!down) g_swallow_code = 0;
        return TRUE;
    }
    /* The window's own keys and the shortcuts are never Map's. */
    if (g_map_state != -2 && keyval != GDK_KEY_F9 && keyval != GDK_KEY_F11 &&
        keyval != GDK_KEY_F12 && !(st & GDK_CONTROL_MASK)) {
        guint32 keysym;
        char stolen[128], name[32];
        if (!down) return TRUE;
        if (keyval == GDK_KEY_Escape) {
            g_swallow_code = code;
            set_map_state(g_map_state >= 0 ? -1 : -2);
            return TRUE;
        }
        if (g_map_state < 0)
            return TRUE;          /* armed, no target yet: typing is not typed */
        keysym = code >= 8 ? chfsession_keysym_from_evdev(code - 8) : 0;
        if (!keysym) keysym = keyval;
        chfsession_binding_set_key(g_session, g_map_state, keysym, stolen, sizeof stolen);
        chfsession_keysym_name(keysym, name, sizeof name);
        g_swallow_code = code;
        set_map_state(-1);        /* stay armed: remapping several in a row is normal */
        say_bound(name, stolen);
        return TRUE;
    }
    /* Otherwise exactly what the main window does with it. */
    if (!g_parent) return FALSE;
    return chf_window_key(CHF_WINDOW(g_parent), keyval, code, st, down);
}

static gboolean on_key_pressed(GtkEventControllerKey *c, guint keyval,
                               guint code, GdkModifierType st, gpointer d)
{
    (void)c; (void)d;
    return on_key(keyval, code, st, TRUE);
}

static void on_key_released(GtkEventControllerKey *c, guint keyval,
                            guint code, GdkModifierType st, gpointer d)
{
    (void)c; (void)d;
    on_key(keyval, code, st, FALSE);
}

/* Keys typed here went through the main window, so a key held into a focus
 * change must be let go there too (and the mouse's, here). */
static void on_focus_leave(GtkEventControllerFocus *c, gpointer d)
{
    (void)c; (void)d;
    mouse_release();
    if (g_parent)
        chf_window_release_keys(CHF_WINDOW(g_parent));
    g_swallow_code = 0;     /* its release went elsewhere */
}

/* ---- the window ----------------------------------------------------------- */

static void on_map_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    set_map_state(g_map_state == -2 ? -1 : -2);
}

static void on_defaults_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    chfsession_bindings_reset(g_session);
    gtk_label_set_text(GTK_LABEL(g_map_hint), "Bindings back to their defaults");
    refresh_all();
}

static void on_show_bindings(GtkCheckButton *b, gpointer d)
{
    (void)d;
    chfsession_set_int(g_session, "controllers_show_bindings",
                      gtk_check_button_get_active(b) ? 1 : 0);
    refresh_all();
}

static void hide_window(void)
{
    set_map_state(-2);
    mouse_release();
    g_swallow_code = 0;
    gtk_widget_set_visible(g_window, FALSE);
}

static gboolean on_close(GtkWindow *w, gpointer d)
{
    (void)w; (void)d;
    hide_window();
    return TRUE;
}

/* The refresh timers run only while the window is on screen: closing only
 * hides it, and a hidden window has nobody to update. */
static void on_window_map(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (!g_held_timer) g_held_timer = g_timeout_add(33, held_tick, NULL);
    if (!g_pad_timer) g_pad_timer = g_timeout_add(250, pad_tick, NULL);
}

static void on_window_unmap(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (g_held_timer) g_source_remove(g_held_timer);
    if (g_pad_timer) g_source_remove(g_pad_timer);
    g_held_timer = g_pad_timer = 0;
}

static void on_destroy(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (g_capture_timer) g_source_remove(g_capture_timer);
    if (g_held_timer) g_source_remove(g_held_timer);
    if (g_pad_timer) g_source_remove(g_pad_timer);
    g_capture_timer = g_held_timer = g_pad_timer = 0;
    memset(g_pane, 0, sizeof g_pane);
    g_window = NULL;
    g_npad_rows = 0;
    g_built_machine = -1;
    g_map_state = -2;
}


static void build_window(GtkWindow *parent)
{
    GtkWidget *root, *maprow, *toolbar, *header, *defaults, *padtitle;
    GtkEventController *keys, *focus;

    chf_install_accent_css();
    g_window = adw_window_new();
    gtk_window_set_title(GTK_WINDOW(g_window), "Controllers");
    if (parent) {
        gtk_window_set_transient_for(GTK_WINDOW(g_window), parent);
        /* Same app-id as the main window, so the shell groups and icons it. */
        gtk_window_set_application(GTK_WINDOW(g_window), gtk_window_get_application(parent));
    }
    gtk_window_set_destroy_with_parent(GTK_WINDOW(g_window), TRUE);
    /* min == max: Wayland compositors float it */
    gtk_window_set_resizable(GTK_WINDOW(g_window), FALSE);
    g_signal_connect(g_window, "close-request", G_CALLBACK(on_close), NULL);
    g_signal_connect(g_window, "destroy", G_CALLBACK(on_destroy), NULL);

    memset(g_pane, 0, sizeof g_pane);
    pane_init(&g_pane[PANE_CONSOLE], 0, draw_console);
    pane_init(&g_pane[PANE_STRIP], 0, draw_strip);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);

    gtk_box_append(GTK_BOX(root), g_pane[PANE_CONSOLE].area);
    gtk_box_append(GTK_BOX(root), g_pane[PANE_STRIP].area);

    padtitle = gtk_label_new("Gamepads");
    gtk_widget_add_css_class(padtitle, "heading");
    gtk_box_append(GTK_BOX(root), padtitle);
    g_pad_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(root), g_pad_box);

    maprow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_map_button = gtk_button_new_with_label("Map");
    gtk_widget_set_size_request(g_map_button, 72, -1);
    gtk_widget_set_focus_on_click(g_map_button, FALSE);
    g_signal_connect(g_map_button, "clicked", G_CALLBACK(on_map_clicked), NULL);
    defaults = gtk_button_new_with_label("Defaults");
    gtk_widget_set_focus_on_click(defaults, FALSE);
    g_signal_connect(defaults, "clicked", G_CALLBACK(on_defaults_clicked), NULL);
    g_show_bindings = gtk_check_button_new_with_label("Show bindings");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(g_show_bindings),
        chfsession_get_int(g_session, "controllers_show_bindings", 0) != 0);
    gtk_widget_set_focus_on_click(g_show_bindings, FALSE);
    g_signal_connect(g_show_bindings, "toggled", G_CALLBACK(on_show_bindings), NULL);
    g_map_hint = gtk_label_new("");
    gtk_widget_add_css_class(g_map_hint, "dim-label");
    gtk_widget_set_hexpand(g_map_hint, TRUE);
    gtk_label_set_xalign(GTK_LABEL(g_map_hint), 0);
    /* a long hint ("Press a key or a gamepad button for Player 1: Right")
     * must not widen the window */
    gtk_label_set_ellipsize(GTK_LABEL(g_map_hint), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(g_map_hint), 1);
    gtk_box_append(GTK_BOX(maprow), g_map_button);
    gtk_box_append(GTK_BOX(maprow), defaults);
    gtk_box_append(GTK_BOX(maprow), g_show_bindings);
    gtk_box_append(GTK_BOX(root), maprow);
    gtk_box_append(GTK_BOX(root), g_map_hint);

    header = adw_header_bar_new();
    g_title = ADW_WINDOW_TITLE(adw_window_title_new("Controllers", ""));
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), GTK_WIDGET(g_title));
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), root);
    adw_window_set_content(ADW_WINDOW(g_window), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), NULL);
    gtk_widget_add_controller(g_window, keys);

    focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), NULL);
    gtk_widget_add_controller(g_window, focus);

    g_built_machine = -1;
    set_map_state(-2);
    rebuild();
    g_signal_connect(g_window, "map", G_CALLBACK(on_window_map), NULL);
    g_signal_connect(g_window, "unmap", G_CALLBACK(on_window_unmap), NULL);
}

static void show_window(void)
{
    if (machine_now() != g_built_machine)
        rebuild();
    g_pad_generation = chfsession_gamepad_generation(g_session);
    refresh_pads();
    gtk_window_present(GTK_WINDOW(g_window));
}

void chf_controllers_toggle(GtkWindow *parent, chfsession *session)
{
    g_session = session;
    if (parent)
        g_parent = parent;
    if (!g_window)
        build_window(g_parent);

    if (gtk_widget_get_visible(g_window))
        hide_window();
    else
        show_window();
}

gboolean chf_controllers_visible(void)
{
    return g_window && gtk_widget_get_visible(g_window);
}

