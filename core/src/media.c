/*
 * media -- routing a file the user dropped on the window to the right place.
 *
 * Two destinations:
 *
 *   Cartridges (.bin, .rom, .chf, or a .zip of one) go to the cartridge
 *   directory and are opened from there (load_cart plans every image itself).
 *
 *   Anything else goes to the FujiNet SD folder, because FujiNet is what
 *   serves it.
 *
 * Ported from the Studio II sibling's media.c.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "session_internal.h"

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    return slash ? slash + 1 : path;
}

static int ext_is(const char *path, const char *const *exts)
{
    const char *dot = strrchr(base_name(path), '.');
    int i;
    if (!dot) return 0;
    for (i = 0; exts[i]; i++) {
        const char *a = dot + 1, *b = exts[i];
        while (*a && *b) {
            char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
            if (ca != *b) break;
            a++; b++;
        }
        if (!*a && !*b) return 1;
    }
    return 0;
}

/* What a cartridge file is called (MAME's slot takes bin and rom; arc is the
 * emulators' own; a zip holds one). The extension only routes the file: load_cart identifies
 * every image by its contents. */
static const char *const cart_exts[] = { "bin", "rom", "chf", "zip", NULL };

static int is_zip(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && (!strcmp(name + n - 4, ".zip") || !strcmp(name + n - 4, ".ZIP"));
}

static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out;
    char buf[16384];
    size_t n;

    if (!in) return -1;
    out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            return -1;
        }
    }
    fclose(in);
    if (fclose(out) != 0) return -1;
    return 0;
}

int chfsession_import_media(chfsession *s, const char *src_path,
                            char *dest_out, int dest_sz)
{
    const char *name;
    const char *dir;
    struct stat st;
    char abs[CHF_PATH_MAX];

    if (!src_path || !*src_path) {
        session_set_error(s, "No file to import");
        return -1;
    }
    src_path = paths_resolve(src_path, abs, sizeof abs);
    name = base_name(src_path);

    if (ext_is(src_path, cart_exts)) {
        dir = s->carts_dir;
    } else {
        /* The SD tree only exists once the FujiNet runtime has been
         * provisioned. Test the DIRECTORY, not just the path string: the
         * path is always computed, so a string check passes and the copy
         * then fails with "could not copy", which tells the user nothing
         * about the actual problem. */
        if (!s->fujinet_sd[0] ||
            stat(s->fujinet_sd, &st) != 0 || !S_ISDIR(st.st_mode)) {
            session_set_error(s,
                "%s is not a cartridge, so FujiNet would serve it -- but the "
                "FujiNet runtime is not available, so there is nowhere to "
                "put it.", name);
            return -1;
        }
        dir = s->fujinet_sd;
    }

    snprintf(dest_out, (size_t)dest_sz, "%s/%s", dir, name);
    if (copy_file(src_path, dest_out) != 0) {
        session_set_error(s, "Could not copy %s to %s", name, dir);
        return -1;
    }
    return 0;
}

int chfsession_media_is_cartridge(const char *path)
{
    return path ? ext_is(path, cart_exts) : 0;
}

/* Into the SD root, so CONFIG shows it at the top of the SD host with no
 * navigation. FujiNet pushes cartridge files to the cartridge, which plans
 * whatever its chfmap can. */
int chfsession_import_cart_to_sd(chfsession *s, const char *src_path,
                                 char *dest_out, int dest_sz)
{
    const char *name;
    char abs[CHF_PATH_MAX];
    struct stat st;

    if (!src_path || !*src_path) {
        session_set_error(s, "No file to import");
        return -1;
    }
    src_path = paths_resolve(src_path, abs, sizeof abs);
    name = base_name(src_path);
    if (!s->fujinet_sd[0] ||
        stat(s->fujinet_sd, &st) != 0 || !S_ISDIR(st.st_mode)) {
        session_set_error(s,
            "The FujiNet runtime is not available, so there is no SD folder "
            "to import %s into.", name);
        return -1;
    }
    {
        char why[256];
        if (!session_cart_plan_check(src_path, why, sizeof why)) {
            session_set_error(s, "%s: %s", name, why);
            return -1;
        }
    }
    if (is_zip(name)) {
        /* a zip of one cartridge: unpacked to <zip name>.bin */
        uint8_t *img;
        uint32_t len = 0;
        char why[256], stem[256];
        FILE *out;
        size_t n = strlen(name) - 4;

        img = session_read_cart(src_path, &len, why, sizeof why);
        if (!img) {
            session_set_error(s, "%s", why);
            return -1;
        }
        if (n >= sizeof stem - 5) n = sizeof stem - 5;
        memcpy(stem, name, n);
        stem[n] = '\0';
        snprintf(dest_out, (size_t)dest_sz, "%s/%s.bin", s->fujinet_sd, stem);
        out = fopen(dest_out, "wb");
        if (!out || fwrite(img, 1, len, out) != len) {
            if (out) fclose(out);
            free(img);
            session_set_error(s, "Could not write %s", dest_out);
            return -1;
        }
        free(img);
        return fclose(out) == 0 ? 0 : -1;
    }
    snprintf(dest_out, (size_t)dest_sz, "%s/%s", s->fujinet_sd, name);
    if (copy_file(src_path, dest_out) != 0) {
        session_set_error(s, "Could not copy %s to %s", name, s->fujinet_sd);
        return -1;
    }
    return 0;
}
