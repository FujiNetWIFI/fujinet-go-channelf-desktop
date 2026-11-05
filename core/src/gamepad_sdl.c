/*
 * gamepad_sdl -- SDL3 gamepads (and unmapped joysticks) as Channel F
 * joysticks, on their own polling thread.
 *
 * A polling thread rather than SDL's event queue on the UI thread: this
 * app's video belongs to GTK/Qt/AppKit/Win32, not SDL, and only the gamepad
 * subsystem is initialised here -- never SDL video. Devices are added and
 * removed as they appear on the system: SDL posts the JOYSTICK_ and
 * GAMEPAD_ add/remove pairs and both are taken, with idempotent open/close,
 * so neither kind of device can be missed. A device SDL has no gamepad
 * mapping for (a plain HID adapter) is opened as a raw joystick instead:
 * its buttons and hats are bindable in the raw bands, just unnamed.
 *
 * What a pad drives is a stick (1 for the first pad, 2 for the second).
 * Every button goes through the bindings table: by default the D-pad is the
 * stick's four directions and South its button, and a button may be bound to
 * a keyboard key or RESET instead. With "analog sticks" on, the left analog
 * stick drives the console's analog disc (two pots, -128..127, radial
 * dead zone), the way the real controller's disc does; with it off it is an
 * eight-way switch with hysteresis instead. Hat 0 of a raw joystick is always
 * digital. The three ways of holding an input
 * -- a pad button, the analog stick, the keyboard -- are separate sources in
 * the session, so letting go of one never releases another. Ported from the
 * O2 sibling's gamepad_sdl.c.
 *
 * Pads are assigned to ports in connection order unless assigned explicitly.
 * A pad that is unplugged and plugged back in (matched by its SDL GUID and
 * name) gets its explicit assignment back, and every connect/disconnect is
 * reported to the session for the frontend's toast.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "gamepad_sdl.h"
#include "session_internal.h"

#define MAX_PADS CHF_MAX_PADS
#define MAX_HATS 4

/* A third of full deflection engages, a sixth releases: low enough that a
 * worn stick still registers, high enough that resting drift does not, and
 * a gap wide enough that jitter across one threshold cannot flutter. */
#define STICK_ENTER 0.35f
#define STICK_EXIT  0.15f
#define TRIGGER_ON  0.5f
#define DISC_DEADZONE 0.12f

typedef struct {
    SDL_JoystickID id;
    int owner;                /* the session's source number for this pad: stable
                               * while it is connected, whatever its slot */
    SDL_Gamepad *handle;      /* mapped gamepad, or NULL */
    SDL_Joystick *joy;        /* raw joystick when not a gamepad */
    int assign;               /* explicit port, -1 = automatic */
    int port;                 /* the port it drove on the last poll, or -1 */
    int stick_dir;            /* left stick's four-way state (hysteresis) */
    uint8_t sent[CHF_ACT_PER_PORT]; /* directions the analog stick / hat holds */
    int disc_x, disc_y;       /* the analog disc last sent for this pad */
    /* What each button's press asserted (target + 1, 0 = nothing), so its
     * release lets go of exactly that even if the binding changed meanwhile,
     * and a pad that goes away drops what it held. */
    int16_t btn_target[256];
    SDL_GUID guid;
    char name[64];
    int trigger_down[2];
    Uint8 last_hat[MAX_HATS];
} pad_slot;

typedef struct {
    struct chfsession *session;
    pthread_t thread;
    pthread_mutex_t lock;
    int running;
    atomic_int stop_requested;
    pad_slot pads[MAX_PADS];
    int npads;
    unsigned generation;
    /* Map-mode capture */
    int cap_active, cap_have, cap_result;
    /* explicit assignments of pads that went away, for when they come back */
    struct { SDL_GUID guid; char name[64]; int assign; int used; } gone[MAX_PADS];
    int gone_next;
} gamepad_state;

static gamepad_state *g_state;   /* one per process: SDL's subsystem is */

/* ---- pure helpers ----------------------------------------------------------- */

void chf_disc_from_stick(float x, float y, int *dx, int *dy)
{
    const float dead = DISC_DEADZONE;
    float mag = sqrtf(x * x + y * y);
    float scale;
    int ix, iy;

    if (mag < dead) {
        *dx = *dy = 0;
        return;
    }
    if (mag > 1.0f)
        mag = 1.0f;
    /* radial dead zone: the deflection past it, rescaled to 0..1 */
    scale = ((mag - dead) / (1.0f - dead)) / sqrtf(x * x + y * y);
    ix = (int)lroundf(x * scale * 127.0f);
    iy = (int)lroundf(y * scale * 127.0f);
    *dx = ix < -128 ? -128 : ix > 127 ? 127 : ix;
    *dy = iy < -128 ? -128 : iy > 127 ? 127 : iy;
}

unsigned chf_acts_from_dir(int dir)
{
    unsigned acts = 0;

    if ((dir & CHF_DIR_UP) && !(dir & CHF_DIR_DOWN)) acts |= 1u << CHF_ACT_UP;
    if ((dir & CHF_DIR_DOWN) && !(dir & CHF_DIR_UP)) acts |= 1u << CHF_ACT_DOWN;
    if ((dir & CHF_DIR_LEFT) && !(dir & CHF_DIR_RIGHT)) acts |= 1u << CHF_ACT_LEFT;
    if ((dir & CHF_DIR_RIGHT) && !(dir & CHF_DIR_LEFT)) acts |= 1u << CHF_ACT_RIGHT;
    return acts;
}

int chf_dir_from_stick(float x, float y, int prev, float enter, float exit)
{
    int dir = 0;
    /* each axis independently, so diagonals are two bits and a stick that
     * is exactly horizontal never picks up a vertical bit from noise */
    if (x <= -enter || (x <= -exit && (prev & CHF_DIR_LEFT))) dir |= CHF_DIR_LEFT;
    if (x >=  enter || (x >=  exit && (prev & CHF_DIR_RIGHT))) dir |= CHF_DIR_RIGHT;
    if (y <= -enter || (y <= -exit && (prev & CHF_DIR_UP))) dir |= CHF_DIR_UP;
    if (y >=  enter || (y >=  exit && (prev & CHF_DIR_DOWN))) dir |= CHF_DIR_DOWN;
    return dir;
}

int chf_pad_for_port(const int *assign, int npads, int port)
{
    int i, next = 0;
    for (i = 0; i < npads; i++)
        if (assign[i] == port) return i;
    for (i = 0; i < npads; i++) {
        if (assign[i] >= 0) continue;   /* explicitly elsewhere */
        if (next == port) return i;
        next++;
    }
    return -1;
}

int chf_pad_button_from_sdl(uint8_t button)
{
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return CHF_PAD_BTN_SOUTH;
    case SDL_GAMEPAD_BUTTON_EAST: return CHF_PAD_BTN_EAST;
    case SDL_GAMEPAD_BUTTON_WEST: return CHF_PAD_BTN_WEST;
    case SDL_GAMEPAD_BUTTON_NORTH: return CHF_PAD_BTN_NORTH;
    case SDL_GAMEPAD_BUTTON_BACK: return CHF_PAD_BTN_BACK;
    case SDL_GAMEPAD_BUTTON_GUIDE: return CHF_PAD_BTN_GUIDE;
    case SDL_GAMEPAD_BUTTON_START: return CHF_PAD_BTN_START;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return CHF_PAD_BTN_LEFT_STICK;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return CHF_PAD_BTN_RIGHT_STICK;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return CHF_PAD_BTN_LEFT_SHOULDER;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return CHF_PAD_BTN_RIGHT_SHOULDER;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return CHF_PAD_BTN_DPAD_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return CHF_PAD_BTN_DPAD_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return CHF_PAD_BTN_DPAD_LEFT;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return CHF_PAD_BTN_DPAD_RIGHT;
    default: return CHF_PAD_BTN_NONE;
    }
}

int chf_sdl_button_from_pad(int button)
{
    switch (button) {
    case CHF_PAD_BTN_SOUTH: return SDL_GAMEPAD_BUTTON_SOUTH;
    case CHF_PAD_BTN_EAST: return SDL_GAMEPAD_BUTTON_EAST;
    case CHF_PAD_BTN_WEST: return SDL_GAMEPAD_BUTTON_WEST;
    case CHF_PAD_BTN_NORTH: return SDL_GAMEPAD_BUTTON_NORTH;
    case CHF_PAD_BTN_BACK: return SDL_GAMEPAD_BUTTON_BACK;
    case CHF_PAD_BTN_GUIDE: return SDL_GAMEPAD_BUTTON_GUIDE;
    case CHF_PAD_BTN_START: return SDL_GAMEPAD_BUTTON_START;
    case CHF_PAD_BTN_LEFT_STICK: return SDL_GAMEPAD_BUTTON_LEFT_STICK;
    case CHF_PAD_BTN_RIGHT_STICK: return SDL_GAMEPAD_BUTTON_RIGHT_STICK;
    case CHF_PAD_BTN_LEFT_SHOULDER: return SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    case CHF_PAD_BTN_RIGHT_SHOULDER: return SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    case CHF_PAD_BTN_DPAD_UP: return SDL_GAMEPAD_BUTTON_DPAD_UP;
    case CHF_PAD_BTN_DPAD_DOWN: return SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    case CHF_PAD_BTN_DPAD_LEFT: return SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    case CHF_PAD_BTN_DPAD_RIGHT: return SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    default: return -1;
    }
}

/* ---- slots ------------------------------------------------------------------ */

static int slot_for_id(gamepad_state *g, SDL_JoystickID id)
{
    int i;
    for (i = 0; i < g->npads; i++)
        if (g->pads[i].id == id) return i;
    return -1;
}

static const char *slot_name(const pad_slot *slot)
{
    const char *n = slot->handle ? SDL_GetGamepadName(slot->handle)
                                 : SDL_GetJoystickName(slot->joy);
    return n ? n : "Gamepad";
}

static void slot_release_all(gamepad_state *g, pad_slot *slot)
{
    int a, b;
    if (slot->port >= 0 && (slot->disc_x || slot->disc_y)) {
        slot->disc_x = slot->disc_y = 0;
        session_gamepad_disc(g->session, slot->owner, slot->port, 0, 0);
    }
    if (slot->port >= 0)
        for (a = 0; a < CHF_ACT_PER_PORT; a++)
            if (slot->sent[a]) {
                slot->sent[a] = 0;
                session_gamepad_stick(g->session, slot->owner, slot->port, a, 0);
            }
    for (b = 0; b < (int)(sizeof slot->btn_target / sizeof slot->btn_target[0]); b++)
        if (slot->btn_target[b]) {
            session_gamepad_press(g->session, slot->owner, slot->btn_target[b] - 1, 0);
            slot->btn_target[b] = 0;
        }
}

static void slot_close(gamepad_state *g, pad_slot *slot)
{
    slot_release_all(g, slot);
    if (slot->handle) SDL_CloseGamepad(slot->handle);
    if (slot->joy) SDL_CloseJoystick(slot->joy);
    slot->handle = NULL;
    slot->joy = NULL;
}

static int effective_port_locked(gamepad_state *g, int idx);

/* Idempotent, and the single place the gamepad/joystick decision is made:
 * both SDL_EVENT_JOYSTICK_ADDED and SDL_EVENT_GAMEPAD_ADDED land here (SDL
 * posts both for a mapped device) and the slot_for_id guard makes the second
 * one a no-op. */
static void open_pad(gamepad_state *g, SDL_JoystickID id)
{
    pthread_mutex_lock(&g->lock);
    if (g->npads < MAX_PADS && slot_for_id(g, id) < 0) {
        /* Not an else-if on SDL_IsGamepad: a device SDL calls a gamepad can
         * still fail to open as one. Falling through to the raw joystick
         * keeps it usable -- every button is still bindable, just unnamed. */
        SDL_Gamepad *gp = SDL_IsGamepad(id) ? SDL_OpenGamepad(id) : NULL;
        SDL_Joystick *j = gp ? NULL : SDL_OpenJoystick(id);
        if (gp || j) {
            pad_slot *slot = &g->pads[g->npads++];
            int h, used = 0, k;
            for (k = 0; k < g->npads - 1; k++)
                used |= 1 << g->pads[k].owner;
            memset(slot, 0, sizeof *slot);
            for (k = 0; k < MAX_PADS && (used & (1 << k)); k++) {}
            slot->owner = k;
            slot->id = id;
            slot->handle = gp;
            slot->joy = j;
            slot->assign = -1;
            slot->port = -1;
            for (h = 0; h < MAX_HATS; h++) slot->last_hat[h] = SDL_HAT_CENTERED;
            slot->guid = SDL_GetJoystickGUIDForID(id);
            snprintf(slot->name, sizeof slot->name, "%s", slot_name(slot));
            for (h = 0; h < MAX_PADS; h++) {
                if (g->gone[h].used && !memcmp(&g->gone[h].guid, &slot->guid, sizeof slot->guid)
                    && !strcmp(g->gone[h].name, slot->name)) {
                    slot->assign = g->gone[h].assign;
                    g->gone[h].used = 0;
                    break;
                }
            }
            g->generation++;
            fprintf(stderr, "chf gamepad: %s connected (%s)\n", slot->name,
                    gp ? "gamepad" : "raw joystick");
            {
                char msg[128];
                int port = effective_port_locked(g, g->npads - 1);
                if (port >= 0)
                    snprintf(msg, sizeof msg, "Connected: %s (player %d)", slot->name, port + 1);
                else
                    snprintf(msg, sizeof msg, "Connected: %s", slot->name);
                session_gamepad_event(g->session, msg);
            }
        }
    }
    pthread_mutex_unlock(&g->lock);
}

static void close_pad(gamepad_state *g, SDL_JoystickID id)
{
    int idx, i;
    pthread_mutex_lock(&g->lock);
    idx = slot_for_id(g, id);
    if (idx >= 0) {
        pad_slot *slot = &g->pads[idx];
        char msg[128];
        fprintf(stderr, "chf gamepad: %s disconnected\n", slot->name);
        snprintf(msg, sizeof msg, "Disconnected: %s", slot->name);
        if (slot->assign >= 0) {
            int k = g->gone_next++ % MAX_PADS;
            g->gone[k].guid = slot->guid;
            memcpy(g->gone[k].name, slot->name, sizeof g->gone[k].name);
            g->gone[k].assign = slot->assign;
            g->gone[k].used = 1;
        }
        slot_close(g, slot);
        session_gamepad_event(g->session, msg);
        for (i = idx; i < g->npads - 1; i++) g->pads[i] = g->pads[i + 1];
        g->npads--;
        g->generation++;
    }
    pthread_mutex_unlock(&g->lock);
}

/* ---- buttons ---------------------------------------------------------------- */

/* The port a slot drives right now. Locked by the caller. */
static int effective_port_locked(gamepad_state *g, int idx)
{
    int assign[MAX_PADS], i, port;
    for (i = 0; i < MAX_PADS; i++) assign[i] = -1;
    for (i = 0; i < g->npads; i++) assign[i] = g->pads[i].assign;
    for (port = 0; port < 2; port++)
        if (chf_pad_for_port(assign, g->npads, port) == idx) return port;
    return -1;
}

static void dispatch_button(gamepad_state *g, SDL_JoystickID which, int b, int down)
{
    int idx, port, owner = 0, target = -1;
    if (b == CHF_PAD_BTN_NONE) return;

    pthread_mutex_lock(&g->lock);
    if (g->cap_active) {
        /* Map mode: every button event is swallowed while armed, not just
         * the captured one, so the press that maps a button does not also
         * fire whatever it used to do. */
        if (down && !g->cap_have) { g->cap_have = 1; g->cap_result = b; }
        pthread_mutex_unlock(&g->lock);
        return;
    }
    idx = slot_for_id(g, which);
    port = idx >= 0 ? effective_port_locked(g, idx) : -1;
    if (idx >= 0 && port >= 0 && b >= 0 && b < 256) {
        pad_slot *slot = &g->pads[idx];
        owner = slot->owner;
        if (down) {
            target = chfsession_target_for_button(g->session, port, b);
            if (target >= 0 && target < CHF_TARGET_SYSACT(0))
                slot->btn_target[b] = (int16_t)(target + 1);
        } else if (slot->btn_target[b]) {
            /* what the press asserted, whatever the binding says now */
            target = slot->btn_target[b] - 1;
            slot->btn_target[b] = 0;
        }
    }
    pthread_mutex_unlock(&g->lock);
    if (target < 0) return;

    if (target >= CHF_TARGET_SYSACT(0)) {
        if (down) chfsession_sysaction_post(g->session, target - CHF_TARGET_SYSACT(0));
        return;
    }
    session_gamepad_press(g->session, owner, target, down);
}

static int hat_dir_from_mask(Uint8 mask)
{
    switch (mask) {
    case SDL_HAT_UP: return 0;
    case SDL_HAT_RIGHTUP: return 1;
    case SDL_HAT_RIGHT: return 2;
    case SDL_HAT_RIGHTDOWN: return 3;
    case SDL_HAT_DOWN: return 4;
    case SDL_HAT_LEFTDOWN: return 5;
    case SDL_HAT_LEFT: return 6;
    case SDL_HAT_LEFTUP: return 7;
    default: return -1;
    }
}

static void handle_hat_motion(gamepad_state *g, SDL_JoystickID which, int hat, Uint8 mask)
{
    int idx, prev_dir, dir;
    if (hat < 0 || hat >= MAX_HATS) return;
    pthread_mutex_lock(&g->lock);
    idx = slot_for_id(g, which);
    if (idx < 0 || g->pads[idx].handle) { pthread_mutex_unlock(&g->lock); return; }
    prev_dir = hat_dir_from_mask(g->pads[idx].last_hat[hat]);
    g->pads[idx].last_hat[hat] = mask;
    pthread_mutex_unlock(&g->lock);
    dir = hat_dir_from_mask(mask);
    /* hat positions are bindable buttons in their own band; hat 0's four
     * cardinal points also feed the directional role in poll_sticks */
    if (prev_dir >= 0)
        dispatch_button(g, which, CHF_PAD_HAT_BASE + hat * CHF_PAD_HAT_DIRS + prev_dir, 0);
    if (dir >= 0)
        dispatch_button(g, which, CHF_PAD_HAT_BASE + hat * CHF_PAD_HAT_DIRS + dir, 1);
}

/* ---- the directional role and the analog controllers ------------------------ */

static float axis_norm(Sint16 v)
{
    return v < 0 ? (float)v / 32768.0f : (float)v / 32767.0f;
}

static void slot_stick(const pad_slot *slot, int right, float *x, float *y)
{
    if (slot->handle) {
        *x = axis_norm(SDL_GetGamepadAxis(slot->handle, right ? SDL_GAMEPAD_AXIS_RIGHTX : SDL_GAMEPAD_AXIS_LEFTX));
        *y = axis_norm(SDL_GetGamepadAxis(slot->handle, right ? SDL_GAMEPAD_AXIS_RIGHTY : SDL_GAMEPAD_AXIS_LEFTY));
    } else {
        int base = right ? 2 : 0;
        *x = SDL_GetNumJoystickAxes(slot->joy) > base ? axis_norm(SDL_GetJoystickAxis(slot->joy, base)) : 0.f;
        *y = SDL_GetNumJoystickAxes(slot->joy) > base + 1 ? axis_norm(SDL_GetJoystickAxis(slot->joy, base + 1)) : 0.f;
    }
}

/* The raw joystick's hat 0 as directions (a mapped gamepad's D-pad is a
 * button, bound like any other). */
static int slot_hat_dir(const pad_slot *slot)
{
    int dir = 0;
    if (!slot->handle && slot->joy && SDL_GetNumJoystickHats(slot->joy) > 0) {
        Uint8 m = SDL_GetJoystickHat(slot->joy, 0);
        if (m & SDL_HAT_UP) dir |= CHF_DIR_UP;
        if (m & SDL_HAT_DOWN) dir |= CHF_DIR_DOWN;
        if (m & SDL_HAT_LEFT) dir |= CHF_DIR_LEFT;
        if (m & SDL_HAT_RIGHT) dir |= CHF_DIR_RIGHT;
    }
    return dir;
}

static void poll_one(gamepad_state *g, pad_slot *slot, int port)
{
    const struct chfsession *s = g->session;
    uint8_t want[CHF_ACT_PER_PORT];
    unsigned acts;
    int dir, a, dx = 0, dy = 0;
    float lx, ly;

    memset(want, 0, sizeof want);
    if (port != slot->port) {
        /* moved to another port (or none): release what it held there */
        slot_release_all(g, slot);
        slot->port = port;
        slot->stick_dir = 0;
        if (port < 0) return;
    }

    dir = slot_hat_dir(slot);
    if (s->opts.analog_joystick) {
        /* the analog disc: the left stick's deflection, as is */
        slot->stick_dir = 0;
        slot_stick(slot, 0, &lx, &ly);
        chf_disc_from_stick(lx, ly, &dx, &dy);
    } else if (slot->handle || slot->joy) {
        slot_stick(slot, 0, &lx, &ly);
        slot->stick_dir = chf_dir_from_stick(lx, ly, slot->stick_dir, STICK_ENTER, STICK_EXIT);
        dir |= slot->stick_dir;
    }
    if (dx != slot->disc_x || dy != slot->disc_y) {
        slot->disc_x = dx;
        slot->disc_y = dy;
        session_gamepad_disc(g->session, slot->owner, port, dx, dy);
    }
    /* opposite directions together cancel: nothing */
    acts = chf_acts_from_dir(dir);
    for (a = 0; a < CHF_ACT_PER_PORT; a++)
        want[a] = (uint8_t)((acts >> a) & 1u);

    for (a = 0; a < CHF_ACT_PER_PORT; a++) {
        if (want[a] != slot->sent[a]) {
            slot->sent[a] = want[a];
            session_gamepad_stick(g->session, slot->owner, port, a, want[a]);
        }
    }

    /* the triggers, as buttons */
    if (slot->handle) {
        int t;
        for (t = 0; t < 2; t++) {
            float v = axis_norm(SDL_GetGamepadAxis(slot->handle,
                t ? SDL_GAMEPAD_AXIS_RIGHT_TRIGGER : SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
            int down = v >= TRIGGER_ON;
            if (down != slot->trigger_down[t]) {
                slot->trigger_down[t] = down;
                /* dispatch_button takes the lock poll_sticks holds */
                pthread_mutex_unlock(&g->lock);
                dispatch_button(g, slot->id, t ? CHF_PAD_BTN_RIGHT_TRIGGER : CHF_PAD_BTN_LEFT_TRIGGER, down);
                pthread_mutex_lock(&g->lock);
            }
        }
    }
}

static void poll_sticks(gamepad_state *g)
{
    int i;
    pthread_mutex_lock(&g->lock);
    for (i = 0; i < g->npads; i++)
        poll_one(g, &g->pads[i], effective_port_locked(g, i));
    pthread_mutex_unlock(&g->lock);
}

/* ---- the thread ------------------------------------------------------------- */

static void *thread_main(void *arg)
{
    gamepad_state *g = arg;
    int count = 0, i;
    /* Every joystick, not just the ones SDL has a gamepad mapping for:
     * SDL_GetGamepads would silently omit a plain HID adapter entirely. */
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    if (ids) {
        for (i = 0; i < count; i++) open_pad(g, ids[i]);
        SDL_free(ids);
    }

    while (!atomic_load(&g->stop_requested)) {
        SDL_Event ev;
        int npads;

        pthread_mutex_lock(&g->lock);
        npads = g->npads;
        pthread_mutex_unlock(&g->lock);
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_EVENT_JOYSTICK_ADDED: open_pad(g, ev.jdevice.which); break;
            case SDL_EVENT_JOYSTICK_REMOVED: close_pad(g, ev.jdevice.which); break;
            case SDL_EVENT_GAMEPAD_ADDED: open_pad(g, ev.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_REMOVED: close_pad(g, ev.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                dispatch_button(g, ev.gbutton.which,
                                chf_pad_button_from_sdl(ev.gbutton.button),
                                ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
                break;
            case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
            case SDL_EVENT_JOYSTICK_BUTTON_UP: {
                /* a mapped gamepad also emits these for the same press it
                 * reported as a GAMEPAD_BUTTON event; the slot's kind decides */
                int gamepad, idx;
                pthread_mutex_lock(&g->lock);
                idx = slot_for_id(g, ev.jbutton.which);
                gamepad = idx >= 0 && g->pads[idx].handle != NULL;
                pthread_mutex_unlock(&g->lock);
                if (gamepad || ev.jbutton.button >= 64) break;
                dispatch_button(g, ev.jbutton.which,
                                CHF_PAD_BTN_RAW_BASE + ev.jbutton.button,
                                ev.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN);
                break;
            }
            case SDL_EVENT_JOYSTICK_HAT_MOTION:
                handle_hat_motion(g, ev.jhat.which, ev.jhat.hat, ev.jhat.value);
                break;
            default:
                break;
            }
        }
        poll_sticks(g);
        /* ~60 Hz while a pad is attached (the machine's own sampling rate).
         * With none there are only hotplug events to notice, which can wait
         * half a second. (Not SDL_WaitEventTimeout: SDL polls inside it at
         * about 1 kHz.) */
        SDL_Delay(npads > 0 ? 16 : 500);
    }

    pthread_mutex_lock(&g->lock);
    for (i = 0; i < g->npads; i++) slot_close(g, &g->pads[i]);
    g->npads = 0;
    pthread_mutex_unlock(&g->lock);
    return NULL;
}

int gamepad_start(struct chfsession *s)
{
    gamepad_state *g;
    if (s->gamepad) return 0;
    /* The app owns its signals; SDL must not intercept SIGINT/SIGTERM. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    /* Bluetooth pads over hidapi and the joystick subsystem together. */
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        session_set_error(s, "SDL gamepad init failed: %s", SDL_GetError());
        return -1;
    }
    g = calloc(1, sizeof *g);
    if (!g) { SDL_QuitSubSystem(SDL_INIT_GAMEPAD); return -1; }
    g->session = s;
    pthread_mutex_init(&g->lock, NULL);
    /* An explicit stack: the platform default is 512 KB on macOS, and SDL's
     * HID enumeration runs on this thread. */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4u << 20);
    if (pthread_create(&g->thread, &attr, thread_main, g) != 0) {
        pthread_attr_destroy(&attr);
        session_set_error(s, "gamepad thread could not start");
        pthread_mutex_destroy(&g->lock);
        free(g);
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        return -1;
    }
    pthread_attr_destroy(&attr);
    g->running = 1;
    g_state = g;
    s->gamepad = g;
    return 0;
}

void gamepad_stop(struct chfsession *s)
{
    gamepad_state *g = s->gamepad;
    if (!g) return;
    atomic_store(&g->stop_requested, 1);
    pthread_join(g->thread, NULL);
    pthread_mutex_destroy(&g->lock);
    s->gamepad = NULL;
    if (g_state == g) g_state = NULL;
    free(g);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

/* ---- public API ------------------------------------------------------------- */

int chfsession_gamepad_count(chfsession *s)
{
    gamepad_state *g = s->gamepad;
    int n;
    if (!g) return 0;
    pthread_mutex_lock(&g->lock);
    n = g->npads;
    pthread_mutex_unlock(&g->lock);
    return n;
}

int chfsession_gamepad_name(chfsession *s, int idx, char *dst, int dstsz)
{
    gamepad_state *g = s->gamepad;
    int n = 0;
    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!g) return 0;
    pthread_mutex_lock(&g->lock);
    if (idx >= 0 && idx < g->npads)
        n = snprintf(dst, (size_t)dstsz, "%s", slot_name(&g->pads[idx]));
    pthread_mutex_unlock(&g->lock);
    return n;
}

void chfsession_gamepad_assign(chfsession *s, int idx, int port)
{
    gamepad_state *g = s->gamepad;
    if (!g) return;
    pthread_mutex_lock(&g->lock);
    if (idx >= 0 && idx < g->npads)
        g->pads[idx].assign = (port >= 0 && port < 2) ? port : -1;
    pthread_mutex_unlock(&g->lock);
}

int chfsession_gamepad_assignment(chfsession *s, int idx)
{
    gamepad_state *g = s->gamepad;
    int r = -1;
    if (!g) return -1;
    pthread_mutex_lock(&g->lock);
    if (idx >= 0 && idx < g->npads) r = g->pads[idx].assign;
    pthread_mutex_unlock(&g->lock);
    return r;
}

int chfsession_gamepad_effective_port(chfsession *s, int idx)
{
    gamepad_state *g = s->gamepad;
    int r = -1;
    if (!g) return -1;
    pthread_mutex_lock(&g->lock);
    if (idx >= 0 && idx < g->npads) r = effective_port_locked(g, idx);
    pthread_mutex_unlock(&g->lock);
    return r;
}

unsigned chfsession_gamepad_generation(chfsession *s)
{
    gamepad_state *g = s->gamepad;
    unsigned r;
    if (!g) return 0;
    pthread_mutex_lock(&g->lock);
    r = g->generation;
    pthread_mutex_unlock(&g->lock);
    return r;
}

void chfsession_gamepad_capture_begin(chfsession *s)
{
    gamepad_state *g = s->gamepad;
    if (!g) return;
    pthread_mutex_lock(&g->lock);
    g->cap_active = 1; g->cap_have = 0; g->cap_result = CHF_PAD_BTN_NONE;
    pthread_mutex_unlock(&g->lock);
}

void chfsession_gamepad_capture_cancel(chfsession *s)
{
    gamepad_state *g = s->gamepad;
    if (!g) return;
    pthread_mutex_lock(&g->lock);
    g->cap_active = 0; g->cap_have = 0;
    pthread_mutex_unlock(&g->lock);
}

int chfsession_gamepad_capture_poll(chfsession *s, int *button)
{
    gamepad_state *g = s->gamepad;
    int got = 0;
    if (!g) return 0;
    pthread_mutex_lock(&g->lock);
    if (g->cap_active && g->cap_have) {
        if (button) *button = g->cap_result;
        g->cap_active = 0; g->cap_have = 0;
        got = 1;
    }
    pthread_mutex_unlock(&g->lock);
    return got;
}
