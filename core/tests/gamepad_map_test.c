/*
 * gamepad_map_test -- the gamepad layer end to end with an SDL3 virtual
 * gamepad: hot-plug pickup, the default mapping onto the hand controller (the
 * D-pad and the left stick tilt the stick, the shoulders twist, East pulls and
 * South pushes, the right stick works the knob's other axes, and not with
 * "right_stick" off), a button rebound to a console button (TIME) and to the
 * console's RESET, the sources not stepping on each other, and hot-unplug
 * releasing everything the pad held -- with no hardware and no human.
 *
 * What the pad holds is read back through chfsession_buttons_held,
 * chfsession_key_held and chfsession_switch_held, which report the same
 * per-source bits the machine is driven from. SKIPs (77) where SDL has no
 * virtual joysticks or a machine's real pads leave no port free.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "chfsession.h"
#include "test_tmpdir.h"

#define PAD_NAME "chftest virtual pad"

static int failures;
static int vport;   /* the controller (player - 1) the virtual pad was given */

static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* The pad thread syncs devices a few times a second and polls state
 * faster; give it two seconds. */
static void expect(chfsession *s, unsigned want, int key, int reset, const char *what)
{
    unsigned held = 0;
    int k = 0, r = 0;
    for (int i = 0; i < 100; i++) {
        held = chfsession_buttons_held(s, vport);
        k = chfsession_key_held(s, CHF_KEY_TIME);
        r = chfsession_switch_held(s, CHF_SW_RESET);
        if (held == want && k == key && r == reset) {
            printf("ok: %s\n", what);
            return;
        }
        sleep_ms(20);
    }
    printf("FAIL: %s (held %02X want %02X, button TIME %d/%d, RESET %d/%d)\n", what, held, want, k, key, r, reset);
    failures++;
}

#define B(a) (1u << (a))

int main(void)
{
    char cfg[512], data[512];
    chfsession_paths p;
    chfsession *s;
    chfsession_start_opts o;
    SDL_VirtualJoystickDesc desc;
    SDL_JoystickID vid;
    SDL_Joystick *vj;
    int i, baseline;

    test_tmpdir(cfg, sizeof cfg, "gmcfg");
    test_tmpdir(data, sizeof data, "gmdata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg;
    p.data_dir = data;
    p.fujinet_lib = "";      /* the pad only */
    s = chfsession_new(&p);
    if (!s)
        return 1;
    chfsession_default_opts(s, &o);
    o.enable_fujinet = 0;
    o.enable_audio = 0;
    o.enable_gamepad = 1;
    if (chfsession_start(s, &o) != 0) {
        printf("FAIL: start: %s\n", chfsession_last_error(s));
        return 1;
    }

    /* Real pads may already be attached: let the first device scan find
     * them, so the virtual one is known by the slot it adds. */
    sleep_ms(700);
    baseline = chfsession_gamepad_count(s);

    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = PAD_NAME;
    vid = SDL_AttachVirtualJoystick(&desc);
    if (!vid) {
        printf("SKIP: no virtual joysticks here: %s\n", SDL_GetError());
        chfsession_free(s);
        return 77;
    }
    vj = SDL_OpenJoystick(vid);
    if (!vj)
        return 1;

    for (i = 0; i < 100 && chfsession_gamepad_count(s) <= baseline; i++)
        sleep_ms(20);
    vport = -1;
    for (i = 0; i < chfsession_gamepad_count(s); i++) {
        char name[64];
        chfsession_gamepad_name(s, i, name, sizeof name);
        if (strcmp(name, PAD_NAME) == 0)
            vport = chfsession_gamepad_effective_port(s, i);
    }
    if (vport < 0 || vport > 1) {
        printf("%s: the virtual pad got no port (%d; real pads attached?)\n",
               vport < 0 && chfsession_gamepad_count(s) <= baseline ? "FAIL" : "SKIP", vport);
        SDL_CloseJoystick(vj);
        SDL_DetachVirtualJoystick(vid);
        chfsession_free(s);
        return chfsession_gamepad_count(s) <= baseline ? 1 : 77;
    }
    printf("ok: the virtual pad was picked up as player %d\n", vport + 1);

    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, true);
    expect(s, B(CHF_ACT_PUSH), 0, 0, "South pushes the knob");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, false);
    expect(s, 0, 0, 0, "released");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_EAST, true);
    expect(s, B(CHF_ACT_PULL), 0, 0, "East pulls it");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, true);
    expect(s, 0, 0, 0, "pull and push together cancel");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_EAST, false);
    expect(s, B(CHF_ACT_PUSH), 0, 0, "and push is back once pull lets go");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, false);

    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
    expect(s, B(CHF_ACT_TWIST_CCW), 0, 0, "the left shoulder twists counter-clockwise");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, false);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true);
    expect(s, B(CHF_ACT_TWIST_CW), 0, 0, "the right shoulder twists clockwise");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, false);
    expect(s, 0, 0, 0, "released");

    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
    expect(s, B(CHF_ACT_FORWARD), 0, 0, "D-pad up is forward");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_LEFT, true);
    expect(s, B(CHF_ACT_FORWARD) | B(CHF_ACT_LEFT), 0, 0, "up and left together are a diagonal");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
    expect(s, B(CHF_ACT_LEFT), 0, 0, "up and down together cancel");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, false);
    expect(s, B(CHF_ACT_LEFT) | B(CHF_ACT_BACK), 0, 0, "and down is back once up lets go");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_DOWN, false);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_LEFT, false);
    expect(s, 0, 0, 0, "released");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true);
    expect(s, B(CHF_ACT_RIGHT), 0, 0, "D-pad right is right");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, false);
    expect(s, 0, 0, 0, "released");

    /* the left stick is an eight-way switch: half deflection engages */
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTX, 20000);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTY, -32768);
    expect(s, B(CHF_ACT_RIGHT) | B(CHF_ACT_FORWARD), 0, 0, "the stick right and up is the stick right and forward");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_LEFT, true);
    expect(s, B(CHF_ACT_FORWARD), 0, 0, "the D-pad is its own source: left cancels the stick's right");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_LEFT, false);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTX, 0);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTY, 0);
    expect(s, 0, 0, 0, "the stick centred: nothing held");
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTX, 12000);   /* 0.37: under half */
    expect(s, 0, 0, 0, "a third of the way is not enough");
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTX, 17000);   /* 0.52 */
    expect(s, B(CHF_ACT_RIGHT), 0, 0, "past half it is");
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTX, 13000);   /* 0.40: between exit and enter */
    expect(s, B(CHF_ACT_RIGHT), 0, 0, "and it holds until it falls below 0.35");
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTX, 9000);    /* 0.27 */
    expect(s, 0, 0, 0, "then lets go");
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTX, 0);

    /* the right stick is the knob's other axes: x twists, y pulls (up) and pushes (down) */
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHTX, 20000);
    expect(s, B(CHF_ACT_TWIST_CW), 0, 0, "the right stick right twists clockwise");
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHTX, -20000);
    expect(s, B(CHF_ACT_TWIST_CCW), 0, 0, "left twists counter-clockwise");
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHTX, 0);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHTY, -32768);
    expect(s, B(CHF_ACT_PULL), 0, 0, "up pulls");
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHTY, 32767);
    expect(s, B(CHF_ACT_PUSH), 0, 0, "down pushes");
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHTX, 20000);
    expect(s, B(CHF_ACT_PUSH) | B(CHF_ACT_TWIST_CW), 0, 0, "twisting and pushing at once are two contacts");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHTX, 0);
    expect(s, B(CHF_ACT_PUSH) | B(CHF_ACT_TWIST_CW), 0, 0,
           "the shoulder holds the twist when the stick lets go of it: the sources are separate");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, false);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHTY, 0);
    expect(s, 0, 0, 0, "centred");

    /* with the right stick off it does nothing, and takes effect at once */
    chfsession_set_right_stick(s, 0);
    check(chfsession_get_int(s, "right_stick", 1) == 0, "the setting is kept");
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHTX, 30000);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHTY, -30000);
    sleep_ms(300);
    expect(s, 0, 0, 0, "right_stick off: the right stick holds nothing");
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTX, -30000);
    expect(s, B(CHF_ACT_LEFT), 0, 0, "while the left stick still tilts the stick");
    chfsession_set_right_stick(s, 1);
    expect(s, B(CHF_ACT_LEFT) | B(CHF_ACT_TWIST_CW) | B(CHF_ACT_PULL), 0, 0, "switched back on, the right stick (still held) works the knob");
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTX, 0);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHTX, 0);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHTY, 0);
    expect(s, 0, 0, 0, "all centred");

    /* the console's buttons on the pad, and a button rebound to TIME and to RESET */
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_NORTH, true);
    check(chfsession_binding_get(s, CHF_TARGET_KEY(CHF_KEY_HOLD)).button == CHF_PAD_BTN_NORTH, "North is HOLD by default");
    for (i = 0; i < 100 && !chfsession_key_held(s, CHF_KEY_HOLD); i++) sleep_ms(20);
    check(chfsession_key_held(s, CHF_KEY_HOLD) && !chfsession_key_held(s, CHF_KEY_TIME), "and North holds HOLD, not TIME");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_NORTH, false);
    for (i = 0; i < 100 && chfsession_key_held(s, CHF_KEY_HOLD); i++) sleep_ms(20);
    check(!chfsession_key_held(s, CHF_KEY_HOLD), "released");

    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_WEST, true);
    expect(s, 0, 1, 0, "West holds TIME");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_WEST, false);
    expect(s, 0, 0, 0, "released");
    chfsession_binding_set_button(s, CHF_TARGET_KEY(CHF_KEY_TIME), CHF_PAD_BTN_EAST, NULL, 0);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_EAST, true);
    expect(s, 0, 1, 0, "East, rebound to TIME, holds it and no longer pulls");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_EAST, false);
    expect(s, 0, 0, 0, "released");
    chfsession_binding_set_button(s, CHF_TARGET_SWITCH(CHF_SW_RESET), CHF_PAD_BTN_START, NULL, 0);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_START, true);
    expect(s, 0, 0, 1, "Start, bound to RESET, holds it");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_START, false);
    expect(s, 0, 0, 0, "released");

    /* a D-pad direction rebound elsewhere stops steering */
    chfsession_binding_set_button(s, CHF_TARGET_KEY(CHF_KEY_TIME), CHF_PAD_BTN_DPAD_UP, NULL, 0);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
    expect(s, 0, 1, 0, "D-pad up, bound to TIME, is TIME and not forward any more");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, false);
    expect(s, 0, 0, 0, "released");
    chfsession_bindings_reset(s);

    /* A trigger past half travel once risked the gamepad thread taking its
     * own lock twice; a hang here is a ctest timeout. */
    chfsession_binding_set_button(s, CHF_TARGET_KEY(CHF_KEY_TIME), CHF_PAD_BTN_LEFT_TRIGGER, NULL, 0);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    expect(s, 0, 1, 0, "left trigger presses its binding");
    (void)chfsession_gamepad_generation(s);
    (void)chfsession_gamepad_effective_port(s, 0);
    check(1, "session lock still free with a trigger held");
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0);
    expect(s, 0, 0, 0, "left trigger released");
    chfsession_gamepad_capture_begin(s);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    {
        int btn = -1, got = 0, n;
        for (n = 0; n < 100 && !got; n++) {
            got = chfsession_gamepad_capture_poll(s, &btn);
            if (!got) sleep_ms(20);
        }
        check(got && btn == CHF_PAD_BTN_RIGHT_TRIGGER, "map mode captures a trigger");
    }
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0);
    chfsession_bindings_reset(s);

    /* hot-unplug while held: the stick, the knob, a button and RESET are
     * released, not left holding forever */
    chfsession_binding_set_button(s, CHF_TARGET_KEY(CHF_KEY_TIME), CHF_PAD_BTN_EAST, NULL, 0);
    chfsession_binding_set_button(s, CHF_TARGET_SWITCH(CHF_SW_RESET), CHF_PAD_BTN_START, NULL, 0);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, true);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_EAST, true);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_START, true);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTY, -30000);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHTX, -30000);
    expect(s, B(CHF_ACT_RIGHT) | B(CHF_ACT_PUSH) | B(CHF_ACT_FORWARD) | B(CHF_ACT_TWIST_CCW), 1, 1,
           "held before the unplug");
    SDL_CloseJoystick(vj);
    SDL_DetachVirtualJoystick(vid);
    for (i = 0; i < 100 && chfsession_gamepad_count(s) != baseline; i++)
        sleep_ms(20);
    if (chfsession_gamepad_count(s) == baseline)
        printf("ok: the unplugged pad is gone\n");
    else {
        printf("FAIL: the backend kept the detached pad (%d, baseline %d)\n",
               chfsession_gamepad_count(s), baseline);
        failures++;
    }
    expect(s, 0, 0, 0, "and its contacts, button and RESET are released");
    chfsession_bindings_reset(s);

    chfsession_stop(s);
    chfsession_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
