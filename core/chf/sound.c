/* sound.c -- the console's tone. See sound.h.
 *
 * Transposed from MAME's channelf_a.cpp (GPL-2.0-or-later, copyright-holders
 * Juergen Buchmueller, Frank Palazzolo, Sean Riddle).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "sound.h"

#include <math.h>
#include <string.h>

#define MAX_AMPLITUDE 0x7fff

void sound_init(sound_t *s)
{
    const int rate = SOUND_RATE;

    memset(s, 0, sizeof *s);
    /* 2V = 1000 Hz; a DDS that overflows bit 16 at that rate (and bit 17 =
     * 4V, bit 18 = 8V ...): the step to add per sample */
    s->incr = (int)(65536.0 / (rate / 1000.0 / 2.0));
    /* the minimum forced on-time, in samples (about 2 ms, estimated) */
    s->min_ontime = rate / 1000 * 2;
    /* measured: a decay with a half life of ~9 ms (the multiplier per sample) */
    s->decay_mult = (float)exp((-0.693 / 9e-3) / rate);
    s->envelope = 0;
}

int sound_set_mode(sound_t *s, int mode)
{
    if (mode == s->mode)
        return 0;
    s->mode = mode;
    s->changes++;
    switch (mode) {
    case 0:
        s->envelope = 0;
        s->forced_ontime = 0;
        break;
    case 1:
    case 2:
    case 3:
        s->envelope = MAX_AMPLITUDE;
        s->forced_ontime = s->min_ontime;
        break;
    }
    return 1;
}

void sound_run(sound_t *s, uint64_t total, float *out, int *n, int cap)
{
    uint32_t mask = 0, target = 0;

    if (s->produced >= total)
        return;
    switch (s->mode) {
    case 1: mask = target = 0x00010000; break;     /* high tone (2V), 1000 Hz */
    case 2: mask = target = 0x00020000; break;     /* medium tone (4V), 500 Hz */
    case 3: mask = target = 0x00140000; break;     /* low, weird tone (32V & 8V) */
    }

    while (s->produced < total) {
        float v = 0.0f;

        if (s->mode != 0) {
            if (s->forced_ontime > 0 || (s->counter & mask) == target)
                v = (float)((double)s->envelope / 32768.0);
            s->counter += (uint32_t)s->incr;
            s->envelope = (int)((float)s->envelope * s->decay_mult);
            if (s->forced_ontime > 0)
                s->forced_ontime--;
        }
        s->level = v;
        s->produced++;
        if (out && n && *n < cap)
            out[(*n)++] = v;
    }
}
