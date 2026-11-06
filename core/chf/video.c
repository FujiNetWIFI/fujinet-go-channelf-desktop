/* video.c -- the Channel F's VRAM, its ports and its picture. See video.h.
 *
 * Transposed from MAME's channelf.cpp (port_0_r .. port_5_w) and
 * channelf_v.cpp (GPL-2.0-or-later, copyright-holders Juergen Buchmueller,
 * Frank Palazzolo, Sean Riddle).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "video.h"

#include <string.h>

/* channelf_pens */
static const uint32_t k_pens[8] = {
    0x101010,   /* black */
    0xfdfdfd,   /* white */
    0xff3153,   /* red */
    0x02cc5d,   /* green */
    0x4b3ff3,   /* blue */
    0xe0e0e0,   /* light grey */
    0x91ffa6,   /* light green */
    0xced0ff,   /* light blue */
};

enum { BLACK, WHITE, RED, GREEN, BLUE, LTGRAY, LTGREEN, LTBLUE };

/* colormap: four palettes of four values */
static const uint8_t k_colormap[16] = {
    BLACK,   WHITE, WHITE, WHITE,
    LTBLUE,  BLUE,  RED,   GREEN,
    LTGRAY,  BLUE,  RED,   GREEN,
    LTGREEN, BLUE,  RED,   GREEN,
};

void video_init(cf_video_t *v)
{
    memset(v, 0, sizeof *v);
}

unsigned video_write(cf_video_t *v, unsigned port, uint8_t data)
{
    switch (port) {
    case 0:
        v->latch[0] = data;
        if (data & 0x20) {
            const unsigned offs = (unsigned)v->row_reg * CF_VRAM_COLS + v->col_reg;

            v->vram[offs] = v->val_reg;
            return offs + 1;
        }
        break;
    case 1:
        v->latch[1] = data;
        v->val_reg = (uint8_t)(((data ^ 0xff) >> 6) & 0x03);
        break;
    case 4:
        v->latch[2] = data;
        v->col_reg = (uint8_t)((data | 0x80) ^ 0xff);
        break;
    case 5:
        v->latch[3] = data;
        v->row_reg = (uint8_t)((data | 0xc0) ^ 0xff);
        break;
    }
    return 0;
}

/* port_read_with_latch */
static uint8_t with_latch(unsigned ext, uint8_t latch)
{
    return (uint8_t)(~ext | latch);
}

uint8_t video_read(const cf_video_t *v, unsigned port, unsigned panel, unsigned right, unsigned left)
{
    /* the PANEL port has bits 7-4 unused and active low, so they read 1 */
    switch (port) {
    case 0:
        return with_latch(0xf0u | (panel & 0x0f), v->latch[0]);
    case 1:
        return with_latch((v->latch[0] & 0x40) == 0 ? (right & 0xff) : (0xc0u | (right & 0xff)),
                          v->latch[1]);
    case 4:
        return with_latch((v->latch[0] & 0x40) == 0 ? (left & 0xff) : 0xffu, v->latch[2]);
    case 5:
        return with_latch(0xff, v->latch[3]);
    }
    return 0;
}

unsigned video_row_palette(const cf_video_t *v, unsigned row)
{
    const unsigned reg1 = v->vram[row * CF_VRAM_COLS + CF_PAL_COL_A] & 3;
    const unsigned reg2 = v->vram[row * CF_VRAM_COLS + CF_PAL_COL_B] & 3;

    return (reg2 & 0x2) | (reg1 >> 1);
}

unsigned video_palette_pen(unsigned palette, unsigned value)
{
    return k_colormap[((palette & 3) << 2) + (value & 3)];
}

uint32_t video_pen_rgb(unsigned pen)
{
    return k_pens[pen & 7];
}

void video_render(const cf_video_t *v, uint32_t *dst)
{
    for (unsigned r = 0; r < CF_VIS_ROWS; r++) {
        const unsigned row = CF_VIS_ROW0 + r;
        const unsigned pal = video_row_palette(v, row);
        uint32_t *line = dst + (size_t)r * CF_CELL_H * CF_FRAME_W;

        for (unsigned c = 0; c < CF_VIS_COLS; c++) {
            const uint32_t rgb = k_pens[video_palette_pen(pal, v->vram[row * CF_VRAM_COLS + CF_VIS_COL0 + c])];

            line[c * CF_CELL_W] = rgb;
            line[c * CF_CELL_W + 1] = rgb;
        }
        for (unsigned y = 1; y < CF_CELL_H; y++)
            memcpy(line + (size_t)y * CF_FRAME_W, line, CF_FRAME_W * sizeof *line);
    }
}
