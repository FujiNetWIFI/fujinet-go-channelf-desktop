/*
 * host.h -- the emulator host: owns the machine and the FujiNet cartridge,
 * runs the machine on its own thread at real speed, and publishes video
 * frames and audio for the session layer.
 *
 * Process-singleton by design, like every core in this family (and the
 * FujiNet cart below it is one too: fujimail's port interface is context-
 * free C function pointers). chf_host_start refuses a second concurrent
 * session.
 *
 * Pacing: the machine runs a whole frame (to the next VBLANK) at a time
 * against an absolute monotonic deadline ladder at the machine's exact frame
 * rate -- NTSC 60.53 Hz, PAL 50.08 Hz. While a frontend feeds display ticks
 * near that rate (chf_host_notify_vsync) the machine phase-locks one frame
 * per tick instead (only possible near 60 Hz: a 50 Hz machine on a 60 Hz
 * display stays on the wall clock). The siblings' hardest-won lesson applies: VERIFY
 * the pacing -- the boot_smoke test measures it.
 *
 * Energy: a frame identical to its predecessor is not republished, the frame
 * callback tells a frontend when there is something to draw, and the machine
 * sleeps to its deadline rather than spinning.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CHF_HOST_H
#define CHF_HOST_H

#include <stdbool.h>
#include <stdint.h>

#include "chf_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int machine;                 /* CHF_MACHINE_* (chfsession.h) */

    /* The BIOS: the two 1K halves ($0000 and $0400), copied, or both NULL for
     * the app's stand-in (core/chf/stub.c). bios_id says which first half it
     * is (CHF_ROM_SL31253 / CHF_ROM_SL90025), -1 for the stand-in. */
    const uint8_t *bios_lo, *bios_hi;
    int bios_id;

    const uint8_t *cart;         /* an opened image, copied; NULL boots CONFIG */
    uint32_t cart_size;
    int cart_type;               /* CHF_CART_* (carts.h) from chf_cart_identify */
    bool cart_app;               /* a FujiNet app (claims the mailbox): served
                                    by the FujiNet cartridge, mailbox live.
                                    Otherwise a game: the Videocart for
                                    cart_type sits in the slot instead, as a
                                    real cartridge would, and FujiNet is not
                                    reachable until the next power cycle. */
    const char *boip_hostport;   /* fujinet-pc's BoIP listener, "host:port" */
    bool cart_sync;              /* run the mailbox inline (tests) */
} chf_host_opts_t;

/* Start the machine thread. Returns 0, or -1 with chf_host_last_error()
 * set (already running, an image the cartridge cannot map, allocation or
 * thread failure). */
int chf_host_start(const chf_host_opts_t *opts);

/* Stop and join the machine thread and power the cart off. Safe when not
 * running. */
void chf_host_stop(void);

bool chf_host_is_running(void);
const char *chf_host_last_error(void);

/* Pause: the machine thread parks between frames and the audio goes silent
 * (the session's pause; the debugger's own stop is separate). */
void chf_host_set_paused(bool paused);
bool chf_host_is_paused(void);

/* ---- video: latest-wins single slot ----
 * Copies the newest frame into dst (CHF_FB_WIDTH x CHF_FB_HEIGHT XRGB words)
 * when its serial differs from *serial_inout; returns nonzero and updates the
 * serial when it copied. */
int chf_host_frame_copy(uint32_t *dst, uint64_t *serial_inout);
/* Called on the machine thread right after each new frame is published, so a
 * frontend with no frame clock can draw when there is something to draw
 * instead of polling. Must be quick and must not call back into the host (it
 * runs with a lock held: set_frame_callback(NULL, NULL) waits for a call in
 * flight, so the context may be freed once it returns). */
typedef void (*chf_host_frame_cb)(void *ctx);
void chf_host_set_frame_callback(chf_host_frame_cb cb, void *ctx);
/* Frames the machine has run since start (for pacing checks). */
uint64_t chf_host_frame_count(void);
/* How the deadline ladder fared (boot_smoke prints it): sleeps taken, how
 * late they woke in total and at worst, and resyncs after falling more
 * than four frames behind. Read after chf_host_stop. */
typedef struct {
    uint64_t sleeps, late_ns, resyncs;
    long worst_late_ns;
} chf_host_pacing_t;
void chf_host_pacing(chf_host_pacing_t *out);
/* The running machine's exact frame rate in Hz (0 when stopped). */
double chf_host_frame_rate(void);

void chf_host_notify_vsync(int64_t frame_time_ns);

/* What the stand-in BIOS could not do (chfsession.h CHF_MISSING_*), and why,
 * since the last start: a Videocart without the FUJI claim, or a call to a
 * BIOS address the stand-in does not implement (the machine parks there). */
unsigned chf_host_missing(char *why, int whysz);

/* ---- audio: 48 kHz interleaved stereo float (mono mirrored), self-
 * correcting backlog. Returns frames written (no padding). */
int chf_host_audio_copy(float *dst, int max_frames);
void chf_host_audio_reset(void);

/* ---- inputs (callable from any thread; latched by the machine once a
 * frame, at VBLANK, as MAME's input ports are) ---- */
/* The console buttons: bit k = button k + 1 held (0 TIME, 1 MODE, 2 HOLD,
 * 3 START; port 0 bits 0-3). */
void chf_host_keys_set(unsigned buttons);
/* A hand controller: port 0 = player 1 = the RIGHT controller (CPU port 1),
 * port 1 = player 2 = the LEFT controller (CPU port 4); mask = chf_action
 * bits held (1 = held; the machine complements them onto the port). */
void chf_host_controller_set(int port, unsigned mask);
/* The console's RESET: the F8's reset (ROMC 08), held while down. */
void chf_host_reset_set(bool down);
/* Pressed for a few frames and released (the menus' and the controllers
 * window's RESET when it is clicked rather than held). */
void chf_host_reset_pulse(void);
void chf_host_release_all(void);

/* ---- the cartridge ---- */
void chf_host_cart_status(chf_cart_status_t *out);
/* The cart device, or NULL when not running (the debugger's page map and
 * mailbox views). Use it under chf_host_lock. */
chf_cart_t *chf_host_cart(void);

/* ---- the run lock ----
 * Held by the machine thread while it runs a frame. Any other thread that
 * reads or edits the machine or the cart (chf_host_machine, chf_host_cart)
 * holds it around the access: it waits for the frame in progress, at most a
 * couple of milliseconds. Re-entrant per thread; a no-op on the machine
 * thread. Never call chf_host_stop while holding it. */
void chf_host_lock(void);
void chf_host_unlock(void);
/* The debugger's park: the machine thread lets go of the run lock while it
 * is parked inside the instruction hook, and takes it back to run on. */
void chf_host_park_release(void);
void chf_host_park_reacquire(void);

/* ---- debugger plumbing ----
 * The live machine; valid only while running, and only touched under
 * chf_host_lock (or on the machine thread). */
chf_machine_t *chf_host_machine(void);

/* Instruction-boundary hook (machine thread; may block). */
void chf_host_set_instr_hook(void (*hook)(chf_machine_t *m, void *user), void *user);
/* Bus hook, fired only for the access kinds in `watch` (CHF_WATCH_*). */
void chf_host_set_bus_hook(void (*hook)(chf_machine_t *m, void *user, int kind,
                                        uint16_t addr, uint8_t data),
                           void *user, uint8_t watch);
/* Publish the machine's current framebuffer now (the debugger shows the
 * picture being drawn while stopped). Machine thread only. */
void chf_host_publish_frame(void);

#ifdef __cplusplus
}
#endif

#endif /* CHF_HOST_H */
