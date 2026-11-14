/*
 * vidtab_live_test -- the Video tab against the real machine: the CONFIG
 * cartridge on the app's stand-in BIOS through a whole session (which draws
 * nothing, so the test paints the VRAM through the debugger first). Running:
 * every view draws at its size with ink on it, the tables fill. Stopped,
 * several times over: Display is exactly the frame the session publishes (a
 * stop is only reported once its state is published, and the session drops
 * identical frames, so the compare retries), Beam and Decoded work, the
 * inspector reads the engine's own cell, and the machine runs on afterwards.
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
#include "chfvidtab.h"
#include "test_tmpdir.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        failures++;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int wait_stopped(chfdebug *dbg, int want, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 5)
    {
        if (!!chfdebug_is_stopped(dbg) == want)
            return 1;
        sleep_ms(5);
    }
    return 0;
}

static uint32_t frame_copy[CHFSESSION_FB_WIDTH * CHFSESSION_FB_HEIGHT];

int main(void)
{
    char cfg[512], data[512];
    chfsession_paths paths;
    chfsession_start_opts opts;
    chfsession *sess;
    chfdebug *dbg;
    chfvid_tab *tab;
    const uint32_t *pic;
    int wide, high;
    uint64_t start;

    setvbuf(stdout, NULL, _IOLBF, 0);
    test_tmpdir(cfg, sizeof cfg, "vcfg");
    test_tmpdir(data, sizeof data, "vdata");
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    paths.fujinet_lib = "";
    sess = chfsession_new(&paths);
    if (!sess)
        return 1;
    chfsession_default_opts(sess, &opts);
    opts.enable_fujinet = 0;
    opts.enable_audio = 0;
    opts.enable_gamepad = 0;
    if (chfsession_start(sess, &opts) != 0)
    {
        printf("SKIP: the session does not start CONFIG here\n");
        chfsession_free(sess);
        return 77;
    }
    check(1, "session starts (CONFIG)");
    dbg = chfsession_debugger(sess);
    chfdebug_attach(dbg);
    chfdebug_resume(dbg);
    tab = chfvid_tab_new(dbg);
    check(tab != NULL, "tab");
    start = chfsession_frame_count(sess);
    for (int waited = 0; waited < 8000 && chfsession_frame_count(sess) < start + 120; waited += 10)
        sleep_ms(10);

    /* The stand-in BIOS draws nothing, so the test paints the VRAM itself while
     * stopped: a pattern over the visible area and a palette on every row. */
    chfdebug_stop(dbg);
    check(wait_stopped(dbg, 1, 3000), "stopped to paint the VRAM");
    for (int row = 0; row < CHFDEBUG_VRAM_ROWS; row++)
    {
        const int pal = row & 3;

        for (int col = CHFDEBUG_VIS_COL0; col < CHFDEBUG_VIS_COL0 + CHFDEBUG_VIS_COLS; col++)
            chfdebug_write(dbg, CHF_SPACE_VRAM, (uint32_t)(row * CHFDEBUG_VRAM_COLS + col),
                           (uint8_t)((row + col / 3) & 3));
        chfdebug_write(dbg, CHF_SPACE_VRAM, (uint32_t)(row * CHFDEBUG_VRAM_COLS + CHFDEBUG_PAL_COL_A),
                       (uint8_t)((pal & 1) << 1));
        chfdebug_write(dbg, CHF_SPACE_VRAM, (uint32_t)(row * CHFDEBUG_VRAM_COLS + CHFDEBUG_PAL_COL_B),
                       (uint8_t)(pal & 2));
    }
    chfdebug_resume(dbg);
    start = chfsession_frame_count(sess);
    for (int waited = 0; waited < 8000 && chfsession_frame_count(sess) < start + 30; waited += 10)
        sleep_ms(10);

    /* running: every view, at its size, with ink */
    chfvid_tab_set_zoom(tab, 1);
    chfvid_tab_set_overlays(tab, 0);
    for (int view = 0; view < CHFVID_V_COUNT; view++)
    {
        const int whole = view == CHFVID_V_DECODED || view == CHFVID_V_VALUES;
        int lit = 0;

        chfvid_tab_set_view(tab, view);
        check((chfvid_tab_refresh(tab, 1) & CHFVID_TAB_PICTURE) != 0, chfvid_view_name(view));
        pic = chfvid_tab_picture(tab, &wide, &high);
        check(pic && wide == (whole ? 256 : 204) && high == (whole ? 256 : 232), "...its size (running)");
        for (int pos = 0; pic && pos < wide * high; pos++)
            lit += pic[pos] != pic[0];
        check(lit > 50, "...with ink on it");
    }
    {
        const chfvid_reg *regs;
        const chfvid_line_row *rows;
        char text[8192];

        check(chfvid_tab_regs(tab, &regs) == 6, "registers (running)");
        check(chfvid_tab_lines(tab, &rows) == 64, "64 lines (running)");
        check(chfvid_tab_text(tab, CHFVID_S_TIMING, text, sizeof text) > 40 && strstr(text, "Strobes"), "timing text");
    }

    /* stopped, several times over */
    for (int round = 0; round < 4; round++)
    {
        int diff = -1, tries;

        chfdebug_stop(dbg);
        check(wait_stopped(dbg, 1, 3000), "stopped");
        for (tries = 0; tries < 20 && diff != 0; tries++)
        {
            uint64_t serial = 0;

            sleep_ms(30);
            chfvid_tab_set_view(tab, CHFVID_V_DISPLAY);
            chfvid_tab_refresh(tab, 1);
            pic = chfvid_tab_picture(tab, &wide, &high);
            chfsession_copy_frame(sess, frame_copy, &serial);
            diff = pic && wide == CHFSESSION_FB_WIDTH && high == CHFSESSION_FB_HEIGHT ? 0 : -1;
            for (int pos = 0; diff == 0 && pos < wide * high; pos++)
                diff += pic[pos] != (frame_copy[pos] | 0xff000000u);
        }
        if (diff != 0)
        {
            printf("      %d pixel(s) differ after %d tries (pic %p %dx%d)\n", diff, tries, (const void *)pic, wide, high);
            for (int pos = 0; pic && pos < wide * high; pos++)
                if (pic[pos] != (frame_copy[pos] | 0xff000000u))
                    printf("      at %d,%d: tab %08X session %08X\n", pos % wide, pos / wide, pic[pos], frame_copy[pos]);
        }
        check(diff == 0, "stopped: Display is exactly the session's frame");

        chfvid_tab_set_view(tab, CHFVID_V_BEAM);
        chfvid_tab_refresh(tab, 1);
        pic = chfvid_tab_picture(tab, &wide, &high);
        check(pic && wide == 204 && high == 232, "Beam is available while stopped");
        check(chfvid_tab_beam_permille(tab) >= 0 && chfvid_tab_beam_permille(tab) <= 1000, "the beam is reported");

        chfvid_tab_set_view(tab, CHFVID_V_DECODED);
        chfvid_tab_refresh(tab, 1);
        pic = chfvid_tab_picture(tab, &wide, &high);
        check(pic && wide == 256, "Decoded while stopped");
        {
            /* a hover on the visible area's top-left cell reads the engine's own cell */
            chfdebug_vram_cell cell;
            const chfvid_pixel *pix;

            chfvid_tab_set_zoom(tab, 2);
            chfvid_tab_refresh(tab, 1);
            chfvid_tab_hover(tab, CHFDEBUG_VIS_COL0 * 4 + 1, CHFDEBUG_VIS_ROW0 * 8 + 1);
            pix = chfvid_tab_pixel(tab);
            check(chfdebug_vram_cell_get(dbg, CHFDEBUG_VIS_ROW0, CHFDEBUG_VIS_COL0, &cell) == 0 && pix && pix->valid &&
                      pix->value == cell.value && pix->rgb == (cell.rgb | 0xff000000u),
                  "the inspector reads the cell the engine does");
            chfvid_tab_hover(tab, -1, -1);
            chfvid_tab_set_zoom(tab, 1);
        }
        chfdebug_resume(dbg);
        check(wait_stopped(dbg, 0, 3000), "running again");
        sleep_ms(60);
    }

    chfvid_tab_free(tab);
    chfsession_stop(sess);
    chfsession_free(sess);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
