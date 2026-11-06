/*
 * media_test -- where a dropped file goes (a BIOS ROM to the ROM directory, a
 * cartridge to the cartridge directory, anything else to the FujiNet SD
 * folder), what makes a cartridge image acceptable (a file, or a zip of
 * exactly one), and Import Cartridge to SD: a flat Videocart of up to 16K
 * that starts with $55 and a FujiNet app go in; Maze, Hangman, Chess and the
 * multi-game cartridges (named by their checksums in MAME's software list)
 * and the images the FujiNet cartridge cannot plan are refused with the
 * reason. No machine is started; no firmware is used.
 *
 * The images are filler. A FujiNet app is a 16K image with the "FUJI" claim
 * where the cartridge's planner looks for it (image offset $3FFC), and a
 * Videocart starts with the $55 the BIOS checks. The softlist cartridges are
 * made by steering the last four bytes of filler to the software list's
 * CRC-32 (CRC-32 is linear): their contents are nobody's.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "chfsession.h"
#include "test_tmpdir.h"
#include "test_zip.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

static void write_bytes(const char *path, const uint8_t *b, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) { printf("FAIL: cannot write %s\n", path); failures++; return; }
    fwrite(b, 1, n, f);
    fclose(f);
}

/* Filler; with `sig`, the $55 a Videocart starts with. */
static void write_filler(const char *path, size_t n, int sig)
{
    uint8_t *b = malloc(n ? n : 1);
    for (size_t i = 0; i < n; i++)
        b[i] = (uint8_t)((i * 7 + 3) & 0xff);
    if (sig && n)
        b[0] = 0x55;
    write_bytes(path, b, n);
    free(b);
}

/* A FujiNet app: 16K that claims the mailbox ("FUJI" at $3FFC), or with
 * `pages` 8K pages after it (an APPBANK image). */
static void write_app(const char *path, unsigned pages)
{
    size_t size = 0x4000 + (size_t)pages * 0x2000;
    uint8_t *b = malloc(size);
    memset(b, 0xea, size);
    b[0] = 0x55;
    memcpy(b + 0x3FFC, "FUJI", 4);
    write_bytes(path, b, size);
    free(b);
}

/* ---- a cartridge the software list names: filler with its CRC-32 ------------- */

static uint32_t T[256];
static uint8_t inv[256];

static void crc_tables(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (~(c & 1) + 1));
        T[i] = c;
        inv[c >> 24] = (uint8_t)i;
    }
}

static void forge(uint8_t *buf, uint32_t size, uint32_t want)
{
    uint32_t r = 0xffffffffu, v = ~want;
    uint8_t idx[4];

    for (uint32_t i = 0; i < size - 4; i++) {
        if (i) buf[i] = (uint8_t)(i * 31 + (i >> 8));
        r = (r >> 8) ^ T[(r ^ buf[i]) & 0xff];
    }
    idx[3] = inv[v >> 24];          v ^= T[idx[3]];
    idx[2] = inv[(v >> 16) & 0xff]; v ^= T[idx[2]] >> 8;
    idx[1] = inv[(v >> 8) & 0xff];  v ^= T[idx[1]] >> 16;
    idx[0] = inv[v & 0xff];
    for (int k = 0; k < 4; k++) {
        buf[size - 4 + k] = (uint8_t)(idx[k] ^ (r & 0xff));
        r = (r >> 8) ^ T[idx[k]];
    }
}

static void write_softlist(const char *path, uint32_t size, uint32_t crc)
{
    uint8_t *b = malloc(size);
    b[0] = 0x55;
    forge(b, size, crc);
    write_bytes(path, b, size);
    free(b);
}

int main(void)
{
    char cfg[400], data[500], src[500], dest[900], sd[900], why[256];
    uint8_t cart[4096], other[4096];
    chfsession_paths p;
    chfsession *s;

    crc_tables();
    test_tmpdir(cfg, sizeof cfg, "mcfg");
    snprintf(data, sizeof data, "%s/data", cfg);
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
    s = chfsession_new(&p);
    if (!s) return 1;

    /* routing by extension */
    check(chfsession_media_is_cartridge("game.bin") && chfsession_media_is_cartridge("game.chf")
          && chfsession_media_is_cartridge("game.zip"), ".bin, .chf and .zip are cartridges");
    check(chfsession_media_is_cartridge("GAME.BIN") && chfsession_media_is_cartridge("Game.CHF"),
          "case-insensitively");
    check(!chfsession_media_is_cartridge("game.rom") && !chfsession_media_is_cartridge("game.st2")
          && !chfsession_media_is_cartridge("game.sms"), "another extension is not (a .rom is a BIOS or nothing)");
    check(!chfsession_media_is_cartridge("notes.txt") && !chfsession_media_is_cartridge("noext")
          && !chfsession_media_is_cartridge(NULL), "nor are .txt, nothing, or NULL");

    /* what makes a cartridge acceptable */
    snprintf(src, sizeof src, "%s/game.bin", cfg);
    write_filler(src, 2048, 1);
    check(chfsession_check_cart(src, why, sizeof why) == 1, "a 2K game plans");
    check(!chfsession_cart_is_app(src), "a game is not a FujiNet app");
    snprintf(src, sizeof src, "%s/game6k.bin", cfg);
    write_filler(src, 6144, 1);
    check(chfsession_check_cart(src, why, sizeof why) == 1 && !chfsession_cart_is_app(src), "a 6K game plans");

    snprintf(src, sizeof src, "%s/app.bin", cfg);
    write_app(src, 0);
    check(chfsession_check_cart(src, why, sizeof why) == 1 && chfsession_cart_is_app(src),
          "a 16K image with the claim is a FujiNet app");
    snprintf(src, sizeof src, "%s/appbank.bin", cfg);
    write_app(src, 6);
    check(chfsession_check_cart(src, why, sizeof why) == 1 && chfsession_cart_is_app(src),
          "and an APPBANK image (16K and six 8K pages)");
    snprintf(src, sizeof src, "%s/noclaim.bin", cfg);
    {
        uint8_t *b = malloc(0x4000 + 0x2000);
        memset(b, 0xea, 0x4000 + 0x2000);
        b[0] = 0x55;
        write_bytes(src, b, 0x4000 + 0x2000);
        free(b);
    }
    check(chfsession_check_cart(src, why, sizeof why) == 1 && !chfsession_cart_is_app(src),
          "pages without the claim are not an app (the slot's own cartridge may still take it)");

    snprintf(src, sizeof src, "%s/huge.bin", cfg);
    write_filler(src, 300000, 1);
    check(chfsession_check_cart(src, why, sizeof why) == 0 && strstr(why, "larger") != NULL,
          "an image past the biggest cartridge is refused, and says why");
    snprintf(src, sizeof src, "%s/empty.bin", cfg);
    write_bytes(src, (const uint8_t *)"", 0);
    check(chfsession_check_cart(src, why, sizeof why) == 0 && strstr(why, "empty") != NULL, "an empty file is refused");
    snprintf(src, sizeof src, "%s/missing.bin", cfg);
    check(chfsession_check_cart(src, why, sizeof why) == 0 && strstr(why, "cannot open") != NULL,
          "a missing file says so");

    /* a zip of exactly one cartridge (MAME's softlist zips) */
    for (int i = 0; i < 4096; i++) { cart[i] = (uint8_t)(i * 5 + 1); other[i] = (uint8_t)(i * 3); }
    cart[0] = 0x55;
    {
        const char *one[] = { "tennis.bin" };
        const uint8_t *od[] = { cart };
        uint32_t os[] = { 2048 };
        const char *two[] = { "a.bin", "b.bin" };
        const uint8_t *td[] = { cart, other };
        uint32_t ts[] = { 2048, 2048 };
        const char *txt[] = { "readme.txt", "tennis.bin" };

        snprintf(src, sizeof src, "%s/one.zip", cfg);
        check(test_zip_write(src, 1, one, od, os), "wrote a zip of one cartridge");
        check(chfsession_check_cart(src, why, sizeof why) == 1 && !chfsession_cart_is_app(src),
              "it plans as a game");
        snprintf(src, sizeof src, "%s/two.zip", cfg);
        check(test_zip_write(src, 2, two, td, ts), "wrote a zip of two");
        check(chfsession_check_cart(src, why, sizeof why) == 0 && strstr(why, "exactly one") != NULL,
              "two cartridges in a zip are refused, with the reason");
        snprintf(src, sizeof src, "%s/readme.zip", cfg);
        check(test_zip_write(src, 2, txt, td, ts), "wrote a zip of a cartridge and a text file");
        check(chfsession_check_cart(src, why, sizeof why) == 1, "the entry with the cartridge extension is the one");
    }

    /* a cartridge drops into the cartridge directory */
    snprintf(src, sizeof src, "%s/spacewar.bin", cfg);
    write_filler(src, 2048, 1);
    check(chfsession_import_media(s, src, dest, sizeof dest) == 0, "a cartridge imports");
    check(strncmp(dest, chfsession_carts_path(s), strlen(chfsession_carts_path(s))) == 0,
          "into the cartridge directory");
    check(exists(dest), "and the copy exists");

    /* a BIOS dump is a ROM whatever its extension: it goes to the ROM directory */
    {
        uint8_t half[1024];
        half[0] = 0x2b;
        forge(half, sizeof half, 0x04694ed9u);
        snprintf(src, sizeof src, "%s/bios-from-somewhere.bin", cfg);
        write_bytes(src, half, sizeof half);
        check(chfsession_media_is_rom(src) && !chfsession_media_is_cartridge(src), "a BIOS dump is a ROM, not a cartridge");
        check(chfsession_import_media(s, src, dest, sizeof dest) == 0
              && strncmp(dest, chfsession_roms_path(s), strlen(chfsession_roms_path(s))) == 0
              && strstr(dest, "sl31253.rom") != NULL && exists(dest), "dropped, it goes to the ROM directory under MAME's name");
        check(chfsession_rom_item_available(s, CHF_ROM_SL31253), "and is there");
        snprintf(src, sizeof src, "%s/notabios.rom", cfg);
        write_filler(src, 1024, 0);
        check(!chfsession_media_is_rom(src), "an unrecognised 1K file is not");
    }

    /* anything else goes to the SD folder, which needs FujiNet's runtime */
    snprintf(src, sizeof src, "%s/notes.txt", cfg);
    write_filler(src, 10, 0);
    check(chfsession_import_media(s, src, dest, sizeof dest) == -1,
          "anything else is refused while there is no SD folder");
    check(strstr(chfsession_last_error(s), "nowhere to put it") != NULL, "with a useful message");

    /* Import Cartridge to SD. The SD folder needs the runtime's
     * provisioning; make it by hand here. */
    snprintf(src, sizeof src, "%s/tennis.bin", cfg);
    write_filler(src, 2048, 1);
    check(chfsession_import_cart_to_sd(s, src, dest, sizeof dest) == -1,
          "SD import refuses when there is no SD folder");
    snprintf(sd, sizeof sd, "%s/fujinet", data); test_mkdir(sd);
    snprintf(sd, sizeof sd, "%s/fujinet/SD", data); test_mkdir(sd);
    check(chfsession_import_cart_to_sd(s, src, dest, sizeof dest) == 0, "SD import copies a flat Videocart into the SD root");
    snprintf(sd, sizeof sd, "%s/fujinet/SD/tennis.bin", data);
    check(strcmp(dest, sd) == 0 && exists(dest), "at the top of the SD tree");
    snprintf(src, sizeof src, "%s/sd16k.bin", cfg);
    write_filler(src, 0x4000, 1);
    check(chfsession_import_cart_to_sd(s, src, dest, sizeof dest) == 0, "so does a 16K one");
    snprintf(src, sizeof src, "%s/appsd.bin", cfg);
    write_app(src, 4);
    check(chfsession_import_cart_to_sd(s, src, dest, sizeof dest) == 0, "an APPBANK app imports to SD too");
    snprintf(src, sizeof src, "%s/appsd16.bin", cfg);
    write_app(src, 0);
    check(chfsession_import_cart_to_sd(s, src, dest, sizeof dest) == 0, "and a 16K app");

    /* what the FujiNet cartridge cannot serve is refused, with the reason */
    snprintf(src, sizeof src, "%s/oddsd.bin", cfg);
    write_filler(src, 0x4000 + 1000, 1);
    check(chfsession_import_cart_to_sd(s, src, dest, sizeof dest) == -1
          && strstr(chfsession_last_error(s), "cannot serve") != NULL
          && strstr(chfsession_last_error(s), "16 KB") != NULL,
          "an image over 16K that is not an app is refused for the SD, with the reason");
    snprintf(src, sizeof src, "%s/nosig.bin", cfg);
    write_filler(src, 2048, 0);
    check(chfsession_import_cart_to_sd(s, src, dest, sizeof dest) == -1
          && strstr(chfsession_last_error(s), "$55") != NULL, "an image without the $55 signature is refused: the BIOS would ignore it");
    {
        static const struct { const char *file; uint32_t size, crc; const char *reason; } sl[] = {
            { "maze", 2048, 0x4d42b296u, "2102" },
            { "hangman", 3072, 0x9238d6ceu, "2102" },
            { "schach", 6144, 0x04fb6dceu, "$2800" },
            { "multicrt", 262144, 0xa1ecbd58u, "banked" },
            { "multicrto", 262144, 0xad4ebe3fu, "banked" },
        };
        for (unsigned i = 0; i < sizeof sl / sizeof *sl; i++) {
            char what[160];
            snprintf(src, sizeof src, "%s/%s.bin", cfg, sl[i].file);
            write_softlist(src, sl[i].size, sl[i].crc);
            check(chfsession_check_cart(src, why, sizeof why) == 1, "the softlist cartridge plans as a game");
            snprintf(what, sizeof what, "%s: refused for the SD, because %s", sl[i].file, sl[i].reason);
            check(chfsession_import_cart_to_sd(s, src, dest, sizeof dest) == -1
                  && strstr(chfsession_last_error(s), "cannot serve") != NULL
                  && strstr(chfsession_last_error(s), sl[i].reason) != NULL, what);
        }
        /* a softlist cartridge that is just a ROM (Bowling) goes in */
        snprintf(src, sizeof src, "%s/bowling.bin", cfg);
        write_softlist(src, 2048, 0x94322c79u);
        check(chfsession_import_cart_to_sd(s, src, dest, sizeof dest) == 0, "a plain softlist Videocart (Bowling) is fine");
    }
    snprintf(src, sizeof src, "%s/one.zip", cfg);
    check(chfsession_import_cart_to_sd(s, src, dest, sizeof dest) == 0, "a zip of one cartridge imports to SD");
    snprintf(sd, sizeof sd, "%s/fujinet/SD/one.bin", data);
    check(strcmp(dest, sd) == 0 && exists(dest), "unpacked as <zip name>.bin");

    /* with an SD folder, anything else is served by FujiNet */
    snprintf(src, sizeof src, "%s/notes.txt", cfg);
    check(chfsession_import_media(s, src, dest, sizeof dest) == 0, "other files go to the SD folder");
    snprintf(sd, sizeof sd, "%s/fujinet/SD/notes.txt", data);
    check(strcmp(dest, sd) == 0 && exists(dest), "at its top");

    chfsession_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
