/*
 * The Preferences window: a tab per page -- Machine, Display, Audio, Input,
 * FujiNet -- over the same settings keys as the other frontends'
 * Preferences, so a machine configured in one comes up the same in another.
 *
 * The picture, the volume, the analog sticks and the gamepad assignments
 * apply at once. The machine (which console) and the host
 * options (audio, gamepads, FujiNet) wait for the window to close, which
 * hands the main window one "dirty" flag so it power-cycles once for the lot.
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

#include "resource.h"

#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif

#define PREFS_CLASS "FujiNetGoArcPrefs"
#define PREFS_STYLE (WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN)
#define MAX_ITEMS 160

/* Layout, in 96-dpi units. */
#define CW 540              /* client width */
#define M 14                /* margin */
#define PAGE_H 250          /* the tallest page: Input */
#define ROW 22

enum { PG_MACHINE, PG_DISPLAY, PG_AUDIO, PG_INPUT, PG_FUJINET, PG_COUNT };

enum {
    IDC_TABS = 100, IDC_MACHINE,
    IDC_ASPECT, IDC_SMOOTH, IDC_AUDIO, IDC_VOLUME, IDC_GAMEPAD, IDC_ANALOG, IDC_PAD_LIST,
    IDC_PAD_PORT, IDC_FUJINET, IDC_EXTERNAL, IDC_EXT_ADDR, IDC_WEBUI, IDC_CLOSE
};

#define IDT_PADS 1

typedef struct {
    HWND h;
    int page;               /* PG_*, or -1: always shown */
    int x, y, w, ht;        /* page-relative, 96 dpi */
    int bold;               /* a heading */
} item;

static HWND g_win, g_tabs, g_close;
static HWND g_machine_note, g_pad_list, g_pad_port, g_ext_addr;
static HWND g_machine_combo;
static item g_items[MAX_ITEMS];
static int g_nitems;
static HFONT g_font, g_bold;
static int g_dpi = 96;
static chfsession *g_session;
static chf_prefs_hooks g_hooks;
static int g_dirty;
static int g_page;
static unsigned g_pad_seen;

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
    wchar_t cur[512], *w = chf_wide_dup(utf8);
    if (!w) return;
    GetWindowTextW(h, cur, 512);
    if (wcscmp(cur, w) != 0) SetWindowTextW(h, w);
    free(w);
}

/* ---- the machine page ---------------------------------------------------------- */

static int machine_selected(void)
{
    const int sel = (int)SendMessageA(g_machine_combo, CB_GETCURSEL, 0, 0);
    return sel >= 0 && sel < CHF_MACHINE_COUNT ? sel : CHF_MACHINE_ARCADIA;
}

/* The PAL clone is not verified against MAME (which runs every clone at
 * NTSC): say so under the combo. */
static void refresh_machine(void)
{
    set_text(g_machine_note,
             machine_selected() == CHF_MACHINE_ARCADIA_PAL
             ? "PAL clone (4.433 MHz / 5, 50 Hz, 312 lines): not verified against MAME, which runs every clone at NTSC."
             : "NTSC: Signetics 2650 at 0.895 MHz, 60 Hz. There is no BIOS and no ROM to import: "
               "games are cartridge files (Machine > Open Cartridge).");
}

static void build_machine(void)
{
    int y = 6, m;
    heading(PG_MACHINE, "Machine", y); y += 22;
    label(PG_MACHINE, "Applied by a power cycle when this window closes.", M, y, CW - 2 * M, 18); y += 28;
    label(PG_MACHINE, "Console:", M, y + 3, 70, 18);
    g_machine_combo = combo(PG_MACHINE, IDC_MACHINE, M + 76, y, 380);
    for (m = 0; m < CHF_MACHINE_COUNT; m++)
        combo_add(g_machine_combo, chf_machine_name(m));
    {
        const int cur = chfsession_get_int(g_session, "machine", CHF_MACHINE_ARCADIA);
        SendMessageA(g_machine_combo, CB_SETCURSEL, (WPARAM)(cur >= 0 && cur < CHF_MACHINE_COUNT ? cur : 0), 0);
    }
    y += 40;
    g_machine_note = label(PG_MACHINE, "", M, y, CW - 2 * M, 54);
    refresh_machine();
}

/* ---- display, audio -------------------------------------------------------------- */

static void build_display(void)
{
    HWND c;
    int y = 6;
    heading(PG_DISPLAY, "Display", y); y += 28;
    label(PG_DISPLAY, "Picture:", M, y + 3, 70, 18);
    c = combo(PG_DISPLAY, IDC_ASPECT, M + 76, y, 240);
    combo_add(c, "TV (4:3)");
    combo_add(c, "Square pixels");
    SendMessageA(c, CB_SETCURSEL, chfsession_get_int(g_session, "aspect", 0) == 0 ? 0 : 1, 0);
    y += 38;
    checkbox(PG_DISPLAY, IDC_SMOOTH, "Smooth scaling (off: sharp pixels)", M, y, 320,
             chfsession_get_int(g_session, "smooth", 0));
}

static void build_audio(void)
{
    HWND t;
    int y = 6;
    heading(PG_AUDIO, "Audio", y); y += 26;
    checkbox(PG_AUDIO, IDC_AUDIO, "Enable audio (applied when this window closes)", M, y, CW - 2 * M,
             chfsession_get_int(g_session, "enable_audio", 1));
    y += 34;
    label(PG_AUDIO, "Volume:", M, y + 4, 70, 18);
    t = add(L"msctls_trackbar32", NULL, TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, PG_AUDIO, IDC_VOLUME,
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
        const int eff = chfsession_gamepad_effective_port(g_session, i);
        chfsession_gamepad_name(g_session, i, name, sizeof name);
        if (eff >= 0)
            snprintf(line, sizeof line, "%s  [stick %d%s]", name, eff + 1,
                     chfsession_gamepad_assignment(g_session, i) < 0 ? ", automatic" : "");
        else
            snprintf(line, sizeof line, "%s  [unused]", name);
        SendMessageA(g_pad_list, LB_ADDSTRING, 0, (LPARAM)line);
    }
    if (sel >= 0 && sel < n) {
        SendMessageA(g_pad_list, LB_SETCURSEL, (WPARAM)sel, 0);
        SendMessageA(g_pad_port, CB_SETCURSEL, (WPARAM)(chfsession_gamepad_assignment(g_session, sel) + 1), 0);
    }
    g_pad_seen = chfsession_gamepad_generation(g_session);
}

static void build_input(void)
{
    int y = 6;
    heading(PG_INPUT, "Input", y); y += 26;
    checkbox(PG_INPUT, IDC_GAMEPAD, "Enable gamepads (applied when this window closes)", M, y, CW - 2 * M,
             chfsession_get_int(g_session, "enable_gamepad", 1));
    y += 28;
    checkbox(PG_INPUT, IDC_ANALOG, "Analog sticks steer too", M, y, 320,
             chfsession_get_int(g_session, "analog_joystick", 1));
    y += 36;
    label(PG_INPUT, "Gamepads (select one, then choose its stick):", M, y, CW - 2 * M, 18); y += 24;
    g_pad_list = add(L"LISTBOX", NULL, WS_VSCROLL | LBS_NOTIFY | WS_TABSTOP | WS_BORDER, PG_INPUT,
                     IDC_PAD_LIST, M, y, 340, 96);
    g_pad_port = combo(PG_INPUT, IDC_PAD_PORT, M + 350, y, 130);
    combo_add(g_pad_port, "Automatic");
    combo_add(g_pad_port, "Stick 1");
    combo_add(g_pad_port, "Stick 2");
    SendMessageA(g_pad_port, CB_SETCURSEL, 0, 0);
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
    label(PG_FUJINET, "Address:port:", M, y + 3, 90, 18);
    e = add(L"EDIT", chfsession_get_str(g_session, "fujinet_external_addr", ""),
            ES_AUTOHSCROLL | WS_TABSTOP | WS_BORDER, PG_FUJINET, IDC_EXT_ADDR, M + 96, y, 240, 24);
    g_ext_addr = e;
    if (e) {
        SendMessageW(e, EM_SETCUEBANNER, FALSE, (LPARAM)L"127.0.0.1:11526");
        EnableWindow(e, chfsession_get_int(g_session, "fujinet_external", 0) != 0);
    }
    y += 40;
    if (chfsession_fujinet_external(g_session))
        snprintf(line, sizeof line, "Web UI: FujiNet is external");
    else
        snprintf(line, sizeof line, "Web UI: %s", chfsession_fujinet_webui_url(g_session));
    label(PG_FUJINET, line, M, y + 5, 360, 18);
    button(PG_FUJINET, IDC_WEBUI, "Open", CW - M - 100, y, 100);
}

/* ---- layout ------------------------------------------------------------------------ */

static void show_page(int page)
{
    int i;
    g_page = page;
    for (i = 0; i < g_nitems; i++)
        if (g_items[i].page >= 0)
            ShowWindow(g_items[i].h, g_items[i].page == page ? SW_SHOW : SW_HIDE);
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
        int h = S(it->ht);
        if (!it->h) continue;
        /* a drop-down's height is its list: keep the closed box one line */
        MoveWindow(it->h, S(it->x), top + S(it->y), S(it->w), h, TRUE);
    }
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
            if (chfsession_get_int(g_session, "machine", CHF_MACHINE_ARCADIA) != m) {
                chfsession_set_int(g_session, "machine", m);
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
    case IDC_ANALOG:
        chfsession_set_analog(g_session, SendDlgItemMessageA(g_win, IDC_ANALOG, BM_GETCHECK, 0, 0) == BST_CHECKED);
        break;
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
    case IDC_FUJINET: setting_from_checkbox(id, "enable_fujinet", 1); break;
    case IDC_EXTERNAL:
        setting_from_checkbox(id, "fujinet_external", 0);
        EnableWindow(g_ext_addr, SendDlgItemMessageA(g_win, IDC_EXTERNAL, BM_GETCHECK, 0, 0) == BST_CHECKED);
        break;
    case IDC_WEBUI: open_web_ui(); break;
    default: break;
    }
}

/* ---- the window ----------------------------------------------------------------------- */

static LRESULT CALLBACK prefs_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_CLOSE || LOWORD(wp) == IDCANCEL) {
            PostMessageA(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        if (LOWORD(wp) != IDOK) on_command(LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_NOTIFY:
        if (((LPNMHDR)lp)->hwndFrom == g_tabs && ((LPNMHDR)lp)->code == (UINT)TCN_SELCHANGE) {
            show_page(TabCtrl_GetCurSel(g_tabs));
            return 0;
        }
        break;
    case WM_HSCROLL:
        if ((HWND)lp == GetDlgItem(hwnd, IDC_VOLUME))
            chfsession_set_volume(g_session, (int)SendMessageA((HWND)lp, TBM_GETPOS, 0, 0));
        return 0;
    case WM_TIMER:
        if (wp == IDT_PADS && chfsession_gamepad_generation(g_session) != g_pad_seen)
            refresh_pad_list();
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
        g_win = NULL;
        g_tabs = g_close = g_pad_list = g_pad_port = g_ext_addr = g_machine_combo = g_machine_note = NULL;
        g_nitems = 0;
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
    static const char *const names[PG_COUNT] = { "Machine", "Display", "Audio", "Input", "FujiNet" };
    HINSTANCE inst = GetModuleHandleA(NULL);
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_TAB_CLASSES | ICC_BAR_CLASSES };
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
    g_nitems = 0;
    g_dirty = 0;
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
    build_audio();
    build_input();
    build_fujinet();
    g_close = CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                              0, 0, 10, 10, g_win, (HMENU)(INT_PTR)IDC_CLOSE, inst, NULL);
    /* the tab strip first in creation order, last in z-order: with
     * WS_CLIPSIBLINGS it never paints over a page */
    SetWindowPos(g_tabs, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    make_fonts();
    refresh_pad_list();
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

int chf_prefs_pretranslate(MSG *msg)
{
    if (!g_win || !msg->hwnd || (msg->hwnd != g_win && !IsChild(g_win, msg->hwnd))) return 0;
    return IsDialogMessageW(g_win, msg) ? 1 : 0;
}
