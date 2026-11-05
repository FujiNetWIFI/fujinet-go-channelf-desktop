/*
 * fujitrace.c -- see fujitrace.h.
 *
 * Each direction has its own SLIP decoder (0xC0 ends a frame, 0xDB escapes
 * the next byte); a finished frame is one packet, its FujiBus header read
 * when it has one. The console polls, so the same exchange often repeats
 * back to back: a packet byte-identical to the newest counts as a repeat of
 * it, and a request that repeats the request two back is held as pending
 * until the next packet shows whether its reply repeats too, in which case
 * the pair before it counts again instead.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "fujitrace.h"
#include "fuji_names.h"

static const char *fuji_command_name(int id)
{
    for (size_t i = 0; i < sizeof fuji_command_names / sizeof fuji_command_names[0]; i++)
        if (fuji_command_names[i].id == id)
            return fuji_command_names[i].name;
    return "";
}

/* FNV-1a over the whole packet; with the length and the first 64 bytes,
 * as good as comparing every byte. */
static uint32_t hash(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    while (n--)
        h = (h ^ *p++) * 16777619u;
    return h;
}

/* The slot of the i'th newest packet (1 the newest), -1 if none. */
static int back(const fujitrace *t, int i)
{
    if (i > t->count)
        return -1;
    return (t->head - i + FUJITRACE_MAX) % FUJITRACE_MAX;
}

static int same(const chfdebug_fuji_packet *a, uint32_t asum,
                const chfdebug_fuji_packet *b, uint32_t bsum)
{
    const int n = a->nbytes < (int)sizeof a->data ? a->nbytes : (int)sizeof a->data;
    return a->to_host == b->to_host && a->nbytes == b->nbytes && asum == bsum &&
           memcmp(a->data, b->data, (size_t)n) == 0;
}

static void emit(fujitrace *t, int to_host)
{
    const uint8_t *f = t->acc[to_host];
    const size_t n = t->acclen[to_host];
    const uint32_t sum = hash(f, n);
    const int a = back(t, 1), b = back(t, 2), c = back(t, 3);
    const char *dir = to_host ? "FujiNet \xe2\x86\x92 Channel F" : "Channel F \xe2\x86\x92 FujiNet";
    chfdebug_fuji_packet p;

    memset(&p, 0, sizeof p);
    p.to_host = to_host;
    p.nbytes = (int)n;
    memcpy(p.data, f, n < sizeof p.data ? n : sizeof p.data);
    p.device = n >= 6 ? f[0] : -1;
    p.command = n >= 6 ? f[1] : -1;
    p.length = n >= 6 ? (f[2] | (f[3] << 8)) : -1;
    p.repeat = 1;
    if (p.device >= 0)
        snprintf(p.text, sizeof p.text, "%s %02X %02X %s (%d bytes)", dir, p.device, p.command,
                 to_host ? "" : fuji_command_name(p.command), p.nbytes);
    else
        snprintf(p.text, sizeof p.text, "%s %d bytes", dir, p.nbytes);

    /* the newest again: a bare send, a push, a retry */
    if (a >= 0 && same(&t->ring[a], t->sum[a], &p, sum))
    {
        t->ring[a].repeat++;
        t->ring[a].serial = ++t->serial;
        t->pending = 0;
        return;
    }
    /* the pending request's reply repeats too: the exchange before it
     * happened again */
    if (t->pending && c >= 0 && same(&t->ring[b], t->sum[b], &p, sum) &&
        same(&t->ring[c], t->sum[c], &t->ring[a], t->sum[a]))
    {
        t->head = a;
        t->count--;
        t->ring[c].repeat++;
        t->ring[b].repeat++;
        t->ring[b].serial = ++t->serial;
        t->pending = 0;
        return;
    }

    p.serial = ++t->serial;
    t->ring[t->head] = p;
    t->sum[t->head] = sum;
    t->head = (t->head + 1) % FUJITRACE_MAX;
    if (t->count < FUJITRACE_MAX)
        t->count++;
    /* a request like the one before the last reply, with that exchange
     * counted as often as its reply */
    t->pending = !to_host && b >= 0 && t->ring[a].to_host &&
                 same(&t->ring[b], t->sum[b], &p, sum) && t->ring[b].repeat == t->ring[a].repeat;
}

void fujitrace_init(fujitrace *t)
{
    memset(t, 0, sizeof *t);
}

void fujitrace_feed(fujitrace *t, int dir, const uint8_t *buf, size_t len)
{
    const int to_host = dir == 0;
    uint8_t *acc = t->acc[to_host];

    /* a send starts a new exchange: a reply cut short is not glued onto
     * the next one */
    if (!to_host && len)
    {
        t->acclen[1] = 0;
        t->esc[1] = 0;
    }
    for (size_t i = 0; i < len; i++)
    {
        const uint8_t v = buf[i];
        if (v == 0xC0)
        {
            if (t->acclen[to_host])
                emit(t, to_host);
            t->acclen[to_host] = 0;
            t->esc[to_host] = 0;
            continue;
        }
        if (t->esc[to_host])
        {
            if (t->acclen[to_host] < FUJITRACE_FRAME_MAX)
                acc[t->acclen[to_host]++] = v == 0xDC ? 0xC0 : v == 0xDD ? 0xDB : v;
            t->esc[to_host] = 0;
        }
        else if (v == 0xDB)
        {
            t->esc[to_host] = 1;
        }
        else if (t->acclen[to_host] < FUJITRACE_FRAME_MAX)
        {
            acc[t->acclen[to_host]++] = v;
        }
    }
}

int fujitrace_read(const fujitrace *t, chfdebug_fuji_packet *out, int max)
{
    int n;
    if (max <= 0)
        return 0;
    n = t->count < max ? t->count : max;
    for (int i = 0; i < n; i++)
        out[i] = t->ring[(t->head - n + i + FUJITRACE_MAX) % FUJITRACE_MAX];
    return n;
}

void fujitrace_clear(fujitrace *t)
{
    t->head = t->count = 0;
    t->pending = 0;
}
