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

/* An analog stick engages a direction at half deflection and lets go below
 * 0.35: wide enough that a thumb resting near the threshold cannot flutter. */
#define CHF_STICK_ENTER 0.5f
#define CHF_STICK_EXIT  0.35f

/* The left stick's (or the D-pad's) direction bits as the controller's stick
 * contacts, a mask over chf_action: UP is FORWARD, DOWN is BACK. Opposite
 * directions cancel, so a stick pushed both ways holds neither. Pure. */
unsigned chf_acts_from_dir(int dir);

/* The right stick's direction bits as the knob's other two axes: LEFT and
 * RIGHT twist it (counter-clockwise, clockwise), UP pulls and DOWN pushes.
 * Opposites cancel. Pure. */
unsigned chf_knob_acts_from_dir(int dir);

/* An analog stick as an eight-way switch with hysteresis: an axis engages
 * past `enter` and releases below `exit` (both 0..1 of full deflection),
 * so a thumb resting near the threshold cannot flutter. x and y are SDL's
 * (+x right, +y down); `prev` is the previous result. Pure. */
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
