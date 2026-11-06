/*
 * bindings_test -- the remappable table and the keyboard translator, pure:
 * the defaults (both hand controllers' eight contacts, the four console
 * buttons, F1 for RESET), that no two targets share a default key or a
 * default pad button, steal-and-describe, persistence through the settings
 * store, and names. No machine is started.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "chfsession.h"
#include "test_tmpdir.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static int key_target(chfsession *s, uint32_t keysym)
{
    return chfsession_target_for_key(s, keysym);
}

#define P1(a) CHF_TARGET_PORT(0, a)
#define P2(a) CHF_TARGET_PORT(1, a)

int main(void)
{
    char cfg[512], data[512], stolen[96], name[64];
    chfsession_paths p;
    chfsession *s;
    chf_binding b;
    int t, u, clash = 0, bclash = 0, bad;

    test_tmpdir(cfg, sizeof cfg, "bcfg");
    test_tmpdir(data, sizeof data, "bdata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
    s = chfsession_new(&p);
    if (!s) return 1;

    /* the table's shape: the action number is the hardware bit */
    check(CHF_TARGET_COUNT == 16 + 4 + 1 + 2, "sixteen controller contacts, four buttons, RESET, two actions");
    check(CHF_TARGET_KEY(0) == 16 && CHF_TARGET_SWITCH(CHF_SW_RESET) == 20
          && CHF_TARGET_SYSACT(0) == 21, "the flat target indices");
    check(CHF_ACT_RIGHT == 0 && CHF_ACT_LEFT == 1 && CHF_ACT_BACK == 2 && CHF_ACT_FORWARD == 3
          && CHF_ACT_TWIST_CCW == 4 && CHF_ACT_TWIST_CW == 5 && CHF_ACT_PULL == 6
          && CHF_ACT_PUSH == 7, "the actions are MAME's controller bits 0-7");
    check(CHF_KEY_TIME == 0 && CHF_KEY_MODE == 1 && CHF_KEY_HOLD == 2 && CHF_KEY_START == 3,
          "the buttons are bits 0-3 of the panel");

    /* player 1, the right controller */
    check(key_target(s, CHF_KEYSYM_RIGHT) == P1(CHF_ACT_RIGHT)
          && key_target(s, CHF_KEYSYM_LEFT) == P1(CHF_ACT_LEFT)
          && key_target(s, CHF_KEYSYM_DOWN) == P1(CHF_ACT_BACK)
          && key_target(s, CHF_KEYSYM_UP) == P1(CHF_ACT_FORWARD),
          "player 1's stick is the arrow keys (Down is back, Up is forward)");
    check(key_target(s, 'z') == P1(CHF_ACT_TWIST_CCW) && key_target(s, 'x') == P1(CHF_ACT_TWIST_CW),
          "Z and X twist it counter-clockwise and clockwise");
    check(key_target(s, 'c') == P1(CHF_ACT_PULL) && key_target(s, CHF_KEYSYM_SPACE) == P1(CHF_ACT_PUSH),
          "C pulls and Space pushes");
    check(key_target(s, 'Z') == P1(CHF_ACT_TWIST_CCW), "lookup folds case");

    /* player 2, the left controller: the numeric keypad */
    check(key_target(s, CHF_KEYSYM_KP_8) == P2(CHF_ACT_FORWARD)
          && key_target(s, CHF_KEYSYM_KP_2) == P2(CHF_ACT_BACK)
          && key_target(s, CHF_KEYSYM_KP_4) == P2(CHF_ACT_LEFT)
          && key_target(s, CHF_KEYSYM_KP_6) == P2(CHF_ACT_RIGHT),
          "player 2's stick is the keypad's 8 2 4 6 (forward, back, left, right)");
    check(key_target(s, CHF_KEYSYM_KP_7) == P2(CHF_ACT_TWIST_CCW)
          && key_target(s, CHF_KEYSYM_KP_9) == P2(CHF_ACT_TWIST_CW),
          "the keypad's 7 and 9 twist it");
    check(key_target(s, CHF_KEYSYM_KP_PERIOD) == P2(CHF_ACT_PULL)
          && key_target(s, CHF_KEYSYM_KP_0) == P2(CHF_ACT_PUSH),
          "the keypad's . pulls and 0 pushes");
    check(key_target(s, CHF_KEYSYM_KP_5) == -1 && key_target(s, CHF_KEYSYM_KP_1) == -1
          && key_target(s, CHF_KEYSYM_KP_3) == -1, "the keypad's other digits are left alone");

    /* the console */
    check(key_target(s, '1') == CHF_TARGET_KEY(CHF_KEY_TIME) && key_target(s, '2') == CHF_TARGET_KEY(CHF_KEY_MODE)
          && key_target(s, '3') == CHF_TARGET_KEY(CHF_KEY_HOLD) && key_target(s, '4') == CHF_TARGET_KEY(CHF_KEY_START),
          "buttons 1 2 3 4 (TIME MODE HOLD START) are the keys 1 2 3 4");
    b = chfsession_binding_get(s, CHF_TARGET_SWITCH(CHF_SW_RESET));
    check(b.keysym == CHF_KEYSYM_F1 && b.button == CHF_PAD_BTN_NONE, "RESET defaults to F1");
    b = chfsession_binding_get(s, CHF_TARGET_SYSACT(CHF_SYSACT_RESET_CONFIG));
    check(b.keysym == CHF_KEYSYM_ESCAPE, "Reset to CONFIG defaults to Escape");
    b = chfsession_binding_get(s, CHF_TARGET_SYSACT(CHF_SYSACT_DEBUG_STOP));
    check(b.keysym == 0 && b.button == CHF_PAD_BTN_NONE, "Debugger Stop has no default");
    check(key_target(s, CHF_KEYSYM_F5) == -1 && key_target(s, CHF_KEYSYM_F9) == -1
          && key_target(s, CHF_KEYSYM_F11) == -1 && key_target(s, CHF_KEYSYM_F12) == -1,
          "F5, F9, F11 and F12 are the frontends'");

    /* every default is its own: no key drives two targets, no button two
     * targets of one pad */
    for (t = 0; t < CHF_TARGET_COUNT; t++) {
        chf_binding bt = chfsession_binding_get(s, t);
        for (u = t + 1; u < CHF_TARGET_COUNT; u++) {
            chf_binding bu = chfsession_binding_get(s, u);
            if (bt.keysym && bt.keysym == bu.keysym) {
                printf("  (%s and %s share a key)\n", chf_target_name(t), chf_target_name(u));
                clash++;
            }
            /* the same button on both players' contacts is the default; within
             * a port and against the console's targets it must not repeat */
            if (bt.button != CHF_PAD_BTN_NONE && bt.button == bu.button
                && !(t < CHF_ACT_PER_PORT && u >= CHF_ACT_PER_PORT && u < 2 * CHF_ACT_PER_PORT)) {
                printf("  (%s and %s share a button)\n", chf_target_name(t), chf_target_name(u));
                bclash++;
            }
        }
    }
    check(clash == 0, "no two default keys clash");
    check(bclash == 0, "no default gamepad button drives two things");

    /* gamepad defaults, the same on both players */
    for (t = 0; t < 2; t++) {
        char what[96];
        snprintf(what, sizeof what, "pad %d: D-pad is the stick, LB/RB twist, East pulls, South pushes", t + 1);
        check(chfsession_binding_get(s, CHF_TARGET_PORT(t, CHF_ACT_FORWARD)).button == CHF_PAD_BTN_DPAD_UP
              && chfsession_binding_get(s, CHF_TARGET_PORT(t, CHF_ACT_RIGHT)).button == CHF_PAD_BTN_DPAD_RIGHT
              && chfsession_binding_get(s, CHF_TARGET_PORT(t, CHF_ACT_BACK)).button == CHF_PAD_BTN_DPAD_DOWN
              && chfsession_binding_get(s, CHF_TARGET_PORT(t, CHF_ACT_LEFT)).button == CHF_PAD_BTN_DPAD_LEFT
              && chfsession_binding_get(s, CHF_TARGET_PORT(t, CHF_ACT_TWIST_CCW)).button == CHF_PAD_BTN_LEFT_SHOULDER
              && chfsession_binding_get(s, CHF_TARGET_PORT(t, CHF_ACT_TWIST_CW)).button == CHF_PAD_BTN_RIGHT_SHOULDER
              && chfsession_binding_get(s, CHF_TARGET_PORT(t, CHF_ACT_PULL)).button == CHF_PAD_BTN_EAST
              && chfsession_binding_get(s, CHF_TARGET_PORT(t, CHF_ACT_PUSH)).button == CHF_PAD_BTN_SOUTH,
              what);
        check(chfsession_target_for_button(s, t, CHF_PAD_BTN_SOUTH) == CHF_TARGET_PORT(t, CHF_ACT_PUSH)
              && chfsession_target_for_button(s, t, CHF_PAD_BTN_EAST) == CHF_TARGET_PORT(t, CHF_ACT_PULL),
              "button lookup is scoped to the player");
        check(chfsession_target_for_button(s, t, CHF_PAD_BTN_WEST) == CHF_TARGET_KEY(CHF_KEY_TIME)
              && chfsession_target_for_button(s, t, CHF_PAD_BTN_BACK) == CHF_TARGET_KEY(CHF_KEY_MODE)
              && chfsession_target_for_button(s, t, CHF_PAD_BTN_NORTH) == CHF_TARGET_KEY(CHF_KEY_HOLD)
              && chfsession_target_for_button(s, t, CHF_PAD_BTN_START) == CHF_TARGET_KEY(CHF_KEY_START),
              "West, Back, North and Start are TIME, MODE, HOLD and START on either pad");
    }
    check(chfsession_target_for_button(s, 0, CHF_PAD_BTN_GUIDE) == -1, "Guide is nothing");

    /* steal: one key, one target */
    chfsession_binding_set_key(s, CHF_TARGET_KEY(CHF_KEY_MODE), CHF_KEYSYM_LEFT, stolen, sizeof stolen);
    check(strstr(stolen, "Player 1: Left") != NULL, "rebinding Left reports the holder it displaced");
    check(chfsession_binding_get(s, P1(CHF_ACT_LEFT)).keysym == 0, "the old holder lost the key");
    check(key_target(s, CHF_KEYSYM_LEFT) == CHF_TARGET_KEY(CHF_KEY_MODE), "the new holder has it");
    chfsession_binding_set_key(s, CHF_TARGET_KEY(CHF_KEY_MODE), '2', stolen, sizeof stolen);
    check(stolen[0] == '\0' && key_target(s, '2') == CHF_TARGET_KEY(CHF_KEY_MODE),
          "putting 2 back takes nothing from anyone");
    chfsession_binding_set_key(s, P1(CHF_ACT_LEFT), CHF_KEYSYM_LEFT, NULL, 0);

    chfsession_binding_set_button(s, CHF_TARGET_KEY(CHF_KEY_HOLD), CHF_PAD_BTN_WEST, NULL, 0);
    check(chfsession_target_for_button(s, 0, CHF_PAD_BTN_WEST) == CHF_TARGET_KEY(CHF_KEY_HOLD)
          && chfsession_target_for_button(s, 1, CHF_PAD_BTN_WEST) == CHF_TARGET_KEY(CHF_KEY_HOLD),
          "a pad button bound to a console button works on either pad");
    check(chfsession_binding_get(s, CHF_TARGET_KEY(CHF_KEY_TIME)).button == CHF_PAD_BTN_NONE,
          "and took it from TIME");
    chfsession_binding_set_button(s, P2(CHF_ACT_PUSH), CHF_PAD_BTN_WEST, stolen, sizeof stolen);
    check(strstr(stolen, "Console: 3 HOLD") != NULL, "stealing a pad button names the button that had it");
    check(chfsession_binding_get(s, CHF_TARGET_KEY(CHF_KEY_HOLD)).button == CHF_PAD_BTN_NONE, "which lost it");
    check(chfsession_binding_get(s, P1(CHF_ACT_PUSH)).button == CHF_PAD_BTN_SOUTH,
          "but player 1's push kept its own");
    chfsession_binding_set_key(s, CHF_TARGET_KEY(CHF_KEY_HOLD), 0, NULL, 0);
    check(chfsession_binding_get(s, CHF_TARGET_KEY(CHF_KEY_HOLD)).keysym == 0, "a key can be unbound");

    /* names */
    chfsession_keysym_name(CHF_KEYSYM_F12, name, sizeof name);
    check(strcmp(name, "F12") == 0, "F12 names itself");
    chfsession_keysym_name('z', name, sizeof name);
    check(strcmp(name, "Z") == 0, "letters name upper-case");
    chfsession_keysym_name(CHF_KEYSYM_KP_7, name, sizeof name);
    check(strcmp(name, "Keypad 7") == 0, "numeric keypad keys say so");
    chfsession_keysym_name(CHF_KEYSYM_KP_PERIOD, name, sizeof name);
    check(strcmp(name, "Keypad .") == 0, "the keypad's . too");
    chfsession_keysym_name(CHF_KEYSYM_SPACE, name, sizeof name);
    check(strcmp(name, "Space") == 0, "Space");
    check(strcmp(chf_pad_button_name(CHF_PAD_BTN_DPAD_LEFT), "D-pad Left") == 0, "pad button names");
    check(strcmp(chf_target_name(CHF_TARGET_SYSACT(CHF_SYSACT_RESET_CONFIG)), "Reset to CONFIG") == 0,
          "system action names");
    check(strcmp(chf_target_name(P1(CHF_ACT_FORWARD)), "Player 1: Forward") == 0
          && strcmp(chf_target_name(P2(CHF_ACT_TWIST_CCW)), "Player 2: Twist CCW") == 0
          && strcmp(chf_target_name(P2(CHF_ACT_PUSH)), "Player 2: Push") == 0,
          "controller target names");
    check(strcmp(chf_target_name(CHF_TARGET_KEY(CHF_KEY_TIME)), "Console: 1 TIME") == 0
          && strcmp(chf_target_name(CHF_TARGET_KEY(CHF_KEY_START)), "Console: 4 START") == 0,
          "the console's buttons are named as the case prints them");
    check(strcmp(chf_target_short_name(P1(CHF_ACT_BACK)), "Back") == 0
          && strcmp(chf_target_short_name(P2(CHF_ACT_PULL)), "Pull") == 0, "short names");
    check(strcmp(chf_target_short_name(CHF_TARGET_KEY(CHF_KEY_MODE)), "2 MODE") == 0, "short button names");
    check(strcmp(chf_target_short_name(CHF_TARGET_SWITCH(CHF_SW_RESET)), "RESET") == 0, "the console's RESET");
    bad = 0;
    for (t = 0; t < CHF_TARGET_COUNT; t++)
        if (chf_target_name(t)[0] == '\0' || chf_target_short_name(t)[0] == '\0') bad++;
    check(bad == 0, "every target has a name");

    /* native key codes: evdev KEY_1 is 2, Windows scancode 0x02 is '1', macOS 0x12 is '1' */
    check(chfsession_keysym_from_evdev(2) == '1', "evdev KEY_1 -> '1'");
    check(chfsession_keysym_from_win_scancode(0x02, 0) == '1', "Windows scancode 02 -> '1'");
    check(chfsession_keysym_from_macos_keycode(0x12) == '1', "macOS keycode 0x12 -> '1'");
    check(chfsession_keysym_from_evdev(103) == CHF_KEYSYM_UP, "evdev KEY_UP -> Up");
    check(chfsession_keysym_from_win_scancode(0x48, 1) == CHF_KEYSYM_UP, "Windows E0 48 -> Up");
    check(chfsession_keysym_from_macos_keycode(0x7E) == CHF_KEYSYM_UP, "macOS 0x7E -> Up");
    check(chfsession_keysym_from_evdev(44) == 'z', "evdev KEY_Z -> 'z'");
    check(chfsession_keysym_from_evdev(57) == CHF_KEYSYM_SPACE, "evdev KEY_SPACE -> Space");
    check(chfsession_keysym_from_evdev(83) == CHF_KEYSYM_KP_PERIOD, "evdev KEY_KPDOT -> the keypad's .");

    /* persistence */
    chfsession_binding_set_key(s, P2(CHF_ACT_FORWARD), 'z', stolen, sizeof stolen);
    check(strstr(stolen, "Player 1: Twist CCW") != NULL, "rebinding Z names the control it took it from");
    chfsession_free(s);
    s = chfsession_new(&p);
    check(s && key_target(s, 'z') == P2(CHF_ACT_FORWARD), "the rebinding persisted");
    check(s && key_target(s, CHF_KEYSYM_KP_8) == -1, "and what it displaced stayed displaced");
    chfsession_bindings_reset(s);
    check(key_target(s, 'z') == P1(CHF_ACT_TWIST_CCW), "reset restores the defaults");
    check(key_target(s, CHF_KEYSYM_KP_8) == P2(CHF_ACT_FORWARD), "all of them");
    check(strcmp(chfsession_get_str(s, "bindings", "x"), "") == 0, "and persists nothing");
    chfsession_free(s);

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
