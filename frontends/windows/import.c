/*
 * Choosing cartridges: the common file dialog (several files at once for the
 * import to SD), each path through the session, then one message box saying
 * what came of it. The console has no BIOS, so there is nothing else to import.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "import.h"

#include <commdlg.h>
#include <stdio.h>
#include <string.h>

#include "win_common.h"

static const char k_cart_filter[] =
    "Channel F cartridges (*.bin;*.rom;*.chf;*.zip)\0*.bin;*.rom;*.chf;*.zip\0All files\0*.*\0\0";

/* A GetOpenFileName multi-select buffer is "dir\0name\0name\0\0", or one full
 * path when a single file was chosen. Calls fn for each full path; 0 when the
 * chooser was cancelled. */
static int pick_each(HWND owner, const char *filter, const char *title,
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
                OFN_ALLOWMULTISELECT;
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
    if (!pick_each(owner, k_cart_filter, "Import Cartridge to SD", import_cart, &l))
        return -1;
    if (l.failed) {
        if (l.ok)
            snprintf(note, sizeof note, "%d imported, %d failed.\n%s", l.ok, l.failed, l.errors);
        else
            snprintf(note, sizeof note, "%s", l.errors);
        chf_message(owner, note, "Import failed", MB_ICONWARNING | MB_OK);
    } else if (l.ok == 1) {
        snprintf(note, sizeof note, "%s is on the SD host. Boot it from the CONFIG client.", l.first_name);
        chf_message(owner, note, "Imported", MB_ICONINFORMATION | MB_OK);
    } else if (l.ok > 1) {
        snprintf(note, sizeof note, "%d files are on the SD host. Boot them from the CONFIG client.", l.ok);
        chf_message(owner, note, "Imported", MB_ICONINFORMATION | MB_OK);
    }
    return l.ok;
}
