/*
 * The FujiNet console log window: a live view of the in-process runtime's
 * captured output, refreshed once a second, following its end while the
 * reader is there. The text is set only when the log changed, so an idle
 * runtime costs the window no repaint.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fujilog.h"

#include <string.h>

#include "help/help_window.h"
#include "resource.h"

#define LOG_CLASS "FujiNetGoChfLog"
#define MONO_PX 13
#define HELP_BAR 32          /* the strip with the Help button, 96-dpi units */

static HWND g_win, g_edit, g_help;
static HFONT g_font;
static chfsession *g_session;
static unsigned long g_last_hash;
static int g_last_len = -1;

/* EDIT controls want CR LF and draw control characters as boxes. */
static char *to_crlf(const char *text)
{
    size_t n = strlen(text), i, o = 0;
    char *buf = malloc(n * 2 + 1);
    if (!buf) return NULL;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n') { buf[o++] = '\r'; buf[o++] = '\n'; }
        else if (c >= 0x20 || c == '\t') buf[o++] = (char)c;
    }
    buf[o] = '\0';
    return buf;
}

static unsigned long hash_text(const char *s, int n)
{
    unsigned long h = 5381;
    int i;
    for (i = 0; i < n; i++) h = h * 33 + (unsigned char)s[i];
    return h;
}

/* How many rows of the log fit, for "is the reader at the end". */
static int visible_rows(void)
{
    RECT c;
    TEXTMETRICA tm;
    HDC dc = GetDC(g_edit);
    HGDIOBJ old = SelectObject(dc, g_font);
    int rows = 1;
    GetClientRect(g_edit, &c);
    if (GetTextMetricsA(dc, &tm) && tm.tmHeight > 0) rows = c.bottom / tm.tmHeight;
    SelectObject(dc, old);
    ReleaseDC(g_edit, dc);
    return rows > 0 ? rows : 1;
}

static void refresh(void)
{
    static char buf[128 * 1024];
    int first, count, n, at_end;
    char *crlf;
    wchar_t *w;

    if (!g_edit) return;
    n = chfsession_fujinet_copy_log(g_session, buf, sizeof buf);
    if (n < 0) n = 0;
    if (n == g_last_len && hash_text(buf, n) == g_last_hash) return;
    g_last_len = n;
    g_last_hash = hash_text(buf, n);

    first = (int)SendMessageA(g_edit, EM_GETFIRSTVISIBLELINE, 0, 0);
    count = (int)SendMessageA(g_edit, EM_GETLINECOUNT, 0, 0);
    at_end = first + visible_rows() >= count;
    crlf = to_crlf(n > 0 ? buf : "(no FujiNet output yet)");
    if (!crlf) return;
    /* the log is UTF-8 */
    w = chf_wide_dup(crlf);
    if (w) {
        SendMessageW(g_edit, WM_SETREDRAW, FALSE, 0);
        SetWindowTextW(g_edit, w);
        /* follow the tail only while the reader is already at the bottom */
        if (at_end)
            SendMessageW(g_edit, EM_LINESCROLL, 0, SendMessageW(g_edit, EM_GETLINECOUNT, 0, 0));
        else
            SendMessageW(g_edit, EM_LINESCROLL, 0, first);
        SendMessageW(g_edit, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g_edit, NULL, TRUE);
        free(w);
    }
    free(crlf);
}

static void make_font(HWND hwnd)
{
    if (g_font) DeleteObject(g_font);
    g_font = chf_make_font(chf_window_dpi(hwnd), MONO_PX, FW_NORMAL, CHF_MONO_FACE);
    if (g_edit) SendMessageA(g_edit, WM_SETFONT, (WPARAM)g_font, TRUE);
    if (g_help) {
        static HFONT ui;
        HFONT old = ui;
        ui = chf_make_font(chf_window_dpi(hwnd), 0, FW_NORMAL, NULL);
        SendMessageA(g_help, WM_SETFONT, (WPARAM)ui, TRUE);
        if (old) DeleteObject(old);
    }
}

static LRESULT CALLBACK log_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE: {
        RECT rc;
        const int dpi = chf_window_dpi(hwnd), bar = chf_scale(HELP_BAR, dpi);
        GetClientRect(hwnd, &rc);
        if (g_edit) MoveWindow(g_edit, 0, bar, rc.right - rc.left, rc.bottom - rc.top - bar, TRUE);
        if (g_help) MoveWindow(g_help, rc.right - chf_scale(84, dpi), chf_scale(4, dpi), chf_scale(76, dpi),
                               bar - chf_scale(8, dpi), TRUE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_LOG_HELP) { chf_help_for(hwnd); return 0; }
        break;
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)lp;
        make_font(hwnd);
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        /* a read-only EDIT: the window colour, as a text view */
        SetTextColor((HDC)wp, GetSysColor(COLOR_WINDOWTEXT));
        SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_TIMER:
        if (wp == IDT_LOG_REFRESH) refresh();
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, IDT_LOG_REFRESH);
        g_win = NULL;
        g_edit = NULL;
        g_help = NULL;
        if (g_font) DeleteObject(g_font);
        g_font = NULL;
        return 0;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

void chf_fujilog_show(HWND parent, chfsession *session)
{
    static int registered;
    HINSTANCE inst = GetModuleHandleA(NULL);
    RECT rc;
    int dpi;

    g_session = session;
    if (g_win) {
        if (IsIconic(g_win)) ShowWindow(g_win, SW_RESTORE);
        SetForegroundWindow(g_win);
        return;
    }
    if (!registered) {
        WNDCLASSEXA wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = log_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = LOG_CLASS;
        wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(IDI_APPICON));
        registered = RegisterClassExA(&wc) != 0;
    }
    dpi = chf_window_dpi(parent);
    /* owned by the main window: it stays over it and goes with it */
    g_win = CreateWindowA(LOG_CLASS, "FujiNet Console Log", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                          CW_USEDEFAULT, chf_scale(820, dpi), chf_scale(560, dpi), parent, NULL, inst, NULL);
    if (!g_win) return;
    GetClientRect(g_win, &rc);
    g_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        0, 0, rc.right - rc.left, rc.bottom - rc.top, g_win, (HMENU)(INT_PTR)IDC_LOG_EDIT, inst, NULL);
    SendMessageA(g_edit, EM_SETLIMITTEXT, 0, 0);
    g_help = CreateWindowExA(0, "BUTTON", "Help", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10,
                             g_win, (HMENU)(INT_PTR)IDC_LOG_HELP, inst, NULL);
    chf_help_set_context(g_win, "fujilog", 1);
    SendMessageA(g_win, WM_SIZE, 0, 0);
    make_font(g_win);
    g_last_len = -1;
    SetTimer(g_win, IDT_LOG_REFRESH, 1000, NULL);
    refresh();
    ShowWindow(g_win, SW_SHOW);
}
