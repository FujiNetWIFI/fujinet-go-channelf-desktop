/* chf_internal.h -- the emulated machine, shared by the core modules, the host
 * and the debugger engine.
 *
 * The machine logic is transposed from MAME's Fairchild Channel F driver
 * (GPL-2.0-or-later for the driver, BSD-3-Clause for the CPU and the cartridge
 * bus; see COMPLIANCE.md):
 *
 *   src/mame/fairchild/channelf.cpp      the memory map, the ports and their
 *                                        latches, the panel and the controllers
 *   src/mame/fairchild/channelf_v.cpp    the VRAM's palette and the picture (video.c)
 *   src/mame/fairchild/channelf_a.cpp    the tone (sound.c)
 *   src/devices/cpu/f8/f8.cpp            the Fairchild F8 (f8.c)
 *   src/devices/bus/chanf/               the cartridge slot (carts.c) and the
 *                                        FujiNet cartridge (fujinet_cart.c)
 *
 * Field names keep MAME's own (minus the m_ prefix) where there is one.
 *
 * Time is the CPU's clock: f8_t.cycles counts MAME's icount units, the F8's
 * own PHI clock (1789772 Hz NTSC, 2 MHz PAL; a short bus cycle costs 4, a long
 * one 6). MAME's scheduler runs the CPU until the next timer and fires the
 * timer after the instruction that crossed it; the machine reproduces that
 * with integer attosecond arithmetic (machine.c): the k-th vertical blank
 * (MAME's screen_update, which draws the WHOLE VRAM at once, and the frame
 * notifier) is due at k * frame_as attoseconds, the CPU's cycle c is at
 * c * cycle_as, and the frame fires on the first instruction boundary at or
 * after floor(k * frame_as / cycle_as) cycles. The same time drives the sound
 * stream's sample index, the FujiNet cartridge's 60 Hz tick and the beam.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CHF_INTERNAL_H
#define CHF_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "chfsession.h"      /* CHF_MACHINE_*, CHF_ROM_* */
#include "carts.h"
#include "f8.h"
#include "fujinet_cart.h"
#include "sound.h"
#include "video.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the picture: VRAM columns 4-105 x rows 4-61, 2 x 4 pixels a cell ---- */
#define CHF_FB_WIDTH      CF_FRAME_W            /* 204 */
#define CHF_FB_HEIGHT     CF_FRAME_H            /* 232 */

/* ---- the clocks (channelf.cpp) ----
 * NTSC: F8 at 3.579545 MHz / 2 and a screen set_raw(3.579545 MHz * 8 / 7, 256,
 * 8, 212, 264, 16, 248) = 60.53 Hz. PAL: 4 MHz / 2 and set_raw(4 MHz, 256, 8,
 * 212, 312, 20, 310) = 50.08 Hz. MAME truncates both to u32 Hz. */
#define CF_NTSC_CPU_HZ    1789772u
#define CF_NTSC_PIXEL_HZ  4090908u
#define CF_PAL_CPU_HZ     2000000u
#define CF_PAL_PIXEL_HZ   4000000u
#define CF_HTOTAL         256
#define CF_NTSC_LINES     264       /* vtotal; the vertical blank begins at line 248 */
#define CF_NTSC_VBSTART   248
#define CF_NTSC_VBEND     16
#define CF_PAL_LINES      312       /* ... and at line 310 */
#define CF_PAL_VBSTART    310
#define CF_PAL_VBEND      20

/* ---- the BIOS ---- */
#define CF_BIOS_HALF      0x400
#define CF_BIOS_SIZE      0x800     /* $0000-$07FF: sl31253 / sl90025, then sl31254 */
/* The app's stand-in for the BIOS (stub.c, generated from stub.asm by
 * tools/stub/build-stub.sh): the whole 2K. What it does not implement is $FF,
 * an illegal opcode: executing one below $0800 under the stand-in parks the
 * machine (the session says "Import BIOS"). */
extern const uint8_t chf_stub_rom[CF_BIOS_SIZE];

/* ---- sound ---- */
#define CHF_AUDIO_RATE    SOUND_RATE            /* MAME's stream rate: 48 kHz */
#define CHF_AUDIO_MAX     2048                  /* stereo frames buffered per run_frame */

/* ---- inputs ----
 * The console buttons: bit k = button k + 1 (0 TIME, 1 MODE, 2 HOLD, 3 START),
 * port 0 bits 0-3. A hand controller: one bit per chf_action (the hardware
 * bit of the controller byte, 1 = held). Player 1 = the RIGHT controller
 * (CPU port 1), player 2 = the LEFT controller (CPU port 4). */

typedef struct chf_mach chf_machine_t;

/* ---- the debugger's spaces, for peek / poke (chfdebug.h's numbering) ---- */
#define CHF_PEEK_PROG     0       /* the CPU's 64K as a read would see it, no side effects */
#define CHF_PEEK_SCRATCH  1       /* the 64 scratchpad registers */
#define CHF_PEEK_VRAM     2       /* one byte per cell at row * 128 + col, 8192 */
#define CHF_PEEK_CART     3       /* the cartridge's own memory (see chf_machine_space_size) */
#define CHF_PEEK_IO       4       /* the ports as the CPU would read them now, 256 */

/* ---- hooks ----
 * instr_hook fires at every instruction boundary on the emulation thread,
 * BEFORE the instruction; it may block (that is how the debugger pauses the
 * machine). bus_hook fires on the accesses selected by `watch`. */
#define CHF_WATCH_MEMR 0x01       /* program-space data reads (ROMC 02: not fetches) */
#define CHF_WATCH_MEMW 0x02       /* program-space writes (ROMC 05) */
#define CHF_WATCH_IOR  0x04       /* port reads (INS / IN) */
#define CHF_WATCH_IOW  0x08       /* port writes (OUTS / OUT) */
#define CHF_WATCH_VRAM 0x10       /* VRAM stores (port 0 strobes) */

#define CHF_BUS_MEMR  1           /* addr = the address, data = the byte */
#define CHF_BUS_MEMW  2
#define CHF_BUS_IOR   3           /* addr = the port, data = the byte read */
#define CHF_BUS_IOW   4           /* addr = the port, data = the byte written */
#define CHF_BUS_VRAM  5           /* addr = row * 128 + col, data = the value; m->vw has the rest */

/* One VRAM store, as the debugger logs it. */
typedef struct {
    uint64_t cycle;               /* the CPU clock when the strobe hit */
    uint32_t frame;               /* vertical blanks completed before it */
    int16_t line;                 /* the beam line */
    uint16_t pc;                  /* the OUT instruction */
    uint8_t row, col, value, old;
} chf_vwrite_t;

#define CHF_VWRITE_RING 16384     /* power of two */

/* The debugger-only video bookkeeping, allocated by chf_machine_track(m, true)
 * and gone with chf_machine_track(m, false): while a debugger window holds the
 * engine attached. The machine fills it on every VRAM store and on every
 * vertical blank. */
typedef struct {
    /* the log of stores, newest at ring[(total - 1) & (RING - 1)] */
    chf_vwrite_t ring[CHF_VWRITE_RING];
    uint64_t total;

    /* per cell, since tracking began */
    struct {
        uint64_t cycle;           /* the last store's clock */
        uint32_t frame;
        uint32_t writes;          /* stores that hit this cell */
        int32_t pc;               /* the last writer's PC, -1 if never written */
        int16_t line;
        uint8_t old;              /* the value before that store */
    } cell[CF_VRAM_SIZE];

    /* the CRT: rows[0 .. beam_rows) were copied from the VRAM as the beam
     * passed them this frame (so a row drawn before a store shows the old
     * value, the tearing MAME's whole-frame update hides); the rest still hold
     * the previous frame's. The palette columns are in the copy. */
    uint8_t beam[CF_VRAM_SIZE];
    int beam_rows;

    /* what the display shows: the VRAM at the last vertical blank, and the one
     * before it (rows whose palette changed, cells that changed) */
    uint8_t frame_vram[CF_VRAM_SIZE];
    uint8_t prev_vram[CF_VRAM_SIZE];

    /* stores per cell this frame (saturating) and in the last whole one */
    uint8_t heat[CF_VRAM_SIZE];
    uint8_t heat_last[CF_VRAM_SIZE];
    uint32_t strobes_frame, strobes_last;     /* stores */
    uint32_t changes_frame, changes_last;     /* ... that changed the cell */
} chf_vtrack_t;

struct chf_mach {
    f8_t cpu;
    cf_video_t vid;
    sound_t snd;
    int machine;                  /* CHF_MACHINE_* */
    int is_pal;
    float route_gain;             /* the speaker's gain: 1.0, 0.5 on the Videoplay 2 group */

    /* the timing (attoseconds, MAME's integer arithmetic) */
    uint64_t cycle_as;            /* one CPU clock: 1e18 / cpu_hz */
    uint64_t frame_as;            /* the screen's frame period */
    uint64_t scan_as;             /* a line: frame_as / lines */
    uint64_t pixel_as;            /* a pixel: frame_as / (lines * 256) */
    int lines, vbstart, vbend, y_rpt;
    double cycle_hz;              /* the CPU clock, for display */

    /* the vertical blank: frame k (k = frames_done + 1) is due at k * frame_as */
    uint64_t frames_done;         /* vertical blanks fired: screen_update calls */
    uint64_t next_limit;          /* the CPU clock the next one waits for */
    uint64_t frame_stop;          /* run_frame: stop when frames_done reaches this (0: off) */

    /* the BIOS: the real halves or the stand-in */
    uint8_t rom[CF_BIOS_SIZE];
    bool have_bios;
    int bios_id;                  /* CHF_ROM_SL31253 / CHF_ROM_SL90025, -1 the stand-in */
    bool parked;                  /* the stand-in met something it cannot run */
    char park_why[96];
    unsigned illegal_seen;        /* cpu.illegal as last looked at */
    unsigned fj_swaps;            /* FujiNet swaps seen: to catch a game arriving */

    /* the slot: the cartridge in it (a mapper, or the FujiNet cart), and the
     * FujiNet cart itself when it is the one (NULL for a game) */
    chf_cart_t *cart;
    chf_fujinet_t *fujinet;

    /* inputs: live = set by any thread; the rest latched at each frame */
    unsigned panel_live, panel;
    uint8_t ctl_live[2], ctl[2];
    bool reset_live, reset_held;

    /* audio: mono samples the tone made since the last run_frame, then mixed
     * up to stereo float at CHF_AUDIO_RATE */
    float audio_gain;             /* route_gain by default; the tests that compare with a MAME recording set 1 */
    float mono[CHF_AUDIO_MAX];
    int nmono;
    float out[2 * CHF_AUDIO_MAX];
    int out_frames;
    uint64_t audio_samples;       /* produced since power-on */

    /* the picture the last vertical blank drew */
    uint32_t fb[CHF_FB_WIDTH * CHF_FB_HEIGHT];
    uint64_t frame_count;         /* run_frame calls */

    /* ---- debugger ---- */
    void (*instr_hook)(chf_machine_t *m, void *user);
    void *instr_hook_user;
    void (*bus_hook)(chf_machine_t *m, void *user, int kind, uint16_t addr, uint8_t data);
    void *bus_hook_user;
    uint8_t watch;                /* CHF_WATCH_* */
    chf_vtrack_t *track;          /* NULL unless a debugger window holds the engine */
    chf_vwrite_t vw;              /* the VRAM store the bus hook is being told about */
};

/* machine.c */
typedef struct {
    int machine;                  /* CHF_MACHINE_* */
    /* the BIOS: both halves (1K each, copied), or both NULL for the stand-in */
    const uint8_t *bios_lo, *bios_hi;
    int bios_id;                  /* which first half bios_lo is (CHF_ROM_SL31253 / CHF_ROM_SL90025) */
    chf_cart_t *cart;             /* the cartridge in the slot (not owned); NULL: an empty slot */
    chf_fujinet_t *fujinet;       /* ... and the FujiNet cart when it is that one */
} chf_machine_opts_t;

/* The machines' own names and standards (chfsession.h declares both). */
const char *chf_machine_name(int machine);
int chf_machine_is_pal(int machine);

/* Power on. The machine does not own the cart; the caller powers it on first. */
void chf_machine_init(chf_machine_t *m, const chf_machine_opts_t *opts);
/* After the FujiNet cartridge has been powered on (it is not part of the
 * machine): with the stand-in as the BIOS, an image that does not claim the
 * mailbox is a game the stand-in cannot run, and parks the console. */
void chf_machine_check_cart(chf_machine_t *m);
/* Run until the next vertical blank; m->fb holds the picture, m->out the audio
 * of the time that took, and inputs are latched at its start. */
void chf_machine_run_frame(chf_machine_t *m);
/* One instruction, hooks and all (a vertical blank that is due fires first).
 * Returns the CPU clocks it took; 0 if run_frame's frame had just fired. */
int chf_machine_step(chf_machine_t *m);
/* One instruction with no debugger hooks (the debugger's own stepping). */
int chf_machine_step_quiet(chf_machine_t *m);
/* Latch the live inputs (the frame boundary): the panel and the controllers,
 * and RESET -- pressing it resets the F8 (only: chf_machine_reset), releasing
 * it lets it run from $0000. */
void chf_machine_latch_inputs(chf_machine_t *m);
/* The console's RESET: the F8's reset (ROMC 08); the scratchpad, the accumulator,
 * the VRAM, the port latches and the sound survive, and a Videocart's latches
 * go back to power-on (MAME's device_reset). The FujiNet cartridge has no reset
 * pin and sees nothing. */
void chf_machine_reset(chf_machine_t *m);

/* Mix the tone made since the last call into m->out (stereo float) and return
 * the frame count (also m->out_frames). run_frame does this itself; the
 * debugger's loops that step through frames themselves call it per frame. */
int chf_machine_flush_audio(chf_machine_t *m);
/* The picture as it stands now (the debugger shows a frame being drawn). */
void chf_machine_snapshot(chf_machine_t *m, uint32_t *dst);

/* Machine time, in CPU clocks. */
static inline uint64_t chf_machine_now(const chf_machine_t *m)
{
    return m->cpu.cycles;
}
/* Machine time in attoseconds modulo 2^64 is not enough for a long session;
 * split into whole seconds and attoseconds, as MAME's attotime. */
void chf_machine_time(const chf_machine_t *m, uint64_t *sec, uint64_t *attos);
/* Where the beam is: the line (0 .. lines - 1; the picture's visible lines are
 * vbend .. vbstart - 1 and the vertical blank begins at vbstart), and how many
 * CPU clocks into that line. */
void chf_machine_beam(const chf_machine_t *m, int *line, int *cycle_in_line);
/* The VRAM row the beam draws at a line (line / y_rpt; 64+ past the VRAM). */
static inline int chf_machine_line_row(const chf_machine_t *m, int line)
{
    return line / m->y_rpt;
}

/* ---- the debugger's access ---- */
/* Size of a space. */
uint32_t chf_machine_space_size(const chf_machine_t *m, int space);
/* peek never disturbs the machine (no mailbox hotspot, no 2102 read). */
uint8_t chf_machine_peek(chf_machine_t *m, int space, uint32_t addr);
/* poke: PROG writes a Videocart's RAM or the FujiNet cartridge's RAM and image
 * (never the BIOS, a ROM, or a mailbox hotspot); IO acts as if the CPU had
 * written the port (a strobe draws a pixel, port 5 changes the tone). */
void chf_machine_poke(chf_machine_t *m, int space, uint32_t addr, uint8_t data);
/* The ports as the CPU would read them now, with the live console inputs. */
uint8_t chf_machine_port_read(chf_machine_t *m, unsigned port);
/* The sound mode port 5 selects now (0 off .. 3). */
static inline int chf_machine_sound_mode(const chf_machine_t *m)
{
    return m->snd.mode;
}
/* Begin or end the debugger's video bookkeeping (chf_vtrack_t). Under the run
 * lock. Returns 0, or -1 if out of memory. */
int chf_machine_track(chf_machine_t *m, bool on);
/* The most recent VRAM stores, newest last: copies up to `max` into out and
 * returns the count (0 when not tracking). */
int chf_machine_vwrites(const chf_machine_t *m, chf_vwrite_t *out, int max);

/* What the stand-in BIOS could not do (CHF_MISSING_*), and why; 0 if nothing. */
unsigned chf_machine_missing(const chf_machine_t *m, char *why, int whysz);

/* The frame period in seconds (1e18 / frame_as): 60.5303 NTSC, 50.0801 PAL. */
static inline double chf_machine_frame_hz(const chf_machine_t *m)
{
    return 1e18 / (double)m->frame_as;
}

#ifdef __cplusplus
}
#endif

#endif /* CHF_INTERNAL_H */
