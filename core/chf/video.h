/* video.h -- the Channel F's video: a 128 x 64 VRAM of 2-bit values and the
 * four port latches that write it.
 *
 * Transposed from MAME's src/mame/fairchild/channelf.cpp (the port handlers)
 * and channelf_v.cpp (GPL-2.0-or-later, copyright-holders Juergen Buchmueller,
 * Frank Palazzolo, Sean Riddle; see COMPLIANCE.md).
 *
 * The CPU cannot read the VRAM. It writes a cell by putting the value on port
 * 1 (bits 7-6, complemented), the column on port 4 (bits 6-0, complemented)
 * and the row on port 5 (bits 5-0, complemented; bits 7-6 are the sound mode),
 * then writing port 0 with bit 5 set: MAME stores the value at every such
 * write (level-triggered; the real BIOS strobes $60 then $50). Port 0 bit 6
 * gates the hand controllers.
 *
 * Each ROW chooses its own palette from the values in columns 125 and 126;
 * the picture is drawn from the whole VRAM at the vertical blank. The TV shows
 * columns 4-105 and rows 4-61, 2 pixels wide and 4 tall (the PAL machines draw
 * the rows 5 tall on the TV; the published frame is always 2 x 4).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CHF_VIDEO_H
#define CHF_VIDEO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CF_VRAM_COLS   128
#define CF_VRAM_ROWS   64
#define CF_VRAM_SIZE   (CF_VRAM_COLS * CF_VRAM_ROWS)
#define CF_VIS_COL0    4          /* the visible area: columns 4-105 */
#define CF_VIS_COLS    102
#define CF_VIS_ROW0    4          /* rows 4-61 */
#define CF_VIS_ROWS    58
#define CF_PAL_COL_A   125        /* the palette columns */
#define CF_PAL_COL_B   126
#define CF_CELL_W      2          /* the published frame's cell */
#define CF_CELL_H      4
#define CF_FRAME_W     (CF_VIS_COLS * CF_CELL_W)       /* 204 */
#define CF_FRAME_H     (CF_VIS_ROWS * CF_CELL_H)       /* 232 */

typedef struct {
    uint8_t vram[CF_VRAM_SIZE];   /* value (0-3) at row * 128 + col */
    uint8_t latch[4];             /* ports 0, 1, 4, 5 as last written (MAME's m_latch[0..3]) */
    uint8_t val_reg;              /* ((port1 ^ $FF) >> 6) & 3 */
    uint8_t col_reg;              /* (port4 | $80) ^ $FF */
    uint8_t row_reg;              /* (port5 | $C0) ^ $FF */
} cf_video_t;

/* Power on: the VRAM clear (ROMREGION_ERASE00), every latch and register 0. */
void video_init(cf_video_t *v);

/* A write to port 0, 1, 4 or 5 (any other port: ignored). Port 0 with bit 5
 * set stores val_reg at row_reg * 128 + col_reg and returns that offset + 1;
 * every other write returns 0. */
unsigned video_write(cf_video_t *v, unsigned port, uint8_t data);

/* A read of port 0, 1, 4 or 5 (MAME's port_N_r: ~external | latch). The
 * external values are the live console inputs: panel = the four console
 * buttons (bit 0-3, 1 = pressed), right / left = the hand controllers' contacts
 * (1 = held). Port 1 and 4 read the controllers only while port 0 bit 6 is
 * clear. Any other port reads 0. */
uint8_t video_read(const cf_video_t *v, unsigned port, unsigned panel, unsigned right, unsigned left);

/* The palette (0-3) a row chooses: (v[126] & 2) | (v[125] >> 1 & 1). */
unsigned video_row_palette(const cf_video_t *v, unsigned row);
/* The pen (0-7) a value (0-3) shows as in a palette, and a pen's colour. */
unsigned video_palette_pen(unsigned palette, unsigned value);
uint32_t video_pen_rgb(unsigned pen);

/* Draw the published frame (CF_FRAME_W x CF_FRAME_H, 0x00RRGGBB) from the
 * VRAM as it stands. */
void video_render(const cf_video_t *v, uint32_t *dst);

#ifdef __cplusplus
}
#endif

#endif /* CHF_VIDEO_H */
