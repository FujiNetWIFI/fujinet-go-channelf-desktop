/*
 * The debugger window's hand-drawn views (Win32): the disassembly, the UVI's
 * picture and object list, the character screen and the keypad matrix. Each
 * is a small window class that paints what dbg_window.c last read from the
 * engine, double-buffered; none of them reads the engine on its own except to
 * name the object under the pointer.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <windowsx.h>

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "dbg_internal.h"

#define COMMENT_COL 60       /* where a disassembly line's comment starts, in characters */

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

/* A top-down 32-bit DIB onto dc, every pixel scale x scale, nearest. */
static void blit(HDC dc, const image *im, int x, int y, int scale)
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
    StretchDIBits(dc, x, y, im->w * scale, im->h * scale, 0, 0, im->w, im->h, im->px, &bmi,
                  DIB_RGB_COLORS, SRCCOPY);
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

/* The UVI's 8 colours as the palette strip gave them. */
static COLORREF uvi_colour(const debugger *d, int c)
{
    return xrgb_ref(d->pal[c & 7]);
}

/* The box of an object in frame pixels, for the highlight. */
static int object_box(const debugger *d, int kind, int idx, RECT *r)
{
    if (kind == CHFDEBUG_UVI_HIT_OBJECT && idx >= 0 && idx < 4) {
        const chfdebug_uvi_object *o = &d->uvi.obj[idx];
        SetRect(r, o->px, o->py, o->px + o->width, o->py + o->height);
        return 1;
    }
    return 0;
}


/* ---- the disassembly ---------------------------------------------------------------- */

/* Drawn by hand, one row per line: the breakpoint gutter, address, bytes,
 * label, instruction, and the comment (a branch target's label) in grey. The
 * PC's line is filled with the accent, dark text on it. */
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
        RECT row;
        char text[240];
        if (y >= c->bottom) break;
        SetRect(&row, 0, y, c->right, y + d->mono_h);
        if (l->is_pc) FillRect(dc, &row, d->accent);
        SetTextColor(dc, l->is_pc ? RGB(0x1e, 0x1e, 0x1e) : GetSysColor(COLOR_WINDOWTEXT));
        if (l->has_breakpoint) {
            const int size = d->mono_h - S(6) > 4 ? d->mono_h - S(6) : 4;
            HGDIOBJ oldb = SelectObject(dc, l->is_pc ? (HGDIOBJ)GetStockObject(BLACK_BRUSH)
                                                     : (HGDIOBJ)d->accent_dark);
            HGDIOBJ oldp = SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, S(4), y + S(3), S(4) + size + 1, y + S(3) + size + 1);
            SelectObject(dc, oldp);
            SelectObject(dc, oldb);
        }
        snprintf(text, sizeof text, "%04X  %-8.8s  %-14.14s %s", l->address & 0x7fff, l->bytes, l->label,
                 l->disasm);
        text_at(dc, gutter, y, text, -1);
        if (l->comment[0]) {
            snprintf(text, sizeof text, "; %s", l->comment);
            if (!l->is_pc) SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            text_at(dc, gutter + d->mono_w * COMMENT_COL, y, text, -1);
        }
    }
}

/* A click toggles the breakpoint on that line; the wheel and the keys
 * browse (and turn Follow PC off, as in the other frontends). */
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
    case WM_LBUTTONDOWN: {
        const int line = d->mono_h > 0 ? GET_Y_LPARAM(lp) / d->mono_h : -1;
        SetFocus(hwnd);
        if (line >= 0 && line < d->line_count) {
            chfdebug_breakpoint_toggle(d->dbg, d->lines[line].address);
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

/* ---- pictures ------------------------------------------------------------------------ */

/* Where the Video picture sits in its window: the largest whole scale that
 * fits, centred, so the chip's pixels stay whole. */
static void uvi_geom(const debugger *d, const RECT *c, int *x0, int *y0, int *scale)
{
    int s = 1;
    const int w = d->vimg.w, h = d->vimg.h;
    if (w > 0 && h > 0) {
        const int sx = c->right / w, sy = c->bottom / h;
        s = sx < sy ? sx : sy;
        if (s < 1) s = 1;
    }
    *scale = s;
    *x0 = (c->right - w * s) / 2 > 0 ? (c->right - w * s) / 2 : 0;
    *y0 = (c->bottom - h * s) / 2 > 0 ? (c->bottom - h * s) / 2 : 0;
}

/* The picture, each object's index beside its box, and a highlight round the
 * thing under the pointer: white outside the accent so it shows on anything. */
static void paint_vpic(debugger *d, HWND hwnd, HDC dc, const RECT *c)
{
    int x0, y0, s, k;
    (void)hwnd;
    FillRect(dc, c, (HBRUSH)GetStockObject(DKGRAY_BRUSH));
    uvi_geom(d, c, &x0, &y0, &s);
    blit(dc, &d->vimg, x0, y0, s);
    if (SendMessageW(d->overlay[0], BM_GETCHECK, 0, 0) == BST_CHECKED) {
        /* the engine boxes the objects; the number says which is which */
        SetTextColor(dc, RGB(255, 255, 255));
        for (k = 0; k < 4; k++) {
            RECT box;
            char n[4];
            if (!d->uvi.obj[k].visible || !object_box(d, CHFDEBUG_UVI_HIT_OBJECT, k, &box)) continue;
            snprintf(n, sizeof n, "%d", k);
            /* above the box's corner when there is room, else inside it */
            text_at(dc, x0 + box.left * s, y0 + box.top * s - d->mono_h > 0 ? y0 + box.top * s - d->mono_h : y0 + box.top * s + 1, n, -1);
        }
    }
    if (d->hover_valid && d->nhits > 0) {
        RECT box;
        if (object_box(d, d->hits[0].kind, d->hits[0].index, &box)) {
            RECT r;
            SetRect(&r, x0 + box.left * s, y0 + box.top * s, x0 + box.right * s, y0 + box.bottom * s);
            InflateRect(&r, 1, 1);
            FrameRect(dc, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
            InflateRect(&r, -1, -1);
            FrameRect(dc, &r, d->accent);
        }
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
    case WM_MOUSEMOVE: {
        RECT c;
        TRACKMOUSEEVENT tme;
        int x0, y0, s, fx, fy;
        GetClientRect(hwnd, &c);
        uvi_geom(d, &c, &x0, &y0, &s);
        fx = GET_X_LPARAM(lp) - x0;
        fy = GET_Y_LPARAM(lp) - y0;
        if (fx >= 0 && fy >= 0 && fx < d->vimg.w * s && fy < d->vimg.h * s) {
            d->hover_valid = 1;
            d->hover_x = fx / s;
            d->hover_y = fy / s;
        } else {
            d->hover_valid = 0;
        }
        dbg_update_hover(d);
        memset(&tme, 0, sizeof tme);
        tme.cbSize = sizeof tme;
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        d->hover_valid = 0;
        dbg_update_hover(d);
        return 0;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* A picture the engine drew, as large as fits (a whole scale) when centred,
 * else at the top left at 1:1: the glyph, object and palette sheets. */
static void paint_pic(debugger *d, HWND hwnd, HDC dc, const RECT *c)
{
    const picview *pv = (const picview *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    int s = 1, x = 0, y = 0;
    FillRect(dc, c, (HBRUSH)GetStockObject(DKGRAY_BRUSH));
    (void)d;
    if (!pv || pv->img.w <= 0 || pv->img.h <= 0) return;
    if (pv->center) {
        const int sx = c->right / pv->img.w, sy = c->bottom / pv->img.h;
        s = sx < sy ? sx : sy;
        if (s < 1) s = 1;
        x = (c->right - pv->img.w * s) / 2 > 0 ? (c->right - pv->img.w * s) / 2 : 0;
        y = (c->bottom - pv->img.h * s) / 2 > 0 ? (c->bottom - pv->img.h * s) / 2 : 0;
    }
    blit(dc, &pv->img, x, y, s);
}

static LRESULT CALLBACK pic_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_debugger;
    if (msg == WM_PAINT && d) { paint_buffered(d, hwnd, paint_pic); return 0; }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- the object list ----------------------------------------------------------------- */

/* Screen pixels per bitmap pixel: the bitmaps arrive at twice their size, and
 * a high-DPI display doubles them whole again. */
static int obj_bscale(const debugger *d)
{
    const int s = (d->dpi + 48) / 96;
    return s < 1 ? 1 : s;
}

static int obj_row_height(const debugger *d)
{
    const int bm = 32 * obj_bscale(d);        /* the tallest image: 8 x 16 pixels, arrived at twice the size */
    const int text = 4 * d->mono_h;
    return (text > bm ? text : bm) + S(12);
}

static void paint_objects(debugger *d, HWND hwnd, HDC dc, const RECT *c)
{
    const chfdebug_uvi *u = &d->uvi;
    const int bs = obj_bscale(d), col = 16 * bs + S(22), h = obj_row_height(d);
    int k;
    (void)hwnd;
    FillRect(dc, c, GetSysColorBrush(COLOR_WINDOW));
    for (k = 0; k < OBJ_COUNT; k++) {
        const chfdebug_uvi_object *o = &u->obj[k];
        const int y = k * h;
        char lines[4][112];
        RECT line;
        int i;
        if (d->obj_w[k] > 0) {
            image im;
            im.px = d->obj_bm[k];
            im.w = d->obj_w[k];
            im.h = d->obj_h[k];
            im.cap = OBJ_BM_PX;
            blit(dc, &im, S(8), y + (h - im.h * bs) / 2, bs);
        }
        snprintf(lines[0], 112, "Object %d   x=%d y=%d   %s", k, o->x, o->y, chfdebug_uvi_colour_name(o->colour));
        snprintf(lines[1], 112, "regs $%02X $%02X   UDC $%02X%s", o->reg_x, o->reg_y, o->udc,
                 o->double_height ? "   double height (8 x 16)" : "");
        snprintf(lines[2], 112, "shape %02X %02X %02X %02X %02X %02X %02X %02X", o->shape[0], o->shape[1],
                 o->shape[2], o->shape[3], o->shape[4], o->shape[5], o->shape[6], o->shape[7]);
        snprintf(lines[3], 112, "%s   hit chars %s   hits objects %s%s%s%s", o->visible ? "visible" : "off screen",
                 o->hits_chars ? "yes" : "no", (o->hits_objects & 1) ? "0 " : "", (o->hits_objects & 2) ? "1 " : "",
                 (o->hits_objects & 4) ? "2 " : "", (o->hits_objects & 8) ? "3" : "");
        for (i = 0; i < 4; i++) {
            SetTextColor(dc, i == 0 ? GetSysColor(COLOR_WINDOWTEXT) : GetSysColor(COLOR_GRAYTEXT));
            text_at(dc, S(8) + col, y + S(6) + i * d->mono_h, lines[i], -1);
        }
        SetRect(&line, 0, y + h - 1, c->right, y + h);
        FillRect(dc, &line, GetSysColorBrush(COLOR_BTNFACE));
    }
}

static LRESULT CALLBACK objects_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_debugger;
    if (msg == WM_PAINT && d) { paint_buffered(d, hwnd, paint_objects); return 0; }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- the character screen -------------------------------------------------------------- */

/* The cell size follows the section's width (whole glyph-pixel scales). */
static int cell_index_at(const debugger *d, int x, int y)
{
    int cw, ch, col, row;
    dbg_screen_cell(d, &cw, &ch);
    col = x / cw;
    row = y / ch;
    if (x < 0 || y < 0 || col >= CHFDEBUG_UVI_COLS || row >= d->cell_rows) return -1;
    return row * CHFDEBUG_UVI_COLS + col;
}

/* The 16 x 13 (or 26) cells as the chip draws them: each glyph in the colours
 * of its own cell, a block-graphics cell as its code on its background. */
static void paint_screen(debugger *d, HWND hwnd, HDC dc, const RECT *c)
{
    int cw, ch, row, col;
    (void)hwnd;
    FillRect(dc, c, GetSysColorBrush(COLOR_BTNFACE));
    dbg_screen_cell(d, &cw, &ch);
    for (row = 0; row < d->cell_rows; row++)
        for (col = 0; col < CHFDEBUG_UVI_COLS; col++) {
            const int idx = row * CHFDEBUG_UVI_COLS + col;
            const chfdebug_uvi_cell *cell = &d->cells[idx];
            const int s = cw / 8;
            RECT r;
            SetRect(&r, col * cw, row * ch, (col + 1) * cw, (row + 1) * ch);
            fill_rgb(dc, &r, uvi_colour(d, cell->bg));
            if (cell->graphics || !d->glyph_ok) {
                char t[8];
                SetTextColor(dc, uvi_colour(d, cell->fg));
                snprintf(t, sizeof t, "%02X%s", cell->code, cell->graphics ? "g" : "");
                text_in(dc, t, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else {
                const uint32_t *g = d->glyph[cell->glyph & 0x3f];
                HBRUSH fg = CreateSolidBrush(uvi_colour(d, cell->fg));
                int px, py;
                for (py = 0; py < 8; py++)
                    for (px = 0; px < 8; px++) {
                        RECT p;
                        if (g[py * 8 + px] == GLYPH_CLEAR) continue;
                        SetRect(&p, r.left + px * s, r.top + py * s, r.left + (px + 1) * s, r.top + (py + 1) * s);
                        FillRect(dc, &p, fg);
                    }
                DeleteObject(fg);
            }
            if (idx == d->screen_hover) {
                FrameRect(dc, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
                InflateRect(&r, -1, -1);
                FrameRect(dc, &r, d->accent);
            }
        }
    if (d->cell_rows == 0) {
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        text_at(dc, S(8), S(8), "(machine not running)", -1);
    }
}

int dbg_screen_cell_text(const debugger *d, int idx, char *out, int n)
{
    const chfdebug_uvi_cell *c;
    if (idx < 0 || idx >= d->cell_rows * CHFDEBUG_UVI_COLS) return 0;
    c = &d->cells[idx];
    return snprintf(out, (size_t)n, "Row %d col %d: code $%02X, glyph $%02X%s, fg %s, bg %s", idx / CHFDEBUG_UVI_COLS,
                    idx % CHFDEBUG_UVI_COLS, c->code, c->glyph, c->graphics ? " (block graphics)" : "",
                    chfdebug_uvi_colour_name(c->fg), chfdebug_uvi_colour_name(c->bg));
}

static LRESULT CALLBACK screen_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_debugger;
    if (!d) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_PAINT:
        paint_buffered(d, hwnd, paint_screen);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme;
        char t[200];
        d->screen_hover = cell_index_at(d, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (dbg_screen_cell_text(d, d->screen_hover, t, sizeof t) <= 0)
            snprintf(t, sizeof t, "Hover over a cell for its code, glyph and colours.");
        dbg_label_set(d->screen_status, t);
        InvalidateRect(hwnd, NULL, FALSE);
        memset(&tme, 0, sizeof tme);
        tme.cbSize = sizeof tme;
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        d->screen_hover = -1;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- the keypad matrix and the discs ------------------------------------------------------ */

#define MX_CELL_W 52
#define MX_CELL_H 26
#define MX_GAP 4
#define MX_HEAD_H 22
#define MX_MID_W 82
#define MX_SEP 14
#define MX_STICK_LABEL_W 62

/* The size the view wants. */
void dbg_matrix_size(const debugger *d, int *w, int *h)
{
    const int pad_w = 3 * S(MX_CELL_W) + 2 * S(MX_GAP);
    *w = 2 * pad_w + S(MX_MID_W) + 2 * S(MX_SEP);
    *h = S(MX_HEAD_H) + 4 * (S(MX_CELL_H) + S(MX_GAP)) + S(16) + 2 * (S(MX_CELL_H) + S(MX_GAP) + d->ui_h + S(6));
}

/* A held key or disc contact in the accent, dark text on it. */
static void matrix_cell(debugger *d, HDC dc, RECT r, const char *text, int held)
{
    FillRect(dc, &r, held ? d->accent : GetSysColorBrush(COLOR_WINDOW));
    FrameRect(dc, &r, GetSysColorBrush(COLOR_BTNSHADOW));
    SetTextColor(dc, held ? RGB(0x1e, 0x1e, 0x1e) : GetSysColor(COLOR_WINDOWTEXT));
    InflateRect(&r, -2, 0);
    text_in(dc, text, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

/* Both 12-key keypads and the console's buttons as the machine latched them,
 * then both discs: the four directions and FIRE, the analog deflection. */
static void paint_matrix(debugger *d, HWND hwnd, HDC dc, const RECT *c)
{
    static const char *const dirs[5] = { "UP", "RIGHT", "DOWN", "LEFT", "FIRE" };
    const chfdebug_io *io = &d->io;
    const int pad_w = 3 * S(MX_CELL_W) + 2 * S(MX_GAP);
    const int mid_x = pad_w + S(MX_SEP);
    int s, i, y, port;
    (void)hwnd;
    FillRect(dc, c, GetSysColorBrush(COLOR_BTNFACE));
    SelectObject(dc, d->ui);
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    for (port = 0; port < 2; port++) {
        const int x0 = port == 0 ? 0 : mid_x + S(MX_MID_W) + S(MX_SEP);
        RECT r;
        char n[24];
        snprintf(n, sizeof n, "Player %d keypad", port + 1);
        SetRect(&r, x0, 0, x0 + pad_w, S(MX_HEAD_H));
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        text_in(dc, n, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        for (i = 0; i < 12; i++) {
            const int key = CHF_KEY_PAD_BASE(port) + i;
            const char *label = chfsession_key_label(io->machine, key);
            const int cx = x0 + (i % 3) * (S(MX_CELL_W) + S(MX_GAP)), cy = S(MX_HEAD_H) + (i / 3) * (S(MX_CELL_H) + S(MX_GAP));
            SetRect(&r, cx, cy, cx + S(MX_CELL_W), cy + S(MX_CELL_H));
            matrix_cell(d, dc, r, label ? label : "", (int)((io->keys >> key) & 1));
        }
    }
    {
        RECT r;
        static const int keys[3] = { CHF_KEY_START, CHF_KEY_OPTION, CHF_KEY_SELECT };
        SetRect(&r, mid_x, 0, mid_x + S(MX_MID_W), S(MX_HEAD_H));
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        text_in(dc, "Console", &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        for (i = 0; i < 4; i++) {
            const int cy = S(MX_HEAD_H) + i * (S(MX_CELL_H) + S(MX_GAP));
            SetRect(&r, mid_x, cy, mid_x + S(MX_MID_W), cy + S(MX_CELL_H));
            if (i < 3) matrix_cell(d, dc, r, chfsession_key_label(io->machine, keys[i]), (int)((io->keys >> keys[i]) & 1));
            else matrix_cell(d, dc, r, "RESET", io->reset_held);
        }
    }
    y = S(MX_HEAD_H) + 4 * (S(MX_CELL_H) + S(MX_GAP)) + S(16);
    for (s = 0; s < 2; s++) {
        RECT r;
        char n[96];
        SetRect(&r, 0, y, S(MX_STICK_LABEL_W), y + S(MX_CELL_H));
        snprintf(n, sizeof n, "Stick %d", s + 1);
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        text_in(dc, n, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        for (i = 0; i < 5; i++) {
            const int w = S(52);
            SetRect(&r, S(MX_STICK_LABEL_W) + i * (w + S(MX_GAP)), y, S(MX_STICK_LABEL_W) + i * (w + S(MX_GAP)) + w,
                    y + S(MX_CELL_H));
            matrix_cell(d, dc, r, dirs[i], (io->sticks[s] >> i) & 1);
        }
        snprintf(n, sizeof n, "disc x=%d y=%d%s   pots x=%02X y=%02X", io->disc[s][0], io->disc[s][1],
                 (io->disc[s][0] || io->disc[s][1]) ? " (analog)" : " (digital)", io->pot[s][0], io->pot[s][1]);
        SetRect(&r, 0, y + S(MX_CELL_H) + S(2), c->right, y + S(MX_CELL_H) + S(2) + d->ui_h + 2);
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        text_in(dc, n, &r, DT_LEFT | DT_SINGLELINE);
        y += S(MX_CELL_H) + S(MX_GAP) + d->ui_h + S(6);
    }
}

static LRESULT CALLBACK matrix_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_debugger;
    if (msg == WM_PAINT && d) { paint_buffered(d, hwnd, paint_matrix); return 0; }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- the classes ----------------------------------------------------------------------- */

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


/* Registers the window classes of the views, once. */
void dbg_views_register(HINSTANCE inst)
{
    register_class(inst, L"ChfDbgUvi", vpic_proc);
    register_class(inst, L"ChfDbgPic", pic_proc);
    register_class(inst, L"ChfDbgDisasm", disasm_proc);
    register_class(inst, L"ChfDbgObjects", objects_proc);
    register_class(inst, L"ChfDbgScreen", screen_proc);
    register_class(inst, L"ChfDbgMatrix", matrix_proc);
}
