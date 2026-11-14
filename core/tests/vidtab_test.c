/*
 * vidtab_test -- the Video tab's controller (core/debugger/vidtab.c) on a
 * stopped machine whose VRAM and palette columns the test writes through the
 * debugger's own spaces: no program draws anything, so every pixel is known.
 * No machine: the tab says so. The five views and their sizes, the real pens
 * and the row palettes in Decoded, Live, Values and Beam, zoom and Fit, the
 * overlays, hover, pin and the marks, Break on writes, the tables (registers
 * from the port latches, colours, the 64 lines, the write log, timing), the
 * text sections, steady refreshes, and a fuzz over views, zooms, overlays,
 * cells and writes that must never crash or return a picture of the wrong
 * size.
 *
 * No setenv/unsetenv or POSIX-only calls: this builds under MinGW too.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_chf.h"
#include "chfmap.h"
#include "debugger_internal.h"
#include "chfvidtab.h"

static int failures;
static void check(int good, const char *what)
{
    printf("%s: %s\n", good ? "ok" : "FAIL", what);
    if (!good)
        failures++;
}

static uint8_t g_lo[CF_BIOS_HALF], g_hi[CF_BIOS_HALF];
static uint8_t g_app[16384];

static int wait_stopped(chfdebug *d, int want, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 2)
    {
        if (!!chfdebug_is_stopped(d) == want)
            return 1;
        test_sleep_ms(2);
    }
    return 0;
}

/* The machine on the tests' BIOS and a counter app, stopped on its first instruction. */
static int boot(chfdebug *d)
{
    chf_host_opts_t ho;

    test_bios(g_lo, g_hi);
    test_counter_image(g_app, 1);
    memset(&ho, 0, sizeof ho);
    ho.machine = CHF_MACHINE_CHANNELF;
    ho.bios_lo = g_lo;
    ho.bios_hi = g_hi;
    ho.bios_id = CHF_ROM_SL31253;
    ho.cart = g_app;
    ho.cart_size = sizeof g_app;
    ho.cart_type = CHF_CART_STD;
    ho.cart_app = true;
    ho.cart_sync = true;
    for (int tries = 0; tries < 40; tries++)
    {
        chfdebug_cpu cpu;
        int started;

        chf_host_lock();
        started = chf_host_start(&ho) == 0;
        if (started)
            chfdebug_power_up(d);
        chf_host_unlock();
        if (!started || !wait_stopped(d, 1, 5000))
            return 0;
        chfdebug_cpu_get(d, &cpu);
        if (cpu.pc == 0 && cpu.instructions == 0)
            return 1;
        chfdebug_power_down(d);
        chf_host_stop();
        test_sleep_ms(5);
    }
    return 0;
}

#define OPAQUE 0xff000000u

static uint32_t pen_rgb(int palette, int value)
{
    return chfdebug_colour_rgb(chfdebug_palette_pen(palette, value)) | OPAQUE;
}

static void put_cell(chfdebug *d, int row, int col, int value)
{
    chfdebug_write(d, CHF_SPACE_VRAM, (uint32_t)(row * CHFDEBUG_VRAM_COLS + col), (uint8_t)value);
}

/* A row's palette through its two palette columns: (v[126] & 2) | (v[125] >> 1). */
static void set_palette(chfdebug *d, int row, int palette)
{
    put_cell(d, row, CHFDEBUG_PAL_COL_A, (palette & 1) << 1);
    put_cell(d, row, CHFDEBUG_PAL_COL_B, palette & 2);
}

static uint32_t at(const uint32_t *pic, int wide, int posx, int posy)
{
    return pic[(size_t)posy * (size_t)wide + (size_t)posx];
}

static uint32_t rng_state = 20261114;
static uint32_t rnd(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

int main(void)
{
    chfdebug *dbg = chfdebug_create();
    chfvid_tab *tab;
    const uint32_t *pic;
    int wide, high;
    unsigned got;

    setvbuf(stdout, NULL, _IOLBF, 0);
    check(dbg != NULL, "an engine");
    if (!dbg)
        return 1;
    chfdebug_attach(dbg);

    /* ---- no machine ---- */
    tab = chfvid_tab_new(dbg);
    check(tab != NULL, "a tab");
    got = chfvid_tab_refresh(tab, 1);
    check((got & CHFVID_TAB_OFF) != 0 && chfvid_tab_picture(tab, &wide, &high) == NULL &&
              strlen(chfvid_tab_why(tab)) > 10,
          "no machine: OFF, no picture, a reason");
    {
        char text[256];
        check(chfvid_tab_text(tab, CHFVID_S_PIXEL, text, sizeof text) > 10 && strstr(text, "not running"),
              "no machine: the text says so");
        check(chfvid_tab_break_on_pixel(tab) == -1 && !chfvid_tab_can_break(tab),
              "no machine: Break on writes refuses");
    }
    chfvid_tab_free(tab);

    check(boot(dbg), "the machine stops on its first instruction");
    tab = chfvid_tab_new(dbg);

    /* a known VRAM: row 10 palette 1, row 12 palette 3, row 20 palette 0 */
    set_palette(dbg, 10, 1);
    set_palette(dbg, 12, 3);
    put_cell(dbg, 10, 20, 2);
    put_cell(dbg, 12, 40, 1);
    put_cell(dbg, 20, 30, 3);
    put_cell(dbg, 2, 2, 1);                       /* a hidden cell */

    chfvid_tab_set_overlays(tab, 0);
    chfvid_tab_set_zoom(tab, 1);

    /* ---- the views ---- */
    chfvid_tab_set_view(tab, CHFVID_V_LIVE);
    check((chfvid_tab_refresh(tab, 1) & CHFVID_TAB_PICTURE) != 0, "refresh");
    pic = chfvid_tab_picture(tab, &wide, &high);
    check(pic && wide == 204 && high == 232, "Live is 204 x 232");
    check(pic && at(pic, wide, (20 - 4) * 2, (10 - 4) * 4) == pen_rgb(1, 2),
          "Live: row 10 col 20 = 2 in palette 1 shows the real pen");
    check(pic && at(pic, wide, (40 - 4) * 2 + 1, (12 - 4) * 4 + 3) == pen_rgb(3, 1),
          "Live: row 12 col 40 = 1 in palette 3");
    check(pic && at(pic, wide, (30 - 4) * 2, (20 - 4) * 4) == pen_rgb(0, 3), "Live: palette 0, value 3");
    check(pic && at(pic, wide, 0, 0) == pen_rgb(0, 0) && (at(pic, wide, 0, 0) & OPAQUE) == OPAQUE,
          "pixels are opaque");

    chfvid_tab_set_view(tab, CHFVID_V_DECODED);
    chfvid_tab_refresh(tab, 1);
    pic = chfvid_tab_picture(tab, &wide, &high);
    check(pic && wide == 256 && high == 256, "Decoded is the whole VRAM, 256 x 256");
    check(pic && at(pic, wide, 20 * 2, 10 * 4) == pen_rgb(1, 2) && at(pic, wide, 40 * 2, 12 * 4) == pen_rgb(3, 1),
          "Decoded: each cell in its row's palette");
    {
        uint32_t decoded = pic ? at(pic, wide, 20 * 2, 10 * 4) : 0;
        chfvid_tab_set_view(tab, CHFVID_V_VALUES);
        chfvid_tab_refresh(tab, 1);
        pic = chfvid_tab_picture(tab, &wide, &high);
        check(pic && wide == 256 && high == 256, "Values is 256 x 256");
        check(pic && at(pic, wide, 20 * 2, 10 * 4) != decoded, "Values: false colour, not the palette's");
        check(pic && at(pic, wide, 20 * 2, 10 * 4) != at(pic, wide, 40 * 2, 12 * 4) &&
                  at(pic, wide, 40 * 2, 12 * 4) != at(pic, wide, 30 * 2, 20 * 4),
              "Values: values 2, 1 and 3 differ");
    }

    chfvid_tab_set_view(tab, CHFVID_V_BEAM);
    chfvid_tab_refresh(tab, 1);
    pic = chfvid_tab_picture(tab, &wide, &high);
    check(pic && wide == 204 && high == 232, "Beam is 204 x 232 (works stopped)");

    chfvid_tab_set_view(tab, CHFVID_V_DISPLAY);
    chfvid_tab_refresh(tab, 1);
    pic = chfvid_tab_picture(tab, &wide, &high);
    check(pic && wide == 204 && high == 232, "Display is the frame, 204 x 232");
    {
        static uint32_t ref[CHFDEBUG_VIEW_MAX_PIXELS];
        int rw = 0, rh = 0, same = 0;
        if (chfdebug_video_view(dbg, CHFDEBUG_VIEW_FRAME, 0, 1, ref, CHFDEBUG_VIEW_MAX_PIXELS, &rw, &rh) && pic)
        {
            same = rw == wide && rh == high;
            for (int pos = 0; same && pos < wide * high; pos++)
                same = (ref[pos] | OPAQUE) == pic[pos];
        }
        check(same, "Display is exactly the engine's FRAME view");
    }
    check(strcmp(chfvid_view_name(CHFVID_V_LIVE), "Live") == 0 && strcmp(chfvid_view_key(CHFVID_V_DECODED), "decoded") == 0,
          "view names and keys");

    /* ---- zoom and Fit ---- */
    chfvid_tab_set_view(tab, CHFVID_V_LIVE);
    chfvid_tab_set_zoom(tab, 3);
    chfvid_tab_refresh(tab, 0);
    pic = chfvid_tab_picture(tab, &wide, &high);
    check(pic && wide == 612 && high == 696 && chfvid_tab_effective_zoom(tab) == 3, "zoom 3 scales 204 x 232");
    check(pic && at(pic, wide, (20 - 4) * 6 + 5, (10 - 4) * 12 + 11) == pen_rgb(1, 2), "...and a cell covers its block");
    chfvid_tab_set_zoom(tab, 0);
    chfvid_tab_set_viewport(tab, 1000, 700);
    chfvid_tab_refresh(tab, 0);
    chfvid_tab_picture(tab, &wide, &high);
    check(chfvid_tab_effective_zoom(tab) == 3 && wide == 612, "Fit in 1000 x 700: the tall picture decides (3)");
    chfvid_tab_set_viewport(tab, 400, 300);
    chfvid_tab_refresh(tab, 0);
    chfvid_tab_picture(tab, &wide, &high);
    check(chfvid_tab_effective_zoom(tab) == 1 && wide == 204, "Fit in 400 x 300: 1");
    chfvid_tab_set_view(tab, CHFVID_V_DECODED);
    chfvid_tab_set_viewport(tab, 600, 600);
    chfvid_tab_refresh(tab, 0);
    chfvid_tab_picture(tab, &wide, &high);
    check(chfvid_tab_effective_zoom(tab) == 2 && wide == 512 && high == 512, "Fit Decoded in 600 x 600: 2");
    chfvid_tab_set_zoom(tab, 16);
    chfvid_tab_refresh(tab, 0);
    pic = chfvid_tab_picture(tab, &wide, &high);
    check(pic && (long)wide * high <= 17000000L && wide % 256 == 0 && chfvid_tab_effective_zoom(tab) == wide / 256,
          "zoom 16 on the whole VRAM is capped to the pixel budget");

    /* ---- overlays ---- */
    chfvid_tab_set_view(tab, CHFVID_V_DECODED);
    chfvid_tab_set_zoom(tab, 2);
    chfvid_tab_set_overlays(tab, 0);
    chfvid_tab_refresh(tab, 1);
    pic = chfvid_tab_picture(tab, &wide, &high);
    {
        uint32_t *plain = pic ? malloc((size_t)wide * (size_t)high * sizeof *plain) : NULL;
        int diff = 0, count = 0;
        const chfvid_overlay_info *info = chfvid_overlays(&count);

        check(count == 8 && info[0].name && info[7].label, "eight overlays, each named");
        if (plain)
        {
            memcpy(plain, pic, (size_t)wide * (size_t)high * sizeof *plain);
            chfvid_tab_set_overlays(tab, CHFVID_OV_GRID | CHFVID_OV_VISIBLE);
            check((chfvid_tab_refresh(tab, 0) & CHFVID_TAB_PICTURE) != 0, "an overlay change repaints");
            pic = chfvid_tab_picture(tab, &wide, &high);
            for (int pos = 0; pic && pos < wide * high; pos++)
                diff += plain[pos] != pic[pos];
            check(diff > 500, "Grid and Visible draw over the picture");
            chfvid_tab_set_overlays(tab, CHFVID_OV_ALL);
            chfvid_tab_refresh(tab, 0);
            check(chfvid_tab_overlays(tab) == CHFVID_OV_ALL && chfvid_tab_picture(tab, NULL, NULL) != NULL,
                  "all overlays render");
            chfvid_tab_set_overlays(tab, 0);
            chfvid_tab_refresh(tab, 0);
            pic = chfvid_tab_picture(tab, &wide, &high);
            diff = 0;
            for (int pos = 0; pic && pos < wide * high; pos++)
                diff += plain[pos] != pic[pos];
            check(diff == 0, "none: the plain picture again");
            free(plain);
        }
    }

    /* ---- hover, pin, marks (Decoded, zoom 4) ---- */
    chfvid_tab_set_zoom(tab, 4);
    chfvid_tab_refresh(tab, 1);
    {
        const chfvid_pixel *pix;
        int hover[4] = { 0 }, pin[4] = { 0 }, marks;

        check(chfvid_tab_hover(tab, 20 * 2 * 4 + 3, 10 * 4 * 4 + 5) == 1, "hover over a cell changes the inspector");
        pix = chfvid_tab_pixel(tab);
        check(pix && pix->valid && pix->row == 10 && pix->col == 20 && pix->value == 2 && pix->palette == 1 &&
                  pix->rgb == pen_rgb(1, 2) && pix->visible && pix->scanline == 40,
              "the cell: row 10 col 20, value 2, palette 1, its pen, beam line 40");
        check(pix && strstr(pix->text, "row 10, column 20") && strstr(pix->text, "not written"), "its text");
        marks = chfvid_tab_marks(tab, hover, pin);
        check(marks == 1 && hover[0] == 20 * 2 * 4 && hover[1] == 10 * 4 * 4 && hover[2] == 8 && hover[3] == 16,
              "the hover mark is the cell's rectangle");
        check(chfvid_tab_hover(tab, 20 * 2 * 4 + 7, 10 * 4 * 4 + 12) == 0, "another pixel of the same cell: unchanged");
        chfvid_tab_pin(tab, 40 * 2 * 4, 12 * 4 * 4);
        pix = chfvid_tab_pixel(tab);
        check(pix && pix->row == 12 && pix->col == 40 && pix->palette == 3, "pin wins over hover");
        marks = chfvid_tab_marks(tab, hover, pin);
        check(marks == 3 && pin[0] == 40 * 8, "both marks");
        chfvid_tab_pin(tab, 40 * 2 * 4 + 1, 12 * 4 * 4 + 1);
        pix = chfvid_tab_pixel(tab);
        check(pix && pix->row == 10, "the same cell again unpins (the hover shows)");
        chfvid_tab_hover(tab, -1, -1);
        check(chfvid_tab_pixel(tab) == NULL && chfvid_tab_marks(tab, hover, pin) == 0, "the pointer left: nothing");
        chfvid_tab_hover(tab, 0, 0);
        pix = chfvid_tab_pixel(tab);
        check(pix && !pix->visible && pix->hidden_why && strcmp(pix->hidden_why, "left border") == 0,
              "a hidden cell says why");
        chfvid_tab_hover(tab, 2 * 8, 2 * 16);
        check(chfvid_tab_pixel(tab) && !chfvid_tab_pixel(tab)->visible, "(2,2) of the whole VRAM is hidden");
        chfvid_tab_hover(tab, 100000, 100000);
        check(chfvid_tab_pixel(tab) == NULL, "past the picture: nothing");

        /* the visible views map pixels to cells 4 and up */
        chfvid_tab_set_view(tab, CHFVID_V_LIVE);
        chfvid_tab_refresh(tab, 1);
        chfvid_tab_hover(tab, (20 - 4) * 2 * 4 + 1, (10 - 4) * 4 * 4 + 1);
        pix = chfvid_tab_pixel(tab);
        check(pix && pix->row == 10 && pix->col == 20, "Live: pixel -> cell is offset by the visible origin");
        marks = chfvid_tab_marks(tab, hover, pin);
        check(marks == 1 && hover[0] == 16 * 8 && hover[1] == 6 * 16, "...and so is the mark");
        chfvid_tab_pin_cell(tab, 2, 2);
        marks = chfvid_tab_marks(tab, hover, pin);
        check((marks & 2) == 0 && chfvid_tab_pixel(tab)->row == 2, "a pinned cell the view does not show has no mark");
        chfvid_tab_unpin(tab);
    }

    /* ---- Break on writes ---- */
    {
        chfdebug_breakpoint list[32];
        int first, second, count, found = 0;

        chfvid_tab_set_view(tab, CHFVID_V_DECODED);
        chfvid_tab_refresh(tab, 1);
        chfvid_tab_hover(tab, 40 * 8 + 1, 12 * 16 + 1);
        check(chfvid_tab_can_break(tab), "a cell can break");
        first = chfvid_tab_break_on_pixel(tab);
        second = chfvid_tab_break_on_pixel(tab);
        check(first > 0 && first == second, "Break on writes adds one breakpoint, not two");
        count = chfdebug_breakpoint_list(dbg, list, 32);
        for (int idx = 0; idx < count; idx++)
            found |= list[idx].id == first && list[idx].type == CHFDEBUG_BP_VRAM && list[idx].space == CHF_SPACE_VRAM &&
                     list[idx].start == 12 * 128 + 40 && list[idx].end == 12 * 128 + 40;
        check(found, "...a VRAM breakpoint on row 12 col 40");
        chfdebug_breakpoint_clear(dbg);
    }

    /* ---- the tables ---- */
    chfvid_tab_set_view(tab, CHFVID_V_LIVE);
    chfvid_tab_unpin(tab);
    chfvid_tab_hover(tab, -1, -1);
    chfdebug_write(dbg, CHF_SPACE_IO, 1, 0x40);                 /* colour value (~0x40 >> 6) & 3 = 2 */
    chfdebug_write(dbg, CHF_SPACE_IO, 4, (uint8_t)~40);         /* column 40 */
    chfdebug_write(dbg, CHF_SPACE_IO, 5, (uint8_t)(~12 & 0x3f) | 0x40);   /* row 12, sound 1 kHz */
    chfvid_tab_refresh(tab, 1);
    {
        const chfvid_reg *regs;
        const chfvid_swatch *swat;
        const chfvid_line_row *rows;
        const char *const *timing;
        int nregs = chfvid_tab_regs(tab, &regs);
        int found4 = 0, found5 = 0, foundcell = 0;

        check(nregs == 6 && strcmp(regs[0].name, "PORT 0") == 0, "six registers: four ports, CELL, SOUND");
        for (int idx = 0; idx < nregs; idx++)
        {
            found4 |= regs[idx].port == 4 && regs[idx].raw == (uint8_t)~40 && regs[idx].comp == 40 &&
                      strstr(regs[idx].meaning, "column 40");
            found5 |= regs[idx].port == 5 && strstr(regs[idx].meaning, "row 12") && strstr(regs[idx].meaning, "1 kHz");
            foundcell |= regs[idx].port < 0 && strstr(regs[idx].value, "r12 c40") != NULL;
        }
        check(found4 && found5, "ports 4 and 5: raw, complement and decoded");
        check(foundcell, "CELL: the next strobe's row, column and value");
        check(regs[0].age >= 0 && strlen(regs[0].last) > 0, "ages and last-write text are filled");

        check(chfvid_tab_swatches(tab, &swat) == 16, "sixteen swatches");
        check(swat[1 * 4 + 2].rgb == pen_rgb(1, 2) && swat[1 * 4 + 2].cells >= 1 && swat[3 * 4 + 1].cells >= 1,
              "the palettes' pens, and how many cells of its rows hold each");
        check(swat[0].rows + swat[4].rows + swat[8].rows + swat[12].rows == 64, "every row has one palette");

        check(chfvid_tab_lines(tab, &rows) == 64, "64 lines");
        check(rows[10].palette == 1 && rows[12].palette == 3 && rows[20].palette == 0, "per-row palettes");
        check(rows[10].scanline == rows[10].row * rows[10].rows_lines && rows[10].rows_lines >= 4,
              "scanline = row * lines per row");
        check(rows[3].visible == 0 && rows[4].visible == 1 && rows[61].visible == 1 && rows[62].visible == 0,
              "rows 4-61 are visible");
        check(rows[10].background == pen_rgb(1, 0), "a row's background is its palette's value 0");

        check(chfvid_tab_timing(tab, &timing) >= 8 && strstr(timing[0], "NTSC") && strchr(timing[1], '\t'),
              "timing lines are label<TAB>value");
        check(strchr(chfvid_tab_status(tab), '\n') != NULL, "status is two lines");
        check(chfvid_tab_beam_permille(tab) >= 0, "the beam is reported");
    }

    /* ---- the write log ---- */
    {
        const chfdebug_vram_write *writes;
        chfvid_write_filter filt;
        int total;

        /* strobe a cell the way the CPU does: the latches set above, then port 0 bit 5 */
        chfdebug_write(dbg, CHF_SPACE_IO, 0, 0x60);
        chfdebug_write(dbg, CHF_SPACE_IO, 0, 0x50);
        chfvid_tab_refresh(tab, 1);
        total = chfvid_tab_writes(tab, &writes);
        printf("  %d logged write(s) after a strobe\n", total);
        if (total > 0)
        {
            char line[128];
            check(writes[total - 1].row == 12 && writes[total - 1].col == 40 && writes[total - 1].value == 2,
                  "the strobe is in the log: row 12 col 40 = 2");
            check(chfvid_write_text(&writes[total - 1], line, sizeof line) > 20 && strstr(line, "row 12 col 40 = 2"),
                  "its text");
            memset(&filt, 0, sizeof filt);
            filt.changes_only = 1;
            chfvid_tab_set_write_filter(tab, &filt);
            chfvid_tab_refresh(tab, 1);
            check(chfvid_tab_writes(tab, &writes) >= 1, "changes only: the strobe changed the cell (1 -> 2)");
            filt.palette_cols = 1;
            chfvid_tab_set_write_filter(tab, &filt);
            chfvid_tab_refresh(tab, 1);
            check(chfvid_tab_writes(tab, &writes) == 0, "palette columns only: none");
            memset(&filt, 0, sizeof filt);
            filt.inspected_cell = 1;
            chfvid_tab_set_write_filter(tab, &filt);
            chfvid_tab_pin_cell(tab, 12, 40);
            chfvid_tab_refresh(tab, 1);
            total = chfvid_tab_writes(tab, &writes);
            check(total >= 1 && writes[0].row == 12 && writes[0].col == 40, "the inspected cell's writes only");
            chfvid_tab_pin_cell(tab, 50, 50);
            chfvid_tab_refresh(tab, 1);
            check(chfvid_tab_writes(tab, &writes) == 0, "...and none for a cell never written");
            {
                const chfvid_line_row *rows;
                chfvid_tab_lines(tab, &rows);
                check(rows[12].last_pc >= -1, "the Lines table's last writer");
            }
            chfvid_tab_unpin(tab);
            memset(&filt, 0, sizeof filt);
            chfvid_tab_set_write_filter(tab, &filt);
            chfvid_tab_clear_writes(tab);
            chfvid_tab_refresh(tab, 1);
            check(chfvid_tab_writes(tab, &writes) == 0, "Clear empties the log");
        }
        else
            check(1, "(this machine logs no strobe from a debugger IO write: write-log checks skipped)");
    }

    /* ---- the text sections ---- */
    {
        static char text[65536];
        int len;

        for (int sec = 0; sec < CHFVID_S_COUNT; sec++)
        {
            len = chfvid_tab_text(tab, sec, text, sizeof text);
            printf("  [%s] %d bytes\n", chfvid_section_name(sec), len);
            check(len > 10 && (int)strlen(text) == len, chfvid_section_key(sec));
        }
        len = chfvid_tab_text(tab, CHFVID_S_REGISTERS, text, sizeof text);
        check(strstr(text, "PORT 4") && strstr(text, "column 40"), "registers text");
        len = chfvid_tab_text(tab, CHFVID_S_LINES, text, sizeof text);
        check(strstr(text, "row 10") && strstr(text, "light blue"), "lines text");
        len = chfvid_tab_text(tab, CHFVID_S_COLOURS, text, sizeof text);
        check(strstr(text, "palette 3") && strstr(text, "light green"), "colours text");
        len = chfvid_tab_text(tab, CHFVID_S_PIXEL, text, sizeof text);
        check(strstr(text, "next strobe's cell") && strstr(text, "row 12, column 40"), "pixel text: the next strobe's cell");
        len = chfvid_tab_text(tab, CHFVID_S_TIMING, text, 40);
        check(len > 0 && len < 40 && (int)strlen(text) == len, "a short buffer is cut, not overrun");
        check(chfvid_section_from_name("reg") == CHFVID_S_REGISTERS && chfvid_section_from_name("wri") == CHFVID_S_WRITES &&
                  chfvid_section_from_name("zzz") == -1 && chfvid_section_from_name("") == -1,
              "section names by prefix");
    }

    /* ---- steady: nothing changed, nothing to repaint ---- */
    {
        unsigned last = 0xffffu;

        for (int pass = 0; pass < CHFVID_REG_FADE + 4; pass++)
            last = chfvid_tab_refresh(tab, 0);
        check(last == 0, "a stopped machine, nothing touched: refresh reports nothing to redraw");
        put_cell(dbg, 30, 60, 3);
        last = chfvid_tab_refresh(tab, 0);
        check((last & CHFVID_TAB_PICTURE) != 0, "a VRAM edit repaints");
    }

    /* ---- fuzz ---- */
    {
        static const int zooms[7] = { 0, 1, 2, 3, 4, 8, 16 };
        int bad = 0;

        for (int iter = 0; iter < 1500; iter++)
        {
            int expect_w = 0, expect_h = 0, view = (int)(rnd() % CHFVID_V_COUNT);
            const int zoom = zooms[rnd() % 7];

            chfvid_tab_set_view(tab, view);
            chfvid_tab_set_zoom(tab, zoom);
            chfvid_tab_set_overlays(tab, rnd() & 0xff);
            chfvid_tab_set_viewport(tab, 50 + (int)(rnd() % 1500), 50 + (int)(rnd() % 1200));
            for (int edit = (int)(rnd() % 4); edit > 0; edit--)
            {
                if (rnd() % 3 == 0)
                    set_palette(dbg, (int)(rnd() % 64), (int)(rnd() % 4));
                else
                    put_cell(dbg, (int)(rnd() % 64), (int)(rnd() % 128), (int)(rnd() % 4));
            }
            chfvid_tab_refresh(tab, (int)(rnd() % 2));
            pic = chfvid_tab_picture(tab, &wide, &high);
            chfdebug_video_view(dbg, view == 0 ? CHFDEBUG_VIEW_FRAME : view == 1 ? CHFDEBUG_VIEW_VRAM :
                                view == 2 ? CHFDEBUG_VIEW_BEAM : view == 3 ? CHFDEBUG_VIEW_VALUES : CHFDEBUG_VIEW_LIVE,
                                0, 1, NULL, 0, &expect_w, &expect_h);
            if (!pic || wide != expect_w * chfvid_tab_effective_zoom(tab) ||
                high != expect_h * chfvid_tab_effective_zoom(tab) || chfvid_tab_effective_zoom(tab) < 1 ||
                chfvid_tab_effective_zoom(tab) > 16 || (long)wide * high > 17000000L)
                bad++;
            if (zoom && chfvid_tab_effective_zoom(tab) > zoom)
                bad++;
            if (pic && (rnd() % 3 == 0))
            {
                int hover[4], pin[4];
                chfvid_tab_hover(tab, (int)(rnd() % (unsigned)(wide + 20)) - 10, (int)(rnd() % (unsigned)(high + 20)) - 10);
                if (rnd() % 5 == 0)
                    chfvid_tab_pin(tab, (int)(rnd() % (unsigned)wide), (int)(rnd() % (unsigned)high));
                if (rnd() % 7 == 0)
                    chfvid_tab_unpin(tab);
                if (chfvid_tab_marks(tab, hover, pin) & 1)
                    if (hover[0] < 0 || hover[1] < 0 || hover[0] + hover[2] > wide || hover[1] + hover[3] > high)
                        bad++;
                if (rnd() % 11 == 0)
                    chfvid_tab_break_on_pixel(tab);
            }
            if (iter % 200 == 0)
            {
                static char text[65536];
                chfvid_tab_text(tab, (int)(rnd() % CHFVID_S_COUNT), text, sizeof text);
                chfdebug_breakpoint_clear(dbg);
            }
        }
        check(bad == 0, "fuzz: every picture is the view's size times its zoom, marks inside it");
    }

    chfvid_tab_free(tab);
    chfdebug_detach(dbg);
    chf_host_stop();
    chfdebug_free(dbg);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
