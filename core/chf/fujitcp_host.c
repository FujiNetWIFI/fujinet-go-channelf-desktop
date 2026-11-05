/* fujitcp_host.c -- the FujiBus-over-TCP transport this app links, in place
 * of the staged POSIX emu/fujitcp.c.
 *
 * Same contract (the staged fujitcp.h), same wire behaviour, kept close to
 * the staged original so the two stay diffable. It differs in three ways,
 * all because here the transport runs on the cartridge's own worker thread
 * (core/chf/fujinet_cart.c) inside a long-lived desktop process rather than
 * on MAME's emulation thread:
 *
 *   - One file for POSIX and Winsock (the siblings carry a separate
 *     fujitcp_win32.c twin; one portable file is one fewer copy to drift).
 *   - fujitcp_abort() (fujitcp_host.h) shuts the socket down from another
 *     thread, so stopping the session never waits out a 90-second network
 *     OPEN that is still in flight.
 *   - The peer closing the socket (FujiNet stopped, or restarted from its
 *     web UI) is a dead link, not a stream of empty reads: the staged
 *     read_frame() spins on recv() == 0 until its deadline. Here the socket
 *     is closed and the transaction fails with FB_ENOLINK, so the next one
 *     reconnects (the cart's wait_link_ms port hook).
 *
 * It also hands every frame sent and read to the debugger's FujiNet trace
 * (fujitcp_set_trace).
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * (matches the staged fujitcp.c it mirrors; copyright-holders Thomas Cherryhomes)
 */

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define sock_close closesocket
#define SHUT_BOTH SD_BOTH
#else
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define sock_close close
#define SHUT_BOTH SHUT_RDWR
#endif

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fujitcp.h"
#include "fujitcp_host.h"
#include "fujimail.h"
#include "fuji_mailbox.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* Must hold the largest SLIP-encoded frame: a 6-byte header plus a 1K reply
 * (FUJIMAIL_RX_MAX), doubled for SLIP's worst case, plus two delimiters. */
#define RX_RAW_MAX (2 * (6 + 1024) + 2)

/* The socket is read on the worker thread and shut down from the session's;
 * a plain atomic word is all the sharing there is. */
static _Atomic(sock_t) fd = BAD_SOCK;

#ifdef _WIN32
static int wsa_started = 0;
#endif

/* The debugger's tap; the lock makes removing it wait out a call. */
static pthread_mutex_t trace_mtx = PTHREAD_MUTEX_INITIALIZER;
static fujitcp_trace_fn trace_fn;
static void *trace_user;

void fujitcp_set_trace(fujitcp_trace_fn fn, void *user)
{
    pthread_mutex_lock(&trace_mtx);
    trace_fn = fn;
    trace_user = user;
    pthread_mutex_unlock(&trace_mtx);
}

static void trace(int dir, const uint8_t *buf, size_t len)
{
    pthread_mutex_lock(&trace_mtx);
    if (trace_fn)
        trace_fn(trace_user, dir, buf, len);
    pthread_mutex_unlock(&trace_mtx);
}

static uint64_t now_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

bool fujitcp_active(void)
{
    return atomic_load(&fd) != BAD_SOCK;
}

int fujitcp_init(const char *hostport)
{
    char host[256], *colon;
    int port = 9995;
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[16];
    sock_t s = BAD_SOCK;
#ifdef _WIN32
    BOOL one = TRUE;

    if (!wsa_started) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            fprintf(stderr, "fujinet: WSAStartup failed\n");
            return -1;
        }
        wsa_started = 1;
    }
#else
    int one = 1;
#endif

    fujitcp_close();

    if (hostport == NULL)
        hostport = getenv("FUJINET_TCP");
    if (hostport == NULL)
        hostport = "127.0.0.1:9995";
    snprintf(host, sizeof host, "%s", hostport);
    colon = strrchr(host, ':');
    if (colon) {
        *colon = '\0';
        port = atoi(colon + 1);
    }
    if (host[0] == '\0')
        snprintf(host, sizeof host, "127.0.0.1");
    snprintf(portstr, sizeof portstr, "%d", port);

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || res == NULL) {
        fprintf(stderr, "fujinet: cannot resolve %s:%d\n", host, port);
        return -1;
    }
    /* Walk every result: "localhost" usually resolves to ::1 first, while
     * fujinet-pc's BoIP listener binds 127.0.0.1 only. */
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == BAD_SOCK)
            continue;
        if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0)
            break;
        sock_close(s);
        s = BAD_SOCK;
    }
    freeaddrinfo(res);
    if (s == BAD_SOCK) {
        fprintf(stderr, "fujinet: cannot connect to %s:%d\n", host, port);
        return -1;
    }
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    atomic_store(&fd, s);
    fprintf(stderr, "fujinet: connected to %s:%d\n", host, port);
    return 0;
}

void fujitcp_close(void)
{
    sock_t s = atomic_exchange(&fd, BAD_SOCK);
    if (s != BAD_SOCK)
        sock_close(s);
}

void fujitcp_abort(void)
{
    /* shutdown, not close: a select()/recv() blocked on the socket in
     * another thread wakes up (readable, EOF) and that thread closes it */
    sock_t s = atomic_load(&fd);
    if (s != BAD_SOCK)
        shutdown(s, SHUT_BOTH);
}

/* Called every ~2 ms while a reply is awaited (the cart draws meanwhile, as
 * core0 does in fujibus_usb.c's pump); NULL when nothing needs it. */
static void (*idle_fn)(void);

void fujitcp_set_idle(void (*fn)(void))
{
    idle_fn = fn;
}

/* Read one complete SLIP frame (two 0xC0 delimiters) or time out. */
static fb_status_t read_frame(uint8_t *buf, size_t cap, size_t *out_len, int secs)
{
    size_t n = 0;
    int ends = 0;
    const uint64_t deadline = now_ms() + (uint64_t)secs * 1000u;

    for (;;) {
        sock_t s = atomic_load(&fd);
        fd_set rf;
        struct timeval tv;
        int64_t remain = (int64_t)(deadline - now_ms());

        if (s == BAD_SOCK)
            return FB_ENOLINK;
        if (remain <= 0)
            return FB_ETIMEOUT;
        if (idle_fn && remain > 2)
            remain = 2;                 /* wake for the idle hook */
        tv.tv_sec = (long)(remain / 1000);
        tv.tv_usec = (long)((remain % 1000) * 1000);

        FD_ZERO(&rf);
        FD_SET(s, &rf);
        switch (select((int)s + 1, &rf, NULL, NULL, &tv)) {
        case 0:
            if (idle_fn) {
                idle_fn();
                continue;               /* the deadline decides */
            }
            return FB_ETIMEOUT;
        case -1:
            return FB_ETIMEOUT;
        default:
            break;
        }

        /* Drain what is there, a byte at a time so nothing past the frame's
         * closing delimiter is consumed (a push frame may follow it). */
        for (;;) {
            unsigned char c;
            int r;
#ifdef _WIN32
            u_long avail = 0;
            if (ioctlsocket(s, FIONREAD, &avail) != 0)
                break;
            if (avail == 0) {
                /* readable with nothing to read: the peer closed */
                char probe;
                if (n == 0 && recv(s, &probe, 1, MSG_PEEK) == 0) {
                    fujitcp_close();
                    return FB_ENOLINK;
                }
                break;
            }
            r = recv(s, (char *)&c, 1, 0);
#else
            r = (int)recv(s, &c, 1, MSG_DONTWAIT);
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                break;
#endif
            if (r == 0 || r < 0) {
                /* EOF or a reset: the link is gone */
                fujitcp_close();
                return FB_ENOLINK;
            }
            if (n >= cap)
                return FB_ETOOBIG;
            buf[n++] = c;
            if (c == 0xC0 && ++ends == 2) {
                *out_len = n;
                trace(0, buf, n);
                return FB_OK;
            }
        }
    }
}

fb_status_t fujitcp_transact(uint8_t device, uint8_t command,
                             const fb_param_t *params, unsigned nparams,
                             const uint8_t *payload, uint16_t payload_len,
                             uint32_t timeout_ms, fb_reply_t *reply)
{
    static uint8_t req[FN_TX_MAX + 64];
    static uint8_t raw[RX_RAW_MAX];
    size_t reqlen, rawlen;
    fb_status_t st;
    int secs = (int)((timeout_ms + 999) / 1000);
    sock_t s = atomic_load(&fd);

    if (s == BAD_SOCK)
        return FB_ENOLINK;

    reqlen = fujibus_build_request(device, command, params, nparams,
                                   payload, payload_len, req, sizeof req);
    if (reqlen == 0)
        return FB_ETOOBIG;
    if (send(s, (const char *)req, (int)reqlen, MSG_NOSIGNAL) != (int)reqlen) {
        fujitcp_close();
        return FB_ENOLINK;
    }
    trace(1, req, reqlen);

    for (;;) {
        st = read_frame(raw, sizeof raw, &rawlen, secs);
        if (st != FB_OK)
            return st;
        if (!fujibus_parse_reply(raw, rawlen, reply))
            return FB_EBADFRAME;
        /* Push frames arrive interleaved with the reply we are waiting for;
         * consuming one proves the link is alive, so the deadline restarts
         * rather than counting down. */
        if (!fujimail_inbound(reply))
            return FB_OK;
    }
}

void fujitcp_send_bare(uint8_t device, uint8_t command,
                       const uint8_t *payload, uint16_t payload_len)
{
    uint8_t frame[64];
    size_t n = fujibus_build_request(device, command, NULL, 0,
                                     payload, payload_len, frame,
                                     sizeof frame);
    sock_t s = atomic_load(&fd);

    if (n && s != BAD_SOCK) {
        int sent = (int)send(s, (const char *)frame, (int)n, MSG_NOSIGNAL);
        if (sent > 0)
            trace(1, frame, (size_t)sent);
    }
}
