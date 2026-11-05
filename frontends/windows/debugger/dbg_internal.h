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
#define MAX_CTL 256
#define OBJ_COUNT 4          /* the UVI's four objects */
#define OBJ_BM_PX (8 * 16 * 4)  /* the biggest object bitmap (8 x 16 at scale 2) */
#define UVI_REGS 0x300       /* the register file, CPU $1800-$1AFF */

enum {
    PAGE_PROMPT = 0, PAGE_CPU, PAGE_DISASM, PAGE_VIDEO, PAGE_IO, PAGE_BREAKS, PAGE_CART,
    PAGE_FUJINET, PAGE_COUNT
};

/* The Video tab's sections (the strip beside the picture). */
enum { SEC_OBJECTS = 0, SEC_SCREEN, SEC_REGS, SEC_STATUS, SEC_SHEETS, SEC_COUNT };

#define NFIELDS 12           /* R0 R1 R2 R3 R1' R2' R3' IAR PSU PSL SP CC */
#define NFLAGS 8             /* F II C COM OVF WC IDC RS: editable */
#define NINPUTS 3            /* SENSE, halted, IRQ: read-only */
#define NSTACK 8             /* the return address stack */
#define NLAYERS 2
#define NOVERLAYS 4
#define NSAVE 7
#define NSHEETS 3
#define NSPACES 3            /* PROG, UVI, CART */
#define MEM_ROWS 16          /* rows of the memory view (16 bytes each) */

/* An XRGB picture the engine drew (0x00RRGGBB, top-down: a 32-bit BI_RGB DIB as it is). */
typedef struct {
    uint32_t *px;
    size_t cap;
    int w, h;
} image;

/* What a pixel window (ChfDbgPic) shows; its GWLP_USERDATA points at one. */
typedef struct {
    image img;
    int center;               /* centred and as large as fits, else top-left at 1:1 */
} picview;

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
    HWND run_btn, step_btn, over_btn, out_btn, scan_btn, frame_btn, load_symbols, save, status;

    HWND prompt_out, prompt_in;

    HWND fld_label[NFIELDS], fld[NFIELDS], flag[NFLAGS], input[NINPUTS];
    HWND stack_head, stack_label[NSTACK], stack_edit[NSTACK], cpu_state;
    HWND mem_label, mem_view, ram_space, ram_goto_label, ram_goto, ram_prev, ram_next;
    HWND ram_addr_label, ram_addr, ram_val_label, ram_val;
    uint32_t mem_base;       /* the memory view's first address */

    HWND follow_pc, jump_label, jump, disasm_hint, disasm;

    HWND view_label, view_mode, layers_label, layer[NLAYERS], overlays_label, overlay[NOVERLAYS];
    HWND vpic, vstatus, vtabs;
    HWND objects, screen, screen_status, regs_list, reg_addr_label, reg_addr, reg_val_label, reg_val, reg_write;
    HWND status_text, sheet_label[NSHEETS], sheet[NSHEETS];

    HWND io_text, io_matrix;

    HWND bp_list, bp_type, bp_start_label, bp_start, bp_end_label, bp_end, bp_cond_label, bp_cond;
    HWND bp_add, bp_remove, bp_enable, bp_clear;

    HWND cart_text, cart_mailbox_label, cart_mailbox;

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

    /* breakpoints */
    chfdebug_breakpoint bps[MAX_BPS];
    int nbps;

    /* the CPU, the I/O and the cartridge as last read */
    chfdebug_cpu cpu;
    chfdebug_io io;
    chfdebug_cart cart_state;

    /* the prompt's history */
    char history[HISTORY_MAX][256];
    int nhistory, history_pos;

    /* the 2637 UVI */
    chfdebug_uvi uvi;
    image vimg;               /* the Video tab's picture (scale 1) */
    uint32_t pal[8];          /* the 8 colours, sampled from the palette strip */
    image palimg;
    uint32_t obj_bm[OBJ_COUNT][OBJ_BM_PX];
    int obj_w[OBJ_COUNT], obj_h[OBJ_COUNT];
    uint32_t glyph[64][64];   /* the glyph table, 8 x 8 each, the bg sentinel where clear */
    int glyph_ok;
    chfdebug_uvi_cell cells[CHFDEBUG_UVI_COLS * CHFDEBUG_UVI_MAX_ROWS];
    int cell_rows;
    int hover_valid, hover_x, hover_y;
    chfdebug_hit hits[8];
    int nhits;
    char reg_name[UVI_REGS][32];
    char reg_text[UVI_REGS][112];
    picview sheet_pv[NSHEETS];
    int sheet_scale[NSHEETS];
    int screen_hover;         /* the cell under the pointer, row * 16 + col, or -1 */

} debugger;

/* The one window, for the views' window procs. */
extern debugger *g_debugger;

/* Lays out in 96-dpi units scaled by the window's DPI. */
#define S(v) chf_scale((v), d->dpi)

/* The colour a glyph's clear pixels come back in (chfdebug_uvi_thing's bg). */
#define GLYPH_CLEAR 0xFF123456u

/* In dbg_window.c, for the views. */
void dbg_wide_from(const char *utf8, wchar_t *w, int n);
void dbg_label_set(HWND h, const char *utf8);
void dbg_refresh_disasm(debugger *d);
void dbg_update_hover(debugger *d);

/* In dbg_views.c, for the window. */
void dbg_views_register(HINSTANCE inst);
void dbg_screen_cell(const debugger *d, int *cw, int *ch);
int dbg_screen_cell_text(const debugger *d, int idx, char *out, int n);
void dbg_matrix_size(const debugger *d, int *w, int *h);
