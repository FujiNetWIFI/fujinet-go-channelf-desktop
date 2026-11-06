/*
 * Choosing and importing cartridges and the BIOS: the common file dialog
 * (several files at once) or the folder browser, each path through the
 * session, then one message box saying what came of it. Also Open SD Folder.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "import.h"

#include <commdlg.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

#include "win_common.h"

static const char k_cart_filter[] =
    "Channel F cartridges (*.bin;*.chf;*.zip)\0*.bin;*.chf;*.zip\0All files\0*.*\0\0";
static const char k_bios_filter[] =
    "BIOS images and ROM sets (*.zip;*.rom;*.bin)\0*.zip;*.rom;*.bin\0All files\0*.*\0\0";

UINT chf_import_help_msg(void)
{
    static UINT msg;
    if (!msg) msg = RegisterWindowMessageA(HELPMSGSTRING);
    return msg;
}

/* A GetOpenFileName multi-select buffer is "dir\0name\0name\0\0", or one full
 * path when a single file was chosen. Calls fn for each full path; 0 when the
 * chooser was cancelled. help: a Help button that tells the owner. */
static int pick_each(HWND owner, const char *filter, const char *title, int help,
                     void (*fn)(const char *path, void *ctx), void *ctx)
{
    static char buf[64 * 1024];
    OPENFILENAMEA ofn;
    const char *p;
    memset(&ofn, 0, sizeof ofn);
    buf[0] = buf[1] = '\0';
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = sizeof buf;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER |
                OFN_ALLOWMULTISELECT | (help ? OFN_SHOWHELP : 0);
    if (help) chf_import_help_msg();
    if (!GetOpenFileNameA(&ofn))
        return 0;
    p = buf + strlen(buf) + 1;
    if (!*p) {
        fn(buf, ctx);
        return 1;
    }
    for (; *p; p += strlen(p) + 1) {
        char path[2048];
        snprintf(path, sizeof path, "%.1000s\\%.1000s", buf, p);
        fn(path, ctx);
    }
    return 1;
}

static int pick_folder(HWND owner, const char *title, char *path, DWORD pathsz)
{
    BROWSEINFOA bi;
    PIDLIST_ABSOLUTE pidl;
    char dir[MAX_PATH] = "";
    memset(&bi, 0, sizeof bi);
    bi.hwndOwner = owner;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    pidl = SHBrowseForFolderA(&bi);
    if (!pidl)
        return 0;
    if (!SHGetPathFromIDListA(pidl, dir))
        dir[0] = '\0';
    CoTaskMemFree(pidl);
    snprintf(path, pathsz, "%s", dir);
    return path[0] != '\0';
}

int chf_pick_cart(HWND owner, const char *title, char *path, DWORD pathsz)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    path[0] = '\0';
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = k_cart_filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = pathsz;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
}

/* ---- the BIOS ------------------------------------------------------------------ */

typedef struct {
    chfsession *session;
    char log[6144];
    int imported;
    int refused;           /* paths the session refused outright */
} import_log;

static void import_one(const char *path, void *ctx)
{
    import_log *l = ctx;
    char msg[1024];
    size_t len = strlen(l->log);
    int rc;
    msg[0] = '\0';
    rc = chfsession_rom_import(l->session, path, msg, sizeof msg);
    if (rc > 0)
        l->imported += rc;
    else if (rc < 0)
        l->refused++;
    if (len < sizeof l->log - 1)
        snprintf(l->log + len, sizeof l->log - len, "%s%s", len ? "\n" : "", msg);
}

static int report_roms(HWND owner, import_log *l)
{
    if (l->log[0])
        chf_message(owner, l->log, l->imported ? "BIOS Imported" : "Nothing Imported",
                    (l->imported ? MB_ICONINFORMATION : MB_ICONWARNING) | MB_OK);
    return l->imported;
}

int chf_import_bios(HWND owner, chfsession *session, int folder)
{
    static import_log l;
    memset(&l, 0, sizeof l);
    l.session = session;
    if (folder) {
        char dir[MAX_PATH];
        if (!pick_folder(owner, "Import BIOS from a Folder (MAME's channelf.zip set works unpacked)",
                         dir, sizeof dir))
            return -1;
        import_one(dir, &l);
    } else if (!pick_each(owner, k_bios_filter, "Import BIOS", 0, import_one, &l)) {
        return -1;
    }
    return report_roms(owner, &l);
}

int chf_import_roms(HWND owner, chfsession *session, const char *const *paths, int n)
{
    static import_log l;
    int i;
    memset(&l, 0, sizeof l);
    l.session = session;
    for (i = 0; i < n; i++)
        import_one(paths[i], &l);
    return report_roms(owner, &l);
}

/* ---- cartridges for the SD host ---------------------------------------------- */

typedef struct {
    chfsession *session;
    int ok, failed;
    char first_name[MAX_PATH];
    char errors[2048];
} cart_log;

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '\\'), *t = strrchr(path, '/');
    if (t && (!s || t > s)) s = t;
    return s ? s + 1 : path;
}

static void import_cart(const char *path, void *ctx)
{
    cart_log *l = ctx;
    char dest[1024];
    dest[0] = '\0';
    if (chfsession_import_cart_to_sd(l->session, path, dest, sizeof dest) == 0) {
        if (!l->ok)
            snprintf(l->first_name, sizeof l->first_name, "%s", base_name(dest[0] ? dest : path));
        l->ok++;
    } else {
        size_t len = strlen(l->errors);
        l->failed++;
        if (len < sizeof l->errors - 1)
            snprintf(l->errors + len, sizeof l->errors - len, "%s%s", len ? "\n" : "",
                     chfsession_last_error(l->session));
    }
}

int chf_import_carts(HWND owner, chfsession *session)
{
    static cart_log l;
    char note[2400];
    memset(&l, 0, sizeof l);
    l.session = session;
    if (!pick_each(owner, k_cart_filter, "Import Cartridge to SD", 1, import_cart, &l))
        return -1;
    if (l.failed) {
        if (l.ok)
            snprintf(note, sizeof note, "%d imported, %d failed.\n%s", l.ok, l.failed, l.errors);
        else
            snprintf(note, sizeof note, "%s", l.errors);
        chf_message(owner, note, "Import failed", MB_ICONWARNING | MB_OK | MB_HELP);
    } else if (l.ok == 1) {
        snprintf(note, sizeof note, "%s is on the SD host. Boot it from the CONFIG client.", l.first_name);
        chf_message(owner, note, "Imported", MB_ICONINFORMATION | MB_OK | MB_HELP);
    } else if (l.ok > 1) {
        snprintf(note, sizeof note, "%d files are on the SD host. Boot them from the CONFIG client.", l.ok);
        chf_message(owner, note, "Imported", MB_ICONINFORMATION | MB_OK | MB_HELP);
    }
    return l.ok;
}

/* ---- the SD folder ------------------------------------------------------------- */

void chf_open_sd_folder(HWND owner, chfsession *session)
{
    const char *sd = chfsession_sd_path(session);
    char path[MAX_PATH * 2];
    snprintf(path, sizeof path, "%s", sd ? sd : "");
    for (char *c = path; *c; c++)
        if (*c == '/') *c = '\\';
    if (!path[0])
        return;
    SHCreateDirectoryExA(NULL, path, NULL);     /* it may not exist yet */
    ShellExecuteA(owner, "explore", path, NULL, NULL, SW_SHOWNORMAL);
}
