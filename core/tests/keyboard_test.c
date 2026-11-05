/*
 * keyboard_test -- the on-screen controllers' layout table, pure: no key caps
 * overlap, every console key (0-26) is on exactly one cap, the RESET cap is
 * there once, the extent covers every cap, the labels agree with
 * chfsession_key_label(), each cap's port matches its key, and the two
 * keypads are the same shape side by side with the panel between them.
 * No machine is started.
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

static const chf_keycap *find_key(const chf_keycap *c, int n, int target)
{
    for (int i = 0; i < n; i++)
        if (c[i].target == target) return &c[i];
    return NULL;
}

static void check_layout(int machine, const char *name)
{
    int n = 0, i, j, bad = 0, resets = 0, missing = 0, dup = 0;
    float w = 0, h = 0;
    const chf_keycap *c = chfsession_keyboard_layout(machine, &n, &w, &h);
    char msg[160];

    snprintf(msg, sizeof msg, "%s: a layout with %d caps (24 keys, 3 buttons, RESET)", name, n);
    check(c != NULL && n == 24 + 3 + 1, msg);

    for (i = 0; i < n; i++) {
        for (j = i + 1; j < n; j++) {
            int ox = c[i].x < c[j].x + c[j].w - EPS && c[j].x < c[i].x + c[i].w - EPS;
            int oy = c[i].y < c[j].y + c[j].h - EPS && c[j].y < c[i].y + c[i].h - EPS;
            if (ox && oy) {
                printf("  (%s overlaps %s)\n", c[i].label, c[j].label);
                bad++;
            }
        }
        if (c[i].x < -EPS || c[i].y < -EPS || c[i].x + c[i].w > w + EPS || c[i].y + c[i].h > h + EPS)
            bad++;
        if (c[i].target == -1) resets++;
    }
    snprintf(msg, sizeof msg, "%s: no caps overlap and all sit inside the extent", name);
    check(bad == 0, msg);
    snprintf(msg, sizeof msg, "%s: one RESET cap", name);
    check(resets == 1, msg);

    for (i = 0; i < CHF_KEY_COUNT; i++) {
        int count = 0;
        for (j = 0; j < n; j++)
            if (c[j].target == i) count++;
        if (count == 0) missing++;
        if (count > 1) dup++;
    }
    snprintf(msg, sizeof msg, "%s: every console key is on exactly one cap", name);
    check(missing == 0 && dup == 0, msg);

    bad = 0;
    for (i = 0; i < n; i++) {
        const char *label = c[i].target >= 0 ? chfsession_key_label(machine, c[i].target) : "RESET";
        int want_port = c[i].target < 0 ? -1 : c[i].target < 12 ? 0 : c[i].target < 24 ? 1 : -1;
        if (!label || strcmp(label, c[i].label)) bad++;
        if (c[i].port != want_port) bad++;
        if (!c[i].label || !c[i].label[0]) bad++;
    }
    snprintf(msg, sizeof msg, "%s: the labels and ports agree with the keys", name);
    check(bad == 0, msg);
}

int main(void)
{
    int n = 0;
    float w, h;
    const chf_keycap *c;

    check(CHF_KEY_COUNT == 27 && CHF_KEY_START == 24 && CHF_KEY_SELECT == 26, "27 console keys");
    check(chfsession_key_label(CHF_MACHINE_ARCADIA, -1) == NULL
          && chfsession_key_label(CHF_MACHINE_ARCADIA, CHF_KEY_COUNT) == NULL, "no label out of range");
    check(!strcmp(chfsession_key_label(CHF_MACHINE_ARCADIA, 9), "CLEAR")
          && !strcmp(chfsession_key_label(CHF_MACHINE_ARCADIA, 10), "0")
          && !strcmp(chfsession_key_label(CHF_MACHINE_ARCADIA, 11), "ENTER")
          && !strcmp(chfsession_key_label(CHF_MACHINE_ARCADIA, 12), "1")
          && !strcmp(chfsession_key_label(CHF_MACHINE_ARCADIA, CHF_KEY_OPTION), "OPTION"),
          "the key order is 1-9, CLEAR, 0, ENTER, twice, then START OPTION SELECT");

    check_layout(CHF_MACHINE_ARCADIA, "NTSC");
    check_layout(CHF_MACHINE_ARCADIA_PAL, "PAL");

    c = chfsession_keyboard_layout(CHF_MACHINE_ARCADIA, &n, &w, &h);
    {
        const chf_keycap *k1 = find_key(c, n, 0), *k5 = find_key(c, n, 4), *k0 = find_key(c, n, 10);
        const chf_keycap *p1 = find_key(c, n, 12), *st = find_key(c, n, CHF_KEY_START);
        const chf_keycap *k9 = find_key(c, n, 8), *clear = find_key(c, n, 9);

        check(k1 && k5 && k0 && p1 && st && k9 && clear, "the caps are found");
        if (k1 && k5 && k0 && p1 && st && k9 && clear) {
            check(fabsf(k1->y - 0) < EPS && k5->y > k1->y && k9->y > k5->y && clear->y > k9->y,
                  "a keypad runs 1 2 3 / 4 5 6 / 7 8 9 / CLEAR 0 ENTER top to bottom");
            check(fabsf(k0->x - k5->x) < EPS && k0->y > k5->y, "0 sits under 5");
            check(fabsf(clear->x - k1->x) < EPS, "CLEAR sits under 1");
            check(st->x > k1->x + k1->w && p1->x > st->x + st->w,
                  "player 1's keypad, then the console's buttons, then player 2's");
            check(fabsf((p1->y) - (k1->y)) < EPS, "the two keypads start on the same row");
        }
    }
    check(CHFSESSION_ACCENT_RGB == 0xF2C300 && CHFSESSION_ACCENT_DARK_RGB == 0x8A6D00, "the console's gold");

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
