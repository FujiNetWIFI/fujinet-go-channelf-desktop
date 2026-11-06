/*
 * machine_test -- the whole console, with no host thread and no ROM: the memory
 * map, the four ports and their latches, the pads, the VRAM strobe, the vertical
 * blank's exact timing, the tone, RESET, the stand-in's rule, the FujiNet
 * cartridge's tick, and the debugger's spaces and bookkeeping.
 *
 * The programs are F8 machine code written out by hand in test_chf.h's style, on
 * the tests' own BIOS (a jump to the cartridge's entry); the expected values are
 * worked out from MAME's driver (channelf.cpp) and its screen's timing, not read
 * back from the code under test. The real BIOS and MAME are the A/B tools' business
 * (tools/ab/).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "test_chf.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        failures++;
}

static test_rig rig;
static uint8_t img[0x40000];

static void boot(int machine, int type, const uint8_t *code, size_t n, uint32_t size)
{
    test_cart(img, size, code, n, 0);
    if (test_rig_init(&rig, machine, type, img, size) != 0) {
        printf("FAIL: cannot build the rig\n");
        exit(1);
    }
}

static void frames(int n)
{
    for (int i = 0; i < n; i++)
        chf_machine_run_frame(&rig.m);
}

/* ---- the clocks: MAME's integer attoseconds ---- */
static void timing(void)
{
    static const uint8_t spin[] = { T_SPIN };

    boot(CHF_MACHINE_CHANNELF, CHF_CART_STD, spin, sizeof spin, 0x800);
    check(rig.m.cycle_as == 558730385769ull, "NTSC: a CPU clock is 1e18 / 1789772 attoseconds");
    check(rig.m.frame_as == 16520537738766336ull,
          "NTSC: the frame is 264 lines of 256 pixels at 4090908 Hz (1e18/px truncated, as MAME)");
    check(rig.m.lines == 264 && rig.m.vbstart == 248 && rig.m.y_rpt == 4, "NTSC: 264 lines, blank at 248, rows 4 lines tall");
    frames(600);
    {
        const uint64_t due = (uint64_t)(((unsigned __int128)600 * rig.m.frame_as) / rig.m.cycle_as);

        check(rig.m.frames_done == 600, "600 frames fired");
        check(rig.m.cpu.cycles >= due && rig.m.cpu.cycles < due + 24,
              "the 600th blank fires on the first instruction boundary at or after floor(600 * frame / clock)");
        printf("  NTSC: %.6f clocks a frame, %.5f Hz\n", (double)rig.m.cpu.cycles / 600.0, chf_machine_frame_hz(&rig.m));
        check(chf_machine_frame_hz(&rig.m) > 60.5307 && chf_machine_frame_hz(&rig.m) < 60.5308, "60.5307 Hz");
    }
    boot(CHF_MACHINE_VIDEOPLAY, CHF_CART_STD, spin, sizeof spin, 0x800);
    check(rig.m.cycle_as == 500000000000ull && rig.m.frame_as == 19968000000000000ull,
          "PAL: 2 MHz CPU, 4 MHz pixel clock, 312 lines: exactly 39936 clocks a frame");
    frames(100);
    check(rig.m.cpu.cycles >= 100 * 39936ull && rig.m.cpu.cycles < 100 * 39936ull + 24, "PAL: 100 frames of 39936 clocks");
    check(rig.m.lines == 312 && rig.m.vbstart == 310 && rig.m.y_rpt == 5 && chf_machine_is_pal(CHF_MACHINE_VIDEOPLAY2),
          "PAL: 312 lines, blank at 310, rows 5 lines tall");
    check(chf_machine_name(CHF_MACHINE_CHANNELF2) != NULL && !chf_machine_is_pal(CHF_MACHINE_CHANNELF2),
          "the System II is NTSC and has a name");
}

/* ---- the ports and the VRAM strobe ---- */
static void ports(void)
{
    /* value 2 at row 10, column 20, the BIOS's own sequence: colour, column, row, strobe $60 then $50 */
    static const uint8_t pixel[] = { 0x20, 0x7f, 0xb1, 0x20, 0xeb, 0xb4, 0x20, 0x35, 0xb5, 0x20, 0x60, 0xb0,
                                     0x20, 0x50, 0xb0, T_SPIN };
    /* the same without a strobe: bit 5 never set */
    static const uint8_t nostrobe[] = { 0x20, 0x7f, 0xb1, 0x20, 0xeb, 0xb4, 0x20, 0x35, 0xb5, 0x20, 0x50, 0xb0,
                                        0x20, 0xdf, 0xb0, 0x20, 0x00, 0xb0, T_SPIN };
    /* a held strobe re-stores: $20, then another colour with bit 5 still the last thing written, then port 0 again */
    static const uint8_t level[] = { 0x20, 0x7f, 0xb1, 0x20, 0xeb, 0xb4, 0x20, 0x35, 0xb5, 0x20, 0x20, 0xb0,
                                     0x20, 0x3f, 0xb1, 0x20, 0x20, 0xb0, T_SPIN };
    const uint8_t *vram;

    boot(CHF_MACHINE_CHANNELF, CHF_CART_STD, pixel, sizeof pixel, 0x800);
    vram = rig.m.vid.vram;
    frames(1);
    check(vram[10 * 128 + 20] == 2, "the strobe stores colour 2 at row 10, column 20");
    {
        unsigned n = 0;

        for (int i = 0; i < CF_VRAM_SIZE; i++)
            n += vram[i] != 0;
        check(n == 1, "and nothing else");
    }
    check(rig.m.vid.latch[0] == 0x50 && rig.m.vid.latch[1] == 0x7f && rig.m.vid.latch[2] == 0xeb && rig.m.vid.latch[3] == 0x35,
          "the latches hold what the CPU wrote to ports 0, 1, 4 and 5");
    check(rig.m.vid.val_reg == 2 && rig.m.vid.col_reg == 20 && rig.m.vid.row_reg == 10,
          "the registers are the complements: value (~p1 >> 6) & 3, column ~p4 & $7F, row ~p5 & $3F");
    check(chf_machine_peek(&rig.m, CHF_PEEK_VRAM, 10 * 128 + 20) == 2, "the debugger reads it");

    boot(CHF_MACHINE_CHANNELF, CHF_CART_STD, nostrobe, sizeof nostrobe, 0x800);
    frames(1);
    {
        unsigned n = 0;

        for (int i = 0; i < CF_VRAM_SIZE; i++)
            n += rig.m.vid.vram[i] != 0;
        check(n == 0, "port 0 writes without bit 5 store nothing ($50, $DF and $00)");
    }
    boot(CHF_MACHINE_CHANNELF, CHF_CART_STD, level, sizeof level, 0x800);
    frames(1);
    check(rig.m.vid.vram[10 * 128 + 20] == 3,
          "bit 5 alone is a strobe (level-triggered): a second port 0 write stores the new colour (~$3F >> 6 = 3) again");
}

/* ---- the panel and the hand controllers ---- */
static void pads(void)
{
    /* INS 0, INS 1, INS 4 into the RAM at $2800 (a chess cart), then the controller gate on (port 0 bit 6) and INS 1, INS 4 again */
    static const uint8_t read[] = { 0x2a, 0x28, 0x00, 0xa0, 0x17, 0xa1, 0x17, 0xa4, 0x17, 0x20, 0x40, 0xb0,
                                    0xa1, 0x17, 0xa4, 0x17, T_SPIN };

    boot(CHF_MACHINE_CHANNELF, CHF_CART_CHESS, read, sizeof read, 0x800);
    rig.m.panel_live = 0x05;                 /* TIME and HOLD held: port 0 bits 0 and 2 */
    rig.m.ctl_live[0] = 0x81;                /* player 1 = the right controller, port 1: right and push */
    rig.m.ctl_live[1] = 0x42;                /* player 2 = the left controller, port 4: left and pull */
    frames(2);
    check(chf_machine_peek(&rig.m, CHF_PEEK_PROG, 0x2800) == 0x0a,
          "port 0 reads the console buttons complemented, bits 7-4 clear (~$F5 & $FF -> $0A)");
    check(chf_machine_peek(&rig.m, CHF_PEEK_PROG, 0x2801) == 0x7e, "port 1 reads the RIGHT controller, active low ($7E)");
    check(chf_machine_peek(&rig.m, CHF_PEEK_PROG, 0x2802) == 0xbd, "port 4 reads the LEFT controller, active low ($BD)");
    check(chf_machine_peek(&rig.m, CHF_PEEK_PROG, 0x2803) == 0x3e,
          "with port 0 bit 6 set port 1 loses bits 7-6 ($3E: the contacts below bit 6 still read)");
    check(chf_machine_peek(&rig.m, CHF_PEEK_PROG, 0x2804) == 0x00, "and port 4 reads nothing at all ($00)");

    /* inputs are latched at the frame boundary: a change mid-stream is seen from the next frame */
    boot(CHF_MACHINE_CHANNELF, CHF_CART_CHESS, read, sizeof read, 0x800);
    frames(1);
    rig.m.ctl_live[0] = 0xff;
    /* the program left port 0 at $40 (the gate set), so port 1 reads ~($C0 | contacts) */
    check(chf_machine_port_read(&rig.m, 1) == 0x3f, "a live change does not reach the port before the frame boundary");
    chf_machine_latch_inputs(&rig.m);
    check(chf_machine_port_read(&rig.m, 1) == 0x00, "and does at it");
    check(chf_machine_port_read(&rig.m, 2) == 0 && chf_machine_port_read(&rig.m, 3) == 0 && chf_machine_port_read(&rig.m, 7) == 0,
          "ports nothing answers read 0");
}

/* ---- the memory map ---- */
static void memory(void)
{
    static const uint8_t spin[] = { T_SPIN };
    /* store $A5 at $0900 (ROM: nothing), $2800 (RAM on a chess cart) and $3000 */
    static const uint8_t st[] = { 0x2a, 0x09, 0x00, 0x20, 0xa5, 0x17, 0x2a, 0x28, 0x00, 0x20, 0xa5, 0x17,
                                  0x2a, 0x30, 0x00, 0x20, 0xa5, 0x17, T_SPIN };

    boot(CHF_MACHINE_CHANNELF, CHF_CART_STD, spin, sizeof spin, 0x800);
    check(chf_machine_peek(&rig.m, CHF_PEEK_PROG, 0x0000) == 0x29 && chf_machine_peek(&rig.m, CHF_PEEK_PROG, 0x0802) == 0x90,
          "$0000 is the BIOS, $0800 the cartridge");
    check(chf_machine_peek(&rig.m, CHF_PEEK_PROG, 0x1000) == 0xff && chf_machine_peek(&rig.m, CHF_PEEK_PROG, 0x2800) == 0xff &&
              chf_machine_peek(&rig.m, CHF_PEEK_PROG, 0xffff) == 0xff,
          "a 2K standard cart reads $FF past its ROM, and has no RAM at $2800");
    boot(CHF_MACHINE_CHANNELF, CHF_CART_CHESS, st, sizeof st, 0x800);
    check(chf_machine_peek(&rig.m, CHF_PEEK_PROG, 0x2800) == 0x00, "a chess cart's RAM reads 0 until written");
    frames(1);
    check(chf_machine_peek(&rig.m, CHF_PEEK_PROG, 0x0900) == 0xff, "a store to the BIOS/ROM area changes nothing");
    check(chf_machine_peek(&rig.m, CHF_PEEK_PROG, 0x2800) == 0xa5, "a store at $2800 lands in a chess cart's RAM");
    check(chf_machine_peek(&rig.m, CHF_PEEK_PROG, 0x3000) == 0xff, "and one at $3000 does not");
    chf_machine_poke(&rig.m, CHF_PEEK_PROG, 0x2801, 0x11);
    chf_machine_poke(&rig.m, CHF_PEEK_PROG, 0x0003, 0x77);
    check(chf_machine_peek(&rig.m, CHF_PEEK_PROG, 0x2801) == 0x11 && chf_machine_peek(&rig.m, CHF_PEEK_PROG, 0x0003) != 0x77,
          "the debugger's poke writes the RAM, never the BIOS");
    check(chf_machine_space_size(&rig.m, CHF_PEEK_PROG) == 0x10000 && chf_machine_space_size(&rig.m, CHF_PEEK_SCRATCH) == 64 &&
              chf_machine_space_size(&rig.m, CHF_PEEK_VRAM) == 8192 && chf_machine_space_size(&rig.m, CHF_PEEK_IO) == 256 &&
              chf_machine_space_size(&rig.m, CHF_PEEK_CART) == 0x800 + 0x800,
          "the five spaces' sizes (a chess cart's CART space is its ROM and its 2K of RAM)");
    chf_machine_poke(&rig.m, CHF_PEEK_SCRATCH, 5, 0x5a);
    check(rig.m.cpu.r[5] == 0x5a && chf_machine_peek(&rig.m, CHF_PEEK_SCRATCH, 5) == 0x5a, "SCRATCH is the register file");
    chf_machine_poke(&rig.m, CHF_PEEK_CART, 0x800 + 3, 0x33);
    check(chf_machine_peek(&rig.m, CHF_PEEK_PROG, 0x2803) == 0x33 && chf_machine_peek(&rig.m, CHF_PEEK_CART, 0x800 + 3) == 0x33,
          "CART space puts the RAM after the ROM image");
}

/* ---- the tone ---- */
static void tone(void)
{
    /* port 5 = $40 | row bits: mode 1; later $C0: mode 3; the mode is data >> 6 */
    static const uint8_t t1[] = { 0x20, 0x7f, 0xb5, T_SPIN };
    static const uint8_t t3[] = { 0x20, 0xff, 0xb5, T_SPIN };

    boot(CHF_MACHINE_CHANNELF, CHF_CART_STD, t1, sizeof t1, 0x800);
    frames(2);
    check(rig.m.snd.mode == 1 && rig.m.snd.changes == 1, "port 5 bits 7-6 = 1 selects the 1 kHz tone");
    check(rig.m.audio_samples >= 1500 && rig.m.audio_samples <= 1600, "two frames made ~1586 samples at 48 kHz");
    boot(CHF_MACHINE_CHANNELF, CHF_CART_STD, t3, sizeof t3, 0x800);
    frames(1);
    check(rig.m.snd.mode == 3, "and 3 the low tone");
    {
        float peak = 0;

        for (int i = 0; i < rig.m.out_frames; i++)
            if (rig.m.out[2 * i] > peak)
                peak = rig.m.out[2 * i];
        check(peak > 0.9f && rig.m.out[0] == rig.m.out[1], "the frame's audio is full scale mono, mirrored to both channels");
    }
    boot(CHF_MACHINE_VIDEOPLAY2, CHF_CART_STD, t1, sizeof t1, 0x800);
    check(rig.m.audio_gain == 0.5f, "the Videoplay 2 group's speaker gain is 0.5");
    boot(CHF_MACHINE_CHANNELF, CHF_CART_STD, t1, sizeof t1, 0x800);
    check(rig.m.audio_gain == 1.0f, "every other machine's is 1");
}

/* ---- RESET ---- */
static void reset(void)
{
    /* r2 = r2 + 1, then spin: each run of the cartridge adds one */
    static const uint8_t inc[] = { 0x42, 0x24, 0x01, 0x52, T_SPIN };
    static const uint8_t pix[] = { 0x20, 0x7f, 0xb1, 0x20, 0xeb, 0xb4, 0x20, 0x35, 0xb5, 0x20, 0x60, 0xb0,
                                   0x42, 0x24, 0x01, 0x52, T_SPIN };

    boot(CHF_MACHINE_CHANNELF, CHF_CART_STD, inc, sizeof inc, 0x800);
    frames(2);
    check(rig.m.cpu.r[2] == 1, "the program ran once");
    rig.m.reset_live = true;
    frames(1);
    check(rig.m.reset_held, "pressing RESET holds the CPU");
    {
        const uint64_t c = rig.m.cpu.cycles;
        const uint64_t i = rig.m.cpu.instructions;

        frames(1);
        check(rig.m.cpu.cycles > c && rig.m.cpu.instructions == i, "time passes, the CPU does not run");
    }
    rig.m.reset_live = false;
    frames(2);
    check(rig.m.cpu.r[2] == 2, "releasing it runs the BIOS again and the cartridge again; the scratchpad was NOT cleared");

    boot(CHF_MACHINE_CHANNELF, CHF_CART_CHESS, pix, sizeof pix, 0x800);
    frames(1);
    chf_machine_poke(&rig.m, CHF_PEEK_PROG, 0x2900, 0x42);
    chf_machine_reset(&rig.m);
    check(rig.m.cpu.pc0 == 1 && f8_pc(&rig.m.cpu) == 0, "chf_machine_reset: PC back at $0000 (ROMC 08)");
    check(rig.m.vid.vram[10 * 128 + 20] == 2 && rig.m.vid.latch[3] == 0x35, "the VRAM and the port latches survive");
    check(chf_machine_peek(&rig.m, CHF_PEEK_PROG, 0x2900) == 0x42, "and a cartridge's RAM");
}

/* ---- the stand-in's rule ---- */
static void standin(void)
{
    static const uint8_t spin[] = { T_SPIN };
    chf_machine_opts_t mo;
    chf_cart_t cart;
    static chf_machine_t m;
    char why[100];

    test_cart(img, 0x800, spin, sizeof spin, 0);
    chf_cart_init(&cart, CHF_CART_STD, img, 0x800, NULL);
    memset(&mo, 0, sizeof mo);
    mo.machine = CHF_MACHINE_CHANNELF;
    mo.cart = &cart;
    mo.bios_id = -1;
    chf_machine_init(&m, &mo);
    check(!m.have_bios && m.bios_id == -1, "no BIOS halves: the app's stand-in");
    check(chf_machine_missing(&m, why, sizeof why) == CHF_MISSING_BIOS && why[0] != '\0',
          "a Videocart under the stand-in parks the console and says why");
    chf_machine_run_frame(&m);
    check(m.cpu.instructions == 0 && m.frames_done == 1, "parked: no instruction runs, the frames still do");
    if (chf_stub_rom[0] == 0xff) {
        /* the placeholder (no stub.c yet) is all illegal opcodes: a FujiNet app parks at $0000 with the BIOS call named */
        static uint8_t app[16384];
        chf_fujinet_t *fj = chf_fujinet_create();

        test_counter_image(app, 1);
        chf_fujinet_power_on(fj, app, sizeof app, "", true, why, sizeof why);
        mo.cart = chf_fujinet_cart(fj);
        mo.fujinet = fj;
        chf_machine_init(&m, &mo);
        chf_machine_check_cart(&m);
        chf_machine_run_frame(&m);
        check(chf_machine_missing(&m, why, sizeof why) == CHF_MISSING_BIOS && strstr(why, "$0000"),
              "an illegal opcode executed below $0800 under the stand-in parks it and names the address");
        chf_fujinet_destroy(fj);
    }
    check(chf_machine_peek(&m, CHF_PEEK_PROG, 0x0000) == chf_stub_rom[0], "and $0000 is the stand-in's image");
    printf("  (%s)\n", why);
}

/* ---- the debugger's views ---- */
static unsigned hooked_mem, hooked_io, hooked_vram, hooked_instr;
static void on_bus(chf_machine_t *m, void *u, int kind, uint16_t a, uint8_t d)
{
    (void)m;
    (void)u;
    (void)a;
    (void)d;
    hooked_mem += kind == CHF_BUS_MEMW || kind == CHF_BUS_MEMR;
    hooked_io += kind == CHF_BUS_IOW || kind == CHF_BUS_IOR;
    hooked_vram += kind == CHF_BUS_VRAM;
}
static void on_instr(chf_machine_t *m, void *u)
{
    (void)m;
    (void)u;
    hooked_instr++;
}

static void debugger(void)
{
    /* plot (row 10, column 20, colour 2) and (row 11, column 21, colour 1), reading a byte and writing one on the way */
    static const uint8_t prog[] = {
        0x20, 0x7f, 0xb1, 0x20, 0xeb, 0xb4, 0x20, 0x35, 0xb5, 0x20, 0x60, 0xb0, 0x20, 0x50, 0xb0,
        0x20, 0xbf, 0xb1, 0x20, 0xea, 0xb4, 0x20, 0x34, 0xb5, 0x20, 0x60, 0xb0, 0x20, 0x50, 0xb0,
        0x2a, 0x28, 0x10, 0x16, 0x17, T_SPIN };
    chf_vwrite_t w[8];
    int n;

    boot(CHF_MACHINE_CHANNELF, CHF_CART_CHESS, prog, sizeof prog, 0x800);
    check(chf_machine_track(&rig.m, true) == 0 && rig.m.track != NULL, "the video bookkeeping starts on demand");
    rig.m.bus_hook = on_bus;
    rig.m.watch = CHF_WATCH_MEMR | CHF_WATCH_MEMW | CHF_WATCH_IOR | CHF_WATCH_IOW | CHF_WATCH_VRAM;
    rig.m.instr_hook = on_instr;
    frames(1);
    check(hooked_vram == 2, "the bus hook told of both VRAM stores");
    check(hooked_io == 10 && hooked_mem == 2, "and of the ten port writes and the one read and one write");
    check(hooked_instr > 20, "the instruction hook ran before each instruction");
    n = chf_machine_vwrites(&rig.m, w, 8);
    check(n == 2 && w[0].row == 10 && w[0].col == 20 && w[0].value == 2 && w[0].old == 0 && w[1].row == 11 && w[1].col == 21 &&
              w[1].value == 1,
          "the write log has both strobes, oldest first, with row, column, value and the old value");
    check(w[0].pc >= 0x0800 && w[1].cycle > w[0].cycle && w[1].frame == 0, "with the PC of the OUT, the clock and the frame");
    check(rig.m.track->cell[10 * 128 + 20].writes == 1 && rig.m.track->cell[10 * 128 + 20].pc == w[0].pc &&
              rig.m.track->heat[11 * 128 + 21] == 0 && rig.m.track->heat_last[11 * 128 + 21] == 1,
          "per cell: write count and last writer; the heat of the frame just ended moved to heat_last");
    check(rig.m.track->strobes_last == 2 && rig.m.track->changes_last == 2, "two strobes, both changed a cell");
    check(rig.m.track->beam_rows == 0 && rig.m.track->frame_vram[10 * 128 + 20] == 2,
          "at the blank the beam starts over, and frame_vram is what the display shows");
    {
        int line, cyc;

        chf_machine_beam(&rig.m, &line, &cyc);
        check(line >= 248 && line < 264 && cyc >= 0 && cyc < 130, "the beam is just past the vertical blank (line 248 on)");
    }
    chf_machine_poke(&rig.m, CHF_PEEK_IO, 0, 0x60);          /* a debugger strobe draws the registers' cell */
    check(rig.m.vid.vram[11 * 128 + 21] == 1, "poking port 0 strobes like the CPU");
    chf_machine_poke(&rig.m, CHF_PEEK_IO, 5, 0x40 | 0x3f);
    check(chf_machine_sound_mode(&rig.m) == 1, "poking port 5 changes the tone");
    check(chf_machine_track(&rig.m, false) == 0 && rig.m.track == NULL, "and it stops again");
    rig.m.watch = 0;
    rig.m.bus_hook = NULL;
    rig.m.instr_hook = NULL;
}

/* ---- the FujiNet cartridge's tick and mailbox, with the machine ---- */
static void fujinet(void)
{
    static test_rig r2;
    static uint8_t app[16384];
    chf_fujinet_t *fj = chf_fujinet_create();
    chf_machine_opts_t mo;
    char why[160];
    chf_cart_status_t st;

    test_counter_image(app, 1);
    check(fj != NULL && chf_fujinet_power_on(fj, app, sizeof app, "", true, why, sizeof why) == 0,
          "the FujiNet cartridge powers on with a counter app (link off, mailbox inline)");
    memset(&r2, 0, sizeof r2);
    test_bios(r2.lo, r2.hi);
    memset(&mo, 0, sizeof mo);
    mo.machine = CHF_MACHINE_CHANNELF;
    mo.bios_lo = r2.lo;
    mo.bios_hi = r2.hi;
    mo.cart = chf_fujinet_cart(fj);
    mo.fujinet = fj;
    chf_machine_init(&r2.m, &mo);
    chf_machine_check_cart(&r2.m);
    check(!r2.m.parked, "an app that claims the mailbox is no problem for the stand-in's rule");
    check(chf_machine_peek(&r2.m, CHF_PEEK_PROG, 0xfc09) == 'F' && chf_machine_peek(&r2.m, CHF_PEEK_PROG, 0xfc0a) == 'N' &&
              chf_machine_peek(&r2.m, CHF_PEEK_PROG, 0xfc0b) == FN_PROTO_VER,
          "the status page carries the magic and protocol 2");
    for (int i = 0; i < 600; i++)
        chf_machine_run_frame(&r2.m);
    check(chf_machine_peek(&r2.m, CHF_PEEK_PROG, 0x8000) != 0, "the app counts in the cartridge's RAM at $8000");
    {
        /* the tick the console reads: floor(seconds * 60) at the CPU's own time (a few clocks past the blank) */
        const unsigned t = (unsigned)chf_machine_peek(&r2.m, CHF_PEEK_PROG, 0xfc0e) |
                           (unsigned)chf_machine_peek(&r2.m, CHF_PEEK_PROG, 0xfc0f) << 8;

        printf("  600 frames -> tick %u\n", t);
        check(t == 594, "600 frames of 60.53 Hz are 9.912 s: tick 594");
    }
    {
        const uint8_t c1 = chf_machine_peek(&r2.m, CHF_PEEK_PROG, 0x8000);

        chf_machine_reset(&r2.m);
        for (int i = 0; i < 2; i++)
            chf_machine_run_frame(&r2.m);
        check(chf_machine_peek(&r2.m, CHF_PEEK_PROG, 0x8000) != c1 && chf_machine_peek(&r2.m, CHF_PEEK_PROG, 0xfc0b) == FN_PROTO_VER,
              "RESET does not reach the cartridge: the app restarts on its RAM, the mailbox stays painted");
    }
    chf_fujinet_status(fj, &st);
    check(st.powered && st.mailbox && st.link_off && !st.link_up && st.page == -1 && st.npages == 0,
          "status: powered, mailbox live, no link, a flat image");
    check(chf_machine_space_size(&r2.m, CHF_PEEK_CART) == 0x18000, "the FujiNet CART space is the 64K image store then the 32K arena");
    check(chf_machine_peek(&r2.m, CHF_PEEK_CART, 0) == 0x55 && chf_machine_peek(&r2.m, CHF_PEEK_CART, 0x10000 + 0x7c09) == 'F',
          "its first byte is the signature and its arena holds the status page");
    chf_machine_poke(&r2.m, CHF_PEEK_PROG, 0xfd00, 0x99);
    check(chf_fujinet_arena(fj, 0x7d00) == 0, "a debugger edit of the register page does not touch it (and fires nothing)");
    chf_fujinet_destroy(fj);
}

int main(void)
{
    timing();
    ports();
    pads();
    memory();
    tone();
    reset();
    standin();
    debugger();
    fujinet();
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
