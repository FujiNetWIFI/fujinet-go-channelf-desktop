/*
 * external_test -- "Use external FujiNet": the session starts no runtime of
 * its own and its cartridge dials the address in the settings, here a
 * stand-in listener on a spare loopback port. A bad address refuses to
 * start.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
int main(void)
{
    printf("SKIP: POSIX sockets only\n");
    return 77;
}
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "chfsession.h"
#include "test_tmpdir.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

int main(void)
{
    char cfg[512], data[512], addr[64];
    chfsession_paths paths;
    chfsession_start_opts opts;
    struct sockaddr_in sin;
    socklen_t len = sizeof sin;
    int lsock, conn = -1;
    chfsession *s;

    lsock = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (lsock < 0 || bind(lsock, (struct sockaddr *)&sin, sizeof sin) != 0 ||
        listen(lsock, 1) != 0 ||
        getsockname(lsock, (struct sockaddr *)&sin, &len) != 0) {
        printf("SKIP: no loopback listener\n");
        return 77;
    }
    snprintf(addr, sizeof addr, "127.0.0.1:%d", (int)ntohs(sin.sin_port));

    test_tmpdir(cfg, sizeof cfg, "extcfg");
    test_tmpdir(data, sizeof data, "extdata");
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    s = chfsession_new(&paths);
    if (!s) return 1;

    chfsession_default_opts(s, &opts);
    opts.enable_audio = 0;
    opts.enable_gamepad = 0;
    opts.external_fujinet = 1;

    opts.boip_connect = "not an address";
    check(chfsession_start(s, &opts) != 0, "a bad address refuses to start");
    check(!chfsession_is_running(s), "nothing runs after the refusal");

    opts.boip_connect = addr;
    check(chfsession_start(s, &opts) == 0, "starts with an external FujiNet");
    check(chfsession_fujinet_external(s), "reports external");
    check(!chfsession_fujinet_running(s), "no built-in runtime started");
    check(strcmp(chfsession_boip_endpoint(s), addr) == 0, "endpoint is the address");
    check(chfsession_fujinet_webui_url(s)[0] == '\0', "no web UI URL");

    {
        struct pollfd pfd = { lsock, POLLIN, 0 };
        int waited = 0;
        while (waited < 15000 && poll(&pfd, 1, 100) == 0)
            waited += 100;
        if (pfd.revents & POLLIN)
            conn = accept(lsock, NULL, NULL);
        check(conn >= 0, "the cartridge dialed the stand-in listener");
    }

    if (conn >= 0) close(conn);
    chfsession_stop(s);
    check(!chfsession_fujinet_external(s), "not external once stopped");
    chfsession_free(s);
    close(lsock);
    return failures ? 1 : 0;
}
#endif
