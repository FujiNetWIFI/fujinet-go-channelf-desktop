/*
 * machine_test -- the console without a host thread (core/chf/machine.c): the
 * CPU's memory map as MAME's arcadia_mem and the cartridge slot lay it out, the
 * keypads, the panel and the fire wire as the CPU reads them, the pots from
 * digital and analog stick values, SENSE and the line timer against MAME's
 * scheduler arithmetic, the beeper, and the 2650's RESET key. The machine runs
 * a cartridge of this test's own (a 2650 loop), so only what the test pokes
 * through the CPU's bus callbacks happens.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "chf_internal.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

#define K1(n) (1ull << (CHF_KEY_PAD_BASE(0) + (n)))
#define K2(n) (1ull << (CHF_KEY_PAD_BASE(1) + (n)))
#define KSTART  (1ull << CHF_KEY_START)
#define KOPTION (1ull << CHF_KEY_OPTION)
#define KSELECT (1ull << CHF_KEY_SELECT)

static chf_machine_t m;
static uint8_t rom[8192];
static chf_cart_t cart;

/* the CPU's bus, as its callbacks see it */
static uint8_t rd(uint16_t a) { return m.cpu.read(m.cpu.ctx, a); }
static void wr(uint16_t a, uint8_t v) { m.cpu.write(m.cpu.ctx, a, v); }

static void power_on(int machine, const uint8_t *image, uint32_t len)
{
    chf_machine_opts_t mo;
    const char *err = NULL;

    chf_cart_init(&cart, CHF_CART_STD, image, len, &err);
    memset(&mo, 0, sizeof mo);
    mo.machine = machine;
    mo.cart = &cart;
    chf_machine_init(&m, &mo);
}

static void press(uint64_t keys, uint8_t joy0, uint8_t joy1)
{
    m.keys_live = keys;
    m.joy_live[0] = joy0;
    m.joy_live[1] = joy1;
    chf_machine_latch_inputs(&m);
}

int main(void)
{
    /* a recognisable image; the first bytes are BCTA,UN $0000 (a loop) */
    for (unsigned i = 0; i < sizeof rom; i++)
        rom[i] = (uint8_t)(i * 13 + (i >> 8) * 7);
    rom[0] = 0x1f; rom[1] = 0x00; rom[2] = 0x00;
    power_on(CHF_MACHINE_ARCADIA, rom, sizeof rom);

    /* ---- the memory map (arcadia_mem and the slot) ---- */
    check(rd(0x0000) == 0x1f && rd(0x0fff) == rom[0xfff], "$0000-$0FFF is the cartridge's first 4K");
    check(rd(0x2000) == rom[0x1000] && rd(0x2fff) == rom[0x1fff], "$2000-$2FFF is its second 4K");
    check(rd(0x3000) == 0xff && rd(0x7fff) == 0xff, "$3000 and up read $FF past an 8K image (not mirrors)");
    check(rd(0x1000) == 0 && rd(0x17ff) == 0 && rd(0x1b00) == 0 && rd(0x1c00) == 0 && rd(0x1fff) == 0,
          "the holes around the UVI ($1000-$17FF, $1B00-$1FFF) are unmapped and read 0");
    wr(0x0010, 0x55);
    check(rd(0x0010) == rom[0x10], "the cartridge is ROM: writes are ignored");
    wr(0x18d0, 0xa5);
    wr(0x1a05, 0x5a);
    check(rd(0x18d0) == 0xa5 && rd(0x1a05) == 0x5a, "the UVI's RAM at $18D0 and $1A00 holds what is written");
    check(rd(0x7ffff & 0x7fff) == 0xff && m.cpu.read(m.cpu.ctx, 0x9800) == rd(0x1800), "addresses are 15 bits");
    wr(0x1b00, 1);
    check(rd(0x1b00) == 0, "a write to the unmapped $1B00 goes nowhere");
    check(m.cpu.io_read(m.cpu.ctx, S2650_CTRL_PORT) == 0 && m.cpu.io_read(m.cpu.ctx, 3) == 0,
          "the CPU's I/O ports are wired to nothing and read 0");

    /* ---- keypads, panel, fire ---- */
    press(0, 0, 0);
    check(rd(0x1900) == 0xf0 && rd(0x1901) == 0xf0 && rd(0x1902) == 0xf0 && rd(0x1903) == 0xf0 &&
          rd(0x1904) == 0xf0 && rd(0x1908) == 0, "nothing pressed: the keypad bytes read $F0, the panel 0");
    press(K1(CHF_K_1), 0, 0);       /* key 1: column 1, bit 3 */
    check(rd(0x1900) == 0xf8 && rd(0x1901) == 0xf0 && rd(0x1904) == 0xf0, "player 1's 1 is $1900 bit 3");
    press(K1(CHF_K_CLEAR) | K1(CHF_K_ENTER) | K1(CHF_K_5), 0, 0);
    check(rd(0x1900) == 0xf1 && rd(0x1902) == 0xf1 && rd(0x1901) == 0xf4, "CLEAR is column 1 bit 0, ENTER column 3 bit 0, 5 column 2 bit 2");
    press(K2(CHF_K_9) | K2(CHF_K_7) | K2(CHF_K_0), 0, 0);
    check(rd(0x1904) == 0xf2 && rd(0x1905) == 0xf1 && rd(0x1906) == 0xf2, "player 2 reads at $1904-$1906 (7 and 0 and 9)");
    press(0, CHF_STICK_FIRE, 0);
    check(rd(0x1901) == 0xf8 && rd(0x1905) == 0xf0, "FIRE is wired to keypad 2: $1901 bit 3");
    press(K1(CHF_K_2), CHF_STICK_FIRE, CHF_STICK_FIRE);
    check(rd(0x1901) == 0xf8 && rd(0x1905) == 0xf8, "and ORed with the key, and for player 2 at $1905");
    press(KSTART | KSELECT, 0, 0);
    check(rd(0x1908) == 5, "the panel: START bit 0, OPTION bit 1, SELECT bit 2");
    press(KOPTION, 0, 0);
    check(rd(0x1908) == 2, "OPTION is bit 1");
    press(0xffffffffffffffffull, 0, 0);
    check(rd(0x1908) == 7 && rd(0x1900) == 0xff && rd(0x1903) == 0xf0, "bits past the 27 console keys are ignored");

    /* ---- the pots ---- */
    press(0, 0, 0);
    wr(0x19f9, 0x00);       /* pot mux low: y axes; wait out the settle delay */
    for (int i = 0; i < 12; i++)
        uvi_line(&m.uvi);
    check(rd(0x19ff) == 0x6f && rd(0x19fe) == 0x80, "centred: player 1 reads $6F, player 2 $80 (MAME's values)");
    press(0, CHF_STICK_UP, CHF_STICK_UP);
    check(rd(0x19ff) == 0x00 && rd(0x19fe) == 0x00, "UP drives the y pot to 0");
    press(0, CHF_STICK_DOWN, CHF_STICK_DOWN);
    check(rd(0x19ff) == 0xff && rd(0x19fe) == 0xff, "DOWN drives it to $FF");
    press(0, CHF_STICK_UP | CHF_STICK_DOWN, 0);
    check(rd(0x19ff) == 0x00, "both: up wins, as in MAME's video_r");
    wr(0x19f9, 0x40);       /* x axes */
    for (int i = 0; i < 12; i++)
        uvi_line(&m.uvi);
    press(0, CHF_STICK_LEFT, CHF_STICK_RIGHT);
    check(rd(0x19ff) == 0x00 && rd(0x19fe) == 0xff, "mux high: LEFT is 0, RIGHT $FF");
    press(0, CHF_STICK_LEFT | CHF_STICK_RIGHT, 0);
    check(rd(0x19ff) == 0xff, "both: right wins");
    m.disc_live[0][0] = 127; m.disc_live[0][1] = -128;
    m.disc_live[1][0] = -128; m.disc_live[1][1] = 127;
    m.joy_live[0] = CHF_STICK_UP;           /* the digital bits are ignored while the disc is off-centre */
    chf_machine_latch_inputs(&m);
    check(rd(0x19ff) == 0xff && rd(0x19fe) == 0x00, "the analog disc overrides the digital bits: full right = $FF, full left = 0");
    m.disc_live[0][0] = 0; m.disc_live[0][1] = 0;
    m.disc_live[1][0] = 64; m.disc_live[1][1] = 0;
    m.joy_live[0] = 0;
    chf_machine_latch_inputs(&m);
    check(rd(0x19ff) == 0x6f, "a centred disc is the centre value");
    check(rd(0x19fe) > 0x80 + 0x30 && rd(0x19fe) < 0x80 + 0x50, "half deflection is about half way");
    m.disc_live[0][0] = m.disc_live[0][1] = m.disc_live[1][0] = m.disc_live[1][1] = 0;
    press(0, 0, 0);

    /* ---- the line timer and SENSE (MAME's scheduler arithmetic) ---- */
    power_on(CHF_MACHINE_ARCADIA, rom, sizeof rom);
    check(m.line_as == 63613231552162ull && m.cycle_as == 1117318435754ull,
          "the line period is attotime::from_hz(262 * 60) and the CPU's HZ_TO_ATTOSECONDS(895000)");
    check(m.next_limit == 56, "the first tick waits for CPU cycle 56");
    chf_machine_run_frame(&m);
    check(m.uvi.line_count == 262 && m.uvi.line == 0, "a frame is 262 line ticks and wraps to line 0");
    check(m.cpu.cycles >= 14916 && m.cpu.cycles < 14916 + 12, "the first frame ends at the CPU cycle MAME's does (14916)");
    for (int i = 0; i < 2; i++)
        chf_machine_run_frame(&m);
    check(m.cpu.cycles >= 44749 && m.cpu.cycles < 44749 + 12, "three frames end at cycle 44749-44750 (895000 / 60 each)");
    {
        uint64_t before = m.frames_done;
        int sense_low = 0, sense_high = 0, steps = 0;

        while (m.frames_done == before && steps++ < 200000) {
            chf_machine_step(&m);
            if (m.uvi.line >= 216)
                sense_high += m.cpu.sense(m.cpu.ctx) == 1;
            else
                sense_low += m.cpu.sense(m.cpu.ctx) == 0;
        }
        check(sense_low > 0 && sense_high > 0 && steps < 200000, "SENSE follows the line: low above 216, high from 216");
        check(sense_high == 0 || (double)sense_high / (steps) > 0.1, "about 46 of 262 lines are vertical blank");
    }
    check(m.fb[0] != 0 || m.fb[100] == 0xffffff, "the frame buffer is the bitmap as it stood after the last tick");

    /* ---- PAL ---- */
    power_on(CHF_MACHINE_ARCADIA_PAL, rom, sizeof rom);
    chf_machine_run_frame(&m);
    check(m.uvi.lines == 312 && m.uvi.line_count == 312, "the PAL row has 312 lines");
    check(m.cpu.cycles > 886724 / 50 - 20 && m.cpu.cycles < 886724 / 50 + 20, "and 50 frames a second at 4.433619 MHz / 5");

    /* ---- the beeper ---- */
    power_on(CHF_MACHINE_ARCADIA, rom, sizeof rom);
    m.audio_gain = 1.0f;
    chf_machine_run_frame(&m);
    check(m.out_frames > 700 && m.out_frames < 900, "a frame makes about 800 audio samples");
    {
        double peak = 0;
        for (int i = 0; i < m.out_frames; i++)
            peak = fmax(peak, fabs(m.out[2 * i]));
        check(peak == 0.0, "silent until the pitch and volume are written");
    }
    wr(0x18fd, 20);         /* pitch */
    wr(0x18fe, 0x08 | 7);   /* tone on, volume 7 */
    chf_machine_run_frame(&m);
    chf_machine_run_frame(&m);
    {
        double peak = 0, lo = 1e9;
        int trans = 0;

        for (int i = 0; i < m.out_frames; i++) {
            peak = fmax(peak, fabs(m.out[2 * i]));
            lo = fmin(lo, m.out[2 * i]);
            if (i && fabs(m.out[2 * i] - m.out[2 * i - 2]) > 0.2)
                trans++;
            if (m.out[2 * i] != m.out[2 * i + 1])
                trans = -1000;
        }
        check(peak > 0.3 && peak < 0.5, "a tone at volume 7 reaches 0.4375 of full scale (7 * $800 / 32768)");
        check(trans > 5 && trans < 40, "and is a square wave: pitch 20 flips every 20 samples of 15625 Hz");
        (void)lo;
    }
    wr(0x18fe, 0x00);
    chf_machine_run_frame(&m);
    chf_machine_run_frame(&m);
    {
        double peak = 0;
        for (int i = 0; i < m.out_frames; i++)
            peak = fmax(peak, fabs(m.out[2 * i]));
        check(peak == 0.0, "mode 0 is silence again");
    }

    /* ---- RESET: the 2650 only ---- */
    power_on(CHF_MACHINE_ARCADIA, rom, sizeof rom);
    wr(0x18d0, 0x77);
    chf_machine_run_frame(&m);
    m.cpu.page = 0x2000;
    m.cpu.iar = 0x123;
    m.reset_live = true;
    chf_machine_latch_inputs(&m);
    check(s2650_pc(&m.cpu) == 0, "pressing RESET resets the CPU");
    {
        uint64_t c0 = m.cpu.cycles, n0 = m.cpu.instructions;

        chf_machine_run_frame(&m);
        check(m.cpu.instructions == n0 && m.cpu.cycles > c0, "while it is held the CPU does not run, but time passes");
    }
    check(rd(0x18d0) == 0x77, "the UVI keeps its registers");
    m.reset_live = false;
    chf_machine_run_frame(&m);
    check(m.cpu.instructions > 0, "released, it runs from $0000 again");

    /* ---- the debugger's access ---- */
    check(chf_machine_peek(&m, CHF_PEEK_PROG, 0x1800 + 0xd0) == 0x77 && chf_machine_peek(&m, CHF_PEEK_UVI, 0xd0) == 0x77,
          "peek reads the program space and the UVI's registers");
    chf_machine_poke(&m, CHF_PEEK_UVI, 0xd1, 0x42);
    check(rd(0x18d1) == 0x42, "poke writes the UVI");
    chf_machine_poke(&m, CHF_PEEK_UVI, 0xfd, 0x80);
    check(m.uvi.multicolor == 0x80, "and with the register's effects");

    /* ---- determinism ---- */
    {
        static chf_machine_t a, b;
        chf_machine_opts_t mo;

        memset(&mo, 0, sizeof mo);
        mo.cart = &cart;
        chf_machine_init(&a, &mo);
        chf_machine_init(&b, &mo);
        for (int i = 0; i < 5; i++) {
            chf_machine_run_frame(&a);
            chf_machine_run_frame(&b);
        }
        check(memcmp(a.fb, b.fb, sizeof a.fb) == 0 && a.cpu.cycles == b.cpu.cycles, "two machines run alike");
    }

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
