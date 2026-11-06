/*
 * gamepad_test -- the gamepad layer's pure helpers (no hardware): the sticks'
 * hysteresis (0.5 in, 0.35 out), the left stick and D-pad as the controller's
 * stick, the right stick as the knob's twist, pull and push, port assignment
 * and the SDL button tables; and that the SDL gamepad subsystem starts and
 * stops cleanly with nothing attached.
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

    /* stick hysteresis: in at half deflection, out below 0.35 */
    check(CHF_STICK_ENTER == 0.5f && CHF_STICK_EXIT == 0.35f, "the thresholds are 0.5 and 0.35");
#define IN  CHF_STICK_ENTER, CHF_STICK_EXIT
    check(chf_dir_from_stick(0.f, 0.f, 0, IN) == 0, "centered stick is no direction");
    check(chf_dir_from_stick(0.6f, 0.f, 0, IN) == CHF_DIR_RIGHT, "past half right engages right");
    check(chf_dir_from_stick(0.4f, 0.f, 0, IN) == 0, "two fifths right does not engage");
    check(chf_dir_from_stick(0.4f, 0.f, CHF_DIR_RIGHT, IN) == CHF_DIR_RIGHT, "but holds once engaged");
    check(chf_dir_from_stick(0.3f, 0.f, CHF_DIR_RIGHT, IN) == 0, "and releases below exit");
    check(chf_dir_from_stick(-0.6f, -0.6f, 0, IN) == (CHF_DIR_LEFT | CHF_DIR_UP), "diagonals are two bits");
    check(chf_dir_from_stick(0.6f, 0.05f, 0, IN) == CHF_DIR_RIGHT,
          "a pure horizontal picks up no vertical noise");
    check(chf_dir_from_stick(0.f, 0.9f, CHF_DIR_RIGHT, IN) == CHF_DIR_DOWN,
          "an axis is judged on its own: a stick that left right is down");

    /* directions as the controller's stick contacts */
#define ACT(a) (1u << (a))
    check(chf_acts_from_dir(CHF_DIR_UP) == ACT(CHF_ACT_FORWARD) &&
          chf_acts_from_dir(CHF_DIR_DOWN) == ACT(CHF_ACT_BACK) &&
          chf_acts_from_dir(CHF_DIR_LEFT) == ACT(CHF_ACT_LEFT) &&
          chf_acts_from_dir(CHF_DIR_RIGHT) == ACT(CHF_ACT_RIGHT),
          "up is forward, down is back, left and right are the stick's four contacts");
    check(chf_acts_from_dir(CHF_DIR_UP | CHF_DIR_LEFT) == (ACT(CHF_ACT_FORWARD) | ACT(CHF_ACT_LEFT)) &&
          chf_acts_from_dir(CHF_DIR_DOWN | CHF_DIR_RIGHT) == (ACT(CHF_ACT_BACK) | ACT(CHF_ACT_RIGHT)),
          "a diagonal is two contacts");
    check(chf_acts_from_dir(CHF_DIR_UP | CHF_DIR_DOWN) == 0 && chf_acts_from_dir(0) == 0 &&
          chf_acts_from_dir(CHF_DIR_LEFT | CHF_DIR_RIGHT) == 0,
          "opposites cancel");
    check(chf_acts_from_dir(CHF_DIR_UP | CHF_DIR_DOWN | CHF_DIR_LEFT) == ACT(CHF_ACT_LEFT),
          "a cancelled axis leaves the other one alone");

    /* the right stick: the knob's twist, pull and push */
    check(chf_knob_acts_from_dir(CHF_DIR_LEFT) == ACT(CHF_ACT_TWIST_CCW) &&
          chf_knob_acts_from_dir(CHF_DIR_RIGHT) == ACT(CHF_ACT_TWIST_CW) &&
          chf_knob_acts_from_dir(CHF_DIR_UP) == ACT(CHF_ACT_PULL) &&
          chf_knob_acts_from_dir(CHF_DIR_DOWN) == ACT(CHF_ACT_PUSH),
          "left and right twist, up pulls, down pushes");
    check(chf_knob_acts_from_dir(CHF_DIR_RIGHT | CHF_DIR_UP) == (ACT(CHF_ACT_TWIST_CW) | ACT(CHF_ACT_PULL)),
          "twisting and pulling together are two contacts");
    check(chf_knob_acts_from_dir(CHF_DIR_LEFT | CHF_DIR_RIGHT) == 0 && chf_knob_acts_from_dir(CHF_DIR_UP | CHF_DIR_DOWN) == 0
          && chf_knob_acts_from_dir(0) == 0, "and opposites cancel");

    /* port assignment */
    assign[0] = -1; assign[1] = -1; assign[2] = -1;
    check(chf_pad_for_port(assign, 2, 0) == 0 && chf_pad_for_port(assign, 2, 1) == 1,
          "connection order: first pad player 1, second pad player 2");
    check(chf_pad_for_port(assign, 1, 1) == -1, "one pad leaves player 2 to the keyboard");
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
