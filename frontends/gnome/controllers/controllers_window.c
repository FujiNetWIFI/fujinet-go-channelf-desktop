/*
 * The Controllers window -- the Channel F console drawn as a player sees it: a
 * charcoal case with its walnut strip, the console's panel across the top
 * (RESET, 1 TIME, 2 MODE, 3 HOLD, 4 START) and the two hand controllers below
 * it, the left one Player 2 and the right one Player 1. Everything is drawn
 * from the session's layout (chfsession_controls_layout), so every frontend
 * shows the same console.
 *
 * Each controller is one knob with four axes, drawn as two views of it. From
 * above: a ring of eight direction wedges round the knob cap, which shifts
 * toward the held direction, turns a little when twisted, shrinks when pushed
 * and grows when pulled, with the twist arrows either side. From the side: the
 * handle with PULL above and PUSH below and the knob at its height.
 *
 * Mouse: a drag on the stick tilts it eight ways (chfsession_stick_from_vector
 * -> chfsession_pointer_set), a left-press on the cap pushes it, a right-press
 * pulls it, the wheel twists it (chfsession_twist_pulse), and a press on a
 * wedge, a twist or PULL/PUSH arrow or a console button holds it. Every press
 * is the raw press and release (a GtkGestureDrag, not a click: a click gives up
 * when the pointer wobbles), because the console samples its controls once per
 * frame -- a press present only for the instant of a click falls between
 * frames. Losing the focus, hiding and the pointer leaving a held arrow let
 * go of whatever the mouse holds here.
 *
 * What the console sees held -- keyboard, gamepads and this window together
 * (chfsession_buttons_held, key_held, switch_held) -- is polled every frame and
 * painted in the accent, so the window doubles as a gamepad tester.
 *
 * Keys typed with the window active go through the main window
 * (chf_window_key), so the console, F9 and the shortcuts behave the same
 * whichever window has the focus.
 *
 * FIXED SIZE, FLOATING: the console is a drawing area of one size, the labels
 * ellipsize rather than grow, and the window is not resizable, so neither Map
 * mode nor a long gamepad name changes its size and tiling compositors float
 * it. It is hidden at startup (F9 or the menu shows it).
 *
 * SINGLETON, hidden rather than destroyed, so a remap survives closing it.
 * Its timer runs only while it is on screen.
 *
 * MAP MODE: the Map button arms a two-step rebind -- click a control to pick
 * the target, then press a keyboard key OR a gamepad button. While armed the
 * same gestures pick the target instead of pressing, every control shows the
 * key bound to it, and typed keys are not typed into the console; once a
 * target is picked the other controls are greyed. Rebinding steals the key
 * from whatever held it. Escape cancels. Clear takes the picked target's key
 * and button away, Defaults puts every binding back.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controllers_window.h"

#include "../help/help_window.h"
#include "../window.h"

#include <math.h>
#include <pango/pangocairo.h>
#include <string.h>

#define U 44.0                  /* pixels per layout unit (a console button) */
#define CASE_PAD 18             /* the charcoal case around everything */
#define WOOD_H 22               /* the walnut strip along the top */
#define TOP_GAP 12
#define FOOT 24                 /* the views' captions under the controllers */
#define STRIP_BW 214
#define STRIP_GAP 12
#define STRIP_H 34
#define MAX_PARTS 64

enum { PANE_CONSOLE, PANE_STRIP, PANE_COUNT };
enum { K_BUTTON = 100 };        /* the strip's buttons */

typedef struct {
    double x, y, w, h;
    int kind;                   /* K_BUTTON */
    int target;
    const char *label;
} StripItem;

typedef struct {
    GtkWidget *area;
    StripItem items[2];
    int n, pressed, hover;
} Strip;

/* ---- singleton state ------------------------------------------------------ */

static GtkWidget *g_window;
static GtkWindow *g_parent;     /* the ChfWindow: typed keys go through it */
static chfsession *g_session;
static AdwWindowTitle *g_title;
static GtkWidget *g_console;    /* the console's drawing area */
static Strip g_strip;

static const chf_control *g_layout;     /* chfsession_controls_layout: static */
static int g_n_parts;
static float g_lw, g_lh;
static gboolean g_lit[MAX_PARTS];       /* what the console sees held */
static int g_hover = -1;                /* the part under the pointer */
static int g_pressed = -1;              /* a console button the mouse holds */
static int g_machine = -1;              /* what the title shows */

/* The knob the mouse is dragging, and where its stick is. */
static struct {
    gboolean active;
    int port;                   /* 0 player 1, 1 player 2 */
    double cx, cy, r;           /* the stick disc, pixels */
    unsigned extra;             /* PUSH (left-press on the cap) or PULL (right) */
} g_drag;
static int g_arrow_port = -1;           /* port of a held twist/axis arrow */
static double g_ptr_x, g_ptr_y;         /* the pointer, for the wheel */
static gboolean g_ptr_in;
static double g_scroll_acc;

/* each knob as it is painted: eased toward what is held */
static struct {
    double tx, ty;              /* tilt, -1..1 (+y toward the player) */
    double twist;               /* -1 counter-clockwise .. 1 clockwise */
    double height;              /* -1 pushed .. 1 pulled */
} g_anim[2];

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 waiting for a
 * key or pad button to bind to that target. */
static int g_map_state = -2;
static GtkWidget *g_map_button, *g_clear_button;
static GtkWidget *g_map_hint;
static GtkWidget *g_show_bindings;
#define MAX_PADS    8           /* pads listed (SDL opens few more) */
#define PAD_ROWS    3           /* pad rows the window reserves; more scroll */
#define PAD_ROW_H   36          /* one row's height, px */
#define PAD_GAP     4
static GtkWidget *g_pad_box;
static GtkWidget *g_pad_eff[MAX_PADS];  /* each pad's "drives player N" */
static int g_npad_rows;
static unsigned g_pad_generation = (unsigned)-1;
static guint g_capture_timer;
static guint g_swallow_code;    /* the key Map took: its repeats and release too */
static guint g_info_timer;

static void refresh_all(void);

/* "controllers_show_bindings"; the older spellings are read when it is absent. */
static gboolean show_bindings_saved(void)
{
    static const char *const old[] = { "controllers_bindings", "controllers_key_names",
                                       "show_bindings" };
    int val = chfsession_get_int(g_session, "controllers_show_bindings", -1);
    size_t i;
    for (i = 0; val < 0 && i < G_N_ELEMENTS(old); i++)
        val = chfsession_get_int(g_session, old[i], -1);
    return val > 0;
}

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

/* A host key's name as a small legend: the long ones shortened to fit. */
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

static gboolean showing_bindings(void)
{
    return g_map_state != -2 ||
           gtk_check_button_get_active(GTK_CHECK_BUTTON(g_show_bindings));
}

static int machine_now(void)
{
    int m = chfsession_get_int(g_session, "machine", CHF_MACHINE_CHANNELF);
    return m >= 0 && m < CHF_MACHINE_COUNT ? m : CHF_MACHINE_CHANNELF;
}

/* The binding legend for a target, or "" when none or not shown. */
static void bound_text(int target, char *dst, int dstsz)
{
    chf_binding b;
    dst[0] = '\0';
    if (target < 0 || !showing_bindings()) return;
    b = chfsession_binding_get(g_session, target);
    if (b.keysym)
        legend_name(b.keysym, dst, dstsz);
    else
        g_snprintf(dst, dstsz, "\xe2\x80\x94");
}

/* ---- geometry ---------------------------------------------------------------- */

#define OX (CASE_PAD)
#define OY (CASE_PAD + WOOD_H + TOP_GAP)

static double px_x(double ux) { return OX + ux * U; }
static double px_y(double uy) { return OY + uy * U; }

static int console_width(void)
{
    return (int)(2 * CASE_PAD + g_lw * U + 0.5);
}

static int console_height(void)
{
    return (int)(OY + g_lh * U + FOOT + CASE_PAD + 0.5);
}

/* The stick disc (centre, radius in pixels) of a controller's port, or FALSE. */
static gboolean stick_of(int port, double *cx, double *cy, double *r)
{
    int i;
    for (i = 0; i < g_n_parts; i++)
        if (g_layout[i].kind == CHF_CTL_STICK && g_layout[i].port == port) {
            *cx = px_x(g_layout[i].x + g_layout[i].w / 2);
            *cy = px_y(g_layout[i].y + g_layout[i].h / 2);
            *r = g_layout[i].w * U / 2;
            return TRUE;
        }
    return FALSE;
}

/* ---- what is held --------------------------------------------------------------- */

static gboolean part_held(const chf_control *c, const unsigned held[2])
{
    switch (c->kind) {
    case CHF_CTL_BUTTON:
        return chfsession_key_held(g_session, c->target - CHF_TARGET_KEY(0)) != 0;
    case CHF_CTL_RESET:
        return chfsession_switch_held(g_session, CHF_SW_RESET) != 0;
    case CHF_CTL_DIRECTION:
        /* the stick's wedge that is exactly the direction held: a diagonal
         * lights its own wedge, not the two beside it */
        return c->act_mask && (held[c->port] & 0x0Fu) == c->act_mask;
    case CHF_CTL_TWIST:
    case CHF_CTL_AXIS:
        return (held[c->port] & c->act_mask) != 0;
    default:
        return FALSE;
    }
}

/* The knob's eased state toward what is held. Returns whether it moved. */
static gboolean ease_knob(int port, unsigned held)
{
    double tx = 0, ty = 0, tw = 0, ht = 0;
    gboolean moved = FALSE;
    double *cur[4] = { &g_anim[port].tx, &g_anim[port].ty, &g_anim[port].twist,
                       &g_anim[port].height };
    double tgt[4];
    int i;

    /* opposite contacts held together cancel */
    tx = (int)((held >> CHF_ACT_RIGHT) & 1) - (int)((held >> CHF_ACT_LEFT) & 1);
    ty = (int)((held >> CHF_ACT_BACK) & 1) - (int)((held >> CHF_ACT_FORWARD) & 1);
    if (tx != 0 && ty != 0) { tx *= 0.7071; ty *= 0.7071; }
    tw = (int)((held >> CHF_ACT_TWIST_CW) & 1) - (int)((held >> CHF_ACT_TWIST_CCW) & 1);
    ht = (int)((held >> CHF_ACT_PULL) & 1) - (int)((held >> CHF_ACT_PUSH) & 1);
    tgt[0] = tx; tgt[1] = ty; tgt[2] = tw; tgt[3] = ht;
    for (i = 0; i < 4; i++) {
        const double d = tgt[i] - *cur[i];
        if (fabs(d) < 0.004) {
            if (d != 0) { *cur[i] = tgt[i]; moved = TRUE; }
            continue;
        }
        *cur[i] += d * 0.4;
        moved = TRUE;
    }
    return moved;
}

/* ---- drawing ------------------------------------------------------------------- */

#define COL_CASE_TOP 0x3A3A3D
#define COL_CASE_BOT 0x1F1F21
#define COL_CASE_EDGE 0x0B0B0C
#define COL_BLOCK 0x2A2A2D
#define COL_PART 0x3C3C41
#define COL_PART_HOT 0x54545B
#define COL_PART_SIDE 0x161618
#define COL_INK 0xE4E0D6
#define COL_DIM_INK 0x9A968C
#define COL_ACCENT CHFSESSION_ACCENT_RGB
#define COL_DARK_INK 0x1E1A14
#define COL_RESET_CAP 0x9B2B23
#define COL_RESET_SIDE 0x4F1410

typedef struct {
    gboolean held, selected, dim, hot;
} PartState;

static PartState part_state(int i)
{
    const chf_control *c = &g_layout[i];
    PartState st;
    const gboolean armed = g_map_state != -2;
    st.held = g_lit[i] || g_pressed == i;
    st.selected = g_map_state >= 0 && c->target >= 0 && g_map_state == c->target;
    st.dim = armed && (c->target < 0 || (g_map_state >= 0 && !st.selected));
    st.hot = g_hover == i && !st.held;
    return st;
}

static void rgba_rgb(cairo_pattern_t *g, double off, guint32 c)
{
    cairo_pattern_add_color_stop_rgb(g, off, ((c >> 16) & 0xFF) / 255.0,
                                     ((c >> 8) & 0xFF) / 255.0, (c & 0xFF) / 255.0);
}

/* The walnut strip along the top of the case: a brown gradient with a few
 * wandering grain lines. */
static void draw_wood(cairo_t *cr, double x, double y, double w, double h)
{
    cairo_pattern_t *g = cairo_pattern_create_linear(0, y, 0, y + h);
    int i;
    rgba_rgb(g, 0, 0x6E4C39);
    rgba_rgb(g, 0.5, 0x58402F);
    rgba_rgb(g, 1, 0x47332A);
    rrect(cr, x, y, w, h, 4);
    cairo_set_source(cr, g);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(g);
    cairo_save(cr);
    cairo_clip(cr);
    cairo_set_line_width(cr, 0.8);
    for (i = 0; i < 7; i++) {
        double yy = y + 2.5 + i * (h - 5) / 6.0, xx;
        rgb(cr, i % 2 ? 0x3A2A21 : 0x7C5A44);
        cairo_move_to(cr, x, yy);
        for (xx = x; xx <= x + w; xx += 8)
            cairo_line_to(cr, xx, yy + 1.4 * sin(xx * 0.045 + i * 1.7));
        cairo_stroke(cr);
    }
    cairo_restore(cr);
    rrect(cr, x, y, w, h, 4);
    rgb(cr, 0x241812);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
}

/* A console button: a cap with its number, the legend under it. RESET has no
 * number on its cap; its name sits under it in the accent. */
static void draw_button(cairo_t *cr, GtkWidget *w, int i)
{
    const chf_control *c = &g_layout[i];
    const PartState st = part_state(i);
    const double x = px_x(c->x), y = px_y(c->y), cw = c->w * U, ch = c->h * U - 3;
    const gboolean reset = c->kind == CHF_CTL_RESET;
    guint32 face = reset ? COL_RESET_CAP : COL_PART, side = reset ? COL_RESET_SIDE : COL_PART_SIDE;
    guint32 ink = COL_INK;
    char bound[40];
    double lh = ch;

    if (st.held || st.selected) {
        face = COL_ACCENT; side = mix(COL_ACCENT, 0x000000, 0.45); ink = COL_DARK_INK;
    } else if (st.hot) {
        face = mix(face, 0xFFFFFF, 0.16);
    }
    if (st.dim) cairo_push_group(cr);
    rrect(cr, x, y + 3, cw, ch, 7);
    rgb(cr, side);
    cairo_fill(cr);
    rrect(cr, x, y, cw, ch, 7);
    rgb(cr, face);
    cairo_fill_preserve(cr);
    rgb(cr, st.held || st.selected ? 0xFFFFFF : mix(face, 0x000000, 0.5));
    cairo_set_line_width(cr, st.held || st.selected ? 2 : 1);
    cairo_stroke(cr);

    bound_text(c->target, bound, sizeof bound);
    if (bound[0]) {
        lh = ch * 0.62;
        legend(cr, w, bound, 9, FALSE, mix(ink, face, 0.3), x, y + lh - 2, cw, ch - lh);
    }
    if (!reset)
        legend(cr, w, c->label, 18, TRUE, ink, x, y, cw, lh);
    /* the legend under the cap */
    legend(cr, w, reset ? c->label : c->legend, 11, TRUE,
           reset ? COL_ACCENT : COL_INK, x - 8, y + ch + 6, cw + 16, 16);
    if (st.dim) {
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, 0.4);
    }
}

/* The angle (screen, y down) of a wedge from its contacts. */
static double wedge_angle(unsigned acts)
{
    const double dx = (int)((acts >> CHF_ACT_RIGHT) & 1) - (int)((acts >> CHF_ACT_LEFT) & 1);
    const double up = (int)((acts >> CHF_ACT_FORWARD) & 1) - (int)((acts >> CHF_ACT_BACK) & 1);
    return atan2(-up, dx);
}

/* One of the eight wedges: the sector of the stick disc between half its
 * radius and all of it. */
static void draw_wedge(cairo_t *cr, GtkWidget *w, int i, double cx, double cy, double r)
{
    const chf_control *c = &g_layout[i];
    const PartState st = part_state(i);
    const double a = wedge_angle(c->act_mask), span = 22.0 * G_PI / 180.0;
    const double r0 = r * 0.5 + 2, r1 = r - 1;
    const double mid = (r0 + r1) / 2;
    const double gx = cx + cos(a) * mid, gy = cy + sin(a) * mid;
    guint32 face = st.held || st.selected ? COL_ACCENT : st.hot ? COL_PART_HOT : COL_PART;
    guint32 ink = st.held || st.selected ? COL_DARK_INK : COL_INK;
    char bound[40];

    if (st.dim) cairo_push_group(cr);
    cairo_arc(cr, cx, cy, r1, a - span, a + span);
    cairo_arc_negative(cr, cx, cy, r0, a + span, a - span);
    cairo_close_path(cr);
    rgb(cr, face);
    cairo_fill_preserve(cr);
    rgb(cr, st.held || st.selected ? 0xFFFFFF : mix(face, 0x000000, 0.5));
    cairo_set_line_width(cr, st.held || st.selected ? 2 : 1);
    cairo_stroke(cr);

    bound_text(c->target, bound, sizeof bound);
    if (bound[0]) {
        legend(cr, w, bound, 10, TRUE, ink, gx - 22, gy - 9, 44, 18);
    } else {
        /* an arrow pointing the way the stick tilts */
        const double ux = cos(a), uy = sin(a);
        cairo_move_to(cr, gx + ux * 8, gy + uy * 8);
        cairo_line_to(cr, gx - ux * 5 - uy * 7, gy - uy * 5 + ux * 7);
        cairo_line_to(cr, gx - ux * 5 + uy * 7, gy - uy * 5 - ux * 7);
        cairo_close_path(cr);
        rgb(cr, ink);
        cairo_fill(cr);
    }
    if (st.dim) {
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, 0.4);
    }
}

/* The disc's plate, under the wedges. */
static void draw_plate(cairo_t *cr, double cx, double cy, double r)
{
    cairo_arc(cr, cx, cy, r + 5, 0, 2 * G_PI);
    rgb(cr, 0x0D0D0E);
    cairo_fill(cr);
    cairo_arc(cr, cx, cy, r + 1, 0, 2 * G_PI);
    rgb(cr, 0x18181A);
    cairo_fill(cr);
}

/* The knob cap, over the wedges: shifted toward the tilt, turned by the twist,
 * smaller pushed and bigger pulled. */
static void draw_cap(cairo_t *cr, int port, double cx, double cy, double r, gboolean accent)
{
    const double capr = r * 0.5 - 5;
    const double shift = r * 0.16;
    const double scale = 1.0 + 0.16 * g_anim[port].height;
    const double kx = cx + g_anim[port].tx * shift, ky = cy + g_anim[port].ty * shift;
    cairo_pattern_t *g;
    int i;

    /* its shadow grows when it is pulled up */
    cairo_arc(cr, kx + 2 + 3 * g_anim[port].height, ky + 4 + 3 * g_anim[port].height,
              capr * scale, 0, 2 * G_PI);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
    cairo_fill(cr);

    cairo_save(cr);
    cairo_translate(cr, kx, ky);
    cairo_rotate(cr, g_anim[port].twist * 25.0 * G_PI / 180.0);
    cairo_scale(cr, scale, scale);

    g = cairo_pattern_create_radial(-capr * 0.3, -capr * 0.3, capr * 0.1, 0, 0, capr);
    rgba_rgb(g, 0, 0x6A6A72);
    rgba_rgb(g, 1, 0x26262A);
    cairo_arc(cr, 0, 0, capr, 0, 2 * G_PI);
    cairo_set_source(cr, g);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(g);
    if (accent) rgb(cr, COL_ACCENT); else rgb(cr, 0x0E0E10);
    cairo_set_line_width(cr, accent ? 3 : 1.5);
    cairo_stroke(cr);

    /* grip ridges across it, and a notch at the top so the turn shows */
    cairo_set_line_width(cr, 1.5);
    for (i = -2; i <= 2; i++) {
        const double yy = i * capr * 0.2;
        const double half = sqrt(MAX(0.0, capr * capr * 0.64 - yy * yy));
        rgb(cr, 0x151517);
        cairo_move_to(cr, -half, yy);
        cairo_line_to(cr, half, yy);
        cairo_stroke(cr);
    }
    rgb(cr, COL_ACCENT);
    cairo_set_line_width(cr, 3);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cr, 0, -capr * 0.96);
    cairo_line_to(cr, 0, -capr * 0.72);
    cairo_stroke(cr);
    cairo_restore(cr);
}

/* A curved arrow for the twist: an arc nearly all the way round, the head at
 * its end. cw selects the way it turns. */
static void twist_glyph(cairo_t *cr, double cx, double cy, double r, gboolean cw, guint32 ink)
{
    const double a0 = cw ? -G_PI * 0.75 : -G_PI * 0.25;
    const double span = G_PI * 1.35;
    const double a1 = cw ? a0 + span : a0 - span;
    const double hx = cx + r * cos(a1), hy = cy + r * sin(a1);
    const double tan_a = a1 + (cw ? G_PI / 2 : -G_PI / 2);      /* travel direction */
    const double tx = cos(tan_a), ty = sin(tan_a), nx = -ty, ny = tx;

    rgb(cr, ink);
    cairo_set_line_width(cr, 3);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    if (cw) cairo_arc(cr, cx, cy, r, a0, a1);
    else cairo_arc_negative(cr, cx, cy, r, a0, a1);
    cairo_stroke(cr);
    cairo_move_to(cr, hx + tx * 8, hy + ty * 8);
    cairo_line_to(cr, hx - tx * 2 + nx * 7, hy - ty * 2 + ny * 7);
    cairo_line_to(cr, hx - tx * 2 - nx * 7, hy - ty * 2 - ny * 7);
    cairo_close_path(cr);
    cairo_fill(cr);
}

static void draw_twist(cairo_t *cr, GtkWidget *w, int i)
{
    const chf_control *c = &g_layout[i];
    const PartState st = part_state(i);
    const double x = px_x(c->x), y = px_y(c->y), cw = c->w * U, ch = c->h * U;
    const gboolean clockwise = c->act_mask == (1u << CHF_ACT_TWIST_CW);
    guint32 face = st.held || st.selected ? COL_ACCENT : st.hot ? COL_PART_HOT : COL_PART;
    guint32 ink = st.held || st.selected ? COL_DARK_INK : COL_INK;
    char bound[40];

    if (st.dim) cairo_push_group(cr);
    rrect(cr, x, y + 3, cw, ch - 3, 9);
    rgb(cr, COL_PART_SIDE);
    cairo_fill(cr);
    rrect(cr, x, y, cw, ch - 3, 9);
    rgb(cr, face);
    cairo_fill_preserve(cr);
    rgb(cr, st.held || st.selected ? 0xFFFFFF : mix(face, 0x000000, 0.5));
    cairo_set_line_width(cr, st.held || st.selected ? 2 : 1);
    cairo_stroke(cr);
    twist_glyph(cr, x + cw / 2, y + ch * 0.36, cw * 0.27, clockwise, ink);
    bound_text(c->target, bound, sizeof bound);
    legend(cr, w, bound[0] ? bound : clockwise ? "CW" : "CCW", bound[0] ? 10 : 11, TRUE,
           ink, x, y + ch * 0.64, cw, ch * 0.3);
    if (st.dim) {
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, 0.4);
    }
}

/* PULL above the handle and PUSH below it. */
static void draw_axis(cairo_t *cr, GtkWidget *w, int i)
{
    const chf_control *c = &g_layout[i];
    const PartState st = part_state(i);
    const double x = px_x(c->x), y = px_y(c->y), cw = c->w * U, ch = c->h * U;
    const gboolean pull = c->act_mask == (1u << CHF_ACT_PULL);
    guint32 face = st.held || st.selected ? COL_ACCENT : st.hot ? COL_PART_HOT : COL_PART;
    guint32 ink = st.held || st.selected ? COL_DARK_INK : COL_INK;
    const double ax = x + cw / 2, ay = y + ch * 0.32;
    const double dir = pull ? -1 : 1;
    char bound[40];

    if (st.dim) cairo_push_group(cr);
    rrect(cr, x, y + 3, cw, ch - 3, 9);
    rgb(cr, COL_PART_SIDE);
    cairo_fill(cr);
    rrect(cr, x, y, cw, ch - 3, 9);
    rgb(cr, face);
    cairo_fill_preserve(cr);
    rgb(cr, st.held || st.selected ? 0xFFFFFF : mix(face, 0x000000, 0.5));
    cairo_set_line_width(cr, st.held || st.selected ? 2 : 1);
    cairo_stroke(cr);
    /* an arrow up for PULL, down for PUSH */
    cairo_move_to(cr, ax, ay + dir * 9);
    cairo_line_to(cr, ax - 9, ay - dir * 3);
    cairo_line_to(cr, ax + 9, ay - dir * 3);
    cairo_close_path(cr);
    rgb(cr, ink);
    cairo_fill(cr);
    bound_text(c->target, bound, sizeof bound);
    legend(cr, w, c->label, 11, TRUE, ink, x, y + ch * 0.52, cw, ch * 0.2);
    if (bound[0])
        legend(cr, w, bound, 9, FALSE, ink, x, y + ch * 0.7, cw, ch * 0.2);
    if (st.dim) {
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, 0.4);
    }
}

/* The side view: the handle, with the knob at its height. */
static void draw_side(cairo_t *cr, int i)
{
    const chf_control *c = &g_layout[i];
    const double x = px_x(c->x), y = px_y(c->y), cw = c->w * U, ch = c->h * U;
    const double h = g_anim[c->port].height;
    const double cx = x + cw / 2;
    const double mid = y + ch * 0.46, travel = ch * 0.3;
    const double ky = mid - h * travel;
    int k;

    cairo_save(cr);
    rrect(cr, x, y, cw, ch, 8);
    rgb(cr, 0x151517);
    cairo_fill_preserve(cr);
    rgb(cr, 0x000000);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    rrect(cr, x, y, cw, ch, 8);
    cairo_clip(cr);

    /* the three heights: pulled, resting, pushed */
    rgb(cr, 0x5A5A60);
    cairo_set_line_width(cr, 1);
    for (k = -1; k <= 1; k++) {
        cairo_move_to(cr, x + cw - 16, mid - k * travel);
        cairo_line_to(cr, x + cw - 6, mid - k * travel);
        cairo_stroke(cr);
    }
    /* the base and the shaft */
    rrect(cr, cx - 18, y + ch - 7, 36, 6, 2);
    rgb(cr, 0x55555B);
    cairo_fill(cr);
    rrect(cr, cx - 4, ky, 8, y + ch - 6 - ky, 2);
    rgb(cr, 0x8A8A92);
    cairo_fill(cr);
    /* the knob */
    rrect(cr, cx - 15, ky - 8, 30, 14, 5);
    rgb(cr, h > 0.05 || h < -0.05 ? mix(0x3A3A40, COL_ACCENT, 0.35) : 0x3A3A40);
    cairo_fill_preserve(cr);
    rgb(cr, 0x0E0E10);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
    rgb(cr, COL_ACCENT);
    cairo_set_line_width(cr, 2);
    cairo_move_to(cr, cx - 9, ky - 3);
    cairo_line_to(cr, cx + 9, ky - 3);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void draw_console(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer ud)
{
    GtkWidget *widget = GTK_WIDGET(area);
    cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, h);
    int i;
    (void)ud;

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
    cairo_move_to(cr, 18, 2.5);
    cairo_line_to(cr, w - 18, 2.5);
    rgb(cr, 0x58585C);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);

    draw_wood(cr, CASE_PAD, CASE_PAD, w - 2 * CASE_PAD, WOOD_H);
    legend(cr, widget, "CHANNEL F", 12, TRUE, COL_ACCENT, w / 2.0 - 60, CASE_PAD, 120, WOOD_H);

    if (!g_layout)
        return;

    /* a recessed panel behind each controller, caption to captions */
    for (i = 0; i < g_n_parts; i++) {
        const chf_control *c = &g_layout[i];
        if (c->kind != CHF_CTL_CAPTION)
            continue;
        rrect(cr, px_x(c->x) - 6, px_y(c->y) - 4, c->w * U + 12, (g_lh - c->y) * U + FOOT + 2, 12);
        rgb(cr, COL_BLOCK);
        cairo_fill(cr);
        legend(cr, widget, c->label, 12, TRUE, COL_ACCENT, px_x(c->x) + 6, px_y(c->y), c->w * U - 12, c->h * U);
    }

    /* the plates first, then the parts, then the caps over the wedges */
    for (i = 0; i < g_n_parts; i++) {
        double cx, cy, r;
        if (g_layout[i].kind == CHF_CTL_STICK && stick_of(g_layout[i].port, &cx, &cy, &r)) {
            draw_plate(cr, cx, cy, r);
            legend(cr, widget, "from above", 9, FALSE, COL_DIM_INK, cx - 40, cy + r + 6, 80, 14);
        }
        if (g_layout[i].kind == CHF_CTL_SIDE)
            legend(cr, widget, "side view", 9, FALSE, COL_DIM_INK, px_x(g_layout[i].x),
                   px_y(g_layout[i].y + 2.6) + 6, g_layout[i].w * U, 14);
    }
    for (i = 0; i < g_n_parts; i++) {
        const chf_control *c = &g_layout[i];
        double cx, cy, r;
        switch (c->kind) {
        case CHF_CTL_BUTTON:
        case CHF_CTL_RESET:
            draw_button(cr, widget, i);
            break;
        case CHF_CTL_DIRECTION:
            if (stick_of(c->port, &cx, &cy, &r))
                draw_wedge(cr, widget, i, cx, cy, r);
            break;
        case CHF_CTL_TWIST:
            draw_twist(cr, widget, i);
            break;
        case CHF_CTL_AXIS:
            draw_axis(cr, widget, i);
            break;
        case CHF_CTL_SIDE:
            if (g_map_state != -2) cairo_push_group(cr);
            draw_side(cr, i);
            if (g_map_state != -2) {
                cairo_pop_group_to_source(cr);
                cairo_paint_with_alpha(cr, 0.4);
            }
            break;
        default:
            break;
        }
    }
    for (i = 0; i < g_n_parts; i++) {
        double cx, cy, r;
        if (g_layout[i].kind == CHF_CTL_STICK && stick_of(g_layout[i].port, &cx, &cy, &r)) {
            const unsigned held = chfsession_buttons_held(g_session, g_layout[i].port);
            const gboolean armed = g_map_state != -2;
            if (armed) cairo_push_group(cr);
            draw_cap(cr, g_layout[i].port, cx, cy, r,
                     (held & ((1u << CHF_ACT_PUSH) | (1u << CHF_ACT_PULL))) != 0);
            if (armed) {
                cairo_pop_group_to_source(cr);
                cairo_paint_with_alpha(cr, 0.4);
            }
        }
    }
}

/* The two buttons under the case. */
static void draw_strip(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer ud)
{
    Strip *p = ud;
    int i;
    (void)w; (void)h;

    for (i = 0; i < p->n; i++) {
        const StripItem *it = &p->items[i];
        const gboolean held = p->pressed == i;
        const gboolean sel = g_map_state >= 0 && g_map_state == it->target;
        const gboolean dim = g_map_state >= 0 && !sel;
        guint32 face = held || sel ? COL_ACCENT : p->hover == i ? 0x4A4A52 : 0x38383E;
        char bound[40], text[120];

        if (dim) cairo_push_group(cr);
        rrect(cr, it->x, it->y + 3, it->w, it->h - 3, 7);
        rgb(cr, held || sel ? CHFSESSION_ACCENT_DARK_RGB : 0x1A1A1D);
        cairo_fill(cr);
        rrect(cr, it->x, it->y, it->w, it->h - 3, 7);
        rgb(cr, face);
        cairo_fill(cr);
        bound_text(it->target, bound, sizeof bound);
        if (bound[0])
            g_snprintf(text, sizeof text, "%s  \xc2\xb7  %s", it->label, bound);
        else
            g_snprintf(text, sizeof text, "%s", it->label);
        legend(cr, GTK_WIDGET(area), text, 12, FALSE, held || sel ? COL_DARK_INK : 0xFFFFFF,
               it->x + 4, it->y, it->w - 8, it->h - 3);
        if (dim) {
            cairo_pop_group_to_source(cr);
            cairo_paint_with_alpha(cr, 0.4);
        }
    }
}

/* ---- pointer ---------------------------------------------------------------------- */

static int console_hit(double x, double y)
{
    return chfsession_controls_hit(machine_now(), (float)((x - OX) / U), (float)((y - OY) / U));
}

static void run_sysaction(int sa)
{
    if (g_parent)
        chf_window_run_sysaction(CHF_WINDOW(g_parent), sa);
    else
        chfsession_sysaction(g_session, sa);
}

/* Lets go of everything the mouse holds on the console. */
static void mouse_release(void)
{
    if (g_drag.active) {
        chfsession_pointer_set(g_session, g_drag.port, 0);
        g_drag.active = FALSE;
        g_drag.extra = 0;
    }
    if (g_arrow_port >= 0) {
        chfsession_pointer_set(g_session, g_arrow_port, 0);
        g_arrow_port = -1;
    }
    if (g_pressed >= 0) {
        const int t = g_layout[g_pressed].target;
        g_pressed = -1;
        if (t >= 0)
            chfsession_press(g_session, t, 0);
    }
    if (g_strip.pressed >= 0) {
        g_strip.pressed = -1;
        if (g_strip.area) gtk_widget_queue_draw(g_strip.area);
    }
    if (g_console) gtk_widget_queue_draw(g_console);
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
    gtk_widget_set_sensitive(g_clear_button, state >= 0);
    if (state == -2) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Map");
        gtk_widget_remove_css_class(g_map_button, "chf-accent");
        gtk_label_set_text(GTK_LABEL(g_map_hint),
                           "Hold a control with the mouse. Map rebinds one.");
    } else if (state == -1) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Done");
        gtk_widget_add_css_class(g_map_button, "chf-accent");
        gtk_label_set_text(GTK_LABEL(g_map_hint), "Click a control to remap it");
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

/* The stick under a drag: the pointer's vector from the knob's centre. */
static void drag_follow(double x, double y)
{
    const unsigned stick = chfsession_stick_from_vector((float)(x - g_drag.cx),
                                                        (float)(y - g_drag.cy), (float)g_drag.r);
    chfsession_pointer_set(g_session, g_drag.port, stick | g_drag.extra);
    gtk_widget_queue_draw(g_console);
}

static void on_console_begin(GtkGestureDrag *g, double x, double y, gpointer ud)
{
    const guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
    const int i = console_hit(x, y);
    const chf_control *c;
    (void)ud;

    if (i < 0 || (button != GDK_BUTTON_PRIMARY && button != GDK_BUTTON_SECONDARY))
        return;
    c = &g_layout[i];
    if (g_map_state != -2) {
        /* armed: picks (or changes) the target; the cap and the diagonal
         * wedges are no single control */
        if (c->target >= 0 && button == GDK_BUTTON_PRIMARY)
            set_map_state(c->target);
        return;
    }
    mouse_release();
    switch (c->kind) {
    case CHF_CTL_STICK:
    case CHF_CTL_DIRECTION:
        if (!stick_of(c->port, &g_drag.cx, &g_drag.cy, &g_drag.r))
            return;
        g_drag.active = TRUE;
        g_drag.port = c->port;
        /* a left-press on the cap pushes it, a right-press anywhere pulls it */
        g_drag.extra = button == GDK_BUTTON_SECONDARY ? (1u << CHF_ACT_PULL)
                     : c->kind == CHF_CTL_STICK ? (1u << CHF_ACT_PUSH) : 0;
        drag_follow(x, y);
        break;
    case CHF_CTL_TWIST:
    case CHF_CTL_AXIS:
        if (button != GDK_BUTTON_PRIMARY)
            return;
        g_arrow_port = c->port;
        g_pressed = i;
        chfsession_pointer_set(g_session, c->port, c->act_mask);
        break;
    case CHF_CTL_BUTTON:
    case CHF_CTL_RESET:
        if (button != GDK_BUTTON_PRIMARY)
            return;
        g_pressed = i;
        chfsession_press(g_session, c->target, 1);
        break;
    default:
        return;
    }
    gtk_widget_queue_draw(g_console);
}

static void on_console_update(GtkGestureDrag *g, double ox, double oy, gpointer ud)
{
    double sx, sy;
    (void)ud;
    if (!g_drag.active) return;
    gtk_gesture_drag_get_start_point(g, &sx, &sy);
    drag_follow(sx + ox, sy + oy);
}

static void on_console_end(GtkGestureDrag *g, double ox, double oy, gpointer ud)
{
    (void)g; (void)ox; (void)oy; (void)ud;
    mouse_release();
}

static void on_console_cancel(GtkGesture *g, GdkEventSequence *seq, gpointer ud)
{
    (void)g; (void)seq; (void)ud;
    mouse_release();
}

/* Dragging off a held arrow or button lets it go. */
static void on_console_motion(GtkEventControllerMotion *c, double x, double y, gpointer ud)
{
    const int i = console_hit(x, y);
    (void)c; (void)ud;
    g_ptr_x = x; g_ptr_y = y; g_ptr_in = TRUE;
    if (g_pressed >= 0 && i != g_pressed) {
        const int hit = g_pressed;
        mouse_release();
        (void)hit;
    }
    if (i != g_hover) {
        g_hover = i;
        gtk_widget_queue_draw(g_console);
    }
}

static void on_console_leave(GtkEventControllerMotion *c, gpointer ud)
{
    (void)c; (void)ud;
    g_ptr_in = FALSE;
    if (g_pressed >= 0)
        mouse_release();
    if (g_hover >= 0) {
        g_hover = -1;
        gtk_widget_queue_draw(g_console);
    }
}

/* The wheel twists the controller under the pointer, one notch a pulse; a
 * trackpad's smooth scrolling adds up to notches. Down is clockwise. */
static gboolean on_console_scroll(GtkEventControllerScroll *c, double dx, double dy, gpointer ud)
{
    int i, port = -1, dir;
    (void)c; (void)ud;
    if (!g_ptr_in || g_map_state != -2)
        return FALSE;
    i = console_hit(g_ptr_x, g_ptr_y);
    if (i >= 0)
        port = g_layout[i].port;
    if (port < 0) {
        /* between the parts: the controller whose block the pointer is over */
        const double uy = (g_ptr_y - OY) / U, ux = (g_ptr_x - OX) / U;
        int k;
        for (k = 0; k < g_n_parts && uy > 0; k++)
            if (g_layout[k].kind == CHF_CTL_CAPTION && uy >= g_layout[k].y &&
                ux >= g_layout[k].x && ux < g_layout[k].x + g_layout[k].w)
                port = g_layout[k].port;
    }
    if (port < 0)
        return FALSE;
    g_scroll_acc += dy + dx;
    while (g_scroll_acc >= 1.0) {
        dir = 1;
        chfsession_twist_pulse(g_session, port, dir);
        g_scroll_acc -= 1.0;
    }
    while (g_scroll_acc <= -1.0) {
        dir = -1;
        chfsession_twist_pulse(g_session, port, dir);
        g_scroll_acc += 1.0;
    }
    return TRUE;
}

static gboolean on_console_tooltip(GtkWidget *w, int x, int y, gboolean kbd, GtkTooltip *tip,
                                   gpointer ud)
{
    const int i = kbd ? -1 : console_hit(x, y);
    const chf_control *c;
    char text[300];
    (void)w; (void)ud;
    if (i < 0) return FALSE;
    c = &g_layout[i];
    if (c->target >= 0) {
        const chf_binding b = chfsession_binding_get(g_session, c->target);
        char key[32];
        chfsession_keysym_name(b.keysym, key, sizeof key);
        g_snprintf(text, sizeof text, "%s\nKey: %s    Gamepad: %s", chf_target_name(c->target),
                   key[0] ? key : "none",
                   b.button != CHF_PAD_BTN_NONE ? chf_pad_button_name(b.button) : "none");
    } else if (c->kind == CHF_CTL_STICK) {
        g_snprintf(text, sizeof text, "Drag to tilt the stick, press to push the knob down, "
                   "right-press to pull it up, wheel to twist it");
    } else if (c->kind == CHF_CTL_DIRECTION) {
        g_snprintf(text, sizeof text, "%s (two contacts): press and hold", c->label);
    } else {
        return FALSE;
    }
    gtk_tooltip_set_text(tip, text);
    return TRUE;
}

/* ---- the strip ------------------------------------------------------------------------ */

static int strip_hit(const Strip *p, double x, double y)
{
    int i;
    for (i = p->n - 1; i >= 0; i--)
        if (x >= p->items[i].x && x < p->items[i].x + p->items[i].w &&
            y >= p->items[i].y && y < p->items[i].y + p->items[i].h)
            return i;
    return -1;
}

static void strip_release(gboolean activate)
{
    int i = g_strip.pressed;
    if (i < 0) return;
    g_strip.pressed = -1;
    /* a system action fires on release, like a real button: pressing and
     * dragging off should not power-cycle the cartridge */
    if (activate && is_sysact(g_strip.items[i].target))
        run_sysaction(g_strip.items[i].target - CHF_TARGET_SYSACT(0));
    gtk_widget_queue_draw(g_strip.area);
}

static void on_strip_begin(GtkGestureDrag *g, double x, double y, gpointer ud)
{
    const int i = strip_hit(&g_strip, x, y);
    (void)g; (void)ud;
    if (i < 0) return;
    if (g_map_state != -2) {
        set_map_state(g_strip.items[i].target);
        return;
    }
    g_strip.pressed = i;
    gtk_widget_queue_draw(g_strip.area);
}

static void on_strip_end(GtkGestureDrag *g, double ox, double oy, gpointer ud)
{
    (void)g; (void)ox; (void)oy; (void)ud;
    strip_release(g_strip.hover == g_strip.pressed);
}

static void on_strip_cancel(GtkGesture *g, GdkEventSequence *seq, gpointer ud)
{
    (void)g; (void)seq; (void)ud;
    strip_release(FALSE);
}

static void on_strip_motion(GtkEventControllerMotion *c, double x, double y, gpointer ud)
{
    const int i = strip_hit(&g_strip, x, y);
    (void)c; (void)ud;
    if (i != g_strip.hover) {
        g_strip.hover = i;
        gtk_widget_queue_draw(g_strip.area);
    }
}

static void on_strip_leave(GtkEventControllerMotion *c, gpointer ud)
{
    (void)c; (void)ud;
    g_strip.hover = -1;
    gtk_widget_queue_draw(g_strip.area);
}

static gboolean on_strip_tooltip(GtkWidget *w, int x, int y, gboolean kbd, GtkTooltip *tip,
                                 gpointer ud)
{
    const int i = kbd ? -1 : strip_hit(&g_strip, x, y);
    chf_binding b;
    char key[32], text[260];
    (void)w; (void)ud;
    if (i < 0) return FALSE;
    b = chfsession_binding_get(g_session, g_strip.items[i].target);
    chfsession_keysym_name(b.keysym, key, sizeof key);
    g_snprintf(text, sizeof text, "%s\nKey: %s    Gamepad: %s",
               chf_target_name(g_strip.items[i].target), key[0] ? key : "none",
               b.button != CHF_PAD_BTN_NONE ? chf_pad_button_name(b.button) : "none");
    gtk_tooltip_set_text(tip, text);
    return TRUE;
}

static void build_strip(void)
{
    Strip *p = &g_strip;
    const int bw = STRIP_BW;

    p->n = 2;
    p->pressed = p->hover = -1;
    p->items[0] = (StripItem){ 0, 0, bw, STRIP_H, K_BUTTON,
                               CHF_TARGET_SYSACT(CHF_SYSACT_RESET_CONFIG), "Reset to CONFIG" };
    p->items[1] = (StripItem){ bw + STRIP_GAP, 0, bw, STRIP_H, K_BUTTON,
                               CHF_TARGET_SYSACT(CHF_SYSACT_DEBUG_STOP), "Stop in Debugger" };
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(p->area), 2 * bw + STRIP_GAP);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(p->area), STRIP_H);
}

/* ---- refresh --------------------------------------------------------------------------- */

static void refresh_all(void)
{
    if (g_console) gtk_widget_queue_draw(g_console);
    if (g_strip.area) gtk_widget_queue_draw(g_strip.area);
}

/* Every frame while the window is on screen: what the console sees held,
 * painted in the accent, and the knobs eased toward it. Redraws only when
 * something moved. */
static gboolean frame_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer d)
{
    unsigned held[2];
    gboolean changed = FALSE;
    int i, port;
    (void)clock; (void)d;

    held[0] = chfsession_buttons_held(g_session, 0);
    held[1] = chfsession_buttons_held(g_session, 1);
    for (i = 0; i < g_n_parts && i < MAX_PARTS; i++) {
        const gboolean on = part_held(&g_layout[i], held);
        if (on != g_lit[i]) {
            g_lit[i] = on;
            changed = TRUE;
        }
    }
    for (port = 0; port < 2; port++)
        if (ease_knob(port, held[port]))
            changed = TRUE;
    if (changed)
        gtk_widget_queue_draw(widget);
    return G_SOURCE_CONTINUE;
}

/* ---- gamepads ------------------------------------------------------------- */

static void refresh_pad_ports(void)
{
    int i;
    for (i = 0; i < g_npad_rows; i++) {
        const int eff = chfsession_gamepad_effective_port(g_session, i);
        char text[32];
        if (eff >= 0) g_snprintf(text, sizeof text, "drives player %d", eff + 1);
        else g_snprintf(text, sizeof text, "drives nothing");
        if (strcmp(gtk_label_get_text(GTK_LABEL(g_pad_eff[i])), text) != 0)
            gtk_label_set_text(GTK_LABEL(g_pad_eff[i]), text);
    }
}

static void on_assign_changed(GObject *o, GParamSpec *ps, gpointer user_data)
{
    const int idx = GPOINTER_TO_INT(user_data);
    const guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(o));
    (void)ps;
    /* 0 = automatic (connection order), 1-2 = that player */
    chfsession_gamepad_assign(g_session, idx, sel == 0 ? -1 : (int)sel - 1);
    refresh_pad_ports();
}

/* Rebuilt whenever a pad comes or goes (the session's generation counter):
 * one row per pad, its name, the player it drives and the one it is set to.
 * The area is PAD_ROWS rows tall whatever is plugged in (more pads scroll),
 * so the window never changes size. */
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
        gtk_label_set_xalign(GTK_LABEL(none), 0);
        gtk_widget_set_size_request(none, -1, PAD_ROW_H);
        gtk_widget_set_tooltip_text(none, "Plug one in: it is picked up as it appears");
        gtk_box_append(GTK_BOX(g_pad_box), none);
        return;
    }
    for (i = 0; i < n && i < MAX_PADS; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *name, *drop, *eff;
        char text[128];
        const int assign = chfsession_gamepad_assignment(g_session, i);

        chfsession_gamepad_name(g_session, i, text, sizeof text);
        gtk_widget_set_size_request(row, -1, PAD_ROW_H);
        name = gtk_label_new(text);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(name), 1);
        gtk_widget_set_tooltip_text(name, text);
        eff = gtk_label_new("");
        gtk_widget_add_css_class(eff, "dim-label");
        drop = gtk_drop_down_new_from_strings(choices);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(drop),
                                   assign < 0 || assign > 1 ? 0 : (guint)assign + 1);
        gtk_widget_set_focus_on_click(drop, FALSE);
        gtk_widget_set_valign(drop, GTK_ALIGN_CENTER);
        g_signal_connect(drop, "notify::selected", G_CALLBACK(on_assign_changed),
                         GINT_TO_POINTER(i));
        gtk_box_append(GTK_BOX(row), name);
        gtk_box_append(GTK_BOX(row), eff);
        gtk_box_append(GTK_BOX(row), drop);
        gtk_box_append(GTK_BOX(g_pad_box), row);
        g_pad_eff[g_npad_rows++] = eff;
    }
    refresh_pad_ports();
}

/* The pad rows (hot plug, automatic assignment changes) and the machine's
 * name. */
static gboolean info_tick(gpointer d)
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
    if (machine_now() != g_machine) {
        g_machine = machine_now();
        adw_window_title_set_subtitle(g_title, chf_machine_name(g_machine));
    }
    return G_SOURCE_CONTINUE;
}

/* ---- keyboard ------------------------------------------------------------- */

static gboolean on_key(guint keyval, guint code, GdkModifierType st, gboolean down)
{
    /* an open popover has the arrows and Return */
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

static void on_active_changed(GObject *o, GParamSpec *ps, gpointer d)
{
    (void)ps; (void)d;
    if (!gtk_window_is_active(GTK_WINDOW(o)))
        mouse_release();
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

/* Takes the picked control's key and gamepad button away. */
static void on_clear_clicked(GtkButton *b, gpointer d)
{
    char stolen[16];
    (void)b; (void)d;
    if (g_map_state < 0) return;
    chfsession_binding_set_key(g_session, g_map_state, 0, stolen, sizeof stolen);
    chfsession_binding_set_button(g_session, g_map_state, CHF_PAD_BTN_NONE, stolen, sizeof stolen);
    gtk_label_set_text(GTK_LABEL(g_map_hint), "Cleared. Press a key or a gamepad button to bind it again");
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

/* The pad line's timer runs only while the window is on screen: closing only
 * hides it, and a hidden window has nobody to update. (The per-frame tick
 * stops by itself while the console is unmapped.) */
static void on_window_map(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (!g_info_timer) g_info_timer = g_timeout_add(250, info_tick, NULL);
    info_tick(NULL);
}

static void on_window_unmap(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (g_info_timer) g_source_remove(g_info_timer);
    g_info_timer = 0;
}

static void on_destroy(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (g_capture_timer) g_source_remove(g_capture_timer);
    if (g_info_timer) g_source_remove(g_info_timer);
    g_capture_timer = g_info_timer = 0;
    memset(&g_strip, 0, sizeof g_strip);
    g_console = NULL;
    g_window = NULL;
    g_pad_box = NULL;
    g_npad_rows = 0;
    g_machine = -1;
    g_map_state = -2;
    g_hover = g_pressed = -1;
    g_drag.active = FALSE;
    g_arrow_port = -1;
}

static void build_window(GtkWindow *parent)
{
    GtkWidget *root, *maprow, *toolbar, *header, *defaults, *padscroll;
    GtkEventController *keys, *focus, *motion, *scroll;
    GtkGesture *drag;

    chf_install_accent_css();
    g_layout = chfsession_controls_layout(machine_now(), &g_n_parts, &g_lw, &g_lh);
    if (g_n_parts > MAX_PARTS) g_n_parts = MAX_PARTS;
    memset(g_lit, 0, sizeof g_lit);
    memset(g_anim, 0, sizeof g_anim);
    g_hover = g_pressed = -1;

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
    g_signal_connect(g_window, "notify::is-active", G_CALLBACK(on_active_changed), NULL);

    /* the console */
    g_console = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(g_console), console_width());
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(g_console), console_height());
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_console), draw_console, NULL, NULL);
    gtk_widget_set_halign(g_console, GTK_ALIGN_CENTER);
    /* Not focusable: clicking a control must not steal focus. */
    gtk_widget_set_focusable(g_console, FALSE);
    gtk_widget_set_has_tooltip(g_console, TRUE);
    g_signal_connect(g_console, "query-tooltip", G_CALLBACK(on_console_tooltip), NULL);
    drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), 0);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_console_begin), NULL);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_console_update), NULL);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_console_end), NULL);
    g_signal_connect(drag, "cancel", G_CALLBACK(on_console_cancel), NULL);
    gtk_widget_add_controller(g_console, GTK_EVENT_CONTROLLER(drag));
    motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_console_motion), NULL);
    g_signal_connect(motion, "leave", G_CALLBACK(on_console_leave), NULL);
    gtk_widget_add_controller(g_console, motion);
    scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(scroll, "scroll", G_CALLBACK(on_console_scroll), NULL);
    gtk_widget_add_controller(g_console, scroll);
    gtk_widget_add_tick_callback(g_console, frame_tick, NULL, NULL);

    /* the strip */
    memset(&g_strip, 0, sizeof g_strip);
    g_strip.area = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_strip.area), draw_strip, &g_strip, NULL);
    gtk_widget_set_halign(g_strip.area, GTK_ALIGN_CENTER);
    gtk_widget_set_focusable(g_strip.area, FALSE);
    gtk_widget_set_has_tooltip(g_strip.area, TRUE);
    g_signal_connect(g_strip.area, "query-tooltip", G_CALLBACK(on_strip_tooltip), NULL);
    drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_strip_begin), NULL);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_strip_end), NULL);
    g_signal_connect(drag, "cancel", G_CALLBACK(on_strip_cancel), NULL);
    gtk_widget_add_controller(g_strip.area, GTK_EVENT_CONTROLLER(drag));
    motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_strip_motion), NULL);
    g_signal_connect(motion, "leave", G_CALLBACK(on_strip_leave), NULL);
    gtk_widget_add_controller(g_strip.area, motion);
    build_strip();

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);
    gtk_box_append(GTK_BOX(root), g_console);
    gtk_box_append(GTK_BOX(root), g_strip.area);

    /* the pad rows: a fixed-height area, so the window never resizes */
    g_pad_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, PAD_GAP);
    padscroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(padscroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(padscroll), FALSE);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(padscroll),
        PAD_ROWS * PAD_ROW_H + (PAD_ROWS - 1) * PAD_GAP);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(padscroll),
        PAD_ROWS * PAD_ROW_H + (PAD_ROWS - 1) * PAD_GAP);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(padscroll), g_pad_box);
    gtk_box_append(GTK_BOX(root), padscroll);
    g_pad_generation = (unsigned)-1;
    refresh_pads();

    maprow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_map_button = gtk_button_new_with_label("Map");
    gtk_widget_set_size_request(g_map_button, 72, -1);
    gtk_widget_set_focus_on_click(g_map_button, FALSE);
    g_signal_connect(g_map_button, "clicked", G_CALLBACK(on_map_clicked), NULL);
    g_clear_button = gtk_button_new_with_label("Clear");
    gtk_widget_set_focus_on_click(g_clear_button, FALSE);
    gtk_widget_set_tooltip_text(g_clear_button, "Take the picked control's key and gamepad button away");
    g_signal_connect(g_clear_button, "clicked", G_CALLBACK(on_clear_clicked), NULL);
    defaults = gtk_button_new_with_label("Defaults");
    gtk_widget_set_focus_on_click(defaults, FALSE);
    g_signal_connect(defaults, "clicked", G_CALLBACK(on_defaults_clicked), NULL);
    g_show_bindings = gtk_check_button_new_with_label("Show bindings");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(g_show_bindings),
        show_bindings_saved());
    gtk_widget_set_focus_on_click(g_show_bindings, FALSE);
    g_signal_connect(g_show_bindings, "toggled", G_CALLBACK(on_show_bindings), NULL);
    gtk_box_append(GTK_BOX(maprow), g_map_button);
    gtk_box_append(GTK_BOX(maprow), g_clear_button);
    gtk_box_append(GTK_BOX(maprow), defaults);
    gtk_box_append(GTK_BOX(maprow), g_show_bindings);
    gtk_box_append(GTK_BOX(root), maprow);

    g_map_hint = gtk_label_new("");
    gtk_widget_add_css_class(g_map_hint, "dim-label");
    gtk_widget_set_hexpand(g_map_hint, TRUE);
    gtk_label_set_xalign(GTK_LABEL(g_map_hint), 0);
    /* a long hint ("Press a key or a gamepad button for Player 1: Right")
     * must not widen the window */
    gtk_label_set_ellipsize(GTK_LABEL(g_map_hint), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(g_map_hint), 1);
    gtk_box_append(GTK_BOX(root), g_map_hint);

    header = adw_header_bar_new();
    g_title = ADW_WINDOW_TITLE(adw_window_title_new("Controllers", ""));
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), GTK_WIDGET(g_title));
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), chf_help_button("controllers"));
    chf_help_set_context(GTK_WINDOW(g_window), "controllers");
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

    g_machine = -1;
    set_map_state(-2);
    g_signal_connect(g_window, "map", G_CALLBACK(on_window_map), NULL);
    g_signal_connect(g_window, "unmap", G_CALLBACK(on_window_unmap), NULL);
}

static void ensure_window(GtkWindow *parent, chfsession *session)
{
    g_session = session;
    if (parent)
        g_parent = parent;
    if (!g_window)
        build_window(g_parent);
}

void chf_controllers_show(GtkWindow *parent, chfsession *session)
{
    ensure_window(parent, session);
    gtk_window_present(GTK_WINDOW(g_window));
}

void chf_controllers_toggle(GtkWindow *parent, chfsession *session)
{
    ensure_window(parent, session);
    if (gtk_widget_get_visible(g_window))
        hide_window();
    else
        gtk_window_present(GTK_WINDOW(g_window));
}

gboolean chf_controllers_visible(void)
{
    return g_window && gtk_widget_get_visible(g_window);
}
