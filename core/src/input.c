/*
 * input -- every way a hand controller, a console button or RESET gets held,
 * and the one result the machine sees.
 *
 * The inputs are held by sources (session_internal.h SRC_*: the keyboard and
 * the controllers window's buttons, the pointer's drag, a wheel pulse, and for
 * each gamepad its buttons and its sticks), one bit per source in an array per
 * contact, so letting go of one source never releases another's. After every
 * change the result is handed to the host (chf_host_controller_set,
 * chf_host_keys_set, chf_host_reset_set), which latches it at the frame
 * boundary as MAME's input ports do.
 *
 * A controller's contacts are four opposed pairs (RIGHT/LEFT, BACK/FORWARD,
 * TWIST_CCW/TWIST_CW, PULL/PUSH): a thumb that holds both of a pair holds
 * neither, as MAME's 8-way inputs do. What the console sees is what
 * chfsession_buttons_held reports.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdlib.h>
#include <string.h>

#include "compat.h"
#include "chfdebug.h"
#include "host.h"
#include "session_internal.h"

/* A wheel notch is held this many frames, so a program that polls the
 * controller once a frame sees it. */
#define PULSE_FRAMES 6

void input_init(struct chfsession *s)
{
    pthread_cond_init(&s->pulse_cond, NULL);
}

/* ---- what the console sees ---------------------------------------------------- */

static const unsigned k_pairs[4][2] = {
    { 1u << CHF_ACT_RIGHT, 1u << CHF_ACT_LEFT },
    { 1u << CHF_ACT_BACK, 1u << CHF_ACT_FORWARD },
    { 1u << CHF_ACT_TWIST_CCW, 1u << CHF_ACT_TWIST_CW },
    { 1u << CHF_ACT_PULL, 1u << CHF_ACT_PUSH },
};

/* input_mtx held. */
static unsigned held_locked(const struct chfsession *s, int port)
{
    unsigned mask = 0;

    for (int a = 0; a < CHF_ACT_PER_PORT; a++)
        if (s->pad_src[port][a])
            mask |= 1u << a;
    for (int p = 0; p < 4; p++)
        if ((mask & k_pairs[p][0]) && (mask & k_pairs[p][1]))
            mask &= ~(k_pairs[p][0] | k_pairs[p][1]);
    return mask;
}

static void push_controller_locked(struct chfsession *s, int port)
{
    chf_host_controller_set(port, held_locked(s, port));
}

static void push_keys_locked(struct chfsession *s)
{
    unsigned buttons = 0;

    for (int k = 0; k < CHF_KEY_COUNT; k++)
        if (s->key_src[k])
            buttons |= 1u << k;
    chf_host_keys_set(buttons);
}

static void push_reset_locked(struct chfsession *s)
{
    chf_host_reset_set(s->sw_src[CHF_SW_RESET] != 0);
}

static void push_all_locked(struct chfsession *s)
{
    for (int port = 0; port < CHF_PLAYERS; port++)
        push_controller_locked(s, port);
    push_keys_locked(s);
    push_reset_locked(s);
}

/* A start: nothing is held, and the machine is told. */
void input_clear(struct chfsession *s)
{
    pthread_mutex_lock(&s->input_mtx);
    memset(s->pad_src, 0, sizeof s->pad_src);
    memset(s->key_src, 0, sizeof s->key_src);
    memset(s->sw_src, 0, sizeof s->sw_src);
    memset(s->pulse_until, 0, sizeof s->pulse_until);
    chf_host_release_all();
    pthread_mutex_unlock(&s->input_mtx);
}

/* A power cycle makes a new machine: whatever is still held (a key down
 * across Reset to CONFIG) is handed to it, and the pulses, counted in the old
 * machine's frames, are over. */
void input_after_power_cycle(struct chfsession *s)
{
    pthread_mutex_lock(&s->input_mtx);
    for (int port = 0; port < CHF_PLAYERS; port++)
        for (int i = 0; i < 2; i++)
            if (s->pulse_until[port][i]) {
                s->pulse_until[port][i] = 0;
                s->pad_src[port][i ? CHF_ACT_TWIST_CW : CHF_ACT_TWIST_CCW] &= ~SRC_PULSE;
            }
    push_all_locked(s);
    pthread_mutex_unlock(&s->input_mtx);
}

static void set_src(uint32_t *word, uint32_t src, int down)
{
    if (down)
        *word |= src;
    else
        *word &= ~src;
}

/* One source of one target; system actions are not held inputs and are left
 * to the caller. */
static void source_press(struct chfsession *s, int target, uint32_t src, int down)
{
    pthread_mutex_lock(&s->input_mtx);
    if (target < CHF_PLAYERS * CHF_ACT_PER_PORT) {
        const int port = target / CHF_ACT_PER_PORT;
        set_src(&s->pad_src[port][target % CHF_ACT_PER_PORT], src, down);
        push_controller_locked(s, port);
    } else if (target < CHF_TARGET_SWITCH(0)) {
        set_src(&s->key_src[target - CHF_TARGET_KEY(0)], src, down);
        push_keys_locked(s);
    } else if (target < CHF_TARGET_SYSACT(0)) {
        set_src(&s->sw_src[target - CHF_TARGET_SWITCH(0)], src, down);
        push_reset_locked(s);
    }
    pthread_mutex_unlock(&s->input_mtx);
}

/* ---- the public input API ------------------------------------------------------- */

void chfsession_press(chfsession *s, int target, int down)
{
    if (!s->running || target < 0 || target >= CHF_TARGET_COUNT)
        return;
    if (target >= CHF_TARGET_SYSACT(0)) {
        if (down)
            chfsession_sysaction(s, target - CHF_TARGET_SYSACT(0));
        return;
    }
    source_press(s, target, SRC_KEYBOARD, down);
}

void chfsession_sysaction(chfsession *s, int sysact)
{
    switch (sysact) {
    case CHF_SYSACT_RESET_CONFIG:
        chfsession_reset_to_config(s);
        break;
    case CHF_SYSACT_DEBUG_STOP:
        if (s->running) {
            chfdebug *d = chfdebug_get(s);
            chfdebug_attach(d);
            chfdebug_stop(d);
        }
        break;
    default:
        break;
    }
}

void chfsession_sysaction_post(chfsession *s, int sysact)
{
    if (sysact < 0 || sysact >= CHF_SYSACT_COUNT)
        return;
    pthread_mutex_lock(&s->sysact_mtx);
    s->sysact_pending |= 1u << sysact;
    pthread_mutex_unlock(&s->sysact_mtx);
}

int chfsession_sysaction_take(chfsession *s, int *out)
{
    int found = 0;
    pthread_mutex_lock(&s->sysact_mtx);
    for (int i = 0; i < CHF_SYSACT_COUNT; i++) {
        if (s->sysact_pending & (1u << i)) {
            s->sysact_pending &= ~(1u << i);
            if (out)
                *out = i;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&s->sysact_mtx);
    return found;
}

unsigned chfsession_buttons_held(chfsession *s, int port)
{
    unsigned held;
    if (port < 0 || port >= CHF_PLAYERS)
        return 0;
    pthread_mutex_lock(&s->input_mtx);
    held = held_locked(s, port);
    pthread_mutex_unlock(&s->input_mtx);
    return held;
}

void chfsession_pointer_set(chfsession *s, int port, unsigned mask)
{
    if (!s->running || port < 0 || port >= CHF_PLAYERS)
        return;
    pthread_mutex_lock(&s->input_mtx);
    for (int a = 0; a < CHF_ACT_PER_PORT; a++)
        set_src(&s->pad_src[port][a], SRC_POINTER, (mask >> a) & 1u);
    push_controller_locked(s, port);
    pthread_mutex_unlock(&s->input_mtx);
}

int chfsession_key_held(chfsession *s, int key)
{
    int held;
    if (key < 0 || key >= CHF_KEY_COUNT)
        return 0;
    pthread_mutex_lock(&s->input_mtx);
    held = s->key_src[key] != 0;
    pthread_mutex_unlock(&s->input_mtx);
    return held;
}

int chfsession_switch_held(chfsession *s, int sw)
{
    int held;
    if (sw < 0 || sw >= CHF_SW_COUNT)
        return 0;
    pthread_mutex_lock(&s->input_mtx);
    held = s->sw_src[sw] != 0;
    pthread_mutex_unlock(&s->input_mtx);
    return held;
}

/* Everything the keyboard and the controllers window's buttons hold, let go
 * (focus loss): what a key's press asserted, whichever way it was asserted.
 * A gamepad's own holds are left to the gamepad, the pointer's to
 * chfsession_pointer_set. */
static void release_keyboard_sources(struct chfsession *s)
{
    pthread_mutex_lock(&s->input_mtx);
    for (int port = 0; port < CHF_PLAYERS; port++) {
        for (int a = 0; a < CHF_ACT_PER_PORT; a++)
            s->pad_src[port][a] &= ~SRC_KEYBOARD;
        push_controller_locked(s, port);
    }
    for (int k = 0; k < CHF_KEY_COUNT; k++)
        s->key_src[k] &= ~SRC_KEYBOARD;
    push_keys_locked(s);
    for (int w = 0; w < CHF_SW_COUNT; w++)
        s->sw_src[w] &= ~SRC_KEYBOARD;
    push_reset_locked(s);
    pthread_mutex_unlock(&s->input_mtx);
}

void chfsession_release_all(chfsession *s)
{
    if (!s)
        return;
    memset(s->held_keysym, 0, sizeof s->held_keysym);
    release_keyboard_sources(s);
}

/* The gamepad thread's entry points: the same routing, from the pad's own
 * source bits, never touching the settings or the debugger. */
void session_gamepad_press(struct chfsession *s, int owner, int target, int down)
{
    if (!s->running || owner < 0 || owner >= CHF_MAX_PADS || target < 0
        || target >= CHF_TARGET_SYSACT(0))
        return;
    source_press(s, target, SRC_PAD(owner), down);
}

void session_gamepad_stick(struct chfsession *s, int owner, int port, int act, int down)
{
    if (!s->running || owner < 0 || owner >= CHF_MAX_PADS || port < 0 || port >= CHF_PLAYERS
        || act < 0 || act >= CHF_ACT_PER_PORT)
        return;
    source_press(s, CHF_TARGET_PORT(port, act), SRC_STICK(owner), down);
}

void chfsession_set_right_stick(chfsession *s, int on)
{
    s->opts.right_stick = on ? 1 : 0;
    chfsession_set_int(s, "right_stick", s->opts.right_stick);
}

/* ---- the wheel's twist pulses ------------------------------------------------------ */

/* Lets each pulse go once the machine has run its frames. Waits on pulse_cond
 * while there is none. */
static void *pulse_main(void *arg)
{
    struct chfsession *s = arg;

    pthread_mutex_lock(&s->input_mtx);
    while (!s->pulse_quit) {
        int active = 0;

        for (int port = 0; port < CHF_PLAYERS; port++)
            for (int i = 0; i < 2; i++)
                active |= s->pulse_until[port][i] != 0;
        if (!active) {
            pthread_cond_wait(&s->pulse_cond, &s->input_mtx);
            continue;
        }
        pthread_mutex_unlock(&s->input_mtx);
        chf_sleep_ms(2);
        pthread_mutex_lock(&s->input_mtx);
        {
            const uint64_t now = chf_host_frame_count();

            for (int port = 0; port < CHF_PLAYERS; port++) {
                int changed = 0;

                for (int i = 0; i < 2; i++) {
                    if (s->pulse_until[port][i] && now >= s->pulse_until[port][i]) {
                        s->pulse_until[port][i] = 0;
                        s->pad_src[port][i ? CHF_ACT_TWIST_CW : CHF_ACT_TWIST_CCW] &= ~SRC_PULSE;
                        changed = 1;
                    }
                }
                if (changed)
                    push_controller_locked(s, port);
            }
        }
    }
    pthread_mutex_unlock(&s->input_mtx);
    return NULL;
}

void chfsession_twist_pulse(chfsession *s, int port, int dir)
{
    const int i = dir > 0;           /* 0: counter-clockwise, 1: clockwise */

    if (!s->running || port < 0 || port >= CHF_PLAYERS || dir == 0)
        return;
    pthread_mutex_lock(&s->input_mtx);
    if (!s->pulse_thread_up) {
        s->pulse_quit = 0;
        if (pthread_create(&s->pulse_thread, NULL, pulse_main, s) != 0) {
            pthread_mutex_unlock(&s->input_mtx);
            return;
        }
        s->pulse_thread_up = 1;
    }
    /* a notch the other way ends the one before it */
    s->pulse_until[port][!i] = 0;
    s->pad_src[port][i ? CHF_ACT_TWIST_CCW : CHF_ACT_TWIST_CW] &= ~SRC_PULSE;
    s->pulse_until[port][i] = chf_host_frame_count() + PULSE_FRAMES;
    s->pad_src[port][i ? CHF_ACT_TWIST_CW : CHF_ACT_TWIST_CCW] |= SRC_PULSE;
    push_controller_locked(s, port);
    pthread_cond_signal(&s->pulse_cond);
    pthread_mutex_unlock(&s->input_mtx);
}

void input_stop(struct chfsession *s)
{
    pthread_mutex_lock(&s->input_mtx);
    if (!s->pulse_thread_up) {
        pthread_mutex_unlock(&s->input_mtx);
        return;
    }
    s->pulse_quit = 1;
    pthread_cond_signal(&s->pulse_cond);
    pthread_mutex_unlock(&s->input_mtx);
    pthread_join(s->pulse_thread, NULL);
    pthread_mutex_lock(&s->input_mtx);
    s->pulse_thread_up = 0;
    s->pulse_quit = 0;
    memset(s->pulse_until, 0, sizeof s->pulse_until);
    for (int port = 0; port < CHF_PLAYERS; port++) {
        s->pad_src[port][CHF_ACT_TWIST_CCW] &= ~SRC_PULSE;
        s->pad_src[port][CHF_ACT_TWIST_CW] &= ~SRC_PULSE;
    }
    pthread_mutex_unlock(&s->input_mtx);
}
