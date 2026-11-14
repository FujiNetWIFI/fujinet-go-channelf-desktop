/*
 * chfvidtab -- the Video tab's controller. All four frontends draw the same
 * tab from it, so what they own is widgets: a view and zoom chooser, overlay
 * toggles, a picture, the inspector and five tables. It sits over the
 * engine's public video API (chfdebug.h: chfdebug_video_view and friends), so
 * it needs no hook of its own:
 *
 *   - the machine is only READ, under the host lock, a call at a time. The
 *     picture is rebuilt only when something it shows changed, into one reused
 *     buffer, on the caller's (UI) thread;
 *   - hover and pin are state here (a VRAM cell each), so a mouse move never
 *     re-renders the picture: draw the marks from chfvid_tab_marks() over the
 *     cached image. A pinned cell is re-read at every refresh;
 *   - pixels are 0xFFRRGGBB (opaque), in the machine's real pens;
 *   - one table of overlays and one env-var parser, so the frontends cannot
 *     disagree.
 *
 * The Channel F has one 128 x 64 VRAM of 2-bit values that the CPU fills a
 * cell at a time through ports 1, 4 and 5 and a strobe on port 0; the
 * picture is drawn from the WHOLE VRAM at the vertical blank, and each of the
 * 64 rows picks its palette from the values in columns 125 and 126. Every
 * per-row fact below is therefore a VRAM row (scanline = row * lines per
 * row), and every per-cell fact is the machine's own tracking of the cell's
 * writes.
 *
 * Views: Display is the frame the console last finished (the 102 x 58 visible
 * cells); Decoded is the whole VRAM, hidden columns and rows hatched and every
 * cell coloured by its row's palette; Beam is what a CRT shows, the rows the
 * beam has passed this frame as they were when it drew them and the rest from
 * the last frame, dimmed; Values is the whole VRAM's raw 0-3 values as flat
 * false colours; Live is the VRAM as it is now, drawn the way Display is. All
 * of them work running or stopped.
 *
 * UI thread only. Not thread-safe. chfvid_tab_text() builds the tables
 * without a picture, so a console command may create a tab, ask for the text
 * and free it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef CHFVIDTAB_H
#define CHFVIDTAB_H

#include <stdint.h>

#include "chfdebug.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct chfvid_tab chfvid_tab;

enum chfvid_view {
    CHFVID_V_DISPLAY = 0,   /* the last finished frame, as the display shows it (204 x 232) */
    CHFVID_V_DECODED,       /* the whole VRAM (256 x 256), hatched outside the visible area, by row palette */
    CHFVID_V_BEAM,          /* the frame as the beam draws it, the rest last frame's, dimmed (204 x 232) */
    CHFVID_V_VALUES,        /* the whole VRAM's raw values, one false colour each (256 x 256) */
    CHFVID_V_LIVE,          /* the VRAM as it is now, drawn like Display (204 x 232) */
    CHFVID_V_COUNT
};

/* Overlays: one table, a bit per entry (the engine's CHFDEBUG_F_* set). */
enum {
    CHFVID_OV_VISIBLE = 1u << 0,   /* the visible area's outline (Decoded, Values) */
    CHFVID_OV_GRID    = 1u << 1,   /* cell borders (zoom 2 and up) */
    CHFVID_OV_PALCOLS = 1u << 2,   /* mark columns 125-126, the palette columns (Decoded, Values) */
    CHFVID_OV_BEAM    = 1u << 3,   /* the beam's line */
    CHFVID_OV_CURSOR  = 1u << 4,   /* the cell the next strobe writes (ports 4 and 5) */
    CHFVID_OV_HEAT    = 1u << 5,   /* cells written this frame, brighter the more */
    CHFVID_OV_DIRTY   = 1u << 6,   /* cells that changed since the last frame */
    CHFVID_OV_PALROWS = 1u << 7,   /* a tick at the left of rows whose palette changed this frame */
    CHFVID_OV_ALL     = 0xFFu
};
typedef struct {
    unsigned bit;
    const char *name;           /* the env name: visible, grid, ... */
    const char *label;          /* the checkbox text */
} chfvid_overlay_info;
const chfvid_overlay_info *chfvid_overlays(int *count);
const char *chfvid_view_name(int view);            /* "Display", ... */
const char *chfvid_view_key(int view);             /* "display", ... (env) */

/* Right-hand sections. */
enum { CHFVID_S_PIXEL = 0, CHFVID_S_REGISTERS, CHFVID_S_COLOURS, CHFVID_S_LINES,
       CHFVID_S_WRITES, CHFVID_S_TIMING, CHFVID_S_COUNT };
const char *chfvid_section_name(int section);
const char *chfvid_section_key(int section);
/* A section by name or prefix ("pixel", "reg", ...); -1 if none. */
int  chfvid_section_from_name(const char *name);

/* d is the debugger the tab adds the Break on writes breakpoint to and reads
 * the port log from; NULL makes Break on writes return -1 and the Registers'
 * last-write column "never" (the picture and tables still read the machine).
 * The picture and the tables are allocated on first use. */
chfvid_tab *chfvid_tab_new(chfdebug *dbg);
void        chfvid_tab_free(chfvid_tab *tab);

/* ---- what to show (they mark the picture stale) ------------------------------------ */
void     chfvid_tab_set_view(chfvid_tab *tab, int view);
int      chfvid_tab_view(const chfvid_tab *tab);
void     chfvid_tab_set_overlays(chfvid_tab *tab, unsigned ovl);
unsigned chfvid_tab_overlays(const chfvid_tab *tab);
/* 1, 2, 3, 4, 6, 8, 12, 16, or 0 for "fit the viewport" (the largest of
 * those that fits both ways). A zoom whose picture would pass 17 million
 * pixels is lowered to the largest that does not. */
void     chfvid_tab_set_zoom(chfvid_tab *tab, int zoom);
int      chfvid_tab_zoom(const chfvid_tab *tab);
/* the room the picture has, for Fit; call when the widget resizes */
void     chfvid_tab_set_viewport(chfvid_tab *tab, int wide, int high);
/* the zoom steps the chooser offers (0 = Fit first) */
const int *chfvid_zooms(int *count);

/*
 * The scripted-capture variables, read once by chfvid_tab_new:
 *   CHF_DEBUGGER_VIDEO_VIEW      display decoded beam values live   (a prefix is enough)
 *   CHF_DEBUGGER_VIDEO_OVERLAYS  visible,grid,palcols,beam,cursor,heat,dirty,palrows | all | none
 *   CHF_DEBUGGER_VIDEO_ZOOM      1..16 or fit
 *   CHF_DEBUGGER_VIDEO_SECTION   pixel registers colours lines writes timing   (names, never indexes)
 * Returns the starting section, CHFVID_S_PIXEL when unset.
 */
int chfvid_tab_env_section(const chfvid_tab *tab);

/* ---- refresh ------------------------------------------------------------------------ */
#define CHFVID_TAB_PICTURE 1u   /* repaint the picture (chfvid_tab_picture) */
#define CHFVID_TAB_TABLES  2u   /* refill the status line and the sections */
#define CHFVID_TAB_OFF     4u   /* no machine: show chfvid_tab_why() instead of a picture */

/* Take the machine's state, update the trackers, and say what to redraw.
 * force: redraw everything. Works running (a changing frame, ports and VRAM
 * repaint it) and stopped. */
unsigned chfvid_tab_refresh(chfvid_tab *tab, int force);

/* ---- the picture ------------------------------------------------------------------- */
/*
 * The rendered picture, 0xFFRRGGBB, valid until the next refresh or set_*.
 * NULL when there is none (no machine): chfvid_tab_why() says why, and the tab
 * should show that text.
 */
const uint32_t *chfvid_tab_picture(chfvid_tab *tab, int *wide, int *high);
const char     *chfvid_tab_why(const chfvid_tab *tab);
int             chfvid_tab_effective_zoom(const chfvid_tab *tab);

/* ---- the tables -------------------------------------------------------------------- */
const char *chfvid_tab_status(const chfvid_tab *tab);        /* two lines, '\n' between */

/* Registers: the four output latches (ports 0, 1, 4, 5) raw, complemented and
 * decoded, then the cell the next strobe writes and the sound mode. */
typedef struct {
    const char *name;           /* "PORT 0", ..., "CELL", "SOUND" */
    int port;                   /* 0, 1, 4, 5, or -1 for the derived rows */
    uint8_t raw, comp;          /* the latch, and ~latch (port rows) */
    char value[24];             /* "$50" / "row 12 col 40 = 1" / "1 kHz" */
    char meaning[128];
    char last[56];              /* "frame 12 pc $0834" -- the newest OUT to it, "never" */
    int age;                    /* refreshes since it last changed (CHFVID_REG_FADE+: not recent) */
} chfvid_reg;
#define CHFVID_REG_FADE 12
int chfvid_tab_regs(const chfvid_tab *tab, const chfvid_reg **out);

/* Colours: the four palettes, four values each (16 entries, palette-major). */
typedef struct {
    int palette;                /* 0-3 */
    const char *palette_name;   /* "black & white", ... */
    int value;                  /* 0-3 */
    int pen;                    /* 0-7 */
    const char *name;           /* the colour: "light blue" */
    uint32_t rgb;               /* 0xFFRRGGBB */
    int rows;                   /* rows using the palette (all four values of it share it) */
    long cells;                 /* cells of those rows holding this value */
    int inspected;              /* the inspected cell has this palette and value */
} chfvid_swatch;
int chfvid_tab_swatches(const chfvid_tab *tab, const chfvid_swatch **out);

/* Lines: the 64 VRAM rows. */
typedef struct {
    int row;                    /* 0-63 */
    int scanline;               /* the beam line the row starts at: row * lines per row */
    int rows_lines;             /* lines per row (4 NTSC, 5 PAL) */
    int visible;                /* inside rows 4-61 */
    int palette;                /* from columns 125 and 126 */
    const char *palette_name;
    uint32_t background;        /* 0xFFRRGGBB, value 0 of that palette */
    int writes;                 /* strobes into the row this frame so far (the log's tail) */
    int pal_writes;             /* ... of those, into the palette columns */
    int last_pc;                /* the newest writer found in the log, -1 none */
    int last_col;
    uint32_t last_frame;
    int beam;                   /* the beam is drawing this row now */
} chfvid_line_row;
int chfvid_tab_lines(const chfvid_tab *tab, const chfvid_line_row **out);

/* Writes: the VRAM write log, filtered, oldest first (the newest
 * CHFVID_WRITES_MAX that match). The log holds strobes while the debugger is
 * attached. */
#define CHFVID_WRITES_MAX 1000
typedef struct {
    int this_frame;             /* only this frame's */
    int changes_only;           /* only writes that changed the cell */
    int palette_cols;           /* only columns 125-127 */
    int inspected_cell;         /* only the inspected cell's */
    int inspected_row;          /* only the inspected cell's row */
} chfvid_write_filter;
void chfvid_tab_set_write_filter(chfvid_tab *tab, const chfvid_write_filter *filter);
void chfvid_tab_write_filter(const chfvid_tab *tab, chfvid_write_filter *out);
int  chfvid_tab_writes(const chfvid_tab *tab, const chfdebug_vram_write **out);
/* One write as a table line: "frame 12 line 48 pc $0834  row 12 col 40 = 1 (was 2)". */
int  chfvid_write_text(const chfdebug_vram_write *write, char *dst, int dstsz);
/* Forget the log (the engine's own clear). */
void chfvid_tab_clear_writes(chfvid_tab *tab);

/* the timing section, as ready-made "label\tvalue" lines */
int  chfvid_tab_timing(const chfvid_tab *tab, const char *const **lines);
/* where the beam is in the whole frame, 0..1000 (0 when unknown) */
int  chfvid_tab_beam_permille(const chfvid_tab *tab);

/* A section as plain text (the console's `video [section]` shares it). The
 * Pixel text describes the inspected cell, or the one the next strobe writes
 * when none is. Refreshes the tables itself, so a fresh tab works. */
int  chfvid_tab_text(chfvid_tab *tab, int section, char *dst, int dstsz);

/* ---- hover, pin ---------------------------------------------------------------------- */
typedef struct {
    int valid;
    int row, col;               /* the VRAM cell */
    int visible;                /* inside columns 4-105 and rows 4-61 */
    const char *hidden_why;     /* "palette column", "left border", ... or NULL */
    int value;                  /* 0-3 */
    int palette;                /* the row's */
    const char *palette_name;
    int pen;                    /* 0-7 */
    const char *colour;         /* the pen's name */
    uint32_t rgb;               /* 0xFFRRGGBB */
    int scanline;               /* the beam line the row starts at */
    uint32_t writes;            /* strobes that hit the cell since the debugger attached */
    int last_pc;                /* the last writer's PC, -1 if never written */
    uint32_t last_frame;
    int last_line;
    uint64_t last_cycle;
    int last_old;               /* the value before that write */
    char text[512];             /* ready-made inspector text */
} chfvid_pixel;

/* x, y in picture pixels at the current zoom; a point that is not on a cell
 * (or -1,-1: the pointer left) clears the hover. Returns 1 when the
 * inspector changed. */
int  chfvid_tab_hover(chfvid_tab *tab, int posx, int posy);
void chfvid_tab_pin(chfvid_tab *tab, int posx, int posy);      /* a click; the same cell unpins */
void chfvid_tab_unpin(chfvid_tab *tab);
/* inspect a cell directly (the Writes and Lines tables' rows); hover or pin */
void chfvid_tab_pin_cell(chfvid_tab *tab, int row, int col);
/* the pinned cell if any, else the hovered one, else NULL */
const chfvid_pixel *chfvid_tab_pixel(const chfvid_tab *tab);
/* rectangles x,y,w,h in picture pixels for the marks drawn over the picture;
 * bit0 of the result: hover mark valid, bit1: pin mark valid (a cell the view
 * does not show has no mark) */
int  chfvid_tab_marks(const chfvid_tab *tab, int hover[4], int pin[4]);
/* "Break on writes": a VRAM breakpoint on the inspected cell (CHFDEBUG_BP_VRAM,
 * one cell). Not added twice. The breakpoint's id, or -1 (no cell, no debugger). */
int  chfvid_tab_break_on_pixel(chfvid_tab *tab);
/* 1 when "Break on writes" has a cell to break on now */
int  chfvid_tab_can_break(const chfvid_tab *tab);

/* ---- a read-only accessor over the machine's geometry (vidview.c) ------------------- */
/* Lines per VRAM row (4 NTSC, 5 PAL), the line the vertical blank begins at
 * and the one it ends at. Returns 0 when no machine runs. */
int  chfdebug_video_geometry(chfdebug *d, int *lines_per_row, int *vbstart, int *vbend);

#ifdef __cplusplus
}
#endif

#endif
