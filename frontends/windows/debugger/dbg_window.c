/*
 * Debugger window (Win32) over the Signetics 2650 / 2637 UVI debugger engine,
 * via core/include/chfdebug.h. Mirrors the GTK and Qt debuggers tab for tab --
 * Prompt, CPU & RAM, Disassembly, Video, Sound & I/O, Breakpoints, Cart,
 * FujiNet -- built from plain common controls, with the disassembly, the
 * UVI's pictures and object tables, the character screen and the keypad
 * matrix drawn by hand.
 *
 * The Video tab is the showpiece: the display as the chip draws it (the
 * finished frame, the registers re-rendered with chosen layers and overlays,
 * or the frame as far as the beam has got), a hover that names the object or
 * character cell under the pointer, the objects boxed and numbered on the
 * picture, and beside it the object list with their bitmaps, the 16 x 13/26
 * character screen with its colour bits, every register decoded and editable,
 * the status and collision bits, and the glyph, object and palette sheets.
 *
 * The engine only exists while this window is showing: showing it attaches
 * (which stops the machine, as on every sibling) and hiding it detaches,
 * which lets the machine run on at full speed. The window refreshes on a
 * timer keyed to the engine's generation counter rather than on every tick,
 * and the timer exists only while the window does.
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

#include "dbg_internal.h"

#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif

#define TRACE_MAX 512        /* FujiNet packets shown */
#define MAILBOX_BASE 0x2B00  /* the cartridge's mailbox page, as the console reads it */
#define MAILBOX_SIZE 0x500

#define WM_DBG_ACCEPT  (WM_APP + 2)   /* wp: control id -- Enter in an edit */
#define WM_DBG_TAB     (WM_APP + 3)   /* Tab in the prompt: complete */
#define WM_DBG_HISTORY (WM_APP + 4)   /* wp: 0 Up (older), 1 Down (newer) in the prompt */

#define TIMER_REFRESH 1
#define TICKS_SLOW 5                  /* running: the slow pages every 5th 100 ms tick */


enum {
    IDC_RUN = 1000, IDC_STEP, IDC_OVER, IDC_OUT, IDC_SCAN, IDC_FRAME, IDC_LOAD_SYMBOLS, IDC_SAVE,
    IDC_STATUS, IDC_TABS,
    IDC_PROMPT_OUT, IDC_PROMPT_IN,
    IDC_FIELD0, IDC_FIELD_LAST = IDC_FIELD0 + NFIELDS - 1,
    IDC_FLAG0, IDC_FLAG_LAST = IDC_FLAG0 + NFLAGS - 1,
    IDC_INPUT0, IDC_INPUT_LAST = IDC_INPUT0 + NINPUTS - 1,
    IDC_RAS0, IDC_RAS_LAST = IDC_RAS0 + NSTACK - 1,
    IDC_CPU_STATE, IDC_MEM_VIEW, IDC_RAM_SPACE, IDC_RAM_GOTO, IDC_RAM_PREV, IDC_RAM_NEXT, IDC_RAM_ADDR,
    IDC_RAM_VAL,
    IDC_FOLLOW_PC, IDC_JUMP, IDC_DISASM,
    IDC_VIEW_MODE, IDC_LAYER0, IDC_LAYER_LAST = IDC_LAYER0 + NLAYERS - 1,
    IDC_OVL0, IDC_OVL_LAST = IDC_OVL0 + NOVERLAYS - 1,
    IDC_VPIC, IDC_VSTATUS, IDC_VTABS,
    IDC_OBJECTS, IDC_SCREEN, IDC_REGS_LIST, IDC_REG_ADDR, IDC_REG_VAL, IDC_REG_WRITE,
    IDC_STATUS_TEXT,
    IDC_SHEET0, IDC_SHEET_LAST = IDC_SHEET0 + NSHEETS - 1,
    IDC_IO_TEXT, IDC_IO_MATRIX,
    IDC_BP_LIST, IDC_BP_TYPE, IDC_BP_START, IDC_BP_END, IDC_BP_COND, IDC_BP_ADD,
    IDC_BP_REMOVE, IDC_BP_ENABLE, IDC_BP_CLEAR,
    IDC_CART_TEXT, IDC_CART_MAILBOX,
    IDC_FUJI_NOTE, IDC_FUJI_CLEAR, IDC_FUJI_TEXT, IDC_FUJI_STATE,
    IDC_SAVE_KIND0, IDC_SAVE_KIND_LAST = IDC_SAVE_KIND0 + NSAVE - 1,
    IDC_SYMS_CART, IDC_SYMS_FILE,
    IDC_LABEL_FIRST = 3000
};


debugger *g_debugger;

static const char *const kSaveKinds[NSAVE] = { "dis", "mailbox", "uvi", "font", "cart", "screen", "regs" };
static const char *const kSaveTitles[NSAVE] = {
    "Disassembly (PROG space)...", "Mailbox page...", "UVI registers...", "Glyph table...",
    "Cartridge image...", "Screen (PPM)...", "Registers (text)..." };

/* The editable CPU fields, in the order the CPU page lays them out. */
typedef struct { const char *name; int reg; int digits; } cpu_field;
static const cpu_field kFields[NFIELDS] = {
    { "R0", CHF_REG_R0, 2 }, { "R1", CHF_REG_R1, 2 }, { "R2", CHF_REG_R2, 2 }, { "R3", CHF_REG_R3, 2 },
    { "R1'", CHF_REG_R1B, 2 }, { "R2'", CHF_REG_R2B, 2 }, { "R3'", CHF_REG_R3B, 2 },
    { "PC", CHF_REG_PC, 4 }, { "PSU", CHF_REG_PSU, 2 }, { "PSL", CHF_REG_PSL, 2 },
    { "SP", CHF_REG_SP, 1 }, { "CC", CHF_REG_CC, 1 } };
static const char *const kFlagNames[NFLAGS] = { "F", "II", "C", "COM", "OVF", "WC", "IDC", "RS" };
static const int kFlagIds[NFLAGS] = { CHF_REG_F, CHF_REG_II, CHF_REG_C, CHF_REG_COM, CHF_REG_OVF, CHF_REG_WC,
                                      CHF_REG_IDC, CHF_REG_RS };
static const char *const kInputNames[NINPUTS] = { "SENSE (vblank)", "Halted", "IRQ line" };

/* The page names CHF_DEBUGGER_TAB accepts, by page. */
static const char *const kPageNames[PAGE_COUNT] = { "prompt", "cpu", "disassembly", "video", "io",
                                                    "breakpoints", "cart", "fujinet" };
static const wchar_t *const kPageTitles[PAGE_COUNT] = {
    L"Prompt", L"CPU && RAM", L"Disassembly", L"Video", L"Sound && I/O", L"Breakpoints", L"Cart",
    L"FujiNet" };
static const wchar_t *const kSectionTitles[SEC_COUNT] = {
    L"Objects", L"Screen", L"Registers", L"Status", L"Sheets" };

static const wchar_t *const kLayerNames[NLAYERS] = { L"Characters", L"Objects" };
static const unsigned kLayerBits[NLAYERS] = { CHFDEBUG_UVI_L_CHARS, CHFDEBUG_UVI_L_OBJECTS };
static const wchar_t *const kOverlayNames[NOVERLAYS] = {
    L"Object boxes", L"Character grid", L"Collisions", L"Beam cross-hair" };
static const unsigned kOverlayBits[NOVERLAYS] = {
    CHFDEBUG_UVI_F_BOXES, CHFDEBUG_UVI_F_GRID, CHFDEBUG_UVI_F_COLLIDE, CHFDEBUG_UVI_F_BEAM };

/* The collision registers: $19FC objects x characters, $19FD object pairs (a 0
 * bit is a hit). */
static const char *const kPairNames[6] = { "0-1", "0-2", "0-3", "1-2", "1-3", "2-3" };

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

/* Replaces a log's text where the reader left it, or, following, at its end. */
static void view_set_at(HWND h, const char *text, int follow)
{
    const int first = (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    wchar_t *w = view_wide(text);
    if (!w) return;
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

/* One of the chip's views into im: asks for its size first, grows the buffer
 * to fit and draws. An engine that does not answer the size query gets the
 * biggest picture's room instead. 0 (and an empty image) when there is none. */
static int fetch_view(debugger *d, image *im, int view, unsigned layers, unsigned flags, int scale)
{
    int w = 0, h = 0;
    im->w = im->h = 0;
    chfdebug_uvi_view(d->dbg, view, layers, flags, scale, NULL, 0, &w, &h);
    if (w > 0 && h > 0) {
        if (!image_reserve(im, (size_t)w * (size_t)h)) return 0;
    } else if (!image_reserve(im, (size_t)CHFDEBUG_UVI_VIEW_MAX_PIXELS * (size_t)scale * (size_t)scale)) {
        return 0;
    }
    w = h = 0;
    if (!chfdebug_uvi_view(d->dbg, view, layers, flags, scale, im->px, im->cap, &w, &h) || w <= 0 || h <= 0)
        return 0;
    im->w = w;
    im->h = h;
    return 1;
}

/* ---- the Video tab's state ---------------------------------------------------- */

/* The layers the checkboxes ask for. Only the re-rendered picture has layers
 * to choose; the other two are what the machine drew. */
static unsigned video_layers(const debugger *d)
{
    unsigned bits = 0;
    int i;
    if (SendMessageW(d->view_mode, CB_GETCURSEL, 0, 0) != CHFDEBUG_UVI_RENDER) return CHFDEBUG_UVI_L_ALL;
    for (i = 0; i < NLAYERS; i++)
        if (SendMessageW(d->layer[i], BM_GETCHECK, 0, 0) == BST_CHECKED) bits |= kLayerBits[i];
    return bits;
}

/* The overlays apply to all three pictures. */
static unsigned video_flags(const debugger *d)
{
    unsigned bits = 0;
    int i;
    for (i = 0; i < NOVERLAYS; i++)
        if (SendMessageW(d->overlay[i], BM_GETCHECK, 0, 0) == BST_CHECKED) bits |= kOverlayBits[i];
    return bits;
}

static void video_enable_state(debugger *d)
{
    const int mode = (int)SendMessageW(d->view_mode, CB_GETCURSEL, 0, 0);
    int i;
    for (i = 0; i < NLAYERS; i++) EnableWindow(d->layer[i], mode == CHFDEBUG_UVI_RENDER);
}

/* The 8 colours, sampled from the palette strip: the character screen and
 * the status text use the chip's own. */
static void refresh_palette(debugger *d)
{
    image *im = &d->palimg;
    int col;
    if (!fetch_view(d, im, CHFDEBUG_UVI_PALETTE, CHFDEBUG_UVI_L_ALL, 0, 4) || im->w < 8 || im->h < 1) return;
    for (col = 0; col < 8; col++) {
        const int x = col * (im->w / 8) + (im->w / 8) / 2, y = im->h / 2;
        d->pal[col] = im->px[y * im->w + x];
    }
}

/* The four objects' bitmaps at twice their size on a mid-grey so any colour
 * shows. */
static void refresh_objects(debugger *d)
{
    int k;
    for (k = 0; k < OBJ_COUNT; k++) {
        int w = 0, h = 0;
        d->obj_w[k] = d->obj_h[k] = 0;
        if (chfdebug_uvi_thing(d->dbg, CHFDEBUG_UVI_OBJECT, k, 2, 0x00707070, d->obj_bm[k], OBJ_BM_PX, &w, &h) &&
            w > 0 && h > 0 && (size_t)w * (size_t)h <= OBJ_BM_PX) {
            d->obj_w[k] = w;
            d->obj_h[k] = h;
        }
    }
    InvalidateRect(d->objects, NULL, FALSE);
}

/* The character screen: the cells, and the 64 glyphs they are drawn from. */
static void refresh_screen(debugger *d)
{
    int g, ok = 1;
    d->cell_rows = chfdebug_uvi_cells(d->dbg, d->cells);
    if (d->cell_rows < 0) d->cell_rows = 0;
    for (g = 0; g < 64; g++) {
        int w = 0, h = 0;
        if (!chfdebug_uvi_thing(d->dbg, CHFDEBUG_UVI_GLYPH, g, 1, GLYPH_CLEAR, d->glyph[g], 64, &w, &h) ||
            w != 8 || h != 8)
            ok = 0;
    }
    d->glyph_ok = ok;
    InvalidateRect(d->screen, NULL, FALSE);
}

/* $000-$2FF with the names and decoded values the chip gives them. */
static void refresh_regs(debugger *d)
{
    int off;
    for (off = 0; off < UVI_REGS; off++)
        chfdebug_uvi_describe(d->dbg, (unsigned)off, d->reg_name[off], sizeof d->reg_name[off],
                              d->reg_text[off], sizeof d->reg_text[off]);
    InvalidateRect(d->regs_list, NULL, FALSE);
}

/* Beam, screen, colours, collisions, sound and the pots, decoded. */
static void refresh_status_text(debugger *d)
{
    const chfdebug_uvi *u = &d->uvi;
    static char t[4096];
    size_t len = 0;
    int b;
    len = addf(t, len, sizeof t,
        "BEAM  line %d/%d   row %d   SENSE %d\n\n"
        "SCREEN  ($18FC, $18FD, $18FE, $19F8)\n"
        "  first line          %d\n"
        "  fine scroll         %d pixels\n"
        "  rows                %d of %d lines each%s\n"
        "  block graphics      %s from the top of the screen\n"
        "  multicolor          %s\n"
        "  pots                %s\n\n",
        u->line, u->lines, u->charline, u->vblank, u->first_line, u->shift, u->rows, u->cell_height,
        u->two_screens ? "  (two screens)" : "", u->block_graphics ? "on" : "off", u->multicolor ? "on" : "off",
        u->pot_y ? "reading the Y axis" : "reading the X axis");
    len = addf(t, len, sizeof t,
        "COLOURS\n"
        "  screen              %d  %s\n"
        "  characters          %d  %s\n"
        "  alternate screen    %d  %s\n"
        "  alternate chars     %d  %s\n\n",
        u->screen_colour, chfdebug_uvi_colour_name(u->screen_colour), u->char_colour,
        chfdebug_uvi_colour_name(u->char_colour), u->alt_screen_colour,
        chfdebug_uvi_colour_name(u->alt_screen_colour), u->alt_char_colour,
        chfdebug_uvi_colour_name(u->alt_char_colour));
    len = addf(t, len, sizeof t, "COLLISIONS  ($19FC, $19FD: a 0 bit is a hit)\n");
    for (b = 0; b < 4; b++)
        len = addf(t, len, sizeof t, "  object %d  hits a character   %s\n", b,
                   (u->coll_chars >> b) & 1 ? "no" : "YES");
    for (b = 0; b < 6; b++)
        len = addf(t, len, sizeof t, "  objects %s  overlap        %s\n", kPairNames[b],
                   (u->coll_objects >> b) & 1 ? "no" : "YES");
    len = addf(t, len, sizeof t,
        "\nSOUND  ($18FD, $18FE)\n"
        "  pitch divider       %d\n"
        "  volume              %d of 7\n"
        "  tone                %s\n"
        "  noise               %s\n",
        u->sound_pitch, u->sound_volume, u->sound_tone ? "on" : "off", u->sound_noise ? "on" : "off");
    view_set(d->status_text, t);
}

/* The glyph, object and palette sheets, each as the engine draws it. */
static void layout(debugger *d);

static void refresh_sheets(debugger *d)
{
    static const int views[NSHEETS] = { CHFDEBUG_UVI_FONT, CHFDEBUG_UVI_OBJECTS, CHFDEBUG_UVI_PALETTE };
    static const int scales[NSHEETS] = { 1, 1, 1 };
    int i, changed = 0;
    for (i = 0; i < NSHEETS; i++) {
        const int w = d->sheet_pv[i].img.w, h = d->sheet_pv[i].img.h;
        fetch_view(d, &d->sheet_pv[i].img, views[i], CHFDEBUG_UVI_L_ALL, 0, scales[i]);
        if (d->sheet_pv[i].img.w != w || d->sheet_pv[i].img.h != h) changed = 1;
        InvalidateRect(d->sheet[i], NULL, FALSE);
    }
    if (changed) layout(d);
}

/* What the pointer is over, in words: the topmost thing first. */
static void describe_hit(const debugger *d, int kind, int idx, char *out, int n)
{
    const chfdebug_uvi *u = &d->uvi;
    if (kind == CHFDEBUG_UVI_HIT_OBJECT && idx >= 0 && idx < 4) {
        const chfdebug_uvi_object *o = &u->obj[idx];
        snprintf(out, (size_t)n, "Object %d: x=%d y=%d colour=%s udc=$%02X%s", idx, o->x, o->y,
                 chfdebug_uvi_colour_name(o->colour), o->udc, o->double_height ? " double height" : "");
    } else if (kind == CHFDEBUG_UVI_HIT_CELL && idx >= 0 && idx < CHFDEBUG_UVI_COLS * CHFDEBUG_UVI_MAX_ROWS) {
        snprintf(out, (size_t)n, "Character row %d col %d", idx / CHFDEBUG_UVI_COLS, idx % CHFDEBUG_UVI_COLS);
        if (idx / CHFDEBUG_UVI_COLS < d->cell_rows) {
            const chfdebug_uvi_cell *c = &d->cells[idx];
            char more[80];
            snprintf(more, sizeof more, ": code $%02X glyph $%02X %s fg %s bg %s", c->code, c->glyph,
                     c->graphics ? "block graphics" : "", chfdebug_uvi_colour_name(c->fg),
                     chfdebug_uvi_colour_name(c->bg));
            strncat(out, more, (size_t)n - strlen(out) - 1);
        }
    } else {
        snprintf(out, (size_t)n, "Nothing");
    }
}

/* The pointer's pixel in the frame: asks the engine what draws there and
 * puts the answer under the picture. */
void dbg_update_hover(debugger *d)
{
    char text[240], more[40];
    if (!d->hover_valid) {
        d->nhits = 0;
        dbg_label_set(d->vstatus, "Hover over the picture to name the object or character under the pointer.");
        InvalidateRect(d->vpic, NULL, FALSE);
        return;
    }
    d->nhits = chfdebug_uvi_hit_test(d->dbg, d->hover_x, d->hover_y, d->hits,
                                     (int)(sizeof d->hits / sizeof d->hits[0]));
    if (d->nhits < 0) d->nhits = 0;
    if (d->nhits == 0) {
        snprintf(text, sizeof text, "Nothing here (x=%d y=%d)", d->hover_x, d->hover_y);
    } else {
        describe_hit(d, d->hits[0].kind, d->hits[0].index, text, sizeof text);
        if (d->nhits > 1) {
            snprintf(more, sizeof more, "   (+%d under it)", d->nhits - 1);
            strncat(text, more, sizeof text - strlen(text) - 1);
        }
    }
    dbg_label_set(d->vstatus, text);
    InvalidateRect(d->vpic, NULL, FALSE);
}

/* The Video tab: the snapshot and the picture every time (at most ten times
 * a second, on the timer), and the section showing. The registers and the
 * sheets are the heavy ones: they follow only when `slow`. */
static void refresh_video(debugger *d, int slow)
{
    const int mode = (int)SendMessageW(d->view_mode, CB_GETCURSEL, 0, 0);
    chfdebug_uvi_get(d->dbg, &d->uvi);
    refresh_palette(d);
    video_enable_state(d);
    fetch_view(d, &d->vimg, mode < 0 ? CHFDEBUG_UVI_FRAME : mode, video_layers(d), video_flags(d), 1);
    if (d->vsec != SEC_SCREEN && d->cell_rows == 0) refresh_screen(d);   /* the hover names cells too */
    dbg_update_hover(d);
    switch (d->vsec) {
    case SEC_OBJECTS: refresh_objects(d); break;
    case SEC_SCREEN: refresh_screen(d); break;
    case SEC_REGS: if (slow) refresh_regs(d); break;
    case SEC_STATUS: refresh_status_text(d); break;
    case SEC_SHEETS: if (slow) refresh_sheets(d); break;
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
    return sel == 1 ? CHF_SPACE_UVI : sel == 2 ? CHF_SPACE_CART : CHF_SPACE_PROG;
}

static int mem_digits(int space)
{
    return space == CHF_SPACE_PROG ? 4 : space == CHF_SPACE_UVI ? 3 : 5;
}

/* The memory view: 16 rows of 16 bytes from mem_base in the space showing. */
static void refresh_mem(debugger *d)
{
    static uint8_t bytes[16 * MEM_ROWS];
    static char dump[2048];
    const int space = mem_space_now(d);
    const uint32_t size = (uint32_t)chfdebug_space_size(d->dbg, space);
    char title[96];
    if (d->mem_base >= size) d->mem_base = size > sizeof bytes ? (size - 1) & ~0xfu : 0;
    memset(bytes, 0xff, sizeof bytes);
    chfdebug_read(d->dbg, space, d->mem_base, bytes, (int)sizeof bytes);
    hexdump(dump, sizeof dump, bytes, (int)sizeof bytes, d->mem_base, mem_digits(space));
    view_set(d->mem_view, dump);
    snprintf(title, sizeof title, "%s from $%0*X (size $%X; reads have no side effects)",
             space == CHF_SPACE_PROG ? "Program space" : space == CHF_SPACE_UVI ? "UVI registers" : "Cartridge image",
             mem_digits(space), d->mem_base, size);
    dbg_label_set(d->mem_label, title);
}

static void refresh_cpu(debugger *d)
{
    chfdebug_cpu *c = &d->cpu;
    static char buf[1024];
    int i;
    chfdebug_cpu_get(d->dbg, c);
    {
        const int v[NFIELDS] = { c->r0, c->r[c->rs ? 1 : 0][1], c->r[c->rs ? 1 : 0][2], c->r[c->rs ? 1 : 0][3],
                                 c->r[1][1], c->r[1][2], c->r[1][3], c->pc, c->psu, c->psl, c->sp, c->cc };
        for (i = 0; i < NFIELDS; i++) {
            if (GetFocus() == d->fld[i]) continue;      /* do not fight the user's typing */
            snprintf(buf, sizeof buf, "%0*X", kFields[i].digits, v[i] & ((1 << (4 * kFields[i].digits)) - 1));
            dbg_label_set(d->fld[i], buf);
        }
    }
    {
        const int flags[NFLAGS] = { c->flag, c->ii, c->c, c->com, c->ovf, c->wc, c->idc, c->rs };
        const int inputs[NINPUTS] = { c->sense, c->halted, c->irq_line };
        for (i = 0; i < NFLAGS; i++)
            SendMessageW(d->flag[i], BM_SETCHECK, flags[i] ? BST_CHECKED : BST_UNCHECKED, 0);
        for (i = 0; i < NINPUTS; i++)
            SendMessageW(d->input[i], BM_SETCHECK, inputs[i] ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    snprintf(buf, sizeof buf, "Return address stack  (SP %d; the live returns are 1..SP)", c->sp);
    dbg_label_set(d->stack_head, buf);
    for (i = 0; i < NSTACK; i++) {
        if (GetFocus() == d->stack_edit[i]) continue;
        snprintf(buf, sizeof buf, "%04X", c->ras[i] & 0x7fff);
        dbg_label_set(d->stack_edit[i], buf);
        snprintf(buf, sizeof buf, "%s%d", i == c->sp ? ">" : " ", i);
        dbg_label_set(d->stack_label[i], buf);
    }
    snprintf(buf, sizeof buf,
        "PC $%04X  (page $%04X, IAR $%04X)   registers: set %d\n"
        "R0 $%02X   R1-R3  $%02X $%02X $%02X   R1'-R3' $%02X $%02X $%02X\n"
        "PSU $%02X  S%d F%d II%d SP%d      PSL $%02X  CC%d IDC%d RS%d WC%d OVF%d COM%d C%d\n"
        "%s\n"
        "Beam         line %d   pixel %d   frame %u\n"
        "Cycles %llu   instructions %llu   illegal opcodes %u\n"
        "Machine      %s\n",
        c->pc & 0x7fff, c->page, c->iar, c->rs,
        c->r0, c->r[0][1], c->r[0][2], c->r[0][3], c->r[1][1], c->r[1][2], c->r[1][3],
        c->psu & 0xff, c->sense, c->flag, c->ii, c->sp, c->psl & 0xff, c->cc, c->idc, c->rs, c->wc, c->ovf,
        c->com, c->c,
        c->halted ? "HALTed, waiting for an interrupt" : "running",
        c->vpos, c->hpos, c->frame, (unsigned long long)c->cycles, (unsigned long long)c->instructions,
        c->illegal_count,
        chf_machine_name(c->machine));
    view_set(d->cpu_state, buf);
    refresh_mem(d);
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

/* The console's inputs as the CPU reads them, and the beeper. */
static void refresh_io(debugger *d)
{
    chfdebug_io *io = &d->io;
    static char s[4096];
    size_t len = 0;
    int i;
    chfdebug_io_get(d->dbg, io);
    len = addf(s, len, sizeof s,
        "Machine         %s%s\n"
        "BIOS            none: the console has no BIOS\n"
        "Cartridge       %s\n"
        "Console RESET   %s      SENSE %d   FLAG %d\n\n",
        chf_machine_name(io->machine), io->is_pal ? " (not verified against MAME)" : "",
        io->cart_type == 3 ? "FujiNet" : io->cart_type == 2 ? "Golf" : io->cart_type == 1 ? "standard" : "none",
        io->reset_held ? "held" : "released", io->sense, io->flag);
    len = addf(s, len, sizeof s, "Keypad columns ($1900-$1907):");
    for (i = 0; i < 8; i++) len = addf(s, len, sizeof s, " %02X", io->pad[i]);
    len = addf(s, len, sizeof s, "\nPanel ($1908) = $%02X  (b0 START, b1 OPTION, b2 SELECT)\n\n", io->panel);
    for (i = 0; i < 2; i++)
        len = addf(s, len, sizeof s,
            "Stick %d   digital $%02X   disc %d,%d%s   pots x $%02X y $%02X\n", i + 1, io->sticks[i],
            io->disc[i][0], io->disc[i][1], (io->disc[i][0] || io->disc[i][1]) ? " (analog overrides)" : "",
            io->pot[i][0], io->pot[i][1]);
    len = addf(s, len, sizeof s,
        "\nSound  ($18FD, $18FE)\n"
        "  pitch divider %d   volume %d of 7   tone %s   noise %s\n"
        "  %llu samples made since power-on\n",
        d->uvi.sound_pitch, d->uvi.sound_volume, d->uvi.sound_tone ? "on" : "off",
        d->uvi.sound_noise ? "on" : "off", (unsigned long long)io->audio_samples);
    view_set(d->io_text, s);
    InvalidateRect(d->io_matrix, NULL, FALSE);
}

static void refresh_bps(debugger *d)
{
    int i, sel = (int)SendMessageW(d->bp_list, LB_GETCURSEL, 0, 0);
    d->nbps = chfdebug_breakpoint_list(d->dbg, d->bps, MAX_BPS);
    SendMessageW(d->bp_list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(d->bp_list, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < d->nbps; i++) {
        const chfdebug_breakpoint *b = &d->bps[i];
        char line[256], kind[8] = "", where[40];
        wchar_t w[256];
        if (b->type & CHFDEBUG_BP_EXEC) strcat(kind, "x");
        if (b->type & CHFDEBUG_BP_READ) strcat(kind, "r");
        if (b->type & CHFDEBUG_BP_WRITE) strcat(kind, "w");
        if (b->start == b->end) snprintf(where, sizeof where, "$%04X", b->start);
        else snprintf(where, sizeof where, "$%04X-$%04X", b->start, b->end);
        snprintf(line, sizeof line, "%c %3d  %-3s  %-14s  hits %-6u %s%s", b->enabled ? '+' : '-', b->id, kind,
                 where, b->hits, b->condition[0] ? "if " : "", b->condition);
        dbg_wide_from(line, w, 256);
        SendMessageW(d->bp_list, LB_ADDSTRING, 0, (LPARAM)w);
    }
    if (d->nbps == 0)
        SendMessageW(d->bp_list, LB_ADDSTRING, 0,
                     (LPARAM)L"(no breakpoints -- click a disassembly line, or add one below)");
    if (sel >= 0 && sel < d->nbps) SendMessageW(d->bp_list, LB_SETCURSEL, (WPARAM)sel, 0);
    SendMessageW(d->bp_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(d->bp_list, NULL, FALSE);
}

static int cart_is_fujinet(const chfdebug_cart *c)
{
    return c->present && c->type == 3;
}

static void refresh_cart(debugger *d)
{
    chfdebug_cart *c = &d->cart_state;
    char info[256];
    static char s[4096];
    static char dump[MAILBOX_SIZE / 16 * 56 + 256];
    size_t len = 0;
    chfdebug_cart_get(d->dbg, c);
    chfdebug_cart_info(d->dbg, info, sizeof info);
    if (!c->present) {
        view_set(d->cart_text, "No cartridge is running.");
        view_set(d->cart_mailbox, "");
        return;
    }
    len = addf(s, len, sizeof s, "%s\n\nType            %s\n", info, c->type_name ? c->type_name : "?");
    if (cart_is_fujinet(c)) {
        len = addf(s, len, sizeof s,
            "Link            %s%s\n"
            "Mailbox         %s\n"
            "Running         %s\n"
            "Armed           %s\n"
            "Live image      %u bytes, CRC-32 %08X%s\n",
            c->link_up ? "up" : "down", c->link_off ? " (switched off)" : "",
            c->mailbox ? "open" : "closed",
            c->app_live ? "a FujiNet app" : c->booted_game ? "a game from the network" : "CONFIG",
            c->armed ? "yes (a swap waits for the next reset)" : "no", c->live_size,
            c->live_crc, c->staged_mailbox ? ", claims the mailbox" : "");
        if (c->staged_size)
            len = addf(s, len, sizeof s, "Staged          %s%s, %u bytes, CRC-32 %08X\n",
                       c->staged_app ? "an app" : "a game", c->staged_mailbox ? " that claims the mailbox" : "",
                       c->staged_size, c->staged_crc);
        else
            len = addf(s, len, sizeof s, "Staged          nothing\n");
        len = addf(s, len, sizeof s,
            "Swaps           %d\n"
            "APPBANK pages   %d, showing %d at $0800\n"
            "Boot            state %d   %d%%   error %d\n\n"
            "Mailbox         ACKSEQ $%02X   STATUS $%02X   last error %u\n"
            "                reply cmd $%02X   RXLEN %u\n"
            "Queue           %u deep\n",
            c->swaps, c->nbanks, c->fbank, c->boot_state, c->boot_pct, c->boot_err, c->ackseq, c->status,
            c->last_error, c->reply_cmd, c->rxlen, c->queue_depth);
        hexdump(dump, sizeof dump, c->mailbox_page, MAILBOX_SIZE, MAILBOX_BASE, 4);
        view_set(d->cart_mailbox, dump);
    } else {
        len = addf(s, len, sizeof s, "Image           %u bytes\n", c->rom_size);
        view_set(d->cart_mailbox, "(the mailbox page is the FujiNet cartridge's)");
    }
    view_set(d->cart_text, s);
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
 * parts (registers, sheets) on the Video tab. */
static void refresh_page(debugger *d, int slow)
{
    switch (d->page) {
    case PAGE_CPU: refresh_cpu(d); break;
    case PAGE_DISASM: dbg_refresh_disasm(d); break;
    case PAGE_VIDEO: refresh_video(d, slow); break;
    case PAGE_IO:
        chfdebug_uvi_get(d->dbg, &d->uvi);
        refresh_io(d);
        break;
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
 * opened cartridge (game.map / .lst / .sym next to game.bin). */
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

/* Breakpoints are on PROG space (the UVI is at $1800-$1AFF in it). */
static void add_breakpoint(debugger *d)
{
    static const int types[4] = { CHFDEBUG_BP_EXEC, CHFDEBUG_BP_READ, CHFDEBUG_BP_WRITE,
                                  CHFDEBUG_BP_READ | CHFDEBUG_BP_WRITE };
    char a[64], b[64], cond[256];
    int start, end, sel, id;
    edit_text(d->bp_start, a, sizeof a);
    edit_text(d->bp_end, b, sizeof b);
    edit_text(d->bp_cond, cond, sizeof cond);
    sel = (int)SendMessageW(d->bp_type, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel > 3) sel = 0;
    start = resolve_addr(d, a);
    if (start < 0 || start > 0x7fff) {
        chf_message(d->hwnd, "The start is not an address ($0000-$7FFF) or label.", "Breakpoint",
                    MB_ICONWARNING | MB_OK);
        return;
    }
    end = b[0] ? resolve_addr(d, b) : start;
    if (end < start) end = start;
    if (end > 0x7fff) end = 0x7fff;
    id = chfdebug_breakpoint_add(d->dbg, types[sel], CHF_SPACE_PROG, (uint16_t)start, (uint16_t)end, cond);
    if (id < 0) {
        chf_message(d->hwnd, "The condition does not parse.", "Breakpoint", MB_ICONWARNING | MB_OK);
        return;
    }
    SetWindowTextW(d->bp_start, L"");
    SetWindowTextW(d->bp_end, L"");
    SetWindowTextW(d->bp_cond, L"");
    refresh_bps(d);
    dbg_refresh_disasm(d);
}

static void on_accept(debugger *d, int id)
{
    char buf[512];
    long v;

    if (id >= IDC_FIELD0 && id <= IDC_FIELD_LAST) {
        edit_text(d->fld[id - IDC_FIELD0], buf, sizeof buf);
        if (parse_num(buf, &v)) chfdebug_cpu_set(d->dbg, kFields[id - IDC_FIELD0].reg, (int)v);
        SetFocus(d->hwnd);
        refresh_all(d);
        return;
    }
    if (id >= IDC_RAS0 && id <= IDC_RAS_LAST) {
        const int n = id - IDC_RAS0;
        edit_text(d->stack_edit[n], buf, sizeof buf);
        if (parse_num(buf, &v)) chfdebug_cpu_set(d->dbg, CHF_REG_RAS0 + n, (int)v);
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
    case IDC_JUMP:
        edit_text(d->jump, buf, sizeof buf);
        jump_to(d, buf);
        break;
    case IDC_BP_START:
    case IDC_BP_END:
    case IDC_BP_COND:
        add_breakpoint(d);
        break;
    case IDC_REG_ADDR:
    case IDC_REG_VAL:
        SendMessageW(d->hwnd, WM_COMMAND, MAKEWPARAM(IDC_REG_WRITE, BN_CLICKED), (LPARAM)d->reg_write);
        break;
    default: break;
    }
}

/* Write a UVI register: the CPU address ($18F0) or the offset ($0F0); only
 * while stopped, as the engine's contract has it. */
static void write_uvi_register(debugger *d)
{
    char a[64], b[64];
    long addr, val;
    edit_text(d->reg_addr, a, sizeof a);
    edit_text(d->reg_val, b, sizeof b);
    if (!chfdebug_is_stopped(d->dbg)) {
        chf_message(d->hwnd, "Stop the machine first (F5): registers are edited while it is stopped.",
                   "UVI register", MB_ICONINFORMATION | MB_OK);
        return;
    }
    if (parse_num(a, &addr) && addr >= 0x1800) addr -= 0x1800;
    else if (!parse_num(a, &addr)) addr = -1;
    if (addr < 0 || addr >= UVI_REGS || !parse_num(b, &val) || val < 0 || val > 0xff) {
        chf_message(d->hwnd, "The address is the CPU address ($1800-$1AFF) or the offset ($000-$2FF), the value a byte, "
                             "both in hex ($18F0, 3F).", "UVI register", MB_ICONWARNING | MB_OK);
        return;
    }
    chfdebug_uvi_write(d->dbg, (unsigned)addr, (uint8_t)val);
    refresh_all(d);
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
    static const wchar_t *const views[3] = { L"Frame (what the display shows)", L"Rendered from the registers",
                                             L"Frame in progress (to the beam)" };
    static const wchar_t *const bp_types[4] = { L"Execute", L"Read", L"Write", L"Read/Write" };
    static const wchar_t *const ram_spaces[NSPACES] = { L"Program (CPU view)", L"UVI registers", L"Cartridge" };
    static const char *const sheet_names[NSHEETS] = {
        "Glyph table: 64 characters ($38-$3F are the UDCs)",
        "Objects: the four images", "Palette: the UVI's 8 colours" };
    TCITEMW item;
    LVCOLUMNW col;
    int i;

    d->tabs = mk(d, 0, WC_TABCONTROLW, L"", WS_CLIPSIBLINGS | WS_TABSTOP, IDC_TABS, -1, -1, 0);
    memset(&item, 0, sizeof item);
    item.mask = TCIF_TEXT;
    for (i = 0; i < PAGE_COUNT; i++) {
        item.pszText = (wchar_t *)kPageTitles[i];
        SendMessageW(d->tabs, TCM_INSERTITEMW, (WPARAM)i, (LPARAM)&item);
    }

    /* Toolbar: the stepping buttons, the symbol and save menus (they matter
     * on every page), then the stop reason. Run/Stop is owner-drawn so it
     * can wear the accent while the machine is stopped. */
    d->run_btn = mk(d, 0, L"BUTTON", L"Stop (F5)", BS_OWNERDRAW | WS_TABSTOP, IDC_RUN, -1, -1, 0);
    d->step_btn = button(d, L"Step (F7)", IDC_STEP, -1, -1);
    d->over_btn = button(d, L"Over (F8)", IDC_OVER, -1, -1);
    d->out_btn = button(d, L"Out (Shift+F8)", IDC_OUT, -1, -1);
    d->scan_btn = button(d, L"Scanline+1", IDC_SCAN, -1, -1);
    d->frame_btn = button(d, L"Frame+1", IDC_FRAME, -1, -1);
    d->load_symbols = button(d, L"Load symbols...", IDC_LOAD_SYMBOLS, -1, -1);
    d->save = button(d, L"Save...", IDC_SAVE, -1, -1);
    d->status = mk(d, 0, L"STATIC", L"Running", SS_RIGHT | SS_ENDELLIPSIS | SS_NOPREFIX, IDC_STATUS, -1, -1, 0);

    /* Prompt */
    d->prompt_out = view(d, IDC_PROMPT_OUT, PAGE_PROMPT, -1, 1);
    SetWindowTextW(d->prompt_out,
                   L"Fairchild Channel F debugger prompt (Signetics 2650 / 2637 UVI). Type 'help' for every command.\r\n");
    d->prompt_in = field(d, IDC_PROMPT_IN, PAGE_PROMPT, -1, 1, 0);
    SendMessageW(d->prompt_in, EM_SETCUEBANNER, TRUE,
                 (LPARAM)L"command (help, step, break, bpw, print, mem, runto, uvi, ...) - Tab completes, Up/Down recall");

    /* CPU & RAM */
    for (i = 0; i < NFIELDS; i++) {
        d->fld_label[i] = label(d, kFields[i].name, PAGE_CPU, -1);
        d->fld[i] = field(d, IDC_FIELD0 + i, PAGE_CPU, -1, 1, kFields[i].digits);
    }
    for (i = 0; i < NFLAGS; i++) {
        wchar_t w[16];
        dbg_wide_from(kFlagNames[i], w, 16);
        d->flag[i] = checkbox(d, w, IDC_FLAG0 + i, PAGE_CPU, -1);
    }
    for (i = 0; i < NINPUTS; i++) {
        wchar_t w[32];
        dbg_wide_from(kInputNames[i], w, 32);
        /* state the CPU samples, not set from here: shown, not edited */
        d->input[i] = checkbox(d, w, IDC_INPUT0 + i, PAGE_CPU, -1);
        EnableWindow(d->input[i], FALSE);
    }
    d->stack_head = label(d, "Return address stack", PAGE_CPU, -1);
    for (i = 0; i < NSTACK; i++) {
        char name[8];
        snprintf(name, sizeof name, " %d", i);
        d->stack_label[i] = label(d, name, PAGE_CPU, -1);
        d->stack_edit[i] = field(d, IDC_RAS0 + i, PAGE_CPU, -1, 1, 4);
    }
    d->cpu_state = view(d, IDC_CPU_STATE, PAGE_CPU, -1, 0);
    d->mem_label = label(d, "Memory", PAGE_CPU, -1);
    d->mem_view = view(d, IDC_MEM_VIEW, PAGE_CPU, -1, 0);
    d->ram_space = combo(d, IDC_RAM_SPACE, ram_spaces, NSPACES, PAGE_CPU, -1);
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
    d->disasm_hint = label(d, "Click a line to toggle its breakpoint; scroll to browse", PAGE_DISASM, -1);
    d->disasm = mk(d, WS_EX_CLIENTEDGE, L"ChfDbgDisasm", L"", WS_TABSTOP, IDC_DISASM, PAGE_DISASM, -1, 1);

    /* Video: the picture and what controls it ... */
    d->view_label = label(d, "View", PAGE_VIDEO, -1);
    d->view_mode = combo(d, IDC_VIEW_MODE, views, 3, PAGE_VIDEO, -1);
    d->layers_label = label(d, "Layers", PAGE_VIDEO, -1);
    for (i = 0; i < NLAYERS; i++) {
        d->layer[i] = checkbox(d, kLayerNames[i], IDC_LAYER0 + i, PAGE_VIDEO, -1);
        SendMessageW(d->layer[i], BM_SETCHECK, BST_CHECKED, 0);
    }
    d->overlays_label = label(d, "Overlays", PAGE_VIDEO, -1);
    for (i = 0; i < NOVERLAYS; i++) {
        d->overlay[i] = checkbox(d, kOverlayNames[i], IDC_OVL0 + i, PAGE_VIDEO, -1);
        if (kOverlayBits[i] == CHFDEBUG_UVI_F_BOXES) SendMessageW(d->overlay[i], BM_SETCHECK, BST_CHECKED, 0);
    }
    d->vpic = mk(d, WS_EX_CLIENTEDGE, L"ChfDbgUvi", L"", 0, IDC_VPIC, PAGE_VIDEO, -1, 0);
    d->vstatus = label(d, "Hover over the picture to name the object or character under the pointer.", PAGE_VIDEO, -1);
    /* ... and the sections beside it. The strip is a tab control used as a
     * strip alone, the sections below it controls of their own. */
    d->vtabs = mk(d, 0, WC_TABCONTROLW, L"", WS_CLIPSIBLINGS | WS_TABSTOP, IDC_VTABS, PAGE_VIDEO, -1, 0);
    for (i = 0; i < SEC_COUNT; i++) {
        item.pszText = (wchar_t *)kSectionTitles[i];
        SendMessageW(d->vtabs, TCM_INSERTITEMW, (WPARAM)i, (LPARAM)&item);
    }
    d->objects = mk(d, WS_EX_CLIENTEDGE, L"ChfDbgObjects", L"", 0, IDC_OBJECTS, PAGE_VIDEO, SEC_OBJECTS, 1);
    d->screen = mk(d, WS_EX_CLIENTEDGE, L"ChfDbgScreen", L"", 0, IDC_SCREEN, PAGE_VIDEO, SEC_SCREEN, 1);
    d->screen_status = label(d, "Hover over a cell for its code, glyph and colours.", PAGE_VIDEO, SEC_SCREEN);
    d->regs_list = mk(d, WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                      LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP | WS_VSCROLL,
                      IDC_REGS_LIST, PAGE_VIDEO, SEC_REGS, 1);
    ListView_SetExtendedListViewStyle(d->regs_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    memset(&col, 0, sizeof col);
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    {
        static const wchar_t *const heads[4] = { L"Addr", L"Register", L"Value", L"Meaning" };
        for (i = 0; i < 4; i++) {
            col.pszText = (wchar_t *)heads[i];
            col.cx = 80;
            SendMessageW(d->regs_list, LVM_INSERTCOLUMNW, (WPARAM)i, (LPARAM)&col);
        }
    }
    ListView_SetItemCountEx(d->regs_list, UVI_REGS, LVSICF_NOINVALIDATEALL);
    d->reg_addr_label = label(d, "Register $", PAGE_VIDEO, SEC_REGS);
    d->reg_addr = field(d, IDC_REG_ADDR, PAGE_VIDEO, SEC_REGS, 1, 0);
    d->reg_val_label = label(d, "value $", PAGE_VIDEO, SEC_REGS);
    d->reg_val = field(d, IDC_REG_VAL, PAGE_VIDEO, SEC_REGS, 1, 0);
    d->reg_write = button(d, L"Write", IDC_REG_WRITE, PAGE_VIDEO, SEC_REGS);
    d->status_text = view(d, IDC_STATUS_TEXT, PAGE_VIDEO, SEC_STATUS, 1);
    for (i = 0; i < NSHEETS; i++) {
        d->sheet_label[i] = label(d, sheet_names[i], PAGE_VIDEO, SEC_SHEETS);
        d->sheet[i] = mk(d, WS_EX_CLIENTEDGE, L"ChfDbgPic", L"", 0, IDC_SHEET0 + i, PAGE_VIDEO, SEC_SHEETS, 0);
        d->sheet_pv[i].center = 1;
        SetWindowLongPtrW(d->sheet[i], GWLP_USERDATA, (LONG_PTR)&d->sheet_pv[i]);
    }

    /* Sound & I/O */
    d->io_text = view(d, IDC_IO_TEXT, PAGE_IO, -1, 1);
    d->io_matrix = mk(d, 0, L"ChfDbgMatrix", L"", 0, IDC_IO_MATRIX, PAGE_IO, -1, 0);

    /* Breakpoints */
    d->bp_list = mk(d, WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_TABSTOP,
                    IDC_BP_LIST, PAGE_BREAKS, -1, 1);
    d->bp_type = combo(d, IDC_BP_TYPE, bp_types, 4, PAGE_BREAKS, -1);
    d->bp_start_label = label(d, "Address", PAGE_BREAKS, -1);
    d->bp_start = field(d, IDC_BP_START, PAGE_BREAKS, -1, 1, 0);
    d->bp_end_label = label(d, "to", PAGE_BREAKS, -1);
    d->bp_end = field(d, IDC_BP_END, PAGE_BREAKS, -1, 1, 0);
    d->bp_cond_label = label(d, "if", PAGE_BREAKS, -1);
    d->bp_cond = field(d, IDC_BP_COND, PAGE_BREAKS, -1, 1, 0);
    SendMessageW(d->bp_cond, EM_SETCUEBANNER, TRUE, (LPARAM)L"optional condition, e.g. value == $FF");
    d->bp_add = button(d, L"Add", IDC_BP_ADD, PAGE_BREAKS, -1);
    d->bp_remove = button(d, L"Remove", IDC_BP_REMOVE, PAGE_BREAKS, -1);
    d->bp_enable = button(d, L"Enable/Disable", IDC_BP_ENABLE, PAGE_BREAKS, -1);
    d->bp_clear = button(d, L"Clear all", IDC_BP_CLEAR, PAGE_BREAKS, -1);

    /* Cart */
    d->cart_text = view(d, IDC_CART_TEXT, PAGE_CART, -1, 1);
    d->cart_mailbox_label = label(d, "The mailbox page the console reads ($2B00-$2FFF)", PAGE_CART, -1);
    d->cart_mailbox = view(d, IDC_CART_MAILBOX, PAGE_CART, -1, 1);

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
    const int rh = d->row_h, gap = S(4), lw = S(500);
    const int fcol = S(150), flab = S(46), fed = S(64);
    int y = py, i, rx, rw, view_w;
    (void)pw;
    for (i = 0; i < NFIELDS; i++) {
        const int cx = px + (i % 3) * fcol, cy = y + (i / 3) * (rh + gap);
        put(d->fld_label[i], cx, cy + (rh - d->ui_h) / 2, flab, d->ui_h + 2);
        put(d->fld[i], cx + flab, cy, fed, rh);
    }
    y += ((NFIELDS + 2) / 3) * (rh + gap) + S(2);
    y = flow(d, d->flag, NFLAGS, px, y, px + lw);
    y = flow(d, d->input, NINPUTS, px, y, px + lw);
    y += S(4);
    put(d->stack_head, px, y, lw, d->ui_h + 2);
    y += d->ui_h + S(6);
    for (i = 0; i < NSTACK; i++) {
        const int cx = px + (i % 4) * (lw / 4), cy = y + (i / 4) * (rh + gap);
        put(d->stack_label[i], cx, cy + (rh - d->ui_h) / 2, S(22), d->ui_h + 2);
        put(d->stack_edit[i], cx + S(24), cy, S(64), rh);
    }
    y += 2 * (rh + gap) + S(4);
    put(d->cpu_state, px, y, lw, ph - (y - py) > 6 * d->mono_h ? ph - (y - py) : 6 * d->mono_h);

    rx = px + lw + S(16);
    rw = px + pw - rx;
    view_w = (8 + 16 * 3) * d->mono_w + S(18);
    if (view_w > rw) view_w = rw;
    y = py;
    put(d->mem_label, rx, y, rw, d->ui_h + 2);
    y += d->ui_h + S(6);
    put(d->mem_view, rx, y, view_w, (MEM_ROWS + 1) * d->mono_h + S(8));
    y += (MEM_ROWS + 1) * d->mono_h + S(8) + S(12);
    {
        int x = rx;
        put(d->ram_space, x, y, S(150), rh + S(120));
        x += S(158);
        put(d->ram_prev, x, y - 1, S(64), rh + 2);
        x += S(68);
        put(d->ram_next, x, y - 1, S(64), rh + 2);
        y += rh + S(8);
        x = rx;
        put(d->ram_goto_label, x, y + (rh - d->ui_h) / 2, S(40), d->ui_h + 2);
        put(d->ram_goto, x + S(42), y, S(70), rh);
        x += S(124);
        put(d->ram_addr_label, x, y + (rh - d->ui_h) / 2, S(40), d->ui_h + 2);
        put(d->ram_addr, x + S(42), y, S(70), rh);
        x += S(116);
        put(d->ram_val_label, x, y + (rh - d->ui_h) / 2, S(14), d->ui_h + 2);
        put(d->ram_val, x + S(16), y, S(52), rh);
    }
}

static void layout_sheets(debugger *d, int sx, int sy, int sw)
{
    int i, y = sy;
    for (i = 0; i < NSHEETS; i++) {
        const image *im = &d->sheet_pv[i].img;
        int s = 1, w = S(120), h = S(40);
        if (im->w > 0 && im->h > 0) {
            s = sw / im->w;
            if (s < 1) s = 1;
            if (s > 12) s = 12;
            w = im->w * s;
            h = im->h * s;
        }
        put(d->sheet_label[i], sx, y, sw, d->ui_h + 2);
        y += d->ui_h + S(4);
        put(d->sheet[i], sx, y, w + 4, h + 4);
        y += h + 4 + S(14);
    }
}

static void layout_video(debugger *d, int px, int py, int pw, int ph)
{
    const int rh = d->row_h, gap = S(8);
    int rw = pw * 36 / 100, lw, y = py, cap_w, th, rx, sx, sy, sw, sh;
    if (rw < S(380)) rw = S(380);
    if (rw > S(560)) rw = S(560);
    lw = pw - rw - gap;
    {
        /* the picture at twice its size, whole, when the sections can spare
         * the room: its window is a client edge wider */
        const int want = 2 * ((d->dpi + 48) / 96 > 0 ? (d->dpi + 48) / 96 : 1) * CHFSESSION_FB_WIDTH + S(8);
        if (lw < want && pw - want - gap >= S(360)) {
            lw = want;
            rw = pw - lw - gap;
        }
    }
    cap_w = S(70);

    put(d->view_label, px, y + (rh - d->ui_h) / 2, cap_w, d->ui_h + 2);
    put(d->view_mode, px + cap_w, y, S(270), rh + S(120));
    y += rh + S(4);
    put(d->layers_label, px, y + (rh - d->ui_h) / 2, cap_w, d->ui_h + 2);
    y = flow(d, d->layer, NLAYERS, px + cap_w, y, px + lw);
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
    put(d->objects, sx, sy, sw, sh);
    put(d->screen, sx, sy, sw, sh - rh - S(6));
    put(d->screen_status, sx, sy + sh - rh, sw, rh);
    {
        int cw = S(50) + S(160) + S(54) + S(24);
        put(d->regs_list, sx, sy, sw, sh - rh - S(6));
        ListView_SetColumnWidth(d->regs_list, 0, S(50));
        ListView_SetColumnWidth(d->regs_list, 1, S(160));
        ListView_SetColumnWidth(d->regs_list, 2, S(54));
        ListView_SetColumnWidth(d->regs_list, 3, sw - cw > S(120) ? sw - cw : S(120));
        y = sy + sh - rh;
        put(d->reg_addr_label, sx, y + (rh - d->ui_h) / 2, S(74), d->ui_h + 2);
        put(d->reg_addr, sx + S(76), y, S(56), rh);
        put(d->reg_val_label, sx + S(142), y + (rh - d->ui_h) / 2, S(54), d->ui_h + 2);
        put(d->reg_val, sx + S(198), y, S(48), rh);
        put(d->reg_write, sx + S(258), y - 1, S(72), rh + 2);
    }
    put(d->status_text, sx, sy, sw, sh);
    layout_sheets(d, sx, sy, sw - S(8));
}

/* The screen cell's size in the Screen section: 8 x 8 glyph pixels at a whole
 * scale, so the 16 columns fill the width. */
void dbg_screen_cell(const debugger *d, int *cw, int *ch)
{
    RECT c;
    int s = 3;
    GetClientRect(d->screen, &c);
    if (c.right > 0) {
        s = c.right / (CHFDEBUG_UVI_COLS * 8);
        if (s < 1) s = 1;
        if (s > 6) s = 6;
    }
    *cw = *ch = 8 * s;
}

static void layout_io(debugger *d, int px, int py, int pw, int ph)
{
    int mw, mh, lw;
    dbg_matrix_size(d, &mw, &mh);
    lw = pw - mw - S(14);
    if (lw < S(300)) lw = S(300);
    put(d->io_text, px, py, lw, ph);
    put(d->io_matrix, px + lw + S(14), py, mw, mh);
}

static void layout_breaks(debugger *d, int px, int py, int pw, int ph)
{
    const int rh = d->row_h;
    int ry = py + ph - 2 * rh - S(14), x = px, cond_w;
    put(d->bp_list, px, py, pw, ry - py - S(6));
    put(d->bp_type, x, ry, S(120), rh + S(120));
    x += S(130);
    put(d->bp_start_label, x, ry + (rh - d->ui_h) / 2, S(50), d->ui_h + 2);
    put(d->bp_start, x + S(52), ry, S(84), rh);
    x += S(144);
    put(d->bp_end_label, x, ry + (rh - d->ui_h) / 2, S(18), d->ui_h + 2);
    put(d->bp_end, x + S(20), ry, S(84), rh);
    x += S(112);
    put(d->bp_cond_label, x, ry + (rh - d->ui_h) / 2, S(14), d->ui_h + 2);
    cond_w = px + pw - S(76) - (x + S(18));
    put(d->bp_cond, x + S(18), ry, cond_w > S(80) ? cond_w : S(80), rh);
    put(d->bp_add, px + pw - S(68), ry - 1, S(68), rh + 2);
    ry += rh + S(8);
    put(d->bp_remove, px, ry, S(96), rh + 2);
    put(d->bp_enable, px + S(104), ry, S(130), rh + 2);
    put(d->bp_clear, px + S(242), ry, S(96), rh + 2);
}

static void layout_cart(debugger *d, int px, int py, int pw, int ph)
{
    const int lw = pw * 46 / 100, rx = px + lw + S(14), rw = pw - lw - S(14);
    put(d->cart_text, px, py, lw, ph);
    put(d->cart_mailbox_label, rx, py, rw, d->ui_h + 2);
    put(d->cart_mailbox, rx, py + d->ui_h + S(6), rw, ph - d->ui_h - S(6));
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
    RECT client, page;
    int y = S(8), bx = S(8), px, py, pw, ph, tab_top;
    const int rh = d->row_h;

    GetClientRect(d->hwnd, &client);
    put(d->run_btn, bx, y, S(96), rh + 2);               bx += S(102);
    put(d->step_btn, bx, y, S(90), rh + 2);              bx += S(96);
    put(d->over_btn, bx, y, S(90), rh + 2);              bx += S(96);
    put(d->out_btn, bx, y, S(120), rh + 2);              bx += S(126);
    put(d->scan_btn, bx, y, S(96), rh + 2);              bx += S(102);
    put(d->frame_btn, bx, y, S(84), rh + 2);             bx += S(90);
    put(d->load_symbols, bx, y, S(120), rh + 2);         bx += S(126);
    put(d->save, bx, y, S(84), rh + 2);                  bx += S(90);
    put(d->status, bx, y + (rh - d->ui_h) / 2 + 1, client.right - bx - S(8) > S(40) ? client.right - bx - S(8) : S(40),
        d->ui_h + 2);

    /* The tabs are a strip alone: the pages sit on the window's own face,
     * where the labels' background is the same grey. */
    tab_top = y + rh + S(10);
    page.top = tab_top + chf_place_tab_strip(d->tabs, S(8), tab_top, client.right - S(16)) + S(8);
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
    d->hover_valid = 0;
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
        SetTextColor(di->hDC, RGB(0x1e, 0x1e, 0x1e));
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

/* The Registers section's rows come from the cache refresh_regs fills. */
static void regs_dispinfo(debugger *d, NMLVDISPINFOW *di)
{
    static wchar_t buf[160];
    char text[160];
    const int i = di->item.iItem;
    if (i < 0 || i >= UVI_REGS || !(di->item.mask & LVIF_TEXT)) return;
    switch (di->item.iSubItem) {
    case 0: snprintf(text, sizeof text, "$%04X", 0x1800 + i); break;
    case 1: snprintf(text, sizeof text, "%s", d->reg_name[i]); break;
    case 2: snprintf(text, sizeof text, "%02X", d->uvi.reg[i]); break;
    default: snprintf(text, sizeof text, "%s", d->reg_text[i]); break;
    }
    dbg_wide_from(text, buf, 160);
    di->item.pszText = buf;
}

/* Picking a register row puts it in the write fields. */
static void regs_selected(debugger *d, int i)
{
    char t[16];
    if (i < 0 || i >= UVI_REGS) return;
    snprintf(t, sizeof t, "%04X", 0x1800 + i);
    dbg_label_set(d->reg_addr, t);
    snprintf(t, sizeof t, "%02X", d->uvi.reg[i]);
    dbg_label_set(d->reg_val, t);
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
    if ((id >= IDC_LAYER0 && id <= IDC_LAYER_LAST) || (id >= IDC_OVL0 && id <= IDC_OVL_LAST)) {
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
    case IDC_LOAD_SYMBOLS: symbols_menu(d); break;
    case IDC_SYMS_FILE: load_symbols(d, 0); break;
    case IDC_SYMS_CART: load_symbols(d, 1); break;
    case IDC_SAVE: save_menu(d); break;
    case IDC_FOLLOW_PC: dbg_refresh_disasm(d); break;
    case IDC_VIEW_MODE:
        if (code == CBN_SELCHANGE) refresh_video(d, 0);
        break;
    case IDC_RAM_SPACE:
        if (code == CBN_SELCHANGE) { d->mem_base = 0; refresh_mem(d); }
        break;
    case IDC_RAM_PREV: mem_page(d, -1); break;
    case IDC_RAM_NEXT: mem_page(d, 1); break;
    case IDC_REG_WRITE: write_uvi_register(d); break;
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
                snprintf(a, sizeof a, "$%03X", d->bps[sel].start);
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
 * refreshes; while running, the Video tab follows every tick (ten a second,
 * the most the spec asks) and the other pages twice a second. A minimised
 * window refreshes nothing. */
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
    if (d->page == PAGE_VIDEO) {
        const int slow = d->running_ticks >= TICKS_SLOW;
        refresh_video(d, slow);
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
        if (d->nctl > 0 && d->vtabs) layout(d);
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
        if (h->idFrom == IDC_REGS_LIST) {
            if (h->code == (UINT)LVN_GETDISPINFOW) { regs_dispinfo(d, (NMLVDISPINFOW *)lp); return 0; }
            if (h->code == (UINT)LVN_ITEMCHANGED) {
                const NMLISTVIEW *nm = (const NMLISTVIEW *)lp;
                if ((nm->uChanged & LVIF_STATE) && (nm->uNewState & LVIS_SELECTED)) regs_selected(d, nm->iItem);
                return 0;
            }
        }
        break;
    }
    case WM_DRAWITEM:
        if (wp == IDC_RUN) { draw_run_button(d, (const DRAWITEMSTRUCT *)lp); return TRUE; }
        break;
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == d->status || (HWND)lp == d->disasm_hint || (HWND)lp == d->fuji_note ||
            (HWND)lp == d->vstatus || (HWND)lp == d->screen_status) {
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
    case WM_DESTROY: {
        int i;
        KillTimer(hwnd, TIMER_REFRESH);
        DeleteObject(d->ui);
        DeleteObject(d->bold);
        DeleteObject(d->mono);
        DeleteObject(d->accent);
        DeleteObject(d->accent_dark);
        DestroyAcceleratorTable(d->accel);
        free(d->vimg.px);
        free(d->palimg.px);
        for (i = 0; i < NSHEETS; i++) free(d->sheet_pv[i].img.px);
        free(d);
        g_debugger = NULL;
        return 0;
    }
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
    int i;

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
    d->vsec = SEC_OBJECTS;
    d->screen_hover = -1;
    d->dpi = chf_window_dpi(parent);
    for (i = 0; i < 8; i++) d->pal[i] = 0x00808080;
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
    {   /* CHF_DEBUGGER_SECTION: the Video tab's section (0-4), for scripted checks */
        const char *sec = getenv("CHF_DEBUGGER_SECTION");
        if (sec && *sec >= '0' && *sec < '0' + SEC_COUNT) d->vsec = *sec - '0';
        SendMessageW(d->vtabs, TCM_SETCURSEL, (WPARAM)d->vsec, 0);
    }
    SendMessageW(d->tabs, TCM_SETCURSEL, (WPARAM)d->page, 0);
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
