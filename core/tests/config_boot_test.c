/*
 * config_boot_test -- CONFIG, driven like a user from keypad A, boots an
 * image through the in-process FujiNet. The test suite's own counter app (an
 * 8K image claiming the mailbox) and counter game (a plain 8K image) go to the
 * SD host (Import Cartridge to SD); then, on CONFIG's own screens: ENTER opens
 * host slot 1 (SD) -> keypad 8 moves the bar onto the file -> ENTER boots it ->
 * FujiNet mounts it as ROM media and pushes it, the cart stages it, CONFIG arms
 * the swap and hands over through the stub at $18E0, and the app runs (its
 * counter in the UVI's RAM moves). Reset to CONFIG brings CONFIG back; then the
 * game booted the same way runs with the mailbox closed.
 *
 * CONFIG's screens are recognised by their text, read from the UVI's character
 * RAM (test_arc.h); the selection bar is the row drawn with the high bit of
 * its letters clear. Needs nothing but the FujiNet runtime; SKIPs (77)
 * without it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chfsession.h"
#include "test_arc.h"
#include "test_tmpdir.h"

#define APP_NAME  "ACOUNTER.BIN"
#define GAME_NAME "BGAME.BIN"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static int wait_text(const char *text, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 100) {
        if (test_find_row(text) >= 0)
            return 1;
        test_sleep_ms(100);
    }
    printf("  (no \"%s\" on screen)\n", text);
    test_dump_screen();
    return 0;
}

/* One keypad A press, held long enough for CONFIG's scan to see it. */
static void press(chfsession *s, int key)
{
    chfsession_press(s, CHF_TARGET_KEY(key), 1);
    test_sleep_ms(150);
    chfsession_press(s, CHF_TARGET_KEY(key), 0);
    test_sleep_ms(250);
}

enum { K8 = 7, KENT = 11 };

/* From the hosts screen: open slot 1 (SD), the bar onto `name`, boot it.
 * Returns 1 once the cart has swapped. */
static int boot_from_sd(chfsession *s, const char *name)
{
    char shown[17];
    int target, booted = 0;

    if (!wait_text("FUJINET HOSTS", 20000))
        return 0;
    press(s, KENT);
    /* the file list: names are cut to the screen's 16 columns */
    snprintf(shown, sizeof shown, "%.8s", name);
    if (!wait_text(shown, 15000))
        return 0;
    target = test_find_row(shown);
    for (int tries = 0; tries < 12 && test_bar_row() != target; tries++)
        press(s, K8);
    if (target <= 0 || test_bar_row() != target) {
        printf("  (the bar never reached %s)\n", name);
        test_dump_screen();
        return 0;
    }
    press(s, KENT);
    for (int waited = 0; waited < 30000 && !booted; waited += 100) {
        booted = chfsession_cart_booted_game(s);
        if (!booted)
            test_sleep_ms(100);
    }
    if (!booted)
        test_dump_screen();
    return booted;
}

int main(void)
{
    static uint8_t img[8192];
    char cfg[512], data[512], src[700], dest[1200], status[128];
    chfsession_paths p;
    chfsession *s;
    chfsession_start_opts o;

    test_tmpdir(cfg, sizeof cfg, "cbcfg");
    test_tmpdir(data, sizeof data, "cbdata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data;
    s = chfsession_new(&p);
    if (!s) return 1;

    chfsession_default_opts(s, &o);
    o.enable_audio = 0; o.enable_gamepad = 0; o.enable_fujinet = 1;
    if (chfsession_start(s, &o) != 0) {
        printf("start failed: %s\n", chfsession_last_error(s));
        return 1;
    }
    if (!chfsession_fujinet_running(s)) {
        printf("SKIP: no FujiNet runtime available (%s)\n", chfsession_last_error(s));
        chfsession_free(s);
        return 77;
    }

    snprintf(src, sizeof src, "%s/%s", cfg, APP_NAME);
    test_counter_image(img, 1);
    test_write_file(src, img, sizeof img);
    check(chfsession_import_cart_to_sd(s, src, dest, sizeof dest) == 0, "the app goes to the SD host");
    snprintf(src, sizeof src, "%s/%s", cfg, GAME_NAME);
    test_counter_image(img, 0);
    test_write_file(src, img, sizeof img);
    check(chfsession_import_cart_to_sd(s, src, dest, sizeof dest) == 0, "and a game");

    check(wait_text("FUJINET HOSTS", 30000), "CONFIG reaches its hosts screen over the live link");
    check(wait_text("SD", 15000), "and lists the SD host");

    check(boot_from_sd(s, APP_NAME),
          "ENTER opens SD, 8 onto the file, ENTER boots it: the cart swaps it in");
    {
        uint8_t a = test_uvi_ram(TEST_COUNTER);

        test_sleep_ms(300);
        check(test_uvi_ram(TEST_COUNTER) != a, "the app runs (its counter moves)");
    }
    chfsession_cart_status(s, status, sizeof status);
    check(strstr(status, "mailbox closed") == NULL, "the app kept the mailbox");

    check(chfsession_reset_to_config(s) == 0, "Reset to CONFIG");
    check(wait_text("FUJINET HOSTS", 30000), "CONFIG is back on its hosts screen");
    check(chfsession_cart_booted_game(s) == 0, "the session sees CONFIG again");

    check(boot_from_sd(s, GAME_NAME), "the game boots the same way");
    {
        uint8_t a = test_uvi_ram(TEST_COUNTER);

        test_sleep_ms(300);
        check(test_uvi_ram(TEST_COUNTER) != a, "and runs");
    }
    chfsession_cart_status(s, status, sizeof status);
    check(strstr(status, "mailbox closed") != NULL, "with the mailbox closed, as for any game");

    chfsession_stop(s);
    chfsession_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
