/*
 * video_test -- the VRAM, its four ports and its picture (core/chf/video.c), held to
 * MAME's channelf.cpp / channelf_v.cpp as written out here by hand: the register
 * complements, the level-triggered strobe, the read-back (~external | latch) and
 * the controller gate, the sixteen palette-column combinations, and the geometry of
 * the published frame. No machine, no CPU, no ROM.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "video.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        failures++;
}

/* the pens, MAME's channelf_pens */
#define BLACK   0x101010
#define WHITE   0xfdfdfd
#define RED     0xff3153
#define GREEN   0x02cc5d
#define BLUE    0x4b3ff3
#define LTGRAY  0xe0e0e0
#define LTGREEN 0x91ffa6
#define LTBLUE  0xced0ff

static cf_video_t v;
static uint32_t fb[CF_FRAME_W * CF_FRAME_H];

static void plot(unsigned row, unsigned col, unsigned val)
{
    /* the BIOS's sequence: colour, column, row, complemented; strobe $60 then $50 */
    video_write(&v, 1, (uint8_t)~(val << 6));
    video_write(&v, 4, (uint8_t)~col);
    video_write(&v, 5, (uint8_t)(~row & 0x3f));
    video_write(&v, 0, 0x60);
    video_write(&v, 0, 0x50);
}

static uint32_t pixel(unsigned row, unsigned col)      /* the frame's pixel at a visible cell's top-left */
{
    return fb[(row - 4) * 4 * CF_FRAME_W + (col - 4) * 2];
}

int main(void)
{
    video_init(&v);
    check(v.latch[0] == 0 && v.latch[1] == 0 && v.latch[2] == 0 && v.latch[3] == 0 && v.vram[0] == 0 && v.vram[8191] == 0,
          "power-on: VRAM and latches clear");

    /* the registers */
    video_write(&v, 1, 0x3f);
    video_write(&v, 4, 0x00);
    video_write(&v, 5, 0xc0);
    check(v.val_reg == 3, "port 1: value = (~data >> 6) & 3 ($3F -> 3)");
    check(v.col_reg == 0x7f, "port 4: column = ~data & $7F ($00 -> 127; bit 7 ignored)");
    check(v.row_reg == 0x3f, "port 5: row = ~data & $3F ($C0 -> 63; bits 7-6 are the sound)");
    video_write(&v, 4, 0x80);
    video_write(&v, 5, 0x00);
    check(v.col_reg == 0x7f && v.row_reg == 0x3f, "bit 7 of port 4 and bits 7-6 of port 5 change nothing");
    video_write(&v, 1, 0xff);
    video_write(&v, 4, (uint8_t)~5);
    video_write(&v, 5, (uint8_t)(~7 & 0x3f));
    check(v.val_reg == 0 && v.col_reg == 5 && v.row_reg == 7, "colour 0, column 5, row 7");
    video_write(&v, 2, 0x55);
    video_write(&v, 3, 0x55);
    check(v.latch[1] == 0xff && v.latch[2] == (uint8_t)~5, "ports 2 and 3 are not ports of this console");

    /* the strobe: any port 0 write with bit 5 */
    video_init(&v);
    plot(10, 20, 2);
    check(v.vram[10 * 128 + 20] == 2, "$60 then $50 stores once");
    video_write(&v, 1, (uint8_t)~(3 << 6));
    check(v.vram[10 * 128 + 20] == 2, "writing the colour does not store");
    check(video_write(&v, 0, 0x50) == 0 && v.vram[10 * 128 + 20] == 2, "a port 0 write without bit 5 stores nothing");
    {
        static const uint8_t no[] = { 0x00, 0x10, 0x40, 0x80, 0xc0, 0x0f, 0x5f, 0xdf };
        static const uint8_t yes[] = { 0x20, 0x30, 0x60, 0xa0, 0xe0, 0x3f, 0xff, 0x21 };

        for (unsigned i = 0; i < sizeof no; i++)
            check(video_write(&v, 0, no[i]) == 0, "no bit 5: no store");
        for (unsigned i = 0; i < sizeof yes; i++) {
            video_write(&v, 1, (uint8_t)~(3 << 6));
            check(video_write(&v, 0, yes[i]) == 10 * 128 + 20 + 1 && v.vram[10 * 128 + 20] == 3, "bit 5: a store of colour 3 (level-triggered)");
            video_write(&v, 1, (uint8_t)~(1 << 6));
        }
    }
    check(video_write(&v, 2, 0x20) == 0 && video_write(&v, 7, 0x20) == 0, "other ports store nothing");

    /* the last cell and the first */
    plot(63, 127, 1);
    plot(0, 0, 3);
    check(v.vram[63 * 128 + 127] == 1 && v.vram[0] == 3, "rows 0-63 and columns 0-127 all reachable");

    /* read-back: ~external | latch */
    video_init(&v);
    check(video_read(&v, 0, 0x0, 0, 0) == 0x0f, "port 0, nothing pressed, latch 0: ~$F0 | 0 = $0F");
    check(video_read(&v, 0, 0xf, 0, 0) == 0x00, "all four buttons held: $00");
    check(video_read(&v, 0, 0x1, 0, 0) == 0x0e, "TIME held: bit 0 low");
    video_write(&v, 0, 0x00);
    check(video_read(&v, 1, 0, 0x00, 0x00) == 0xff, "port 1, no contact: $FF");
    check(video_read(&v, 1, 0, 0xff, 0x00) == 0x00 && video_read(&v, 1, 0, 0x81, 0) == 0x7e, "port 1 shows the RIGHT controller, active low");
    check(video_read(&v, 4, 0, 0x00, 0xff) == 0x00 && video_read(&v, 4, 0, 0, 0x42) == 0xbd, "port 4 shows the LEFT controller, active low");
    video_write(&v, 1, 0xa5);
    video_write(&v, 4, 0x5a);
    video_write(&v, 5, 0x3c);
    check(video_read(&v, 1, 0, 0, 0) == 0xff && video_read(&v, 1, 0, 0xff, 0) == 0xa5,
          "the port 1 latch ORs in: a held contact reads as the latch bit");
    check(video_read(&v, 4, 0, 0, 0) == 0xff && video_read(&v, 4, 0, 0, 0xff) == 0x5a, "likewise port 4");
    check(video_read(&v, 5, 0, 0, 0) == 0x3c, "port 5 reads its latch back (~$FF | latch)");
    /* the controller gate, port 0 bit 6 */
    video_write(&v, 0, 0x40);
    check(video_read(&v, 1, 0, 0x81, 0) == (uint8_t)(~(0xc0 | 0x81) | 0xa5) && video_read(&v, 1, 0, 0x81, 0) == 0xbf,
          "gate set: port 1's external gets bits 7-6 forced high, so they read 0 | latch ($BF with the $A5 latch)");
    check(video_read(&v, 4, 0, 0, 0x42) == 0x5a, "gate set: port 4's external is $FF, so it reads only its latch");
    video_write(&v, 0, 0x20);
    check(video_read(&v, 0, 0, 0, 0) == 0x2f, "port 0's own latch reads back too");
    check(video_read(&v, 2, 0xf, 0xff, 0xff) == 0 && video_read(&v, 3, 0xf, 0xff, 0xff) == 0, "unmapped ports read 0");

    /* the sixteen palettes */
    {
        static const struct { unsigned pal; uint32_t bg; } expect[4] = {
            { 0, BLACK }, { 1, LTBLUE }, { 2, LTGRAY }, { 3, LTGREEN },
        };
        int bad = 0;

        for (unsigned a = 0; a < 4; a++)
            for (unsigned b = 0; b < 4; b++) {
                const unsigned pal = (b & 2) | (a >> 1);

                video_init(&v);
                plot(20, 125, a);
                plot(20, 126, b);
                bad += video_row_palette(&v, 20) != pal;
                bad += video_row_palette(&v, 19) != 0;
                bad += video_pen_rgb(video_palette_pen(pal, 0)) != expect[pal].bg;
                if (pal == 0) {
                    bad += video_pen_rgb(video_palette_pen(0, 1)) != WHITE || video_pen_rgb(video_palette_pen(0, 2)) != WHITE ||
                           video_pen_rgb(video_palette_pen(0, 3)) != WHITE;
                } else {
                    bad += video_pen_rgb(video_palette_pen(pal, 1)) != BLUE || video_pen_rgb(video_palette_pen(pal, 2)) != RED ||
                           video_pen_rgb(video_palette_pen(pal, 3)) != GREEN;
                }
            }
        check(!bad, "all 16 (col 125, col 126) combinations: palette = (v126 & 2) | (v125 >> 1); black&white, light blue, light grey, light green");
    }

    /* the picture */
    video_init(&v);
    plot(4, 4, 1);
    plot(61, 105, 2);
    plot(4, 105, 3);
    plot(61, 4, 1);
    plot(3, 4, 3);
    plot(4, 3, 3);
    plot(62, 4, 3);
    plot(4, 106, 3);
    plot(30, 127, 3);                               /* the palette columns' neighbour, hidden */
    video_render(&v, fb);
    check(pixel(4, 4) == WHITE && fb[1] == WHITE && fb[CF_FRAME_W] == WHITE && fb[3 * CF_FRAME_W + 1] == WHITE &&
              fb[2] == BLACK && fb[4 * CF_FRAME_W] == BLACK,
          "a cell is 2 pixels wide and 4 tall, the top-left cell is VRAM (4, 4), value 1 white on black");
    check(pixel(61, 105) == WHITE && fb[(CF_FRAME_H - 1) * CF_FRAME_W + CF_FRAME_W - 1] == WHITE,
          "the bottom-right cell is (61, 105): the frame is 204 x 232");
    check(pixel(4, 105) == WHITE && pixel(61, 4) == WHITE, "palette 0 shows every non-zero value white");
    {
        unsigned lit = 0;

        for (int i = 0; i < CF_FRAME_W * CF_FRAME_H; i++)
            lit += fb[i] != BLACK;
        check(lit == 4 * 2 * 4, "cells outside columns 4-105 and rows 4-61 are not drawn: only the four corner cells (8 pixels each) are lit");
    }
    video_init(&v);
    plot(40, 125, 2);
    plot(40, 50, 1);
    plot(40, 51, 2);
    plot(40, 52, 3);
    video_render(&v, fb);
    check(pixel(40, 49) == LTBLUE && pixel(40, 50) == BLUE && pixel(40, 51) == RED && pixel(40, 52) == GREEN,
          "a row with a palette: light blue ground, colours 1-3 blue, red, green");
    check(pixel(39, 49) == BLACK, "the next row up is still palette 0");

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
