/* sound.c -- the UVI's beeper. See sound.h.
 *
 * Transposed from MAME's src/mame/channelf/arcadia_a.cpp
 * (GPL-2.0-or-later, copyright-holders David Viens, Peter Trauner).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "sound.h"

#include <string.h>

/* lfsr is 9 bits long (and the same as the Atari TIA's pure noise) */
#define LFSR_MASK (1 << 8)
/* makes alien invaders' samples noise sync */
#define LFSR_INIT 0x00f0

void sound_init(sound_t *s)
{
    memset(s, 0, sizeof *s);
    s->lfsr = LFSR_INIT;        /* device_start */
    s->tval = 1;
}

void sound_write(sound_t *s, unsigned offset, uint8_t data)
{
    s->reg[offset] = data;
    switch (offset) {
    case 1:
        /* as per Gobbler samples: the freq counter is only applied on the
         * next change in the flip flop */
        s->size = data & 0x7f;
        if (!s->size)
            s->lfsr = LFSR_INIT;        /* reset LFSR */
        break;
    case 2:
        s->volume = (data & 0x07) * 0x800;
        s->mode = (data & 0x18) >> 3;
        if (s->mode != s->omode)
            s->tval = 0;
        s->omode = s->mode;
        break;
    }
}

static float step(sound_t *s)
{
    int result = 0;

    /* if minimal pitch ? */
    if (s->reg[1]) {
        switch (s->mode) {
        case 0: break;                                          /* nothing */
        case 1: result = (int)s->volume * s->tval; break;       /* tone only */
        case 2: result = (int)s->volume * s->nval; break;       /* noise only */
        case 3: result = (int)s->volume * (s->tval & s->nval); break;
        }
        s->pos++;
        if (s->pos >= s->size) {
            /* the new noise bit (taps: 0000T000T) */
            unsigned char new_bit = s->lfsr & 1;

            new_bit = (unsigned char)(new_bit ^ ((s->lfsr & 0x10) ? 1 : 0));
            s->nval = s->lfsr & 1;
            s->lfsr = s->lfsr >> 1;
            if (new_bit)
                s->lfsr |= LFSR_MASK;
            s->tval = !s->tval;
            s->pos = 0;
        }
    }
    return (float)result / 32768.0f;
}

void sound_run(sound_t *s, uint64_t total)
{
    while (s->produced < total) {
        const float v = step(s);

        s->produced++;
        if (s->qcount == SOUND_QUEUE) {         /* nobody is mixing: drop the oldest */
            s->qhead = (s->qhead + 1) % SOUND_QUEUE;
            s->qcount--;
        }
        s->queue[(s->qhead + s->qcount) % SOUND_QUEUE] = v;
        s->qcount++;
    }
}

int sound_mix(sound_t *s, float *out, int n, int rate)
{
    const double step_in = (double)SOUND_RATE / (double)rate;
    float last = 0.0f;

    for (int i = 0; i < n; i++) {
        double pos = s->mix_pos, end = pos + step_in, acc = 0.0;
        unsigned idx = 0;

        /* box average over [pos, end) of the queue */
        for (double p = pos; p < end - 1e-12; idx++) {
            const double next = (double)(idx + 1) + 0.0;
            const double seg_end = next < end ? next : end;
            const double from = p < (double)idx ? (double)idx : p;
            float v;

            if (seg_end <= from) {
                p = seg_end;
                continue;
            }
            v = idx < s->qcount ? s->queue[(s->qhead + idx) % SOUND_QUEUE] : last;
            last = v;
            acc += (double)v * (seg_end - from);
            p = seg_end;
        }
        out[i] = (float)(acc / step_in);
        s->mix_pos = end;
        {   /* drop whole input samples already consumed */
            unsigned drop = (unsigned)s->mix_pos;

            if (drop > s->qcount) {             /* underrun: hold, do not run away */
                s->mix_pos -= (double)(drop - s->qcount);
                drop = s->qcount;
            }
            if (drop) {
                last = s->queue[(s->qhead + drop - 1) % SOUND_QUEUE];
                s->qhead = (s->qhead + drop) % SOUND_QUEUE;
                s->qcount -= drop;
                s->mix_pos -= (double)drop;
            }
        }
    }
    return n;
}
