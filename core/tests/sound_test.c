/*
 * sound_test -- the console's tone (core/chf/sound.c) against MAME's own recording.
 *
 * The golden is what MAME's `-wavwrite` of the same state machine (channelf_a.cpp,
 * 48 kHz) wrote for a probe that switches port 5 to the 1 kHz tone: the first eight
 * samples, a few later ones, and a hash of 3000 of them -- the 96-sample forced-on
 * window, the decay with its ~9 ms half life and its integer truncation, the 1 kHz
 * square wave gated by the phase counter. tools/ab/sound_ab.py checks the whole stream
 * of every machine against MAME; this holds the arithmetic where the CI can run it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "sound.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        failures++;
}

static int16_t s16(float v)
{
    int i = (int)(v * 32768.0f);

    return (int16_t)(i > 32767 ? 32767 : i < -32768 ? -32768 : i);
}

int main(void)
{
    static float out[8192];
    static int16_t pcm[8192];
    sound_t s;
    int n = 0;
    uint64_t h = 14695981039346656037ull;
    int edges = 0;

    sound_init(&s);
    check(s.incr == 2730 && s.min_ontime == 96, "48 kHz: the phase step is 2730 (65536 / 24, truncated), the forced on-time 96 samples");
    check(s.mode == 0 && s.envelope == 0, "power-on: off, no envelope");

    /* silence first: mode 0 makes samples of 0 and freezes everything */
    sound_run(&s, 229, out, &n, 8192);
    check(n == 229 && s.produced == 229 && s.counter == 0 && s.envelope == 0, "229 samples of silence; the phase counter did not move");
    for (int i = 0; i < n; i++)
        if (out[i] != 0.0f)
            n = -1;
    check(n == 229, "all zero");
    n = 0;

    /* the 1 kHz tone from sample 229, as MAME recorded it */
    check(sound_set_mode(&s, 1) == 1 && sound_set_mode(&s, 1) == 0, "a change of mode is reported once; the same mode again is not a change");
    sound_run(&s, 229 + 3000, out, &n, 8192);
    check(n == 3000, "3000 samples produced");
    for (int i = 0; i < n; i++)
        pcm[i] = s16(out[i]);
    {
        static const int16_t first[8] = { 32767, 32714, 32661, 32608, 32555, 32502, 32449, 32396 };

        check(memcmp(pcm, first, sizeof first) == 0, "the first eight samples: 32767 decaying by x0.99838 a sample, truncated");
    }
    check(pcm[47] == 30364 && pcm[48] == 30315 && pcm[95] == 28090 && pcm[96] == 28044,
          "samples 47, 48, 95, 96 as MAME has them (the forced on-time covers the first 97)");
    check(pcm[97] == 0 && pcm[100] == 0 && pcm[200] == 0 && pcm[1000] == 6339 && pcm[3000 - 1] == 0 &&
              pcm[500] == 0,
          "after it the tone is a square wave gated by the phase counter (bit 16, 48 samples a period)");
    for (int i = 0; i < n; i++) {
        h ^= (uint16_t)pcm[i];
        h *= 1099511628211ull;
        if (i >= 96 && pcm[i] && !pcm[i - 1])
            edges++;
    }
    check(h == 0xea649c356a16a443ull, "the hash of the whole 3000 samples is MAME's");
    check(edges == 57, "57 periods of 1 kHz in the 2904 samples after the window (48 samples each, until the envelope decays away)");

    /* mode 0 freezes: the phase does not advance while off */
    {
        const uint32_t counter = s.counter;
        const int env = s.envelope;

        sound_set_mode(&s, 0);
        check(s.envelope == 0 && s.forced_ontime == 0, "off: envelope cleared");
        n = 0;
        sound_run(&s, s.produced + 500, out, &n, 8192);
        check(s.counter == counter && n == 500 && out[0] == 0.0f && out[499] == 0.0f, "500 silent samples, and the phase counter stands still");
        sound_set_mode(&s, 3);
        check(s.envelope == 32767 && s.forced_ontime == 96, "a tone retriggers the envelope to full scale and the forced-on window");
        (void)env;
    }
    /* the output buffer's capacity: samples past it are made, not stored */
    {
        const uint64_t before = s.produced;

        n = 0;
        sound_run(&s, before + 100, out, &n, 40);
        check(n == 40 && s.produced == before + 100, "a full buffer drops the surplus but the state advances");
        sound_run(&s, before + 200, NULL, NULL, 0);
        check(s.produced == before + 200, "and a NULL buffer only advances it");
    }

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
