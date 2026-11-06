/*
 * The Preferences window: a tab per page -- Machine, Display & Audio, Input,
 * FujiNet, ROMs -- over the same settings keys as the other frontends'
 * Preferences, so a machine configured in one comes up the same in another.
 * A Help button (and F1) opens the Help topic of the page showing.
 *
 * The picture, the volume, the right stick, the gamepad assignments and the
 * controls apply at once. The machine, the BIOS and the host options (audio,
 * gamepads, FujiNet) wait for the window to close, which hands the main
 * window one "dirty" flag so it power-cycles once for the lot.
 *
 * Laid out in 96-dpi units through chf_scale() and rebuilt on WM_DPICHANGED;
 * each page is a set of children of the window shown or hidden under a tab
 * strip, so no page can paint over another.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "prefs.h"

#include <commctrl.h>
#include <shellapi.h>

#include "help/help_window.h"
#include "import.h"
#include "key_forward.h"
#include "resource.h"

#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif

#define PREFS_CLASS "FujiNetGoChfPrefs"
#define PREFS_STYLE (WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN)
#define MAX_ITEMS 160
#define MAX_ROMS 16

/* Layout, in 96-dpi units. */
#define CW 560              /* client width */
#define M 14                /* margin */
#define PAGE_H 412          /* the tallest page: Input */
#define ROW 22
#define HINT_IDLE "A key does one thing: giving it to a control takes it from the old one."

enum { PG_MACHINE, PG_DISPLAY, PG_INPUT, PG_FUJINET, PG_ROMS, PG_COUNT };

enum {
    IDC_TABS = 100, IDC_MACHINE, IDC_BIOS,
    IDC_ASPECT, IDC_SMOOTH, IDC_AUDIO, IDC_VOLUME, IDC_GAMEPAD, IDC_RSTICK, IDC_PAD_LIST,
    IDC_PAD_PORT, IDC_CONTROLS, IDC_CLEAR, IDC_RESET_CONTROLS,
    IDC_FUJINET, IDC_EXTERNAL, IDC_EXT_ADDR, IDC_WEBUI,
    IDC_IMPORT, IDC_IMPORT_FOLDER, IDC_PREFS_HELP, IDC_PREFS_CLOSE
};

#define IDT_PADS 1
#define IDT_CAPTURE 2

/* The Help contexts, by page. */
static const char *const k_contexts[PG_COUNT] = {
    "prefs/Machine", "prefs/Display", "prefs/Input", "prefs/FujiNet", "prefs/ROMs"
};

typedef struct {
    HWND h;
    int page;               /* PG_*, or -1: always shown */
    int x, y, w, ht;        /* page-relative, 96 dpi */
    int bold;               /* a heading */
} item;

static HWND g_win, g_tabs, g_close, g_help;
static HWND g_machine_note, g_pad_list, g_pad_port, g_ext_addr;
static HWND g_machine_combo, g_bios_combo, g_controls, g_hint, g_rom_path;
static HWND g_rom_state[MAX_ROMS];
static item g_items[MAX_ITEMS];
static int g_nitems, g_nroms;
static HFONT g_font, g_bold;
static int g_dpi = 96;
static chfsession *g_session;
static chf_prefs_hooks g_hooks;
static int g_dirty;
static int g_page;
static unsigned g_pad_seen;
static int g_armed = -1;        /* the control waiting for a key or a pad button */
static int g_filling;

static int S(int v) { return chf_scale(v, g_dpi); }

/* ---- building -------------------------------------------------------------- */

static HWND add(const wchar_t *cls, const char *utf8, DWORD style, int page, int id,
                int x, int y, int w, int ht)
{
    wchar_t *text = chf_wide_dup(utf8 ? utf8 : "");
    HWND h;
    if (g_nitems >= MAX_ITEMS) { free(text); return NULL; }
    h = CreateWindowExW(0, cls, text ? text : L"", WS_CHILD | style, 0, 0, 10, 10, g_win,
                        (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
    free(text);
    g_items[g_nitems].h = h;
    g_items[g_nitems].page = page;
    g_items[g_nitems].x = x;
    g_items[g_nitems].y = y;
    g_items[g_nitems].w = w;
    g_items[g_nitems].ht = ht;
    g_items[g_nitems].bold = 0;
    g_nitems++;
    return h;
}

static HWND label(int page, const char *text, int x, int y, int w, int ht)
{
    return add(L"STATIC", text, SS_LEFT | SS_NOPREFIX, page, 0, x, y, w, ht);
}

static HWND heading(int page, const char *text, int y)
{
    HWND h = label(page, text, M, y, CW - 2 * M, 18);
    if (h) g_items[g_nitems - 1].bold = 1;
    return h;
}

static HWND checkbox(int page, int id, const char *text, int x, int y, int w, int on)
{
    HWND h = add(L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, page, id, x, y, w, 22);
    if (h) SendMessageW(h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    return h;
}

static HWND button(int page, int id, const char *text, int x, int y, int w)
{
    return add(L"BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, page, id, x, y, w, 28);
}

static HWND combo(int page, int id, int x, int y, int w)
{
    return add(L"COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, page, id, x, y, w, 220);
}

static void combo_add(HWND c, const char *text)
{
    wchar_t *w = chf_wide_dup(text);
    if (w) { SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)w); free(w); }
}

static void set_text(HWND h, const char *utf8)
{
    wchar_t cur[1024], *w = chf_wide_dup(utf8);
    if (!w) return;
    GetWindowTextW(h, cur, 1024);
    if (wcscmp(cur, w) != 0) SetWindowTextW(h, w);
    free(w);
}

static void to_backslashes(char *p)
{
    for (; *p; p++)
        if (*p == '/') *p = '\\';
}

/* ---- the machine page ---------------------------------------------------------- */

static int machine_selected(void)
{
    const int sel = (int)SendMessageA(g_machine_combo, CB_GETCURSEL, 0, 0);
    return sel >= 0 && sel < CHF_MACHINE_COUNT ? sel : CHF_MACHINE_CHANNELF;
}

/* What the chosen machine is, and whether a real BIOS would run on it. */
static void refresh_machine(void)
{
    const int m = machine_selected();
    char text[900];
    int n;
    n = snprintf(text, sizeof text, "%s", chf_machine_is_pal(m)
                 ? "PAL: the F3850 at 2 MHz, 312 lines at 50 Hz."
                 : "NTSC: the F3850 at 1.79 MHz, 264 lines at 60.5 Hz.");
    if (m == CHF_MACHINE_VIDEOPLAY2)
        n += snprintf(text + n, sizeof text - (size_t)n, " Its sound is half the level of the others, as in MAME.");
    n += snprintf(text + n, sizeof text - (size_t)n, "\n\n");
    if (chfsession_have_bios(g_session, m))
        snprintf(text + n, sizeof text - (size_t)n,
                 "A real BIOS runs on this machine (Videocarts need one).");
    else
        snprintf(text + n, sizeof text - (size_t)n,
                 "No real BIOS runs on this machine: CONFIG and the FujiNet apps use the app's own stand-in, "
                 "and Videocarts need the BIOS (the ROMs tab imports it).");
    set_text(g_machine_note, text);
}

static void build_machine(void)
{
    int y = 6, m, b;
    heading(PG_MACHINE, "Machine", y); y += 22;
    label(PG_MACHINE, "Applied by a power cycle when this window closes.", M, y, CW - 2 * M, 18); y += 28;
    label(PG_MACHINE, "Machine:", M, y + 3, 70, 18);
    g_machine_combo = combo(PG_MACHINE, IDC_MACHINE, M + 76, y, 400);
    for (m = 0; m < CHF_MACHINE_COUNT; m++)
        combo_add(g_machine_combo, chf_machine_name(m));
    {
        const int cur = chfsession_get_int(g_session, "machine", CHF_MACHINE_CHANNELF);
        SendMessageA(g_machine_combo, CB_SETCURSEL, (WPARAM)(cur >= 0 && cur < CHF_MACHINE_COUNT ? cur : 0), 0);
    }
    y += 34;
    label(PG_MACHINE, "BIOS:", M, y + 3, 70, 18);
    g_bios_combo = combo(PG_MACHINE, IDC_BIOS, M + 76, y, 400);
    for (b = 0; b < CHF_BIOS_CHOICE_COUNT; b++)
        combo_add(g_bios_combo, chf_bios_choice_name(b));
    {
        const int cur = chfsession_get_int(g_session, "bios", CHF_BIOS_AUTO);
        SendMessageA(g_bios_combo, CB_SETCURSEL, (WPARAM)(cur >= 0 && cur < CHF_BIOS_CHOICE_COUNT ? cur : 0), 0);
    }
    y += 44;
    g_machine_note = label(PG_MACHINE, "", M, y, CW - 2 * M, 96);
    refresh_machine();
}

/* ---- display and audio ----------------------------------------------------------- */

static void build_display(void)
{
    HWND c, t;
    int y = 6;
    heading(PG_DISPLAY, "Display and audio", y); y += 28;
    label(PG_DISPLAY, "Aspect:", M, y + 3, 70, 18);
    c = combo(PG_DISPLAY, IDC_ASPECT, M + 76, y, 240);
    combo_add(c, "Television (4:3)");
    combo_add(c, "Square pixels");
    SendMessageA(c, CB_SETCURSEL, chfsession_get_int(g_session, "aspect", 0) == 0 ? 0 : 1, 0);
    y += 38;
    checkbox(PG_DISPLAY, IDC_SMOOTH, "Smooth scaling (off: sharp pixels)", M, y, 320,
             chfsession_get_int(g_session, "smooth", 0));
    y += 40;
    checkbox(PG_DISPLAY, IDC_AUDIO, "Enable audio (applied when this window closes)", M, y, CW - 2 * M,
             chfsession_get_int(g_session, "enable_audio", 1));
    y += 34;
    label(PG_DISPLAY, "Volume:", M, y + 4, 70, 18);
    t = add(L"msctls_trackbar32", NULL, TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, PG_DISPLAY, IDC_VOLUME,
            M + 76, y, 320, 30);
    if (t) {
        SendMessageA(t, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageA(t, TBM_SETTICFREQ, 10, 0);
        SendMessageA(t, TBM_SETPOS, TRUE, (LPARAM)chfsession_get_int(g_session, "volume", 100));
    }
}

/* ---- input ------------------------------------------------------------------------ */

static void refresh_pad_list(void)
{
    int i, n, sel;
    if (!g_pad_list) return;
    sel = (int)SendMessageA(g_pad_list, LB_GETCURSEL, 0, 0);
    SendMessageA(g_pad_list, LB_RESETCONTENT, 0, 0);
    n = chfsession_gamepad_count(g_session);
    if (n == 0)
        SendMessageA(g_pad_list, LB_ADDSTRING, 0, (LPARAM)"(no gamepads connected)");
    for (i = 0; i < n; i++) {
        char name[128], line[220];
        wchar_t w[220];
        const int eff = chfsession_gamepad_effective_port(g_session, i);
        chfsession_gamepad_name(g_session, i, name, sizeof name);
        if (eff >= 0)
            snprintf(line, sizeof line, "%s  [player %d%s]", name, eff + 1,
                     chfsession_gamepad_assignment(g_session, i) < 0 ? ", automatic" : "");
        else
            snprintf(line, sizeof line, "%s  [unused]", name);
        MultiByteToWideChar(CP_UTF8, 0, line, -1, w, 220);
        SendMessageW(g_pad_list, LB_ADDSTRING, 0, (LPARAM)w);
    }
    if (sel >= 0 && sel < n) {
        SendMessageA(g_pad_list, LB_SETCURSEL, (WPARAM)sel, 0);
        SendMessageA(g_pad_port, CB_SETCURSEL, (WPARAM)(chfsession_gamepad_assignment(g_session, sel) + 1), 0);
    }
    g_pad_seen = chfsession_gamepad_generation(g_session);
}

/* Every control with its key and gamepad button. */
static void fill_controls(void)
{
    int t;
    LVITEMW it;
    if (!g_controls) return;
    g_filling = 1;
    SendMessageW(g_controls, WM_SETREDRAW, FALSE, 0);
    if ((int)SendMessageW(g_controls, LVM_GETITEMCOUNT, 0, 0) == 0) {
        memset(&it, 0, sizeof it);
        it.mask = LVIF_TEXT;
        for (t = 0; t < CHF_TARGET_COUNT; t++) {
            it.iItem = t;
            it.pszText = L"";
            SendMessageW(g_controls, LVM_INSERTITEMW, 0, (LPARAM)&it);
        }
    }
    for (t = 0; t < CHF_TARGET_COUNT; t++) {
        const chf_binding b = chfsession_binding_get(g_session, t);
        char key[48] = "";
        const char *texts[3];
        int c;
        if (b.keysym) chfsession_keysym_name(b.keysym, key, sizeof key);
        texts[0] = chf_target_name(t);
        texts[1] = key[0] ? key : "\xe2\x80\x94";
        texts[2] = b.button != CHF_PAD_BTN_NONE ? chf_pad_button_name(b.button) : "\xe2\x80\x94";
        for (c = 0; c < 3; c++) {
            wchar_t w[96];
            MultiByteToWideChar(CP_UTF8, 0, texts[c], -1, w, 96);
            memset(&it, 0, sizeof it);
            it.iSubItem = c;
            it.pszText = w;
            SendMessageW(g_controls, LVM_SETITEMTEXTW, (WPARAM)t, (LPARAM)&it);
        }
    }
    SendMessageW(g_controls, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_controls, NULL, TRUE);
    g_filling = 0;
}

static void set_hint(const char *text)
{
    set_text(g_hint, text);
}

static void disarm(const char *hint)
{
    g_armed = -1;
    KillTimer(g_win, IDT_CAPTURE);
    chfsession_gamepad_capture_cancel(g_session);
    set_hint(hint);
}

static void arm(int target)
{
    char msg[200];
    if (target < 0 || target >= CHF_TARGET_COUNT) return;
    g_armed = target;
    chfsession_gamepad_capture_begin(g_session);
    SetTimer(g_win, IDT_CAPTURE, 50, NULL);
    snprintf(msg, sizeof msg, "Press a key or a gamepad button for %s (Escape cancels).", chf_target_name(target));
    set_hint(msg);
    SetFocus(g_controls);
}

static void bound(const char *what, const char *stolen)
{
    char msg[300];
    if (stolen[0]) snprintf(msg, sizeof msg, "Bound %s (it was %s's).", what, stolen);
    else snprintf(msg, sizeof msg, "Bound %s.", what);
    disarm(msg);
    fill_controls();
}

static void build_input(void)
{
    LVCOLUMNW col;
    int y = 6, i;
    static const wchar_t *const heads[3] = { L"Control", L"Key", L"Gamepad" };
    static const int widths[3] = { 220, 150, 130 };
    heading(PG_INPUT, "Input", y); y += 24;
    checkbox(PG_INPUT, IDC_GAMEPAD, "Enable gamepads (applied when this window closes)", M, y, 330,
             chfsession_get_int(g_session, "enable_gamepad", 1));
    checkbox(PG_INPUT, IDC_RSTICK, "Right stick works the knob", M + 340, y, 190,
             chfsession_get_int(g_session, "right_stick", 1));
    y += 30;
    label(PG_INPUT, "Gamepads (select one, then choose its player):", M, y, CW - 2 * M, 18); y += 20;
    g_pad_list = add(L"LISTBOX", NULL, WS_VSCROLL | LBS_NOTIFY | WS_TABSTOP | WS_BORDER, PG_INPUT,
                     IDC_PAD_LIST, M, y, 400, 64);
    g_pad_port = combo(PG_INPUT, IDC_PAD_PORT, M + 410, y, 122);
    combo_add(g_pad_port, "Automatic");
    combo_add(g_pad_port, "Player 1");
    combo_add(g_pad_port, "Player 2");
    SendMessageA(g_pad_port, CB_SETCURSEL, 0, 0);
    y += 72;
    label(PG_INPUT, "Controls (choose one, then press its key or button):", M, y + 5, 340, 18);
    button(PG_INPUT, IDC_CLEAR, "Clear", CW - M - 176, y - 2, 60);
    button(PG_INPUT, IDC_RESET_CONTROLS, "Reset Controls", CW - M - 110, y - 2, 110);
    y += 30;
    g_controls = add(WC_LISTVIEWW, NULL, LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP | WS_BORDER |
                                          WS_VSCROLL, PG_INPUT, IDC_CONTROLS, M, y, CW - 2 * M, 176);
    if (g_controls) {
        SendMessageW(g_controls, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        memset(&col, 0, sizeof col);
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        for (i = 0; i < 3; i++) {
            col.pszText = (wchar_t *)heads[i];
            col.cx = widths[i];
            SendMessageW(g_controls, LVM_INSERTCOLUMNW, (WPARAM)i, (LPARAM)&col);
        }
    }
    y += 182;
    g_hint = add(L"STATIC", HINT_IDLE, SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, PG_INPUT, 0, M, y, CW - 2 * M, 18);
}

/* ---- FujiNet ---------------------------------------------------------------------- */

static void build_fujinet(void)
{
    int y = 6;
    char line[300];
    HWND e;
    heading(PG_FUJINET, "FujiNet", y); y += 26;
    checkbox(PG_FUJINET, IDC_FUJINET, "Enable FujiNet (applied when this window closes)", M, y, CW - 2 * M,
             chfsession_get_int(g_session, "enable_fujinet", 1));
    y += 28;
    checkbox(PG_FUJINET, IDC_EXTERNAL, "Use external FujiNet (applied when this window closes)", M, y, CW - 2 * M,
             chfsession_get_int(g_session, "fujinet_external", 0));
    y += 32;
    label(PG_FUJINET, "Address:", M, y + 3, 70, 18);
    e = add(L"EDIT", chfsession_get_str(g_session, "fujinet_external_addr", ""),
            ES_AUTOHSCROLL | WS_TABSTOP | WS_BORDER, PG_FUJINET, IDC_EXT_ADDR, M + 76, y, 240, 24);
    g_ext_addr = e;
    if (e) {
        SendMessageW(e, EM_SETCUEBANNER, FALSE, (LPARAM)L"127.0.0.1:11526");
        EnableWindow(e, chfsession_get_int(g_session, "fujinet_external", 0) != 0);
    }
    label(PG_FUJINET, "host:port, or just a port", M + 326, y + 3, 200, 18);
    y += 40;
    if (chfsession_fujinet_external(g_session))
        snprintf(line, sizeof line, "Web UI: FujiNet is external");
    else
        snprintf(line, sizeof line, "Web UI: %s", chfsession_fujinet_webui_url(g_session));
    label(PG_FUJINET, line, M, y + 5, 360, 18);
    button(PG_FUJINET, IDC_WEBUI, "Open", CW - M - 100, y, 100);
}

/* ---- the ROMs page -------------------------------------------------------------- */

static void refresh_roms(void)
{
    int i;
    for (i = 0; i < g_nroms; i++)
        if (g_rom_state[i])
            set_text(g_rom_state[i], chfsession_rom_item_available(g_session, i) ? "Imported" : "Not imported");
}

static void build_roms(void)
{
    int y = 6, i;
    const char *last = NULL;
    char path[1100];

    label(PG_ROMS, "The Channel F BIOS is not part of the app. CONFIG and the FujiNet apps run on the app's own "
                   "stand-in; Videocarts need the real BIOS: two 1 KB ROMs. Import your own dumps from files, "
                   "a zip (MAME's channelf.zip works) or a folder; they are recognised by size and checksum, "
                   "whatever they are named. Kept in:",
          M, y, CW - 2 * M, 70);
    y += 74;
    snprintf(path, sizeof path, "%s", chfsession_roms_path(g_session));
    to_backslashes(path);
    g_rom_path = add(L"STATIC", path, SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS, PG_ROMS, 0, M, y, CW - 2 * M, 18);
    y += 28;
    button(PG_ROMS, IDC_IMPORT, "Import...", M, y, 130);
    button(PG_ROMS, IDC_IMPORT_FOLDER, "Import a Folder...", M + 138, y, 170);
    y += 42;
    g_nroms = chfsession_rom_item_count();
    if (g_nroms > MAX_ROMS) g_nroms = MAX_ROMS;
    for (i = 0; i < g_nroms; i++) {
        const chf_rom_item *it = chfsession_rom_item(i);
        if (!last || strcmp(last, it->group) != 0) {
            if (last) y += 6;
            heading(PG_ROMS, it->group, y);
            y += 20;
            last = it->group;
        }
        label(PG_ROMS, it->name, M + 14, y, CW - 2 * M - 14 - 110, 18);
        g_rom_state[i] = add(L"STATIC", "", SS_RIGHT | SS_NOPREFIX, PG_ROMS, 0, CW - M - 110, y, 110, 18);
        y += 20;
    }
}

/* ---- layout ------------------------------------------------------------------------ */

static void show_page(int page)
{
    int i;
    g_page = page;
    for (i = 0; i < g_nitems; i++)
        if (g_items[i].page >= 0)
            ShowWindow(g_items[i].h, g_items[i].page == page ? SW_SHOW : SW_HIDE);
    /* Help, F1 and Ctrl+Shift+H open the page's own */
    chf_help_set_context(g_win, k_contexts[page], 1);
}

/* Places everything at the current DPI and sizes the window to its
 * contents. */
static void layout(void)
{
    RECT want;
    int i, top;
    top = chf_place_tab_strip(g_tabs, 0, 0, S(CW)) + S(8);
    for (i = 0; i < g_nitems; i++) {
        const item *it = &g_items[i];
        if (!it->h) continue;
        MoveWindow(it->h, S(it->x), top + S(it->y), S(it->w), S(it->ht), TRUE);
    }
    if (g_controls) {
        static const int widths[3] = { 220, 150, 130 };
        int c;
        for (c = 0; c < 3; c++)
            SendMessageW(g_controls, LVM_SETCOLUMNWIDTH, (WPARAM)c, S(widths[c]));
    }
    MoveWindow(g_help, S(M), top + S(PAGE_H) + S(6), S(100), S(28), TRUE);
    MoveWindow(g_close, S(CW) - S(M) - S(100), top + S(PAGE_H) + S(6), S(100), S(28), TRUE);
    SetRect(&want, 0, 0, S(CW), top + S(PAGE_H) + S(6) + S(28) + S(M));
    {
        const SIZE sz = chf_outer_size(g_win, want.right, want.bottom, FALSE);
        SetWindowPos(g_win, NULL, 0, 0, sz.cx, sz.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

/* The window's fonts at the current DPI: plain on every child, bold on the
 * headings. */
static void make_fonts(void)
{
    int i;
    if (g_font) DeleteObject(g_font);
    if (g_bold) DeleteObject(g_bold);
    g_font = chf_make_font(g_dpi, 0, FW_NORMAL, NULL);
    g_bold = chf_make_font(g_dpi, 0, FW_BOLD, NULL);
    chf_set_font_all(g_win, g_font);
    for (i = 0; i < g_nitems; i++)
        if (g_items[i].bold) SendMessageW(g_items[i].h, WM_SETFONT, (WPARAM)g_bold, TRUE);
}

/* ---- actions -------------------------------------------------------------------------- */

static void setting_from_checkbox(int id, const char *key, int def)
{
    const int on = SendDlgItemMessageA(g_win, id, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (chfsession_get_int(g_session, key, def) != on) {
        chfsession_set_int(g_session, key, on);
        g_dirty = 1;
    }
}

static void open_web_ui(void)
{
    if (chfsession_fujinet_external(g_session)) {
        chf_message(g_win, "FujiNet is external: use the web UI of the instance this app dials.", "FujiNet",
                   MB_ICONINFORMATION | MB_OK);
        return;
    }
    if (!chfsession_fujinet_running(g_session)) {
        chf_message(g_win, "FujiNet is not running.", "FujiNet", MB_ICONINFORMATION | MB_OK);
        return;
    }
    ShellExecuteA(g_win, "open", chfsession_fujinet_webui_url(g_session), NULL, NULL, SW_SHOWNORMAL);
}

static void do_import(int folder)
{
    const int n = chf_import_bios(g_win, g_session, folder);
    if (n < 0) return;
    refresh_roms();
    refresh_machine();
    if (n > 0) {
        /* a console that can run its BIOS now boots it at the power cycle
         * this window ends with */
        g_dirty = 1;
        if (g_hooks.roms_changed) g_hooks.roms_changed();
    }
}

/* The external address is saved, and only if it is one; an invalid one with
 * the option on keeps the window open and says so when interactive. */
static int commit_address(int interactive)
{
    char addr[96], host[64];
    const char *old;
    int port;
    char *first = addr, *last;

    GetWindowTextA(g_ext_addr, addr, sizeof addr);
    while (*first == ' ' || *first == '\t') first++;
    memmove(addr, first, strlen(first) + 1);
    last = addr + strlen(addr);
    while (last > addr && (last[-1] == ' ' || last[-1] == '\t')) *--last = '\0';

    if (SendDlgItemMessageA(g_win, IDC_EXTERNAL, BM_GETCHECK, 0, 0) == BST_CHECKED &&
        chfsession_parse_hostport(addr, host, sizeof host, &port) != 0) {
        if (!interactive) return 1;
        chf_message(g_win, "That is not an address:port.\n\nExample: 127.0.0.1:11526", "External FujiNet",
                   MB_ICONWARNING | MB_OK);
        TabCtrl_SetCurSel(g_tabs, PG_FUJINET);
        show_page(PG_FUJINET);
        SetFocus(g_ext_addr);
        return 0;
    }
    old = chfsession_get_str(g_session, "fujinet_external_addr", "");
    if (chfsession_parse_hostport(addr, host, sizeof host, &port) == 0 && strcmp(old, addr) != 0) {
        chfsession_set_str(g_session, "fujinet_external_addr", addr);
        g_dirty = 1;
    }
    return 1;
}

static void on_command(int id, int code)
{
    switch (id) {
    case IDC_MACHINE:
        if (code == CBN_SELCHANGE) {
            const int m = machine_selected();
            if (chfsession_get_int(g_session, "machine", CHF_MACHINE_CHANNELF) != m) {
                chfsession_set_int(g_session, "machine", m);
                g_dirty = 1;
            }
            refresh_machine();
        }
        break;
    case IDC_BIOS:
        if (code == CBN_SELCHANGE) {
            const int b = (int)SendMessageA(g_bios_combo, CB_GETCURSEL, 0, 0);
            if (b >= 0 && chfsession_get_int(g_session, "bios", CHF_BIOS_AUTO) != b) {
                chfsession_set_int(g_session, "bios", b);
                g_dirty = 1;
            }
            refresh_machine();
        }
        break;
    case IDC_ASPECT:
        if (code == CBN_SELCHANGE) {
            chfsession_set_int(g_session, "aspect",
                              SendDlgItemMessageA(g_win, IDC_ASPECT, CB_GETCURSEL, 0, 0) == 0 ? 0 : 1);
            if (g_hooks.display_changed) g_hooks.display_changed();
        }
        break;
    case IDC_SMOOTH:
        chfsession_set_int(g_session, "smooth",
                          SendDlgItemMessageA(g_win, IDC_SMOOTH, BM_GETCHECK, 0, 0) == BST_CHECKED);
        if (g_hooks.display_changed) g_hooks.display_changed();
        break;
    case IDC_AUDIO: setting_from_checkbox(id, "enable_audio", 1); break;
    case IDC_GAMEPAD: setting_from_checkbox(id, "enable_gamepad", 1); break;
    case IDC_RSTICK: {
        const int on = SendDlgItemMessageA(g_win, IDC_RSTICK, BM_GETCHECK, 0, 0) == BST_CHECKED;
        chfsession_set_int(g_session, "right_stick", on);
        chfsession_set_right_stick(g_session, on);
        break;
    }
    case IDC_PAD_LIST:
        if (code == LBN_SELCHANGE) {
            const int sel = (int)SendMessageA(g_pad_list, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < chfsession_gamepad_count(g_session))
                SendMessageA(g_pad_port, CB_SETCURSEL, (WPARAM)(chfsession_gamepad_assignment(g_session, sel) + 1), 0);
        }
        break;
    case IDC_PAD_PORT:
        if (code == CBN_SELCHANGE) {
            const int sel = (int)SendMessageA(g_pad_list, LB_GETCURSEL, 0, 0);
            const int choice = (int)SendMessageA(g_pad_port, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < chfsession_gamepad_count(g_session))
                chfsession_gamepad_assign(g_session, sel, choice - 1);
            refresh_pad_list();
        }
        break;
    case IDC_CLEAR: {
        const int t = (int)SendMessageW(g_controls, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
        if (t >= 0 && t < CHF_TARGET_COUNT) {
            char msg[200];
            chfsession_binding_set_key(g_session, t, 0, NULL, 0);
            chfsession_binding_set_button(g_session, t, CHF_PAD_BTN_NONE, NULL, 0);
            snprintf(msg, sizeof msg, "%s has no key or button now.", chf_target_name(t));
            disarm(msg);
            fill_controls();
        }
        break;
    }
    case IDC_RESET_CONTROLS:
        chfsession_bindings_reset(g_session);
        disarm("Every control is back to its default.");
        fill_controls();
        break;
    case IDC_FUJINET: setting_from_checkbox(id, "enable_fujinet", 1); break;
    case IDC_EXTERNAL:
        setting_from_checkbox(id, "fujinet_external", 0);
        EnableWindow(g_ext_addr, SendDlgItemMessageA(g_win, IDC_EXTERNAL, BM_GETCHECK, 0, 0) == BST_CHECKED);
        break;
    case IDC_WEBUI: open_web_ui(); break;
    case IDC_IMPORT: do_import(0); break;
    case IDC_IMPORT_FOLDER: do_import(1); break;
    case IDC_PREFS_HELP: chf_help_for(g_win); break;
    default: break;
    }
}

/* ---- the window ----------------------------------------------------------------------- */

static LRESULT CALLBACK prefs_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_PREFS_CLOSE || LOWORD(wp) == IDCANCEL) {
            PostMessageA(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        if (LOWORD(wp) != IDOK) on_command(LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_NOTIFY: {
        const NMHDR *h = (const NMHDR *)lp;
        if (h->hwndFrom == g_tabs && h->code == (UINT)TCN_SELCHANGE) {
            show_page(TabCtrl_GetCurSel(g_tabs));
            return 0;
        }
        if (h->hwndFrom == g_controls && !g_filling) {
            if (h->code == (UINT)NM_CLICK || h->code == (UINT)NM_DBLCLK) {
                const NMITEMACTIVATE *ia = (const NMITEMACTIVATE *)lp;
                arm(ia->iItem);
                return 0;
            }
            if (h->code == (UINT)NM_RETURN ||
                (h->code == (UINT)LVN_KEYDOWN && ((const NMLVKEYDOWN *)lp)->wVKey == VK_RETURN)) {
                arm((int)SendMessageW(g_controls, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED));
                return 0;
            }
        }
        break;
    }
    case WM_HSCROLL:
        if ((HWND)lp == GetDlgItem(hwnd, IDC_VOLUME))
            chfsession_set_volume(g_session, (int)SendMessageA((HWND)lp, TBM_GETPOS, 0, 0));
        return 0;
    case WM_TIMER:
        if (wp == IDT_PADS && chfsession_gamepad_generation(g_session) != g_pad_seen)
            refresh_pad_list();
        else if (wp == IDT_CAPTURE) {
            int button;
            if (g_armed < 0) {
                KillTimer(hwnd, IDT_CAPTURE);
            } else if (chfsession_gamepad_capture_poll(g_session, &button)) {
                char stolen[128];
                const int target = g_armed;
                stolen[0] = '\0';
                chfsession_binding_set_button(g_session, target, button, stolen, sizeof stolen);
                bound(chf_pad_button_name(button), stolen);
            }
        }
        return 0;
    case WM_ACTIVATE:
        /* the keyboard went elsewhere: nothing is waiting for a key */
        if (LOWORD(wp) == WA_INACTIVE && g_armed >= 0)
            disarm(HINT_IDLE);
        return 0;
    case WM_DPICHANGED:
        g_dpi = HIWORD(wp);
        make_fonts();
        layout();
        return 0;
    case WM_CLOSE:
        if (!commit_address(1)) return 0;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY: {
        const int dirty = g_dirty;
        KillTimer(hwnd, IDT_PADS);
        KillTimer(hwnd, IDT_CAPTURE);
        if (g_armed >= 0) chfsession_gamepad_capture_cancel(g_session);
        g_armed = -1;
        g_win = NULL;
        g_tabs = g_close = g_help = g_pad_list = g_pad_port = g_ext_addr = g_machine_combo = g_machine_note = NULL;
        g_bios_combo = g_controls = g_hint = g_rom_path = NULL;
        memset(g_rom_state, 0, sizeof g_rom_state);
        g_nitems = g_nroms = 0;
        g_dirty = 0;
        if (g_font) DeleteObject(g_font);
        if (g_bold) DeleteObject(g_bold);
        g_font = g_bold = NULL;
        if (g_hooks.closed) g_hooks.closed(dirty);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void chf_prefs_show(HWND parent, chfsession *session, const chf_prefs_hooks *hooks)
{
    static int registered;
    static const char *const names[PG_COUNT] = { "Machine", "Display && Audio", "Input", "FujiNet", "ROMs" };
    HINSTANCE inst = GetModuleHandleA(NULL);
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES };
    int i;

    if (g_win) {
        SetForegroundWindow(g_win);
        return;
    }
    g_session = session;
    g_hooks = *hooks;
    InitCommonControlsEx(&icc);
    if (!registered) {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = prefs_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"" PREFS_CLASS;
        wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(IDI_APPICON));
        registered = RegisterClassExW(&wc) != 0;
    }
    g_dpi = chf_window_dpi(parent);
    g_nitems = g_nroms = 0;
    g_dirty = 0;
    g_armed = -1;
    g_win = CreateWindowExW(WS_EX_CONTROLPARENT, L"" PREFS_CLASS, L"Preferences", PREFS_STYLE,
                            CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, parent, NULL, inst, NULL);
    if (!g_win) return;

    g_tabs = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS,
                             0, 0, 10, 10, g_win, (HMENU)(INT_PTR)IDC_TABS, inst, NULL);
    for (i = 0; i < PG_COUNT; i++) {
        TCITEMW ti;
        wchar_t *w = chf_wide_dup(names[i]);
        memset(&ti, 0, sizeof ti);
        ti.mask = TCIF_TEXT;
        ti.pszText = w;
        SendMessageW(g_tabs, TCM_INSERTITEMW, (WPARAM)i, (LPARAM)&ti);
        free(w);
    }
    build_machine();
    build_display();
    build_input();
    build_fujinet();
    build_roms();
    g_help = CreateWindowExW(0, L"BUTTON", L"Help", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                             0, 0, 10, 10, g_win, (HMENU)(INT_PTR)IDC_PREFS_HELP, inst, NULL);
    g_close = CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                              0, 0, 10, 10, g_win, (HMENU)(INT_PTR)IDC_PREFS_CLOSE, inst, NULL);
    /* the tab strip first in creation order, last in z-order: with
     * WS_CLIPSIBLINGS it never paints over a page */
    SetWindowPos(g_tabs, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    make_fonts();
    refresh_pad_list();
    fill_controls();
    refresh_roms();
    TabCtrl_SetCurSel(g_tabs, PG_MACHINE);
    show_page(PG_MACHINE);
    layout();
    SetTimer(g_win, IDT_PADS, 500, NULL);
    ShowWindow(g_win, SW_SHOW);
}

void chf_prefs_abandon(void)
{
    if (!g_win) return;
    g_hooks.closed = NULL;
    commit_address(0);
    DestroyWindow(g_win);
}

void chf_prefs_gamepads_changed(void)
{
    if (g_win) refresh_pad_list();
}

void chf_prefs_roms_changed(void)
{
    if (!g_win) return;
    refresh_roms();
    refresh_machine();
}

/* Keys while a control waits for one: the next key is the binding (Escape
 * cancels), not a dialog key. */
static int capture_key(MSG *msg)
{
    const int down = msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN;
    if (g_armed < 0) return 0;
    switch (msg->message) {
    case WM_CHAR: case WM_SYSCHAR: case WM_KEYUP: case WM_SYSKEYUP:
        return 1;
    default:
        break;
    }
    if (!down) return 0;
    if (msg->lParam & (1 << 30)) return 1;
    if (msg->wParam == VK_ESCAPE) {
        disarm(HINT_IDLE);
        return 1;
    }
    {
        const uint32_t ks = chf_keysym_from_msg(msg->wParam, msg->lParam);
        char stolen[128], name[32];
        if (!ks) return 1;                  /* nothing to call it: keep waiting */
        stolen[0] = '\0';
        chfsession_binding_set_key(g_session, g_armed, ks, stolen, sizeof stolen);
        chfsession_keysym_name(ks, name, sizeof name);
        bound(name, stolen);
    }
    return 1;
}

int chf_prefs_pretranslate(MSG *msg)
{
    if (!g_win || !msg->hwnd || (msg->hwnd != g_win && !IsChild(g_win, msg->hwnd))) return 0;
    if (capture_key(msg)) return 1;
    /* F1 and the Ctrl shortcuts (Ctrl+Shift+H, Help) are the app's, not a dialog's */
    if ((msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN) &&
        (msg->wParam == VK_F1 || GetKeyState(VK_CONTROL) < 0))
        return 0;
    return IsDialogMessageW(g_win, msg) ? 1 : 0;
}
