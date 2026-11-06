/*
 * FujiNet Go Channel F -- the Windows (Win32) frontend.
 *
 * No toolkit: a plain window, a menu bar and a status bar, with the picture a
 * child window drawn by display.c (Direct3D 11, GDI as the fallback). That
 * keeps the artifact a folder you copy rather than a runtime hunt.
 *
 * Three Windows-specific things are load bearing:
 *
 *   The display's render thread is this platform's frame clock: it feeds each
 *   vblank to the core (display.c). There is no GdkFrameClock here, and a
 *   plain timer would beat against the panel.
 *
 *   WM_ACTIVATE releases every held key. Alt-tabbing away mid-game and coming
 *   back to a stick still pushed is the classic symptom of not doing this, and
 *   Windows is where it happens most, because the WM eats the key-up.
 *
 *   A system key (WM_SYSKEYDOWN) is passed to the session and swallowed only
 *   when it is bound (none is by default): Alt+F4, the menu mnemonics and
 *   Alt+Esc stay Windows'.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chfsession.h"
#include "controllers/controllers_window.h"
#include "debugger/dbg_window.h"
#include "display.h"
#include "fujilog.h"
#include "help/help_window.h"
#include "import.h"
#include "key_forward.h"
#include "prefs.h"
#include "resource.h"
#include "win_common.h"

#define WIN_CLASS "FujiNetGoChf"
#define APP_TITLE "FujiNet Go Channel F"
#define APP_TITLE_W L"FujiNet Go Channel F"
#define INSTANCE_MUTEX "Local\\FujiNetGoChf.Instance"

/* How long a notice (a gamepad hot-plug, Preferences applying) stays in the
 * status bar. */
#define NOTICE_MS 4000

/* The status bar's first part: the FujiNet link dot. */
#define DOT_PART_W 22

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

static chfsession *g_session;
static HWND g_hwnd, g_display, g_statusbar;
static int g_aspect = 1, g_smooth = 0, g_fullscreen = 0;    /* the TV's 4:3 on */
static WINDOWPLACEMENT g_placement;
static int g_sysact_down[CHF_SYSACT_COUNT];
static unsigned g_pad_generation;
static char g_notice[160];
static DWORD g_notice_until;
static HBRUSH g_accent_brush;
static int g_bios_alert;         /* needs_bios as last seen: alert once per rise */
static int g_importing;          /* Import Cartridge to SD is up: its Help button is ours */

static void update_status(void);
static void sync_menu_checks(void);
static void layout_children(void);

/* ---- small helpers -------------------------------------------------------- */

static int S(int v) { return chf_scale(v, chf_window_dpi(g_hwnd)); }

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '\\');
    const char *t = strrchr(p, '/');
    if (t && (!s || t > s)) s = t;
    return s ? s + 1 : p;
}

static int env_on(const char *name)
{
    const char *env = getenv(name);
    return env && *env && *env != '0';
}

static void notice(const char *text)
{
    snprintf(g_notice, sizeof g_notice, "%s", text);
    g_notice_until = GetTickCount() + NOTICE_MS;
    update_status();
}

/* The task dialog's Help button (a third button that does not close it). */
#define ASK_YES  1001
#define ASK_NO   1002
#define ASK_HELP 1003

static HRESULT CALLBACK ask_callback(HWND dlg, UINT note, WPARAM wp, LPARAM lp, LONG_PTR ref)
{
    (void)lp;
    if (note == TDN_BUTTON_CLICKED && wp == ASK_HELP) {
        chf_help_show(dlg, (const char *)ref);
        return S_FALSE;
    }
    return S_OK;
}

/* A question with two buttons of our own words, and a Help button that opens
 * the help context (when not NULL). Task dialogs need the version 6 common
 * controls (the manifest asks for them); looked up rather than imported, so
 * a process without them still gets a plain Yes/No. 1 for the first button. */
static int ask(HWND owner, const char *text, const char *title, const char *yes, const char *no,
               const char *help)
{
    typedef HRESULT (WINAPI *task_fn)(const TASKDIALOGCONFIG *, int *, int *, BOOL *);
    HMODULE cc = LoadLibraryA("comctl32.dll");
    task_fn fn = cc ? (task_fn)(void (*)(void))GetProcAddress(cc, "TaskDialogIndirect") : NULL;
    if (fn) {
        wchar_t *wt = chf_wide_dup(text), *wc = chf_wide_dup(title);
        wchar_t *wy = chf_wide_dup(yes), *wn = chf_wide_dup(no), *wh = chf_wide_dup("Help");
        TASKDIALOG_BUTTON buttons[3];
        TASKDIALOGCONFIG cfg;
        int chosen = 0;
        HRESULT hr;
        memset(&cfg, 0, sizeof cfg);
        buttons[0].nButtonID = ASK_YES;
        buttons[0].pszButtonText = wy;
        buttons[1].nButtonID = ASK_NO;
        buttons[1].pszButtonText = wn;
        buttons[2].nButtonID = ASK_HELP;
        buttons[2].pszButtonText = wh;
        cfg.cbSize = sizeof cfg;
        cfg.hwndParent = owner;
        cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
        cfg.pszWindowTitle = wc;
        cfg.pszMainIcon = TD_INFORMATION_ICON;
        cfg.pszContent = wt;
        cfg.cButtons = help ? 3 : 2;
        cfg.pButtons = buttons;
        cfg.nDefaultButton = ASK_YES;
        cfg.pfCallback = ask_callback;
        cfg.lpCallbackData = (LONG_PTR)help;
        hr = fn(&cfg, &chosen, NULL, NULL);
        free(wt); free(wc); free(wy); free(wn); free(wh);
        if (SUCCEEDED(hr))
            return chosen == ASK_YES;
    }
    return chf_message(owner, text, title, MB_ICONINFORMATION | MB_YESNO) == IDYES;
}

/* ---- layout: picture, status bar --------------------------------------- */

static int statusbar_height(void)
{
    RECT r;
    if (!g_statusbar || !IsWindowVisible(g_statusbar)) return 0;
    GetWindowRect(g_statusbar, &r);
    return r.bottom - r.top;
}

/* The picture fills the client area above the status bar. */
static void layout_children(void)
{
    RECT rc;
    int bottom;
    if (!g_display) return;
    GetClientRect(g_hwnd, &rc);
    bottom = rc.bottom - statusbar_height();
    if (bottom < 0) bottom = 0;
    MoveWindow(g_display, 0, 0, rc.right, bottom, TRUE);
}

/* ---- the machine ----------------------------------------------------------------- */

static void read_display_settings(void);

/* Power-cycle with the current settings: the session decides whether that is
 * a power cycle of the console alone or a restart of FujiNet, audio and the
 * gamepads with it. */
static void restart_session(void)
{
    if (chfsession_restart(g_session) != 0)
        chf_message(g_hwnd, chfsession_last_error(g_session), "Could not start", MB_ICONWARNING | MB_OK);
    read_display_settings();        /* a new machine can be another shape */
    chf_controllers_window_refresh();
    update_status();
}

static void run_sysaction(int sa)
{
    switch (sa) {
    case CHF_SYSACT_RESET_CONFIG:
        chfsession_sysaction(g_session, sa);
        break;
    case CHF_SYSACT_DEBUG_STOP:
        /* Showing the debugger attaches it, and attaching stops the machine. */
        chf_debugger_show(g_hwnd, g_session);
        break;
    default: break;
    }
}

/* The ROMs changed under a console that was missing some: what the machine
 * is missing is a snapshot taken at power-up, so the session restarts to see
 * the new ones -- the cartridge it runs again, or CONFIG. */
static void apply_new_roms(unsigned was_missing)
{
    chf_prefs_roms_changed();
    if (was_missing)
        restart_session();
    chf_controllers_window_refresh();
    update_status();
}

/* Import BIOS from the menu, Preferences or an alert: files or a folder.
 * Returns 1 when something was imported. */
static int import_bios(HWND owner, int folder)
{
    const unsigned was_missing = chfsession_missing(g_session);
    const int n = chf_import_bios(owner, g_session, folder);
    if (n > 0) apply_new_roms(was_missing);
    return n > 0;
}

/* A game the console cannot run yet: the session says so and the message
 * names Import BIOS when importing fixes it. */
static int import_would_fix(const char *err)
{
    return err && strstr(err, "Import BIOS") != NULL;
}

static void open_cart(const char *path)
{
    char again[1024];
    int tries;
    snprintf(again, sizeof again, "%s", path);
    for (tries = 0; tries < 3; tries++) {
        char msg[700];
        if (chfsession_load_cart(g_session, again) == 0) break;
        snprintf(msg, sizeof msg, "%s", chfsession_last_error(g_session));
        if (!import_would_fix(msg)) {
            chf_message(g_hwnd, msg, "Could not open", MB_ICONWARNING | MB_OK);
            break;
        }
        /* the refused cartridge is tried again once the ROMs are in */
        if (!ask(g_hwnd, msg, "BIOS needed", "Import BIOS...", "Not Now", "nobios") ||
            !import_bios(g_hwnd, 0))
            break;
    }
    update_status();
}

/* A dropped file, or one named on the command line: a ROM (a BIOS, a zip of
 * them) is imported, a cartridge opened, anything else copied to FujiNet's
 * SD folder for CONFIG to mount. */
static void load_media(const char *path)
{
    char dest[1024];
    if (chfsession_media_is_rom(path)) {
        const unsigned was_missing = chfsession_missing(g_session);
        const char *one[1];
        one[0] = path;
        if (chf_import_roms(g_hwnd, g_session, one, 1) > 0) apply_new_roms(was_missing);
        return;
    }
    if (chfsession_media_is_cartridge(path)) { open_cart(path); return; }
    if (chfsession_import_media(g_session, path, dest, sizeof dest) != 0) {
        chf_message(g_hwnd, chfsession_last_error(g_session), "Import failed", MB_ICONWARNING | MB_OK);
        return;
    }
    chf_message(g_hwnd, "Copied to FujiNet's SD folder. Mount it from the CONFIG client.", "Imported",
               MB_ICONINFORMATION | MB_OK);
}

/* The stand-in met a game (CONFIG booted one from the network with no BIOS
 * imported): say so once per occurrence, with the reason, and offer the
 * import, after which the console boots on the real BIOS. Polled from the
 * status timers; the latch keeps the alert from coming back while it is
 * still up (a modal loop lets the timers keep firing). */
static void poll_needs_bios(void)
{
    char why[256], text[640];
    if (!chfsession_needs_bios(g_session)) { g_bios_alert = 0; return; }
    if (g_bios_alert) return;
    g_bios_alert = 1;
    why[0] = '\0';
    chfsession_missing_reason(g_session, why, sizeof why);
    snprintf(text, sizeof text,
             "This cartridge needs the Channel F BIOS%s%s%s.\n\n"
             "Videocarts call routines in the console's BIOS, so only the real one runs them; "
             "CONFIG and the FujiNet apps run without it.\n\n"
             "Machine > Import BIOS... adds your own dump.",
             why[0] ? " (" : "", why, why[0] ? ")" : "");
    if (!ask(g_hwnd, text, "Needs BIOS", "Import BIOS...", "Not Now", "nobios"))
        return;
    import_bios(g_hwnd, 0);
}

/* ---- status ------------------------------------------------------------------------- */

static void set_text_utf8(HWND h, const char *text, int statusbar)
{
    wchar_t w[512];
    MultiByteToWideChar(CP_UTF8, 0, text, -1, w, 512);
    if (statusbar) SendMessageW(h, SB_SETTEXTW, 1, (LPARAM)w);   /* part 1: the text */
    else SetWindowTextW(h, w);
}

static void update_status(void)
{
    char title[512], st[160], line[400], type[96];
    const char *cart = chfsession_cart_path(g_session);

    if (!chfsession_is_running(g_session)) {
        snprintf(title, sizeof title, APP_TITLE " \xe2\x80\x94 stopped");
        snprintf(st, sizeof st, "stopped");
        type[0] = '\0';
    } else {
        chfsession_cart_status(g_session, st, sizeof st);
        chfsession_cart_type_name(g_session, type, sizeof type);
        if (cart && cart[0])
            snprintf(title, sizeof title, APP_TITLE " \xe2\x80\x94 %s", base_name(cart));
        else if (chfsession_cart_booted_game(g_session))
            snprintf(title, sizeof title, APP_TITLE " \xe2\x80\x94 booted from FujiNet");
        else
            snprintf(title, sizeof title, APP_TITLE " \xe2\x80\x94 CONFIG");
    }
    set_text_utf8(g_hwnd, title, 0);

    if (!g_statusbar) return;
    /* Part 0 is the link dot, drawn in WM_DRAWITEM from this item data. */
    SendMessageA(g_statusbar, SB_SETTEXTA, SBT_OWNERDRAW | 0,
                 (LPARAM)(chfsession_cart_link_up(g_session) == 1));
    if (g_notice[0] && GetTickCount() < g_notice_until) {
        snprintf(line, sizeof line, "%s", g_notice);
    } else {
        g_notice[0] = '\0';
        snprintf(line, sizeof line, "%s%sFujiNet: %s%s", type, type[0] ? " \xe2\x80\x94 " : "", st,
                 chfsession_fujinet_external(g_session) ? " (external)"
                 : chfsession_fujinet_running(g_session) ? "" : " (runtime not running)");
    }
    set_text_utf8(g_statusbar, line, 1);
}

/* The link dot: the accent while the cartridge's link to FujiNet is up, grey
 * otherwise. */
static void draw_status_dot(const DRAWITEMSTRUCT *di)
{
    const int up = di->itemData != 0;
    const int size = S(10);
    RECT r = di->rcItem;
    const int x = r.left + ((r.right - r.left) - size) / 2;
    const int y = r.top + ((r.bottom - r.top) - size) / 2;
    HGDIOBJ oldb = SelectObject(di->hDC, up ? (HGDIOBJ)g_accent_brush : GetStockObject(GRAY_BRUSH));
    HGDIOBJ oldp = SelectObject(di->hDC, GetStockObject(NULL_PEN));
    Ellipse(di->hDC, x, y, x + size + 1, y + size + 1);
    SelectObject(di->hDC, oldp);
    SelectObject(di->hDC, oldb);
}

/* Gamepad hot-plug: the session bumps a generation on every add/remove and
 * keeps the last event as text; show it for a few seconds. */
static void poll_gamepads(void)
{
    const unsigned gen = chfsession_gamepad_generation(g_session);
    char text[160];
    if (gen == g_pad_generation) return;
    g_pad_generation = gen;
    if (chfsession_gamepad_last_event(g_session, text, sizeof text) > 0)
        notice(text);
    chf_controllers_window_refresh();
    chf_prefs_gamepads_changed();
}

/* ---- menu ----------------------------------------------------------------------------- */

/* A menu item's label with the key it is bound to after the tab -- read from
 * the bindings each time the menu opens, so a remap in the Controllers window
 * shows here. The key is only a label: there is no accelerator table, so F1
 * and Escape reach the console through the bindings rather than being eaten by
 * the menu. */
static void bound_item(HMENU m, UINT id, const char *text, int target)
{
    char key[32], label[96];
    const chf_binding b = chfsession_binding_get(g_session, target);
    key[0] = '\0';
    chfsession_keysym_name(b.keysym, key, sizeof key);
    if (key[0]) snprintf(label, sizeof label, "%s\t%s", text, key);
    else snprintf(label, sizeof label, "%s", text);
    ModifyMenuA(m, id, MF_BYCOMMAND | MF_STRING, id, label);
}

/* The console's own button, RESET, with its bound key; Pause with its check. */
static void sync_machine_menu(void)
{
    HMENU m = GetMenu(g_hwnd);
    if (!m) return;
    bound_item(m, IDM_RESET, "&Reset", CHF_TARGET_SWITCH(CHF_SW_RESET));
    {
        /* Ctrl+R always, and the session action's key (Escape) */
        char key[32], label[96];
        const chf_binding b = chfsession_binding_get(g_session, CHF_TARGET_SYSACT(CHF_SYSACT_RESET_CONFIG));
        key[0] = '\0';
        chfsession_keysym_name(b.keysym, key, sizeof key);
        snprintf(label, sizeof label, "Reset to &CONFIG\tCtrl+R%s%s", key[0] ? ", " : "", key);
        ModifyMenuA(m, IDM_RESET_CONFIG, MF_BYCOMMAND | MF_STRING, IDM_RESET_CONFIG, label);
    }
    CheckMenuItem(m, IDM_PAUSE, MF_BYCOMMAND | (chfsession_is_paused(g_session) ? MF_CHECKED : MF_UNCHECKED));
}

static void sync_menu_checks(void)
{
    HMENU m = GetMenu(g_hwnd);
    if (!m) return;
    CheckMenuItem(m, IDM_TV_ASPECT, MF_BYCOMMAND | (g_aspect ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_SMOOTH, MF_BYCOMMAND | (g_smooth ? MF_CHECKED : MF_UNCHECKED));
}

static void build_menu(HWND hwnd)
{
    HMENU bar = CreateMenu();
    HMENU machine = CreatePopupMenu();
    HMENU view = CreatePopupMenu();
    HMENU fuji = CreatePopupMenu();
    HMENU help = CreatePopupMenu();

    AppendMenuA(machine, MF_STRING, IDM_OPEN, "&Open Cartridge...\tCtrl+O");
    AppendMenuA(machine, MF_STRING, IDM_EJECT, "&Eject Cartridge");
    AppendMenuA(machine, MF_STRING, IDM_IMPORT_SD, "&Import Cartridge to SD...");
    AppendMenuA(machine, MF_STRING, IDM_OPEN_SD, "Open &SD Folder");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_IMPORT_BIOS, "Import &BIOS...");
    AppendMenuA(machine, MF_STRING, IDM_IMPORT_FOLDER, "Import BIOS from a &Folder...");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_PAUSE, "&Pause");
    AppendMenuA(machine, MF_STRING, IDM_RESET, "&Reset");
    AppendMenuA(machine, MF_STRING, IDM_RESET_CONFIG, "Reset to &CONFIG\tCtrl+R");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_SETTINGS, "Pre&ferences...\tCtrl+,");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_EXIT, "E&xit\tCtrl+Q");

    AppendMenuA(view, MF_STRING, IDM_CONTROLLERS, "&Controllers\tF9");
    AppendMenuA(view, MF_STRING, IDM_DEBUGGER, "&Debugger\tF12");
    AppendMenuA(view, MF_SEPARATOR, 0, NULL);
    AppendMenuA(view, MF_STRING, IDM_TV_ASPECT, "&TV Aspect");
    AppendMenuA(view, MF_STRING, IDM_SMOOTH, "&Smooth Scaling");
    AppendMenuA(view, MF_STRING, IDM_FULLSCREEN, "&Fullscreen\tF11");

    AppendMenuA(fuji, MF_STRING, IDM_FUJINET_CONFIG, "FujiNet &Web UI");
    AppendMenuA(fuji, MF_STRING, IDM_FUJINET_LOG, "Console &Log");

    AppendMenuA(help, MF_STRING, IDM_HELP, APP_TITLE " &Help\tCtrl+Shift+H");
    AppendMenuA(help, MF_SEPARATOR, 0, NULL);
    AppendMenuA(help, MF_STRING, IDM_ABOUT, "&About " APP_TITLE);

    AppendMenuA(bar, MF_POPUP, (UINT_PTR)machine, "&Machine");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)view, "&View");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)fuji, "&FujiNet");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)help, "&Help");
    SetMenu(hwnd, bar);
    sync_machine_menu();
    sync_menu_checks();
}

/* ---- Preferences glue -------------------------------------------------------------------- */

/* The picture options live in the settings: Preferences writes them and says
 * so. */
static void read_display_settings(void)
{
    g_aspect = chfsession_get_int(g_session, "aspect", 0) == 0;
    g_smooth = chfsession_get_int(g_session, "smooth", 0) != 0;
    chf_display_set_options(g_aspect, g_smooth);
    sync_menu_checks();
}

static void prefs_closed(int dirty)
{
    if (!dirty) return;
    restart_session();
    notice("Preferences applied: the console was power-cycled");
}

static void prefs_roms_changed(void)
{
    /* a BIOS imported from Preferences: the main window follows it */
    update_status();
}

static void show_prefs(void)
{
    static const chf_prefs_hooks hooks = { read_display_settings, prefs_roms_changed, prefs_closed };
    chf_prefs_show(g_hwnd, g_session, &hooks);
}

static void open_web_ui(HWND owner)
{
    if (chfsession_fujinet_external(g_session)) {
        chf_message(owner, "FujiNet is external: use the web UI of the instance this app dials.", "FujiNet",
                   MB_ICONINFORMATION | MB_OK);
        return;
    }
    if (!chfsession_fujinet_running(g_session)) {
        chf_message(owner, "FujiNet is not running.", "FujiNet", MB_ICONINFORMATION | MB_OK);
        return;
    }
    ShellExecuteA(owner, "open", chfsession_fujinet_webui_url(g_session), NULL, NULL, SW_SHOWNORMAL);
}

static void show_log(void)
{
    if (chfsession_fujinet_external(g_session)) {
        chf_message(g_hwnd, "FujiNet is external: its console output is in the process that is running it.",
                   "FujiNet", MB_ICONINFORMATION | MB_OK);
        return;
    }
    chf_fujilog_show(g_hwnd, g_session);
}

static void show_about(void)
{
    chf_message(g_hwnd,
        APP_TITLE " " CHF_VERSION_STRING "\n"
        "A Fairchild Channel F with a built-in FujiNet cartridge\n\n"
        "The console is modelled on MAME's Channel F driver (GPL-2.0+)\n"
        "and its F8 processor and cartridge slot (BSD-3-Clause),\n"
        "transposed to C. The FujiNet firmware (GPL-3.0+) runs inside\n"
        "the app. No BIOS or other ROM is part of the app.\n\n"
        "Copyright \xc2\xa9 2026 Thomas Cherryhomes \xe2\x80\x94 GPL-3.0-or-later\n"
        "https://fujinet.online/\n"
        "Issues: https://github.com/FujiNetWIFI/fujinet-go-channelf-desktop/issues",
        "About " APP_TITLE, MB_ICONINFORMATION | MB_OK);
}

/* ---- window ---------------------------------------------------------------------------------- */

static void toggle_fullscreen(HWND hwnd)
{
    DWORD style = GetWindowLong(hwnd, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO mi;
        memset(&mi, 0, sizeof mi);
        mi.cbSize = sizeof mi;
        g_placement.length = sizeof g_placement;
        GetWindowPlacement(hwnd, &g_placement);
        if (GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLong(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetMenu(hwnd, NULL);
            ShowWindow(g_statusbar, SW_HIDE);
            g_fullscreen = 1;
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    } else {
        SetWindowLong(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        build_menu(hwnd);
        ShowWindow(g_statusbar, SW_SHOW);
        g_fullscreen = 0;
        SetWindowPlacement(hwnd, &g_placement);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    layout_children();
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg >= 0xC000 && msg == chf_import_help_msg() && g_importing) {
        chf_help_show(hwnd, "import-sd");
        return 0;
    }
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (g_statusbar) SendMessageA(g_statusbar, WM_SIZE, 0, 0);
        layout_children();
        /* minimized: nothing to draw, so the render thread sleeps (the
         * machine, the audio and FujiNet run on) */
        chf_display_set_active(wp != SIZE_MINIMIZED);
        return 0;
    case WM_DRAWITEM:
        if (wp == IDC_STATUSBAR) { draw_status_dot((const DRAWITEMSTRUCT *)lp); return TRUE; }
        break;
    case WM_INITMENUPOPUP:
        sync_machine_menu();
        break;
    case WM_ENTERMENULOOP:
        /* The menu takes the keyboard: whatever was held lets go now rather
         * than staying down until the key is pressed again. */
        chfsession_release_all(g_session);
        memset(g_sysact_down, 0, sizeof g_sysact_down);
        break;
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)lp;
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    /* WM_SYSKEY* as well: Windows reports Alt -- and every key pressed while
     * it is down -- as a system key. One that drives the console (a bound
     * key) is the console's; the rest (Alt+F4, the menu mnemonics, Alt+Esc)
     * stay Windows'. */
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        uint32_t ks;
        int sa;
        if (chf_controllers_map_key(msg, wp, lp)) return 0;
        if (wp == VK_F9) { chf_controllers_window_toggle(hwnd, g_session); return 0; }
        if (wp == VK_F12) { chf_debugger_toggle(hwnd, g_session); return 0; }
        if (wp == VK_F11) { toggle_fullscreen(hwnd); return 0; }
        if (msg == WM_KEYDOWN && chf_app_key(wp)) {
            if (wp == 'O') PostMessage(hwnd, WM_COMMAND, IDM_OPEN, 0);
            else if (wp == 'R') PostMessage(hwnd, WM_COMMAND, IDM_RESET_CONFIG, 0);
            else if (wp == 'Q') PostMessage(hwnd, WM_COMMAND, IDM_EXIT, 0);
            else if (wp == VK_OEM_COMMA) PostMessage(hwnd, WM_COMMAND, IDM_SETTINGS, 0);
            /* Ctrl+Shift+H is the Help's, taken before the message reaches here */
            return 0;
        }
        if (msg == WM_SYSKEYDOWN) {
            /* the session's own actions never fire from a system key: Alt+Esc
             * is Windows', not Reset to CONFIG */
            if (chf_forward_key(g_session, msg, wp, lp)) return 0;
            break;
        }
        ks = chf_keysym_from_msg(wp, lp);
        if (!ks) break;
        /* The session's own actions (Escape) first; then the console, through
         * the bindings like every other key. */
        sa = chfsession_key_sysaction(g_session, ks);
        if (sa >= 0) {
            if (!(lp & (1 << 30)) && !g_sysact_down[sa]) {
                g_sysact_down[sa] = 1;
                run_sysaction(sa);
                update_status();
            }
            return 0;
        }
        if (chf_forward_key(g_session, msg, wp, lp)) return 0;
        break;
    }
    case WM_KEYUP: case WM_SYSKEYUP: {
        uint32_t ks;
        int sa;
        if (chf_controllers_map_key(msg, wp, lp)) return 0;
        ks = chf_keysym_from_msg(wp, lp);
        if (!ks) break;
        sa = chfsession_key_sysaction(g_session, ks);
        if (sa >= 0) { g_sysact_down[sa] = 0; return 0; }
        /* A bound Alt's release stops here, so it does not also put the menu
         * bar into keyboard mode. */
        if (chf_forward_key(g_session, msg, wp, lp)) return 0;
        break;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            chfsession_release_all(g_session);
            memset(g_sysact_down, 0, sizeof g_sysact_down);
        }
        return 0;

    case WM_TIMER:
        if (wp == IDT_STATUS) {
            update_status();
        } else if (wp == IDT_SYSACT) {
            int sa;
            while (chfsession_sysaction_take(g_session, &sa)) {
                run_sysaction(sa);
                update_status();
            }
            poll_gamepads();
            poll_needs_bios();
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_OPEN: {
            char path[MAX_PATH];
            if (chf_pick_cart(hwnd, "Open Cartridge", path, sizeof path)) open_cart(path);
            return 0;
        }
        case IDM_EJECT: chfsession_eject(g_session); update_status(); return 0;
        case IDM_IMPORT_SD:
            g_importing = 1;
            chf_import_carts(hwnd, g_session);
            g_importing = 0;
            return 0;
        case IDM_OPEN_SD: chf_open_sd_folder(hwnd, g_session); return 0;
        case IDM_IMPORT_BIOS: import_bios(hwnd, 0); return 0;
        case IDM_IMPORT_FOLDER: import_bios(hwnd, 1); return 0;
        case IDM_PAUSE: chfsession_set_paused(g_session, !chfsession_is_paused(g_session)); return 0;
        /* the console's RESET key: the session holds it for a few frames, as
         * a finger would -- the console samples it once a frame */
        case IDM_RESET: chfsession_reset(g_session); update_status(); return 0;
        case IDM_RESET_CONFIG: run_sysaction(CHF_SYSACT_RESET_CONFIG); update_status(); return 0;
        case IDM_CONTROLLERS: chf_controllers_window_toggle(hwnd, g_session); return 0;
        case IDM_DEBUGGER: chf_debugger_toggle(hwnd, g_session); return 0;
        case IDM_FULLSCREEN: toggle_fullscreen(hwnd); return 0;
        case IDM_TV_ASPECT:
            g_aspect = !g_aspect;
            chfsession_set_int(g_session, "aspect", g_aspect ? 0 : 1);
            chf_display_set_options(g_aspect, g_smooth);
            sync_menu_checks();
            return 0;
        case IDM_SMOOTH:
            g_smooth = !g_smooth;
            chfsession_set_int(g_session, "smooth", g_smooth);
            chf_display_set_options(g_aspect, g_smooth);
            sync_menu_checks();
            return 0;
        case IDM_SETTINGS: show_prefs(); return 0;
        case IDM_FUJINET_LOG: show_log(); return 0;
        case IDM_FUJINET_CONFIG: open_web_ui(hwnd); return 0;
        case IDM_HELP: chf_help_for(hwnd); return 0;
        case IDM_ABOUT: show_about(); return 0;
        case IDM_EXIT: PostMessage(hwnd, WM_CLOSE, 0, 0); return 0;
        default: break;
        }
        break;

    case WM_HELP:
        /* the Help button of the import chooser's message boxes (F1 is the console's) */
        if (g_importing) { chf_help_show(hwnd, "import-sd"); return TRUE; }
        break;

    case WM_DROPFILES: {
        const UINT n = DragQueryFileA((HDROP)wp, 0xFFFFFFFFu, NULL, 0);
        UINT i;
        for (i = 0; i < n; i++) {
            char path[MAX_PATH];
            if (DragQueryFileA((HDROP)wp, i, path, sizeof path)) load_media(path);
        }
        DragFinish((HDROP)wp);
        return 0;
    }

    /* a second launch hands its file to this one */
    case WM_COPYDATA: {
        const COPYDATASTRUCT *cd = (const COPYDATASTRUCT *)lp;
        if (cd && cd->dwData == 1 && cd->lpData && cd->cbData > 1 && cd->cbData < MAX_PATH) {
            char path[MAX_PATH];
            memcpy(path, cd->lpData, cd->cbData);
            path[sizeof path - 1] = '\0';
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            load_media(path);
        }
        return TRUE;
    }

    case WM_CLOSE:
        chf_prefs_abandon();
        break;
    case WM_DESTROY:
        KillTimer(hwnd, IDT_STATUS);
        KillTimer(hwnd, IDT_SYSACT);
        PostQuitMessage(0);
        return 0;
    default: break;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

/* The path on the command line, quotes and all. */
static int command_line_path(const char *cmdline, char *out, size_t outsz)
{
    const char *p = cmdline;
    size_t n;
    if (!cmdline || !*cmdline) return 0;
    if (*p == '"') p++;
    snprintf(out, outsz, "%s", p);
    n = strlen(out);
    while (n && (out[n - 1] == '"' || out[n - 1] == ' ')) out[--n] = '\0';
    return out[0] != '\0';
}

/* One copy of the app: a second launch brings the first forward and hands it
 * the file it was started with. 1 when this process should exit. */
static int hand_over_to_running_instance(const char *path)
{
    HANDLE mutex = CreateMutexA(NULL, FALSE, INSTANCE_MUTEX);
    HWND other;
    if (!mutex || GetLastError() != ERROR_ALREADY_EXISTS)
        return 0;
    other = FindWindowA(WIN_CLASS, NULL);
    if (!other)
        return 0;
    if (path && path[0]) {
        COPYDATASTRUCT cd;
        cd.dwData = 1;
        cd.cbData = (DWORD)strlen(path) + 1;
        cd.lpData = (void *)path;
        SendMessageA(other, WM_COPYDATA, (WPARAM)g_hwnd, (LPARAM)&cd);
    }
    if (IsIconic(other)) ShowWindow(other, SW_RESTORE);
    SetForegroundWindow(other);
    return 1;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    WNDCLASSEX wc;
    MSG msg;
    chfsession_start_opts opts;
    RECT want;
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_BAR_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES };
    int parts[2] = { DOT_PART_W, -1 };
    char media[MAX_PATH];
    const char *later = NULL;   /* a non-cartridge path from the command line */
    int have_media, dpi;
    (void)prev;

    have_media = command_line_path(cmdline, media, sizeof media);
    if (hand_over_to_running_instance(have_media ? media : NULL))
        return 0;

    InitCommonControlsEx(&icc);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);     /* the folder browser */

    g_session = chfsession_new(NULL);
    if (!g_session) {
        MessageBoxA(NULL, "Could not create the session (unusable config or data directories?)",
                    APP_TITLE, MB_ICONERROR | MB_OK);
        return 1;
    }
    g_accent_brush = CreateSolidBrush(RGB((CHFSESSION_ACCENT_RGB >> 16) & 0xff,
                                          (CHFSESSION_ACCENT_RGB >> 8) & 0xff,
                                          CHFSESSION_ACCENT_RGB & 0xff));

    /* "aspect": 0 (the default) the television's pixel aspect, 1 square --
     * the family's meaning, shared with the other frontends */
    g_aspect = chfsession_get_int(g_session, "aspect", 0) == 0;
    g_smooth = chfsession_get_int(g_session, "smooth", 0);

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = WIN_CLASS;
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(IDI_APPICON));
    wc.hIconSm = wc.hIcon;
    RegisterClassEx(&wc);

    /* Three times the console's picture in height, at the chosen shape
     * (792 x 696 at the TV's 4:3 for the NTSC machines), plus chrome, at the
     * screen's DPI. */
    dpi = chf_system_dpi();
    {
        int num = 4, den = 3;
        chfsession_display_aspect(chfsession_get_int(g_session, "machine", 0), !g_aspect, &num, &den);
        SetRect(&want, 0, 0,
                chf_scale((int)((double)CHFSESSION_FB_HEIGHT * 3 * num / den + 0.5), dpi),
                chf_scale(CHFSESSION_FB_HEIGHT * 3, dpi));
    }
    AdjustWindowRectEx(&want, WS_OVERLAPPEDWINDOW, TRUE, WS_EX_ACCEPTFILES);
    g_hwnd = CreateWindowEx(WS_EX_ACCEPTFILES, WIN_CLASS, APP_TITLE, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, want.right - want.left,
                            want.bottom - want.top + chf_scale(24, dpi), NULL, NULL, inst, NULL);
    if (!g_hwnd) return 1;
    build_menu(g_hwnd);
    g_statusbar = CreateWindowExA(0, STATUSCLASSNAMEA, "", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                  0, 0, 0, 0, g_hwnd, (HMENU)(INT_PTR)IDC_STATUSBAR, inst, NULL);
    {
        int scaled[2] = { chf_scale(parts[0], chf_window_dpi(g_hwnd)), -1 };
        SendMessageA(g_statusbar, SB_SETPARTS, 2, (LPARAM)scaled);
    }
    chf_help_set_context(g_hwnd, "main", 0);       /* F1 is the console's RESET here */
    g_display = chf_display_create(g_hwnd, g_session);
    chf_display_set_options(g_aspect, g_smooth);
    layout_children();
    ShowWindow(g_hwnd, show);

    chfsession_default_opts(g_session, &opts);
    if (have_media) {
        /* a cartridge boots in place of the remembered one; anything
         * else is routed like a dropped file once the machine is up */
        if (chfsession_media_is_cartridge(media))
            opts.cart_path = media;
        else
            later = media;
    }
    if (chfsession_start(g_session, &opts) != 0)
        chf_message(g_hwnd, chfsession_last_error(g_session), APP_TITLE, MB_ICONWARNING | MB_OK);
    else if (opts.cart_path && !*chfsession_cart_path(g_session))
        later = media;      /* the session booted CONFIG instead: open_cart says why */
    if (later) load_media(later);

    g_pad_generation = chfsession_gamepad_generation(g_session);
    SetTimer(g_hwnd, IDT_STATUS, 1000, NULL);
    SetTimer(g_hwnd, IDT_SYSACT, 100, NULL);
    update_status();

    if (env_on("CHF_OPEN_CONTROLLERS")) chf_controllers_window_toggle(g_hwnd, g_session);
    if (env_on("CHF_OPEN_DEBUGGER")) chf_debugger_show(g_hwnd, g_session);
    if (env_on("CHF_OPEN_SETTINGS")) show_prefs();

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        /* Before TranslateMessage, so the sub-windows' keys reach them
         * regardless of which child control has the focus. */
        if (chf_prefs_pretranslate(&msg)) continue;     /* first: it may be capturing a key */
        if (chf_help_pretranslate(&msg)) continue;
        if (chf_debugger_pretranslate(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    chf_display_destroy();
    chfsession_stop(g_session);
    chfsession_free(g_session);
    DeleteObject(g_accent_brush);
    CoUninitialize();
    return (int)msg.wParam;
}
