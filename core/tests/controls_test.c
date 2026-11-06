/*
 * controls_test -- the on-screen controllers' layout, hit test and gesture
 * maths, pure: the console's panel across the top (RESET, 1 TIME, 2 MODE, 3
 * HOLD, 4 START), the left controller (Player 2) on the left and the right
 * one (Player 1) on the right, each with a caption, a stick disc ringed by
 * eight direction wedges, two twist arrows, and a side view with PULL above
 * and PUSH below. No two controls overlap, every binding target is on exactly
 * one control, the hit test agrees with the wedges and with
 * chfsession_stick_from_vector. No machine is started.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "chfsession.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

#define EPS 1e-3f
#define B(a) (1u << (a))

static int lowest(unsigned m)
{
    int b = 0;
    while (m && !(m & 1u)) { m >>= 1; b++; }
    return b;
}

static int is_round(const chf_control *c)
{
    return c->kind == CHF_CTL_STICK || c->kind == CHF_CTL_DIRECTION;
}

static int overlap(const chf_control *a, const chf_control *b)
{
    return a->x < b->x + b->w - EPS && b->x < a->x + a->w - EPS
        && a->y < b->y + b->h - EPS && b->y < a->y + a->h - EPS;
}

static int count_kind(const chf_control *c, int n, int kind, int port)
{
    int k = 0;
    for (int i = 0; i < n; i++)
        if (c[i].kind == kind && c[i].port == port) k++;
    return k;
}

static int find(const chf_control *c, int n, int kind, int port, unsigned acts)
{
    for (int i = 0; i < n; i++)
        if (c[i].kind == kind && c[i].port == port && (kind != CHF_CTL_DIRECTION &&
            kind != CHF_CTL_TWIST && kind != CHF_CTL_AXIS ? 1 : c[i].act_mask == acts))
            return i;
    return -1;
}

static void check_layout(int machine, const char *name)
{
    int n = 0, i, j, bad = 0, resets = 0;
    float w = 0, h = 0;
    const chf_control *c = chfsession_controls_layout(machine, &n, &w, &h);
    char msg[160];

    snprintf(msg, sizeof msg, "%s: 35 controls (the panel's 5, and 15 on each controller)", name);
    check(c != NULL && n == 5 + 2 * 15, msg);

    /* the rectangles: round parts are judged by shape below */
    for (i = 0; i < n; i++) {
        if (c[i].x < -EPS || c[i].y < -EPS || c[i].x + c[i].w > w + EPS || c[i].y + c[i].h > h + EPS) {
            printf("  (%s sits outside the extent)\n", c[i].label);
            bad++;
        }
        if (!c[i].label || !c[i].label[0]) bad++;
        for (j = i + 1; j < n; j++)
            if (!is_round(&c[i]) && !is_round(&c[j]) && overlap(&c[i], &c[j])) {
                printf("  (%s overlaps %s)\n", c[i].label, c[j].label);
                bad++;
            }
    }
    snprintf(msg, sizeof msg, "%s: no two controls overlap and all sit inside the extent", name);
    check(bad == 0, msg);

    /* the round parts keep to their own controller: nothing but their own
     * stick's box contains a wedge, and no other control's box meets the disc */
    bad = 0;
    for (i = 0; i < n; i++) {
        if (!is_round(&c[i])) continue;
        for (j = 0; j < n; j++) {
            if (j == i || is_round(&c[j])) continue;
            if (overlap(&c[i], &c[j])) {
                printf("  (%s overlaps %s)\n", c[i].label, c[j].label);
                bad++;
            }
        }
    }
    snprintf(msg, sizeof msg, "%s: the stick and its wedges meet no other control", name);
    check(bad == 0, msg);

    for (i = 0; i < n; i++)
        if (c[i].kind == CHF_CTL_RESET) resets++;
    snprintf(msg, sizeof msg, "%s: one RESET, wired to the switch", name);
    check(resets == 1 && c[find(c, n, CHF_CTL_RESET, -1, 0)].target == CHF_TARGET_SWITCH(CHF_SW_RESET), msg);

    /* the panel: RESET then 1 2 3 4 left to right, in one row above the controllers */
    {
        const chf_control *r = &c[find(c, n, CHF_CTL_RESET, -1, 0)];
        float prev = r->x + r->w, top = 1e9f, bottom = 0;
        int ok = 1, nb = 0;

        for (i = 0; i < n; i++) {
            if (c[i].kind != CHF_CTL_BUTTON) continue;
            nb++;
            if (c[i].target != CHF_TARGET_KEY(nb - 1) || c[i].x < prev - EPS || fabsf(c[i].y - r->y) > EPS
                || c[i].port != -1 || fabsf(c[i].w - 1.0f) > EPS)
                ok = 0;
            prev = c[i].x + c[i].w;
            if (!chfsession_key_label(machine, nb - 1) || strcmp(chfsession_key_label(machine, nb - 1), c[i].label)
                || !chfsession_key_legend(machine, nb - 1) || !c[i].legend
                || strcmp(chfsession_key_legend(machine, nb - 1), c[i].legend))
                ok = 0;
        }
        snprintf(msg, sizeof msg, "%s: four buttons 1-4, a button wide, in order after RESET", name);
        check(nb == CHF_KEY_COUNT && ok, msg);
        for (i = 0; i < n; i++)
            if (c[i].port >= 0 && c[i].y < top) top = c[i].y;
        bottom = r->y + r->h;
        snprintf(msg, sizeof msg, "%s: the panel is across the top, above the controllers", name);
        check(top > bottom + EPS, msg);
    }

    /* the buttons' legends are the case's */
    check(!strcmp(chfsession_key_legend(machine, 0), "TIME") && !strcmp(chfsession_key_legend(machine, 1), "MODE")
          && !strcmp(chfsession_key_legend(machine, 2), "HOLD") && !strcmp(chfsession_key_legend(machine, 3), "START"),
          "1 TIME, 2 MODE, 3 HOLD, 4 START");

    /* each controller: Player 2 (port 1) on the left, Player 1 (port 0) on the right */
    {
        const int cap0 = find(c, n, CHF_CTL_CAPTION, 0, 0), cap1 = find(c, n, CHF_CTL_CAPTION, 1, 0);
        int ok = cap0 >= 0 && cap1 >= 0;

        snprintf(msg, sizeof msg, "%s: the left controller (Player 2) is on the left, the right one (Player 1) on the right", name);
        check(ok && c[cap1].x < c[cap0].x
              && strstr(c[cap0].label, "Right") && strstr(c[cap0].label, "Player 1")
              && strstr(c[cap1].label, "Left") && strstr(c[cap1].label, "Player 2"), msg);
    }
    for (int port = 0; port < 2; port++) {
        int targets[CHF_ACT_PER_PORT] = {0}, diag = 0, ok = 1;
        const int stick = find(c, n, CHF_CTL_STICK, port, 0);
        char what[160];

        snprintf(what, sizeof what, "%s: controller %d has a stick, 8 wedges, 2 twists, PULL, PUSH, a side view and a caption",
                 name, port + 1);
        check(stick >= 0 && count_kind(c, n, CHF_CTL_DIRECTION, port) == 8 && count_kind(c, n, CHF_CTL_TWIST, port) == 2
              && count_kind(c, n, CHF_CTL_AXIS, port) == 2 && count_kind(c, n, CHF_CTL_SIDE, port) == 1
              && count_kind(c, n, CHF_CTL_CAPTION, port) == 1 && count_kind(c, n, CHF_CTL_STICK, port) == 1, what);
        for (i = 0; i < n; i++) {
            if (c[i].port != port) continue;
            if (c[i].kind == CHF_CTL_DIRECTION) {
                const unsigned m = c[i].act_mask;
                if (m & (m - 1)) {
                    diag++;
                    if (c[i].target != -1) ok = 0;
                } else if (c[i].target != CHF_TARGET_PORT(port, lowest(m))) ok = 0;
                if (!(m & (m - 1))) targets[lowest(m)]++;
                /* a wedge sits inside its stick's box */
                if (c[i].x < c[stick].x - EPS || c[i].y < c[stick].y - EPS ||
                    c[i].x + c[i].w > c[stick].x + c[stick].w + EPS || c[i].y + c[i].h > c[stick].y + c[stick].h + EPS)
                    ok = 0;
            } else if (c[i].kind == CHF_CTL_TWIST || c[i].kind == CHF_CTL_AXIS) {
                const unsigned m = c[i].act_mask;
                if ((m & (m - 1)) || m == 0 || c[i].target != CHF_TARGET_PORT(port, lowest(m))) ok = 0;
                else targets[lowest(m)]++;
            }
        }
        for (i = 0; i < CHF_ACT_PER_PORT; i++)
            if (targets[i] != 1) ok = 0;
        snprintf(what, sizeof what, "%s: controller %d: every contact is on exactly one control, the 4 diagonals on two", name, port + 1);
        check(ok && diag == 4, what);

        /* the twist arrows are either side of the disc, counter-clockwise on the left;
         * PULL is above the handle and PUSH below */
        {
            const int ccw = find(c, n, CHF_CTL_TWIST, port, B(CHF_ACT_TWIST_CCW));
            const int cw = find(c, n, CHF_CTL_TWIST, port, B(CHF_ACT_TWIST_CW));
            const int pull = find(c, n, CHF_CTL_AXIS, port, B(CHF_ACT_PULL));
            const int push = find(c, n, CHF_CTL_AXIS, port, B(CHF_ACT_PUSH));
            const int side = find(c, n, CHF_CTL_SIDE, port, 0);

            snprintf(what, sizeof what, "%s: controller %d: twists either side of the disc, PULL over the handle, PUSH under it", name, port + 1);
            check(ccw >= 0 && cw >= 0 && pull >= 0 && push >= 0 && side >= 0
                  && c[ccw].x + c[ccw].w <= c[stick].x + EPS && c[cw].x >= c[stick].x + c[stick].w - EPS
                  && c[pull].y + c[pull].h <= c[side].y + EPS && c[push].y >= c[side].y + c[side].h - EPS
                  && c[side].x >= c[stick].x + c[stick].w, what);
        }
    }

    /* the hit test */
    {
        int ok = 1, k;
        char what[160];

        for (i = 0; i < n; i++) {
            const chf_control *ci = &c[i];
            float cx = ci->x + ci->w / 2, cy = ci->y + ci->h / 2;
            int hit;

            if (ci->kind == CHF_CTL_DIRECTION) {
                const chf_control *st = &c[find(c, n, CHF_CTL_STICK, ci->port, 0)];
                /* the middle of the ring, toward the wedge */
                float sx = st->x + st->w / 2, sy = st->y + st->h / 2, r = st->w / 2 * 0.75f, ax = 0, ay = 0;
                unsigned m = ci->act_mask;
                if (m & B(CHF_ACT_RIGHT)) ax += 1;
                if (m & B(CHF_ACT_LEFT)) ax -= 1;
                if (m & B(CHF_ACT_FORWARD)) ay -= 1;      /* up on the screen */
                if (m & B(CHF_ACT_BACK)) ay += 1;
                cx = sx + r * ax / sqrtf(ax * ax + ay * ay);
                cy = sy + r * ay / sqrtf(ax * ax + ay * ay);
            } else if (ci->kind == CHF_CTL_STICK) {
                /* the knob cap, the middle of the disc */
            }
            hit = chfsession_controls_hit(machine, cx, cy);
            if (hit != i) {
                printf("  (%s: hit %d, want %d)\n", ci->label, hit, i);
                ok = 0;
            }
        }
        snprintf(what, sizeof what, "%s: the middle of every control hits it (wedges at the ring's middle)", name);
        check(ok, what);

        k = chfsession_controls_hit(machine, w / 2, h - 0.01f);
        check(k == -1 || c[k].kind != CHF_CTL_BUTTON, "a point between the controllers hits no button");
        check(chfsession_controls_hit(machine, -1.0f, -1.0f) == -1 && chfsession_controls_hit(machine, w + 1, h + 1) == -1,
              "a point outside the layout hits nothing");
    }
}

int main(void)
{
    int n = 0;
    float w, h;
    const chf_control *c;

    check(CHF_KEY_COUNT == 4 && CHF_KEY_TIME == 0 && CHF_KEY_START == 3, "four console buttons");
    check(chfsession_key_label(CHF_MACHINE_CHANNELF, -1) == NULL
          && chfsession_key_label(CHF_MACHINE_CHANNELF, CHF_KEY_COUNT) == NULL
          && chfsession_key_legend(CHF_MACHINE_CHANNELF, CHF_KEY_COUNT) == NULL, "no label out of range");
    check(!strcmp(chfsession_key_label(CHF_MACHINE_CHANNELF, 0), "1")
          && !strcmp(chfsession_key_label(CHF_MACHINE_CHANNELF, 3), "4"), "the buttons are numbered 1-4");

    for (int m = 0; m < CHF_MACHINE_COUNT; m++)
        check_layout(m, chf_machine_name(m));

    c = chfsession_controls_layout(CHF_MACHINE_CHANNELF, &n, &w, &h);
    check(c && n > 0 && w > h, "the window is wider than it is tall");
    check(chfsession_controls_layout(CHF_MACHINE_CHANNELF2, NULL, NULL, NULL) == c, "every machine prints the same panel");

    /* the stick's gesture maths: nothing in the middle quarter, else 8 sectors of 45 degrees */
    check(chfsession_stick_from_vector(0, 0, 10) == 0 && chfsession_stick_from_vector(2, 0, 10) == 0
          && chfsession_stick_from_vector(1.5f, 1.5f, 10) == 0, "inside a quarter of the reach: nothing");
    check(chfsession_stick_from_vector(5, 0, 10) == B(CHF_ACT_RIGHT)
          && chfsession_stick_from_vector(-5, 0, 10) == B(CHF_ACT_LEFT)
          && chfsession_stick_from_vector(0, -5, 10) == B(CHF_ACT_FORWARD)
          && chfsession_stick_from_vector(0, 5, 10) == B(CHF_ACT_BACK),
          "right, left, forward (up on the screen) and back (toward the player)");
    check(chfsession_stick_from_vector(5, -5, 10) == (B(CHF_ACT_RIGHT) | B(CHF_ACT_FORWARD))
          && chfsession_stick_from_vector(-5, -5, 10) == (B(CHF_ACT_LEFT) | B(CHF_ACT_FORWARD))
          && chfsession_stick_from_vector(5, 5, 10) == (B(CHF_ACT_RIGHT) | B(CHF_ACT_BACK))
          && chfsession_stick_from_vector(-5, 5, 10) == (B(CHF_ACT_LEFT) | B(CHF_ACT_BACK)),
          "the diagonals are two contacts");
    /* 22.5 degrees is the boundary between right and the diagonal */
    check(chfsession_stick_from_vector(9.3f, -3.0f, 10) == B(CHF_ACT_RIGHT)       /* 17.9 degrees */
          && chfsession_stick_from_vector(9.0f, -4.3f, 10) == (B(CHF_ACT_RIGHT) | B(CHF_ACT_FORWARD)), /* 25.5 */
          "the sectors are 45 degrees wide, centred on the directions");
    check(chfsession_stick_from_vector(100, 0, 10) == B(CHF_ACT_RIGHT) && chfsession_stick_from_vector(5, 0, 0) == 0,
          "past the reach still counts; no reach is nothing");

    /* the hit test agrees with the gesture maths: the wedge at an angle is the one
     * chfsession_stick_from_vector names for it */
    for (int port = 0; port < 2; port++) {
        int ok = 1;
        const chf_control *st = NULL;

        for (int i = 0; i < n; i++)
            if (c[i].kind == CHF_CTL_STICK && c[i].port == port) st = &c[i];
        for (int k = 0; k < 8 && st; k++) {
            const float a = (float)k * 3.14159265f / 4.0f, vx = cosf(a), vy = -sinf(a);
            const unsigned want = chfsession_stick_from_vector(vx, vy, 1.0f);
            const float r = st->w / 2 * 0.75f;
            const int hit = chfsession_controls_hit(CHF_MACHINE_CHANNELF, st->x + st->w / 2 + r * vx,
                                                    st->y + st->h / 2 + r * vy);

            if (hit < 0 || c[hit].kind != CHF_CTL_DIRECTION || c[hit].port != port || c[hit].act_mask != want || !want)
                ok = 0;
        }
        check(st && ok, port ? "the left controller's wedges are the stick's eight sectors"
                             : "the right controller's wedges are the stick's eight sectors");
    }

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
