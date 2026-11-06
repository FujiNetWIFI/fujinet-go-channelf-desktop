/* fujinet_cart.h -- the FujiNet Channel F cartridge, the one cart with a
 * network behind it.
 *
 * A C transposition of the firmware's own MAME device model (fujinet-firmware
 * pico/channelf/emu/fujinet.cpp, BSD-3-Clause, Thomas Cherryhomes): an RP2040
 * on the cartridge connector that is a peer on the F8 bus. It owns $0800-$FFFF:
 * the 16K ROM window at $0800 (CONFIG, or an app's first 16K), the APPBANK
 * page window at $4800-$67FF, 30K of RAM at $8000-$F7FF, and the mailbox at
 * $F800-$FFFF (the reply, the $FC00 status page with the 60 Hz tick at
 * $FC0E/F, the hotspots the console WRITES: $FD00+n registers, $FD80+n page
 * select, $FDFE swap, $FF00 TX). The bus decode (chf_read / chf_write in
 * channelf_cart.h), the image planner (chfmap.c), the mailbox protocol
 * (fujimail.c) and the wire codec (fujibus.c) are the cartridge firmware's own
 * sources, staged verbatim into fuji-generated/ by cmake/StageFujiProto.cmake;
 * this device is only the port.
 *
 * It sits in the cartridge slot through the chf_cart_t interface (carts.h),
 * reached for every read at $0800 up and every write (ROMC 05) at $0800 up.
 * What it serves at power-on is the baked CONFIG, or an opened FujiNet app;
 * the image the client mounts is staged beside it and swapped in when the
 * client writes $FDFE, armed.
 *
 * Like the cartridge, the mailbox service runs on its own thread (the RP2040's
 * core0) while the bus is served on the emulation thread (core1), so a slow
 * network transaction never stalls the picture; a synchronous mode runs it
 * inline instead, which is deterministic for tests and is what MAME does.
 *
 * Unlike MAME's device it reconnects: a link that drops is dialled again
 * before the next transaction. A console RESET (ROMC 08) does not reach it:
 * the image, the page, the RAM and the mailbox's sequence survive, as on the
 * hardware (the connector has no reset pin).
 *
 * Single instance per process: fujimail's port interface is C function
 * pointers with no context argument.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CHF_FUJINET_CART_H
#define CHF_FUJINET_CART_H

#include <stdbool.h>
#include <stdint.h>

#include "carts.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct chf_fujinet chf_fujinet_t;

/* Create the (single) cart; NULL if one already exists. */
chf_fujinet_t *chf_fujinet_create(void);
void chf_fujinet_destroy(chf_fujinet_t *c);

/* The slot interface (valid until destroy). */
chf_cart_t *chf_fujinet_cart(chf_fujinet_t *c);

/* The cart's time, for the 60 Hz tick it paints at $FC0E/F (MAME's
 * machine().time().as_ticks(60)): the machine hands it the CPU's cycle counter
 * (read on the emulation thread) and the CPU clock. */
void chf_fujinet_set_clock(chf_fujinet_t *c, const uint64_t *cycles, double cycle_hz);

/* The CPU's PC0 register (read on the emulation thread), for the debugger's event
 * ring: the instruction making a write is at *pc0 - 1. */
void chf_fujinet_set_pc(chf_fujinet_t *c, const uint16_t *pc0);

/* Power on, as MAME's device_reset with the image it was given:
 *   - `image` NULL: the baked CONFIG client (fujiconfigrom.h), mailbox live;
 *   - an image: served in its place; the mailbox is live when it carries the
 *     "FUJI" claim, dead otherwise (a plain game in the FujiNet slot).
 * `boip` is fujinet-pc's BoIP listener ("host:port"), or "" for none at all
 * (FujiNet switched off: nothing is dialled, every transaction fails at once).
 * Returns 0, or -1 with the reason (an image the cartridge cannot plan) in
 * `why`. */
int chf_fujinet_power_on(chf_fujinet_t *c, const uint8_t *image, uint32_t len,
                         const char *boip, bool sync, char *why, int whysz);
/* Power off: stop the mailbox worker, close the link. */
void chf_fujinet_power_off(chf_fujinet_t *c);

/* Bring what the mailbox worker published into the slot (also done on every
 * fetch); the host calls it once a frame. */
void chf_fujinet_frame(chf_fujinet_t *c);

/* ---- status, for the session and the debugger ---- */
typedef struct {
    bool powered;
    bool mailbox;             /* the running image owns the mailbox pages */
    bool link_up;             /* the BoIP socket is connected */
    bool link_off;            /* FujiNet is switched off: nothing is dialled */
    bool app_live;            /* the running image is an APPBANK one */
    bool booted_game;         /* anything other than the baked CONFIG has been served */
    bool armed;               /* the client armed the swap: the next $FDFE write takes it */
    bool staged_app;          /* a staged image exists and it is an APPBANK one */
    bool staged_mailbox;      /* ... and it claims the mailbox */
    uint32_t live_size, live_crc;
    uint32_t staged_size, staged_crc;
    unsigned swaps;           /* images swapped in since power-on */
    unsigned npages;          /* APPBANK pages after the 16K window (0-6) */
    int page;                 /* the page $4800 shows, -1 none */
    uint16_t tick;            /* the tick as painted now */
    int boot_state, boot_pct, boot_err;
    uint8_t ackseq, status, err, reply_cmd;
    uint16_t rxlen;
    unsigned queue;           /* hotspot reads waiting for the worker */
} chf_cart_status_t;

void chf_fujinet_status(chf_fujinet_t *c, chf_cart_status_t *out);

/* The mailbox ($F800-$FFFF: the reply, the status page, the hotspot pages) as
 * the console would read it, with no side effects (the debugger's Cart tab). */
#define CHF_FJ_MAILBOX_BYTES 0x800
void chf_fujinet_mailbox(chf_fujinet_t *c, uint8_t out[CHF_FJ_MAILBOX_BYTES]);

/* The running image's byte at an image offset (0 .. CHFMAP_STORE-1), no side
 * effects (the debugger's CART space). Returns 0xFF past the image. The
 * arena (the console's $8000-$FFFF: RAM and mailbox) is chf_fujinet_arena. */
uint8_t chf_fujinet_image_byte(chf_fujinet_t *c, uint32_t offset);
/* ... and an edit of it: the running image's own buffer, never a hotspot. */
void chf_fujinet_image_poke(chf_fujinet_t *c, uint32_t offset, uint8_t data);

/* What the console reads at `addr` ($0800-$FFFF), with no side effects (the
 * tick as painted now); and a debugger edit of what a read there returns: the
 * image window, the APPBANK page in view, the RAM and the painted mailbox
 * pages. An edit never fires a hotspot ($FD00 up is ignored). */
uint8_t chf_fujinet_peek(chf_fujinet_t *c, uint16_t addr);
void chf_fujinet_poke(chf_fujinet_t *c, uint16_t addr, uint8_t data);

/* The arena byte at the console's address $8000 + offset (0 .. $7FFF), no
 * side effects; and a debugger edit of it (never a hotspot). */
uint8_t chf_fujinet_arena(chf_fujinet_t *c, uint32_t offset);
void chf_fujinet_arena_poke(chf_fujinet_t *c, uint32_t offset, uint8_t data);

/* The cart's events (register writes, TX bytes, page selects, swaps) since
 * power-on, for the debugger: a ring the debugger drains with a cursor. kind
 * 1 REG, 2 TX, 3 BANK, 4 SWAP. Returns the events copied after *cursor and
 * advances it. */
typedef struct { uint64_t cycle; uint16_t pc; uint8_t kind, reg, value; } chf_fj_event_t;
int chf_fujinet_events(chf_fujinet_t *c, uint64_t *cursor, chf_fj_event_t *out, int max);

/* The image served at power-on or by the last swap, as the debugger's Save
 * wants it. Returns bytes copied. */
uint32_t chf_fujinet_copy_image(chf_fujinet_t *c, uint8_t *dst, uint32_t max);

/* 1 if `image` (len bytes) can run on the cartridge, with *claims set when it
 * carries the claim (a FujiNet app); else 0 and why. */
int chf_fujinet_check_image(const uint8_t *image, uint32_t len, bool *claims,
                            char *why, int whysz);

#ifdef __cplusplus
}
#endif

#endif /* CHF_FUJINET_CART_H */
