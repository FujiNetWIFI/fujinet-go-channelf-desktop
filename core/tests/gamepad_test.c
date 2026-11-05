/*
 * gamepad_test -- the gamepad layer's pure helpers (no hardware), and that
 * the SDL gamepad subsystem starts and stops cleanly with nothing attached.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "chfsession.h"
#include "gamepad_sdl.h"
#include "test_tmpdir.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

int main(void)
{
    int assign[4];

    /* stick hysteresis */
    check(chf_dir_from_stick(0.f, 0.f, 0, 0.35f, 0.15f) == 0, "centered stick is no direction");
    check(chf_dir_from_stick(0.5f, 0.f, 0, 0.35f, 0.15f) == CHF_DIR_RIGHT, "half right engages right");
    check(chf_dir_from_stick(0.2f, 0.f, 0, 0.35f, 0.15f) == 0, "a fifth right does not engage");
    check(chf_dir_from_stick(0.2f, 0.f, CHF_DIR_RIGHT, 0.35f, 0.15f) == CHF_DIR_RIGHT, "but holds once engaged");
    check(chf_dir_from_stick(0.1f, 0.f, CHF_DIR_RIGHT, 0.35f, 0.15f) == 0, "and releases below exit");
    check(chf_dir_from_stick(-0.6f, -0.6f, 0, 0.35f, 0.15f) == (CHF_DIR_LEFT | CHF_DIR_UP), "diagonals are two bits");
    check(chf_dir_from_stick(0.6f, 0.05f, 0, 0.35f, 0.15f) == CHF_DIR_RIGHT,
          "a pure horizontal picks up no vertical noise");

    /* the analog disc */
    {
        int dx = 99, dy = 99;
        chf_disc_from_stick(0.f, 0.f, &dx, &dy);
        check(dx == 0 && dy == 0, "a centred stick is a centred disc");
        chf_disc_from_stick(0.08f, -0.08f, &dx, &dy);
        check(dx == 0 && dy == 0, "a tremor inside the dead zone is nothing");
        chf_disc_from_stick(1.f, 0.f, &dx, &dy);
        check(dx == 127 && dy == 0, "full right is +127");
        chf_disc_from_stick(-1.f, 0.f, &dx, &dy);
        check(dx == -127 && dy == 0, "full left is -127");
        chf_disc_from_stick(0.f, 1.f, &dx, &dy);
        check(dx == 0 && dy == 127, "full down is +127 (SDL's y)");
        chf_disc_from_stick(1.f, 1.f, &dx, &dy);
        check(dx > 80 && dx < 127 && dx == dy, "a corner is clamped to the circle");
        chf_disc_from_stick(0.5f, 0.f, &dx, &dy);
        check(dx > 40 && dx < 90 && dy == 0, "half deflection is part way (past the dead zone)");
        chf_disc_from_stick(0.13f, 0.f, &dx, &dy);
        check(dx >= 0 && dx < 5, "just past the dead zone starts from zero, not a jump");
    }

    /* directions as the stick's switches */
#define ACT(a) (1u << (a))
    check(chf_acts_from_dir(CHF_DIR_UP) == ACT(CHF_ACT_UP) &&
          chf_acts_from_dir(CHF_DIR_DOWN) == ACT(CHF_ACT_DOWN) &&
          chf_acts_from_dir(CHF_DIR_LEFT) == ACT(CHF_ACT_LEFT) &&
          chf_acts_from_dir(CHF_DIR_RIGHT) == ACT(CHF_ACT_RIGHT),
          "up, down, left, right are the stick's four switches");
    check(chf_acts_from_dir(CHF_DIR_UP | CHF_DIR_LEFT) == (ACT(CHF_ACT_UP) | ACT(CHF_ACT_LEFT)) &&
          chf_acts_from_dir(CHF_DIR_DOWN | CHF_DIR_RIGHT) == (ACT(CHF_ACT_DOWN) | ACT(CHF_ACT_RIGHT)),
          "a diagonal is two switches");
    check(chf_acts_from_dir(CHF_DIR_UP | CHF_DIR_DOWN) == 0 && chf_acts_from_dir(0) == 0 &&
          chf_acts_from_dir(CHF_DIR_LEFT | CHF_DIR_RIGHT) == 0,
          "opposites cancel");
    check(chf_acts_from_dir(CHF_DIR_UP | CHF_DIR_DOWN | CHF_DIR_LEFT) == ACT(CHF_ACT_LEFT),
          "a cancelled axis leaves the other one alone");

    /* port assignment */
    assign[0] = -1; assign[1] = -1; assign[2] = -1;
    check(chf_pad_for_port(assign, 2, 0) == 0 && chf_pad_for_port(assign, 2, 1) == 1,
          "connection order: first pad disc 1, second pad disc 2");
    check(chf_pad_for_port(assign, 1, 1) == -1, "one pad leaves disc 2 to the keyboard");
    assign[0] = 1; assign[1] = -1;
    check(chf_pad_for_port(assign, 2, 1) == 0 && chf_pad_for_port(assign, 2, 0) == 1,
          "an explicit assignment wins and the other pad fills the gap");
    assign[0] = 1; assign[1] = 1;
    check(chf_pad_for_port(assign, 2, 1) == 0 && chf_pad_for_port(assign, 2, 0) == -1,
          "two pads on one port: the first wins, the other port stays empty");

    /* SDL button tables round-trip */
    {
        int b, ok = 1;
        for (b = 0; b < CHF_PAD_BTN_LEFT_TRIGGER; b++) {
            int sdl = chf_sdl_button_from_pad(b);
            if (sdl < 0 || chf_pad_button_from_sdl((uint8_t)sdl) != b) ok = 0;
        }
        check(ok, "every named button round-trips through the SDL tables");
        check(chf_pad_button_from_sdl(200) == CHF_PAD_BTN_NONE, "an unknown SDL button is NONE");
    }

    /* the subsystem with no hardware: a session with gamepads enabled
     * starts, reports zero pads, and stops */
    {
        char cfg[512], data[512];
        chfsession_paths p;
        chfsession *s;
        chfsession_start_opts o;
        test_tmpdir(cfg, sizeof cfg, "gcfg");
        test_tmpdir(data, sizeof data, "gdata");
        memset(&p, 0, sizeof p);
        p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
        s = chfsession_new(&p);
        chfsession_default_opts(s, &o);
        o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 1;
        check(chfsession_start(s, &o) == 0, "session starts with the gamepad thread");
        check(chfsession_gamepad_count(s) >= 0, "gamepad count answers");
        check(chfsession_gamepad_effective_port(s, 99) == -1, "an absent pad has no port");
        chfsession_gamepad_capture_begin(s);
        check(chfsession_gamepad_capture_poll(s, NULL) == 0, "nothing captured with no pad");
        chfsession_gamepad_capture_cancel(s);
        chfsession_stop(s);
        chfsession_free(s);
        check(1, "session with gamepads stops cleanly");
    }

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
