/*
 * media_test -- where a dropped file goes (a cartridge to the cartridge
 * directory, anything else to the FujiNet SD folder), what makes a cartridge
 * image acceptable (a file, or a zip of exactly one), and Import Cartridge to
 * SD. No machine is started; no firmware is used.
 *
 * The images are filler: a FujiNet app is one with the "FUJI" claim where the
 * cartridge's planner looks for it (image offset $1CFC, so an 8K image).
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

static void write_filler(const char *path, size_t n)
{
    uint8_t *b = malloc(n ? n : 1);
    for (size_t i = 0; i < n; i++)
        b[i] = (uint8_t)((i * 7 + 3) & 0xff);
    write_bytes(path, b, n);
    free(b);
}

/* An 8K image that claims the mailbox ("FUJI" at $1CFC), or with `pages`
 * extra 2K pages after it (an APPBANK image). */
static void write_app(const char *path, unsigned pages)
{
    size_t size = 0x2000 + (size_t)pages * 0x800;
    uint8_t *b = malloc(size);
    memset(b, 0xea, size);
    memcpy(b + 0x1CFC, "FUJI", 4);
    write_bytes(path, b, size);
    free(b);
}

int main(void)
{
    char cfg[400], data[500], src[500], dest[900], sd[900], why[256];
    uint8_t cart[4096], other[4096];
    chfsession_paths p;
    chfsession *s;

    test_tmpdir(cfg, sizeof cfg, "mcfg");
    snprintf(data, sizeof data, "%s/data", cfg);
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
    s = chfsession_new(&p);
    if (!s) return 1;

    /* routing by extension */
    check(chfsession_media_is_cartridge("game.bin") && chfsession_media_is_cartridge("game.rom")
          && chfsession_media_is_cartridge("game.chf") && chfsession_media_is_cartridge("game.zip"),
          ".bin, .rom, .chf and .zip are cartridges");
    check(chfsession_media_is_cartridge("GAME.BIN") && chfsession_media_is_cartridge("Game.CHF"),
          "case-insensitively");
    check(!chfsession_media_is_cartridge("game.st2") && !chfsession_media_is_cartridge("game.sms"),
          "another console's extensions are not");
    check(!chfsession_media_is_cartridge("notes.txt") && !chfsession_media_is_cartridge("noext")
          && !chfsession_media_is_cartridge(NULL), "nor are .txt, nothing, or NULL");

    /* what makes a cartridge acceptable */
    snprintf(src, sizeof src, "%s/game.bin", cfg);
    write_filler(src, 2048);
    check(chfsession_check_cart(src, why, sizeof why) == 1, "a 2K game plans");
    check(!chfsession_cart_is_app(src), "a game is not a FujiNet app");
    snprintf(src, sizeof src, "%s/game4k.bin", cfg);
    write_filler(src, 4096);
    check(chfsession_check_cart(src, why, sizeof why) == 1 && !chfsession_cart_is_app(src),
          "a 4K game plans");

    snprintf(src, sizeof src, "%s/app.bin", cfg);
    write_app(src, 0);
    check(chfsession_check_cart(src, why, sizeof why) == 1 && chfsession_cart_is_app(src),
          "an 8K image with the claim is a FujiNet app");
    snprintf(src, sizeof src, "%s/appbank.bin", cfg);
    write_app(src, 6);
    check(chfsession_check_cart(src, why, sizeof why) == 1 && chfsession_cart_is_app(src),
          "and an APPBANK image (8K and six 2K pages)");

    snprintf(src, sizeof src, "%s/huge.bin", cfg);
    write_filler(src, 200000);
    check(chfsession_check_cart(src, why, sizeof why) == 0 && strstr(why, "larger") != NULL,
          "an image past what the cartridge can serve is refused, and says why");
    snprintf(src, sizeof src, "%s/empty.bin", cfg);
    write_bytes(src, (const uint8_t *)"", 0);
    check(chfsession_check_cart(src, why, sizeof why) == 0 && strstr(why, "empty") != NULL, "an empty file is refused");
    snprintf(src, sizeof src, "%s/missing.bin", cfg);
    check(chfsession_check_cart(src, why, sizeof why) == 0 && strstr(why, "cannot open") != NULL,
          "a missing file says so");

    /* a zip of exactly one cartridge (MAME's romsets) */
    for (int i = 0; i < 4096; i++) { cart[i] = (uint8_t)(i * 5 + 1); other[i] = (uint8_t)(i * 3); }
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
    write_filler(src, 2048);
    check(chfsession_import_media(s, src, dest, sizeof dest) == 0, "a cartridge imports");
    check(strncmp(dest, chfsession_carts_path(s), strlen(chfsession_carts_path(s))) == 0,
          "into the cartridge directory");
    check(exists(dest), "and the copy exists");

    /* anything else goes to the SD folder, which needs FujiNet's runtime */
    snprintf(src, sizeof src, "%s/notes.txt", cfg);
    write_filler(src, 10);
    check(chfsession_import_media(s, src, dest, sizeof dest) == -1,
          "anything else is refused while there is no SD folder");
    check(strstr(chfsession_last_error(s), "nowhere to put it") != NULL, "with a useful message");

    /* Import Cartridge to SD. The SD folder needs the runtime's
     * provisioning; make it by hand here. */
    snprintf(src, sizeof src, "%s/tennis.bin", cfg);
    write_filler(src, 2048);
    check(chfsession_import_cart_to_sd(s, src, dest, sizeof dest) == -1,
          "SD import refuses when there is no SD folder");
    snprintf(sd, sizeof sd, "%s/fujinet", data); test_mkdir(sd);
    snprintf(sd, sizeof sd, "%s/fujinet/SD", data); test_mkdir(sd);
    check(chfsession_import_cart_to_sd(s, src, dest, sizeof dest) == 0, "SD import copies into the SD root");
    snprintf(sd, sizeof sd, "%s/fujinet/SD/tennis.bin", data);
    check(strcmp(dest, sd) == 0 && exists(dest), "at the top of the SD tree");
    snprintf(src, sizeof src, "%s/appsd.bin", cfg);
    write_app(src, 4);
    check(chfsession_import_cart_to_sd(s, src, dest, sizeof dest) == 0, "an APPBANK app imports to SD too");
    snprintf(src, sizeof src, "%s/oddsd.bin", cfg);
    write_filler(src, 0x2000 + 1000);
    check(chfsession_import_cart_to_sd(s, src, dest, sizeof dest) == -1,
          "an image the FujiNet cartridge cannot serve is refused for the SD");
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
