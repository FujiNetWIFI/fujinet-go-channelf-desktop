/*
 * netboot_test -- the cartridge bring-up's own clients against the in-process
 * FujiNet, opened as cartridges (they claim the mailbox, so they are FujiNet
 * apps):
 *
 *   fujitest  one real transaction (GET_ADAPTERCONFIG_EXTENDED): the cartridge's
 *             status page shows it answered (ACKSEQ 1, no error, an ACK) and the
 *             reply window ($F800) holds the adapter's answer;
 *   fujiboot  MOUNT_HOST -> SET_DEVICE_FULLPATH -> MOUNT_IMAGE of
 *             /ACOUNTER.BIN on the SD host (put there by Import Cartridge to
 *             SD): FujiNet pushes it, the cart stages it, BOOTLOCK, the swap
 *             stub in the cartridge's RAM stores to $FDFE, the BIOS re-enters at
 *             $0802 and the counter app runs (its byte at $8000 moves).
 *
 * Run twice by ctest: with the mailbox on the cart's worker thread, and inline
 * (CHF_CART_SYNC=1 -> FUJINET_ASYNC=0). Needs CHF_TESTROM_DIR: fujinet-firmware's
 * pico/channelf/build, built with
 *   BOOT_HOST=0 BOOT_PATH=/ACOUNTER.BIN pico/channelf/build.sh fujiboot fujitest
 * (the pinned tree under third_party/ will do; needs Macroassembler AS). The
 * stand-in BIOS runs both (they use no BIOS routine).
 * SKIPs (77) without it or without the FujiNet runtime.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chfsession.h"
#include "test_chf.h"
#include "test_tmpdir.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static int file_exists(const char *p)
{
    FILE *f = fopen(p, "rb");

    if (f)
        fclose(f);
    return f != NULL;
}

int main(void)
{
    static uint8_t img[16384];
    char cfg[512], data[512], src[700], dest[1200], fujiboot[1200], fujitest[1200], status[128];
    const char *dir = getenv("CHF_TESTROM_DIR");
    chfsession_paths p;
    chfsession *s;
    chfsession_start_opts o;

    if (!dir || !*dir) {
        printf("SKIP: CHF_TESTROM_DIR is not set (see the header)\n");
        return 77;
    }
    snprintf(fujiboot, sizeof fujiboot, "%s/fujiboot.bin", dir);
    snprintf(fujitest, sizeof fujitest, "%s/fujitest.bin", dir);
    if (!file_exists(fujiboot) || !file_exists(fujitest)) {
        printf("SKIP: no fujiboot.bin / fujitest.bin in %s\n", dir);
        return 77;
    }
    if (getenv("CHF_CART_SYNC"))
#ifdef _WIN32
        _putenv("FUJINET_ASYNC=0");
#else
        setenv("FUJINET_ASYNC", "0", 1);
#endif

    test_tmpdir(cfg, sizeof cfg, "nbcfg");
    test_tmpdir(data, sizeof data, "nbdata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data;
    s = chfsession_new(&p);
    if (!s) return 1;
    chfsession_default_opts(s, &o);
    o.enable_audio = 0; o.enable_gamepad = 0; o.enable_fujinet = 1;
    if (chfsession_start(s, &o) != 0) {
        printf("start failed: %s\n", chfsession_last_error(s));
        return 1;
    }
    if (!chfsession_fujinet_running(s)) {
        printf("SKIP: no FujiNet runtime available (%s)\n", chfsession_last_error(s));
        chfsession_free(s);
        return 77;
    }
    printf("mailbox %s\n", getenv("CHF_CART_SYNC") ? "inline (CHF_CART_SYNC)" : "on the cart's worker");

    snprintf(src, sizeof src, "%s/ACOUNTER.BIN", cfg);
    test_counter_image(img, 1);
    test_write_file(src, img, sizeof img);
    check(chfsession_import_cart_to_sd(s, src, dest, sizeof dest) == 0, "the counter app goes to the SD host");

    /* fujitest: one transaction, the adapter's own answer on screen */
    check(chfsession_load_cart(s, fujitest) == 0, "fujitest opens");
    {
        chf_cart_status_t st;
        int got = 0;

        for (int waited = 0; waited < 20000 && !got; waited += 100) {
            chf_host_cart_status(&st);
            got = st.ackseq >= 1 && st.reply_cmd == 0x06 && st.err == 0 && st.rxlen > 0;
            if (!got)
                test_sleep_ms(100);
        }
        check(got, "GET_ADAPTERCONFIG_EXTENDED answered: ACKSEQ, an ACK, no error, a reply");
        check(got && (test_peek(0xf800) | test_peek(0xf801) | test_peek(0xf802) | test_peek(0xf803)) != 0,
              "and the reply is in the window at $F800");
    }

    /* fujiboot: mount, push, stage, hand over */
    check(chfsession_load_cart(s, fujiboot) == 0, "fujiboot opens");
    {
        int swapped = 0;

        for (int waited = 0; waited < 30000 && !swapped; waited += 100) {
            swapped = chfsession_cart_booted_game(s) && test_peek(TEST_COUNTER_ADDR) != 0;
            if (!swapped)
                test_sleep_ms(100);
        }
        check(swapped, "the image was pushed, staged and swapped in, and is running");
    }
    {
        uint8_t a = test_peek(TEST_COUNTER_ADDR);

        test_sleep_ms(300);
        check(test_peek(TEST_COUNTER_ADDR) != a, "its counter moves");
    }
    chfsession_cart_status(s, status, sizeof status);
    check(strstr(status, "mailbox closed") == NULL, "and it claimed the mailbox, so the cart keeps it");

    chfsession_stop(s);
    chfsession_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
