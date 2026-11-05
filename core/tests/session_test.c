/*
 * session_test -- the frontend contract end to end, through the public API
 * only: settings round-trip and persist across sessions, paths resolve
 * inside the given tree, CONFIG boots, the keypads, the discs and the
 * console's RESET reach the machine, a FujiNet app opens and runs and sees a
 * held key, RESET restarts it, Reset to CONFIG ejects it, an image the
 * console cannot take is refused with the reason, a game and a game in a zip
 * run, a remembered cartridge boots at the next start, and the PAL machine
 * runs at 50 Hz.
 *
 * No firmware and no BIOS (the console has none): the programs are this
 * test's own 2650 code (hand-assembled below), kept in the UVI's spare RAM
 * at $18E0-$18E3 and read back through the debugger.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "chfsession.h"
#include "chfdebug.h"
#include "test_tmpdir.h"
#include "test_zip.h"

static int failures;
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

/* ---- the programs ----------------------------------------------------------------
 * 2650 code at $0000 of the cartridge. State lives in the UVI's spare RAM:
 *
 *   $18E0  counter, incremented in the main loop
 *   $18E1  [app] player 1's keypad column 1 ($1900)
 *   $18E2  boots: incremented at every start (the 2650's RESET leaves RAM alone)
 *   $18E3  [app] the console panel ($1908: START, OPTION, SELECT)
 *
 * A FujiNet app is an 8K image with "FUJI" at $1CFC (the mailbox claim).
 *
 *   0000  LODA,r0 $18E2 / ADDI,r0 1 / STRA,r0 $18E2   boots++
 *   0008  LODI,r0 0 / STRA,r0 $18E0                    counter = 0
 *   000D  [app: LODA,r0 $1900 / STRA,r0 $18E1 / LODA,r0 $1908 / STRA,r0 $18E3]
 *         LODA,r0 $18E0 / ADDI,r0 1 / STRA,r0 $18E0 / BCTA,UN $000D
 */
#define RAM_COUNTER 0x18E0
#define RAM_KEY     0x18E1
#define RAM_BOOTS   0x18E2
#define RAM_PANEL   0x18E3

static const uint8_t prologue[] = {
    0x0C, 0x18, 0xE2,           /* 0000: loda,r0 $18E2 */
    0x84, 0x01,                 /* 0003: addi,r0 1 */
    0xCC, 0x18, 0xE2,           /* 0005: stra,r0 $18E2 */
    0x04, 0x00,                 /* 0008: lodi,r0 0 */
    0xCC, 0x18, 0xE0,           /* 000A: stra,r0 $18E0 */
};
static const uint8_t app_reads[] = {
    0x0C, 0x19, 0x00,           /* 000D: loda,r0 $1900 */
    0xCC, 0x18, 0xE1,           /* 0010: stra,r0 $18E1 */
    0x0C, 0x19, 0x08,           /* 0013: loda,r0 $1908 */
    0xCC, 0x18, 0xE3,           /* 0016: stra,r0 $18E3 */
};
static const uint8_t count_loop[] = {
    0x0C, 0x18, 0xE0,           /* loda,r0 $18E0 */
    0x84, 0x01,                 /* addi,r0 1 */
    0xCC, 0x18, 0xE0,           /* stra,r0 $18E0 */
    0x1F, 0x00, 0x0D,           /* bcta,un $000D */
};

static void build_cart(uint8_t *img, size_t size, int app)
{
    size_t n = sizeof prologue;

    memset(img, 0xFF, size);
    memcpy(img, prologue, n);
    if (app) {
        memcpy(img + n, app_reads, sizeof app_reads);
        n += sizeof app_reads;
        memcpy(img + 0x1CFC, "FUJI", 4);
    }
    memcpy(img + n, count_loop, sizeof count_loop);
    if (app) {
        /* the loop jumps back over the reads */
    } else {
        /* a game's loop starts right after the prologue */
        img[n + 9] = 0x00;
        img[n + 10] = (uint8_t)n;
    }
}

static int write_file(const char *path, const uint8_t *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(data, 1, n, f);
    fclose(f);
    return 1;
}

/* ---- watching the machine ------------------------------------------------------------ */

static uint32_t px[CHFSESSION_FB_WIDTH * CHFSESSION_FB_HEIGHT];

static int wait_frames(chfsession *s, uint64_t *serial, int count, int timeout_ms)
{
    uint64_t start = chfsession_frame_count(s);
    int waited = 0;
    while (waited < timeout_ms) {
        chfsession_copy_frame(s, px, serial);
        if (chfsession_frame_count(s) >= start + (uint64_t)count) return 1;
        sleep_ms(2); waited += 2;
    }
    return 0;
}

/* Poll the cartridge status until it contains `want`, or time out. */
static int wait_status(chfsession *s, const char *want, int timeout_ms)
{
    char st[160];
    int waited = 0;
    while (waited < timeout_ms) {
        chfsession_cart_status(s, st, sizeof st);
        if (strstr(st, want)) return 1;
        sleep_ms(20); waited += 20;
    }
    printf("  (status: %s)\n", st);
    return 0;
}

/* The cartridge is powered on by the machine thread, after load_cart (or a
 * power cycle) has returned -- a failed BoIP connect must not stall the
 * caller -- so what it reports (booted_game, the status) is not true until
 * the thread gets there. Poll for it, as wait_status does. */
static int wait_booted_game(chfsession *s, int want, int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms) {
        if (!!chfsession_cart_booted_game(s) == !!want) return 1;
        sleep_ms(5); waited += 5;
    }
    return 0;
}

/* The programs keep their state in the UVI's spare RAM ($18E0-$18E3); peek at
 * it through the debugger (which stops the machine while attached) and let it
 * run on. */
static uint8_t ram(chfsession *s, uint16_t addr)
{
    chfdebug *d = chfsession_debugger(s);
    uint8_t v = 0;
    chfdebug_attach(d);
    chfdebug_read(d, CHF_SPACE_PROG, addr, &v, 1);
    chfdebug_detach(d);
    return v;
}

/* the counter changes between samples: it wraps in microseconds, so one
 * coincidence is not "stopped" */
static int moving(chfsession *s)
{
    uint8_t a = ram(s, RAM_COUNTER);
    /* a freshly loaded game needs a few frames to start (a loaded CI runner
     * more than a desk), so give it three seconds before "stopped" */
    for (int i = 0; i < 50; i++) {
        sleep_ms(60);
        if (ram(s, RAM_COUNTER) != a) return 1;
    }
    return 0;
}

static int wait_ram(chfsession *s, uint16_t addr, int want, int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms) {
        if (ram(s, addr) == want) return 1;
        sleep_ms(30); waited += 30;
    }
    return 0;
}

int main(void)
{
    char cfg[300], data[400], app[500], game[500], big[500], zip1[500], zip2[500],
         launch[400], why[256];
    static uint8_t img[0x2000], blob[200000];
    chfsession_paths p;
    chfsession *s;
    chfsession_start_opts o;
    uint64_t serial = 0;
    int i;

    test_tmpdir(cfg, sizeof cfg, "cfg");
    snprintf(data, sizeof data, "%s/data", cfg);
    if (!test_getcwd(launch, sizeof launch))
        return 1;
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data;
    p.fujinet_lib = "";     /* no runtime: the cart runs link-down */

    snprintf(app, sizeof app, "%s/counter.bin", cfg);
    build_cart(img, 0x2000, 1);
    check(write_file(app, img, 0x2000), "wrote a FujiNet app");
    snprintf(game, sizeof game, "%s/game.bin", cfg);
    build_cart(img, 4096, 0);
    check(write_file(game, img, 4096), "wrote a game");
    snprintf(big, sizeof big, "%s/huge.bin", cfg);
    memset(blob, 0x55, sizeof blob);
    check(write_file(big, blob, sizeof blob), "wrote a 200,000-byte image");
    {
        const char *one[] = { "game.bin" };
        const uint8_t *od[] = { img };
        uint32_t os[] = { 4096 };
        const char *two[] = { "a.bin", "b.bin" };
        const uint8_t *td[] = { img, img };
        uint32_t ts[] = { 4096, 4096 };

        snprintf(zip1, sizeof zip1, "%s/game.zip", cfg);
        check(test_zip_write(zip1, 1, one, od, os), "wrote a zip of the game");
        snprintf(zip2, sizeof zip2, "%s/pair.zip", cfg);
        check(test_zip_write(zip2, 2, two, td, ts), "wrote a zip of two");
    }

    s = chfsession_new(&p);
    check(s != NULL, "session created");
    if (!s) return 1;

    check(strcmp(chfsession_config_path(s), cfg) == 0, "config path is the given tree");
    check(strncmp(chfsession_carts_path(s), data, strlen(data)) == 0, "carts dir is under the data tree");
    check(strncmp(chfsession_sd_path(s), data, strlen(data)) == 0, "SD path is under the data tree");

    chfsession_set_int(s, "answer", 42);
    chfsession_set_str(s, "greeting", "hello");
    check(chfsession_get_int(s, "answer", 0) == 42, "int setting round-trips");
    check(strcmp(chfsession_get_str(s, "greeting", ""), "hello") == 0, "string setting round-trips");
    check(chfsession_get_int(s, "nope", 7) == 7, "missing setting yields its default");

    chfsession_default_opts(s, &o);
    check(o.enable_fujinet == 1 && o.enable_audio == 1, "default opts enable FujiNet and audio");
    check(o.machine == CHF_MACHINE_ARCADIA && o.analog_joystick == 1, "the NTSC Channel F, analog disc");
    check(o.cart_path == NULL, "no remembered cartridge: CONFIG");
    check(strstr(chf_machine_name(CHF_MACHINE_ARCADIA), "Channel F") != NULL
          && !chf_machine_is_pal(CHF_MACHINE_ARCADIA) && chf_machine_is_pal(CHF_MACHINE_ARCADIA_PAL),
          "the machine names and standards");

    o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
    check(chfsession_start(s, &o) == 0, "session starts (CONFIG, no FujiNet)");
    if (!chfsession_is_running(s)) { printf("error: %s\n", chfsession_last_error(s)); return 1; }

    check(wait_frames(s, &serial, 30, 5000), "frames arrive");
    check(wait_status(s, "link down", 5000), "CONFIG runs link-down");
    check(!chfsession_cart_booted_game(s), "CONFIG is not a booted game");
    {
        char name[96];
        chfsession_cart_type_name(s, name, sizeof name);
        check(strcmp(name, "FujiNet CONFIG") == 0, "the cartridge is the FujiNet CONFIG");
    }
    check(chfsession_refresh_rate(s) == 60, "60 Hz");
    check(!chfsession_is_paused(s), "not paused");
    {
        uint64_t z = 0; int lit = 0, tries;
        check(chfsession_copy_frame(s, px, &z) == 1, "a forced copy (serial 0) always copies");
        /* up to 15 s: a loaded CI runner can take a while to the first paint */
        for (tries = 0; tries < 150; tries++) {
            lit = 0;
            for (i = 0; i < CHFSESSION_FB_WIDTH * CHFSESSION_FB_HEIGHT; i++)
                lit += (px[i] & 0xffffff) != 0;
            if (lit > 50) break;
            sleep_ms(100);
            z = 0;
            chfsession_copy_frame(s, px, &z);
        }
        check(lit > 50, "the CONFIG client painted something");
    }

    /* pause and resume */
    chfsession_set_paused(s, 1);
    sleep_ms(100);
    {
        uint64_t f0 = chfsession_frame_count(s);
        sleep_ms(250);
        check(chfsession_is_paused(s) && chfsession_frame_count(s) - f0 <= 1, "paused: the frames stop");
    }
    chfsession_set_paused(s, 0);
    serial = 0;
    check(!chfsession_is_paused(s) && wait_frames(s, &serial, 10, 3000), "resumed: they run again");

    /* keyboard: the default map */
    check(chfsession_key(s, '1', 1) == 1, "1 is bound (player 1's keypad 1, key 0)");
    check(chfsession_key_held(s, 0), "and holds it");
    check(chfsession_key(s, '1', 0) == 1 && !chfsession_key_held(s, 0), "and releases it");
    check(chfsession_key(s, 'q', 1) == 1 && chfsession_key_held(s, 12), "Q is player 2's keypad 1 (key 12)");
    chfsession_key(s, 'q', 0);
    check(chfsession_key(s, CHF_KEYSYM_F2, 1) == 1 && chfsession_key_held(s, CHF_KEY_START), "F2 is START");
    chfsession_key(s, CHF_KEYSYM_F2, 0);
    check(chfsession_key(s, CHF_KEYSYM_UP, 1) == 1 && (chfsession_buttons_held(s, 0) & (1u << CHF_ACT_UP)),
          "the up arrow is disc 1 up");
    check(chfsession_key(s, CHF_KEYSYM_RCTRL, 1) == 1 && (chfsession_buttons_held(s, 0) & (1u << CHF_ACT_FIRE)),
          "Right Ctrl is its fire button");
    chfsession_key(s, CHF_KEYSYM_DOWN, 1);
    check((chfsession_buttons_held(s, 0) & ((1u << CHF_ACT_UP) | (1u << CHF_ACT_DOWN))) == 0,
          "up and down together: the disc holds neither");
    chfsession_key(s, CHF_KEYSYM_DOWN, 0);
    check((chfsession_buttons_held(s, 0) & (1u << CHF_ACT_UP)) != 0, "up again once down lets go");
    chfsession_release_all(s);
    check(chfsession_buttons_held(s, 0) == 0, "release_all lets go of the keyboard");
    check(chfsession_key(s, CHF_KEYSYM_KP_6, 1) == 1 && (chfsession_buttons_held(s, 1) & (1u << CHF_ACT_RIGHT)),
          "the keypad's 6 is disc 2 right");
    chfsession_key(s, CHF_KEYSYM_KP_6, 0);
    check(chfsession_buttons_held(s, 1) == 0, "released");
    {
        int dx = 9, dy = 9;
        chfsession_disc_get(s, 0, &dx, &dy);
        check(dx == 0 && dy == 0, "no gamepad: the analog disc is centred");
        chfsession_disc_set(s, 1, 500, -40);
        chfsession_disc_get(s, 1, &dx, &dy);
        check(dx == 127 && dy == -40, "a pointer drag sets the analog disc, clamped");
        chfsession_disc_set(s, 1, 0, 0);
        chfsession_disc_get(s, 1, &dx, &dy);
        check(dx == 0 && dy == 0, "0,0 releases it");
    }
    check(chfsession_key(s, CHF_KEYSYM_F11, 1) == 0, "an unbound key is ignored");
    check(chfsession_key_sysaction(s, CHF_KEYSYM_ESCAPE) == CHF_SYSACT_RESET_CONFIG,
          "Escape is the Reset to CONFIG system action");
    check(chfsession_target_for_key(s, CHF_KEYSYM_F1) == CHF_TARGET_SWITCH(CHF_SW_RESET), "F1 is the console's RESET");
    check(chfsession_key(s, CHF_KEYSYM_F1, 1) == 1 && chfsession_switch_held(s, CHF_SW_RESET), "holding it holds RESET");
    chfsession_key(s, CHF_KEYSYM_F1, 0);
    check(!chfsession_switch_held(s, CHF_SW_RESET), "and letting go lets go");
    chfsession_press(s, CHF_TARGET_KEY(20), 1);
    check(chfsession_key_held(s, 20), "the controllers window can press a key directly");
    chfsession_press(s, CHF_TARGET_KEY(20), 0);
    check(wait_frames(s, &serial, 10, 3000), "the machine keeps running after input");

    /* images the console cannot take */
    check(chfsession_check_cart(big, why, sizeof why) == 0, "a 200,000-byte image is refused");
    check(strstr(why, "larger") != NULL, "with the reason");
    check(chfsession_load_cart(s, big) == -1, "load_cart refuses it");
    check(!chfsession_cart_booted_game(s) && chfsession_cart_path(s)[0] == '\0', "and CONFIG keeps running");
    check(chfsession_load_cart(s, zip2) == -1 && strstr(chfsession_last_error(s), "exactly one") != NULL,
          "a zip of two cartridges is refused, with the reason");

    /* a game: its own cartridge, no mailbox, no BIOS needed */
    check(chfsession_check_cart(game, NULL, 0) == 1 && !chfsession_cart_is_app(game),
          "the game plans, and is not an app");
    check(chfsession_load_cart(s, game) == 0, "load_cart opens the game");
    check(chfsession_cart_path(s)[0] != '\0' && strcmp(chfsession_get_str(s, "cart", ""), game) == 0,
          "remembered and persisted");
    check(wait_booted_game(s, 1, 5000), "a game is a booted cartridge");
    check(moving(s), "and the game runs");
    check(wait_status(s, "game running", 3000), "the cartridge reports a game (mailbox closed)");
    {
        char name[96];
        chfsession_cart_type_name(s, name, sizeof name);
        check(strstr(name, "4 KB") != NULL, "the cartridge is 4 KB");
    }
    check(chfsession_reset(s) == 0 && moving(s), "RESET on a game: it runs again");

    /* the same game from a zip */
    check(chfsession_load_cart(s, zip1) == 0, "a zip of one cartridge opens");
    check(strcmp(chfsession_cart_path(s), zip1) == 0 && moving(s), "remembered as the zip, and it runs");

    /* open a FujiNet app: served from power-on, the mailbox live */
    check(chfsession_cart_is_app(app), "the app is an app");
    check(chfsession_load_cart(s, app) == 0, "load_cart");
    check(strcmp(chfsession_cart_path(s), app) == 0, "the cart path is remembered");
    check(wait_booted_game(s, 1, 5000), "something other than CONFIG is in the cartridge");
    check(moving(s), "the app runs (its counter moves)");
    {
        char name[96];
        chfsession_cart_type_name(s, name, sizeof name);
        check(strcmp(name, "FujiNet app") == 0, "the cartridge is a FujiNet app");
    }

    /* the keypad and the panel reach the program through the UVI's ports */
    {
        uint8_t idle = ram(s, RAM_KEY), idle_panel = ram(s, RAM_PANEL);

        chfsession_key(s, '1', 1);
        {
            int waited = 0, changed = 0;
            while (waited < 3000 && !(changed = ram(s, RAM_KEY) != idle)) { sleep_ms(30); waited += 30; }
            check(changed, "player 1's key 1 changes what $1900 reads");
        }
        chfsession_key(s, '1', 0);
        check(wait_ram(s, RAM_KEY, idle, 3000), "and it reads as before once let go");
        chfsession_key(s, CHF_KEYSYM_F2, 1);
        {
            int waited = 0, changed = 0;
            while (waited < 3000 && !(changed = ram(s, RAM_PANEL) != idle_panel)) { sleep_ms(30); waited += 30; }
            check(changed, "START changes what $1908 reads");
        }
        chfsession_key(s, CHF_KEYSYM_F2, 0);
        check(wait_ram(s, RAM_PANEL, idle_panel, 3000), "and not once let go");
    }

    /* RESET: the 2650 starts over, the app is still open */
    {
        uint8_t boots = ram(s, RAM_BOOTS);
        check(chfsession_reset(s) == 0, "RESET");
        check(wait_ram(s, RAM_BOOTS, (uint8_t)(boots + 1), 3000), "the app started over");
        check(moving(s), "and runs");
        check(strcmp(chfsession_cart_path(s), app) == 0, "still the open cartridge");
    }

    /* Reset to CONFIG: a power cycle that ejects it */
    check(chfsession_reset_to_config(s) == 0, "reset to CONFIG");
    check(chfsession_cart_path(s)[0] == '\0', "the cartridge is ejected");
    check(strcmp(chfsession_get_str(s, "cart", "x"), "") == 0, "and forgotten");
    serial = 0;
    check(wait_frames(s, &serial, 10, 5000), "frames flow from the new console");
    check(wait_status(s, "link down", 5000), "CONFIG is back");
    check(!chfsession_cart_booted_game(s), "no game in the cartridge");
    check(chfsession_cart_link_up(s) == 0, "with no runtime the cart reports link down");

    /* A path relative to where the app started still means that file after
     * the process's working directory moves: the in-process FujiNet makes
     * its runtime root the working directory. */
    {
        char rel[64], abs[900];
        snprintf(rel, sizeof rel, "chf-rel-%ld.bin", (long)test_getpid());
        snprintf(abs, sizeof abs, "%s/%s", launch, rel);
        build_cart(img, 0x2000, 1);
        check(write_file(abs, img, 0x2000), "wrote an image in the launch directory");
        check(test_chdir(data) == 0, "the working directory moves away");
        check(chfsession_check_cart(rel, NULL, 0) == 1, "a bare name still finds it");
        check(chfsession_load_cart(s, rel) == 0, "and opens it");
        check(strcmp(chfsession_cart_path(s), abs) == 0, "remembered by its full path");
        check(moving(s), "and it runs");
        test_chdir(launch);
        remove(abs);
    }

    /* a remembered cartridge boots at the next start */
    check(chfsession_load_cart(s, app) == 0, "open the app again");
    chfsession_stop(s);
    check(!chfsession_is_running(s), "session stops");
    chfsession_free(s);

    s = chfsession_new(&p);
    check(s && chfsession_get_int(s, "answer", 0) == 42, "settings persisted across sessions");
    if (s) {
        chfsession_default_opts(s, &o);
        check(o.cart_path && strcmp(o.cart_path, app) == 0, "the cartridge is remembered");
        o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
        check(chfsession_start(s, &o) == 0, "restart");
        check(moving(s), "and it boots the remembered cartridge");

        /* the PAL machine: 50 Hz, the same cartridge */
        chfsession_set_int(s, "machine", CHF_MACHINE_ARCADIA_PAL);
        check(chfsession_restart(s) == 0, "the PAL console: a power cycle");
        check(chfsession_refresh_rate(s) == 50, "50 Hz");
        serial = 0;
        check(wait_frames(s, &serial, 10, 5000) && moving(s), "it runs the same cartridge");
        chfsession_set_int(s, "machine", CHF_MACHINE_ARCADIA);
        check(chfsession_restart(s) == 0 && chfsession_refresh_rate(s) == 60, "and back to the NTSC console");

        /* a remembered cartridge that has gone: CONFIG, and forgotten */
        chfsession_stop(s);
        chfsession_set_str(s, "cart", "/nonexistent/gone.bin");
        chfsession_default_opts(s, &o);
        o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
        check(chfsession_start(s, &o) == 0, "a vanished remembered cartridge does not stop the start");
        check(chfsession_cart_path(s)[0] == '\0' && wait_status(s, "link down", 5000), "CONFIG boots instead");
        chfsession_stop(s);
        chfsession_free(s);
    }

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
