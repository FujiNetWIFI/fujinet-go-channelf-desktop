/* chf_internal.h -- the emulated machine, shared by the core modules.
 *
 * The machine logic is transposed from MAME's Fairchild Channel F driver
 * (GPL-2.0-or-later for the driver, BSD-3-Clause for the CPU and the cartridge
 * bus; see COMPLIANCE.md):
 *
 *   src/mame/channelf/channelf.cpp        the memory map, the keypads and the panel,
 *                                       the 262 * 60 Hz line timer, SENSE = vblank
 *   src/mame/channelf/arcadia_v.cpp      the 2637 UVI: video, pots (uvi.c)
 *   src/mame/channelf/arcadia_a.cpp      its beeper (sound.c)
 *   src/devices/cpu/s2650/              the Signetics 2650 (s2650.c)
 *   src/devices/bus/channelf/            the cartridge slot (carts.c) and the
 *                                       FujiNet cartridge (fujinet_cart.c)
 *
 * Field names keep MAME's own (minus the m_ prefix) where there is one.
 *
 * Time is the CPU's clock: s2650_t.cycles counts MAME's icount units (the
 * 2650 at 3.58 MHz / 4 = 895 kHz executes 6, 9 or 12 of them per
 * instruction). MAME's scheduler runs the CPU until the next timer and fires
 * the timer after the instruction that crossed it; the machine reproduces that
 * with integer attosecond arithmetic (machine.c: line_limit()), so a register
 * written on the instruction before a line lands before that line is drawn,
 * exactly as in MAME.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CHF_INTERNAL_H
#define CHF_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "chfsession.h"      /* CHF_MACHINE_* */
#include "carts.h"
#include "fujinet_cart.h"
#include "s2650.h"
#include "sound.h"
#include "uvi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the picture: MAME's visible area, 192 x 262 ----
 * 128 pixels of screen with a 32-pixel border each side (the objects and the
 * scroll reach into it), 262 lines. The same size for the PAL row. */
#define CHF_FB_WIDTH      UVI_WIDTH
#define CHF_FB_HEIGHT     UVI_HEIGHT

/* ---- clocks (channelf.cpp) ----
 * NTSC: 3.58 MHz / 4 = 0.895 MHz, 262 lines at 60 Hz. PAL clones run
 * 4.433619 MHz / 5, 312 lines at 50 Hz (MAME runs every clone at NTSC: the PAL
 * row is not verified against it). */
#define CHF_NTSC_CPU_HZ   (3580000.0 / 4.0)
#define CHF_PAL_CPU_HZ    (4433619.0 / 5.0)
#define CHF_NTSC_LINES    262
#define CHF_PAL_LINES     312
#define CHF_NTSC_FRAME_HZ 60
#define CHF_PAL_FRAME_HZ  50

/* ---- sound ---- */
#define CHF_AUDIO_RATE    48000
#define CHF_AUDIO_MAX     2048          /* stereo frames buffered per frame */
/* The UVI's final output level is quiet (volume 7 = 0.44 of full scale, one
 * polarity); the host brings it up. The tests that compare with a MAME
 * recording set audio_gain to 1. */
#define CHF_AUDIO_BOOST   1.5f

/* ---- inputs ---- */
/* The key matrix (host.h): bit i = held. 0-11 player 1 keypad, 12-23 player 2
 * keypad, in this order: */
enum {
    CHF_K_1, CHF_K_2, CHF_K_3, CHF_K_4, CHF_K_5, CHF_K_6, CHF_K_7, CHF_K_8, CHF_K_9,
    CHF_K_CLEAR, CHF_K_0, CHF_K_ENTER,
    CHF_K_PAD_COUNT                     /* 12 */
};
/* matrix bits 24 START, 25 OPTION, 26 SELECT are the panel ($1908 b0-b2);
 * chfsession.h names them CHF_KEY_START/OPTION/SELECT */
#define CHF_KEYS_MASK     0x07ffffffull

/* A stick: bit 0 UP, 1 RIGHT, 2 DOWN, 3 LEFT, 4 FIRE */
#define CHF_STICK_UP      0x01
#define CHF_STICK_RIGHT   0x02
#define CHF_STICK_DOWN    0x04
#define CHF_STICK_LEFT    0x08
#define CHF_STICK_FIRE    0x10

typedef struct chf_machine chf_machine_t;

struct chf_machine {
    s2650_t cpu;
    uvi_t uvi;
    sound_t snd;
    int machine;                  /* CHF_MACHINE_ARCADIA / _ARCADIA_PAL */
    int is_pal;
    double cycle_hz;

    /* the line timer (set_periodic_int(video_line, 262 * 60 Hz)): the k-th tick
     * is due at k * line_as attoseconds; the CPU runs until its cycle counter
     * reaches floor(due / cycle_as) and the tick fires after the instruction
     * that got there (MAME's timeslice rule) */
    uint64_t line_as, cycle_as;
    uint64_t line_k;              /* ticks fired */
    uint64_t next_limit;          /* the CPU cycle count the next tick waits for */
    uint64_t frames_done;         /* frames the tick has completed */
    uint64_t frame_stop;          /* run_frame: stop when frames_done reaches this (0: off) */

    /* the slot: the cartridge in it (a mapper, or the FujiNet cart), and the
     * FujiNet cart itself when it is the one (NULL for a game) */
    chf_cart_t *cart;
    chf_fujinet_t *fujinet;

    /* inputs: live = set by any thread; the rest latched at each frame */
    uint64_t keys_live, keys;
    uint8_t joy_live[2], joy[2];
    int8_t disc_live[2][2], disc[2][2];
    bool reset_live, reset_held;

    /* audio, mono from the UVI mixed up to stereo float at CHF_AUDIO_RATE; one
     * frame's worth per run_frame */
    float audio_gain;
    uint64_t audio_cycles;                /* the CPU time the samples run to */
    double audio_frac;                    /* fractional sample carried over */
    uint64_t audio_samples;               /* samples produced since power-on */
    float out[2 * CHF_AUDIO_MAX];
    int out_frames;

    /* the picture the last run_frame finished */
    uint32_t fb[CHF_FB_WIDTH * CHF_FB_HEIGHT];
    uint64_t frame_count;

    /* ---- debugger ----
     * instr_hook fires at every instruction boundary on the emulation
     * thread, BEFORE the instruction; it may block (that is how the debugger
     * pauses the machine). bus_hook fires on memory and I/O accesses, only
     * while `watch` is nonzero. */
    void (*instr_hook)(chf_machine_t *m, void *user);
    void *instr_hook_user;
    void (*bus_hook)(chf_machine_t *m, void *user, int kind, uint16_t addr, uint8_t data);
    void *bus_hook_user;
    uint8_t watch;                /* CHF_WATCH_* */
};

#define CHF_WATCH_MEMR 0x01       /* program-space data reads (not fetches) */
#define CHF_WATCH_MEMW 0x02       /* program-space writes */
#define CHF_WATCH_IOR  0x04       /* port reads (REDC/REDD/REDE) */
#define CHF_WATCH_IOW  0x08       /* port writes */

#define CHF_BUS_MEMR  1
#define CHF_BUS_MEMW  2
#define CHF_BUS_IOR   3           /* addr = the port (S2650_CTRL_PORT ...) */
#define CHF_BUS_IOW   4

/* debugger spaces for peek / poke */
#define CHF_PEEK_PROG 0           /* the CPU's 15-bit space as a read would see it, no side effects */
#define CHF_PEEK_UVI  1           /* the UVI's register file, offset 0-$2FF (CPU $1800-$1AFF) */
#define CHF_PEEK_CART 2           /* the FujiNet cart's image, offset 0-$117FF; a game cart's ROM */

/* machine.c */
typedef struct {
    int machine;                  /* CHF_MACHINE_ARCADIA / _ARCADIA_PAL */
    chf_cart_t *cart;             /* the cartridge in the slot (not owned) */
    chf_fujinet_t *fujinet;       /* ... and the FujiNet cart when it is that one */
} chf_machine_opts_t;

int chf_machine_is_pal(int machine);
const char *chf_machine_name(int machine);

/* Power on. The machine does not own the cart; the caller powers it on first. */
void chf_machine_init(chf_machine_t *m, const chf_machine_opts_t *opts);
/* Run until the line timer completes the next frame; m->fb holds it, m->out
 * the audio of the time that took. */
void chf_machine_run_frame(chf_machine_t *m);
/* One instruction, hooks and all (any line tick due fires first). Returns the
 * CPU cycles it took. */
int chf_machine_step(chf_machine_t *m);
/* One instruction with no debugger hooks (the debugger's own stepping). */
int chf_machine_step_quiet(chf_machine_t *m);
/* Latch the live inputs (the frame boundary): the keypads, the panel, the
 * sticks, the discs and RESET -- pressing RESET resets the 2650 (only),
 * releasing it lets it run from $0000. */
void chf_machine_latch_inputs(chf_machine_t *m);
/* The picture as it stands now (the debugger shows a frame being drawn). */
void chf_machine_snapshot(chf_machine_t *m, uint32_t *dst);
/* Machine time, in CPU cycles. */
static inline uint64_t chf_machine_now(const chf_machine_t *m)
{
    return m->cpu.cycles;
}
/* Debugger access: peek never disturbs the machine (no mailbox hotspot); poke
 * writes the UVI's registers (with their write effects), or the cartridge's
 * own buffers. space is CHF_PEEK_*. */
uint8_t chf_machine_peek(chf_machine_t *m, int space, uint32_t addr);
void chf_machine_poke(chf_machine_t *m, int space, uint32_t addr, uint8_t data);
/* The keypad column byte the CPU would read at $1900 + n (n 0-7), the panel
 * at $1908. */
uint8_t chf_machine_pad(const chf_machine_t *m, int n);

#ifdef __cplusplus
}
#endif

#endif /* CHF_INTERNAL_H */
