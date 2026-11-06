/*
 * debug_fujinet_test -- the debugger's FujiNet trace through the whole session:
 * an EXTERNAL FujiNet (the cartridge dials a stand-in on loopback, no built-in
 * runtime), CONFIG running on the stand-in BIOS, and the trace read the way the
 * Cart tab reads it -- from chfsession_debugger(), polled. It must fill from
 * session start (no window open, the engine never attached), keep filling
 * after Reset to CONFIG, a restart and a debugger attach / detach, and survive
 * the engine's own power cycle hooks.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
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
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "chfdebug.h"
#include "chfsession.h"
#include "fujitrace.h"
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

/* ---- a stand-in FujiNet: ACKs every request frame, for every console that dials in ---- */

typedef struct { int listen_fd; atomic_int stop; atomic_int dials; } fake_t;

static size_t ack_frame(uint8_t *out)
{
    uint8_t pkt[7] = { 0x70, 0x06, 7, 0, 0, 0, 0 };
    uint16_t ck = 0;
    size_t o = 0;
    for (int i = 0; i < 7; i++) {
        ck = (uint16_t)(ck + pkt[i]);
        ck = (uint16_t)((ck >> 8) + (ck & 0xFF));
    }
    pkt[4] = (uint8_t)ck;
    out[o++] = 0xC0;
    for (int i = 0; i < 7; i++) out[o++] = pkt[i];
    out[o++] = 0xC0;
    return o;
}

static void *fake_main(void *arg)
{
    fake_t *f = arg;
    uint8_t c, reply[16];

    for (;;) {
        int ends = 0, fd = accept(f->listen_fd, NULL, NULL);

        if (fd < 0 || atomic_load(&f->stop)) {
            if (fd >= 0) close(fd);
            break;
        }
        atomic_fetch_add(&f->dials, 1);
        while (recv(fd, &c, 1, 0) == 1) {
            if (c != 0xC0) continue;
            if (++ends == 2) {
                send(fd, reply, ack_frame(reply), 0);
                ends = 0;
            }
        }
        close(fd);
    }
    return NULL;
}

static int wait_stopped(chfdebug *d, int want, int timeout_ms)
{
    int waited = 0;

    while (waited < timeout_ms) {
        if (!!chfdebug_is_stopped(d) == want) return 1;
        sleep_ms(2);
        waited += 2;
    }
    return 0;
}

/* wait for the trace to hold packets newer than `after` (a serial) */
static int wait_trace(chfdebug *d, uint64_t after, int timeout_ms, chfdebug_fuji_packet *pk, int *n)
{
    int waited = 0;

    for (;;) {
        *n = chfdebug_fujinet_trace(d, pk, FUJITRACE_MAX);
        if (*n && pk[*n - 1].serial > after)
            return 1;
        if (waited >= timeout_ms)
            return 0;
        sleep_ms(20);
        waited += 20;
    }
}

int main(void)
{
    static chfdebug_fuji_packet pk[FUJITRACE_MAX];
    char cfg[512], data[512], addr[64];
    chfsession_paths paths;
    chfsession_start_opts opts;
    struct sockaddr_in sin;
    socklen_t len = sizeof sin;
    int one = 1, n, real = 0;
    uint64_t mark;
    pthread_t th;
    fake_t fake;
    chfsession *s;
    chfdebug *d;

    memset(&fake, 0, sizeof fake);
    fake.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    setsockopt(fake.listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (fake.listen_fd < 0 || bind(fake.listen_fd, (struct sockaddr *)&sin, sizeof sin) != 0 ||
        listen(fake.listen_fd, 4) != 0 || getsockname(fake.listen_fd, (struct sockaddr *)&sin, &len) != 0 ||
        pthread_create(&th, NULL, fake_main, &fake) != 0) {
        printf("SKIP: no loopback listener\n");
        return 77;
    }
    snprintf(addr, sizeof addr, "127.0.0.1:%d", (int)ntohs(sin.sin_port));
    /* a real fujinet-pc instead of the stand-in, for a local run: CHF_TEST_EXTERNAL=host:port
     * (a PRIVATE one, never a shared port) */
    if (getenv("CHF_TEST_EXTERNAL") && *getenv("CHF_TEST_EXTERNAL")) {
        snprintf(addr, sizeof addr, "%s", getenv("CHF_TEST_EXTERNAL"));
        real = 1;
    }

    test_tmpdir(cfg, sizeof cfg, "dfcfg");
    test_tmpdir(data, sizeof data, "dfdata");
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    s = chfsession_new(&paths);
    if (!s) return 1;

    chfsession_default_opts(s, &opts);
    opts.enable_audio = 0;
    opts.enable_gamepad = 0;
    opts.external_fujinet = 1;
    opts.boip_connect = addr;
    check(chfsession_start(s, &opts) == 0 && chfsession_fujinet_external(s), "starts with an external FujiNet");
    d = chfsession_debugger(s);
    check(d != NULL && !chfdebug_is_attached(d), "the engine exists and no window holds it");

    if (chfsession_missing(s)) {
        char why[160];
        chfsession_missing_reason(s, why, sizeof why);
        printf("note: the stand-in cannot run CONFIG here (%s): skipped\n", why);
        chfsession_stop(s);
        chfsession_free(s);
        return 77;
    }

    check(wait_trace(d, 0, 15000, pk, &n), "CONFIG's exchanges with the external FujiNet are in the trace");
    for (int waited = 0; n < 2 && waited < 15000; waited += 20) {   /* the reply may lag the request */
        sleep_ms(20);
        n = chfdebug_fujinet_trace(d, pk, FUJITRACE_MAX);
    }
    check(n >= 2 && !pk[0].to_host && pk[0].device >= 0 && pk[0].text[0], "the first is a request from the Channel F");
    if (n)
        printf("      %d packets, first '%s'\n", n, pk[0].text);
    mark = n ? pk[n - 1].serial : 0;

    /* a debugger window opens: what was recorded before it is still there */
    chfdebug_attach(d);
    check(chfdebug_is_attached(d), "attach");
    check(chfdebug_fujinet_trace(d, pk, FUJITRACE_MAX) >= n, "the packets from before the attach are still in the trace");
    chfdebug_detach(d);
    check(chfdebug_fujinet_trace(d, pk, FUJITRACE_MAX) >= 2, "and after the detach");

    /* Reset to CONFIG: a power cycle with a new machine and a new dial */
    chfdebug_fujinet_trace_clear(d);
    check(chfsession_reset_to_config(s) == 0, "Reset to CONFIG");
    check(wait_trace(d, mark, 15000, pk, &n), "the trace fills after a power cycle");
    mark = n ? pk[n - 1].serial : mark;

    /* the same with a window attached across the cycle */
    chfdebug_attach(d);
    chfdebug_resume(d);
    chfdebug_fujinet_trace_clear(d);
    check(chfsession_reset_to_config(s) == 0, "Reset to CONFIG with the debugger attached");
    sleep_ms(300);
    chfdebug_resume(d);
    check(wait_trace(d, mark, 15000, pk, &n), "and with the debugger attached");
    mark = n ? pk[n - 1].serial : mark;
    chfdebug_detach(d);

    /* a restart of the whole session */
    chfsession_stop(s);
    chfdebug_fujinet_trace_clear(d);
    check(chfsession_start(s, &opts) == 0, "restart");
    check(chfsession_debugger(s) == d, "the same engine");
    check(wait_trace(d, mark, 15000, pk, &n), "the trace fills after a stop and a start");
    mark = n ? pk[n - 1].serial : mark;

    /* the window is open before the machine starts (the engine stops it on its first
     * instruction); nothing is exchanged until Run, and then everything is */
    chfsession_stop(s);
    chfdebug_fujinet_trace_clear(d);
    chfdebug_attach(d);
    check(chfsession_start(s, &opts) == 0, "restart with the debugger attached");
    check(wait_stopped(d, 1, 5000), "the new machine stops on its first instruction");
    sleep_ms(300);
    check(chfdebug_fujinet_trace(d, pk, FUJITRACE_MAX) == 0, "and nothing has been exchanged yet");
    chfdebug_resume(d);
    check(wait_trace(d, mark, 15000, pk, &n), "after Run the trace fills");
    chfdebug_detach(d);
    check(real || atomic_load(&fake.dials) >= 4, "(the cartridge dialled the external FujiNet each time)");

    chfsession_stop(s);
    chfsession_free(s);
    atomic_store(&fake.stop, 1);
    shutdown(fake.listen_fd, SHUT_RDWR);
    close(fake.listen_fd);
    pthread_join(th, NULL);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
#endif
