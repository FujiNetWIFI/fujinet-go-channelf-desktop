/*
 * fujitrace_test -- the debugger's FujiNet trace (core/debugger/fujitrace.c)
 * on byte streams built here: both directions decoded and named, frames
 * split anywhere (inside an escape too), long and short packets, a send
 * dropping a reply cut short, repeats and repeated exchanges counted as one
 * entry, the ring's limit, and Clear. No machine, no network.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "fujitrace.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static fujitrace t;
static chfdebug_fuji_packet p[FUJITRACE_MAX];

/* A FujiBus packet: device, command, length (LE, header included), a
 * checksum (not checked), a descriptor, then the payload. */
static size_t packet(uint8_t dev, uint8_t cmd, const uint8_t *payload, size_t plen, uint8_t *out)
{
    const size_t n = 6 + plen;
    out[0] = dev;
    out[1] = cmd;
    out[2] = (uint8_t)(n & 0xff);
    out[3] = (uint8_t)(n >> 8);
    out[4] = 0x5a;
    out[5] = 0;
    if (plen) memcpy(out + 6, payload, plen);
    return n;
}

/* SLIP: C0, the bytes with C0 and DB escaped, C0. */
static size_t slip(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t o = 0;
    out[o++] = 0xC0;
    for (size_t i = 0; i < n; i++) {
        if (in[i] == 0xC0) { out[o++] = 0xDB; out[o++] = 0xDC; }
        else if (in[i] == 0xDB) { out[o++] = 0xDB; out[o++] = 0xDD; }
        else out[o++] = in[i];
    }
    out[o++] = 0xC0;
    return o;
}

/* One framed packet, fed whole. */
static void send_packet(int dir, uint8_t dev, uint8_t cmd, const uint8_t *payload, size_t plen)
{
    uint8_t raw[1100], framed[2300];
    const size_t n = packet(dev, cmd, payload, plen, raw);
    fujitrace_feed(&t, dir, framed, slip(raw, n, framed));
}

static int read_all(void)
{
    return fujitrace_read(&t, p, FUJITRACE_MAX);
}

static void empty(void)
{
    fujitrace_init(&t);
    check(read_all() == 0, "a new trace is empty");
    fujitrace_feed(&t, 0, (const uint8_t *)"\xC0\xC0\xC0", 3);
    check(read_all() == 0, "bare delimiters make no packet");
}

static void both_ways(void)
{
    static const uint8_t slots[] = { 'h', 'o', 's', 't', 0 };
    fujitrace_init(&t);
    send_packet(1, 0x70, 0xF4, NULL, 0);
    send_packet(0, 0x70, 0x01, slots, sizeof slots);
    check(read_all() == 2, "a request and its reply are two packets");
    check(p[0].to_host == 0 && p[0].device == 0x70 && p[0].command == 0xF4 && p[0].length == 6 &&
          p[0].nbytes == 6, "the request's header");
    check(strcmp(p[0].text, "Channel F \xe2\x86\x92 FujiNet 70 F4 READ HOST SLOTS (6 bytes)") == 0,
          "the request is named");
    check(p[1].to_host == 1 && p[1].command == 0x01 && p[1].length == 11 && p[1].nbytes == 11 &&
          memcmp(p[1].data + 6, slots, sizeof slots) == 0, "the reply's header and bytes");
    check(strncmp(p[1].text, "FujiNet \xe2\x86\x92 Channel F 70 01", 25) == 0, "the reply's line");
    check(p[0].repeat == 1 && p[1].repeat == 1 && p[1].serial > p[0].serial, "counts and serials");
}

static void split(void)
{
    static const uint8_t payload[] = { 0xC0, 0x11, 0xDB, 0x22, 0xC0, 0xDB };
    uint8_t raw[64], framed[128];
    size_t n, f, i;

    n = packet(0x70, 0x01, payload, sizeof payload, raw);
    f = slip(raw, n, framed);

    fujitrace_init(&t);
    for (i = 0; i < f; i++)
        fujitrace_feed(&t, 0, framed + i, 1);
    check(read_all() == 1 && p[0].nbytes == (int)n && memcmp(p[0].data, raw, n) == 0,
          "a byte at a time, escapes and all");

    fujitrace_init(&t);
    for (i = 0; i < f; i += 3)
        fujitrace_feed(&t, 0, framed + i, f - i < 3 ? f - i : 3);
    check(read_all() == 1 && p[0].nbytes == (int)n && memcmp(p[0].data, raw, n) == 0,
          "three bytes at a time");

    /* the escape byte ends one feed, its partner starts the next */
    fujitrace_init(&t);
    for (i = 0; i < f && framed[i] != 0xDB; i++) {}
    fujitrace_feed(&t, 0, framed, i + 1);
    fujitrace_feed(&t, 0, framed + i + 1, f - i - 1);
    check(read_all() == 1 && p[0].data[6] == 0xC0, "an escape split across feeds");
}

static void long_and_short(void)
{
    uint8_t big[100];
    for (int i = 0; i < 100; i++) big[i] = (uint8_t)i;
    fujitrace_init(&t);
    send_packet(0, 0x70, 0x01, big, sizeof big);
    fujitrace_feed(&t, 1, (const uint8_t *)"\xC0\x01\x02\x03\xC0", 5);
    check(read_all() == 2, "two packets");
    check(p[0].nbytes == 106 && p[0].data[63] == 57, "a long packet keeps its first 64 bytes");
    check(p[1].nbytes == 3 && p[1].device == -1 && p[1].command == -1 && p[1].length == -1,
          "a short packet has no header");
    check(strcmp(p[1].text, "Channel F \xe2\x86\x92 FujiNet 3 bytes") == 0, "the short packet's line");
}

static void send_drops_partial(void)
{
    static const uint8_t reply[] = { 1, 2, 3 };
    fujitrace_init(&t);
    fujitrace_feed(&t, 0, (const uint8_t *)"\xC0\x70\x01\x0A", 4);   /* cut short */
    send_packet(1, 0x70, 0xE8, NULL, 0);
    send_packet(0, 0x70, 0x01, reply, sizeof reply);
    check(read_all() == 2 && p[1].nbytes == 9 && p[1].data[6] == 1, "a send drops a reply cut short");
}

static void repeats(void)
{
    static const uint8_t a[] = { 1 }, b[] = { 2 };
    uint64_t last;
    int n;

    /* the same exchange three times */
    fujitrace_init(&t);
    send_packet(1, 0x70, 0xF4, NULL, 0);
    send_packet(0, 0x70, 0x01, a, 1);
    send_packet(1, 0x70, 0xF4, NULL, 0);
    check(read_all() == 3, "a repeated request shows while its reply is awaited");
    last = p[2].serial;
    send_packet(0, 0x70, 0x01, a, 1);
    n = read_all();
    check(n == 2 && p[0].repeat == 2 && p[1].repeat == 2, "its reply makes the exchange x2");
    check(p[1].serial > last, "the newest entry's serial moves on");
    send_packet(1, 0x70, 0xF4, NULL, 0);
    send_packet(0, 0x70, 0x01, a, 1);
    n = read_all();
    check(n == 2 && p[0].repeat == 3 && p[1].repeat == 3, "x3");

    /* a different reply breaks the run */
    send_packet(1, 0x70, 0xF4, NULL, 0);
    send_packet(0, 0x70, 0x01, b, 1);
    n = read_all();
    check(n == 4 && p[2].repeat == 1 && p[3].repeat == 1 && p[3].data[6] == 2,
          "a changed reply is a new exchange");

    /* the same packet back to back */
    fujitrace_init(&t);
    for (int i = 0; i < 5; i++)
        send_packet(1, 0x70, 0x00, NULL, 0);
    check(read_all() == 1 && p[0].repeat == 5, "a bare send five times is one entry x5");

    /* Q R Q Q: the second Q is not an exchange */
    fujitrace_init(&t);
    send_packet(1, 0x70, 0xF4, NULL, 0);
    send_packet(0, 0x70, 0x01, a, 1);
    send_packet(1, 0x70, 0xF4, NULL, 0);
    send_packet(1, 0x70, 0xF4, NULL, 0);
    n = read_all();
    check(n == 3 && p[0].repeat == 1 && p[2].repeat == 2, "a request repeated without a reply");

    /* Q1 R Q2 R Q2 R: grouped by request, not by reply */
    fujitrace_init(&t);
    send_packet(1, 0x70, 0xF4, NULL, 0);
    send_packet(0, 0x70, 0x01, a, 1);
    send_packet(1, 0x70, 0xF2, NULL, 0);
    send_packet(0, 0x70, 0x01, a, 1);
    send_packet(1, 0x70, 0xF2, NULL, 0);
    send_packet(0, 0x70, 0x01, a, 1);
    n = read_all();
    check(n == 4 && p[0].repeat == 1 && p[1].repeat == 1 && p[2].command == 0xF2 && p[2].repeat == 2 &&
          p[3].repeat == 2, "exchanges start at a request");
}

static void ring(void)
{
    uint8_t v[2];
    uint64_t last;
    int n;

    fujitrace_init(&t);
    for (int i = 0; i < 600; i++) {
        v[0] = (uint8_t)(i >> 8);
        v[1] = (uint8_t)i;
        send_packet(1, 0x70, 0x52, v, 2);
    }
    n = read_all();
    check(n == FUJITRACE_MAX && p[0].serial == 89 && p[n - 1].serial == 600, "the ring keeps the newest 512");
    n = fujitrace_read(&t, p, 10);
    check(n == 10 && p[0].serial == 591 && p[9].serial == 600, "the newest 10, oldest first");
    check(fujitrace_read(&t, p, 0) == 0, "max 0 reads nothing");

    last = p[9].serial;
    fujitrace_clear(&t);
    check(read_all() == 0, "Clear empties it");
    send_packet(1, 0x70, 0x52, v, 2);
    check(read_all() == 1 && p[0].serial > last, "serials go on after Clear");
}

int main(void)
{
    empty();
    both_ways();
    split();
    long_and_short();
    send_drops_partial();
    repeats();
    ring();
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
