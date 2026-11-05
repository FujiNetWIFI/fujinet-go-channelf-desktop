/* sound.h -- the 2637 UVI's sound: one tone/noise beeper.
 *
 * Transposed from MAME's src/mame/channelf/arcadia_a.cpp (GPL-2.0-or-later,
 * copyright-holders David Viens, Peter Trauner; refined by plgDavid from
 * recordings of an MPT-03).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * MAME's stream runs at a fixed 15625 Hz (the PAL UVI's sound clock) on every
 * clone, one state-machine step per sample. The machine brings the chip up to
 * the CPU's time (sound_run) before each write to $18FD/$18FE and at the end of
 * every frame, then sound_mix() resamples what was produced to 48 kHz by
 * box-averaging.
 */

#ifndef CHF_SOUND_H
#define CHF_SOUND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOUND_RATE   15625
#define SOUND_QUEUE  4096           /* produced samples waiting to be mixed */

typedef struct {
    uint8_t reg[3];
    int size, pos, tval, nval;
    unsigned mode, omode;
    unsigned volume;
    unsigned lfsr;

    uint64_t produced;              /* samples generated since power-on */
    float queue[SOUND_QUEUE];
    unsigned qhead, qcount;         /* ring of the samples not yet mixed */
    double mix_pos;                 /* fractional position into the queue head */
} sound_t;

void sound_init(sound_t *s);
/* MAME's write(offset & 3, data): offset 1 = $18FD (pitch), 2 = $18FE (volume
 * and mode). Call sound_run() first. */
void sound_write(sound_t *s, unsigned offset, uint8_t data);
/* Generate samples until `total` have been produced since power-on. */
void sound_run(sound_t *s, uint64_t total);
/* Mix n output samples at `rate` Hz from the queue (box average); fewer
 * input than needed holds the last. Returns n. */
int sound_mix(sound_t *s, float *out, int n, int rate);

#ifdef __cplusplus
}
#endif

#endif /* CHF_SOUND_H */
