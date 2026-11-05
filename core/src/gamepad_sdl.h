/*
 * gamepad_sdl -- private interface: the pure helpers unit-tested without
 * hardware, and the start/stop the session calls.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CHF_GAMEPAD_SDL_H
#define CHF_GAMEPAD_SDL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Direction bits a stick or D-pad resolves to. */
#define CHF_DIR_UP    1
#define CHF_DIR_DOWN  2
#define CHF_DIR_LEFT  4
#define CHF_DIR_RIGHT 8

/* The stick directions a set of direction bits holds, as a mask over
 * chf_action (bit n = action n: UP RIGHT DOWN LEFT). Opposite directions
 * cancel, so a stick pushed both ways holds neither. Pure. */
unsigned chf_acts_from_dir(int dir);

/* The left stick as the analog disc (x, y in -1..1, SDL's convention: +x
 * right, +y down) to the pots' -128..127, 0 = centre, with a radial dead
 * zone (0.12) rescaled so the edge of the dead zone is 0 and full
 * deflection +-127.
 * Pure. */
void chf_disc_from_stick(float x, float y, int *dx, int *dy);

/* An analog stick as an eight-way switch with hysteresis: an axis engages
 * past `enter` and releases below `exit` (both 0..1 of full deflection),
 * so a thumb resting near the threshold cannot flutter. `prev` is the
 * previous result. Pure. */
int chf_dir_from_stick(float x, float y, int prev, float enter, float exit);

/* Which pad drives `port`: an explicit assignment wins; among the rest,
 * connection order (first pad to port 0, second to port 1). `assign[i]` is
 * pad i's assignment (-1 = automatic). Returns the pad index or -1. Pure. */
int chf_pad_for_port(const int *assign, int npads, int port);

/* SDL3's SDL_GamepadButton (a uint8_t) to the public numbering, and back.
 * Explicit tables, never a cast, so an SDL reordering cannot desync them. */
int chf_pad_button_from_sdl(uint8_t button);
int chf_sdl_button_from_pad(int button);

#ifdef __cplusplus
}
#endif

#endif /* CHF_GAMEPAD_SDL_H */
