/*
 * What the debugger window's two files share: the window's state and the
 * views that dbg_views.c draws by hand. Not installed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <windows.h>

#include <stdint.h>

#include "chfdebug.h"
#include "../win_common.h"

#define DISASM_MAX 128       /* rows fetched at most; the view shows what fits */
#define MAX_BPS 128
#define HISTORY_MAX 64
#define MAX_CTL 320
#define WRITES_MAX 600       /* VRAM writes listed */
#define EVENTS_MAX 400       /* sound, I/O and cartridge events listed */

enum {
    PAGE_PROMPT = 0, PAGE_CPU, PAGE_DISASM, PAGE_VIDEO, PAGE_IO, PAGE_BREAKS, PAGE_CART,
    PAGE_FUJINET, PAGE_COUNT
};

/* The Video tab's sections (the strip beside the picture). */
enum { SEC_CELL = 0, SEC_ROWS, SEC_PORTS, SEC_WRITES, SEC_COUNT };

#define NFIELDS 12           /* PC PC0 PC1 DC0 DC1 A W ISAR J H K Q */
#define NFLAGS 5             /* S C Z O ICB */
#define NSPACES 5            /* PROG SCRATCH VRAM CART IO */
#define NVIEWS 6             /* FRAME LIVE BEAM VRAM VALUES PALETTES */
#define NOVERLAYS 8          /* the overlay flags */
#define NSCALES 5            /* Fit 1x 2x 3x 4x */
#define MEM_ROWS 16          /* rows of the memory view (16 bytes each) */
#define NBPTYPES 7           /* the breakpoint types */
#define NLAMPS 8             /* a controller's contacts */

/* An XRGB picture the engine drew (0x00RRGGBB, top-down: a 32-bit BI_RGB DIB as it is). */
typedef struct {
    uint32_t *px;
    size_t cap;
    int w, h;
} image;

/* Every control the window owns, with the page (and section) it belongs to,
 * so a page change is one loop and a DPI change remakes every font. */
typedef struct {
    HWND h;
    int page;
    int sec;                  /* -1: the whole page; else the Video section */
    int mono;
} ctl_t;

typedef struct {
    HWND hwnd;
    HWND tabs;
    HINSTANCE inst;
    HWND run_btn, step_btn, over_btn, out_btn, scan_btn, frame_btn, torun_btn, load_symbols, save, help_btn, status;

    HWND prompt_out, prompt_in;

    HWND fld_label[NFIELDS], fld[NFIELDS], flag[NFLAGS], cpu_state;
    HWND scr_head, scr_grid, scr_reg_label, scr_reg, scr_val_label, scr_val;
    HWND kstack_head, kstack;
    HWND mem_label, mem_view, ram_space, ram_goto_label, ram_goto, ram_prev, ram_next;
    HWND ram_addr_label, ram_addr, ram_val_label, ram_val;
    uint32_t mem_base;       /* the memory view's first address */

    HWND follow_pc, jump_label, jump, disasm_hint, disasm;

    HWND view_label, view_mode, scale_label, scale_mode, overlays_label, overlay[NOVERLAYS];
    HWND vpic, vstatus, vtabs;
    HWND hover_head, hover_text, pin_head, pin_text, pin_break, pin_clear;
    HWND rows_head, rows_view, pal_head, pal_text;
    HWND ports_text;
    HWND wr_row_label, wr_row0, wr_row1, wr_col_label, wr_col0, wr_col1, wr_changes, wr_frame, wr_palcols,
         wr_clear, wr_counts, wr_text;

    HWND io_draw, io_state, snd_head, snd_log, iolog_head, io_log, io_clear;

    HWND bp_list, bp_type, bp_start_label, bp_start, bp_end_label, bp_end, bp_cond_label, bp_cond, bp_hint;
    HWND bp_add, bp_remove, bp_enable, bp_clear;

    HWND cart_text, cart_mailbox_label, cart_mailbox, cart_ev_label, cart_events;

    HWND fuji_title, fuji_note, fuji_clear, fuji_text, state_title, fuji_state;
    uint64_t fuji_last;
    int fuji_count;

    ctl_t ctl[MAX_CTL];
    int nctl;

    HFONT ui, bold, mono;
    int dpi;
    int ui_h;                 /* the UI font's line height */
    int row_h;                /* an edit / button / combo row */
    int mono_w, mono_h;       /* the mono font's cell, for the hand-drawn views */
    HACCEL accel;
    HBRUSH accent, accent_dark;

    chfsession *session;
    chfdebug *dbg;

    int page, vsec;
    unsigned seen_gen;
    int was_stopped;
    int running_ticks;

    /* the disassembly */
    int disasm_top;
    chfdebug_line lines[DISASM_MAX];
    int line_count;
    int pc_line;
    int sel_addr;             /* the line selected for Run to Cursor, -1 none */

    /* breakpoints */
    chfdebug_breakpoint bps[MAX_BPS];
    int nbps;

    /* the CPU, the I/O and the cartridge as last read */
    chfdebug_cpu cpu;
    uint8_t prev_r[64];       /* the scratchpad as of the refresh before: changed cells show */
    int prev_valid;
    uint64_t changed_mask;    /* scratchpad cells that changed since the registers last differed (a bit each) */
    chfdebug_io io;
    chfdebug_cart cart_state;

    /* the prompt's history */
    char history[HISTORY_MAX][256];
    int nhistory, history_pos;

    /* the video */
    chfdebug_video vid;
    image vimg;               /* the Video tab's picture at the scale shown */
    int vimg_view, vimg_scale;/* what vimg holds: the view and the integer scale */
    int hover_row, hover_col; /* the cell under the pointer, -1 none */
    int pin_row, pin_col;     /* the pinned cell, -1 none */
    chfdebug_vram_write writes[WRITES_MAX];
    int nwrites;
    uint32_t pal_changed[CHFDEBUG_VRAM_ROWS / 32 + 1];   /* rows whose palette changed (bits) */
    uint8_t prev_row_pal[CHFDEBUG_VRAM_ROWS];
    int prev_row_valid;
    uint32_t pal_changed_frame;

    /* sound and I/O */
    chfdebug_sound_event snd_events[EVENTS_MAX];
    int nsnd;
    chfdebug_io_event io_events[EVENTS_MAX];
    int nio;
    chfdebug_cart_event cart_events_buf[EVENTS_MAX];
    int ncart_ev;
} debugger;

/* The one window, for the views' window procs. */
extern debugger *g_debugger;

/* Lays out in 96-dpi units scaled by the window's DPI. */
#define S(v) chf_scale((v), d->dpi)

/* In dbg_window.c, for the views. */
void dbg_wide_from(const char *utf8, wchar_t *w, int n);
void dbg_label_set(HWND h, const char *utf8);
void dbg_refresh_disasm(debugger *d);
void dbg_hover_changed(debugger *d, int row, int col);
void dbg_pin_cell(debugger *d, int row, int col);
void dbg_break_on_cell(debugger *d, int row, int col);
void dbg_scratch_pick(debugger *d, int reg);

/* In dbg_views.c, for the window. */
void dbg_views_register(HINSTANCE inst);
void dbg_io_draw_size(const debugger *d, int *w, int *h);
void dbg_scratch_size(const debugger *d, int *w, int *h);
void dbg_rows_size(const debugger *d, int *w, int *h);
/* The octal names of the scratchpad registers the BIOS gives jobs. */
const char *dbg_reg_name(int reg);
