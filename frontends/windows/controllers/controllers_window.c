/*
 * The Win32 Controllers window: the Channel F drawn as a player sees it -- a
 * charcoal case with its walnut strip, the console's panel across the top
 * (RESET, 1 TIME, 2 MODE, 3 HOLD, 4 START) and the two hand controllers under
 * it, the left one Player 2 and the right one Player 1 -- from the session's
 * layout (chfsession_controls_layout), so every frontend shows the same
 * console, then the gamepads with their assignments and Map mode.
 *
 * Each controller is one knob with four axes, drawn as two views of it. From
 * above: a ring of eight direction wedges round the knob cap, which shifts
 * toward the held direction, turns a little when twisted, shrinks when pushed
 * and grows when pulled, with the twist arrows either side. From the side: the
 * handle with PULL above and PUSH below and the knob at its height.
 *
 * Mouse: a press inside the disc follows the pointer -- on the cap it pushes
 * (left button) or pulls (right button), dragged out over the wedges it tilts
 * the stick eight ways (chfsession_stick_from_vector); a press on a wedge
 * holds that direction. The wheel twists (chfsession_twist_pulse); a press on
 * a twist arrow, PULL, PUSH or a console button holds it. Presses are the raw
 * button-down and button-up, never a click: the console samples its controls
 * once a frame, so a press present only for the instant of a click falls
 * between frames. The mouse is captured on press; letting go anywhere,
 * losing the capture, hiding or deactivating the window releases whatever the
 * pointer holds (chfsession_pointer_set(port, 0)).
 *
 * What the console sees held -- keyboard, gamepads and this window together
 * (chfsession_buttons_held, key_held, switch_held) -- is polled every 40 ms
 * and painted in the accent, so the window doubles as a gamepad tester.
 *
 * FIXED SIZE: laid out once for the console, three gamepad rows and the Map
 * row, in a caption-only tool window with no thick frame and no maximize
 * box, so neither Map mode nor a long gamepad name changes its size.
 *
 * MAP MODE: Map arms a two-step rebind -- click a control to pick the target
 * (the two session actions below the console too), then press a host key OR a
 * gamepad button (Esc cancels). While armed the controls show what drives
 * them, typing does not reach the console, and rebinding steals the key from
 * whatever held it. "Show bindings" shows the same legends outside Map mode.
 *
 * The art is drawn at twice the size into a memory bitmap and shrunk with
 * HALFTONE (GDI has no anti-aliasing of its own); the text goes on after at
 * the window's own size.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include "controllers_window.h"

/* GET_X_LPARAM / GET_Y_LPARAM live here, not in windows.h. */
#include <windowsx.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../help/help_window.h"
#include "../key_forward.h"
#include "../win_common.h"

#define PANEL_CLASS   "FujiNetGoChfControllers"
#define PANEL_STYLE   (WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN)
#define PANEL_EXSTYLE WS_EX_TOOLWINDOW

/* Sizes at 96 dpi; S() scales them to the window's. */
#define U96         38      /* pixels per layout unit (a console button) */
#define MARGIN      12
#define CASE_PAD    18
#define WOOD_H      16      /* the walnut strip along the top of the case */
#define HEAD_H      22
#define ROW_H       26      /* one gamepad's row */
#define PAD_ROWS     3      /* gamepad rows the window has room for */
#define SEG_W       64      /* Automatic / Player 1 / Player 2 */
#define BTN_H       28
#define MAP_W       60
#define DEFAULTS_W  76
#define SHOW_W     124
#define ACT_W      116
#define HELP_W      64

#define IDT_CAPTURE 1
#define IDT_HELD    2
#define IDT_PADS    3

#define PI_D 3.14159265358979323846

/* The strip's buttons (everything below the console that is pressed). */
enum { SB_MAP, SB_DEFAULTS, SB_SHOW, SB_ACT_CONFIG, SB_ACT_STOP, SB_HELP, SB_COUNT };

typedef struct {
    unsigned generation;
    int count, assign[PAD_ROWS], effective[PAD_ROWS];
} pads_state;

static HWND g_panel, g_parent;
static chfsession *g_session;
static int g_dpi = 96;

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 waiting for a
 * key or pad button to bind to that target. */
static int g_map_state = -2;
static int g_show_bindings;
static WPARAM g_swallow_vk;     /* the key Map took: its repeats and release too */
static char g_hint[300];

/* The console's layout (units), and where it sits in the client. */
static const chf_control *g_layout;
static int g_nlayout;
static float g_lw, g_lh;
static int g_built_machine = -1;
static double g_u, g_ox, g_oy;
static struct { double cx, cy, r; } g_disc[2];      /* per port */

/* What the pointer holds. */
static int g_mouse_part = -1;       /* a pressed button-like part (layout index) */
static int g_mouse_port = -1;       /* a stick drag */
static int g_mouse_btn;             /* 1 left, 2 right (the stick drag) */
static int g_mouse_action = -1;     /* a pressed session action button */
static unsigned g_pointer[2];       /* the bits given to chfsession_pointer_set */
static int g_wheel_acc;

/* What was last painted as held. */
static unsigned g_shown_held[2];
static unsigned g_shown_keys;
static int g_shown_reset;

static pads_state g_pads_shown;
static int g_total_w, g_total_h;
static RECT g_case_rc, g_wood_rc, g_gp_rc, g_gp_head_rc, g_gp_row_rc[PAD_ROWS];
static RECT g_btn_rc[SB_COUNT], g_hint_rc;

static HBRUSH g_accent_brush;
static HFONT g_font, g_bold, g_small, g_num, g_leg, g_tiny;

static void refresh_held(void);
static void chf_controllers_binding_label(int target, char *out, int outsz);

static int S(int v) { return chf_scale(v, g_dpi); }

/* ---- fonts ------------------------------------------------------------------ */

static void free_fonts(void)
{
    HFONT *f[] = { &g_font, &g_bold, &g_small, &g_num, &g_leg, &g_tiny };
    size_t i;
    for (i = 0; i < sizeof f / sizeof f[0]; i++)
        if (*f[i]) { DeleteObject(*f[i]); *f[i] = NULL; }
}

static void make_fonts(void)
{
    free_fonts();
    g_font = chf_make_font(g_dpi, 0, FW_NORMAL, NULL);
    g_bold = chf_make_font(g_dpi, 0, FW_BOLD, NULL);
    g_small = chf_make_font(g_dpi, 11, FW_BOLD, NULL);
    g_num = chf_make_font(g_dpi, 20, FW_BOLD, NULL);
    g_leg = chf_make_font(g_dpi, 10, FW_BOLD, NULL);
    g_tiny = chf_make_font(g_dpi, 10, FW_NORMAL, NULL);
}

/* ---- text -------------------------------------------------------------------- */

/* UTF-8 (legends, key names, gamepad names) through DrawTextW. */
static int text_out(HDC dc, const char *utf8, RECT *rc, UINT fmt)
{
    wchar_t w[256];
    if (!utf8 || !*utf8) return 0;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, (int)(sizeof w / sizeof w[0])) <= 0) return 0;
    return DrawTextW(dc, w, -1, rc, fmt | DT_NOPREFIX);
}

static void text_line(HDC dc, const char *utf8, const RECT *rc, UINT fmt)
{
    RECT r = *rc;
    text_out(dc, utf8, &r, fmt | DT_SINGLELINE | DT_VCENTER);
}

/* Centred both ways, wrapped (a Map legend is a key and a pad button). */
static void text_block(HDC dc, const char *utf8, const RECT *rc)
{
    RECT m = *rc;
    int h;
    InflateRect(&m, -S(1), 0);
    h = text_out(dc, utf8, &m, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
    m.left = rc->left + S(1);
    m.right = rc->right - S(1);
    m.top = rc->top + ((rc->bottom - rc->top) - h) / 2;
    if (m.top < rc->top) m.top = rc->top;
    m.bottom = rc->bottom;
    text_out(dc, utf8, &m, DT_CENTER | DT_WORDBREAK);
}

static COLORREF rgb_of(unsigned v)
{
    return RGB((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
}

#define C_ACCENT   rgb_of(CHFSESSION_ACCENT_RGB)
#define C_ACCENT_D rgb_of(CHFSESSION_ACCENT_DARK_RGB)
#define C_ON_ACC   RGB(0x14, 0x14, 0x14)         /* dark text on the accent */
#define C_CASE     RGB(0x2b, 0x2b, 0x2e)
#define C_CASE_E   RGB(0x10, 0x10, 0x12)
#define C_BLOCK    RGB(0x36, 0x36, 0x3a)
#define C_PART     RGB(0x45, 0x45, 0x4b)
#define C_PART_E   RGB(0x70, 0x70, 0x78)
#define C_WELL     RGB(0x12, 0x12, 0x15)
#define C_KNOB     RGB(0x5a, 0x5a, 0x61)
#define C_KNOB_HI  RGB(0x80, 0x80, 0x88)
#define C_LIGHT    RGB(0xe8, 0xe8, 0xe8)
#define C_DIM      RGB(0xa6, 0xa6, 0xae)

/* ---- art: GDI at a supersampled scale ------------------------------------------- */

/* One art pass draws the client rectangle with its top-left at (g_art_x,
 * g_art_y) into a bitmap g_art_k times the size. */
static int g_art_k = 1, g_art_x, g_art_y;

static int AX(double v) { return (int)floor((v - g_art_x) * g_art_k + 0.5); }
static int AY(double v) { return (int)floor((v - g_art_y) * g_art_k + 0.5); }
static int AL(double v) { return (int)floor(v * g_art_k + 0.5); }

static void poly(HDC dc, const POINT *pt, int n, COLORREF fill, COLORREF line, double lw)
{
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, lw > 0 ? AL(lw) : 0, line);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, lw > 0 ? (HGDIOBJ)p : GetStockObject(NULL_PEN));
    Polygon(dc, pt, n);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(p);
    DeleteObject(b);
}

static void ellipse(HDC dc, double cx, double cy, double rx, double ry, COLORREF fill, COLORREF line, double lw)
{
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, lw > 0 ? AL(lw) : 0, line);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, lw > 0 ? (HGDIOBJ)p : GetStockObject(NULL_PEN));
    Ellipse(dc, AX(cx - rx), AY(cy - ry), AX(cx + rx), AY(cy + ry));
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(p);
    DeleteObject(b);
}

static void rrect(HDC dc, double x, double y, double w, double h, double rad, COLORREF fill, COLORREF line,
                  double lw)
{
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, lw > 0 ? AL(lw) : 0, line);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, lw > 0 ? (HGDIOBJ)p : GetStockObject(NULL_PEN));
    RoundRect(dc, AX(x), AY(y), AX(x + w), AY(y + h), AL(rad * 2), AL(rad * 2));
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(p);
    DeleteObject(b);
}

static void stroke(HDC dc, const POINT *pt, int n, COLORREF col, double lw)
{
    HPEN p = CreatePen(PS_SOLID, AL(lw), col);
    HGDIOBJ op = SelectObject(dc, p);
    Polyline(dc, pt, n);
    SelectObject(dc, op);
    DeleteObject(p);
}

/* The pixel rectangle of a layout part. */
static void part_box(const chf_control *c, double *x, double *y, double *w, double *h)
{
    *x = g_ox + (double)c->x * g_u;
    *y = g_oy + (double)c->y * g_u;
    *w = (double)c->w * g_u;
    *h = (double)c->h * g_u;
}

static void part_rect(const chf_control *c, RECT *r)
{
    double x, y, w, h;
    part_box(c, &x, &y, &w, &h);
    SetRect(r, (int)floor(x), (int)floor(y), (int)ceil(x + w), (int)ceil(y + h));
}

static int binding_mode(void)
{
    return g_map_state != -2 || g_show_bindings;
}

/* Whether a part is shown held: what the console sees, in the accent. In Map
 * mode the armed target is the one lit. */
static int part_lit(const chf_control *c)
{
    if (g_map_state != -2)
        return g_map_state >= 0 && c->target == g_map_state;
    switch (c->kind) {
    case CHF_CTL_BUTTON: return (g_shown_keys >> (c->target - CHF_TARGET_KEY(0))) & 1;
    case CHF_CTL_RESET: return g_shown_reset;
    case CHF_CTL_DIRECTION: return c->port >= 0 && (g_shown_held[c->port] & 0xF) == c->act_mask;
    case CHF_CTL_TWIST:
    case CHF_CTL_AXIS: return c->port >= 0 && (g_shown_held[c->port] & c->act_mask) != 0;
    default: return 0;
    }
}

/* A direction vector from the held bits (+y toward the player, down). */
static void held_vector(unsigned held, double *dx, double *dy)
{
    double x = (int)((held >> CHF_ACT_RIGHT) & 1) - (int)((held >> CHF_ACT_LEFT) & 1);
    double y = (int)((held >> CHF_ACT_BACK) & 1) - (int)((held >> CHF_ACT_FORWARD) & 1);
    const double len = sqrt(x * x + y * y);
    if (len > 0) { x /= len; y /= len; }
    *dx = x;
    *dy = y;
}

/* A curved arrow: the arc from s to e degrees (maths angles, y up) with the
 * head at e. */
static void curved_arrow(HDC dc, double cx, double cy, double rr, double s, double e, COLORREF col)
{
    POINT pt[40];
    const int n = 30;
    const double dir = e > s ? 1.0 : -1.0;
    double tx, ty, ex, ey, a;
    POINT head[3];
    int i;
    for (i = 0; i < n; i++) {
        a = (s + (e - s) * i / (n - 1)) * PI_D / 180.0;
        pt[i].x = AX(cx + rr * cos(a));
        pt[i].y = AY(cy - rr * sin(a));
    }
    stroke(dc, pt, n, col, rr * 0.26);
    a = e * PI_D / 180.0;
    ex = cx + rr * cos(a);
    ey = cy - rr * sin(a);
    tx = -sin(a) * dir;                 /* the tangent in the direction of travel, y up */
    ty = cos(a) * dir;
    head[0].x = AX(ex + tx * rr * 0.55);
    head[0].y = AY(ey - ty * rr * 0.55);
    head[1].x = AX(ex - ty * rr * 0.5 - tx * rr * 0.1);
    head[1].y = AY(ey - tx * rr * 0.5 + ty * rr * 0.1);
    head[2].x = AX(ex + ty * rr * 0.5 - tx * rr * 0.1);
    head[2].y = AY(ey + tx * rr * 0.5 + ty * rr * 0.1);
    poly(dc, head, 3, col, col, 0.5);
}

static void tri(HDC dc, double cx, double cy, double hw, double hh, int up, COLORREF col)
{
    POINT pt[3];
    pt[0].x = AX(cx);
    pt[0].y = AY(up ? cy - hh : cy + hh);
    pt[1].x = AX(cx + hw);
    pt[1].y = AY(up ? cy + hh : cy - hh);
    pt[2].x = AX(cx - hw);
    pt[2].y = AY(up ? cy + hh : cy - hh);
    poly(dc, pt, 3, col, col, 0.5);
}

/* One of the eight wedges: the sector of the stick disc between half its
 * radius and all of it, a little narrower than 45 degrees so they read as
 * separate. */
static void wedge(HDC dc, double cx, double cy, double r, int k, COLORREF fill, COLORREF line)
{
    POINT pt[40];
    int n = 0, i;
    const double a0 = (k * 45.0 - 21.5) * PI_D / 180.0, span = 43.0 * PI_D / 180.0;
    for (i = 0; i <= 9; i++) {
        const double a = a0 + span * i / 9.0;
        pt[n].x = AX(cx + r * cos(a));
        pt[n++].y = AY(cy - r * sin(a));
    }
    for (i = 9; i >= 0; i--) {
        const double a = a0 + span * i / 9.0;
        pt[n].x = AX(cx + r * 0.52 * cos(a));
        pt[n++].y = AY(cy - r * 0.52 * sin(a));
    }
    poly(dc, pt, n, fill, line, 1.0);
}

/* The sector index of a wedge (0 = RIGHT, counter-clockwise) from its bits. */
static int wedge_sector(unsigned mask)
{
    static const unsigned acts[8] = {
        1u << CHF_ACT_RIGHT,
        (1u << CHF_ACT_FORWARD) | (1u << CHF_ACT_RIGHT),
        1u << CHF_ACT_FORWARD,
        (1u << CHF_ACT_FORWARD) | (1u << CHF_ACT_LEFT),
        1u << CHF_ACT_LEFT,
        (1u << CHF_ACT_BACK) | (1u << CHF_ACT_LEFT),
        1u << CHF_ACT_BACK,
        (1u << CHF_ACT_BACK) | (1u << CHF_ACT_RIGHT),
    };
    int k;
    for (k = 0; k < 8; k++)
        if (acts[k] == mask) return k;
    return 0;
}

static void art_controller(HDC dc, int port)
{
    const double u = g_u;
    const unsigned held = g_map_state == -2 ? g_shown_held[port] : 0;
    const double cx = g_disc[port].cx, cy = g_disc[port].cy, R = g_disc[port].r;
    const int pulled = (held >> CHF_ACT_PULL) & 1, pushed = (held >> CHF_ACT_PUSH) & 1;
    const int cw = (held >> CHF_ACT_TWIST_CW) & 1, ccw = (held >> CHF_ACT_TWIST_CCW) & 1;
    const int mapping = binding_mode();
    double dx, dy, kx, ky, rk, ang = 0;
    int i;

    /* the block: both views of the knob on a lighter plate */
    for (i = 0; i < g_nlayout; i++) {
        const chf_control *c = &g_layout[i];
        double x, y, w, h;
        if (c->kind != CHF_CTL_CAPTION || c->port != port) continue;
        part_box(c, &x, &y, &w, &h);
        rrect(dc, x - u * 0.2, y - u * 0.1, w + u * 0.4, ((double)g_lh - c->y) * u + u * 0.2, u * 0.3, C_BLOCK,
              C_CASE_E, 1.0);
    }

    /* from above: the well, the eight wedges, the cap */
    ellipse(dc, cx, cy, R * 1.04, R * 1.04, C_WELL, C_CASE_E, 1.0);
    for (i = 0; i < g_nlayout; i++) {
        const chf_control *c = &g_layout[i];
        if (c->kind != CHF_CTL_DIRECTION || c->port != port) continue;
        {
            const int lit = part_lit(c);
            wedge(dc, cx, cy, R, wedge_sector(c->act_mask), lit ? C_ACCENT : C_PART, lit ? C_ACCENT_D : C_PART_E);
        }
    }
    ellipse(dc, cx, cy, R * 0.5, R * 0.5, C_WELL, C_WELL, 0);

    {
        held_vector(held, &dx, &dy);
        kx = cx + dx * R * 0.2;
        ky = cy + dy * R * 0.2;
        rk = R * 0.5 * 0.8 * (pushed && !pulled ? 0.84 : pulled && !pushed ? 1.18 : 1.0);
        if (ccw && !cw) ang = -25.0;
        else if (cw && !ccw) ang = 25.0;
        {
            const int lit = (pushed != pulled);
            const double a = ang * PI_D / 180.0;
            POINT mark[2];
            ellipse(dc, kx, ky, rk + u * 0.08, rk + u * 0.08, RGB(0x0a, 0x0a, 0x0c), RGB(0x0a, 0x0a, 0x0c), 0);
            ellipse(dc, kx, ky, rk, rk, lit ? C_ACCENT_D : C_KNOB, lit ? C_ACCENT : C_PART_E, 1.0);
            ellipse(dc, kx - rk * 0.18, ky - rk * 0.2, rk * 0.62, rk * 0.62, lit ? C_ACCENT : C_KNOB_HI,
                    lit ? C_ACCENT : C_KNOB_HI, 0);
            mark[0].x = AX(kx + rk * 0.18 * sin(a));
            mark[0].y = AY(ky - rk * 0.18 * cos(a));
            mark[1].x = AX(kx + rk * 0.86 * sin(a));
            mark[1].y = AY(ky - rk * 0.86 * cos(a));
            stroke(dc, mark, 2, lit ? C_ON_ACC : RGB(0x1a, 0x1a, 0x1c), rk * 0.2);
        }
    }

    /* the rest of this controller's parts */
    for (i = 0; i < g_nlayout; i++) {
        const chf_control *c = &g_layout[i];
        double x, y, w, h;
        const int lit = part_lit(c);
        if (c->port != port) continue;
        part_box(c, &x, &y, &w, &h);
        if (c->kind == CHF_CTL_TWIST) {
            const COLORREF glyph = lit ? C_ON_ACC : C_LIGHT;
            rrect(dc, x + u * 0.04, y, w - u * 0.08, h, u * 0.2, lit ? C_ACCENT : C_PART, lit ? C_ACCENT_D : C_PART_E,
                  1.0);
            if (!mapping) {
                const int left = (c->act_mask >> CHF_ACT_TWIST_CCW) & 1;
                if (left) curved_arrow(dc, x + w / 2, y + h / 2, u * 0.34, 35, 290, glyph);
                else curved_arrow(dc, x + w / 2, y + h / 2, u * 0.34, 145, -110, glyph);
            }
        } else if (c->kind == CHF_CTL_AXIS) {
            const int up = (c->act_mask >> CHF_ACT_PULL) & 1;
            rrect(dc, x, y, w, h, u * 0.2, lit ? C_ACCENT : C_PART, lit ? C_ACCENT_D : C_PART_E, 1.0);
            if (!mapping)
                tri(dc, x + w / 2, up ? y + h * 0.3 : y + h * 0.7, u * 0.26, u * 0.15, up,
                    lit ? C_ON_ACC : C_LIGHT);
        } else if (c->kind == CHF_CTL_SIDE) {
            /* the handle between PULL and PUSH, the knob at its height */
            const double mid = y + h / 2, span = h / 2 - u * 0.28;
            const double ky2 = mid + (pushed && !pulled ? span : pulled && !pushed ? -span : 0.0);
            const int lit2 = (pushed != pulled);
            rrect(dc, x + w / 2 - u * 0.11, y - u * 0.1, u * 0.22, h + u * 0.2, u * 0.08, C_WELL, C_CASE_E, 1.0);
            rrect(dc, x + w / 2 - u * 0.5, ky2 - u * 0.22, u * 1.0, u * 0.44, u * 0.2, lit2 ? C_ACCENT_D : C_KNOB,
                  lit2 ? C_ACCENT : C_PART_E, 1.0);
            rrect(dc, x + w / 2 - u * 0.36, ky2 - u * 0.15, u * 0.72, u * 0.14, u * 0.07, lit2 ? C_ACCENT : C_KNOB_HI,
                  lit2 ? C_ACCENT : C_KNOB_HI, 0);
        }
    }
}

static void art_panel(HDC dc)
{
    int i;
    const double u = g_u;
    for (i = 0; i < g_nlayout; i++) {
        const chf_control *c = &g_layout[i];
        double x, y, w, h;
        const int lit = part_lit(c);
        if (c->kind != CHF_CTL_BUTTON && c->kind != CHF_CTL_RESET) continue;
        part_box(c, &x, &y, &w, &h);
        if (c->kind == CHF_CTL_RESET)
            rrect(dc, x, y + u * 0.06, w, h - u * 0.06, u * 0.3, lit ? RGB(0xff, 0x5a, 0x4c) : RGB(0xa8, 0x2c, 0x24),
                  RGB(0x4a, 0x10, 0x0c), 1.0);
        else
            rrect(dc, x, y + u * 0.06, w, h - u * 0.06, u * 0.3, lit ? C_ACCENT : RGB(0x1c, 0x1c, 0x1f),
                  lit ? C_ACCENT_D : RGB(0x62, 0x62, 0x68), 1.0);
        if (!lit)
            rrect(dc, x + u * 0.12, y + u * 0.1, w - u * 0.24, u * 0.14, u * 0.07, c->kind == CHF_CTL_RESET
                                                                                      ? RGB(0xc8, 0x50, 0x46)
                                                                                      : RGB(0x38, 0x38, 0x3d),
                  RGB(0, 0, 0), 0);
    }
}

/* The case, its walnut strip, the plate behind the panel and the grain. */
static void art_case(HDC dc)
{
    int i;
    rrect(dc, g_case_rc.left, g_case_rc.top, g_case_rc.right - g_case_rc.left, g_case_rc.bottom - g_case_rc.top,
          S(16), C_CASE, C_CASE_E, 1.5);
    rrect(dc, g_wood_rc.left, g_wood_rc.top, g_wood_rc.right - g_wood_rc.left, g_wood_rc.bottom - g_wood_rc.top, S(5),
          RGB(0x6a, 0x47, 0x2f), RGB(0x3a, 0x24, 0x16), 1.0);
    for (i = g_wood_rc.left + S(24); i < g_wood_rc.right - S(40); i += S(53)) {
        POINT a[2];
        a[0].x = AX(i);
        a[0].y = AY(g_wood_rc.top + S(5));
        a[1].x = AX(i + S(22));
        a[1].y = a[0].y;
        stroke(dc, a, 2, RGB(0x57, 0x38, 0x24), 1.0);
        a[0].x = AX(i + S(14));
        a[0].y = AY(g_wood_rc.bottom - S(6));
        a[1].x = AX(i + S(40));
        a[1].y = a[0].y;
        stroke(dc, a, 2, RGB(0x57, 0x38, 0x24), 1.0);
    }
    art_panel(dc);
    art_controller(dc, 0);
    art_controller(dc, 1);
}

/* The labels and numerals over the art, at the window's own size. */
static void text_parts(HDC dc)
{
    int i;
    SetBkMode(dc, TRANSPARENT);
    for (i = 0; i < g_nlayout; i++) {
        const chf_control *c = &g_layout[i];
        RECT r;
        const int lit = part_lit(c);
        part_rect(c, &r);
        switch (c->kind) {
        case CHF_CTL_CAPTION:
            SelectObject(dc, g_bold);
            SetTextColor(dc, C_LIGHT);
            text_line(dc, c->label, &r, DT_LEFT);
            break;
        case CHF_CTL_BUTTON:
        case CHF_CTL_RESET: {
            RECT cap = r, leg;
            cap.bottom = r.top + (LONG)(g_u + 0.5);
            SetRect(&leg, r.left - (int)(g_u * 0.3), cap.bottom + S(1), r.right + (int)(g_u * 0.3),
                    cap.bottom + (int)(g_u * 0.62));
            if (binding_mode() && c->target >= 0) {
                char legend[96];
                chf_controllers_binding_label(c->target, legend, sizeof legend);
                SelectObject(dc, g_tiny);
                SetTextColor(dc, lit ? C_ON_ACC : C_LIGHT);
                text_block(dc, legend, &cap);
            } else if (c->kind == CHF_CTL_BUTTON) {
                SelectObject(dc, g_num);
                SetTextColor(dc, lit ? C_ON_ACC : C_LIGHT);
                text_line(dc, c->label, &cap, DT_CENTER);
            }
            SelectObject(dc, g_leg);
            if (c->kind == CHF_CTL_RESET) {
                SetTextColor(dc, C_ACCENT);                  /* RESET's legend is yellow */
                text_line(dc, c->label, &leg, DT_CENTER);
            } else {
                SetTextColor(dc, C_DIM);
                text_line(dc, c->legend, &leg, DT_CENTER);
            }
            break;
        }
        case CHF_CTL_AXIS:
            if (binding_mode() && c->target >= 0) {
                char legend[96];
                chf_controllers_binding_label(c->target, legend, sizeof legend);
                SelectObject(dc, g_tiny);
                SetTextColor(dc, lit ? C_ON_ACC : C_LIGHT);
                text_block(dc, legend, &r);
            } else {
                const int up = (c->act_mask >> CHF_ACT_PULL) & 1;
                RECT t = r;
                if (up) t.top = r.top + (LONG)((r.bottom - r.top) * 0.52);
                else t.bottom = r.top + (LONG)((r.bottom - r.top) * 0.48);
                SelectObject(dc, g_small);
                SetTextColor(dc, lit ? C_ON_ACC : C_LIGHT);
                text_line(dc, c->label, &t, DT_CENTER);
            }
            break;
        case CHF_CTL_TWIST:
        case CHF_CTL_DIRECTION:
            if (binding_mode() && c->target >= 0) {
                char legend[96];
                chf_controllers_binding_label(c->target, legend, sizeof legend);
                SelectObject(dc, g_tiny);
                SetTextColor(dc, lit ? C_ON_ACC : C_LIGHT);
                if (c->kind == CHF_CTL_DIRECTION) {
                    /* a wedge's box is its bounding box: the legend sits at the middle of the sector */
                    const int k = wedge_sector(c->act_mask);
                    const double a = k * 45.0 * PI_D / 180.0, rad = g_disc[c->port].r * 0.76;
                    const int px = (int)(g_disc[c->port].cx + rad * cos(a)), py = (int)(g_disc[c->port].cy - rad * sin(a));
                    char *nl = strchr(legend, '\n');
                    RECT t;
                    const int lh = S(11), half = (int)(g_u * 0.62);
                    /* a wedge is small: the key over the gamepad button, each on one line */
                    if (nl) *nl = '\0';
                    SetRect(&t, px - half, py - lh, px + half, py);
                    text_line(dc, legend, &t, DT_CENTER | DT_END_ELLIPSIS);
                    SetRect(&t, px - half, py, px + half, py + lh);
                    if (nl) text_line(dc, nl + 1, &t, DT_CENTER | DT_END_ELLIPSIS);
                } else {
                    text_block(dc, legend, &r);
                }
            }
            break;
        default:
            break;
        }
    }
}

/* ---- the strip: gamepads, Map ------------------------------------------------------- */

static void draw_face(HDC dc, const RECT *rc, int pushed, int accent)
{
    RECT r = *rc;
    if (accent) {
        FillRect(dc, &r, g_accent_brush);
        FrameRect(dc, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetTextColor(dc, C_ON_ACC);
    } else {
        DrawFrameControl(dc, &r, DFC_BUTTON, DFCS_BUTTONPUSH | (pushed ? DFCS_PUSHED : 0));
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    }
}

/* In Map mode a target shows what drives it: the key, then the pad button. */
static void chf_controllers_binding_label(int target, char *out, int outsz)
{
    const chf_binding bind = chfsession_binding_get(g_session, target);
    char key[32];
    key[0] = '\0';
    chfsession_keysym_name(bind.keysym, key, sizeof key);
    if (bind.button != CHF_PAD_BTN_NONE)
        snprintf(out, (size_t)outsz, "%s\n%s", key[0] ? key : "\xe2\x80\x94", chf_pad_button_name(bind.button));
    else
        snprintf(out, (size_t)outsz, "%s", key[0] ? key : "\xe2\x80\x94");
}

static const char *strip_label(int b)
{
    switch (b) {
    case SB_MAP: return g_map_state == -2 ? "Map" : "Done";
    case SB_DEFAULTS: return "Defaults";
    case SB_SHOW: return "Show bindings";
    case SB_ACT_CONFIG: return "Reset to CONFIG";
    case SB_ACT_STOP: return "Debugger Stop";
    default: return "Help";
    }
}

static int strip_target(int b)
{
    return b == SB_ACT_CONFIG ? CHF_TARGET_SYSACT(CHF_SYSACT_RESET_CONFIG)
         : b == SB_ACT_STOP ? CHF_TARGET_SYSACT(CHF_SYSACT_DEBUG_STOP) : -1;
}

static void segment_rect(int row, int seg, RECT *rc)
{
    const RECT *r = &g_gp_row_rc[row];
    const int x = r->right - (3 - seg) * (S(SEG_W) + S(4)) + S(4);
    SetRect(rc, x, r->top + 1, x + S(SEG_W), r->bottom - 1);
}

static void gp_name_rect(int row, RECT *rc)
{
    RECT seg0;
    segment_rect(row, 0, &seg0);
    *rc = g_gp_row_rc[row];
    rc->right = seg0.left - S(110);
}

/* One row per gamepad: its name, the player it drives now, and the Automatic /
 * Player 1 / Player 2 choice, the current one pushed in. */
static void paint_gamepads(HDC dc)
{
    static const char *const segs[3] = { "Automatic", "Player 1", "Player 2" };
    const int n = chfsession_gamepad_count(g_session);
    int i, s;

    SelectObject(dc, g_bold);
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    text_line(dc, "Gamepads", &g_gp_head_rc, DT_LEFT);
    SelectObject(dc, g_font);
    SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    if (n == 0) {
        text_line(dc, "No gamepads connected", &g_gp_row_rc[0], DT_LEFT);
        return;
    }
    if (n > PAD_ROWS) {
        char more[48];
        snprintf(more, sizeof more, "%d more not shown", n - PAD_ROWS);
        text_line(dc, more, &g_gp_head_rc, DT_RIGHT);
    }
    for (i = 0; i < n && i < PAD_ROWS; i++) {
        const int assign = chfsession_gamepad_assignment(g_session, i);
        const int eff = chfsession_gamepad_effective_port(g_session, i);
        const int on = assign < 0 || assign >= 2 ? 0 : assign + 1;
        char name[128], line[32];
        RECT name_rc, eff_rc, seg0;

        gp_name_rect(i, &name_rc);
        segment_rect(i, 0, &seg0);
        chfsession_gamepad_name(g_session, i, name, sizeof name);
        SelectObject(dc, g_font);
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        text_line(dc, name, &name_rc, DT_LEFT | DT_END_ELLIPSIS);
        if (eff >= 0) snprintf(line, sizeof line, "drives player %d", eff + 1);
        else snprintf(line, sizeof line, "drives nothing");
        SetRect(&eff_rc, name_rc.right + S(8), name_rc.top, seg0.left - S(8), name_rc.bottom);
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        text_line(dc, line, &eff_rc, DT_RIGHT);
        for (s = 0; s < 3; s++) {
            RECT r;
            segment_rect(i, s, &r);
            draw_face(dc, &r, s == on, 0);
            text_line(dc, segs[s], &r, DT_CENTER);
        }
    }
}

static void paint_strip(HDC dc)
{
    int b;
    SelectObject(dc, g_font);
    for (b = 0; b < SB_COUNT; b++) {
        const int armed = (b == SB_MAP && g_map_state != -2) || (b == SB_SHOW && g_show_bindings) ||
                          (g_map_state >= 0 && strip_target(b) == g_map_state);
        draw_face(dc, &g_btn_rc[b], (b == g_mouse_action) || (b == SB_SHOW && g_show_bindings && 0), armed);
        text_line(dc, strip_label(b), &g_btn_rc[b], DT_CENTER);
    }
    SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    text_line(dc, g_hint, &g_hint_rc, DT_LEFT | DT_END_ELLIPSIS);
}

/* The whole window: the console's art supersampled, its text, the strip. Only
 * what the update region needs is drawn. */
static void paint_rect(HDC dc, const RECT *inv)
{
    RECT art;
    const int k = 2;
    HDC mem;
    HBITMAP bmp;
    void *bits = NULL;
    BITMAPINFO bmi;
    HGDIOBJ old;

    FillRect(dc, inv, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(dc, TRANSPARENT);

    /* the art: the intersection with the case, at 2x */
    IntersectRect(&art, inv, &g_case_rc);
    if (!IsRectEmpty(&art)) {
        const int w = art.right - art.left, h = art.bottom - art.top;
        memset(&bmi, 0, sizeof bmi);
        bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
        bmi.bmiHeader.biWidth = w * k;
        bmi.bmiHeader.biHeight = -h * k;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        mem = CreateCompatibleDC(dc);
        bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (bmp) {
            COLORREF face = GetSysColor(COLOR_BTNFACE);
            HBRUSH fb = CreateSolidBrush(face);
            RECT all = { 0, 0, w * k, h * k };
            old = SelectObject(mem, bmp);
            FillRect(mem, &all, fb);
            DeleteObject(fb);
            SetBkMode(mem, TRANSPARENT);
            g_art_k = k;
            g_art_x = art.left;
            g_art_y = art.top;
            art_case(mem);
            g_art_k = 1;
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, NULL);
            StretchBlt(dc, art.left, art.top, w, h, mem, 0, 0, w * k, h * k, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
        }
        DeleteDC(mem);
        SelectObject(dc, g_font);
        text_parts(dc);
    }
    paint_gamepads(dc);
    paint_strip(dc);
}

/* ---- pressing ----------------------------------------------------------------- */

static void invalidate_part(int i)
{
    RECT r;
    if (!g_panel || i < 0 || i >= g_nlayout) return;
    part_rect(&g_layout[i], &r);
    InflateRect(&r, S(14), S(14));
    InvalidateRect(g_panel, &r, FALSE);
}

static void invalidate_controller(int port)
{
    RECT r;
    int i;
    if (!g_panel) return;
    for (i = 0; i < g_nlayout; i++)
        if (g_layout[i].kind == CHF_CTL_CAPTION && g_layout[i].port == port) {
            part_rect(&g_layout[i], &r);
            r.left -= (int)g_u * 1 / 4;
            r.right += (int)g_u * 1 / 4;
            r.bottom = (int)(g_oy + g_lh * g_u) + (int)g_u / 4;
            InvalidateRect(g_panel, &r, FALSE);
        }
}

static void set_pointer(int port, unsigned mask)
{
    if (port < 0 || port > 1 || g_pointer[port] == mask) return;
    g_pointer[port] = mask;
    chfsession_pointer_set(g_session, port, mask);
}

/* Lets go of what the pointer holds in this window (only that: the main window
 * releases its own keys). A session action fires on release inside its
 * button, like any button. */
static void mouse_release(int fire_action)
{
    const int part = g_mouse_part, act = g_mouse_action, port = g_mouse_port;
    g_mouse_part = -1;
    g_mouse_port = -1;
    g_mouse_btn = 0;
    g_mouse_action = -1;
    set_pointer(0, 0);
    set_pointer(1, 0);
    if (part >= 0 && part < g_nlayout) {
        const chf_control *c = &g_layout[part];
        if (c->kind == CHF_CTL_BUTTON || c->kind == CHF_CTL_RESET)
            chfsession_press(g_session, c->target, 0);
        invalidate_part(part);
    }
    if (act >= 0) {
        if (fire_action) chfsession_sysaction_post(g_session, act == SB_ACT_CONFIG ? CHF_SYSACT_RESET_CONFIG
                                                                                    : CHF_SYSACT_DEBUG_STOP);
        if (g_panel) InvalidateRect(g_panel, &g_btn_rc[act], FALSE);
    }
    if (port >= 0) invalidate_controller(port);
}

static void set_hint(const char *text)
{
    snprintf(g_hint, sizeof g_hint, "%s", text);
    if (g_panel) InvalidateRect(g_panel, &g_hint_rc, FALSE);
}

#define IDLE_HINT "Drag the knob to tilt it, press it to push, right-press to pull, wheel to twist. Map rebinds."

static void set_map_state(int state)
{
    g_map_state = state;
    if (state < 0) {
        if (g_panel) KillTimer(g_panel, IDT_CAPTURE);
        chfsession_gamepad_capture_cancel(g_session);
    }
    if (state == -2) {
        set_hint(IDLE_HINT);
    } else if (state == -1) {
        set_hint("Click a control to remap. Esc finishes.");
    } else {
        char msg[200];
        mouse_release(0);
        snprintf(msg, sizeof msg, "Press a key or a gamepad button for %s. Esc cancels.", chf_target_name(state));
        set_hint(msg);
        chfsession_gamepad_capture_begin(g_session);
        if (g_panel) SetTimer(g_panel, IDT_CAPTURE, 50, NULL);
    }
    if (g_panel) {
        InvalidateRect(g_panel, NULL, FALSE);
        refresh_held();
    }
}

/* After a rebind: what it is now, and whom it was taken from. */
static void bound_hint(const char *what, const char *stolen)
{
    char msg[260];
    if (stolen[0]) snprintf(msg, sizeof msg, "Bound %s (it was %s's)", what, stolen);
    else snprintf(msg, sizeof msg, "Bound %s", what);
    set_hint(msg);
}

/* ---- layout --------------------------------------------------------------------- */

static int machine_now(void)
{
    const int m = chfsession_get_int(g_session, "machine", CHF_MACHINE_CHANNELF);
    return m >= 0 && m < CHF_MACHINE_COUNT ? m : CHF_MACHINE_CHANNELF;
}

/* The case with the console in it, the gamepad rows, the Map row: the window
 * is this size always. */
static void layout_static(void)
{
    const int left = S(MARGIN);
    int y = S(MARGIN), i, x, content_w;
    static const int bw[SB_COUNT] = { MAP_W, DEFAULTS_W, SHOW_W, ACT_W, ACT_W, HELP_W };

    g_layout = chfsession_controls_layout(machine_now(), &g_nlayout, &g_lw, &g_lh);
    g_u = (double)U96 * g_dpi / 96.0;
    content_w = (int)ceil((double)g_lw * g_u) + 2 * S(CASE_PAD);
    g_total_w = 2 * left + content_w;

    SetRect(&g_case_rc, left, y, left + content_w, 0);
    SetRect(&g_wood_rc, g_case_rc.left + S(10), g_case_rc.top + S(10), g_case_rc.right - S(10),
            g_case_rc.top + S(10) + S(WOOD_H));
    g_ox = g_case_rc.left + S(CASE_PAD);
    g_oy = g_wood_rc.bottom + S(16);
    g_case_rc.bottom = (int)ceil(g_oy + (double)g_lh * g_u) + S(CASE_PAD);
    y = g_case_rc.bottom + S(12);

    for (i = 0; i < g_nlayout; i++) {
        const chf_control *c = &g_layout[i];
        if (c->kind == CHF_CTL_STICK && c->port >= 0 && c->port < 2) {
            g_disc[c->port].cx = g_ox + ((double)c->x + c->w / 2) * g_u;
            g_disc[c->port].cy = g_oy + ((double)c->y + c->h / 2) * g_u;
            g_disc[c->port].r = (double)c->w / 2 * g_u;
        }
    }

    SetRect(&g_gp_head_rc, left, y, left + content_w, y + S(HEAD_H));
    y += S(HEAD_H);
    for (i = 0; i < PAD_ROWS; i++) {
        SetRect(&g_gp_row_rc[i], left, y, left + content_w, y + S(ROW_H));
        y += S(ROW_H) + S(2);
    }
    SetRect(&g_gp_rc, left, g_gp_head_rc.top, left + content_w, y);
    y += S(8);

    x = left;
    for (i = 0; i < SB_COUNT; i++) {
        if (i == SB_HELP) x = left + content_w - S(HELP_W);
        SetRect(&g_btn_rc[i], x, y, x + S(bw[i]), y + S(BTN_H));
        x += S(bw[i]) + S(6);
        if (i == SB_SHOW) x += S(8);
    }
    y += S(BTN_H) + S(6);
    SetRect(&g_hint_rc, left, y, left + content_w, y + S(20));
    g_total_h = y + S(20) + S(MARGIN);
}

static void rebuild(void)
{
    mouse_release(0);
    if (g_panel && GetCapture() == g_panel) ReleaseCapture();
    g_built_machine = machine_now();
    make_fonts();
    layout_static();
    if (g_panel) {
        InvalidateRect(g_panel, NULL, FALSE);
        refresh_held();
    }
}

/* ---- polling -------------------------------------------------------------------------- */

/* What the console sees held, lit. */
static void refresh_held(void)
{
    unsigned held[2], keys = 0;
    int port, k, reset;
    if (!g_panel || g_map_state != -2) return;
    for (port = 0; port < 2; port++)
        held[port] = chfsession_buttons_held(g_session, port);
    for (k = 0; k < CHF_KEY_COUNT; k++)
        if (chfsession_key_held(g_session, k)) keys |= 1u << k;
    reset = chfsession_switch_held(g_session, CHF_SW_RESET) != 0;
    for (port = 0; port < 2; port++)
        if (held[port] != g_shown_held[port]) {
            g_shown_held[port] = held[port];
            invalidate_controller(port);
        }
    if (keys != g_shown_keys || reset != g_shown_reset) {
        int i;
        for (i = 0; i < g_nlayout; i++) {
            const chf_control *c = &g_layout[i];
            if (c->kind == CHF_CTL_BUTTON && (((keys ^ g_shown_keys) >> (c->target - CHF_TARGET_KEY(0))) & 1))
                invalidate_part(i);
            else if (c->kind == CHF_CTL_RESET && reset != g_shown_reset)
                invalidate_part(i);
        }
        g_shown_keys = keys;
        g_shown_reset = reset;
    }
}

/* Hot-plugged pads, auto-assignment changes, and the machine changed in
 * Preferences. */
static void refresh_pads(void)
{
    pads_state now;
    int i;
    memset(&now, 0, sizeof now);
    now.generation = chfsession_gamepad_generation(g_session);
    now.count = chfsession_gamepad_count(g_session);
    for (i = 0; i < PAD_ROWS && i < now.count; i++) {
        now.assign[i] = chfsession_gamepad_assignment(g_session, i);
        now.effective[i] = chfsession_gamepad_effective_port(g_session, i);
    }
    if (memcmp(&now, &g_pads_shown, sizeof now) != 0) {
        g_pads_shown = now;
        InvalidateRect(g_panel, &g_gp_rc, FALSE);
    }
    if (g_built_machine != machine_now())
        rebuild();
}

/* ---- keys ------------------------------------------------------------------------------ */

static void hide_panel(void)
{
    set_map_state(-2);
    mouse_release(0);
    if (GetCapture() == g_panel) ReleaseCapture();
    g_swallow_vk = 0;
    ShowWindow(g_panel, SW_HIDE);
}

/* A key while Map is armed: 1 if Map took it. Waiting for a target, a key is
 * not typing; waiting for a binding, the key is the binding (not F9, F11 or
 * F12, which are the app's). */
int chf_controllers_map_key(UINT msg, WPARAM vk, LPARAM lp)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;

    if (!g_panel) return 0;
    if (g_swallow_vk && vk == g_swallow_vk) {
        if (!down) g_swallow_vk = 0;
        return 1;
    }
    if (g_map_state == -2) return 0;
    if (vk == VK_F9 || vk == VK_F11 || vk == VK_F12) return 0;
    if (msg == WM_SYSKEYDOWN && vk == VK_F4) return 0;
    if (!down || (lp & (1 << 30))) return 1;
    if (vk == VK_ESCAPE) {
        set_map_state(g_map_state >= 0 ? -1 : -2);
        g_swallow_vk = vk;
        return 1;
    }
    if (g_map_state >= 0) {
        char stolen[128], name[32];
        const int target = g_map_state;
        const uint32_t ks = chf_keysym_from_msg(vk, lp);
        if (!ks) return 1;              /* nothing to call it: keep waiting */
        stolen[0] = '\0';
        chfsession_binding_set_key(g_session, target, ks, stolen, sizeof stolen);
        chfsession_keysym_name(ks, name, sizeof name);
        g_swallow_vk = vk;
        set_map_state(-1);              /* stay armed: remapping several in a row is normal */
        bound_hint(name, stolen);
    }
    return 1;
}

/* A key with the window focused: 1 if taken. */
static int panel_key(UINT msg, WPARAM vk, LPARAM lp)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    uint32_t ks;
    int sa;

    if (chf_controllers_map_key(msg, vk, lp)) return 1;
    if (vk == VK_F9) {
        if (down && !(lp & (1 << 30))) hide_panel();
        return 1;
    }
    if (chf_app_key(vk)) {
        /* the main window's shortcuts, as if it had the focus */
        if (g_parent && down) SendMessageA(g_parent, msg, vk, lp);
        return 1;
    }
    ks = chf_keysym_from_msg(vk, lp);
    if (down && msg == WM_KEYDOWN && ks && (sa = chfsession_key_sysaction(g_session, ks)) >= 0) {
        /* the main window runs these */
        if (!(lp & (1 << 30))) chfsession_sysaction_post(g_session, sa);
        return 1;
    }
    return chf_forward_key(g_session, msg, vk, lp);
}

/* Exactly the laid-out client size, whatever the frame is at this DPI. */
static void fit_client(void)
{
    RECT wr, cr;
    GetWindowRect(g_panel, &wr);
    GetClientRect(g_panel, &cr);
    if (cr.right == g_total_w && cr.bottom == g_total_h) return;
    SetWindowPos(g_panel, NULL, 0, 0, (wr.right - wr.left) - cr.right + g_total_w,
                 (wr.bottom - wr.top) - cr.bottom + g_total_h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* The window's outer size for the laid-out client. */
static void window_size(HWND hwnd, SIZE *sz)
{
    RECT wr, cr;
    if (hwnd && GetWindowRect(hwnd, &wr) && GetClientRect(hwnd, &cr) && cr.right > 0) {
        sz->cx = (wr.right - wr.left) - cr.right + g_total_w;
        sz->cy = (wr.bottom - wr.top) - cr.bottom + g_total_h;
    } else {
        SetRect(&wr, 0, 0, g_total_w, g_total_h);
        AdjustWindowRectEx(&wr, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
        sz->cx = wr.right - wr.left;
        sz->cy = wr.bottom - wr.top;
    }
}

static void rescale(int dpi)
{
    g_dpi = dpi;
    rebuild();
}

/* ---- the mouse ------------------------------------------------------------------------ */

/* The layout part under a client point, or -1. */
static int part_at(POINT p)
{
    if (!g_layout) return -1;
    return chfsession_controls_hit(machine_now(), (float)((p.x - g_ox) / g_u), (float)((p.y - g_oy) / g_u));
}

/* The controller whose block (the whole of both its views) holds the point,
 * or -1. */
static int controller_at(POINT p)
{
    int i;
    const double ux = (p.x - g_ox) / g_u, uy = (p.y - g_oy) / g_u;
    for (i = 0; i < g_nlayout; i++) {
        const chf_control *c = &g_layout[i];
        if (c->kind == CHF_CTL_CAPTION && ux >= c->x && ux < c->x + c->w && uy >= c->y && uy < g_lh)
            return c->port;
    }
    return -1;
}

/* A stick drag: where the pointer is on the disc decides what is held. On the
 * knob cap (the inner half) the pressed button works the plunger -- left
 * pushes, right pulls; out over the wedges, and beyond them while dragging,
 * the stick is tilted eight ways with nothing near the middle. */
static void stick_follow(int port, POINT p)
{
    const double dx = p.x - g_disc[port].cx, dy = p.y - g_disc[port].cy, r = g_disc[port].r;
    unsigned mask;
    if (sqrt(dx * dx + dy * dy) < r * 0.5)
        mask = 1u << (g_mouse_btn == 2 ? CHF_ACT_PULL : CHF_ACT_PUSH);
    else
        mask = chfsession_stick_from_vector((float)dx, (float)dy, (float)r);
    set_pointer(port, mask);
    invalidate_controller(port);
}

static void mouse_down(HWND hwnd, POINT p, int button)
{
    int i, row, seg, b;
    SetFocus(hwnd);

    for (b = 0; b < SB_COUNT; b++) {
        if (!PtInRect(&g_btn_rc[b], p)) continue;
        if (button != 1) return;
        switch (b) {
        case SB_MAP: set_map_state(g_map_state == -2 ? -1 : -2); return;
        case SB_DEFAULTS:
            chfsession_bindings_reset(g_session);
            set_hint("Every key and button is back to its default");
            InvalidateRect(hwnd, NULL, FALSE);
            return;
        case SB_SHOW:
            g_show_bindings = !g_show_bindings;
            InvalidateRect(hwnd, NULL, FALSE);
            return;
        case SB_HELP: chf_help_show(hwnd, "controllers"); return;
        default:
            if (g_map_state != -2) {                 /* armed: picks the action */
                set_map_state(strip_target(b));
            } else {
                mouse_release(0);
                g_mouse_action = b;
                SetCapture(hwnd);
                InvalidateRect(hwnd, &g_btn_rc[b], FALSE);
            }
            return;
        }
    }
    for (row = 0; row < PAD_ROWS && row < chfsession_gamepad_count(g_session); row++)
        for (seg = 0; seg < 3; seg++) {
            RECT r;
            segment_rect(row, seg, &r);
            if (PtInRect(&r, p) && button == 1) {
                /* 0 = automatic (connection order), 1-2 = that player */
                chfsession_gamepad_assign(g_session, row, seg - 1);
                refresh_pads();
                InvalidateRect(hwnd, &g_gp_rc, FALSE);
                return;
            }
        }

    i = part_at(p);
    if (i < 0) return;
    {
        const chf_control *c = &g_layout[i];
        if (g_map_state != -2) {
            /* armed: picks (or changes) the target */
            if (button == 1 && c->target >= 0 && c->kind != CHF_CTL_STICK) set_map_state(c->target);
            return;
        }
        mouse_release(0);
        switch (c->kind) {
        case CHF_CTL_STICK:
        case CHF_CTL_DIRECTION:
            g_mouse_port = c->port;
            g_mouse_btn = button;
            stick_follow(c->port, p);
            break;
        case CHF_CTL_BUTTON:
        case CHF_CTL_RESET:
            if (button != 1) return;
            g_mouse_part = i;
            chfsession_press(g_session, c->target, 1);
            break;
        case CHF_CTL_TWIST:
        case CHF_CTL_AXIS:
            if (button != 1) return;
            g_mouse_part = i;
            set_pointer(c->port, c->act_mask);
            invalidate_controller(c->port);
            break;
        default:
            return;
        }
        invalidate_part(i);
        SetCapture(hwnd);
        refresh_held();
    }
}

static void mouse_move(HWND hwnd, POINT p)
{
    if (g_mouse_port >= 0) {
        stick_follow(g_mouse_port, p);
        return;
    }
    if (g_mouse_part >= 0) {
        /* dragged off what it holds: let go, as a button does */
        if (g_mouse_part >= g_nlayout || part_at(p) != g_mouse_part) {
            mouse_release(0);
            if (GetCapture() == hwnd) ReleaseCapture();
            refresh_held();
        }
        return;
    }
    if (g_mouse_action >= 0 && !PtInRect(&g_btn_rc[g_mouse_action], p)) {
        mouse_release(0);
        if (GetCapture() == hwnd) ReleaseCapture();
    }
}

static void mouse_up(HWND hwnd, int button)
{
    if (g_mouse_port >= 0 && button != g_mouse_btn) return;     /* the other button */
    if (g_mouse_part >= 0 || g_mouse_port >= 0 || g_mouse_action >= 0) {
        mouse_release(1);
        refresh_held();
    }
    if (GetCapture() == hwnd) ReleaseCapture();
}

/* ---- the window ----------------------------------------------------------------------- */

static LRESULT CALLBACK panel_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        /* Off-screen first: the held highlights repaint at 25 Hz. */
        RECT inv = ps.rcPaint;
        HDC mem;
        HBITMAP bmp;
        HGDIOBJ oldbmp, oldfont;
        const int w = inv.right - inv.left, h = inv.bottom - inv.top;
        if (w <= 0 || h <= 0) { EndPaint(hwnd, &ps); return 0; }
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, w, h);
        oldbmp = SelectObject(mem, bmp);
        oldfont = SelectObject(mem, g_font);
        SetViewportOrgEx(mem, -inv.left, -inv.top, NULL);
        paint_rect(mem, &inv);
        SetViewportOrgEx(mem, 0, 0, NULL);
        BitBlt(dc, inv.left, inv.top, w, h, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldfont);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: {
        POINT p;
        p.x = GET_X_LPARAM(lp);
        p.y = GET_Y_LPARAM(lp);
        mouse_down(hwnd, p, msg == WM_LBUTTONDOWN ? 1 : 2);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p;
        p.x = GET_X_LPARAM(lp);
        p.y = GET_Y_LPARAM(lp);
        mouse_move(hwnd, p);
        return 0;
    }
    case WM_LBUTTONUP: case WM_RBUTTONUP:
        /* released wherever the mouse ended up: capture means this arrives
         * even if the pointer left the part */
        mouse_up(hwnd, msg == WM_LBUTTONUP ? 1 : 2);
        return 0;
    case WM_CAPTURECHANGED:
        /* lost the mouse some other way (Alt+Tab mid-press): let go */
        if ((HWND)lp != hwnd) {
            mouse_release(0);
            refresh_held();
        }
        return 0;
    case WM_MOUSEWHEEL: {
        /* a notch twists the controller under the pointer; a smooth wheel adds
         * up to notches. Down is clockwise. */
        POINT p;
        int port;
        p.x = GET_X_LPARAM(lp);
        p.y = GET_Y_LPARAM(lp);
        ScreenToClient(hwnd, &p);
        port = g_map_state == -2 ? controller_at(p) : -1;
        if (port < 0) return 0;
        g_wheel_acc += GET_WHEEL_DELTA_WPARAM(wp);
        while (g_wheel_acc >= WHEEL_DELTA) { chfsession_twist_pulse(g_session, port, -1); g_wheel_acc -= WHEEL_DELTA; }
        while (g_wheel_acc <= -WHEEL_DELTA) { chfsession_twist_pulse(g_session, port, 1); g_wheel_acc += WHEEL_DELTA; }
        return 0;
    }

    case WM_TIMER:
        if (wp == IDT_CAPTURE) {
            int button;
            if (g_map_state < 0) {
                KillTimer(hwnd, IDT_CAPTURE);
            } else if (chfsession_gamepad_capture_poll(g_session, &button)) {
                char stolen[128];
                const int target = g_map_state;
                stolen[0] = '\0';
                chfsession_binding_set_button(g_session, target, button, stolen, sizeof stolen);
                set_map_state(-1);   /* stay armed: remapping several in a row is normal */
                bound_hint(chf_pad_button_name(button), stolen);
            }
        } else if (wp == IDT_HELD) {
            refresh_held();
        } else if (wp == IDT_PADS) {
            refresh_pads();
        }
        return 0;

    /* WM_SYSKEY* too: a bound Alt is the console's */
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
        if (panel_key(msg, wp, lp)) return 0;
        break;
    case WM_SYSCHAR:
        /* Alt+letter: the main window's menu mnemonics, as when it has the focus */
        if (wp != ' ' && g_parent) { SendMessageA(g_parent, msg, wp, lp); return 0; }
        break;

    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            const HWND other = (HWND)lp;
            mouse_release(0);
            if (GetCapture() == hwnd) ReleaseCapture();
            g_swallow_vk = 0;       /* its release goes elsewhere */
            /* Leaving the app: nothing typed here stays held (within it, the
             * window that gets the key-up lets go). */
            if (!other || GetWindowThreadProcessId(other, NULL) != GetCurrentThreadId())
                chfsession_release_all(g_session);
            refresh_held();
        }
        break;                      /* DefWindowProc gives the window the focus */
    case WM_SHOWWINDOW:
        if (wp) {
            refresh_held();
            SetTimer(hwnd, IDT_HELD, 40, NULL);
            SetTimer(hwnd, IDT_PADS, 250, NULL);
        } else {
            KillTimer(hwnd, IDT_HELD);
            KillTimer(hwnd, IDT_PADS);
        }
        break;
    case WM_GETMINMAXINFO:
        /* Fixed: the size limits say so too, so neither maximizing nor a
         * compositor (Wine passes them on) makes it any other size. */
        if (g_total_w) {
            MINMAXINFO *mm = (MINMAXINFO *)lp;
            SIZE sz;
            window_size(hwnd, &sz);
            mm->ptMinTrackSize.x = mm->ptMaxTrackSize.x = mm->ptMaxSize.x = sz.cx;
            mm->ptMinTrackSize.y = mm->ptMaxTrackSize.y = mm->ptMaxSize.y = sz.cy;
            return 0;
        }
        break;
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)lp;
        rescale(HIWORD(wp));
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        fit_client();
        return 0;
    }
    case WM_CLOSE:
        /* Hide, do not destroy: a remap and the position survive closing it. */
        hide_panel();
        return 0;
    case WM_DESTROY:
        /* with the main window: the next toggle starts afresh */
        mouse_release(0);
        set_map_state(-2);
        g_panel = NULL;
        g_built_machine = -1;
        free_fonts();
        break;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static int create_panel(void)
{
    static int registered;
    HINSTANCE inst = GetModuleHandleA(NULL);
    RECT want;

    if (!registered) {
        WNDCLASSEXA wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = panel_proc;    /* no CS_DBLCLKS: fast clicks stay press/release pairs */
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = PANEL_CLASS;
        registered = RegisterClassExA(&wc) != 0;
    }
    if (!g_accent_brush)
        g_accent_brush = CreateSolidBrush(rgb_of(CHFSESSION_ACCENT_RGB));
    g_dpi = chf_window_dpi(g_parent);
    make_fonts();
    layout_static();

    SetRect(&want, 0, 0, g_total_w, g_total_h);
    /* A tool window with a caption and no thick frame or maximize box: it is
     * exactly the size of its contents, and stays off the taskbar. */
    AdjustWindowRectEx(&want, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
    g_panel = CreateWindowExA(PANEL_EXSTYLE, PANEL_CLASS, "Controllers", PANEL_STYLE,
                              CW_USEDEFAULT, CW_USEDEFAULT, want.right - want.left, want.bottom - want.top,
                              g_parent, NULL, inst, NULL);
    if (!g_panel) return 0;
    chf_help_set_context(g_panel, "controllers", 0);    /* F1 is the console's RESET here */
    rescale(chf_window_dpi(g_panel));    /* placed on another monitor than the main window */
    fit_client();
    memset(&g_pads_shown, 0, sizeof g_pads_shown);
    g_shown_keys = g_shown_reset = 0;
    memset(g_shown_held, 0, sizeof g_shown_held);
    set_map_state(-2);
    return 1;
}

static void show_panel(void)
{
    if (g_built_machine != machine_now())
        rebuild();
    refresh_pads();
    ShowWindow(g_panel, SW_SHOW);
    SetForegroundWindow(g_panel);
}

void chf_controllers_window_toggle(HWND parent, chfsession *session)
{
    g_session = session;
    if (parent)
        g_parent = parent;
    if (!g_panel && !create_panel())
        return;
    if (IsWindowVisible(g_panel))
        hide_panel();
    else
        show_panel();
}

int chf_controllers_window_visible(void)
{
    return g_panel && IsWindowVisible(g_panel);
}

void chf_controllers_window_refresh(void)
{
    if (!g_panel)
        return;
    if (IsWindowVisible(g_panel)) {
        refresh_pads();             /* rebuilds if the machine changed */
        InvalidateRect(g_panel, NULL, FALSE);
    } else {
        g_built_machine = -1;       /* rebuilt when next shown */
    }
}
