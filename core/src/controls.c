/*
 * controls -- the on-screen controllers: one layout for every toolkit, its hit
 * test, the stick's gesture maths and the label every console button has.
 *
 * The Channel F has no keyboard. Its console has four numbered buttons (1
 * TIME, 2 MODE, 3 HOLD, 4 START; bit n-1 of port 0) and a RESET, and each of
 * its two hand controllers is one knob with four axes (chfsession.h):
 * tilted eight ways, twisted either way, pulled up and pushed down. The
 * window draws the console's panel across the top, the LEFT controller
 * (Player 2) on the left and the RIGHT controller (Player 1) on the right;
 * each controller twice, as two views of the same knob:
 *
 *   from above   the STICK: a disc, FORWARD (away from the player) at the top,
 *                BACK at the bottom, RIGHT on the right. The knob cap is the
 *                inner half of the disc; round it, eight DIRECTION wedges.
 *                The TWIST arrows sit either side of the disc (counter-
 *                clockwise on the left).
 *   from the side  PULL above, PUSH below, the handle between them (SIDE:
 *                drawn, its knob rises and sinks with the axis).
 *
 * Units: a console button's width is 1.0. The extents are the controls'
 * bounding box; a toolkit adds its own margin. A button's legend ("TIME") is
 * drawn centred just below its cap (the layout leaves 0.7 for it), and
 * RESET's label sits there too, in the accent colour.
 *
 * Wedges are annular sectors, so their rectangles are only bounding boxes: a
 * toolkit paints a wedge as the sector of the STICK disc whose direction is
 * its act_mask, between half the disc's radius and all of it, and uses
 * chfsession_controls_hit (exact shapes) for hit testing.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <pthread.h>
#include <stddef.h>

#include "chfsession.h"

#define PI_F 3.14159265358979f

/* ---- geometry (layout units) ------------------------------------------------ */

#define BTN       1.0f
#define PANEL_Y   0.0f
#define PANEL_H   (BTN + 0.7f)                 /* the cap and its legend */
#define BLOCK_W   8.8f
#define BLOCK_GAP 0.8f
#define BLOCK_Y   (PANEL_H + 0.6f)
#define TWIST_W   1.1f
#define DISC_D    4.0f
#define DISC_R    (DISC_D / 2)
#define SIDE_W    1.8f
#define VIEW_Y    0.9f                         /* below the caption */
#define VIEW_H    4.0f

#define N_PANEL   5                            /* RESET and the four buttons */
#define N_CTL     15                           /* per controller */
#define N_TOTAL   (N_PANEL + 2 * N_CTL)

static chf_control g_layout[N_TOTAL];
/* the exact shape of the round parts, parallel to g_layout */
typedef struct {
    float cx, cy;          /* the disc's centre */
    float r0, r1;          /* the ring (wedges); a STICK is a disc of r1 */
    int sector;            /* wedges: 0..7, 0 = RIGHT, counter-clockwise on screen */
} shape;
static shape g_shape[N_TOTAL];
static float g_w, g_h;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

/* The eight sectors in order round the disc from RIGHT, counter-clockwise as
 * drawn (so FORWARD, at the top, is 2). */
static const unsigned k_sector_acts[8] = {
    1u << CHF_ACT_RIGHT,
    (1u << CHF_ACT_FORWARD) | (1u << CHF_ACT_RIGHT),
    1u << CHF_ACT_FORWARD,
    (1u << CHF_ACT_FORWARD) | (1u << CHF_ACT_LEFT),
    1u << CHF_ACT_LEFT,
    (1u << CHF_ACT_BACK) | (1u << CHF_ACT_LEFT),
    1u << CHF_ACT_BACK,
    (1u << CHF_ACT_BACK) | (1u << CHF_ACT_RIGHT),
};
static const char *const k_sector_names[8] = {
    "Right", "Forward right", "Forward", "Forward left",
    "Left", "Back left", "Back", "Back right",
};

static const char *const k_key_labels[CHF_KEY_COUNT] = { "1", "2", "3", "4" };
static const char *const k_key_legends[CHF_KEY_COUNT] = { "TIME", "MODE", "HOLD", "START" };

static int lowest_bit(unsigned mask)
{
    int b = 0;
    while (mask && !(mask & 1u)) { mask >>= 1; b++; }
    return b;
}

static void add(int *n, int kind, const char *label, const char *legend, int port,
                int target, unsigned acts, float x, float y, float w, float h)
{
    chf_control *c = &g_layout[*n];
    c->kind = kind;
    c->label = label;
    c->legend = legend;
    c->port = port;
    c->target = target;
    c->act_mask = acts;
    c->x = x; c->y = y; c->w = w; c->h = h;
    g_shape[*n].sector = -1;
    (*n)++;
}

/* The bounding box of the ring sector k round (cx, cy). */
static void sector_box(float cx, float cy, float r0, float r1, int k,
                       float *x, float *y, float *w, float *h)
{
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    const float a0 = ((float)k * 45.0f - 22.5f) * PI_F / 180.0f;

    for (int i = 0; i <= 8; i++) {
        const float a = a0 + (float)i * (45.0f * PI_F / 180.0f) / 8.0f;
        for (int j = 0; j < 2; j++) {
            const float r = j ? r1 : r0;
            const float px = cx + r * cosf(a), py = cy - r * sinf(a);
            if (px < minx) minx = px;
            if (px > maxx) maxx = px;
            if (py < miny) miny = py;
            if (py > maxy) maxy = py;
        }
    }
    *x = minx; *y = miny; *w = maxx - minx; *h = maxy - miny;
}

static void controller(int *n, int port, float x0, float y0,
                       const char *caption, const char *const side_labels[2])
{
    const float yt = y0 + VIEW_Y;
    const float dx = x0 + TWIST_W + 0.2f;      /* the disc's left edge */
    const float cx = dx + DISC_R, cy = yt + VIEW_H / 2;
    const float xs = x0 + BLOCK_W - SIDE_W;     /* the side view */
    const int base = CHF_TARGET_PORT(port, 0);

    add(n, CHF_CTL_CAPTION, caption, NULL, port, -1, 0, x0, y0, BLOCK_W, 0.7f);

    add(n, CHF_CTL_STICK, "Stick", NULL, port, -1, 0, dx, yt, DISC_D, DISC_D);
    g_shape[*n - 1] = (shape){ cx, cy, 0.0f, DISC_R, -1 };

    for (int k = 0; k < 8; k++) {
        const unsigned acts = k_sector_acts[k];
        float x, y, w, h;
        sector_box(cx, cy, DISC_R / 2, DISC_R, k, &x, &y, &w, &h);
        add(n, CHF_CTL_DIRECTION, k_sector_names[k], NULL, port,
            /* a diagonal drives two contacts: no single target */
            (acts & (acts - 1)) ? -1 : base + lowest_bit(acts), acts, x, y, w, h);
        g_shape[*n - 1] = (shape){ cx, cy, DISC_R / 2, DISC_R, k };
    }

    add(n, CHF_CTL_TWIST, "Twist CCW", NULL, port, base + CHF_ACT_TWIST_CCW,
        1u << CHF_ACT_TWIST_CCW, x0, yt + 1.0f, TWIST_W, 2.0f);
    add(n, CHF_CTL_TWIST, "Twist CW", NULL, port, base + CHF_ACT_TWIST_CW,
        1u << CHF_ACT_TWIST_CW, dx + DISC_D + 0.2f, yt + 1.0f, TWIST_W, 2.0f);

    add(n, CHF_CTL_AXIS, "PULL", NULL, port, base + CHF_ACT_PULL,
        1u << CHF_ACT_PULL, xs, yt, SIDE_W, 1.2f);
    add(n, CHF_CTL_SIDE, side_labels[0], side_labels[1], port, -1, 0,
        xs, yt + 1.4f, SIDE_W, 1.2f);
    add(n, CHF_CTL_AXIS, "PUSH", NULL, port, base + CHF_ACT_PUSH,
        1u << CHF_ACT_PUSH, xs, yt + 2.8f, SIDE_W, 1.2f);
}

static void build(void)
{
    static const char *const side[2] = { "Side view", NULL };
    const float total_w = 2 * BLOCK_W + BLOCK_GAP;
    const float panel_w = 2.0f + 3 * 1.6f + BTN;      /* RESET, then four buttons */
    const float xp = (total_w - panel_w) / 2;
    int n = 0;

    /* the console's panel across the top */
    add(&n, CHF_CTL_RESET, "RESET", NULL, -1, CHF_TARGET_SWITCH(CHF_SW_RESET), 0,
        xp, PANEL_Y, BTN, BTN);
    for (int k = 0; k < CHF_KEY_COUNT; k++)
        add(&n, CHF_CTL_BUTTON, k_key_labels[k], k_key_legends[k], -1,
            CHF_TARGET_KEY(k), 0, xp + 2.0f + (float)k * 1.6f, PANEL_Y, BTN, BTN);

    /* the left controller (Player 2, CPU port 4), then the right one (Player
     * 1, CPU port 1) */
    controller(&n, 1, 0.0f, BLOCK_Y, "Left controller - Player 2", side);
    controller(&n, 0, BLOCK_W + BLOCK_GAP, BLOCK_Y, "Right controller - Player 1", side);

    g_w = total_w;
    g_h = BLOCK_Y + VIEW_Y + VIEW_H;
}

static void ensure_built(void)
{
    pthread_once(&g_once, build);
}

const chf_control *chfsession_controls_layout(int machine, int *n, float *width, float *height)
{
    (void)machine;                       /* every machine prints the same panel */
    ensure_built();
    if (n) *n = N_TOTAL;
    if (width) *width = g_w;
    if (height) *height = g_h;
    return g_layout;
}

/* The sector (0 = RIGHT, counter-clockwise on screen) a vector lies in. */
static int sector_of(float dx, float dy)
{
    float deg = atan2f(-dy, dx) * 180.0f / PI_F;      /* +y is toward the player */
    int k;

    if (deg < 0) deg += 360.0f;
    k = (int)floorf((deg + 22.5f) / 45.0f);
    return k & 7;
}

int chfsession_controls_hit(int machine, float x, float y)
{
    (void)machine;
    ensure_built();
    for (int i = N_TOTAL - 1; i >= 0; i--) {
        const chf_control *c = &g_layout[i];
        const shape *sh = &g_shape[i];

        if (c->kind == CHF_CTL_STICK || c->kind == CHF_CTL_DIRECTION) {
            const float dx = x - sh->cx, dy = y - sh->cy;
            const float d = sqrtf(dx * dx + dy * dy);

            if (d > sh->r1 || d < sh->r0)
                continue;
            if (c->kind == CHF_CTL_DIRECTION && sector_of(dx, dy) != sh->sector)
                continue;
            return i;
        }
        if (x >= c->x && x < c->x + c->w && y >= c->y && y < c->y + c->h)
            return i;
    }
    return -1;
}

unsigned chfsession_stick_from_vector(float dx, float dy, float radius)
{
    if (radius <= 0.0f)
        return 0;
    if (sqrtf(dx * dx + dy * dy) < radius * 0.25f)
        return 0;
    return k_sector_acts[sector_of(dx, dy)];
}

const char *chfsession_key_label(int machine, int key)
{
    (void)machine;
    return key >= 0 && key < CHF_KEY_COUNT ? k_key_labels[key] : NULL;
}

const char *chfsession_key_legend(int machine, int key)
{
    (void)machine;
    return key >= 0 && key < CHF_KEY_COUNT ? k_key_legends[key] : NULL;
}
