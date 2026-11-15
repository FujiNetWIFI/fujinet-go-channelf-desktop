/*
 * The debugger window's two Channel F-only hand-drawn widgets (Win32): the
 * scratchpad grid and the controller lamps. See dbg_widgets.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_widgets.h"

#include <windowsx.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chfsession.h"
#include "../win_common.h"

#define ON_ACCENT RGB(0x14, 0x14, 0x14)      /* dark text on the accent */
#define ACCENT_COLORREF RGB((CHFSESSION_ACCENT_RGB >> 16) & 255, (CHFSESSION_ACCENT_RGB >> 8) & 255, CHFSESSION_ACCENT_RGB & 255)
#define NLAMPS 8             /* a controller's contacts */
#define SCR_LABEL_W 22

typedef struct {
    chfdebug_cpu cpu;
    chfdebug_io io;
    chfdebug_video vid;
    uint64_t changed;
    HFONT mono;
    int mono_w, mono_h, dpi;
} wstate;

#define S(w, v) chf_scale((v), (w)->dpi)

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

static void text_in(HDC dc, const char *text, RECT *r, UINT fmt)
{
    wchar_t w[128];
    MultiByteToWideChar(CP_UTF8, 0, text, -1, w, 128);
    DrawTextW(dc, w, -1, r, fmt | DT_NOPREFIX);
}

static void text_at(HDC dc, int x, int y, const char *text)
{
    wchar_t w[256];
    MultiByteToWideChar(CP_UTF8, 0, text, -1, w, 256);
    TextOutW(dc, x, y, w, (int)wcslen(w));
}

typedef void (*paint_fn)(wstate *w, HDC dc, const RECT *c);

static void paint_buffered(HWND hwnd, wstate *w, paint_fn fn)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps), mem;
    RECT c;
    HBITMAP bmp;
    HGDIOBJ oldbmp, oldfont = NULL;
    GetClientRect(hwnd, &c);
    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, c.right > 0 ? c.right : 1, c.bottom > 0 ? c.bottom : 1);
    oldbmp = SelectObject(mem, bmp);
    if (w->mono) oldfont = SelectObject(mem, w->mono);
    SetBkMode(mem, TRANSPARENT);
    fn(w, mem, &c);
    BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
    if (oldfont) SelectObject(mem, oldfont);
    SelectObject(mem, oldbmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

/* ---- the scratchpad ----------------------------------------------------------------- */

/* The registers the BIOS gives jobs: r9 is J, r10/r11 H, r12/r13 K and r14/r15
 * Q (upper and lower bytes). */
const char *dbg_reg_name(int reg)
{
    static const char *const names[16] = { "", "", "", "", "", "", "", "", "", "J", "Hu", "Hl", "Ku", "Kl", "Qu", "Ql" };
    return reg >= 0 && reg < 16 ? names[reg] : "";
}

static int scr_cw(int mono_w, int dpi) { return mono_w * 5 + chf_scale(8, dpi); }
static int scr_ch(int mono_h, int dpi) { return mono_h * 2 + chf_scale(4, dpi); }

void dbg_scratch_size(int mono_w, int mono_h, int dpi, int *w, int *h)
{
    *w = chf_scale(SCR_LABEL_W, dpi) + 8 * scr_cw(mono_w, dpi) + 1;
    *h = mono_h + chf_scale(4, dpi) + 8 * scr_ch(mono_h, dpi) + 1;
}

/* An 8 x 8 grid of the 64 registers, numbered in octal as the ISAR holds them
 * (row = the upper digit, column = the lower): the value in hex over the
 * octal number and, for the registers with jobs, the name. The cell ISAR
 * points at is outlined, the ones that changed since the last stop are in the
 * accent, and the K stack (registers 40-59 decimal) is shaded. */
static void paint_scratch(wstate *w, HDC dc, const RECT *c)
{
    const int cw = scr_cw(w->mono_w, w->dpi), chh = scr_ch(w->mono_h, w->dpi);
    const int x0 = S(w, SCR_LABEL_W), y0 = w->mono_h + S(w, 4);
    const int isar = w->cpu.isar & 63;
    int row, col;
    FillRect(dc, c, GetSysColorBrush(COLOR_WINDOW));
    SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    for (col = 0; col < 8; col++) {
        char t[8];
        RECT r;
        snprintf(t, sizeof t, ".%d", col);
        SetRect(&r, x0 + col * cw, 0, x0 + (col + 1) * cw, w->mono_h);
        text_in(dc, t, &r, DT_CENTER | DT_SINGLELINE);
    }
    for (row = 0; row < 8; row++) {
        char t[8];
        RECT r;
        snprintf(t, sizeof t, "%d.", row);
        SetRect(&r, 0, y0 + row * chh, x0 - S(w, 2), y0 + (row + 1) * chh);
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        text_in(dc, t, &r, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        for (col = 0; col < 8; col++) {
            const int reg = row * 8 + col;
            const int kband = reg >= 40 && reg <= 59;
            const int changed = (int)((w->changed >> reg) & 1);
            RECT cell;
            char t1[16], t2[24];
            SetRect(&cell, x0 + col * cw, y0 + row * chh, x0 + (col + 1) * cw, y0 + (row + 1) * chh);
            if (changed) fill_rgb(dc, &cell, ACCENT_COLORREF);
            else if (kband) fill_rgb(dc, &cell, RGB(0xe6, 0xea, 0xf4));
            InflateRect(&cell, -1, -1);
            if (reg == isar) frame_rgb(dc, &cell, RGB(0x1a, 0x1a, 0x1a), 2);
            else frame_rgb(dc, &cell, changed ? RGB(0xb0, 0x8c, 0x00) : RGB(0xd0, 0xd0, 0xd8), 1);
            snprintf(t1, sizeof t1, "%02X", w->cpu.r[reg]);
            if (dbg_reg_name(reg)[0]) snprintf(t2, sizeof t2, "%02o %s", reg, dbg_reg_name(reg));
            else snprintf(t2, sizeof t2, "%02o", reg);
            SetTextColor(dc, changed ? ON_ACCENT : GetSysColor(COLOR_WINDOWTEXT));
            SetRect(&r, cell.left, cell.top + S(w, 1), cell.right, cell.top + w->mono_h + S(w, 1));
            text_in(dc, t1, &r, DT_CENTER | DT_SINGLELINE);
            SetTextColor(dc, changed ? ON_ACCENT : GetSysColor(COLOR_GRAYTEXT));
            SetRect(&r, cell.left, cell.top + w->mono_h + S(w, 1), cell.right, cell.bottom);
            text_in(dc, t2, &r, DT_CENTER | DT_SINGLELINE);
        }
    }
}

/* ---- the contacts --------------------------------------------------------------------- */

static int lamp_w(const wstate *w) { return w->mono_w * 5 + S(w, 12); }
static int lamp_h(const wstate *w) { return w->mono_h + S(w, 8); }

void dbg_lamps_size(int mono_w, int mono_h, int dpi, int *w, int *h)
{
    *w = NLAMPS * (mono_w * 5 + chf_scale(12, dpi) + chf_scale(4, dpi));
    /* three titled rows of lamps */
    *h = 3 * (mono_h + (mono_h + chf_scale(8, dpi)) + chf_scale(14, dpi)) + chf_scale(4, dpi);
}

static void lamp(wstate *w, HDC dc, int x, int y, const char *label, int on)
{
    RECT r;
    SetRect(&r, x, y, x + lamp_w(w), y + lamp_h(w));
    if (on) fill_rgb(dc, &r, ACCENT_COLORREF);
    else fill_rgb(dc, &r, RGB(0xee, 0xee, 0xf2));
    frame_rgb(dc, &r, on ? RGB(0xb0, 0x8c, 0x00) : RGB(0xb8, 0xb8, 0xc0), 1);
    SetTextColor(dc, on ? ON_ACCENT : GetSysColor(COLOR_GRAYTEXT));
    text_in(dc, label, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void paint_lamps(wstate *w, HDC dc, const RECT *c)
{
    static const char *const names[NLAMPS] = { "RIGHT", "LEFT", "BACK", "FWD", "CCW", "CW", "PULL", "PUSH" };
    static const char *const panel[5] = { "1 TIME", "2 MODE", "3 HOLD", "4 START", "RESET" };
    const int lw = lamp_w(w) + S(w, 4), lh = lamp_h(w);
    int y = 0, i, p;
    char t[160];
    FillRect(dc, c, GetSysColorBrush(COLOR_BTNFACE));
    for (p = 0; p < 2; p++) {
        /* player 1 is the right controller on CPU port 1; player 2 the left, on port 4 */
        const unsigned held = w->io.controller[p];
        const unsigned reads = p == 0 ? w->vid.in1 : w->vid.in4;
        snprintf(t, sizeof t, "%s controller - Player %d   (port %d reads $%02X)", p == 0 ? "Right" : "Left", p + 1,
                 p == 0 ? 1 : 4, reads);
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        text_at(dc, 0, y, t);
        y += w->mono_h + S(w, 2);
        for (i = 0; i < NLAMPS; i++) lamp(w, dc, i * lw, y, names[i], (int)((held >> i) & 1u));
        y += lh + S(w, 12);
    }
    snprintf(t, sizeof t, "Console panel   (port 0 latch $%02X, reads $%02X)", w->vid.panel_latch, w->vid.in0);
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    text_at(dc, 0, y, t);
    y += w->mono_h + S(w, 2);
    for (i = 0; i < 4; i++) lamp(w, dc, i * lw, y, panel[i], (int)((w->io.panel >> i) & 1u));
    lamp(w, dc, 4 * lw, y, panel[4], w->io.reset_held);
}

/* ---- window classes ---------------------------------------------------------------- */

static LRESULT CALLBACK scratch_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    wstate *w = (wstate *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)calloc(1, sizeof(wstate)));
        break;
    case WM_NCDESTROY:
        free(w);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    case WM_PAINT:
        if (w) paint_buffered(hwnd, w, paint_scratch);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
        if (w && w->mono_w > 0) {
            const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            const int col = (x - S(w, SCR_LABEL_W)) / scr_cw(w->mono_w, w->dpi);
            const int row = (y - w->mono_h - S(w, 4)) / scr_ch(w->mono_h, w->dpi);
            if (x >= S(w, SCR_LABEL_W) && y >= w->mono_h + S(w, 4) && col >= 0 && col < 8 && row >= 0 && row < 8)
                SendMessageW(GetParent(hwnd), WM_DBG_SCRATCH_PICK, (WPARAM)(row * 8 + col), 0);
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK lamps_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    wstate *w = (wstate *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)calloc(1, sizeof(wstate)));
        break;
    case WM_NCDESTROY:
        free(w);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    case WM_PAINT:
        if (w) paint_buffered(hwnd, w, paint_lamps);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void register_class(HINSTANCE inst, const wchar_t *name, WNDPROC proc)
{
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = name;
    RegisterClassW(&wc);
}

void dbg_widgets_register(HINSTANCE inst)
{
    static int done;
    if (done) return;
    done = 1;
    register_class(inst, L"ChfDbgScratch", scratch_proc);
    register_class(inst, L"ChfDbgLamps", lamps_proc);
}

HWND dbg_scratch_create(HWND parent, int id)
{
    return CreateWindowExW(0, L"ChfDbgScratch", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)id,
                           (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE), NULL);
}

HWND dbg_lamps_create(HWND parent, int id)
{
    return CreateWindowExW(0, L"ChfDbgLamps", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)id,
                           (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE), NULL);
}

static void set_font(wstate *w, HFONT mono, int mono_w, int mono_h, int dpi)
{
    w->mono = mono;
    w->mono_w = mono_w;
    w->mono_h = mono_h;
    w->dpi = dpi > 0 ? dpi : 96;
}

void dbg_scratch_update(HWND grid, const chfdebug_cpu *cpu, uint64_t changed, HFONT mono, int mono_w, int mono_h, int dpi)
{
    wstate *w = (wstate *)GetWindowLongPtrW(grid, GWLP_USERDATA);
    if (!w) return;
    w->cpu = *cpu;
    w->changed = changed;
    set_font(w, mono, mono_w, mono_h, dpi);
    InvalidateRect(grid, NULL, FALSE);
}

void dbg_lamps_update(HWND lamps, const chfdebug_io *io, const chfdebug_video *vid, HFONT mono, int mono_w, int mono_h, int dpi)
{
    wstate *w = (wstate *)GetWindowLongPtrW(lamps, GWLP_USERDATA);
    if (!w) return;
    w->io = *io;
    w->vid = *vid;
    set_font(w, mono, mono_w, mono_h, dpi);
    InvalidateRect(lamps, NULL, FALSE);
}
