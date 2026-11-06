/*
 * roms -- the BIOS the Channel F console runs and this app does not ship: what
 * MAME knows, what the user imported, and what a developer build embedded.
 *
 * CONFIG and every FujiNet app boot on this app's own stand-in BIOS
 * (core/chf/stub.c); a commercial Videocart calls the console's routines and
 * runs only on the real ones. The real BIOS is two 1K ROMs, copyrighted
 * firmware which this project does not redistribute (see COMPLIANCE.md): the
 * first half at $0000 comes in two versions (sl31253 in the Channel F and the
 * Videoplay, sl90025 in the System II and the Videoplay 2), sl31254 at $0400
 * is the same in every console. "Import BIOS..." copies a user's own dumps
 * into the ROM directory, each recognised by size and CRC-32 against MAME's
 * table (src/mame/fairchild/channelf.cpp) whatever the file is called, and
 * stored under MAME's file name.
 *
 * The input is a file, a zip (MAME's channelf.zip works as it is) or a folder
 * searched eight levels deep; a zip found in a folder is opened too. A 2K file
 * that is a known first half followed by sl31254 is split in two. A 1K file
 * whose CRC is not in the table is never taken: it could be either first half
 * (or neither), and a wrong guess is a console that will not boot a cartridge.
 *
 * WITH_CHF_ROMS=ON embeds the recognised images a developer put in
 * tools/roms/local/ for local development; published builds are built with it
 * OFF and core/tests/no_embedded_roms.py checks that claim against the
 * shipped binaries.
 *
 * Ported from the O2 sibling's roms.c and the ColecoVision sibling's walk.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "roms_embedded.h"
#include "session_internal.h"
#include "zipread.h"

/* The directory walk's depth limit: folders nested this many levels below the
 * one the user chose are searched; deeper ones are not. */
#define MAX_DEPTH 8
/* A zip inside a zip inside a folder is as far as anyone sensibly goes. */
#define MAX_ZIP_DEPTH 2
#define HALF 0x400u
#define MAX_ROM_SIZE (2 * HALF)
#define MAX_NESTED_ZIP (8u * 1024 * 1024)

static const chf_rom_item s_items[CHF_ROM_COUNT] = {
    [CHF_ROM_SL31253] = { "Channel F BIOS, first half (sl31253.rom)", "Console",
                          "sl31253.rom", HALF, 0x04694ed9u },
    [CHF_ROM_SL90025] = { "Channel F System II BIOS, first half (sl90025.rom)", "Console",
                          "sl90025.rom", HALF, 0x015c1e38u },
    [CHF_ROM_SL31254] = { "Channel F BIOS, second half (sl31254.rom)", "Console",
                          "sl31254.rom", HALF, 0x9c047ba3u },
};

int chfsession_rom_item_count(void)
{
    return CHF_ROM_COUNT;
}

const chf_rom_item *chfsession_rom_item(int i)
{
    return i >= 0 && i < CHF_ROM_COUNT ? &s_items[i] : NULL;
}

/* ---- the files ---------------------------------------------------------------- */

static void rom_path(const struct chfsession *s, int id, char *out, size_t n)
{
    snprintf(out, n, "%s/%s", s->roms_dir, s_items[id].file);
}

static const chf_embedded_rom *embedded(int id)
{
    for (unsigned i = 0; i < chf_embedded_rom_count; i++)
        if (strcmp(chf_embedded_roms[i].file, s_items[id].file) == 0
            && chf_embedded_roms[i].size == s_items[id].size)
            return &chf_embedded_roms[i];
    return NULL;
}

/* The whole file, if it is exactly `want` bytes; NULL otherwise. */
static uint8_t *read_file(const char *path, uint32_t want, uint32_t *size)
{
    FILE *f = path && *path ? fopen(path, "rb") : NULL;
    uint8_t *buf;
    size_t got;

    if (!f)
        return NULL;
    buf = malloc((size_t)want + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    got = fread(buf, 1, (size_t)want + 1, f);
    fclose(f);
    if (got != want) {
        free(buf);
        return NULL;
    }
    *size = (uint32_t)got;
    return buf;
}

uint8_t *roms_load(struct chfsession *s, int id, uint32_t *size)
{
    char path[CHF_PATH_MAX];
    uint8_t *buf;
    const chf_embedded_rom *e;

    if (id < 0 || id >= CHF_ROM_COUNT)
        return NULL;
    rom_path(s, id, path, sizeof path);
    buf = read_file(path, s_items[id].size, size);
    if (buf)
        return buf;
    e = embedded(id);
    if (e) {
        buf = malloc(e->size);
        if (!buf)
            return NULL;
        memcpy(buf, e->data, e->size);
        *size = e->size;
        return buf;
    }
    return NULL;
}

int chfsession_rom_item_available(chfsession *s, int i)
{
    char path[CHF_PATH_MAX];
    struct stat st;

    if (!s || i < 0 || i >= CHF_ROM_COUNT)
        return 0;
    if (embedded(i))
        return 1;
    rom_path(s, i, path, sizeof path);
    return stat(path, &st) == 0 && S_ISREG(st.st_mode)
        && (uint32_t)st.st_size == s_items[i].size;
}

/* Materialise the embedded images into the ROM directory on first run, so a
 * developer build behaves like a configured install. */
void roms_provision_embedded(struct chfsession *s)
{
    for (unsigned i = 0; i < chf_embedded_rom_count; i++) {
        char path[CHF_PATH_MAX];
        FILE *f;

        snprintf(path, sizeof path, "%s/%s", s->roms_dir, chf_embedded_roms[i].file);
        f = fopen(path, "rb");
        if (f) {
            fclose(f);   /* the user's own copy wins */
            continue;
        }
        f = fopen(path, "wb");
        if (!f)
            continue;
        fwrite(chf_embedded_roms[i].data, 1, chf_embedded_roms[i].size, f);
        fclose(f);
    }
}

/* ---- which BIOS runs ---------------------------------------------------------- */

const char *chf_bios_choice_name(int choice)
{
    switch (choice) {
    case CHF_BIOS_AUTO:    return "Automatic";
    case CHF_BIOS_SL31253: return "Channel F (sl31253)";
    case CHF_BIOS_SL90025: return "Channel F System II (sl90025)";
    case CHF_BIOS_STANDIN: return "The app's stand-in";
    default:               return "";
    }
}

/* The first half a machine carries: the Channel F and the Videoplay have
 * sl31253, the System II and the Videoplay 2 sl90025. */
static int own_half(int machine)
{
    return machine == CHF_MACHINE_CHANNELF2 || machine == CHF_MACHINE_VIDEOPLAY2
        ? CHF_ROM_SL90025 : CHF_ROM_SL31253;
}

int roms_bios_for(struct chfsession *s, int machine, int choice)
{
    const int own = own_half(machine);
    const int other = own == CHF_ROM_SL31253 ? CHF_ROM_SL90025 : CHF_ROM_SL31253;

    if (!chfsession_rom_item_available(s, CHF_ROM_SL31254))
        return -1;
    switch (choice) {
    case CHF_BIOS_SL31253:
        return chfsession_rom_item_available(s, CHF_ROM_SL31253) ? CHF_ROM_SL31253 : -1;
    case CHF_BIOS_SL90025:
        return chfsession_rom_item_available(s, CHF_ROM_SL90025) ? CHF_ROM_SL90025 : -1;
    case CHF_BIOS_STANDIN:
        return -1;
    default:
        break;
    }
    if (chfsession_rom_item_available(s, own))
        return own;
    return chfsession_rom_item_available(s, other) ? other : -1;
}

int chfsession_have_bios(chfsession *s, int machine)
{
    if (machine < 0 || machine >= CHF_MACHINE_COUNT)
        machine = CHF_MACHINE_CHANNELF;
    return roms_bios_for(s, machine, chfsession_get_int(s, "bios", CHF_BIOS_AUTO)) >= 0;
}

int chfsession_bios_in_use(chfsession *s)
{
    return s->running ? s->run_bios : -1;
}

/* ---- recognition ---------------------------------------------------------------- */

enum { NOT_A_ROM = -1 };

/* The item (a chf_rom_id) for a 1K image, or NOT_A_ROM. By size and CRC-32
 * only: never the name. */
static int identify(uint32_t size, uint32_t crc)
{
    int i;

    for (i = 0; i < CHF_ROM_COUNT; i++)
        if (s_items[i].size == size && s_items[i].crc == crc)
            return i;
    return NOT_A_ROM;
}

/* Sizes worth reading at all, so a sweep does not slurp every file it meets:
 * a 1K ROM, or a 2K first half + sl31254. */
static int size_wanted(uint32_t size)
{
    return size == HALF || size == 2 * HALF;
}

/* ---- the import ------------------------------------------------------------------ */

typedef struct {
    struct chfsession *s;
    int probe;                  /* media_is_rom: look, do not write */
    int found;                  /* probe: something recognised */
    int got[CHF_ROM_COUNT];
    int imported;               /* distinct items written */
    int split;                  /* a 2K file was split */
    int write_failed;
    char write_error[CHF_PATH_MAX + 48];
    char first_dest[CHF_PATH_MAX];
    /* the last candidate looked at, for "what is this file" */
    uint32_t last_size, last_crc;
    int last_valid;             /* last_crc was computed */
    int plain_file;             /* the chosen path is one file, not a zip or folder */
} import_ctx;

/* Written through a .part file and renamed, so a failed write leaves the old
 * ROM in place. */
static int write_rom(import_ctx *ctx, int id, const uint8_t *data, uint32_t size)
{
    char path[CHF_PATH_MAX], tmp[CHF_PATH_MAX + 8];
    FILE *f;
    int ok;

    if (ctx->got[id])
        return 1;               /* identical bytes (same CRC): already done */
    paths_mkdir_p(ctx->s->roms_dir);
    rom_path(ctx->s, id, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.part", path);
    f = fopen(tmp, "wb");
    if (!f) {
        snprintf(ctx->write_error, sizeof ctx->write_error, "Cannot write %s", path);
        ctx->write_failed = 1;
        return 0;
    }
    ok = fwrite(data, 1, size, f) == size;
    if (fclose(f) != 0)
        ok = 0;
    if (ok) {
#if defined(_WIN32)
        remove(path);           /* rename() will not replace a file here */
#endif
        ok = rename(tmp, path) == 0;
    }
    if (!ok) {
        remove(tmp);
        snprintf(ctx->write_error, sizeof ctx->write_error, "Cannot write %s", path);
        ctx->write_failed = 1;
        return 0;
    }
    ctx->got[id] = 1;
    ctx->imported++;
    if (!ctx->first_dest[0])
        snprintf(ctx->first_dest, sizeof ctx->first_dest, "%s", path);
    return 1;
}

/* One candidate image. */
static void take(import_ctx *ctx, const uint8_t *data, uint32_t size)
{
    const uint32_t crc = zipread_crc32(data, size);

    ctx->last_size = size;
    ctx->last_crc = crc;
    ctx->last_valid = 1;
    if (size == HALF) {
        const int id = identify(size, crc);
        if (id >= 0) {
            ctx->found = 1;
            if (!ctx->probe)
                write_rom(ctx, id, data, size);
        }
        return;
    }
    if (size == 2 * HALF) {
        /* a first half followed by sl31254, both known */
        const int lo = identify(HALF, zipread_crc32(data, HALF));
        const int hi = identify(HALF, zipread_crc32(data + HALF, HALF));

        if ((lo == CHF_ROM_SL31253 || lo == CHF_ROM_SL90025) && hi == CHF_ROM_SL31254) {
            ctx->found = 1;
            if (!ctx->probe) {
                write_rom(ctx, lo, data, HALF);
                write_rom(ctx, hi, data + HALF, HALF);
                ctx->split = 1;
            }
        }
    }
}

static int name_is_zip(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && (strcmp(name + n - 4, ".zip") == 0 || strcmp(name + n - 4, ".ZIP") == 0
                     || strcmp(name + n - 4, ".Zip") == 0);
}

static void import_zip(import_ctx *ctx, zipread *z, int zdepth)
{
    int i;

    for (i = 0; i < zipread_count(z) && !(ctx->probe && ctx->found); i++) {
        uint32_t size = zipread_size(z, i);
        uint8_t *data = NULL;
        const char *name = zipread_name(z, i);

        if (name_is_zip(name) && zdepth < MAX_ZIP_DEPTH && size <= MAX_NESTED_ZIP) {
            if (zipread_extract(z, i, &data, &size, MAX_NESTED_ZIP) == 0) {
                zipread *inner = zipread_open_memory(data, size);
                free(data);
                if (inner) {
                    import_zip(ctx, inner, zdepth + 1);
                    zipread_close(inner);
                }
            }
            continue;
        }
        if (!size_wanted(size))
            continue;
        if (zipread_extract(z, i, &data, &size, MAX_ROM_SIZE) == 0) {
            take(ctx, data, size);
            free(data);
        }
    }
}

static void import_path(import_ctx *ctx, const char *path, int depth);

static void import_dir(import_ctx *ctx, const char *dir, int depth)
{
    DIR *d = opendir(dir);
    struct dirent *e;

    if (!d)
        return;
    while ((e = readdir(d)) != NULL && !(ctx->probe && ctx->found)) {
        char path[CHF_PATH_MAX];

        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        import_path(ctx, path, depth + 1);
    }
    closedir(d);
}

/* `depth` is how many folders below the chosen one this path is (0 for the
 * chosen path itself). */
static void import_path(import_ctx *ctx, const char *path, int depth)
{
    struct stat st;
    zipread *z;

    if (stat(path, &st) != 0)
        return;
    if (S_ISDIR(st.st_mode)) {
        if (depth <= MAX_DEPTH && !ctx->probe)
            import_dir(ctx, path, depth);
        return;
    }
    if (!S_ISREG(st.st_mode))
        return;

    z = zipread_open(path);
    if (z) {
        import_zip(ctx, z, 0);
        zipread_close(z);
        return;
    }
    if (depth == 0)
        ctx->plain_file = 1;
    ctx->last_size = (uint32_t)st.st_size;
    ctx->last_valid = 0;
    if (size_wanted((uint32_t)st.st_size)) {
        uint32_t size = 0;
        uint8_t *data = read_file(path, (uint32_t)st.st_size, &size);
        if (data) {
            take(ctx, data, size);
            free(data);
        }
    }
}

static const char *base_of(const char *path)
{
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    return slash ? slash + 1 : path;
}

#if defined(__GNUC__)
static void say(char *msg, int msgsz, int *len, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
#endif
static void say(char *msg, int msgsz, int *len, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (!msg || msgsz <= 0 || *len >= msgsz - 1)
        return;
    va_start(ap, fmt);
    n = vsnprintf(msg + *len, (size_t)(msgsz - *len), fmt, ap);
    va_end(ap);
    if (n > 0)
        *len += n < msgsz - *len ? n : msgsz - *len - 1;
}

static void fail(struct chfsession *s, char *msg, int msgsz, const char *text)
{
    session_set_error(s, "%s", text);
    if (msg && msgsz > 0)
        snprintf(msg, (size_t)msgsz, "%s", text);
}

int roms_import_ex(struct chfsession *s, const char *path, char *msg, int msgsz,
                   char *first_dest, int destsz)
{
    import_ctx *ctx;
    struct stat st;
    char abs[CHF_PATH_MAX];
    int len = 0, i, n, rc;

    if (msg && msgsz > 0)
        msg[0] = '\0';
    if (first_dest && destsz > 0)
        first_dest[0] = '\0';
    if (!s || !path || !*path) {
        fail(s, msg, msgsz, "No file to import");
        return -1;
    }
    path = paths_resolve(path, abs, sizeof abs);
    if (stat(path, &st) != 0) {
        char text[CHF_PATH_MAX + 32];
        snprintf(text, sizeof text, "Cannot read %s", path);
        fail(s, msg, msgsz, text);
        return -1;
    }

    ctx = calloc(1, sizeof *ctx);
    if (!ctx) {
        fail(s, msg, msgsz, "Out of memory");
        return -1;
    }
    ctx->s = s;
    import_path(ctx, path, 0);

    if (ctx->write_failed) {
        fail(s, msg, msgsz, ctx->write_error);
        free(ctx);
        return -1;
    }

    if (ctx->imported > 0) {
        say(msg, msgsz, &len, "Imported ");
        n = 0;
        for (i = 0; i < CHF_ROM_COUNT; i++)
            if (ctx->got[i])
                say(msg, msgsz, &len, "%s%s", n++ ? ", " : "", s_items[i].file);
        say(msg, msgsz, &len, ".");
        /* what the console still lacks, counting what was there before */
        if (!chfsession_rom_item_available(s, CHF_ROM_SL31254))
            say(msg, msgsz, &len, " The BIOS also needs sl31254.rom (the second half).");
        if (!chfsession_rom_item_available(s, CHF_ROM_SL31253)
            && !chfsession_rom_item_available(s, CHF_ROM_SL90025))
            say(msg, msgsz, &len, " It also needs a first half: sl31253.rom or sl90025.rom.");
        say(msg, msgsz, &len, " Takes effect at the next power cycle (Reset to CONFIG).");
    } else if (ctx->plain_file) {
        say(msg, msgsz, &len, "%s is not a ROM this app imports (%u bytes",
            base_of(path), ctx->last_size);
        if (ctx->last_valid)
            say(msg, msgsz, &len, ", CRC-32 %08X", ctx->last_crc);
        say(msg, msgsz, &len,
            "). The console BIOS is two 1,024-byte ROMs, a first half (sl31253 or sl90025) "
            "and sl31254: MAME's channelf.zip. An unrecognised 1,024-byte file is never "
            "taken, because it could be either first half.");
    } else {
        say(msg, msgsz, &len, "No Channel F BIOS ROM that this app uses was found in %s.", path);
    }

    if (first_dest && destsz > 0)
        snprintf(first_dest, (size_t)destsz, "%s", ctx->first_dest);
    rc = ctx->imported;
    free(ctx);
    return rc;
}

int chfsession_rom_import(chfsession *s, const char *path, char *msg, int msgsz)
{
    return roms_import_ex(s, path, msg, msgsz, NULL, 0);
}

int chfsession_media_is_rom(const char *path)
{
    import_ctx *ctx;
    struct stat st;
    char abs[CHF_PATH_MAX];
    int found;

    if (!path || !*path)
        return 0;
    path = paths_resolve(path, abs, sizeof abs);
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;
    ctx = calloc(1, sizeof *ctx);
    if (!ctx)
        return 0;
    ctx->probe = 1;             /* an unknown 1K file is a cartridge (or nothing) */
    import_path(ctx, path, 0);
    found = ctx->found;
    free(ctx);
    return found;
}

const char *chfsession_roms_path(const chfsession *s)
{
    return s->roms_dir;
}
