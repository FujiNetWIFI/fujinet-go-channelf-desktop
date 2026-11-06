/*
 * Debugger window (Win32) over the Fairchild F8 / Channel F video debugger
 * engine, via core/include/chfdebug.h. Mirrors the GTK and Qt debuggers tab
 * for tab -- Prompt, CPU & RAM, Disassembly, Video, Sound & I/O, Breakpoints,
 * Cart, FujiNet -- built from plain common controls, with the disassembly,
 * the video picture, the scratchpad grid, the row palette strip and the
 * controllers' lamps drawn by hand. A Help button (and F1) opens the Help
 * topic of the tab showing.
 *
 * The Video tab is the showpiece: the six views of the 128 x 64 video RAM
 * (the frame the display shows, the RAM live, the beam's CRT, the whole RAM,
 * its raw values, the palettes) at a chosen scale with the overlays, a hover
 * that names the cell under the pointer and its last writer, a click that
 * pins it and a double click that breaks on writes to it; beside it the 64 row
 * palettes, the four palettes, the four ports as raw, complement and decoded,
 * and the log of VRAM writes.
 *
 * The engine only exists while this window is showing: showing it attaches
 * (which stops the machine, as on every sibling) and hiding it detaches,
 * which lets the machine run on at full speed. The window refreshes on a
 * timer keyed to the engine's generation counter rather than on every tick,
 * only the tab that is showing, and the timer exists only while the window
 * does.
 *
 * Everything is laid out in 96-dpi units scaled by the window's DPI (the
 * manifest makes the process per-monitor aware), and every control is a
 * Unicode window so the engine's UTF-8 reaches the screen as it is.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../help/help_window.h"
#include "dbg_internal.h"

#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif

#define TRACE_MAX 512        /* FujiNet packets shown */
#define MAILBOX_BASE 0xF800  /* the cartridge's mailbox page, as the console reads it */
#define MAILBOX_SIZE 0x800

#define WM_DBG_ACCEPT  (WM_APP + 2)   /* wp: control id -- Enter in an edit */
#define WM_DBG_TAB     (WM_APP + 3)   /* Tab in the prompt: complete */
#define WM_DBG_HISTORY (WM_APP + 4)   /* wp: 0 Up (older), 1 Down (newer) in the prompt */

#define TIMER_REFRESH 1
#define TICKS_SLOW 5                  /* running: the slow pages every 5th 100 ms tick */

#define NSAVE 8

enum {
    IDC_RUN = 1000, IDC_STEP, IDC_OVER, IDC_OUT, IDC_SCAN, IDC_FRAME, IDC_TORUN, IDC_LOAD_SYMBOLS, IDC_SAVE,
    IDC_HELP_BTN, IDC_STATUS, IDC_TABS,
    IDC_PROMPT_OUT, IDC_PROMPT_IN,
    IDC_FIELD0, IDC_FIELD_LAST = IDC_FIELD0 + NFIELDS - 1,
    IDC_FLAG0, IDC_FLAG_LAST = IDC_FLAG0 + NFLAGS - 1,
    IDC_CPU_STATE, IDC_SCR_GRID, IDC_SCR_REG, IDC_SCR_VAL, IDC_KSTACK,
    IDC_MEM_VIEW, IDC_RAM_SPACE, IDC_RAM_GOTO, IDC_RAM_PREV, IDC_RAM_NEXT, IDC_RAM_ADDR, IDC_RAM_VAL,
    IDC_FOLLOW_PC, IDC_JUMP, IDC_DISASM,
    IDC_VIEW_MODE, IDC_SCALE_MODE, IDC_OVL0, IDC_OVL_LAST = IDC_OVL0 + NOVERLAYS - 1,
    IDC_VPIC, IDC_VSTATUS, IDC_VTABS,
    IDC_HOVER_TEXT, IDC_PIN_TEXT, IDC_PIN_BREAK, IDC_PIN_CLEAR, IDC_ROWS_VIEW, IDC_PAL_TEXT, IDC_PORTS_TEXT,
    IDC_WR_ROW0, IDC_WR_ROW1, IDC_WR_COL0, IDC_WR_COL1, IDC_WR_CHANGES, IDC_WR_FRAME, IDC_WR_PALCOLS,
    IDC_WR_CLEAR, IDC_WR_TEXT,
    IDC_IO_DRAW, IDC_IO_STATE, IDC_SND_LOG, IDC_IO_LOG, IDC_IO_CLEAR,
    IDC_BP_LIST, IDC_BP_TYPE, IDC_BP_START, IDC_BP_END, IDC_BP_COND, IDC_BP_ADD,
    IDC_BP_REMOVE, IDC_BP_ENABLE, IDC_BP_CLEAR,
    IDC_CART_TEXT, IDC_CART_MAILBOX, IDC_CART_EVENTS,
    IDC_FUJI_NOTE, IDC_FUJI_CLEAR, IDC_FUJI_TEXT, IDC_FUJI_STATE,
    IDC_SAVE_KIND0, IDC_SAVE_KIND_LAST = IDC_SAVE_KIND0 + NSAVE - 1,
    IDC_SYMS_CART, IDC_SYMS_FILE,
    IDC_LABEL_FIRST = 3000
};

debugger *g_debugger;

static const char *const kSaveKinds[NSAVE] = { "dis", "mailbox", "vram", "scratch", "cart", "screen", "vramshot", "regs" };
static const char *const kSaveTitles[NSAVE] = {
    "Disassembly (PROG space)...", "Mailbox page...", "Video RAM (8192 bytes)...", "Scratchpad (64 bytes)...",
    "Cartridge image...", "Screen (PPM)...", "Video RAM picture (PPM)...", "Registers and ports (text)..." };

/* The editable CPU fields, in the order the CPU page lays them out. */
typedef struct { const char *name; int reg; int digits; int octal; } cpu_field;
static const cpu_field kFields[NFIELDS] = {
    { "PC", CHF_REG_PC, 4, 0 }, { "PC0", CHF_REG_PC0, 4, 0 }, { "PC1", CHF_REG_PC1, 4, 0 },
    { "DC0", CHF_REG_DC0, 4, 0 }, { "DC1", CHF_REG_DC1, 4, 0 }, { "A", CHF_REG_A, 2, 0 },
    { "W", CHF_REG_W, 2, 0 }, { "ISAR", CHF_REG_ISAR, 3, 1 }, { "J", CHF_REG_J, 2, 0 },
    { "H", CHF_REG_H, 4, 0 }, { "K", CHF_REG_K, 4, 0 }, { "Q", CHF_REG_Q, 4, 0 } };
static const char *const kFlagNames[NFLAGS] = { "S (positive)", "C", "Z", "O", "ICB" };
static const int kFlagIds[NFLAGS] = { CHF_REG_SIGN, CHF_REG_CARRY, CHF_REG_ZERO, CHF_REG_OVERFLOW, CHF_REG_ICB };

/* The page names CHF_DEBUGGER_TAB accepts, by page, and the Help contexts. */
static const char *const kPageNames[PAGE_COUNT] = { "prompt", "cpu", "disassembly", "video", "io",
                                                    "breakpoints", "cart", "fujinet" };
static const char *const kHelpContexts[PAGE_COUNT] = {
    "debugger/prompt", "debugger/cpu", "debugger/disassembly", "debugger/video", "debugger/io",
    "debugger/breakpoints", "debugger/cart", "debugger/fujinet" };
static const wchar_t *const kPageTitles[PAGE_COUNT] = {
    L"Prompt", L"CPU && RAM", L"Disassembly", L"Video", L"Sound && I/O", L"Breakpoints", L"Cart",
    L"FujiNet" };
static const wchar_t *const kSectionTitles[SEC_COUNT] = { L"Cell", L"Rows && palettes", L"Ports", L"Writes" };

static const wchar_t *const kSpaceNames[NSPACES] = {
    L"Program (CPU view)", L"Scratchpad", L"Video RAM", L"Cartridge", L"I/O ports" };
static const wchar_t *const kViewNames[NVIEWS] = {
    L"Frame (what the display shows)", L"Live (the video RAM now)", L"Beam (what a CRT shows)",
    L"VRAM (all 128 x 64 cells)", L"Values (raw, false colour)", L"Palettes" };
static const wchar_t *const kScaleNames[NSCALES] = { L"Fit", L"1x", L"2x", L"3x", L"4x" };
static const wchar_t *const kOverlayNames[NOVERLAYS] = {
    L"Visible area", L"Grid", L"Palette columns", L"Beam", L"Write cursor", L"Heat", L"Dirty", L"Palette rows" };
static const wchar_t *const kBpTypes[NBPTYPES] = {
    L"Execute", L"Read", L"Write", L"Port in", L"Port out", L"VRAM write", L"Scratchpad write" };
static const char *const kBpHints[NBPTYPES] = {
    "Address in hex ($0802) or a label; a range runs to the second address.",
    "A data read of PROG space: address in hex ($8000) or a label.",
    "A write to PROG space (the cartridge's RAM): address in hex or a label.",
    "A port read: the port in hex ($01); a range runs to the second port.",
    "A port write: the port in hex ($05); a range runs to the second port.",
    "A write to a cell: row,col in decimal (12,40); a range is the rectangle between two cells.",
    "A write to a scratchpad register: 0-63 in decimal (59), or r59; a range runs to the second." };

/* ---- text helpers ----------------------------------------------------------- */

/* UTF-8 into a fixed wide buffer, always terminated. */
void dbg_wide_from(const char *utf8, wchar_t *w, int n)
{
    w[0] = 0;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8 ? utf8 : "", -1, w, n) <= 0) w[n - 1] = 0;
}

/* EDIT controls want CRLF and render control characters as boxes; the
 * engine's text is UTF-8. Returns a malloc'd wide string, or NULL. */
static wchar_t *view_wide(const char *text)
{
    const size_t n = strlen(text);
    char *buf = malloc(n * 2 + 1);
    wchar_t *w = NULL;
    size_t i, o = 0;
    int wn;
    if (!buf) return NULL;
    for (i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)text[i];
        if (c == '\n') { buf[o++] = '\r'; buf[o++] = '\n'; }
        else if (c >= 0x20 || c == '\t') buf[o++] = (char)c;
    }
    buf[o] = '\0';
    wn = MultiByteToWideChar(CP_UTF8, 0, buf, -1, NULL, 0);
    if (wn > 0 && (w = malloc((size_t)wn * sizeof *w)) != NULL)
        MultiByteToWideChar(CP_UTF8, 0, buf, -1, w, wn);
    free(buf);
    return w;
}

/* Replaces a read-only view's text only when it changed, and keeps the
 * reader where they were (the live views refresh while the machine runs). */
static void view_set(HWND h, const char *text)
{
    wchar_t *w = view_wide(text);
    int len, first;
    if (!w) return;
    len = GetWindowTextLengthW(h);
    if (len == (int)wcslen(w)) {
        wchar_t *cur = malloc(((size_t)len + 1) * sizeof *cur);
        const int same = cur && GetWindowTextW(h, cur, len + 1) == len && wcscmp(cur, w) == 0;
        free(cur);
        if (same) { free(w); return; }
    }
    first = (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    SetWindowTextW(h, w);
    SendMessageW(h, EM_LINESCROLL, 0, first);
    free(w);
}

static void view_append(HWND h, const char *text)
{
    wchar_t *w = view_wide(text);
    int len;
    if (!w) return;
    len = GetWindowTextLengthW(h);
    SendMessageW(h, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(h, EM_REPLACESEL, FALSE, (LPARAM)w);
    SendMessageW(h, EM_SCROLLCARET, 0, 0);
    free(w);
}

/* Whether a log's reader is at its end (it then follows new lines). */
static int view_at_end(const debugger *d, HWND h)
{
    RECT c;
    const int first = (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    const int count = (int)SendMessageW(h, EM_GETLINECOUNT, 0, 0);
    GetClientRect(h, &c);
    return first + (d->mono_h > 0 && c.bottom > d->mono_h ? c.bottom / d->mono_h : 1) >= count;
}

/* Replaces a log's text where the reader left it, or, following, at its end;
 * nothing is touched when it did not change. */
static void view_set_at(HWND h, const char *text, int follow)
{
    wchar_t *w = view_wide(text);
    int first, len;
    if (!w) return;
    len = GetWindowTextLengthW(h);
    if (len == (int)wcslen(w)) {
        wchar_t *cur = malloc(((size_t)len + 1) * sizeof *cur);
        const int same = cur && GetWindowTextW(h, cur, len + 1) == len && wcscmp(cur, w) == 0;
        free(cur);
        if (same) { free(w); return; }
    }
    first = (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    SetWindowTextW(h, w);
    if (follow) SendMessageW(h, EM_LINESCROLL, 0, SendMessageW(h, EM_GETLINECOUNT, 0, 0));
    else SendMessageW(h, EM_LINESCROLL, 0, first - (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0));
    free(w);
}

/* A short control's text from UTF-8; the control repaints only when it differs
 * (no flicker on the live refresh). */
void dbg_label_set(HWND h, const char *utf8)
{
    wchar_t cur[512], w[512];
    dbg_wide_from(utf8, w, 512);
    GetWindowTextW(h, cur, 512);
    if (wcscmp(cur, w) != 0) SetWindowTextW(h, w);
}

/* The text of an edit as UTF-8. */
static void edit_text(HWND h, char *out, int outsz)
{
    wchar_t w[512];
    out[0] = '\0';
    w[0] = 0;
    GetWindowTextW(h, w, 512);
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, out, outsz, NULL, NULL) <= 0) out[outsz - 1] = '\0';
}

static size_t addf(char *s, size_t len, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (len >= cap) return len;
    va_start(ap, fmt);
    n = vsnprintf(s + len, cap - len, fmt, ap);
    va_end(ap);
    if (n < 0) return len;
    return len + (size_t)n < cap ? len + (size_t)n : cap - 1;
}

/* $hex, 0xhex, #dec, or bare hex, as the prompt reads numbers. */
static int parse_num(const char *text, long *out)
{
    char buf[64];
    const char *p;
    char *end;
    long v;
    int base = 16;

    snprintf(buf, sizeof buf, "%s", text);
    p = buf;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '$') p++;
    else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    else if (*p == '#') { p++; base = 10; }
    if (!*p) return 0;
    v = strtol(p, &end, base);
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    if (*end) return 0;
    *out = v;
    return 1;
}

/* ISAR in octal: "12" or upper.lower ("1.2"). */
static int parse_isar(const char *text, long *out)
{
    char buf[32];
    char *dot, *end;
    long v;
    snprintf(buf, sizeof buf, "%s", text);
    dot = strchr(buf, '.');
    if (dot) {
        long u, l;
        *dot = '\0';
        u = strtol(buf, &end, 8);
        if (*end || !buf[0]) return 0;
        l = strtol(dot + 1, &end, 8);
        if (*end || !dot[1]) return 0;
        v = (u << 3) | l;
    } else {
        v = strtol(buf, &end, 8);
        if (*end || !buf[0]) return 0;
    }
    if (v < 0 || v > 63) return 0;
    *out = v;
    return 1;
}

static int resolve_addr(debugger *d, const char *text)
{
    long v;
    int addr = -1;
    const char *p = text;
    while (*p == ' ') p++;
    if (!*p) return -1;
    if (*p != '$' && *p != '#' && !(p[0] == '0' && (p[1] == 'x' || p[1] == 'X')))
        addr = chfdebug_label_address(d->dbg, p);
    if (addr < 0 && parse_num(p, &v) && v >= 0 && v <= 0xffff) addr = (int)v;
    return addr;
}

/* A hex dump with a column header, 16 bytes a row, row labels from base. */
static void hexdump(char *s, size_t cap, const uint8_t *m, int n, unsigned base, int digits)
{
    size_t len = 0;
    int row, col;
    len = addf(s, len, cap, "%*s", digits + 3, "");
    for (col = 0; col < 16; col++) len = addf(s, len, cap, "%X  ", col);
    for (row = 0; row * 16 < n; row++) {
        len = addf(s, len, cap, "\n$%0*X: ", digits, base + (unsigned)row * 16);
        for (col = 0; col < 16 && row * 16 + col < n; col++)
            len = addf(s, len, cap, "%02X ", m[row * 16 + col]);
    }
}

static HBRUSH make_brush(uint32_t rgb)
{
    return CreateSolidBrush(RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff));
}

/* ---- images ------------------------------------------------------------------ */

static int image_reserve(image *im, size_t px)
{
    uint32_t *p;
    if (px <= im->cap) return 1;
    p = realloc(im->px, px * sizeof *p);
    if (!p) return 0;
    im->px = p;
    im->cap = px;
    return 1;
}

/* One of the video views into im at an integer scale: asks for its size
 * first, grows the buffer to fit and draws. 0 (and an empty image) when
 * there is none. */
static int fetch_view(debugger *d, image *im, int view, unsigned flags, int scale)
{
    int w = 0, h = 0;
    im->w = im->h = 0;
    chfdebug_video_view(d->dbg, view, flags, scale, NULL, 0, &w, &h);
    if (w > 0 && h > 0) {
        if (!image_reserve(im, (size_t)w * (size_t)h)) return 0;
    } else if (!image_reserve(im, (size_t)CHFDEBUG_VIEW_MAX_PIXELS * (size_t)scale * (size_t)scale)) {
        return 0;
    }
    w = h = 0;
    if (!chfdebug_video_view(d->dbg, view, flags, scale, im->px, im->cap, &w, &h) || w <= 0 || h <= 0)
        return 0;
    im->w = w;
    im->h = h;
    return 1;
}

/* ---- the Video tab ------------------------------------------------------------ */

static unsigned video_flags(const debugger *d)
{
    unsigned bits = 0;
    int i;
    for (i = 0; i < NOVERLAYS; i++)
        if (SendMessageW(d->overlay[i], BM_GETCHECK, 0, 0) == BST_CHECKED) bits |= 1u << i;
    return bits;
}

/* The integer scale for a view: the one the combo chose, or for Fit the
 * largest that fits the picture window. */
static int video_scale(debugger *d, int view, unsigned flags)
{
    const int sel = (int)SendMessageW(d->scale_mode, CB_GETCURSEL, 0, 0);
    int w = 0, h = 0, s;
    RECT c;
    if (sel > 0) return sel;
    chfdebug_video_view(d->dbg, view, flags, 1, NULL, 0, &w, &h);
    GetClientRect(d->vpic, &c);
    if (w <= 0 || h <= 0 || c.right <= 0) return 2;
    s = c.right / w < c.bottom / h ? c.right / w : c.bottom / h;
    return s < 1 ? 1 : s > 8 ? 8 : s;
}

/* A cell as the hover and the pin tell it. */
static void describe_cell(debugger *d, int row, int col, char *out, size_t n)
{
    chfdebug_vram_cell c;
    size_t len = 0;
    if (row < 0 || col < 0 || chfdebug_vram_cell_get(d->dbg, row, col, &c) < 0) {
        snprintf(out, n, "(none)");
        return;
    }
    len = addf(out, len, n, "Row %d, column %d   %s%s%s\n", c.row, c.col, c.visible ? "visible" : "hidden",
               c.hidden_why ? ": " : "", c.hidden_why ? c.hidden_why : "");
    len = addf(out, len, n, "Value %d   palette %d (%s)\nPen %d: %s   RGB #%06X\n", c.value, c.palette,
               chfdebug_palette_name(c.palette), c.pen, chfdebug_colour_name(c.pen), c.rgb & 0xffffff);
    if (c.last_pc < 0)
        len = addf(out, len, n, "Written %u times, none since the debugger attached", c.writes);
    else
        len = addf(out, len, n, "Written %u times; last by PC $%04X at frame %u, line %d (was %d)", c.writes,
                   c.last_pc & 0xffff, c.last_frame, c.last_line, c.last_old);
}

static void refresh_hover_texts(debugger *d)
{
    char t[512];
    if (d->vsec != SEC_CELL) return;
    describe_cell(d, d->hover_row, d->hover_col, t, sizeof t);
    view_set(d->hover_text, t);
    describe_cell(d, d->pin_row, d->pin_col, t, sizeof t);
    view_set(d->pin_text, t);
}

/* The status line under the picture: the view and, if the pointer is on one,
 * the cell. */
static void refresh_vstatus(debugger *d)
{
    char t[300], cell[200] = "";
    if (d->hover_row >= 0) {
        chfdebug_vram_cell c;
        if (chfdebug_vram_cell_get(d->dbg, d->hover_row, d->hover_col, &c) >= 0) {
            if (c.visible)
                snprintf(cell, sizeof cell, "   row %d col %d: value %d, %s", c.row, c.col, c.value,
                         chfdebug_colour_name(c.pen));
            else
                snprintf(cell, sizeof cell, "   row %d col %d: value %d, %s (hidden: %s)", c.row, c.col, c.value,
                         chfdebug_colour_name(c.pen), c.hidden_why ? c.hidden_why : "outside the picture");
        }
    }
    snprintf(t, sizeof t, "%dx%d at %dx.%s", d->vimg.w, d->vimg.h, d->vimg_scale,
             cell[0] ? cell : "   Hover for a cell, click to pin it, double-click to break on its writes.");
    dbg_label_set(d->vstatus, t);
}

void dbg_hover_changed(debugger *d, int row, int col)
{
    if (row == d->hover_row && col == d->hover_col) return;
    d->hover_row = row;
    d->hover_col = col;
    InvalidateRect(d->vpic, NULL, FALSE);
    refresh_hover_texts(d);
    refresh_vstatus(d);
}

void dbg_pin_cell(debugger *d, int row, int col)
{
    d->pin_row = row;
    d->pin_col = col;
    InvalidateRect(d->vpic, NULL, FALSE);
    refresh_hover_texts(d);
}

void dbg_break_on_cell(debugger *d, int row, int col)
{
    char msg[160];
    const int id = chfdebug_breakpoint_add(d->dbg, CHFDEBUG_BP_VRAM, CHF_SPACE_VRAM, (uint16_t)(row * 128 + col),
                                           (uint16_t)(row * 128 + col), "");
    snprintf(msg, sizeof msg, id >= 0 ? "Breaking on writes to row %d column %d (breakpoint %d)."
                                      : "Could not set a breakpoint on row %d column %d.", row, col, id);
    dbg_label_set(d->vstatus, msg);
}

/* Rows whose palette changed since the frame before. */
static void track_palettes(debugger *d)
{
    int row;
    if (d->vid.frame == d->pal_changed_frame && d->prev_row_valid) return;
    memset(d->pal_changed, 0, sizeof d->pal_changed);
    if (d->prev_row_valid)
        for (row = 0; row < CHFDEBUG_VRAM_ROWS; row++)
            if (d->vid.row_palette[row] != d->prev_row_pal[row]) d->pal_changed[row / 32] |= 1u << (row % 32);
    memcpy(d->prev_row_pal, d->vid.row_palette, sizeof d->prev_row_pal);
    d->prev_row_valid = 1;
    d->pal_changed_frame = d->vid.frame;
}

static void refresh_palette_text(debugger *d)
{
    static char t[2048];
    size_t len = 0;
    int p, v;
    for (p = 0; p < 4; p++) {
        len = addf(t, len, sizeof t, "%sP%d %s (%d rows)\n", p ? "\n" : "", p, chfdebug_palette_name(p),
                   d->vid.palette_rows[p]);
        for (v = 0; v < 4; v++) {
            const int pen = chfdebug_palette_pen(p, v);
            len = addf(t, len, sizeof t, "  %d  #%06X  %s\n", v, chfdebug_colour_rgb(pen) & 0xffffff,
                       chfdebug_colour_name(pen));
        }
    }
    view_set(d->pal_text, t);
}

static void refresh_ports_text(debugger *d)
{
    static char t[3072];
    const chfdebug_video *v = &d->vid;
    static const char *const freq[4] = { "off", "1 kHz", "500 Hz", "120 Hz" };
    size_t len = 0;
    len = addf(t, len, sizeof t,
        "Port 0   console buttons and the write strobe\n"
        "  raw $%02X   complement $%02X   strobe %d\n"
        "  controllers %s   panel latch $%X\n"
        "  the CPU reads $%02X now\n\n",
        v->port0, (uint8_t)~v->port0, v->strobe, v->controllers_enabled ? "enabled" : "off", v->panel_latch & 15,
        v->in0);
    len = addf(t, len, sizeof t,
        "Port 1   colour; the right controller (Player 1)\n"
        "  raw $%02X   complement $%02X   value %d (%s)\n"
        "  the CPU reads $%02X now\n\n",
        v->port1, (uint8_t)~v->port1, v->value, chfdebug_colour_name(chfdebug_palette_pen(0, v->value & 3)), v->in1);
    len = addf(t, len, sizeof t,
        "Port 4   column; the left controller (Player 2)\n"
        "  raw $%02X   complement $%02X   column %d\n"
        "  the CPU reads $%02X now\n\n",
        v->port4, (uint8_t)~v->port4, v->col, v->in4);
    len = addf(t, len, sizeof t,
        "Port 5   row and the beeper\n"
        "  raw $%02X   complement $%02X   row %d   sound mode %d (%s)\n"
        "  the CPU reads $%02X now\n\n",
        v->port5, (uint8_t)~v->port5, v->row, v->sound_mode, freq[v->sound_mode & 3], v->in5);
    len = addf(t, len, sizeof t,
        "The next strobe writes %d to row %d, column %d (%s)\n\n"
        "Beam   line %d of %d   cycle %d of %d   frame %u   %s\n"
        "Strobes   %u this frame so far, %u in the last whole one (%u changed a cell)\n",
        v->value, v->row, v->col, v->row >= CHFDEBUG_VIS_ROW0 && v->row < CHFDEBUG_VIS_ROW0 + CHFDEBUG_VIS_ROWS &&
        v->col >= CHFDEBUG_VIS_COL0 && v->col < CHFDEBUG_VIS_COL0 + CHFDEBUG_VIS_COLS ? "visible" : "hidden",
        v->line, v->lines, v->cycle_in_line, v->cycles_per_line, v->frame, v->is_pal ? "PAL" : "NTSC",
        v->strobes_frame, v->strobes_last, v->changes_last);
    view_set(d->ports_text, t);
}

static int field_int(HWND h, int def)
{
    char t[32];
    edit_text(h, t, sizeof t);
    return t[0] ? atoi(t) : def;
}

static void refresh_writes(debugger *d)
{
    static char t[WRITES_MAX * 90 + 128];
    chfdebug_vram_filter f;
    size_t len = 0;
    char counts[200];
    int i, follow;
    f.row0 = field_int(d->wr_row0, 0);
    f.row1 = field_int(d->wr_row1, 63);
    f.col0 = field_int(d->wr_col0, 0);
    f.col1 = field_int(d->wr_col1, 127);
    f.changes_only = SendMessageW(d->wr_changes, BM_GETCHECK, 0, 0) == BST_CHECKED;
    f.this_frame = SendMessageW(d->wr_frame, BM_GETCHECK, 0, 0) == BST_CHECKED;
    f.palette_cols = SendMessageW(d->wr_palcols, BM_GETCHECK, 0, 0) == BST_CHECKED;
    d->nwrites = chfdebug_vram_writes(d->dbg, &f, d->writes, WRITES_MAX);
    if (d->nwrites < 0) d->nwrites = 0;
    snprintf(counts, sizeof counts, "Frame %u: %u strobes so far; the last whole frame had %u (%u changed a cell). %d listed.",
             d->vid.frame, d->vid.strobes_frame, d->vid.strobes_last, d->vid.changes_last, d->nwrites);
    dbg_label_set(d->wr_counts, counts);
    t[0] = '\0';
    for (i = 0; i < d->nwrites; i++) {
        const chfdebug_vram_write *w = &d->writes[i];
        len = addf(t, len, sizeof t, "%sf%-6u l%3d c%-9llu pc $%04X  r%-2d c%-3d  %d -> %d", i ? "\n" : "", w->frame,
                   w->line, (unsigned long long)w->cycle, w->pc, w->row, w->col, w->old, w->value);
    }
    if (d->nwrites == 0) len = addf(t, len, sizeof t, "(no VRAM writes match, or the engine saw none since it attached)");
    follow = view_at_end(d, d->wr_text);
    view_set_at(d->wr_text, t, follow);
}

static void refresh_video(debugger *d, int slow)
{
    const int view = (int)SendMessageW(d->view_mode, CB_GETCURSEL, 0, 0);
    const unsigned flags = video_flags(d);
    int scale;
    chfdebug_video_get(d->dbg, &d->vid);
    track_palettes(d);
    scale = video_scale(d, view < 0 ? 0 : view, flags);
    if (fetch_view(d, &d->vimg, view < 0 ? 0 : view, flags, scale)) {
        d->vimg_view = view < 0 ? 0 : view;
        d->vimg_scale = scale;
    }
    InvalidateRect(d->vpic, NULL, FALSE);
    refresh_vstatus(d);
    switch (d->vsec) {
    case SEC_CELL: refresh_hover_texts(d); break;
    case SEC_ROWS:
        InvalidateRect(d->rows_view, NULL, FALSE);
        if (slow) refresh_palette_text(d);
        break;
    case SEC_PORTS: refresh_ports_text(d); break;
    case SEC_WRITES: if (slow) refresh_writes(d); break;
    default: break;
    }
}

/* ---- the other pages' refreshers ------------------------------------------------ */

static void refresh_status(debugger *d)
{
    char reason[160], buf[220];
    int addr;
    const int stopped = chfdebug_is_stopped(d->dbg);
    chfdebug_stop_reason(d->dbg, reason, sizeof reason, &addr);
    if (stopped) snprintf(buf, sizeof buf, "Stopped%s%s", reason[0] ? ": " : "", reason);
    else snprintf(buf, sizeof buf, "Running");
    dbg_label_set(d->status, buf);
    dbg_label_set(d->run_btn, stopped ? "Run (F5)" : "Stop (F5)");
    InvalidateRect(d->run_btn, NULL, FALSE);
}

static int mem_space_now(const debugger *d)
{
    const int sel = (int)SendMessageW(d->ram_space, CB_GETCURSEL, 0, 0);
    return sel >= 0 && sel < NSPACES ? sel : CHF_SPACE_PROG;
}

static int mem_digits(int space)
{
    switch (space) {
    case CHF_SPACE_PROG: return 4;
    case CHF_SPACE_VRAM: return 4;
    case CHF_SPACE_CART: return 5;
    default: return 2;
    }
}

/* The memory view: 16 rows of 16 bytes from mem_base in the space showing. */
static void refresh_mem(debugger *d)
{
    static uint8_t bytes[16 * MEM_ROWS];
    static char dump[4096];
    const int space = mem_space_now(d);
    const uint32_t size = (uint32_t)chfdebug_space_size(d->dbg, space);
    char title[128];
    if (d->mem_base >= size) d->mem_base = size > sizeof bytes ? (size - 1) & ~0xfu : 0;
    memset(bytes, 0xff, sizeof bytes);
    chfdebug_read(d->dbg, space, d->mem_base, bytes, (int)sizeof bytes);
    hexdump(dump, sizeof dump, bytes, (int)sizeof bytes, d->mem_base, mem_digits(space));
    view_set(d->mem_view, dump);
    snprintf(title, sizeof title, "%ls from $%0*X (size $%X; reads have no side effects)", kSpaceNames[space],
             mem_digits(space), d->mem_base, size);
    dbg_label_set(d->mem_label, title);
}

static void refresh_cpu(debugger *d)
{
    chfdebug_cpu *c = &d->cpu;
    static char buf[2048];
    int i;
    size_t len = 0;
    chfdebug_cpu_get(d->dbg, c);

    /* the scratchpad cells that changed since the registers last differed */
    if (!d->prev_valid || memcmp(d->prev_r, c->r, sizeof c->r) != 0) {
        uint64_t m = 0;
        if (d->prev_valid)
            for (i = 0; i < 64; i++)
                if (d->prev_r[i] != c->r[i]) m |= 1ull << i;
        d->changed_mask = m;
        memcpy(d->prev_r, c->r, sizeof c->r);
        d->prev_valid = 1;
    }
    {
        const unsigned v[NFIELDS] = { (unsigned)c->pc & 0xffff, c->pc0, c->pc1, c->dc0, c->dc1, c->a, c->w, c->isar,
                                      c->j_h_k_q[0], c->j_h_k_q[1], c->j_h_k_q[2], c->j_h_k_q[3] };
        for (i = 0; i < NFIELDS; i++) {
            if (GetFocus() == d->fld[i]) continue;      /* do not fight the user's typing */
            if (kFields[i].octal) snprintf(buf, sizeof buf, "%o.%o", (v[i] >> 3) & 7, v[i] & 7);
            else snprintf(buf, sizeof buf, "%0*X", kFields[i].digits, v[i] & ((1u << (4 * kFields[i].digits)) - 1));
            dbg_label_set(d->fld[i], buf);
        }
    }
    {
        const int flags[NFLAGS] = { c->sign, c->carry, c->zero, c->overflow, c->icb };
        for (i = 0; i < NFLAGS; i++)
            SendMessageW(d->flag[i], BM_SETCHECK, flags[i] ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    len = addf(buf, len, sizeof buf,
        "PC $%04X   (PC0 $%04X: the CPU has fetched the opcode, dbus $%02X)\n"
        "ISAR o%02o = %o.%o   W $%02X: S%s C%d Z%d O%d ICB%d\n"
        "Cycles %llu   instructions %llu   illegal opcodes %u\n"
        "Beam   line %d   cycle %d   frame %u\n"
        "%s, BIOS %s\n",
        c->pc & 0xffff, c->pc0, c->dbus, c->isar & 63, (c->isar >> 3) & 7, c->isar & 7, c->w,
        c->sign ? "+" : "-", c->carry, c->zero, c->overflow, c->icb, (unsigned long long)c->cycles,
        (unsigned long long)c->instructions, c->illegal_count, c->vpos, c->hpos, c->frame,
        chf_machine_name(c->machine),
        c->bios == CHF_ROM_SL31253 ? "sl31253" : c->bios == CHF_ROM_SL90025 ? "sl90025" : "the stand-in");
    if (c->illegal_count && c->illegal_pc >= 0) {
        /* the last illegal opcode's address, on the line it belongs to */
        len = addf(buf, len, sizeof buf, "Last illegal opcode at $%04X\n", c->illegal_pc & 0xffff);
    }
    view_set(d->cpu_state, buf);

    /* the BIOS's K stack: r59 points at the top (40..58), two bytes a return */
    len = 0;
    if (c->k_depth < 0) {
        len = addf(buf, len, sizeof buf, "r59 = %02o (octal 0%o): not a K-stack pointer", c->r[59], c->r[59]);
        len = addf(buf, len, sizeof buf, "\n(r40-r59 is the stack; r59 odd, 40-58 when in use)");
    } else {
        len = addf(buf, len, sizeof buf, "r59 = %d (depth %d)", c->r[59], c->k_depth);
        for (i = 0; i < c->k_depth && i < 10; i++)
            len = addf(buf, len, sizeof buf, "\n%2d: $%04X", i + 1, c->k_stack[i]);
        if (c->k_depth == 0) len = addf(buf, len, sizeof buf, "\n(empty)");
    }
    view_set(d->kstack, buf);
    InvalidateRect(d->scr_grid, NULL, FALSE);
    refresh_mem(d);
}

void dbg_scratch_pick(debugger *d, int reg)
{
    char t[16];
    snprintf(t, sizeof t, "%02o", reg);
    dbg_label_set(d->scr_reg, t);
    snprintf(t, sizeof t, "%02X", d->cpu.r[reg & 63]);
    dbg_label_set(d->scr_val, t);
    SetFocus(d->scr_val);
    SendMessageW(d->scr_val, EM_SETSEL, 0, -1);
}

void dbg_refresh_disasm(debugger *d)
{
    RECT c;
    int pc_line = -1, rows;
    const int follow = SendMessageW(d->follow_pc, BM_GETCHECK, 0, 0) == BST_CHECKED;
    GetClientRect(d->disasm, &c);
    rows = d->mono_h > 0 ? c.bottom / d->mono_h : 1;
    if (rows < 1) rows = 1;
    if (rows > DISASM_MAX) rows = DISASM_MAX;
    if (follow) {
        /* the PC a third of the way down, so what led to it shows too */
        chfdebug_cpu cpu;
        chfdebug_cpu_get(d->dbg, &cpu);
        d->disasm_top = chfdebug_row_address(d->dbg, (uint16_t)cpu.pc, -(rows / 3));
    }
    d->line_count = chfdebug_disassemble(d->dbg, (uint16_t)d->disasm_top, d->lines, rows, &pc_line);
    d->pc_line = pc_line;
    InvalidateRect(d->disasm, NULL, FALSE);
}

/* The controllers, the beeper and the 2102 Videocarts' RAM port. */
static void refresh_io(debugger *d)
{
    static char s[2048], lg[EVENTS_MAX * 80 + 64];
    size_t len = 0;
    int i, follow;
    static const char *const freq[4] = { "off", "1 kHz", "500 Hz", "120 Hz" };
    chfdebug_io_get(d->dbg, &d->io);
    chfdebug_video_get(d->dbg, &d->vid);
    InvalidateRect(d->io_draw, NULL, FALSE);

    len = addf(s, len, sizeof s, "Machine   %s%s\nCartridge type %d   %s\n", chf_machine_name(d->io.machine),
               d->io.is_pal ? " (PAL)" : "", d->io.cart_type, d->io.reset_held ? "RESET held" : "RESET released");
    len = addf(s, len, sizeof s, "Beeper    %s, level %d%s\n          %u mode changes, %llu samples\n", freq[d->io.sound_mode & 3],
               d->io.sound_level, d->io.sound_forced ? " (forced on)" : "", d->io.sound_changes,
               (unsigned long long)d->io.audio_samples);
    if (d->io.has_2102)
        len = addf(s, len, sizeof s, "2102 RAM  port $%02X, address $%03X (%u), bit %d\n", d->io.ram_port,
                   d->io.ram_addr, d->io.ram_addr, d->io.ram_bit);
    else
        len = addf(s, len, sizeof s, "2102 RAM  none (the Maze and Hangman Videocarts have one)\n");
    view_set(d->io_state, s);

    d->nsnd = chfdebug_sound_events(d->dbg, d->snd_events, EVENTS_MAX);
    if (d->nsnd < 0) d->nsnd = 0;
    len = 0;
    lg[0] = '\0';
    for (i = 0; i < d->nsnd; i++) {
        const chfdebug_sound_event *e = &d->snd_events[i];
        len = addf(lg, len, sizeof lg, "%sf%-6u pc $%04X  mode %d -> %d  (%s)", i ? "\n" : "", e->frame, e->pc,
                   e->old_mode, e->mode, freq[e->mode & 3]);
    }
    if (d->nsnd == 0) len = addf(lg, len, sizeof lg, "(no mode changes since the debugger attached)");
    follow = view_at_end(d, d->snd_log);
    view_set_at(d->snd_log, lg, follow);

    d->nio = chfdebug_io_events(d->dbg, d->io_events, EVENTS_MAX);
    if (d->nio < 0) d->nio = 0;
    len = 0;
    lg[0] = '\0';
    for (i = 0; i < d->nio; i++) {
        const chfdebug_io_event *e = &d->io_events[i];
        len = addf(lg, len, sizeof lg, "%sf%-6u pc $%04X  %s $%02X %s $%02X", i ? "\n" : "", e->frame, e->pc,
                   e->is_write ? "OUT" : "IN ", e->port, e->is_write ? "<-" : "->", e->value);
    }
    if (d->nio == 0) len = addf(lg, len, sizeof lg, "(no port accesses since the debugger attached)");
    follow = view_at_end(d, d->io_log);
    view_set_at(d->io_log, lg, follow);
}

static const char *bp_where(const chfdebug_breakpoint *b, char *out, size_t n)
{
    switch (b->type) {
    case CHFDEBUG_BP_VRAM:
        if (b->start == b->end) snprintf(out, n, "r%d,c%d", b->start / 128, b->start % 128);
        else snprintf(out, n, "r%d,c%d-r%d,c%d", b->start / 128, b->start % 128, b->end / 128, b->end % 128);
        break;
    case CHFDEBUG_BP_SCRATCH:
        if (b->start == b->end) snprintf(out, n, "r%d", b->start);
        else snprintf(out, n, "r%d-r%d", b->start, b->end);
        break;
    case CHFDEBUG_BP_IN: case CHFDEBUG_BP_OUT:
        if (b->start == b->end) snprintf(out, n, "$%02X", b->start);
        else snprintf(out, n, "$%02X-$%02X", b->start, b->end);
        break;
    default:
        if (b->start == b->end) snprintf(out, n, "$%04X", b->start);
        else snprintf(out, n, "$%04X-$%04X", b->start, b->end);
        break;
    }
    return out;
}

static const char *bp_kind(int type)
{
    switch (type) {
    case CHFDEBUG_BP_EXEC: return "exec";
    case CHFDEBUG_BP_READ: return "read";
    case CHFDEBUG_BP_WRITE: return "write";
    case CHFDEBUG_BP_IN: return "in";
    case CHFDEBUG_BP_OUT: return "out";
    case CHFDEBUG_BP_VRAM: return "vram";
    case CHFDEBUG_BP_SCRATCH: return "scratch";
    default: return "?";
    }
}

static void refresh_bps(debugger *d)
{
    int i, sel = (int)SendMessageW(d->bp_list, LB_GETCURSEL, 0, 0);
    d->nbps = chfdebug_breakpoint_list(d->dbg, d->bps, MAX_BPS);
    if (d->nbps < 0) d->nbps = 0;
    SendMessageW(d->bp_list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(d->bp_list, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < d->nbps; i++) {
        const chfdebug_breakpoint *b = &d->bps[i];
        char line[300], where[64];
        wchar_t w[300];
        snprintf(line, sizeof line, "%c %3d  %-7s  %-18s  hits %-6u %s%s", b->enabled ? '+' : '-', b->id,
                 bp_kind(b->type), bp_where(b, where, sizeof where), b->hits, b->condition[0] ? "if " : "",
                 b->condition);
        dbg_wide_from(line, w, 300);
        SendMessageW(d->bp_list, LB_ADDSTRING, 0, (LPARAM)w);
    }
    if (d->nbps == 0)
        SendMessageW(d->bp_list, LB_ADDSTRING, 0,
                     (LPARAM)L"(no breakpoints -- double-click a disassembly line, or add one below)");
    if (sel >= 0 && sel < d->nbps) SendMessageW(d->bp_list, LB_SETCURSEL, (WPARAM)sel, 0);
    SendMessageW(d->bp_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(d->bp_list, NULL, FALSE);
}

static int cart_is_fujinet(const chfdebug_cart *c)
{
    return c->present && c->type_name && strcmp(c->type_name, "FujiNet") == 0;
}

static void refresh_cart(debugger *d)
{
    chfdebug_cart *c = &d->cart_state;
    char info[256];
    static char s[6144], ev[EVENTS_MAX * 80 + 64];
    static char dump[MAILBOX_SIZE / 16 * 56 + 256];
    size_t len = 0;
    int i, follow;
    static const char *const kinds[5] = { "?", "REG", "TX", "BANK", "SWAP" };
    chfdebug_cart_get(d->dbg, c);
    chfdebug_cart_info(d->dbg, info, sizeof info);
    if (!c->present) {
        view_set(d->cart_text, "No cartridge is running.");
        view_set(d->cart_mailbox, "");
        view_set(d->cart_events, "");
        return;
    }
    len = addf(s, len, sizeof s, "%s\n\nType            %s\n", info, c->type_name ? c->type_name : "?");
    if (cart_is_fujinet(c)) {
        const uint8_t *sp = c->status_page;
        len = addf(s, len, sizeof s,
            "Link            %s%s\n"
            "Mailbox         %s\n"
            "Running         %s\n"
            "Armed           %s\n"
            "Live image      %u bytes, CRC-32 %08X\n",
            c->link_up ? "up" : "down", c->link_off ? " (switched off)" : "",
            c->mailbox ? "open" : "closed",
            c->app_live ? "a FujiNet app" : c->booted_game ? "a game from the network" : "CONFIG",
            c->armed ? "yes (a swap waits for the console)" : "no", c->live_size, c->live_crc);
        if (c->staged_size)
            len = addf(s, len, sizeof s, "Staged          %s%s, %u bytes, CRC-32 %08X\n",
                       c->staged_app ? "an app" : "a game", c->staged_mailbox ? " that claims the mailbox" : "",
                       c->staged_size, c->staged_crc);
        else
            len = addf(s, len, sizeof s, "Staged          nothing\n");
        len = addf(s, len, sizeof s, "Swaps           %d\nAPPBANK pages   %d", c->swaps, c->npages);
        if (c->page >= 0) len = addf(s, len, sizeof s, ", page %d at $4800\n", c->page);
        else len = addf(s, len, sizeof s, ", none showing at $4800\n");
        len = addf(s, len, sizeof s,
            "Boot            state %d   %d%%   error %d\n\n"
            "The status page ($FC00-$FC0F)\n"
            "  ACKSEQ $%02X   STATUS $%02X (link %d, busy %d)   ERR $%02X   REPLY_CMD $%02X\n"
            "  RXLEN %u   BOOT_STATE %u   BOOT_PCT %u   BOOT_ERR %u\n"
            "  MAGIC \"%c%c\"   PROTO_VER %u   TICK %u (60 Hz count)\n"
            "Queue           %u deep\n",
            c->boot_state, c->boot_pct, c->boot_err, sp[0], sp[1], sp[1] & 1, (sp[1] >> 1) & 1, sp[2], sp[3],
            sp[4] | (sp[5] << 8), sp[6], sp[7], sp[8], sp[9] >= 32 && sp[9] < 127 ? sp[9] : '.',
            sp[10] >= 32 && sp[10] < 127 ? sp[10] : '.', sp[11], c->tick, c->queue_depth);
        hexdump(dump, sizeof dump, c->mailbox_page, MAILBOX_SIZE, MAILBOX_BASE, 4);
        view_set(d->cart_mailbox, dump);
    } else {
        len = addf(s, len, sizeof s, "Softlist        %s\n", c->softlist_name ? c->softlist_name : "(not in the list)");
        if (c->description) len = addf(s, len, sizeof s, "Description     %s\n", c->description);
        len = addf(s, len, sizeof s, "ROM             %u bytes, CRC-32 %08X\n", c->rom_size, c->rom_crc);
        if (c->ram_size) len = addf(s, len, sizeof s, "RAM             %u %s\n", c->ram_size,
                                    c->ram_size == 1024 ? "bits (a 2102)" : "bytes");
        else len = addf(s, len, sizeof s, "RAM             none\n");
        if (c->bank || c->half) len = addf(s, len, sizeof s, "Bank            %d (half %d)\n", c->bank, c->half);
        view_set(d->cart_mailbox, "(the mailbox page is the FujiNet cartridge's)");
    }
    view_set(d->cart_text, s);

    d->ncart_ev = chfdebug_cart_events(d->dbg, d->cart_events_buf, EVENTS_MAX);
    if (d->ncart_ev < 0) d->ncart_ev = 0;
    len = 0;
    ev[0] = '\0';
    for (i = 0; i < d->ncart_ev; i++) {
        const chfdebug_cart_event *e = &d->cart_events_buf[i];
        len = addf(ev, len, sizeof ev, "%sf%-6u pc $%04X  %-4s reg $%02X = $%02X", i ? "\n" : "", e->frame, e->pc,
                   kinds[e->kind <= 4 ? e->kind : 0], e->reg, e->value);
    }
    if (d->ncart_ev == 0) len = addf(ev, len, sizeof ev, "(no cartridge events since the debugger attached)");
    follow = view_at_end(d, d->cart_events);
    view_set_at(d->cart_events, ev, follow);
}

/* The packet log is rebuilt only when it changed: a new packet, or a
 * repeat (which moves the newest serial on). */
static void refresh_fujinet(debugger *d, int force)
{
    static chfdebug_fuji_packet p[TRACE_MAX];
    static char t[TRACE_MAX * 320 + 128];
    static char state[2048];
    char note[200], status[128];
    size_t len = 0;
    int n = chfdebug_fujinet_trace(d->dbg, p, TRACE_MAX), i, follow;

    if (chfsession_fujinet_external(d->session)) {
        chfsession_cart_status(d->session, status, sizeof status);
        snprintf(note, sizeof note, "External FujiNet at %s. Cartridge: %s.",
                 chfsession_boip_endpoint(d->session), status);
    } else if (!chfsession_fujinet_running(d->session)) {
        snprintf(note, sizeof note, "FujiNet is off (Preferences \xe2\x96\xb8 FujiNet).");
    } else {
        chfsession_cart_status(d->session, status, sizeof status);
        snprintf(note, sizeof note, "FujiNet is on. Cartridge: %s.", status);
    }
    dbg_label_set(d->fuji_note, note);
    chfdebug_fujinet_state(d->dbg, state, sizeof state);
    view_set(d->fuji_state, state);
    if (n < 0) n = 0;

    if (!force && n == d->fuji_count && (n == 0 || p[n - 1].serial == d->fuji_last)) return;
    follow = force || d->fuji_count < 0 || view_at_end(d, d->fuji_text);
    d->fuji_count = n;
    d->fuji_last = n > 0 ? p[n - 1].serial : 0;
    t[0] = '\0';
    for (i = 0; i < n; i++) {
        int k;
        len = addf(t, len, sizeof t, "%s%s", i ? "\n" : "", p[i].text);
        if (p[i].repeat > 1) len = addf(t, len, sizeof t, " \xc3\x97%u", p[i].repeat);
        len = addf(t, len, sizeof t, "\n     ");
        for (k = 0; k < p[i].nbytes && k < (int)sizeof p[i].data; k++)
            len = addf(t, len, sizeof t, "%02X ", p[i].data[k]);
        if (p[i].nbytes > (int)sizeof p[i].data) len = addf(t, len, sizeof t, "\xe2\x80\xa6");
    }
    if (n == 0) len = addf(t, len, sizeof t, "(no FujiBus packets since the trace was cleared)");
    view_set_at(d->fuji_text, t, follow);
}

/* The page showing: its refresher, with `slow` standing for the heavier
 * parts (the sections of the Video tab) while the machine runs. */
static void refresh_page(debugger *d, int slow)
{
    switch (d->page) {
    case PAGE_CPU: refresh_cpu(d); break;
    case PAGE_DISASM: dbg_refresh_disasm(d); break;
    case PAGE_VIDEO: refresh_video(d, slow); break;
    case PAGE_IO: refresh_io(d); break;
    case PAGE_BREAKS: refresh_bps(d); break;
    case PAGE_CART: refresh_cart(d); break;
    case PAGE_FUJINET: refresh_fujinet(d, 0); break;
    default: break;
    }
}

static void refresh_all(debugger *d)
{
    refresh_status(d);
    refresh_page(d, 1);
}

/* ---- actions ------------------------------------------------------------------ */

static void toggle_run(debugger *d)
{
    if (chfdebug_is_stopped(d->dbg)) chfdebug_resume(d->dbg);
    else chfdebug_stop(d->dbg);
    refresh_all(d);
}

static void run_prompt(debugger *d)
{
    static char out[65536];
    char cmd[256], echo[300];
    edit_text(d->prompt_in, cmd, sizeof cmd);
    if (!cmd[0]) return;
    /* history: the newest last, no repeats of the one before */
    if (d->nhistory == 0 || strcmp(d->history[d->nhistory - 1], cmd) != 0) {
        if (d->nhistory == HISTORY_MAX) {
            memmove(d->history[0], d->history[1], sizeof d->history[0] * (HISTORY_MAX - 1));
            d->nhistory--;
        }
        snprintf(d->history[d->nhistory++], sizeof d->history[0], "%s", cmd);
    }
    d->history_pos = d->nhistory;
    snprintf(echo, sizeof echo, "> %s\n", cmd);
    view_append(d->prompt_out, echo);
    chfdebug_command(d->dbg, cmd, out, sizeof out);
    view_append(d->prompt_out, out);
    view_append(d->prompt_out, "\n");
    SetWindowTextW(d->prompt_in, L"");
    refresh_all(d);
}

/* Up / Down in the prompt walk the commands already run; past the newest is
 * an empty line again. */
static void history_step(debugger *d, int newer)
{
    wchar_t w[256];
    int len;
    if (d->nhistory == 0) return;
    d->history_pos += newer ? 1 : -1;
    if (d->history_pos < 0) d->history_pos = 0;
    if (d->history_pos >= d->nhistory) {
        d->history_pos = d->nhistory;
        SetWindowTextW(d->prompt_in, L"");
        return;
    }
    dbg_wide_from(d->history[d->history_pos], w, 256);
    SetWindowTextW(d->prompt_in, w);
    len = GetWindowTextLengthW(d->prompt_in);
    SendMessageW(d->prompt_in, EM_SETSEL, (WPARAM)len, (LPARAM)len);
}

static void complete_prompt(debugger *d)
{
    char text[256], comps[4096];
    const char *word;
    char *sp;
    int n;
    edit_text(d->prompt_in, text, sizeof text);
    sp = strrchr(text, ' ');
    word = sp ? sp + 1 : text;
    n = chfdebug_completions(d->dbg, word, comps, sizeof comps);
    if (n == 1) {
        char line[256], joined[520];
        wchar_t w[520];
        char *nl;
        snprintf(line, sizeof line, "%.255s", comps);
        nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (sp) sp[1] = '\0'; else text[0] = '\0';
        snprintf(joined, sizeof joined, "%s%s ", text, line);
        dbg_wide_from(joined, w, 520);
        SetWindowTextW(d->prompt_in, w);
        SendMessageW(d->prompt_in, EM_SETSEL, (WPARAM)wcslen(w), (LPARAM)wcslen(w));
    } else if (n > 1) {
        view_append(d->prompt_out, comps);
    }
}

static void jump_to(debugger *d, const char *text)
{
    const int addr = resolve_addr(d, text);
    if (addr < 0) {
        view_append(d->prompt_out, "No such label or address.\n");
        return;
    }
    SendMessageW(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
    d->disasm_top = addr;
    dbg_refresh_disasm(d);
}

/* Memory view: Go to puts the address in the first row (a multiple of 16);
 * the buttons page through the space. */
static void mem_goto(debugger *d, const char *text)
{
    const int space = mem_space_now(d);
    const int size = chfdebug_space_size(d->dbg, space);
    const int addr = resolve_addr(d, text);
    if (addr < 0 || addr >= size) {
        view_append(d->prompt_out, addr < 0 ? "No such label or address.\n" : "Past the end of that space.\n");
        return;
    }
    d->mem_base = (uint32_t)addr & ~0xfu;
    refresh_mem(d);
}

static void mem_page(debugger *d, int dir)
{
    const int size = chfdebug_space_size(d->dbg, mem_space_now(d));
    const int next = (int)d->mem_base + dir * 16 * MEM_ROWS;
    d->mem_base = next < 0 ? 0 : next >= size ? d->mem_base : (uint32_t)next;
    refresh_mem(d);
}

static int pick_file(debugger *d, int save, const char *title, const wchar_t *filter, wchar_t *path, DWORD pathsz)
{
    OPENFILENAMEW ofn;
    wchar_t wtitle[128];
    memset(&ofn, 0, sizeof ofn);
    path[0] = 0;
    dbg_wide_from(title, wtitle, 128);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = d->hwnd;
    ofn.lpstrFile = path;
    ofn.nMaxFile = pathsz;
    ofn.lpstrTitle = wtitle;
    ofn.lpstrFilter = filter;
    if (save) {
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        return GetSaveFileNameW(&ofn) ? 1 : 0;
    }
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&ofn) ? 1 : 0;
}

static void save_file(debugger *d, int which)
{
    wchar_t wpath[MAX_PATH];
    char path[MAX_PATH * 3], msg[512], title[96];
    snprintf(title, sizeof title, "Save %s", kSaveTitles[which]);
    if (!pick_file(d, 1, title, L"All files\0*.*\0\0", wpath, MAX_PATH)) return;
    WideCharToMultiByte(CP_UTF8, 0, wpath, -1, path, sizeof path, NULL, NULL);
    chfdebug_save(d->dbg, kSaveKinds[which], path, msg, sizeof msg);
    view_append(d->prompt_out, msg);
    view_append(d->prompt_out, "\n");
}

static void popup_below(debugger *d, HWND button, HMENU m)
{
    RECT r;
    GetWindowRect(button, &r);
    TrackPopupMenu(m, TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, 0, d->hwnd, NULL);
    DestroyMenu(m);
}

static void save_menu(debugger *d)
{
    HMENU m = CreatePopupMenu();
    wchar_t w[96];
    int i;
    for (i = 0; i < NSAVE; i++) {
        dbg_wide_from(kSaveTitles[i], w, 96);
        AppendMenuW(m, MF_STRING, (UINT_PTR)(IDC_SAVE_KIND0 + i), w);
    }
    popup_below(d, d->save, m);
}

/* Symbols come from a file the user picks, or from the one beside the
 * opened cartridge (game.map / .lst next to game.bin). */
static void symbols_menu(debugger *d)
{
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDC_SYMS_FILE, L"From a file...");
    AppendMenuW(m, MF_STRING, IDC_SYMS_CART, L"Beside the cartridge");
    popup_below(d, d->load_symbols, m);
}

static void load_symbols(debugger *d, int beside_cart)
{
    wchar_t wpath[MAX_PATH];
    char path[MAX_PATH * 3], msg[512];
    if (!beside_cart) {
        if (!pick_file(d, 0, "Load symbols",
                       L"Symbol files (*.map;*.lst;*.sym;*.txt)\0*.map;*.lst;*.sym;*.txt\0All files\0*.*\0\0",
                       wpath, MAX_PATH))
            return;
        WideCharToMultiByte(CP_UTF8, 0, wpath, -1, path, sizeof path, NULL, NULL);
    }
    chfdebug_load_symbols(d->dbg, beside_cart ? NULL : path, msg, sizeof msg);
    view_append(d->prompt_out, msg);
    view_append(d->prompt_out, "\n");
    refresh_all(d);
}

/* One end of a breakpoint range, by the type's own notation. -1 when the text
 * is not one. */
static int bp_point(debugger *d, int type, const char *text)
{
    long v;
    int row, col;
    const char *p = text;
    while (*p == ' ') p++;
    if (!*p) return -1;
    switch (type) {
    case 5:
        if (sscanf(p, "%d , %d", &row, &col) != 2 && sscanf(p, "%d %d", &row, &col) != 2) return -1;
        if (row < 0 || row > 63 || col < 0 || col > 127) return -1;
        return row * 128 + col;
    case 6:
        if (*p == 'r' || *p == 'R') p++;
        if (!*p || (v = strtol(p, (char **)&p, 10), *p != '\0') || v < 0 || v > 63) return -1;
        return (int)v;
    case 3: case 4:
        if (!parse_num(text, &v) || v < 0 || v > 255) return -1;
        return (int)v;
    default:
        return resolve_addr(d, text);
    }
}

static void add_breakpoint(debugger *d)
{
    static const int types[NBPTYPES] = { CHFDEBUG_BP_EXEC, CHFDEBUG_BP_READ, CHFDEBUG_BP_WRITE, CHFDEBUG_BP_IN,
                                         CHFDEBUG_BP_OUT, CHFDEBUG_BP_VRAM, CHFDEBUG_BP_SCRATCH };
    static const int spaces[NBPTYPES] = { CHF_SPACE_PROG, CHF_SPACE_PROG, CHF_SPACE_PROG, CHF_SPACE_IO,
                                          CHF_SPACE_IO, CHF_SPACE_VRAM, CHF_SPACE_SCRATCH };
    char a[64], b[64], cond[256];
    int start, end, sel, id;
    edit_text(d->bp_start, a, sizeof a);
    edit_text(d->bp_end, b, sizeof b);
    edit_text(d->bp_cond, cond, sizeof cond);
    sel = (int)SendMessageW(d->bp_type, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= NBPTYPES) sel = 0;
    start = bp_point(d, sel, a);
    if (start < 0) {
        chf_message(d->hwnd, kBpHints[sel], "Breakpoint", MB_ICONWARNING | MB_OK);
        return;
    }
    end = b[0] ? bp_point(d, sel, b) : start;
    if (end < 0) {
        chf_message(d->hwnd, kBpHints[sel], "Breakpoint", MB_ICONWARNING | MB_OK);
        return;
    }
    if (end < start) end = start;
    id = chfdebug_breakpoint_add(d->dbg, types[sel], spaces[sel], (uint16_t)start, (uint16_t)end, cond);
    if (id < 0) {
        chf_message(d->hwnd, "The condition does not parse, or the range is outside its space.", "Breakpoint",
                    MB_ICONWARNING | MB_OK);
        return;
    }
    SetWindowTextW(d->bp_start, L"");
    SetWindowTextW(d->bp_end, L"");
    SetWindowTextW(d->bp_cond, L"");
    refresh_bps(d);
    dbg_refresh_disasm(d);
}

/* Run to the line selected in the Disassembly. */
static void run_to_cursor(debugger *d)
{
    if (d->sel_addr < 0) {
        chf_message(d->hwnd, "Select a line in the Disassembly first (click it).", "Run to Cursor",
                    MB_ICONINFORMATION | MB_OK);
        return;
    }
    chfdebug_run_to(d->dbg, (uint16_t)d->sel_addr);
    refresh_all(d);
}

static void write_scratch(debugger *d)
{
    char a[16], b[16];
    long reg, val;
    char *end;
    edit_text(d->scr_reg, a, sizeof a);
    edit_text(d->scr_val, b, sizeof b);
    reg = strtol(a, &end, 8);
    if (!a[0] || *end || reg < 0 || reg > 63 || !parse_num(b, &val) || val < 0 || val > 255) {
        chf_message(d->hwnd, "The register is its octal number (00-77), the value a byte in hex.", "Scratchpad",
                    MB_ICONWARNING | MB_OK);
        return;
    }
    chfdebug_write(d->dbg, CHF_SPACE_SCRATCH, (uint32_t)reg, (uint8_t)val);
    refresh_all(d);
}

static void on_accept(debugger *d, int id)
{
    char buf[512];
    long v;

    if (id >= IDC_FIELD0 && id <= IDC_FIELD_LAST) {
        const cpu_field *f = &kFields[id - IDC_FIELD0];
        edit_text(d->fld[id - IDC_FIELD0], buf, sizeof buf);
        if (f->octal ? parse_isar(buf, &v) : parse_num(buf, &v)) chfdebug_cpu_set(d->dbg, f->reg, (int)v);
        SetFocus(d->hwnd);
        refresh_all(d);
        return;
    }
    switch (id) {
    case IDC_PROMPT_IN: run_prompt(d); break;
    case IDC_RAM_GOTO:
        edit_text(d->ram_goto, buf, sizeof buf);
        mem_goto(d, buf);
        break;
    case IDC_RAM_ADDR:
    case IDC_RAM_VAL: {
        char abuf[64];
        const int space = mem_space_now(d);
        int a;
        edit_text(d->ram_addr, abuf, sizeof abuf);
        edit_text(d->ram_val, buf, sizeof buf);
        a = resolve_addr(d, abuf);
        if (a >= 0 && a < chfdebug_space_size(d->dbg, space) && parse_num(buf, &v))
            chfdebug_write(d->dbg, space, (uint32_t)a, (uint8_t)v);
        refresh_all(d);
        break;
    }
    case IDC_SCR_REG:
    case IDC_SCR_VAL: write_scratch(d); break;
    case IDC_JUMP:
        edit_text(d->jump, buf, sizeof buf);
        jump_to(d, buf);
        break;
    case IDC_BP_START:
    case IDC_BP_END:
    case IDC_BP_COND:
        add_breakpoint(d);
        break;
    case IDC_WR_ROW0: case IDC_WR_ROW1: case IDC_WR_COL0: case IDC_WR_COL1:
        refresh_writes(d);
        break;
    default: break;
    }
}

/* ---- edit subclassing ------------------------------------------------------------- */

static WNDPROC g_edit_proc;

/* Enter in a single-line field means "apply this value"; Tab in the prompt
 * completes and Up/Down recall. None may beep its way through the default
 * handler. */
static LRESULT CALLBACK edit_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    const int id = (int)GetWindowLongPtrW(hwnd, GWLP_ID);
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        PostMessageW(GetAncestor(hwnd, GA_ROOT), WM_DBG_ACCEPT, (WPARAM)id, 0);
        return 0;
    }
    if (msg == WM_CHAR && wp == VK_RETURN) return 0;
    if (id == IDC_PROMPT_IN && msg == WM_KEYDOWN && wp == VK_TAB) {
        PostMessageW(GetAncestor(hwnd, GA_ROOT), WM_DBG_TAB, 0, 0);
        return 0;
    }
    if (id == IDC_PROMPT_IN && msg == WM_KEYDOWN && (wp == VK_UP || wp == VK_DOWN)) {
        PostMessageW(GetAncestor(hwnd, GA_ROOT), WM_DBG_HISTORY, wp == VK_DOWN, 0);
        return 0;
    }
    if (id == IDC_PROMPT_IN && msg == WM_CHAR && wp == VK_TAB) return 0;
    if (id == IDC_PROMPT_IN && msg == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    return CallWindowProcW(g_edit_proc, hwnd, msg, wp, lp);
}

/* ---- construction ------------------------------------------------------------------ */

static HWND mk(debugger *d, DWORD ex, const wchar_t *cls, const wchar_t *text, DWORD style, int id, int page,
               int sec, int mono)
{
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | style, 0, 0, 10, 10, d->hwnd, (HMENU)(INT_PTR)id,
                             d->inst, NULL);
    if (d->nctl < MAX_CTL) {
        d->ctl[d->nctl].h = h;
        d->ctl[d->nctl].page = page;
        d->ctl[d->nctl].sec = sec;
        d->ctl[d->nctl].mono = mono;
        d->nctl++;
    }
    SendMessageW(h, WM_SETFONT, (WPARAM)(mono ? d->mono : d->ui), TRUE);
    return h;
}

/* A read-only mono text view: bars 0 has no scroll bars (a fixed dump), 1 a
 * vertical one (the lines wrap), 2 both (a log). */
static HWND view(debugger *d, int id, int page, int sec, int bars)
{
    HWND h = mk(d, WS_EX_CLIENTEDGE, L"EDIT", L"",
                (bars >= 1 ? WS_VSCROLL : 0) | (bars >= 2 ? WS_HSCROLL : 0) | ES_MULTILINE | ES_READONLY |
                    ES_AUTOVSCROLL | ES_NOHIDESEL,
                id, page, sec, 1);
    SendMessageW(h, EM_SETLIMITTEXT, 0, 0);
    return h;
}

static HWND field(debugger *d, int id, int page, int sec, int mono, int maxlen)
{
    HWND h = mk(d, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, id, page, sec, mono);
    g_edit_proc = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)edit_subclass);
    if (maxlen > 0) SendMessageW(h, EM_SETLIMITTEXT, (WPARAM)maxlen, 0);
    return h;
}

static HWND label(debugger *d, const char *text, int page, int sec)
{
    wchar_t w[160];
    dbg_wide_from(text, w, 160);
    return mk(d, 0, L"STATIC", w, SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 0, page, sec, 0);
}

static HWND button(debugger *d, const wchar_t *text, int id, int page, int sec)
{
    return mk(d, 0, L"BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, id, page, sec, 0);
}

static HWND checkbox(debugger *d, const wchar_t *text, int id, int page, int sec)
{
    return mk(d, 0, L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, id, page, sec, 0);
}

static HWND combo(debugger *d, int id, const wchar_t *const *items, int n, int page, int sec)
{
    HWND c = mk(d, 0, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id, page, sec, 0);
    int i;
    for (i = 0; i < n; i++) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)items[i]);
    SendMessageW(c, CB_SETCURSEL, 0, 0);
    return c;
}

static void build_controls(debugger *d)
{
    TCITEMW item;
    int i;

    d->tabs = mk(d, 0, WC_TABCONTROLW, L"", WS_CLIPSIBLINGS | WS_TABSTOP, IDC_TABS, -1, -1, 0);
    memset(&item, 0, sizeof item);
    item.mask = TCIF_TEXT;
    for (i = 0; i < PAGE_COUNT; i++) {
        item.pszText = (wchar_t *)kPageTitles[i];
        SendMessageW(d->tabs, TCM_INSERTITEMW, (WPARAM)i, (LPARAM)&item);
    }

    /* Toolbar: the stepping buttons, the symbol and save menus (they matter
     * on every page), Help for the tab showing, then the stop reason. Run/Stop
     * is owner-drawn so it can wear the accent while the machine is stopped. */
    d->run_btn = mk(d, 0, L"BUTTON", L"Stop (F5)", BS_OWNERDRAW | WS_TABSTOP, IDC_RUN, -1, -1, 0);
    d->step_btn = button(d, L"Step (F7)", IDC_STEP, -1, -1);
    d->over_btn = button(d, L"Step Over (F8)", IDC_OVER, -1, -1);
    d->out_btn = button(d, L"Step Out (Shift+F8)", IDC_OUT, -1, -1);
    d->frame_btn = button(d, L"Frame", IDC_FRAME, -1, -1);
    d->scan_btn = button(d, L"Scanline", IDC_SCAN, -1, -1);
    d->torun_btn = button(d, L"Run to Cursor", IDC_TORUN, -1, -1);
    d->load_symbols = button(d, L"Symbols...", IDC_LOAD_SYMBOLS, -1, -1);
    d->save = button(d, L"Save...", IDC_SAVE, -1, -1);
    d->help_btn = button(d, L"Help", IDC_HELP_BTN, -1, -1);
    d->status = mk(d, 0, L"STATIC", L"Running", SS_RIGHT | SS_ENDELLIPSIS | SS_NOPREFIX, IDC_STATUS, -1, -1, 0);

    /* Prompt */
    d->prompt_out = view(d, IDC_PROMPT_OUT, PAGE_PROMPT, -1, 1);
    SetWindowTextW(d->prompt_out,
                   L"Fairchild Channel F debugger prompt (F3850 CPU, 128 x 64 video RAM). Type 'help' for every command.\r\n");
    d->prompt_in = field(d, IDC_PROMPT_IN, PAGE_PROMPT, -1, 1, 0);
    SendMessageW(d->prompt_in, EM_SETCUEBANNER, TRUE,
                 (LPARAM)L"command (help, step, break, bpv, bpi, print, mem, scratch, kstack, vram, pal, ...) - Tab completes, Up/Down recall");

    /* CPU & RAM */
    for (i = 0; i < NFIELDS; i++) {
        d->fld_label[i] = label(d, kFields[i].name, PAGE_CPU, -1);
        d->fld[i] = field(d, IDC_FIELD0 + i, PAGE_CPU, -1, 1, kFields[i].digits);
    }
    for (i = 0; i < NFLAGS; i++) {
        wchar_t w[24];
        dbg_wide_from(kFlagNames[i], w, 24);
        d->flag[i] = checkbox(d, w, IDC_FLAG0 + i, PAGE_CPU, -1);
    }
    d->cpu_state = view(d, IDC_CPU_STATE, PAGE_CPU, -1, 0);
    d->scr_head = label(d, "Scratchpad r0-r63 (octal: row.column; shaded = K stack r40-r59)", PAGE_CPU, -1);
    d->scr_grid = mk(d, WS_EX_CLIENTEDGE, L"ChfDbgScratch", L"", 0, IDC_SCR_GRID, PAGE_CPU, -1, 1);
    d->scr_reg_label = label(d, "Write r", PAGE_CPU, -1);
    d->scr_reg = field(d, IDC_SCR_REG, PAGE_CPU, -1, 1, 2);
    d->scr_val_label = label(d, "(octal) =", PAGE_CPU, -1);
    d->scr_val = field(d, IDC_SCR_VAL, PAGE_CPU, -1, 1, 3);
    d->kstack_head = label(d, "K stack", PAGE_CPU, -1);
    d->kstack = view(d, IDC_KSTACK, PAGE_CPU, -1, 1);
    d->mem_label = label(d, "Memory", PAGE_CPU, -1);
    d->mem_view = view(d, IDC_MEM_VIEW, PAGE_CPU, -1, 0);
    d->ram_space = combo(d, IDC_RAM_SPACE, kSpaceNames, NSPACES, PAGE_CPU, -1);
    d->ram_prev = button(d, L"\x25C0 256", IDC_RAM_PREV, PAGE_CPU, -1);
    d->ram_next = button(d, L"256 \x25B6", IDC_RAM_NEXT, PAGE_CPU, -1);
    d->ram_goto_label = label(d, "Go to", PAGE_CPU, -1);
    d->ram_goto = field(d, IDC_RAM_GOTO, PAGE_CPU, -1, 1, 0);
    d->ram_addr_label = label(d, "Write", PAGE_CPU, -1);
    d->ram_addr = field(d, IDC_RAM_ADDR, PAGE_CPU, -1, 1, 0);
    d->ram_val_label = label(d, "=", PAGE_CPU, -1);
    d->ram_val = field(d, IDC_RAM_VAL, PAGE_CPU, -1, 1, 0);

    /* Disassembly */
    d->follow_pc = checkbox(d, L"Follow PC", IDC_FOLLOW_PC, PAGE_DISASM, -1);
    SendMessageW(d->follow_pc, BM_SETCHECK, BST_CHECKED, 0);
    d->jump_label = label(d, "Go to", PAGE_DISASM, -1);
    d->jump = field(d, IDC_JUMP, PAGE_DISASM, -1, 1, 0);
    d->disasm_hint = label(d, "Click the margin or double-click a line to toggle its breakpoint; click a line to select it; scroll to browse", PAGE_DISASM, -1);
    d->disasm = mk(d, WS_EX_CLIENTEDGE, L"ChfDbgDisasm", L"", WS_TABSTOP, IDC_DISASM, PAGE_DISASM, -1, 1);

    /* Video: the picture and what controls it ... */
    d->view_label = label(d, "View", PAGE_VIDEO, -1);
    d->view_mode = combo(d, IDC_VIEW_MODE, kViewNames, NVIEWS, PAGE_VIDEO, -1);
    d->scale_label = label(d, "Scale", PAGE_VIDEO, -1);
    d->scale_mode = combo(d, IDC_SCALE_MODE, kScaleNames, NSCALES, PAGE_VIDEO, -1);
    d->overlays_label = label(d, "Overlays", PAGE_VIDEO, -1);
    for (i = 0; i < NOVERLAYS; i++) {
        d->overlay[i] = checkbox(d, kOverlayNames[i], IDC_OVL0 + i, PAGE_VIDEO, -1);
        if (i == 0) SendMessageW(d->overlay[i], BM_SETCHECK, BST_CHECKED, 0);
    }
    d->vpic = mk(d, WS_EX_CLIENTEDGE, L"ChfDbgVideo", L"", 0, IDC_VPIC, PAGE_VIDEO, -1, 0);
    d->vstatus = label(d, "Hover over the picture for the cell under the pointer.", PAGE_VIDEO, -1);
    /* ... and the sections beside it. The strip is a tab control used as a
     * strip alone, the sections below it controls of their own. */
    d->vtabs = mk(d, 0, WC_TABCONTROLW, L"", WS_CLIPSIBLINGS | WS_TABSTOP, IDC_VTABS, PAGE_VIDEO, -1, 0);
    for (i = 0; i < SEC_COUNT; i++) {
        item.pszText = (wchar_t *)kSectionTitles[i];
        SendMessageW(d->vtabs, TCM_INSERTITEMW, (WPARAM)i, (LPARAM)&item);
    }
    d->hover_head = label(d, "Under the pointer", PAGE_VIDEO, SEC_CELL);
    d->hover_text = view(d, IDC_HOVER_TEXT, PAGE_VIDEO, SEC_CELL, 0);
    d->pin_head = label(d, "Pinned (click a cell)", PAGE_VIDEO, SEC_CELL);
    d->pin_text = view(d, IDC_PIN_TEXT, PAGE_VIDEO, SEC_CELL, 0);
    d->pin_break = button(d, L"Break on its writes", IDC_PIN_BREAK, PAGE_VIDEO, SEC_CELL);
    d->pin_clear = button(d, L"Unpin", IDC_PIN_CLEAR, PAGE_VIDEO, SEC_CELL);
    d->rows_head = label(d, "Row palettes (from columns 125 and 126)", PAGE_VIDEO, SEC_ROWS);
    d->rows_view = mk(d, WS_EX_CLIENTEDGE, L"ChfDbgRows", L"", 0, IDC_ROWS_VIEW, PAGE_VIDEO, SEC_ROWS, 1);
    d->pal_head = label(d, "The four palettes", PAGE_VIDEO, SEC_ROWS);
    d->pal_text = view(d, IDC_PAL_TEXT, PAGE_VIDEO, SEC_ROWS, 1);
    d->ports_text = view(d, IDC_PORTS_TEXT, PAGE_VIDEO, SEC_PORTS, 1);
    d->wr_row_label = label(d, "Rows", PAGE_VIDEO, SEC_WRITES);
    d->wr_row0 = field(d, IDC_WR_ROW0, PAGE_VIDEO, SEC_WRITES, 1, 3);
    d->wr_row1 = field(d, IDC_WR_ROW1, PAGE_VIDEO, SEC_WRITES, 1, 3);
    d->wr_col_label = label(d, "Columns", PAGE_VIDEO, SEC_WRITES);
    d->wr_col0 = field(d, IDC_WR_COL0, PAGE_VIDEO, SEC_WRITES, 1, 3);
    d->wr_col1 = field(d, IDC_WR_COL1, PAGE_VIDEO, SEC_WRITES, 1, 3);
    d->wr_changes = checkbox(d, L"Changes only", IDC_WR_CHANGES, PAGE_VIDEO, SEC_WRITES);
    d->wr_frame = checkbox(d, L"This frame", IDC_WR_FRAME, PAGE_VIDEO, SEC_WRITES);
    d->wr_palcols = checkbox(d, L"Palette columns", IDC_WR_PALCOLS, PAGE_VIDEO, SEC_WRITES);
    d->wr_clear = button(d, L"Clear log", IDC_WR_CLEAR, PAGE_VIDEO, SEC_WRITES);
    d->wr_counts = label(d, "", PAGE_VIDEO, SEC_WRITES);
    d->wr_text = view(d, IDC_WR_TEXT, PAGE_VIDEO, SEC_WRITES, 2);
    SetWindowTextW(d->wr_row0, L"0");
    SetWindowTextW(d->wr_row1, L"63");
    SetWindowTextW(d->wr_col0, L"0");
    SetWindowTextW(d->wr_col1, L"127");

    /* Sound & I/O */
    d->io_draw = mk(d, 0, L"ChfDbgIo", L"", 0, IDC_IO_DRAW, PAGE_IO, -1, 0);
    d->io_state = view(d, IDC_IO_STATE, PAGE_IO, -1, 0);
    d->snd_head = label(d, "Beeper mode changes", PAGE_IO, -1);
    d->snd_log = view(d, IDC_SND_LOG, PAGE_IO, -1, 2);
    d->iolog_head = label(d, "Port accesses (IN / OUT)", PAGE_IO, -1);
    d->io_log = view(d, IDC_IO_LOG, PAGE_IO, -1, 2);
    d->io_clear = button(d, L"Clear", IDC_IO_CLEAR, PAGE_IO, -1);

    /* Breakpoints */
    d->bp_list = mk(d, WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_TABSTOP,
                    IDC_BP_LIST, PAGE_BREAKS, -1, 1);
    d->bp_type = combo(d, IDC_BP_TYPE, kBpTypes, NBPTYPES, PAGE_BREAKS, -1);
    d->bp_start_label = label(d, "From", PAGE_BREAKS, -1);
    d->bp_start = field(d, IDC_BP_START, PAGE_BREAKS, -1, 1, 0);
    d->bp_end_label = label(d, "to", PAGE_BREAKS, -1);
    d->bp_end = field(d, IDC_BP_END, PAGE_BREAKS, -1, 1, 0);
    d->bp_cond_label = label(d, "if", PAGE_BREAKS, -1);
    d->bp_cond = field(d, IDC_BP_COND, PAGE_BREAKS, -1, 1, 0);
    SendMessageW(d->bp_cond, EM_SETCUEBANNER, TRUE, (LPARAM)L"optional condition, e.g. value == 2");
    d->bp_hint = label(d, kBpHints[0], PAGE_BREAKS, -1);
    d->bp_add = button(d, L"Add", IDC_BP_ADD, PAGE_BREAKS, -1);
    d->bp_remove = button(d, L"Remove", IDC_BP_REMOVE, PAGE_BREAKS, -1);
    d->bp_enable = button(d, L"Enable/Disable", IDC_BP_ENABLE, PAGE_BREAKS, -1);
    d->bp_clear = button(d, L"Clear all", IDC_BP_CLEAR, PAGE_BREAKS, -1);

    /* Cart */
    d->cart_text = view(d, IDC_CART_TEXT, PAGE_CART, -1, 1);
    d->cart_mailbox_label = label(d, "The mailbox page the console reads ($F800-$FFFF)", PAGE_CART, -1);
    d->cart_mailbox = view(d, IDC_CART_MAILBOX, PAGE_CART, -1, 1);
    d->cart_ev_label = label(d, "Cartridge events (register writes, bytes sent, page selects, swaps)", PAGE_CART, -1);
    d->cart_events = view(d, IDC_CART_EVENTS, PAGE_CART, -1, 2);

    /* FujiNet: the note and the log are Unicode (the engine's text has arrows
     * and a times sign) */
    d->fuji_title = label(d, "FujiBus packets (SLIP-decoded)", PAGE_FUJINET, -1);
    d->fuji_note = mk(d, 0, L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, IDC_FUJI_NOTE, PAGE_FUJINET, -1, 0);
    d->fuji_clear = button(d, L"Clear", IDC_FUJI_CLEAR, PAGE_FUJINET, -1);
    d->fuji_text = view(d, IDC_FUJI_TEXT, PAGE_FUJINET, -1, 2);
    d->state_title = label(d, "The cartridge's mailbox", PAGE_FUJINET, -1);
    d->fuji_state = view(d, IDC_FUJI_STATE, PAGE_FUJINET, -1, 1);

    /* The tab controls were created first, so every page control sits below
     * them in z-order and their bodies would paint over labels that have not
     * repainted since; at the bottom (with WS_CLIPSIBLINGS) they never do. */
    SetWindowPos(d->tabs, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(d->vtabs, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

/* ---- showing a page ---------------------------------------------------------------- */

static int ctl_wanted(const debugger *d, const ctl_t *c)
{
    if (c->page < 0) return 1;
    if (c->page != d->page) return 0;
    if (c->sec < 0) return 1;
    if (c->page == PAGE_VIDEO) return c->sec == d->vsec;
    return 1;
}

static void apply_visibility(debugger *d)
{
    int i;
    for (i = 0; i < d->nctl; i++)
        if (!ctl_wanted(d, &d->ctl[i])) ShowWindow(d->ctl[i].h, SW_HIDE);
    for (i = 0; i < d->nctl; i++)
        if (ctl_wanted(d, &d->ctl[i])) ShowWindow(d->ctl[i].h, SW_SHOWNA);
}

/* ---- layout --------------------------------------------------------------------------- */

static void put(HWND h, int x, int y, int w, int hgt)
{
    MoveWindow(h, x, y, w > 1 ? w : 1, hgt > 1 ? hgt : 1, TRUE);
}

/* A control's text width in the UI font. */
static int text_width(debugger *d, HWND h)
{
    wchar_t t[160];
    SIZE sz = { 0, 0 };
    HDC dc = GetDC(d->hwnd);
    HGDIOBJ old = SelectObject(dc, d->ui);
    GetWindowTextW(h, t, 160);
    GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz);
    SelectObject(dc, old);
    ReleaseDC(d->hwnd, dc);
    return sz.cx;
}

static int btn_w(debugger *d, HWND h)
{
    return text_width(d, h) + S(24);
}

/* Checkboxes (or any text controls) flowing left to right from x0 and
 * wrapping at `right`; returns the y of the row below. */
static int flow(debugger *d, HWND *h, int n, int x0, int y, int right)
{
    int x = x0, i;
    for (i = 0; i < n; i++) {
        const int w = text_width(d, h[i]) + S(28);
        if (x + w > right && x > x0) { x = x0; y += d->row_h; }
        put(h[i], x, y, w, d->row_h);
        x += w + S(6);
    }
    return y + d->row_h + S(4);
}

static void layout_cpu(debugger *d, int px, int py, int pw, int ph)
{
    const int rh = d->row_h, gap = S(4), lw = S(468);
    const int fcol = S(152), flab = S(44), fed = S(70);
    int y = py, i, sw, sh, sx, kx, kw, top_h, mem_y, view_w, view_h;
    HWND flags[NFLAGS];

    /* the top zone: registers, the scratchpad, the K stack */
    dbg_scratch_size(d, &sw, &sh);
    for (i = 0; i < NFIELDS; i++) {
        const int cx = px + (i % 3) * fcol, cy = y + (i / 3) * (rh + gap);
        put(d->fld_label[i], cx, cy + (rh - d->ui_h) / 2, flab, d->ui_h + 2);
        put(d->fld[i], cx + flab, cy, fed, rh);
    }
    y += ((NFIELDS + 2) / 3) * (rh + gap) + S(2);
    for (i = 0; i < NFLAGS; i++) flags[i] = d->flag[i];
    y = flow(d, flags, NFLAGS, px, y, px + lw);
    top_h = sh + d->ui_h + S(10) + rh + S(8);          /* the scratchpad's zone */
    put(d->cpu_state, px, y, lw, py + top_h - y > 5 * d->mono_h ? py + top_h - y : 5 * d->mono_h);

    sx = px + lw + S(14);
    put(d->scr_head, sx, py, sw + S(60), d->ui_h + 2);
    put(d->scr_grid, sx, py + d->ui_h + S(6), sw + 4, sh + 4);
    {
        const int wy = py + d->ui_h + S(6) + sh + 4 + S(6);
        put(d->scr_reg_label, sx, wy + (rh - d->ui_h) / 2, S(54), d->ui_h + 2);
        put(d->scr_reg, sx + S(56), wy, S(38), rh);
        put(d->scr_val_label, sx + S(100), wy + (rh - d->ui_h) / 2, S(60), d->ui_h + 2);
        put(d->scr_val, sx + S(162), wy, S(48), rh);
    }
    kx = sx + sw + S(18);
    kw = px + pw - kx;
    put(d->kstack_head, kx, py, kw > S(60) ? kw : S(60), d->ui_h + 2);
    put(d->kstack, kx, py + d->ui_h + S(6), kw > S(60) ? kw : S(60), sh + 4 - S(2));

    /* the memory below */
    mem_y = py + top_h + S(10);
    put(d->mem_label, px, mem_y, pw, d->ui_h + 2);
    mem_y += d->ui_h + S(6);
    view_w = (8 + 16 * 3) * d->mono_w + S(18);
    view_h = ph - (mem_y - py);
    if (view_h < 6 * d->mono_h) view_h = 6 * d->mono_h;
    if (view_h > (MEM_ROWS + 1) * d->mono_h + S(8)) view_h = (MEM_ROWS + 1) * d->mono_h + S(8);
    put(d->mem_view, px, mem_y, view_w, view_h);
    {
        int x = px + view_w + S(16), yy = mem_y;
        put(d->ram_space, x, yy, S(170), rh + S(120));
        yy += rh + S(8);
        put(d->ram_prev, x, yy, S(74), rh + 2);
        put(d->ram_next, x + S(82), yy, S(74), rh + 2);
        yy += rh + S(10);
        put(d->ram_goto_label, x, yy + (rh - d->ui_h) / 2, S(44), d->ui_h + 2);
        put(d->ram_goto, x + S(48), yy, S(90), rh);
        yy += rh + S(8);
        put(d->ram_addr_label, x, yy + (rh - d->ui_h) / 2, S(44), d->ui_h + 2);
        put(d->ram_addr, x + S(48), yy, S(90), rh);
        put(d->ram_val_label, x + S(144), yy + (rh - d->ui_h) / 2, S(14), d->ui_h + 2);
        put(d->ram_val, x + S(160), yy, S(50), rh);
    }
}

static void layout_video(debugger *d, int px, int py, int pw, int ph)
{
    const int rh = d->row_h, gap = S(8);
    int rw = pw * 40 / 100, lw, y = py, cap_w, th, rx, sx, sy, sw, sh;
    if (rw < S(400)) rw = S(400);
    if (rw > S(560)) rw = S(560);
    lw = pw - rw - gap;
    cap_w = S(64);

    put(d->view_label, px, y + (rh - d->ui_h) / 2, S(40), d->ui_h + 2);
    put(d->view_mode, px + S(42), y, S(250), rh + S(160));
    put(d->scale_label, px + S(304), y + (rh - d->ui_h) / 2, S(42), d->ui_h + 2);
    put(d->scale_mode, px + S(348), y, S(70), rh + S(120));
    y += rh + S(4);
    put(d->overlays_label, px, y + (rh - d->ui_h) / 2, cap_w, d->ui_h + 2);
    y = flow(d, d->overlay, NOVERLAYS, px + cap_w, y, px + lw);
    put(d->vstatus, px, py + ph - rh, lw, rh);
    put(d->vpic, px, y, lw, py + ph - rh - S(4) - y);

    rx = px + lw + gap;
    th = chf_place_tab_strip(d->vtabs, rx, py, rw);
    sx = rx;
    sy = py + th + S(4);
    sw = rw;
    sh = ph - th - S(4);

    /* Cell */
    {
        const int half = (sh - 2 * (d->ui_h + S(6)) - rh - S(16)) / 2;
        int cy = sy;
        put(d->hover_head, sx, cy, sw, d->ui_h + 2);
        cy += d->ui_h + S(4);
        put(d->hover_text, sx, cy, sw, half > 5 * d->mono_h ? half : 5 * d->mono_h);
        cy += (half > 5 * d->mono_h ? half : 5 * d->mono_h) + S(8);
        put(d->pin_head, sx, cy, sw, d->ui_h + 2);
        cy += d->ui_h + S(4);
        put(d->pin_text, sx, cy, sw, half > 5 * d->mono_h ? half : 5 * d->mono_h);
        cy += (half > 5 * d->mono_h ? half : 5 * d->mono_h) + S(8);
        put(d->pin_break, sx, cy, btn_w(d, d->pin_break), rh + 2);
        put(d->pin_clear, sx + btn_w(d, d->pin_break) + S(8), cy, btn_w(d, d->pin_clear), rh + 2);
    }
    /* Rows & palettes */
    {
        int rwid, rhgt;
        dbg_rows_size(d, &rwid, &rhgt);
        put(d->rows_head, sx, sy, sw, d->ui_h + 2);
        put(d->rows_view, sx, sy + d->ui_h + S(4), rwid + 4, rhgt + 4 < sh - d->ui_h - S(4) ? rhgt + 4 : sh - d->ui_h - S(4));
        put(d->pal_head, sx + rwid + S(14), sy, sw - rwid - S(14), d->ui_h + 2);
        put(d->pal_text, sx + rwid + S(14), sy + d->ui_h + S(4), sw - rwid - S(14), sh - d->ui_h - S(4));
    }
    /* Ports */
    put(d->ports_text, sx, sy, sw, sh);
    /* Writes */
    {
        int wy = sy, x = sx;
        put(d->wr_row_label, x, wy + (rh - d->ui_h) / 2, S(40), d->ui_h + 2);
        put(d->wr_row0, x + S(42), wy, S(44), rh);
        put(d->wr_row1, x + S(90), wy, S(44), rh);
        put(d->wr_col_label, x + S(150), wy + (rh - d->ui_h) / 2, S(60), d->ui_h + 2);
        put(d->wr_col0, x + S(212), wy, S(44), rh);
        put(d->wr_col1, x + S(260), wy, S(44), rh);
        put(d->wr_clear, x + sw - btn_w(d, d->wr_clear), wy - 1, btn_w(d, d->wr_clear), rh + 2);
        wy += rh + S(4);
        {
            HWND cb[3];
            cb[0] = d->wr_changes; cb[1] = d->wr_frame; cb[2] = d->wr_palcols;
            wy = flow(d, cb, 3, sx, wy, sx + sw);
        }
        put(d->wr_counts, sx, wy, sw, d->ui_h + 2);
        wy += d->ui_h + S(6);
        put(d->wr_text, sx, wy, sw, sy + sh - wy);
    }
}

static void layout_io(debugger *d, int px, int py, int pw, int ph)
{
    int dw, dh, lw, rx, rw;
    dbg_io_draw_size(d, &dw, &dh);
    lw = dw + S(8);
    if (lw < S(360)) lw = S(360);
    rx = px + lw + S(16);
    rw = px + pw - rx;
    put(d->io_draw, px, py, lw, dh);
    {
        const int sy = py + dh + S(6), sth = 6 * d->mono_h + S(8);
        put(d->io_state, px, sy, lw, sth);
        put(d->snd_head, px, sy + sth + S(8), lw, d->ui_h + 2);
        put(d->snd_log, px, sy + sth + S(8) + d->ui_h + S(4), lw, py + ph - (sy + sth + S(8) + d->ui_h + S(4)));
    }
    put(d->iolog_head, rx, py + (d->row_h - d->ui_h) / 2, rw - S(80), d->ui_h + 2);
    put(d->io_clear, rx + rw - btn_w(d, d->io_clear), py, btn_w(d, d->io_clear), d->row_h);
    put(d->io_log, rx, py + d->row_h + S(6), rw, ph - d->row_h - S(6));
}

static void layout_breaks(debugger *d, int px, int py, int pw, int ph)
{
    const int rh = d->row_h;
    int ry = py + ph - 3 * rh - S(22), x = px, cond_w;
    put(d->bp_list, px, py, pw, ry - py - S(6));
    put(d->bp_type, x, ry, S(150), rh + S(160));
    x += S(160);
    put(d->bp_start_label, x, ry + (rh - d->ui_h) / 2, S(36), d->ui_h + 2);
    put(d->bp_start, x + S(40), ry, S(110), rh);
    x += S(158);
    put(d->bp_end_label, x, ry + (rh - d->ui_h) / 2, S(18), d->ui_h + 2);
    put(d->bp_end, x + S(20), ry, S(110), rh);
    x += S(138);
    put(d->bp_cond_label, x, ry + (rh - d->ui_h) / 2, S(14), d->ui_h + 2);
    cond_w = px + pw - S(76) - (x + S(18));
    put(d->bp_cond, x + S(18), ry, cond_w > S(80) ? cond_w : S(80), rh);
    put(d->bp_add, px + pw - S(68), ry - 1, S(68), rh + 2);
    ry += rh + S(6);
    put(d->bp_hint, px, ry, pw, d->ui_h + 2);
    ry += d->ui_h + S(10);
    put(d->bp_remove, px, ry, S(96), rh + 2);
    put(d->bp_enable, px + S(104), ry, S(130), rh + 2);
    put(d->bp_clear, px + S(242), ry, S(96), rh + 2);
}

static void layout_cart(debugger *d, int px, int py, int pw, int ph)
{
    const int lw = pw * 44 / 100, rx = px + lw + S(14), rw = pw - lw - S(14);
    const int top = (ph - 2 * (d->ui_h + S(6)) - S(8)) * 58 / 100;
    put(d->cart_text, px, py, lw, ph);
    put(d->cart_mailbox_label, rx, py, rw, d->ui_h + 2);
    put(d->cart_mailbox, rx, py + d->ui_h + S(6), rw, top);
    put(d->cart_ev_label, rx, py + d->ui_h + S(6) + top + S(8), rw, d->ui_h + 2);
    put(d->cart_events, rx, py + 2 * (d->ui_h + S(6)) + top + S(8), rw, ph - (2 * (d->ui_h + S(6)) + top + S(8)));
}

static void layout_fujinet(debugger *d, int px, int py, int pw, int ph)
{
    const int rh = d->row_h;
    int rw = pw / 3 > S(340) ? S(340) : pw / 3 > S(240) ? pw / 3 : S(240);
    const int lw = pw - rw - S(12);
    put(d->fuji_title, px, py, lw, d->ui_h + 2);
    put(d->fuji_clear, px + lw - S(76), py + d->ui_h + S(4), S(76), rh);
    put(d->fuji_note, px, py + d->ui_h + S(4) + (rh - d->ui_h) / 2, lw - S(84), d->ui_h + 2);
    put(d->fuji_text, px, py + d->ui_h + rh + S(10), lw, ph - (d->ui_h + rh + S(10)));
    put(d->state_title, px + lw + S(12), py, rw, d->ui_h + 2);
    put(d->fuji_state, px + lw + S(12), py + d->ui_h + S(6), rw, ph - (d->ui_h + S(6)));
}

/* The toolbar and tabs, then the page that is showing. */
static void layout(debugger *d)
{
    RECT client, page, tr;
    int y = S(8), bx = S(8), px, py, pw, ph, tab_top, tabs_w, sx;
    const int rh = d->row_h;
    HWND bar[10];
    int i;

    GetClientRect(d->hwnd, &client);
    bar[0] = d->run_btn; bar[1] = d->step_btn; bar[2] = d->over_btn; bar[3] = d->out_btn; bar[4] = d->frame_btn;
    bar[5] = d->scan_btn; bar[6] = d->torun_btn; bar[7] = d->load_symbols; bar[8] = d->save; bar[9] = d->help_btn;
    for (i = 0; i < 10; i++) {
        const int w = bar[i] == d->run_btn ? S(88) : btn_w(d, bar[i]);
        put(bar[i], bx, y, w, rh + 2);
        bx += w + S(6);
    }

    /* The tabs are a strip alone: the pages sit on the window's own face,
     * where the labels' background is the same grey. The stop reason sits
     * at the strip's right. */
    tab_top = y + rh + S(10);
    page.top = tab_top + chf_place_tab_strip(d->tabs, S(8), tab_top, client.right - S(16)) + S(8);
    SendMessageW(d->tabs, TCM_GETITEMRECT, PAGE_COUNT - 1, (LPARAM)&tr);
    tabs_w = tr.right + S(6);
    if (tabs_w > client.right - S(16)) tabs_w = client.right - S(16);
    MoveWindow(d->tabs, S(8), tab_top, tabs_w, page.top - S(8) - tab_top, TRUE);
    sx = S(8) + tabs_w + S(12);
    put(d->status, sx, tab_top + S(3), client.right - sx - S(8) > S(40) ? client.right - sx - S(8) : S(40), d->ui_h + 2);
    page.left = S(8);
    page.right = client.right - S(8);
    page.bottom = client.bottom - S(8);
    px = page.left + S(2);
    py = page.top;
    pw = page.right - page.left - S(4);
    ph = page.bottom - page.top;
    if (pw < S(200)) pw = S(200);
    if (ph < S(200)) ph = S(200);

    switch (d->page) {
    case PAGE_PROMPT:
        put(d->prompt_out, px, py, pw, ph - rh - S(8));
        put(d->prompt_in, px, py + ph - rh - S(2), pw, rh);
        break;
    case PAGE_CPU: layout_cpu(d, px, py, pw, ph); break;
    case PAGE_DISASM:
        put(d->follow_pc, px, py + 2, S(96), rh);
        put(d->jump_label, px + S(104), py + (rh - d->ui_h) / 2 + 2, S(46), d->ui_h + 2);
        put(d->jump, px + S(152), py, S(150), rh);
        put(d->disasm_hint, px + S(312), py + (rh - d->ui_h) / 2 + 2, pw - S(312) > S(40) ? pw - S(312) : S(40),
            d->ui_h + 2);
        put(d->disasm, px, py + rh + S(8), pw, ph - rh - S(8));
        break;
    case PAGE_VIDEO: layout_video(d, px, py, pw, ph); break;
    case PAGE_IO: layout_io(d, px, py, pw, ph); break;
    case PAGE_BREAKS: layout_breaks(d, px, py, pw, ph); break;
    case PAGE_CART: layout_cart(d, px, py, pw, ph); break;
    case PAGE_FUJINET: layout_fujinet(d, px, py, pw, ph); break;
    default: break;
    }
}

static void show_page(debugger *d, int page)
{
    d->page = page;
    /* Help, F1 and Ctrl+Shift+H open the tab's section */
    chf_help_set_context(d->hwnd, kHelpContexts[page], 1);
    apply_visibility(d);
    layout(d);
    refresh_page(d, 1);
}

/* ---- fonts and DPI --------------------------------------------------------------------- */

static void make_fonts(debugger *d)
{
    SIZE cell;
    d->ui = chf_make_font(d->dpi, 0, FW_NORMAL, NULL);
    d->bold = chf_make_font(d->dpi, 0, FW_BOLD, NULL);
    d->mono = chf_make_font(d->dpi, 13, FW_NORMAL, CHF_MONO_FACE);
    cell = chf_font_cell(d->hwnd, d->mono);
    d->mono_w = cell.cx;
    d->mono_h = cell.cy;
    cell = chf_font_cell(d->hwnd, d->ui);
    d->ui_h = cell.cy;
    d->row_h = S(24) > d->ui_h + S(8) ? S(24) : d->ui_h + S(8);
}

static void apply_fonts(debugger *d)
{
    int i;
    for (i = 0; i < d->nctl; i++)
        SendMessageW(d->ctl[i].h, WM_SETFONT, (WPARAM)(d->ctl[i].mono ? d->mono : d->ui), TRUE);
}

/* ---- attach / detach -------------------------------------------------------------- */

static void attach(debugger *d)
{
    chfdebug_attach(d->dbg);
    d->seen_gen = chfdebug_generation(d->dbg);
    d->was_stopped = 1;
    d->hover_row = d->hover_col = -1;
    d->prev_valid = 0;
    d->prev_row_valid = 0;
    d->changed_mask = 0;
    refresh_all(d);
    SetTimer(d->hwnd, TIMER_REFRESH, 100, NULL);
}

static void detach_and_hide(debugger *d)
{
    KillTimer(d->hwnd, TIMER_REFRESH);
    chfdebug_detach(d->dbg);
    ShowWindow(d->hwnd, SW_HIDE);
}

/* ---- window proc -------------------------------------------------------------------- */

/* Stopped, Run wears the accent with dark text on it. */
static void draw_run_button(debugger *d, const DRAWITEMSTRUCT *di)
{
    wchar_t text[32];
    const int stopped = chfdebug_is_stopped(d->dbg);
    RECT r = di->rcItem;
    if (stopped) {
        FillRect(di->hDC, &r, d->accent);
        FrameRect(di->hDC, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetTextColor(di->hDC, RGB(0x14, 0x14, 0x14));
    } else {
        DrawFrameControl(di->hDC, &r, DFC_BUTTON, DFCS_BUTTONPUSH | ((di->itemState & ODS_SELECTED) ? DFCS_PUSHED : 0));
        SetTextColor(di->hDC, GetSysColor(COLOR_BTNTEXT));
    }
    SetBkMode(di->hDC, TRANSPARENT);
    SelectObject(di->hDC, d->ui);
    GetWindowTextW(di->hwndItem, text, 32);
    DrawTextW(di->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (di->itemState & ODS_FOCUS) {
        InflateRect(&r, -3, -3);
        DrawFocusRect(di->hDC, &r);
    }
}

static void on_command(debugger *d, int id, int code, HWND src)
{
    (void)src;
    if (id >= IDC_FLAG0 && id <= IDC_FLAG_LAST && code == BN_CLICKED) {
        if (chfdebug_is_stopped(d->dbg))
            chfdebug_cpu_set(d->dbg, kFlagIds[id - IDC_FLAG0],
                            SendMessageW(d->flag[id - IDC_FLAG0], BM_GETCHECK, 0, 0) == BST_CHECKED);
        refresh_cpu(d);
        return;
    }
    if (id >= IDC_SAVE_KIND0 && id <= IDC_SAVE_KIND_LAST) { save_file(d, id - IDC_SAVE_KIND0); return; }
    if (id >= IDC_OVL0 && id <= IDC_OVL_LAST) {
        if (code == BN_CLICKED) refresh_video(d, 0);
        return;
    }
    switch (id) {
    case IDC_RUN: toggle_run(d); break;
    case IDC_STEP: chfdebug_step(d->dbg); refresh_all(d); break;
    case IDC_OVER: chfdebug_step_over(d->dbg); refresh_all(d); break;
    case IDC_OUT: chfdebug_step_out(d->dbg); refresh_all(d); break;
    case IDC_SCAN: chfdebug_scanline(d->dbg, 1); refresh_all(d); break;
    case IDC_FRAME: chfdebug_frame(d->dbg, 1); refresh_all(d); break;
    case IDC_TORUN: run_to_cursor(d); break;
    case IDC_LOAD_SYMBOLS: symbols_menu(d); break;
    case IDC_SYMS_FILE: load_symbols(d, 0); break;
    case IDC_SYMS_CART: load_symbols(d, 1); break;
    case IDC_SAVE: save_menu(d); break;
    case IDC_HELP_BTN: chf_help_for(d->hwnd); break;
    case IDC_FOLLOW_PC: dbg_refresh_disasm(d); break;
    case IDC_VIEW_MODE:
    case IDC_SCALE_MODE:
        if (code == CBN_SELCHANGE) { dbg_hover_changed(d, -1, -1); refresh_video(d, 0); }
        break;
    case IDC_PIN_BREAK:
        if (d->pin_row >= 0) dbg_break_on_cell(d, d->pin_row, d->pin_col);
        break;
    case IDC_PIN_CLEAR: dbg_pin_cell(d, -1, -1); break;
    case IDC_WR_CHANGES: case IDC_WR_FRAME: case IDC_WR_PALCOLS: refresh_writes(d); break;
    case IDC_WR_CLEAR: chfdebug_vram_writes_clear(d->dbg); refresh_writes(d); break;
    case IDC_IO_CLEAR: chfdebug_io_events_clear(d->dbg); refresh_io(d); break;
    case IDC_RAM_SPACE:
        if (code == CBN_SELCHANGE) { d->mem_base = 0; refresh_mem(d); }
        break;
    case IDC_RAM_PREV: mem_page(d, -1); break;
    case IDC_RAM_NEXT: mem_page(d, 1); break;
    case IDC_BP_TYPE:
        if (code == CBN_SELCHANGE) {
            const int sel = (int)SendMessageW(d->bp_type, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < NBPTYPES) dbg_label_set(d->bp_hint, kBpHints[sel]);
        }
        break;
    case IDC_BP_ADD: add_breakpoint(d); break;
    case IDC_BP_REMOVE:
    case IDC_BP_ENABLE: {
        const int sel = (int)SendMessageW(d->bp_list, LB_GETCURSEL, 0, 0);
        if (sel >= 0 && sel < d->nbps) {
            if (id == IDC_BP_REMOVE) chfdebug_breakpoint_remove(d->dbg, d->bps[sel].id);
            else chfdebug_breakpoint_enable(d->dbg, d->bps[sel].id, !d->bps[sel].enabled);
            refresh_bps(d);
            dbg_refresh_disasm(d);
        }
        break;
    }
    case IDC_BP_LIST:
        if (code == LBN_DBLCLK) {
            const int sel = (int)SendMessageW(d->bp_list, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < d->nbps && (d->bps[sel].type & CHFDEBUG_BP_EXEC)) {
                char a[16];
                snprintf(a, sizeof a, "$%04X", d->bps[sel].start);
                TabCtrl_SetCurSel(d->tabs, PAGE_DISASM);
                show_page(d, PAGE_DISASM);
                jump_to(d, a);
            }
        }
        break;
    case IDC_BP_CLEAR:
        chfdebug_breakpoint_clear(d->dbg);
        refresh_bps(d);
        dbg_refresh_disasm(d);
        break;
    case IDC_FUJI_CLEAR:
        chfdebug_fujinet_trace_clear(d->dbg);
        refresh_fujinet(d, 1);
        break;
    case IDCANCEL: detach_and_hide(d); break;
    default: break;
    }
}

/* The timer: while stopped, only a changed generation (or a stop or go)
 * refreshes; while running, the Video and Sound & I/O tabs follow every tick
 * (ten a second) and the other pages twice a second. A minimised window
 * refreshes nothing, and only the tab showing is read. */
static void on_timer(debugger *d)
{
    unsigned gen;
    int stopped;
    if (!IsWindowVisible(d->hwnd) || IsIconic(d->hwnd)) return;
    gen = chfdebug_generation(d->dbg);
    stopped = chfdebug_is_stopped(d->dbg);
    if (gen != d->seen_gen || stopped != d->was_stopped) {
        d->seen_gen = gen;
        d->was_stopped = stopped;
        d->running_ticks = 0;
        refresh_all(d);
        return;
    }
    if (stopped) return;
    d->running_ticks++;
    if (d->page == PAGE_VIDEO || d->page == PAGE_IO) {
        const int slow = d->running_ticks >= TICKS_SLOW;
        refresh_page(d, slow);
        if (slow) { d->running_ticks = 0; refresh_status(d); }
    } else if (d->running_ticks >= TICKS_SLOW) {
        d->running_ticks = 0;
        refresh_status(d);
        refresh_page(d, 1);
    }
}

static LRESULT CALLBACK dbg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_debugger;
    if (!d || d->hwnd != hwnd) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_SIZE:
        if (d->nctl > 0 && d->vtabs) {
            layout(d);
            /* Fit scales the picture to its window: redraw it at the new size */
            if (d->page == PAGE_VIDEO && IsWindowVisible(hwnd) && chfdebug_is_attached(d->dbg)) refresh_video(d, 0);
        }
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = S(960);
        mm->ptMinTrackSize.y = S(620);
        return 0;
    }
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)lp;
        HFONT old_ui = d->ui, old_bold = d->bold, old_mono = d->mono;
        d->dpi = HIWORD(wp);
        make_fonts(d);
        apply_fonts(d);
        DeleteObject(old_ui);
        DeleteObject(old_bold);
        DeleteObject(old_mono);
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        layout(d);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_REFRESH) on_timer(d);
        return 0;
    case WM_DBG_ACCEPT:
        on_accept(d, (int)wp);
        return 0;
    case WM_DBG_TAB:
        complete_prompt(d);
        return 0;
    case WM_DBG_HISTORY:
        history_step(d, (int)wp);
        return 0;
    case WM_NOTIFY: {
        const NMHDR *h = (const NMHDR *)lp;
        if (h->idFrom == IDC_TABS && h->code == (UINT)TCN_SELCHANGE) {
            show_page(d, (int)SendMessageW(d->tabs, TCM_GETCURSEL, 0, 0));
            return 0;
        }
        if (h->idFrom == IDC_VTABS && h->code == (UINT)TCN_SELCHANGE) {
            d->vsec = (int)SendMessageW(d->vtabs, TCM_GETCURSEL, 0, 0);
            apply_visibility(d);
            layout(d);
            refresh_video(d, 1);
            return 0;
        }
        break;
    }
    case WM_DRAWITEM:
        if (wp == IDC_RUN) { draw_run_button(d, (const DRAWITEMSTRUCT *)lp); return TRUE; }
        break;
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == d->status || (HWND)lp == d->disasm_hint || (HWND)lp == d->fuji_note ||
            (HWND)lp == d->vstatus || (HWND)lp == d->bp_hint || (HWND)lp == d->wr_counts) {
            SetTextColor((HDC)wp, GetSysColor(COLOR_GRAYTEXT));
            SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    case WM_COMMAND:
        on_command(d, LOWORD(wp), HIWORD(wp), (HWND)lp);
        return 0;
    case WM_CLOSE:
        /* Hide and let the machine run; F12 brings it back, stopped. */
        detach_and_hide(d);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_REFRESH);
        DeleteObject(d->ui);
        DeleteObject(d->bold);
        DeleteObject(d->mono);
        DeleteObject(d->accent);
        DeleteObject(d->accent_dark);
        DestroyAcceleratorTable(d->accel);
        free(d->vimg.px);
        free(d);
        g_debugger = NULL;
        return 0;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- entry points ---------------------------------------------------------------------- */

static void register_class(HINSTANCE inst, const wchar_t *name, WNDPROC proc, HBRUSH background)
{
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = background;
    wc.lpszClassName = name;
    RegisterClassW(&wc);
}

/* CHF_DEBUGGER_TAB: a page number, or its name (prompt, cpu, disassembly,
 * video, io, breakpoints, cart, fujinet). */
static int page_from_env(void)
{
    const char *tab = getenv("CHF_DEBUGGER_TAB");
    int i;
    if (!tab || !*tab) return PAGE_PROMPT;
    if (tab[0] >= '0' && tab[0] <= '9') {
        i = atoi(tab);
        return (i >= 0 && i < PAGE_COUNT) ? i : PAGE_PROMPT;
    }
    for (i = 0; i < PAGE_COUNT; i++)
        if (lstrcmpiA(tab, kPageNames[i]) == 0) return i;
    return PAGE_PROMPT;
}

void chf_debugger_show(HWND parent, chfsession *session)
{
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES };
    HINSTANCE inst;
    debugger *d;
    ACCEL accels[7];
    SIZE outer;

    if (g_debugger) {
        if (!IsWindowVisible(g_debugger->hwnd)) {
            ShowWindow(g_debugger->hwnd, SW_SHOW);
            attach(g_debugger);
        } else {
            chfdebug_stop(g_debugger->dbg);
            refresh_all(g_debugger);
        }
        SetForegroundWindow(g_debugger->hwnd);
        return;
    }

    InitCommonControlsEx(&icc);
    inst = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    register_class(inst, L"ChfDebuggerWindow", dbg_proc, (HBRUSH)(COLOR_BTNFACE + 1));
    dbg_views_register(inst);

    d = calloc(1, sizeof *d);
    if (!d) return;
    d->session = session;
    d->dbg = chfsession_debugger(session);
    d->inst = inst;
    d->fuji_count = -1;
    d->vsec = SEC_CELL;
    d->hover_row = d->hover_col = -1;
    d->pin_row = d->pin_col = -1;
    d->sel_addr = -1;
    d->dpi = chf_window_dpi(parent);
    g_debugger = d;

    d->hwnd = CreateWindowExW(0, L"ChfDebuggerWindow", L"FujiNet Go Channel F Debugger", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1180, 820, NULL, NULL, inst, NULL);
    if (!d->hwnd) { free(d); g_debugger = NULL; return; }

    d->dpi = chf_window_dpi(d->hwnd);        /* the monitor it actually opened on */
    make_fonts(d);
    d->accent = make_brush(CHFSESSION_ACCENT_RGB);
    d->accent_dark = make_brush(CHFSESSION_ACCENT_DARK_RGB);
    outer = chf_outer_size(d->hwnd, S(1160), S(800), FALSE);
    SetWindowPos(d->hwnd, NULL, 0, 0, outer.cx, outer.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    build_controls(d);

    /* F5/F7/F8/Shift+F8/F12 work wherever the focus is inside the window. */
    accels[0].fVirt = FVIRTKEY;          accels[0].key = VK_F5;  accels[0].cmd = IDC_RUN;
    accels[1].fVirt = FVIRTKEY;          accels[1].key = VK_F7;  accels[1].cmd = IDC_STEP;
    accels[2].fVirt = FVIRTKEY;          accels[2].key = VK_F8;  accels[2].cmd = IDC_OVER;
    accels[3].fVirt = FVIRTKEY | FSHIFT; accels[3].key = VK_F8;  accels[3].cmd = IDC_OUT;
    accels[4].fVirt = FVIRTKEY;          accels[4].key = VK_F12; accels[4].cmd = IDCANCEL;
    accels[5].fVirt = FVIRTKEY | FSHIFT; accels[5].key = VK_F7;  accels[5].cmd = IDC_FRAME;
    accels[6].fVirt = FVIRTKEY | FCONTROL; accels[6].key = VK_F7; accels[6].cmd = IDC_SCAN;
    d->accel = CreateAcceleratorTableW(accels, 7);

    d->page = page_from_env();
    {   /* CHF_DEBUGGER_SECTION: the Video tab's section (0-3), for scripted checks */
        const char *sec = getenv("CHF_DEBUGGER_SECTION");
        if (sec && *sec >= '0' && *sec < '0' + SEC_COUNT) d->vsec = *sec - '0';
        SendMessageW(d->vtabs, TCM_SETCURSEL, (WPARAM)d->vsec, 0);
    }
    SendMessageW(d->tabs, TCM_SETCURSEL, (WPARAM)d->page, 0);
    chf_help_set_context(d->hwnd, kHelpContexts[d->page], 1);
    apply_visibility(d);
    layout(d);

    ShowWindow(d->hwnd, SW_SHOW);
    attach(d);
}

int chf_debugger_pretranslate(MSG *msg)
{
    if (!g_debugger || !g_debugger->accel) return 0;
    if (msg->hwnd != g_debugger->hwnd && !IsChild(g_debugger->hwnd, msg->hwnd)) return 0;
    if (msg->message == WM_KEYDOWN && msg->wParam == VK_F12) { detach_and_hide(g_debugger); return 1; }
    return TranslateAcceleratorW(g_debugger->hwnd, g_debugger->accel, msg) ? 1 : 0;
}

int chf_debugger_visible(void)
{
    return g_debugger && IsWindowVisible(g_debugger->hwnd);
}

void chf_debugger_toggle(HWND parent, chfsession *session)
{
    if (chf_debugger_visible()) detach_and_hide(g_debugger);
    else chf_debugger_show(parent, session);
}
