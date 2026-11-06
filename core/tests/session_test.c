/*
 * session_test -- the frontend contract end to end, through the public API
 * only: settings round-trip and persist across sessions, paths resolve
 * inside the given tree, CONFIG boots, the hand controllers (keys, the
 * pointer, a wheel pulse) and the console's buttons reach the machine, a
 * FujiNet app opens and runs and sees them, RESET restarts it, Reset to
 * CONFIG ejects it, an image the console cannot take is refused with the
 * reason, a game is refused without the console's BIOS and runs with it, a
 * remembered cartridge boots at the next start, and the PAL machine runs at
 * 50 Hz.
 *
 * The programs are this test's own F8 code (hand-assembled below), kept in
 * the FujiNet cartridge's RAM at $8000 and read back through the debugger.
 * They run on the app's stand-in BIOS ("bios" = the stand-in), so nothing
 * here needs firmware; the parts that run a game on the real BIOS import
 * MAME's channelf.zip (CHF_MAME_ROMS, or ~/Workspace/mame/roms) and are
 * skipped when it is not there.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
 * F8 code at $0802 of the cartridge (a Videocart starts with $55 and the BIOS
 * jumps to $0802). A FujiNet app is a 16K image with "FUJI" at $3FFC (console
 * $47FC), and has 30K of RAM at $8000:
 *
 *   $8000  port 1 read now: the right controller (Player 1), active low
 *   $8001  port 4: the left controller (Player 2)
 *   $8002  port 0: the console's buttons (low nibble), active low
 *   $8010  boots: incremented at every start (RESET leaves the RAM alone)
 *   $8020  counter, incremented in the main loop
 *   $8030  Player 1's port ANDed over time (a bit once low stays low), $8031
 *          Player 2's: the test sets them to $FF and reads what went by
 *
 *   0802  DCI $8010 / LM / AI 1 / DCI $8010 / ST            boots++
 *   080C  LIS 0 / OUTS 0 / OUTS 1 / OUTS 4                   controllers on (port 0 bit 6 low)
 *   0810  DCI $8000 / INS 1 / ST / INS 4 / ST / INS 0 / ST   the ports -> $8000-$8002
 *   0819  DCI $8030 / INS 1 / NM / DCI $8030 / ST            sticky P1
 *   0822  DCI $8031 / INS 4 / NM / DCI $8031 / ST            sticky P2
 *   082B  DCI $8020 / LM / AI 1 / DCI $8020 / ST             counter++
 *   0834  BR $0810
 *
 * A game is a Videocart that counts in the 2K of RAM at $2800 (the by-path
 * rule gives an image the software list does not name that RAM):
 *
 *   0802  DCI $2800 / LM / AI 1 / DCI $2800 / ST / BR $0802
 */
#define RAM_P1     0x8000
#define RAM_P2     0x8001
#define RAM_PANEL  0x8002
#define RAM_BOOTS  0x8010
#define RAM_COUNT  0x8020
#define RAM_STICKY1 0x8030
#define RAM_STICKY2 0x8031

static const uint8_t app_code[] = {
    0x2A, 0x80, 0x10, 0x16, 0x24, 0x01, 0x2A, 0x80, 0x10, 0x17,         /* 0802 */
    0x70, 0xB0, 0xB1, 0xB4,                                             /* 080C */
    0x2A, 0x80, 0x00, 0xA1, 0x17, 0xA4, 0x17, 0xA0, 0x17,               /* 0810 */
    0x2A, 0x80, 0x30, 0xA1, 0x8A, 0x2A, 0x80, 0x30, 0x17,               /* 0819 */
    0x2A, 0x80, 0x31, 0xA4, 0x8A, 0x2A, 0x80, 0x31, 0x17,               /* 0822 */
    0x2A, 0x80, 0x20, 0x16, 0x24, 0x01, 0x2A, 0x80, 0x20, 0x17,         /* 082B */
    0x90, 0xDA,                                                         /* 0835: BR $0810 */
};
static const uint8_t game_code[] = {
    0x2A, 0x28, 0x00, 0x16, 0x24, 0x01, 0x2A, 0x28, 0x00, 0x17,
    0x90, 0xF5,                                                         /* BR $0802 */
};

static void build_app(uint8_t *img)
{
    memset(img, 0xFF, 0x4000);
    img[0] = 0x55;
    img[1] = 0x00;
    memcpy(img + 2, app_code, sizeof app_code);
    memcpy(img + 0x3FFC, "FUJI", 4);
}

static void build_game(uint8_t *img, size_t size)
{
    memset(img, 0xFF, size);
    img[0] = 0x55;
    img[1] = 0x00;
    memcpy(img + 2, game_code, sizeof game_code);
}

static int write_file(const char *path, const uint8_t *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(data, 1, n, f);
    fclose(f);
    return 1;
}

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

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

/* The programs keep their state in RAM; peek at it through the debugger
 * (which stops the machine while attached) and let it run on. */
static uint8_t ram(chfsession *s, uint16_t addr)
{
    chfdebug *d = chfsession_debugger(s);
    uint8_t v = 0;
    chfdebug_attach(d);
    chfdebug_read(d, CHF_SPACE_PROG, addr, &v, 1);
    chfdebug_detach(d);
    return v;
}

static void poke(chfsession *s, uint16_t addr, uint8_t v)
{
    chfdebug *d = chfsession_debugger(s);
    chfdebug_attach(d);
    chfdebug_write(d, CHF_SPACE_PROG, addr, v);
    chfdebug_detach(d);
}

/* the counter changes between samples: it wraps in microseconds, so one
 * coincidence is not "stopped" */
static int moving(chfsession *s, uint16_t addr)
{
    uint8_t a = ram(s, addr);
    /* a freshly loaded program needs a few frames to start (a loaded CI
     * runner more than a desk, the real BIOS a splash first), so give it six
     * seconds before "stopped" */
    for (int i = 0; i < 100; i++) {
        sleep_ms(60);
        if (ram(s, addr) != a) return 1;
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

/* The F8 is running a game when its PC is in the program the test wrote. */
static int game_running(chfsession *s, int timeout_ms)
{
    chfdebug *d = chfsession_debugger(s);
    int waited = 0;
    while (waited < timeout_ms) {
        chfdebug_cpu cpu;
        chfdebug_attach(d);
        chfdebug_cpu_get(d, &cpu);
        chfdebug_detach(d);
        if (cpu.pc >= 0x0802 && cpu.pc < 0x0802 + (int)sizeof game_code) return 1;
        sleep_ms(50); waited += 50;
    }
    return 0;
}

/* What the controller reads, as the machine reads it (active low). */
#define LOW(mask) ((uint8_t)~(mask))
#define B(a) (1u << (a))

static int wait_port(chfsession *s, uint16_t addr, uint8_t want, int timeout_ms)
{
    return wait_ram(s, addr, want, timeout_ms);
}

/* The console's buttons are the low nibble of port 0. */
static int wait_panel(chfsession *s, uint8_t want, int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms) {
        if ((ram(s, RAM_PANEL) & 0x0F) == want) return 1;
        sleep_ms(30); waited += 30;
    }
    return 0;
}

/* MAME's BIOS zip, when this machine has it. */
static int mame_zip(char *dst, size_t n)
{
    const char *dir = getenv("CHF_MAME_ROMS");
    if (dir && *dir)
        snprintf(dst, n, "%s/channelf.zip", dir);
    else
        snprintf(dst, n, "%s/Workspace/mame/roms/channelf.zip", getenv("HOME") ? getenv("HOME") : ".");
    return exists(dst);
}

int main(void)
{
    char cfg[300], data[400], app[500], game[500], big[500], zip1[500], zip2[500],
         launch[400], why[256], bioszip[600];
    static uint8_t img[0x4000], blob[300000];
    chfsession_paths p;
    chfsession *s;
    chfsession_start_opts o;
    uint64_t serial = 0;
    int i, have_bios;

    test_tmpdir(cfg, sizeof cfg, "cfg");
    snprintf(data, sizeof data, "%s/data", cfg);
    if (!test_getcwd(launch, sizeof launch))
        return 1;
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data;
    p.fujinet_lib = "";     /* no runtime: the cart runs link-down */

    snprintf(app, sizeof app, "%s/counter.bin", cfg);
    build_app(img);
    check(write_file(app, img, 0x4000), "wrote a FujiNet app");
    snprintf(game, sizeof game, "%s/game.bin", cfg);
    build_game(img, 2048);
    check(write_file(game, img, 2048), "wrote a game");
    snprintf(big, sizeof big, "%s/huge.bin", cfg);
    memset(blob, 0x55, sizeof blob);
    check(write_file(big, blob, sizeof blob), "wrote a 300,000-byte image");
    {
        const char *one[] = { "game.bin" };
        const uint8_t *od[] = { img };
        uint32_t os[] = { 2048 };
        const char *two[] = { "a.bin", "b.bin" };
        const uint8_t *td[] = { img, img };
        uint32_t ts[] = { 2048, 2048 };

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
    check(strncmp(chfsession_roms_path(s), data, strlen(data)) == 0, "ROM dir is under the data tree");

    chfsession_set_int(s, "answer", 42);
    chfsession_set_str(s, "greeting", "hello");
    check(chfsession_get_int(s, "answer", 0) == 42, "int setting round-trips");
    check(strcmp(chfsession_get_str(s, "greeting", ""), "hello") == 0, "string setting round-trips");
    check(chfsession_get_int(s, "nope", 7) == 7, "missing setting yields its default");

    chfsession_default_opts(s, &o);
    check(o.enable_fujinet == 1 && o.enable_audio == 1, "default opts enable FujiNet and audio");
    check(o.machine == CHF_MACHINE_CHANNELF && o.bios == CHF_BIOS_AUTO && o.right_stick == 1,
          "the NTSC Channel F, the automatic BIOS, the right stick on");
    check(o.cart_path == NULL, "no remembered cartridge: CONFIG");
    check(strstr(chf_machine_name(CHF_MACHINE_CHANNELF), "Channel F") != NULL
          && !chf_machine_is_pal(CHF_MACHINE_CHANNELF) && chf_machine_is_pal(CHF_MACHINE_VIDEOPLAY)
          && chf_machine_is_pal(CHF_MACHINE_VIDEOPLAY2) && !chf_machine_is_pal(CHF_MACHINE_CHANNELF2),
          "the machine names and standards");
    {
        int num = 0, den = 0;
        chfsession_display_aspect(CHF_MACHINE_CHANNELF, 0, &num, &den);
        check(num == 4 && den == 3, "the TV's shape is 4:3");
        chfsession_display_aspect(CHF_MACHINE_CHANNELF, 1, &num, &den);
        check(num == 204 && den == 232, "square NTSC pixels are 204:232");
        chfsession_display_aspect(CHF_MACHINE_VIDEOPLAY, 1, &num, &den);
        check(num == 204 && den == 290, "square PAL pixels are 204:290");
    }
    check(!chfsession_have_bios(s, CHF_MACHINE_CHANNELF) && !chfsession_needs_bios(s)
          && chfsession_missing(s) == 0 && chfsession_bios_in_use(s) == -1, "no BIOS imported, nothing missing yet");

    /* the stand-in always, so the programs below do not depend on a BIOS */
    chfsession_set_int(s, "bios", CHF_BIOS_STANDIN);
    chfsession_default_opts(s, &o);
    o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
    check(chfsession_start(s, &o) == 0, "session starts (CONFIG, no FujiNet)");
    if (!chfsession_is_running(s)) { printf("error: %s\n", chfsession_last_error(s)); return 1; }
    check(chfsession_bios_in_use(s) == -1, "on the stand-in BIOS");

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
    check(chfsession_missing(s) == 0, "CONFIG needs nothing the stand-in lacks");
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

    /* keyboard: the default map, and what the console sees */
    check(chfsession_key(s, CHF_KEYSYM_RIGHT, 1) == 1 && (chfsession_buttons_held(s, 0) & B(CHF_ACT_RIGHT)),
          "the right arrow is Player 1's stick right");
    check(chfsession_key(s, CHF_KEYSYM_UP, 1) == 1 && (chfsession_buttons_held(s, 0) & B(CHF_ACT_FORWARD)),
          "the up arrow is forward: a diagonal is two contacts");
    chfsession_key(s, CHF_KEYSYM_LEFT, 1);
    check((chfsession_buttons_held(s, 0) & (B(CHF_ACT_RIGHT) | B(CHF_ACT_LEFT))) == 0,
          "right and left together: the stick holds neither");
    chfsession_key(s, CHF_KEYSYM_LEFT, 0);
    check((chfsession_buttons_held(s, 0) & B(CHF_ACT_RIGHT)) != 0, "right again once left lets go");
    chfsession_release_all(s);
    check(chfsession_buttons_held(s, 0) == 0, "release_all lets go of the keyboard");
    check(chfsession_key(s, 'z', 1) == 1 && chfsession_key(s, 'c', 1) == 1
          && chfsession_buttons_held(s, 0) == (B(CHF_ACT_TWIST_CCW) | B(CHF_ACT_PULL)),
          "Z twists counter-clockwise and C pulls, together");
    chfsession_key(s, 'x', 1);
    check(chfsession_buttons_held(s, 0) == B(CHF_ACT_PULL),
          "twisting both ways at once holds neither");
    chfsession_release_all(s);
    check(chfsession_key(s, CHF_KEYSYM_SPACE, 1) == 1 && chfsession_buttons_held(s, 0) == B(CHF_ACT_PUSH),
          "Space pushes");
    chfsession_key(s, CHF_KEYSYM_SPACE, 0);
    check(chfsession_key(s, CHF_KEYSYM_KP_6, 1) == 1 && chfsession_buttons_held(s, 1) == B(CHF_ACT_RIGHT)
          && chfsession_buttons_held(s, 0) == 0, "the keypad's 6 is Player 2's stick right, and only Player 2's");
    chfsession_key(s, CHF_KEYSYM_KP_6, 0);
    check(chfsession_key(s, CHF_KEYSYM_KP_9, 1) == 1 && chfsession_buttons_held(s, 1) == B(CHF_ACT_TWIST_CW),
          "the keypad's 9 twists clockwise");
    chfsession_key(s, CHF_KEYSYM_KP_9, 0);
    check(chfsession_buttons_held(s, 1) == 0, "released");
    check(chfsession_key(s, '1', 1) == 1 && chfsession_key_held(s, CHF_KEY_TIME), "1 holds button 1 (TIME)");
    chfsession_key(s, '1', 0);
    check(chfsession_key(s, '4', 1) == 1 && chfsession_key_held(s, CHF_KEY_START) && !chfsession_key_held(s, CHF_KEY_TIME),
          "4 holds START");
    chfsession_key(s, '4', 0);
    check(chfsession_key(s, CHF_KEYSYM_F11, 1) == 0, "an unbound key is ignored");
    check(chfsession_key_sysaction(s, CHF_KEYSYM_ESCAPE) == CHF_SYSACT_RESET_CONFIG,
          "Escape is the Reset to CONFIG system action");
    check(chfsession_target_for_key(s, CHF_KEYSYM_F1) == CHF_TARGET_SWITCH(CHF_SW_RESET), "F1 is the console's RESET");
    check(chfsession_key(s, CHF_KEYSYM_F1, 1) == 1 && chfsession_switch_held(s, CHF_SW_RESET), "holding it holds RESET");
    chfsession_key(s, CHF_KEYSYM_F1, 0);
    check(!chfsession_switch_held(s, CHF_SW_RESET), "and letting go lets go");
    chfsession_press(s, CHF_TARGET_KEY(CHF_KEY_MODE), 1);
    check(chfsession_key_held(s, CHF_KEY_MODE), "the controllers window can press a button directly");
    chfsession_press(s, CHF_TARGET_KEY(CHF_KEY_MODE), 0);

    /* the pointer is a source of its own */
    chfsession_pointer_set(s, 0, B(CHF_ACT_BACK) | B(CHF_ACT_PUSH));
    check(chfsession_buttons_held(s, 0) == (B(CHF_ACT_BACK) | B(CHF_ACT_PUSH)) && chfsession_buttons_held(s, 1) == 0,
          "the pointer holds what the window says, on its controller");
    chfsession_key(s, CHF_KEYSYM_DOWN, 1);
    chfsession_pointer_set(s, 0, 0);
    check(chfsession_buttons_held(s, 0) == B(CHF_ACT_BACK), "letting go of the pointer leaves the key's hold");
    chfsession_key(s, CHF_KEYSYM_DOWN, 0);
    check(chfsession_buttons_held(s, 0) == 0, "and the key leaves nothing");
    chfsession_pointer_set(s, 1, B(CHF_ACT_LEFT));
    check(chfsession_buttons_held(s, 1) == B(CHF_ACT_LEFT), "the left controller is Player 2's (port 1)");
    chfsession_pointer_set(s, 1, 0);

    /* a wheel notch twists for six frames, then lets go (counted in machine
     * frames, not time: a loaded runner catches up frames in a burst) */
    {
        const uint64_t f0 = chfsession_frame_count(s);
        int waited = 0;

        chfsession_twist_pulse(s, 0, 1);
        check(chfsession_buttons_held(s, 0) == B(CHF_ACT_TWIST_CW), "a notch forward twists clockwise");
        while (waited < 5000 && chfsession_buttons_held(s, 0) != 0) { sleep_ms(10); waited += 10; }
        check(chfsession_buttons_held(s, 0) == 0 && chfsession_frame_count(s) - f0 >= 6,
              "and lets go six frames later, on its own");
    }
    chfsession_twist_pulse(s, 1, -1);
    check(chfsession_buttons_held(s, 1) == B(CHF_ACT_TWIST_CCW), "a notch back twists counter-clockwise, on Player 2");
    chfsession_twist_pulse(s, 1, 1);
    check(chfsession_buttons_held(s, 1) == B(CHF_ACT_TWIST_CW), "a notch the other way ends the first");
    for (int waited = 0; waited < 5000 && chfsession_buttons_held(s, 1) != 0; waited += 10)
        sleep_ms(10);
    check(chfsession_buttons_held(s, 1) == 0, "released");
    check(wait_frames(s, &serial, 10, 3000), "the machine keeps running after input");

    /* images the console cannot take */
    check(chfsession_check_cart(big, why, sizeof why) == 0, "a 300,000-byte image is refused");
    check(strstr(why, "larger") != NULL, "with the reason");
    check(chfsession_load_cart(s, big) == -1, "load_cart refuses it");
    check(!chfsession_cart_booted_game(s) && chfsession_cart_path(s)[0] == '\0', "and CONFIG keeps running");
    check(chfsession_load_cart(s, zip2) == -1 && strstr(chfsession_last_error(s), "exactly one") != NULL,
          "a zip of two cartridges is refused, with the reason");

    /* a game needs the console's BIOS: refused on the stand-in, with the reason, CONFIG goes on */
    check(chfsession_check_cart(game, NULL, 0) == 1 && !chfsession_cart_is_app(game),
          "the game plans, and is not an app");
    check(chfsession_load_cart(s, game) == -1, "load_cart refuses the game with only the stand-in");
    check(strstr(chfsession_last_error(s), "BIOS") != NULL && strstr(chfsession_last_error(s), "Import BIOS") != NULL,
          "and says the console needs its BIOS");
    check(chfsession_needs_bios(s) && (chfsession_missing(s) & CHF_MISSING_BIOS), "the session reports the missing BIOS");
    {
        char reason[256];
        check(chfsession_missing_reason(s, reason, sizeof reason) > 0 && strstr(reason, "BIOS") != NULL,
              "with a reason for the banner");
    }
    check(chfsession_cart_path(s)[0] == '\0' && strcmp(chfsession_get_str(s, "cart", ""), "") == 0,
          "nothing was remembered");
    check(wait_status(s, "link down", 3000) && wait_frames(s, &serial, 10, 3000), "and CONFIG keeps running");

    /* a FujiNet app runs on the stand-in with the mailbox live */
    check(chfsession_cart_is_app(app), "the app is an app");
    check(chfsession_load_cart(s, app) == 0, "load_cart opens the app");
    check(strcmp(chfsession_cart_path(s), app) == 0 && strcmp(chfsession_get_str(s, "cart", ""), app) == 0,
          "remembered and persisted");
    check(!chfsession_needs_bios(s), "a power cycle that fixed it cleared the missing BIOS");
    check(wait_booted_game(s, 1, 5000), "something other than CONFIG is in the cartridge");
    check(moving(s, RAM_COUNT), "the app runs (its counter moves)");
    {
        char name[96];
        chfsession_cart_type_name(s, name, sizeof name);
        check(strcmp(name, "FujiNet app") == 0, "the cartridge is a FujiNet app");
    }

    /* the hand controllers and the console's buttons reach the program through the ports */
    check(wait_port(s, RAM_P1, 0xFF, 3000) && wait_port(s, RAM_P2, 0xFF, 3000),
          "at rest both controllers read all ones (active low)");
    chfsession_key(s, CHF_KEYSYM_RIGHT, 1);
    check(wait_port(s, RAM_P1, LOW(B(CHF_ACT_RIGHT)), 3000), "Right on Player 1 clears bit 0 of port 1");
    chfsession_key(s, CHF_KEYSYM_UP, 1);
    chfsession_key(s, CHF_KEYSYM_SPACE, 1);
    check(wait_port(s, RAM_P1, LOW(B(CHF_ACT_RIGHT) | B(CHF_ACT_FORWARD) | B(CHF_ACT_PUSH)), 3000),
          "with forward and push, three bits");
    check(ram(s, RAM_P2) == 0xFF, "and Player 2 is untouched");
    chfsession_release_all(s);
    check(wait_port(s, RAM_P1, 0xFF, 3000), "and it reads as before once let go");
    chfsession_key(s, CHF_KEYSYM_KP_7, 1);
    chfsession_key(s, CHF_KEYSYM_KP_PERIOD, 1);
    check(wait_port(s, RAM_P2, LOW(B(CHF_ACT_TWIST_CCW) | B(CHF_ACT_PULL)), 3000),
          "Player 2's twist and pull are on port 4");
    chfsession_release_all(s);
    chfsession_pointer_set(s, 0, B(CHF_ACT_LEFT) | B(CHF_ACT_TWIST_CW));
    check(wait_port(s, RAM_P1, LOW(B(CHF_ACT_LEFT) | B(CHF_ACT_TWIST_CW)), 3000), "the pointer's hold reaches the port");
    chfsession_pointer_set(s, 0, 0);
    check(wait_port(s, RAM_P1, 0xFF, 3000), "and its letting go");
    {
        /* a wheel notch is held six frames: a program polling once a frame sees it */
        poke(s, RAM_STICKY1, 0xFF);
        chfsession_twist_pulse(s, 0, 1);
        sleep_ms(400);
        check(ram(s, RAM_STICKY1) == LOW(B(CHF_ACT_TWIST_CW)), "a wheel notch reaches the program, and only that bit");
        check(ram(s, RAM_P1) == 0xFF, "and is gone again");
    }
    chfsession_key(s, '2', 1);
    check(wait_panel(s, 0x0F & ~(1u << CHF_KEY_MODE), 3000), "button 2 (MODE) clears bit 1 of port 0");
    chfsession_key(s, '2', 0);
    check(wait_panel(s, 0x0F, 3000), "and not once let go");

    /* RESET: the F8 starts over, the app is still open and its memory stays */
    {
        uint8_t boots = ram(s, RAM_BOOTS);
        check(chfsession_reset(s) == 0, "RESET");
        check(wait_ram(s, RAM_BOOTS, (uint8_t)(boots + 1), 3000), "the app started over");
        check(moving(s, RAM_COUNT), "and runs");
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
        build_app(img);
        check(write_file(abs, img, 0x4000), "wrote an image in the launch directory");
        check(test_chdir(data) == 0, "the working directory moves away");
        check(chfsession_check_cart(rel, NULL, 0) == 1, "a bare name still finds it");
        check(chfsession_load_cart(s, rel) == 0, "and opens it");
        check(strcmp(chfsession_cart_path(s), abs) == 0, "remembered by its full path");
        check(moving(s, RAM_COUNT), "and it runs");
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
        check(o.bios == CHF_BIOS_STANDIN, "and so is the BIOS choice");
        o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
        check(chfsession_start(s, &o) == 0, "restart");
        check(moving(s, RAM_COUNT), "and it boots the remembered cartridge");

        /* the PAL machines: 50 Hz, the same cartridge */
        chfsession_set_int(s, "machine", CHF_MACHINE_VIDEOPLAY);
        check(chfsession_restart(s) == 0, "the PAL console: a power cycle");
        check(chfsession_refresh_rate(s) == 50, "50 Hz");
        serial = 0;
        check(wait_frames(s, &serial, 10, 5000) && moving(s, RAM_COUNT), "it runs the same cartridge");
        chfsession_set_int(s, "machine", CHF_MACHINE_CHANNELF2);
        check(chfsession_restart(s) == 0 && chfsession_refresh_rate(s) == 60, "the System II is NTSC again");
        check(moving(s, RAM_COUNT), "and runs it");
        chfsession_set_int(s, "machine", CHF_MACHINE_CHANNELF);
        check(chfsession_restart(s) == 0 && chfsession_refresh_rate(s) == 60, "and back to the Channel F");

        /* a remembered cartridge that has gone: CONFIG, and forgotten */
        chfsession_stop(s);
        chfsession_set_str(s, "cart", "/nonexistent/gone.bin");
        chfsession_default_opts(s, &o);
        o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
        check(chfsession_start(s, &o) == 0, "a vanished remembered cartridge does not stop the start");
        check(chfsession_cart_path(s)[0] == '\0' && wait_status(s, "link down", 5000), "CONFIG boots instead");

        /* a remembered game whose BIOS is not there: CONFIG too */
        chfsession_stop(s);
        chfsession_set_str(s, "cart", game);
        chfsession_default_opts(s, &o);
        o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
        check(chfsession_start(s, &o) == 0, "a remembered game with no BIOS does not stop the start");
        check(chfsession_cart_path(s)[0] == '\0' && wait_status(s, "link down", 5000), "CONFIG boots instead");
        chfsession_stop(s);
        chfsession_free(s);
    }

    /* ---- with the console's own BIOS (MAME's channelf.zip), when it is here ---- */
    have_bios = mame_zip(bioszip, sizeof bioszip);
    if (!have_bios) {
        printf("SKIP: no MAME channelf.zip at %s -- the game-on-the-real-BIOS checks\n", bioszip);
    } else {
        char msg[512];

        s = chfsession_new(&p);
        check(s != NULL, "a session for the real BIOS");
        if (s) {
            chfsession_set_int(s, "bios", CHF_BIOS_AUTO);
            chfsession_set_str(s, "cart", "");
            chfsession_default_opts(s, &o);
            o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
            check(chfsession_start(s, &o) == 0 && wait_status(s, "link down", 5000), "CONFIG on the stand-in, nothing imported");
            check(chfsession_load_cart(s, game) == -1 && chfsession_needs_bios(s), "the game is refused: no BIOS");
            check(chfsession_rom_import(s, bioszip, msg, sizeof msg) == 3, "import MAME's channelf.zip");
            check(chfsession_have_bios(s, CHF_MACHINE_CHANNELF) && chfsession_have_bios(s, CHF_MACHINE_VIDEOPLAY2),
                  "every machine has a BIOS");
            check(!chfsession_needs_bios(s), "the missing BIOS clears once it is imported");
            check(chfsession_bios_in_use(s) == -1, "the running console is still on the stand-in until the power cycle");
            check(chfsession_load_cart(s, game) == 0, "the game loads");
            check(chfsession_bios_in_use(s) == CHF_ROM_SL31253, "on the Channel F's own first half");
            check(game_running(s, 10000), "the real BIOS hands over to the game");
            {
                char name[96];
                chfsession_cart_type_name(s, name, sizeof name);
                check(strstr(name, "KB") != NULL, "the cartridge is a Videocart with a size");
            }
            check(wait_status(s, "game running", 3000), "the cartridge reports a game (mailbox closed)");
            check(chfsession_missing(s) == 0, "nothing is missing");
            check(chfsession_reset(s) == 0 && game_running(s, 10000), "RESET on a game: it runs again");
            check(chfsession_load_cart(s, zip1) == 0 && strcmp(chfsession_cart_path(s), zip1) == 0
                  && game_running(s, 10000), "the same game from a zip");

            chfsession_set_int(s, "machine", CHF_MACHINE_CHANNELF2);
            check(chfsession_restart(s) == 0 && chfsession_bios_in_use(s) == CHF_ROM_SL90025,
                  "the System II takes sl90025");
            check(game_running(s, 10000), "and runs it");

            chfsession_set_int(s, "bios", CHF_BIOS_SL31253);
            check(chfsession_restart(s) == 0 && chfsession_bios_in_use(s) == CHF_ROM_SL31253,
                  "the BIOS choice overrides the machine's own");

            check(chfsession_reset_to_config(s) == 0 && wait_status(s, "link down", 10000),
                  "CONFIG boots on the real BIOS too");
            check(chfsession_load_cart(s, app) == 0 && moving(s, RAM_COUNT), "and so does a FujiNet app");

            chfsession_set_int(s, "bios", CHF_BIOS_STANDIN);
            check(chfsession_load_cart(s, game) == -1 && strstr(chfsession_last_error(s), "Preferences") != NULL,
                  "choosing the stand-in refuses the game again, and says where to change it");
            chfsession_stop(s);
            chfsession_free(s);
        }
    }

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
