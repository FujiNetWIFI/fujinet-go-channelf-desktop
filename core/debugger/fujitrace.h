/*
 * fujitrace -- the debugger's FujiNet trace: the cartridge's socket bytes,
 * both ways, SLIP-decoded into FujiBus packets in a ring. Pure: it takes
 * bytes and keeps packets, so it is testable without a machine or a network
 * (core/tests/fujitrace_test.c). Not thread-safe; the engine locks it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CHF_FUJITRACE_H
#define CHF_FUJITRACE_H

#include <stddef.h>
#include <stdint.h>

#include "chfdebug.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FUJITRACE_MAX 512
#define FUJITRACE_FRAME_MAX 4096

typedef struct {
    chfdebug_fuji_packet ring[FUJITRACE_MAX];
    uint32_t sum[FUJITRACE_MAX];     /* each packet's whole bytes, hashed */
    int head, count;                 /* head: the next slot */
    uint64_t serial;
    int pending;                     /* the newest packet may be an exchange's repeat */
    uint8_t acc[2][FUJITRACE_FRAME_MAX];  /* the frame being decoded, by to_host */
    size_t acclen[2];
    int esc[2];
} fujitrace;

void fujitrace_init(fujitrace *t);
/* dir 1: bytes the console sent; dir 0: bytes it read. Any amount, split
 * anywhere. */
void fujitrace_feed(fujitrace *t, int dir, const uint8_t *buf, size_t len);
/* The newest `max` packets, oldest first; returns the count. */
int  fujitrace_read(const fujitrace *t, chfdebug_fuji_packet *out, int max);
/* Empties the ring. Serials keep counting, so a window sees the change. */
void fujitrace_clear(fujitrace *t);

#ifdef __cplusplus
}
#endif

#endif /* CHF_FUJITRACE_H */
