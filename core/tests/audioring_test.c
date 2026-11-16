/*
 * audioring_test.c -- the rate-controlled ring holds its fill against a
 * clock that does not match the producer's, with no hard trims.
 *
 * The producer makes 800 stereo frames every 1/59.92 s (the machine); the
 * consumer pulls 512-frame chunks on a 48 kHz device clock that is 0.08% FAST
 * or SLOW against it (a display-locked 60.00 Hz against 59.92 Hz is 0.13%).
 * Ten simulated minutes in each direction: the fill must stay inside the
 * ring, no emergency trim fires, nothing is dropped, and the stream stays
 * continuous (a ramp survives the resampler monotonic and gap-free once the
 * ring has primed).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../chf/audioring.h"

#define RATE   48000
#define TARGET (RATE / 30)
#define EMERG  (RATE / 4)
#define CHUNK  512

static int run(double dev_ratio, const char *name)
{
    static audioring r;
    float in[2 * 1024], out[2 * CHUNK];
    const double frame_s = 1.0 / 59.92;
    const int per_frame = (int)(RATE * frame_s + 0.5);   /* 801 */
    const double dev_hz = RATE * dev_ratio;
    double t_prod = 0, t_cons = 0, ramp = 0, last = -1;
    uint32_t minfill = ~0u, maxfill = 0;
    long underrun_calls = 0, pulled = 0;
    int gap = 0;

    audioring_init(&r);
    for (long step = 0; step < 60 * 10 * 120; step++) {
        if (t_prod <= t_cons) {
            for (int i = 0; i < per_frame; i++) {
                in[2 * i] = in[2 * i + 1] = (float)ramp;
                ramp += 1e-7;                 /* a slow ramp: continuity */
            }
            audioring_push(&r, in, per_frame, TARGET);
            t_prod += frame_s;
        } else {
            int n = audioring_pull(&r, out, CHUNK, TARGET, EMERG);
            if (n < CHUNK) underrun_calls++;
            for (int i = 0; i < n; i++) {
                if (last >= 0 && out[2 * i] < last) gap = 1;   /* went back */
                if (last >= 0 && out[2 * i] - last > 4e-7) gap = 1;
                last = out[2 * i];
            }
            pulled += n;
            t_cons += CHUNK / dev_hz;
            uint32_t f = audioring_fill(&r);
            if (t_cons > 2.0) {              /* past the start-up */
                if (f < minfill) minfill = f;
                if (f > maxfill) maxfill = f;
            }
        }
    }
    printf("%s: fill %u..%u (target %d), overruns %u, trims %u, "
           "short pulls %ld, pulled %ld\n", name, minfill, maxfill, TARGET,
           atomic_load(&r.overruns), atomic_load(&r.trims), underrun_calls, pulled);
    if (atomic_load(&r.trims) || atomic_load(&r.overruns)) return 1;
    if (maxfill > TARGET * 3) return 1;
    if (underrun_calls > 4) return 1;     /* start-up priming only */
    if (gap) { printf("%s: stream not continuous\n", name); return 1; }
    return 0;
}

int main(void)
{
    int bad = 0;
    bad |= run(1.0008, "device 0.08% fast");
    bad |= run(0.9992, "device 0.08% slow");
    bad |= run(1.0, "device exact");
    printf(bad ? "FAIL\n" : "PASS\n");
    return bad;
}
