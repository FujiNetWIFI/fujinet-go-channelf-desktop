/*
 * fujibus_smoke -- the real in-process FujiNet: libfujinet starts on its
 * port, the CONFIG client boots, and the cartridge's link comes up. This is
 * the one test that proves the whole chain -- the cartridge device (core/chf/fujinet_cart.c)
 * and its transport, the RS232 PC runtime's BoIP listener, the port numbers and
 * the start ordering -- rather than any one piece. The debugger's FujiNet
 * trace, on from the start, has seen both sides of it.
 *
 * SKIPs (77) when no runtime library is available.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "chfdebug.h"
#include "chfsession.h"
#include "test_tmpdir.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

int main(void)
{
    static chfdebug_fuji_packet pk[512];
    char cfg[512], data[512], status[128], logbuf[4096];
    chfsession_paths p;
    chfsession *s;
    chfsession_start_opts o;
    int waited = 0, up = 0, n, sent = 0, got = 0;

    test_tmpdir(cfg, sizeof cfg, "fcfg");
    test_tmpdir(data, sizeof data, "fdata");
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
    check(1, "libfujinet started in-process");
    check(strcmp(chfsession_fujinet_webui_url(s), "http://127.0.0.1:11527/") == 0, "web UI URL is this app's own port");

    /* the cartridge dials in from its worker thread once the console runs */
    while (waited < 10000) {
        if (chfsession_cart_link_up(s) == 1) { up = 1; break; }
        sleep_ms(50); waited += 50;
    }
    chfsession_cart_status(s, status, sizeof status);
    printf("cart status after %d ms: %s\n", waited, status);
    check(up, "the cartridge's link to FujiNet came up");

    /* let the CONFIG client transact (it reads the host slots on boot) */
    sleep_ms(1500);
    logbuf[0] = '\0';
    chfsession_fujinet_copy_log(s, logbuf, sizeof logbuf);
    check(strstr(logbuf, "BoIP") != NULL || strstr(logbuf, "connected") != NULL || logbuf[0] != '\0',
          "the FujiNet console log has content");
    check(chfsession_cart_booted_game(s) == 0, "nothing has been booted yet");
    /* CONFIG's first transaction waits for its splash to go */
    for (waited = 0; waited < 30000 && !(sent && got); waited += 100) {
        n = chfdebug_fujinet_trace(chfsession_debugger(s), pk, 512);
        sent = got = 0;
        for (int i = 0; i < n; i++) {
            if (pk[i].to_host) got++;
            else sent++;
        }
        if (!(sent && got)) sleep_ms(100);
    }
    printf("FujiNet trace after %d ms: %d sent, %d received\n", waited, sent, got);
    check(sent > 0 && got > 0, "the FujiNet trace saw a request and a reply");

    /* reboot: the old link must die before the new one dials in (backlog 1) */
    check(chfsession_reset_to_config(s) == 0, "reset to CONFIG");
    waited = 0; up = 0;
    while (waited < 10000) {
        if (chfsession_cart_link_up(s) == 1) { up = 1; break; }
        sleep_ms(50); waited += 50;
    }
    check(up, "the link comes up again after a power cycle");

    chfsession_stop(s);
    chfsession_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
