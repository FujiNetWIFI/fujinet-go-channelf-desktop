/*
 * The debugger window's hand-drawn views (Win32): the disassembly, the video
 * picture, the scratchpad grid, the 64-row palette strip and the controllers'
 * lamps with the sound level. Each is a small window class that paints what
 * dbg_window.c last read from the engine, double-buffered; none of them reads
 * the engine on its own.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <windowsx.h>

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "dbg_internal.h"

#define COMMENT_COL 56       /* where a disassembly line's comment starts, in characters */

/* ---- drawing helpers --------------------------------------------------------------- */

/* A hand-drawn view paints into a memory DC and blits once (they refresh
 * live while the machine runs). */
typedef void (*paint_fn)(debugger *d, HWND hwnd, HDC dc, const RECT *c);

static COLORREF xrgb_ref(uint32_t px)
{
    return RGB((px >> 16) & 0xff, (px >> 8) & 0xff, px & 0xff);
}

static void fill_rgb(HDC dc, const RECT *r, COLORREF colour)
{
    HBRUSH b = CreateSolidBrush(colour);
    FillRect(dc, r, b);
    DeleteObject(b);
}

static void frame_rgb(HDC dc, const RECT *r, COLORREF colour, int w)
{
    HBRUSH b = CreateSolidBrush(colour);
    RECT t = *r;
    int i;
    for (i = 0; i < w; i++) {
        FrameRect(dc, &t, b);
        InflateRect(&t, -1, -1);
    }
    DeleteObject(b);
}

/* UTF-8 through TextOutW at a point. */
static void text_at(HDC dc, int x, int y, const char *utf8, int n)
{
    wchar_t w[512];
    char tmp[512];
    if (n < 0 || n > (int)sizeof tmp - 1) n = (int)sizeof tmp - 1;
    snprintf(tmp, sizeof tmp, "%.*s", n, utf8);
    dbg_wide_from(tmp, w, 512);
    TextOutW(dc, x, y, w, (int)wcslen(w));
}

/* UTF-8 into a rectangle through DrawTextW. */
static void text_in(HDC dc, const char *utf8, RECT *r, UINT fmt)
{
    wchar_t w[512];
    dbg_wide_from(utf8, w, 512);
    DrawTextW(dc, w, -1, r, fmt | DT_NOPREFIX);
}

/* A top-down 32-bit DIB onto dc at 1:1. */
static void blit(HDC dc, const image *im, int x, int y)
{
    BITMAPINFO bmi;
    if (!im->px || im->w <= 0 || im->h <= 0) return;
    memset(&bmi, 0, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
    bmi.bmiHeader.biWidth = im->w;
    bmi.bmiHeader.biHeight = -im->h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, x, y, im->w, im->h, 0, 0, im->w, im->h, im->px, &bmi, DIB_RGB_COLORS, SRCCOPY);
}

static void paint_buffered(debugger *d, HWND hwnd, paint_fn fn)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT c;
    HDC mem;
    HBITMAP bmp;
    HGDIOBJ oldbmp, oldfont;
    GetClientRect(hwnd, &c);
    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, c.right > 0 ? c.right : 1, c.bottom > 0 ? c.bottom : 1);
    oldbmp = SelectObject(mem, bmp);
    oldfont = SelectObject(mem, d->mono);
    SetBkMode(mem, TRANSPARENT);
    fn(d, hwnd, mem, &c);
    BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldfont);
    SelectObject(mem, oldbmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

#define ON_ACCENT RGB(0x14, 0x14, 0x14)      /* dark text on the accent */

/* ---- the disassembly ---------------------------------------------------------------- */

/* Drawn by hand, one row per line: the breakpoint gutter, address, bytes,
 * label, instruction, and the comment (a branch target's label) in grey. The
 * PC's line is filled with the accent, dark text on it; the selected line (Run
 * to Cursor) with the system highlight. */
static void paint_disasm(debugger *d, HWND hwnd, HDC dc, const RECT *c)
{
    const int gutter = S(18);
    int i;
    (void)hwnd;
    FillRect(dc, c, GetSysColorBrush(COLOR_WINDOW));
    if (d->line_count == 0) {
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        text_at(dc, gutter, 0, "(no disassembly)", -1);
        return;
    }
    for (i = 0; i < d->line_count; i++) {
        const chfdebug_line *l = &d->lines[i];
        const int y = i * d->mono_h;
        const int sel = d->sel_addr == l->address && !l->is_pc;
        RECT row;
        char text[240];
        if (y >= c->bottom) break;
        SetRect(&row, 0, y, c->right, y + d->mono_h);
        if (l->is_pc) FillRect(dc, &row, d->accent);
        else if (sel) FillRect(dc, &row, GetSysColorBrush(COLOR_HIGHLIGHT));
        SetTextColor(dc, l->is_pc ? ON_ACCENT : sel ? GetSysColor(COLOR_HIGHLIGHTTEXT) : GetSysColor(COLOR_WINDOWTEXT));
        if (l->has_breakpoint) {
            const int size = d->mono_h - S(6) > 4 ? d->mono_h - S(6) : 4;
            HGDIOBJ oldb = SelectObject(dc, l->is_pc ? (HGDIOBJ)GetStockObject(BLACK_BRUSH)
                                                     : (HGDIOBJ)d->accent_dark);
            HGDIOBJ oldp = SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, S(4), y + S(3), S(4) + size + 1, y + S(3) + size + 1);
            SelectObject(dc, oldp);
            SelectObject(dc, oldb);
        }
        snprintf(text, sizeof text, "%04X  %-8.8s  %-14.14s %s", l->address, l->bytes, l->label, l->disasm);
        text_at(dc, gutter, y, text, -1);
        if (l->comment[0]) {
            snprintf(text, sizeof text, "; %s", l->comment);
            if (!l->is_pc && !sel) SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            text_at(dc, gutter + d->mono_w * COMMENT_COL, y, text, -1);
        }
    }
}

/* A click in the gutter toggles the breakpoint on that line and a click
 * elsewhere selects it; a double click toggles the breakpoint too. The wheel
 * and the keys browse (and turn Follow PC off, as in the other frontends). */
static LRESULT CALLBACK disasm_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_debugger;
    if (!d) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_PAINT:
        paint_buffered(d, hwnd, paint_disasm);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        /* more or fewer rows fit now */
        if (d->disasm == hwnd && chfdebug_is_attached(d->dbg)) dbg_refresh_disasm(d);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        const int line = d->mono_h > 0 ? GET_Y_LPARAM(lp) / d->mono_h : -1;
        SetFocus(hwnd);
        if (line >= 0 && line < d->line_count) {
            if (msg == WM_LBUTTONDBLCLK || GET_X_LPARAM(lp) < S(18))
                chfdebug_breakpoint_toggle(d->dbg, d->lines[line].address);
            else
                d->sel_addr = d->lines[line].address;
            dbg_refresh_disasm(d);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int rows = -GET_WHEEL_DELTA_WPARAM(wp) / 40;
        SendMessageW(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
        if (rows) d->disasm_top = chfdebug_row_address(d->dbg, (uint16_t)d->disasm_top, rows);
        dbg_refresh_disasm(d);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_PRIOR || wp == VK_NEXT || wp == VK_UP || wp == VK_DOWN) {
            RECT c;
            int page, rows;
            GetClientRect(hwnd, &c);
            page = d->mono_h > 0 ? c.bottom / d->mono_h / 2 : 1;
            if (page < 1) page = 1;
            rows = wp == VK_PRIOR ? -page : wp == VK_NEXT ? page : wp == VK_UP ? -1 : 1;
            SendMessageW(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
            d->disasm_top = chfdebug_row_address(d->dbg, (uint16_t)d->disasm_top, rows);
            dbg_refresh_disasm(d);
            return 0;
        }
        break;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- the video picture -------------------------------------------------------------- */

/* Where a VRAM cell sits in a view drawn at an integer scale (2 x 4 pixels a
 * cell): the picture views show columns 4-105 and rows 4-61 only, the VRAM
 * and VALUES views all 128 x 64. 0 when the view has no such cell. */
static int cell_box(int view, int s, int row, int col, RECT *r)
{
    int x, y;
    switch (view) {
    case CHFDEBUG_VIEW_FRAME: case CHFDEBUG_VIEW_LIVE: case CHFDEBUG_VIEW_BEAM:
        if (row < CHFDEBUG_VIS_ROW0 || row >= CHFDEBUG_VIS_ROW0 + CHFDEBUG_VIS_ROWS ||
            col < CHFDEBUG_VIS_COL0 || col >= CHFDEBUG_VIS_COL0 + CHFDEBUG_VIS_COLS)
            return 0;
        x = (col - CHFDEBUG_VIS_COL0) * 2 * s;
        y = (row - CHFDEBUG_VIS_ROW0) * 4 * s;
        break;
    case CHFDEBUG_VIEW_VRAM: case CHFDEBUG_VIEW_VALUES:
        x = col * 2 * s;
        y = row * 4 * s;
        break;
    default:
        return 0;
    }
    SetRect(r, x, y, x + 2 * s, y + 4 * s);
    return 1;
}

static void pic_origin(const debugger *d, const RECT *c, int *x0, int *y0)
{
    *x0 = (c->right - d->vimg.w) / 2 > 0 ? (c->right - d->vimg.w) / 2 : 0;
    *y0 = (c->bottom - d->vimg.h) / 2 > 0 ? (c->bottom - d->vimg.h) / 2 : 0;
}

/* The picture the engine drew, centred, and a box round the cell under the
 * pointer (white over the accent, so it shows on anything) and the pinned one
 * (the accent). */
static void paint_vpic(debugger *d, HWND hwnd, HDC dc, const RECT *c)
{
    int x0, y0;
    RECT r;
    (void)hwnd;
    FillRect(dc, c, (HBRUSH)GetStockObject(DKGRAY_BRUSH));
    pic_origin(d, c, &x0, &y0);
    blit(dc, &d->vimg, x0, y0);
    if (d->pin_row >= 0 && cell_box(d->vimg_view, d->vimg_scale, d->pin_row, d->pin_col, &r)) {
        OffsetRect(&r, x0, y0);
        InflateRect(&r, 2, 2);
        frame_rgb(dc, &r, RGB(0xF2, 0xC3, 0x00), 2);
    }
    if (d->hover_row >= 0 && cell_box(d->vimg_view, d->vimg_scale, d->hover_row, d->hover_col, &r)) {
        OffsetRect(&r, x0, y0);
        InflateRect(&r, 1, 1);
        frame_rgb(dc, &r, RGB(255, 255, 255), 1);
    }
}

static LRESULT CALLBACK vpic_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_debugger;
    if (!d) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_PAINT:
        paint_buffered(d, hwnd, paint_vpic);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        RECT c;
        TRACKMOUSEEVENT tme;
        int x0, y0, row = -1, col = -1;
        GetClientRect(hwnd, &c);
        pic_origin(d, &c, &x0, &y0);
        {
            const int fx = GET_X_LPARAM(lp) - x0, fy = GET_Y_LPARAM(lp) - y0;
            if (fx >= 0 && fy >= 0 && fx < d->vimg.w && fy < d->vimg.h)
                chfdebug_video_hit(d->dbg, d->vimg_view, fx, fy, d->vimg_scale, &row, &col);
        }
        if (msg == WM_LBUTTONDOWN && row >= 0) dbg_pin_cell(d, row, col);
        else if (msg == WM_LBUTTONDBLCLK && row >= 0) dbg_break_on_cell(d, row, col);
        else if (msg == WM_MOUSEMOVE) dbg_hover_changed(d, row, col);
        memset(&tme, 0, sizeof tme);
        tme.cbSize = sizeof tme;
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        dbg_hover_changed(d, -1, -1);
        return 0;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- the scratchpad ----------------------------------------------------------------- */

/* The registers the BIOS gives jobs: r9 is J, r10/r11 H, r12/r13 K and r14/r15
 * Q (upper and lower bytes). */
const char *dbg_reg_name(int reg)
{
    static const char *const names[16] = { "", "", "", "", "", "", "", "", "", "J", "Hu", "Hl", "Ku", "Kl", "Qu", "Ql" };
    return reg >= 0 && reg < 16 ? names[reg] : "";
}

#define SCR_LABEL_W 22
static int scr_cw(const debugger *d) { return d->mono_w * 5 + S(8); }
static int scr_ch(const debugger *d) { return d->mono_h * 2 + S(4); }

void dbg_scratch_size(const debugger *d, int *w, int *h)
{
    *w = S(SCR_LABEL_W) + 8 * scr_cw(d) + 1;
    *h = d->mono_h + S(4) + 8 * scr_ch(d) + 1;
}

/* An 8 x 8 grid of the 64 registers, numbered in octal as the ISAR holds them
 * (row = the upper digit, column = the lower): the value in hex over the
 * octal number and, for the registers with jobs, the name. The cell ISAR
 * points at is outlined, the ones that changed since the last stop are in the
 * accent, and the K stack (registers 40-59 decimal) is shaded. */
static void paint_scratch(debugger *d, HWND hwnd, HDC dc, const RECT *c)
{
    const int cw = scr_cw(d), chh = scr_ch(d), x0 = S(SCR_LABEL_W), y0 = d->mono_h + S(4);
    const int isar = d->cpu.isar & 63;
    int row, col;
    (void)hwnd;
    FillRect(dc, c, GetSysColorBrush(COLOR_WINDOW));
    SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    for (col = 0; col < 8; col++) {
        char t[4];
        RECT r;
        snprintf(t, sizeof t, ".%d", col);
        SetRect(&r, x0 + col * cw, 0, x0 + (col + 1) * cw, d->mono_h);
        text_in(dc, t, &r, DT_CENTER | DT_SINGLELINE);
    }
    for (row = 0; row < 8; row++) {
        char t[4];
        RECT r;
        snprintf(t, sizeof t, "%d.", row);
        SetRect(&r, 0, y0 + row * chh, x0 - S(2), y0 + (row + 1) * chh);
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        text_in(dc, t, &r, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        for (col = 0; col < 8; col++) {
            const int reg = row * 8 + col;
            const int kband = reg >= 40 && reg <= 59;
            const int changed = (d->changed_mask >> reg) & 1;
            RECT cell;
            char t1[16], t2[24];
            SetRect(&cell, x0 + col * cw, y0 + row * chh, x0 + (col + 1) * cw, y0 + (row + 1) * chh);
            if (changed) FillRect(dc, &cell, d->accent);
            else if (kband) fill_rgb(dc, &cell, RGB(0xe6, 0xea, 0xf4));
            InflateRect(&cell, -1, -1);
            if (reg == isar) frame_rgb(dc, &cell, RGB(0x1a, 0x1a, 0x1a), 2);
            else frame_rgb(dc, &cell, changed ? RGB(0xb0, 0x8c, 0x00) : RGB(0xd0, 0xd0, 0xd8), 1);
            snprintf(t1, sizeof t1, "%02X", d->cpu.r[reg]);
            if (dbg_reg_name(reg)[0]) snprintf(t2, sizeof t2, "%02o %s", reg, dbg_reg_name(reg));
            else snprintf(t2, sizeof t2, "%02o", reg);
            SetTextColor(dc, changed ? ON_ACCENT : GetSysColor(COLOR_WINDOWTEXT));
            SetRect(&r, cell.left, cell.top + S(1), cell.right, cell.top + d->mono_h + S(1));
            text_in(dc, t1, &r, DT_CENTER | DT_SINGLELINE);
            SetTextColor(dc, changed ? ON_ACCENT : GetSysColor(COLOR_GRAYTEXT));
            SetRect(&r, cell.left, cell.top + d->mono_h + S(1), cell.right, cell.bottom);
            text_in(dc, t2, &r, DT_CENTER | DT_SINGLELINE);
        }
    }
}

static LRESULT CALLBACK scratch_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_debugger;
    (void)wp;
    if (!d) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_PAINT:
        paint_buffered(d, hwnd, paint_scratch);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN: {
        const int col = (GET_X_LPARAM(lp) - S(SCR_LABEL_W)) / scr_cw(d);
        const int row = (GET_Y_LPARAM(lp) - d->mono_h - S(4)) / scr_ch(d);
        if (GET_X_LPARAM(lp) >= S(SCR_LABEL_W) && GET_Y_LPARAM(lp) >= d->mono_h + S(4) &&
            col >= 0 && col < 8 && row >= 0 && row < 8)
            dbg_scratch_pick(d, row * 8 + col);
        return 0;
    }
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- the row palette strip ----------------------------------------------------------- */

#define ROWS_PER_COL 32
static int rows_cw(const debugger *d) { return d->mono_w * 7 + S(64); }

void dbg_rows_size(const debugger *d, int *w, int *h)
{
    *w = 2 * rows_cw(d);
    *h = ROWS_PER_COL * d->mono_h + 1;
}

/* All 64 rows' palettes, two columns of 32: the row, its palette and the
 * background swatch it gives; the rows hidden above and below the picture in
 * grey, and a tick at the row whose palette changed this frame. */
static void paint_rows(debugger *d, HWND hwnd, HDC dc, const RECT *c)
{
    const int cw = rows_cw(d);
    int row;
    (void)hwnd;
    FillRect(dc, c, GetSysColorBrush(COLOR_WINDOW));
    for (row = 0; row < CHFDEBUG_VRAM_ROWS; row++) {
        const int x = (row / ROWS_PER_COL) * cw, y = (row % ROWS_PER_COL) * d->mono_h;
        const int hidden = row < CHFDEBUG_VIS_ROW0 || row >= CHFDEBUG_VIS_ROW0 + CHFDEBUG_VIS_ROWS;
        const int changed = (d->pal_changed[row / 32] >> (row % 32)) & 1;
        RECT sw;
        char t[40];
        if (changed) {
            RECT mark;
            SetRect(&mark, x, y, x + S(4), y + d->mono_h);
            FillRect(dc, &mark, d->accent);
        }
        snprintf(t, sizeof t, "r%02d P%d", row, d->vid.row_palette[row] & 3);
        SetTextColor(dc, hidden ? GetSysColor(COLOR_GRAYTEXT) : GetSysColor(COLOR_WINDOWTEXT));
        text_at(dc, x + S(8), y, t, -1);
        SetRect(&sw, x + S(8) + d->mono_w * 7, y + S(2), x + S(8) + d->mono_w * 7 + S(34), y + d->mono_h - S(1));
        fill_rgb(dc, &sw, xrgb_ref(d->vid.row_background[row]));
        frame_rgb(dc, &sw, RGB(0x70, 0x70, 0x78), 1);
    }
}

static LRESULT CALLBACK rows_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_debugger;
    if (d && msg == WM_PAINT) { paint_buffered(d, hwnd, paint_rows); return 0; }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- the contacts and the sound level --------------------------------------------------- */

static int lamp_w(const debugger *d) { return d->mono_w * 5 + S(12); }
static int lamp_h(const debugger *d) { return d->mono_h + S(8); }

void dbg_io_draw_size(const debugger *d, int *w, int *h)
{
    *w = NLAMPS * (lamp_w(d) + S(4));
    /* three titled rows of lamps and the sound row */
    *h = 3 * (d->mono_h + lamp_h(d) + S(14)) + d->mono_h * 2 + lamp_h(d) + S(24);
}

static void lamp(debugger *d, HDC dc, int x, int y, const char *label, int on)
{
    RECT r;
    SetRect(&r, x, y, x + lamp_w(d), y + lamp_h(d));
    if (on) FillRect(dc, &r, d->accent);
    else fill_rgb(dc, &r, RGB(0xee, 0xee, 0xf2));
    frame_rgb(dc, &r, on ? RGB(0xb0, 0x8c, 0x00) : RGB(0xb8, 0xb8, 0xc0), 1);
    SetTextColor(dc, on ? ON_ACCENT : GetSysColor(COLOR_GRAYTEXT));
    text_in(dc, label, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

/* The two hand controllers' eight contacts and the console's buttons, decoded
 * as the console latched them at the last frame, and the beeper: its mode and
 * level meter, lit "forced on" for the ~2 ms after a change. */
static void paint_io(debugger *d, HWND hwnd, HDC dc, const RECT *c)
{
    static const char *const names[NLAMPS] = { "RIGHT", "LEFT", "BACK", "FWD", "CCW", "CW", "PULL", "PUSH" };
    static const char *const panel[5] = { "1 TIME", "2 MODE", "3 HOLD", "4 START", "RESET" };
    static const char *const freq[4] = { "off", "1 kHz", "500 Hz", "120 Hz" };
    const int lw = lamp_w(d) + S(4), lh = lamp_h(d);
    int y = 0, i, p;
    char t[160];
    (void)hwnd;
    FillRect(dc, c, GetSysColorBrush(COLOR_BTNFACE));
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    for (p = 0; p < 2; p++) {
        /* player 1 is the right controller on CPU port 1; player 2 the left, on port 4 */
        const unsigned held = d->io.controller[p];
        const unsigned reads = p == 0 ? d->vid.in1 : d->vid.in4;
        snprintf(t, sizeof t, "%s controller - Player %d   (port %d reads $%02X)", p == 0 ? "Right" : "Left", p + 1,
                 p == 0 ? 1 : 4, reads);
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        text_at(dc, 0, y, t, -1);
        y += d->mono_h + S(2);
        for (i = 0; i < NLAMPS; i++) lamp(d, dc, i * lw, y, names[i], (held >> i) & 1);
        y += lh + S(12);
    }
    snprintf(t, sizeof t, "Console panel   (port 0 latch $%02X, reads $%02X)", d->vid.panel_latch, d->vid.in0);
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    text_at(dc, 0, y, t, -1);
    y += d->mono_h + S(2);
    for (i = 0; i < 4; i++) lamp(d, dc, i * lw, y, panel[i], (d->io.panel >> i) & 1);
    lamp(d, dc, 4 * lw, y, panel[4], d->io.reset_held);
    y += lh + S(16);

    snprintf(t, sizeof t, "Beeper   mode %d: %s%s", d->io.sound_mode, freq[d->io.sound_mode & 3],
             d->io.sound_forced ? "   (forced on)" : "");
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    text_at(dc, 0, y, t, -1);
    y += d->mono_h + S(4);
    {
        RECT bar, fill;
        const int w = 8 * lw - S(4);
        const int lvl = d->io.sound_level < 0 ? 0 : d->io.sound_level > 32767 ? 32767 : d->io.sound_level;
        SetRect(&bar, 0, y, w, y + lh);
        fill_rgb(dc, &bar, RGB(0xee, 0xee, 0xf2));
        fill = bar;
        fill.right = bar.left + (int)((long long)w * lvl / 32767);
        if (fill.right > fill.left) FillRect(dc, &fill, d->accent);
        frame_rgb(dc, &bar, RGB(0xb8, 0xb8, 0xc0), 1);
        snprintf(t, sizeof t, "level %d", lvl);
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        text_in(dc, t, &bar, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

static LRESULT CALLBACK io_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_debugger;
    if (d && msg == WM_PAINT) { paint_buffered(d, hwnd, paint_io); return 0; }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- the classes ----------------------------------------------------------------------- */

static void register_class(HINSTANCE inst, const wchar_t *name, WNDPROC proc, UINT style)
{
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.style = style;
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = name;
    RegisterClassW(&wc);
}

/* Registers the window classes of the views, once. */
void dbg_views_register(HINSTANCE inst)
{
    register_class(inst, L"ChfDbgVideo", vpic_proc, CS_DBLCLKS);
    register_class(inst, L"ChfDbgDisasm", disasm_proc, CS_DBLCLKS);
    register_class(inst, L"ChfDbgScratch", scratch_proc, 0);
    register_class(inst, L"ChfDbgRows", rows_proc, 0);
    register_class(inst, L"ChfDbgIo", io_proc, 0);
}
