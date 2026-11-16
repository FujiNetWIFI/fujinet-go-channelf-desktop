/*
 * audioring.h -- the rate-controlled sample ring between the machine thread
 * (producer) and the audio device callback (consumer): interleaved stereo
 * float.
 *
 * The machine makes one frame of samples per frame of emulated time; the
 * device drains on its own clock. The two never agree exactly (a vsync-locked
 * 60.00 Hz frame clock against the machine's 59.92 Hz), so the old ring
 * slowly overfilled and then HARD-TRIMMED to its target: an audible click every
 * half minute or so. Here the producer runs a linear resampler that steps up
 * to 0.5% faster or slower to hold the fill near the target, so the drift is
 * absorbed as an inaudible pitch change of a few cents instead.
 *
 * Same design as the coleco / atari800 / atari7800 / MS-DOS desktops (their
 * rings are mono int16; this one is the SMS's stereo float).
 *
 * Single producer, single consumer, acquire/release indices, no locks.
 * Header-only so the test links nothing.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SMS_AUDIORING_H
#define SMS_AUDIORING_H

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#define AR_FRAMES    16384u             /* stereo frames; power of two */
#define AR_MASK      (AR_FRAMES - 1u)
#define AR_MAX_STEP  0.005              /* the resampler's largest nudge */

typedef struct {
    float buf[AR_FRAMES * 2];
    _Atomic uint32_t rd;                /* consumer's */
    _Atomic uint32_t wr;                /* producer's */
    _Atomic int flush;                  /* producer -> consumer: drop it all */
    /* producer-only */
    double pos;
    float last[2];
    /* consumer-only */
    int primed;
    /* counters, for the test and for curiosity */
    _Atomic uint32_t overruns;          /* frames the producer had to drop */
    _Atomic uint32_t trims;             /* emergency trims by the consumer */
} audioring;

/* Not thread-safe: only before both sides run. */
static inline void audioring_init(audioring *r)
{
    memset(r, 0, sizeof *r);
}

/* Asks the consumer to empty the ring (reset, unpause). Safe from the
 * producer: only the consumer ever moves `rd`. */
static inline void audioring_flush(audioring *r)
{
    atomic_store(&r->flush, 1);
}

/* Producer start-up: forget the resampler's phase. Touches producer-only
 * state, so it is safe while a consumer is already pulling. */
static inline void audioring_producer_reset(audioring *r)
{
    r->pos = 0.0;
    r->last[0] = r->last[1] = 0.0f;
    audioring_flush(r);
}

static inline uint32_t audioring_fill(audioring *r)
{
    uint32_t w = atomic_load_explicit(&r->wr, memory_order_acquire);
    uint32_t rd = atomic_load_explicit(&r->rd, memory_order_acquire);
    return (w - rd) & AR_MASK;
}

/* Producer: push `frames` stereo frames, resampled toward `target` fill. */
static inline void audioring_push(audioring *r, const float *in, int frames,
                                  uint32_t target)
{
    uint32_t w = atomic_load_explicit(&r->wr, memory_order_relaxed);
    uint32_t rd = atomic_load_explicit(&r->rd, memory_order_acquire);
    double fill = (double)((w - rd) & AR_MASK);
    double err = (fill - (double)target) / (double)target;
    double step = err * AR_MAX_STEP;
    double pos = r->pos;
    uint32_t dropped = 0;

    if (frames <= 0) return;
    if (step > AR_MAX_STEP) step = AR_MAX_STEP;
    if (step < -AR_MAX_STEP) step = -AR_MAX_STEP;
    /* A fuller ring plays its input FASTER (a bigger step consumes input
     * quicker, so fewer output frames per input frame); an emptier one
     * stretches it. */
    step += 1.0;

    while (pos < (double)frames) {
        int i = (int)pos;
        float frac = (float)(pos - i);
        const float *a = i == 0 ? r->last : in + 2 * (i - 1);
        const float *b = in + 2 * i;
        uint32_t next = (w + 1) & AR_MASK;
        if (next != rd) {
            r->buf[2 * w]     = a[0] + (b[0] - a[0]) * frac;
            r->buf[2 * w + 1] = a[1] + (b[1] - a[1]) * frac;
            w = next;
        } else {
            dropped++;
        }
        pos += step;
    }
    r->pos = pos - (double)frames;
    r->last[0] = in[2 * (frames - 1)];
    r->last[1] = in[2 * (frames - 1) + 1];
    atomic_store_explicit(&r->wr, w, memory_order_release);
    if (dropped) atomic_fetch_add(&r->overruns, dropped);
}

/* Consumer: copy up to `n` stereo frames and return how many were real. The
 * caller zero-fills the rest.
 *
 * Priming: no output until `target` frames have accumulated, so one frame's
 * burst does not dribble out as several short requests; primed again only
 * once the ring runs completely dry.
 *
 * `emergency` is far beyond anything rate control produces (a stall on the
 * producer's side, a long pause): the oldest audio is dropped down to the
 * target rather than played late. */
static inline int audioring_pull(audioring *r, float *out, int n,
                                 uint32_t target, uint32_t emergency)
{
    uint32_t rd = atomic_load_explicit(&r->rd, memory_order_relaxed);
    uint32_t w = atomic_load_explicit(&r->wr, memory_order_acquire);
    int i = 0;

    if (atomic_exchange(&r->flush, 0)) {
        rd = w;
        r->primed = 0;
    }
    if (((w - rd) & AR_MASK) > emergency) {
        rd = (w - target) & AR_MASK;
        atomic_fetch_add(&r->trims, 1);
    }
    if (!r->primed) {
        if (((w - rd) & AR_MASK) < target) {
            atomic_store_explicit(&r->rd, rd, memory_order_release);
            return 0;
        }
        r->primed = 1;
    }
    for (; i < n && rd != w; i++) {
        out[2 * i]     = r->buf[2 * rd];
        out[2 * i + 1] = r->buf[2 * rd + 1];
        rd = (rd + 1) & AR_MASK;
    }
    if (rd == w) r->primed = 0;
    atomic_store_explicit(&r->rd, rd, memory_order_release);
    return i;
}

#endif
