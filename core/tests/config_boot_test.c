/*
 * config_boot_test -- CONFIG, driven like a user from the hand controller, boots an
 * image through the in-process FujiNet. The test suite's own counter app (a 16K image
 * claiming the mailbox) and counter game (a plain image) go to the SD host (Import
 * Cartridge to SD, and a dummy file first so neither is the directory's first row);
 * then, on CONFIG's own screens: fire opens host slot 1 (SD) -> the stick's "back"
 * moves the bar down to the file -> fire boots it -> FujiNet mounts it as ROM media
 * and pushes it, the cart stages it, CONFIG arms the swap and hands over through its
 * stub in the cartridge's RAM, and the app runs (its counter byte at $8000 moves).
 * Reset to CONFIG brings CONFIG back; then the game booted the same way has the
 * mailbox closed, and runs on the user's BIOS -- or, on the app's stand-in, parks the
 * console and the session asks for the BIOS.
 *
 * CONFIG draws its text in the VRAM with its own font, so the test does not read the
 * screen: it follows CONFIG's state variables in the cartridge's RAM, the ones the
 * firmware's own emu/cfgdrive.lua drives it by (VSTATE $8006: 4 the host list, 5 the
 * directory browser; VNROW $8008 the rows listed), and finds a file's row from the SD
 * folder's own listing (CONFIG shows it in name order). The row-0 caveat is real: the
 * browser cannot boot the first entry of a listing (fujinet-pc NAKs the
 * SET_DIRECTORY_POSITION it sends), hence the dummy first.
 *
 * Needs the FujiNet runtime; SKIPs (77) without it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "chfsession.h"
#include "test_chf.h"
#include "test_tmpdir.h"

#define APP_NAME   "ACOUNTER.BIN"
#define GAME_NAME  "BGAME.BIN"
#define DUMMY_NAME "AAAA.BIN"

#define VSTATE   0x8006
#define VNROW    0x8008
#define ST_HOST  4
#define ST_FILE  5

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static int wait_state(int state, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 100) {
        if (test_peek(VSTATE) == state)
            return 1;
        test_sleep_ms(100);
    }
    printf("  (CONFIG's state is %d, wanted %d)\n", test_peek(VSTATE), state);
    return 0;
}

/* one press of a hand-controller contact, held long enough for CONFIG's poll to see it */
static void press(chfsession *s, int act)
{
    chfsession_press(s, CHF_TARGET_PORT(0, act), 1);
    test_sleep_ms(200);
    chfsession_press(s, CHF_TARGET_PORT(0, act), 0);
    test_sleep_ms(400);
}

static int by_name(const void *a, const void *b)
{
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

/* the row `name` has in CONFIG's listing of the SD folder: its place in the name-ordered entries */
static int sd_row(const char *sd, const char *name)
{
    char *names[256];
    int n = 0, row = -1;
    DIR *d = opendir(sd);
    struct dirent *e;

    if (!d)
        return -1;
    while ((e = readdir(d)) != NULL && n < 256) {
        if (e->d_name[0] != '.')
            names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof names[0], by_name);
    for (int i = 0; i < n; i++) {
        if (!strcasecmp(names[i], name))
            row = i;
        free(names[i]);
    }
    return row;
}

/* From the hosts screen: open slot 1 (SD), the bar down onto `name`, boot it.
 * Returns 1 once the cart has swapped. */
static int boot_from_sd(chfsession *s, const char *name)
{
    int row, booted = 0;

    if (!wait_state(ST_HOST, 30000))
        return 0;
    test_sleep_ms(2500);                    /* the host list settles (cfgdrive.lua waits 240 frames) */
    press(s, CHF_ACT_PUSH);
    if (!wait_state(ST_FILE, 15000))
        return 0;
    for (int waited = 0; waited < 10000 && test_peek(VNROW) == 0; waited += 100)
        test_sleep_ms(100);
    row = sd_row(chfsession_sd_path(s), name);
    printf("  %s is row %d of %d\n", name, row, test_peek(VNROW));
    if (row < 1 || row >= test_peek(VNROW))
        return 0;
    for (int i = 0; i < row; i++)
        press(s, CHF_ACT_BACK);
    press(s, CHF_ACT_PUSH);
    for (int waited = 0; waited < 30000 && !booted; waited += 100) {
        booted = chfsession_cart_booted_game(s);
        if (!booted)
            test_sleep_ms(100);
    }
    return booted;
}

int main(void)
{
    static uint8_t img[16384];
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

    /* a plain 2K game the dummy is: it only has to be a file sorting before the others */
    snprintf(src, sizeof src, "%s/%s", cfg, DUMMY_NAME);
    memset(img, 0xff, 2048);
    img[0] = 0x55;
    test_write_file(src, img, 2048);
    check(chfsession_import_cart_to_sd(s, src, dest, sizeof dest) == 0, "a dummy file goes to the SD host first, so no wanted file is row 0");
    snprintf(src, sizeof src, "%s/%s", cfg, APP_NAME);
    test_counter_image(img, 1);
    test_write_file(src, img, sizeof img);
    check(chfsession_import_cart_to_sd(s, src, dest, sizeof dest) == 0, "the app goes to the SD host");
    snprintf(src, sizeof src, "%s/%s", cfg, GAME_NAME);
    test_counter_image(img, 0);
    test_write_file(src, img, sizeof img);
    check(chfsession_import_cart_to_sd(s, src, dest, sizeof dest) == 0, "and a game (the same program, no claim)");

    check(wait_state(ST_HOST, 30000), "CONFIG reaches its host list over the live link");

    check(boot_from_sd(s, APP_NAME), "fire opens SD, back onto the file, fire boots it: the cart swaps it in");
    {
        uint8_t a = test_peek(TEST_COUNTER_ADDR);

        test_sleep_ms(300);
        check(test_peek(TEST_COUNTER_ADDR) != a, "the app runs (its counter moves)");
    }
    chfsession_cart_status(s, status, sizeof status);
    check(strstr(status, "mailbox closed") == NULL, "the app kept the mailbox");

    check(chfsession_reset_to_config(s) == 0, "Reset to CONFIG");
    check(wait_state(ST_HOST, 30000), "CONFIG is back on its host list");
    check(chfsession_cart_booted_game(s) == 0, "the session sees CONFIG again");

    check(boot_from_sd(s, GAME_NAME), "the game boots the same way");
    chfsession_cart_status(s, status, sizeof status);
    check(strstr(status, "mailbox closed") != NULL, "with the mailbox closed, as for any game");
    if (chfsession_bios_in_use(s) >= 0) {
        uint8_t a = test_peek(TEST_COUNTER_ADDR);

        test_sleep_ms(300);
        check(test_peek(TEST_COUNTER_ADDR) != a, "and runs on the console's BIOS");
    } else {
        /* the stand-in has no BIOS to run a game on: it parks and the session says Import BIOS */
        test_sleep_ms(500);
        check(chfsession_needs_bios(s), "on the stand-in BIOS a game parks the console: the session asks for the BIOS");
    }

    chfsession_stop(s);
    chfsession_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
