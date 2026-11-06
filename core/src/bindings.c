/*
 * bindings -- the remappable key/gamepad table, the keyboard translator,
 * and every name the bindings editor and the settings store need.
 *
 * One flat target table (chfsession.h's CHF_TARGET_* indices): per player (1,
 * then 2) the hand controller's eight contacts (the action number is the
 * hardware bit), the console's four numbered buttons, its RESET, then the
 * session's own actions: 23 in all.
 * Each target holds at most one keysym and one gamepad button. Rebinding
 * STEALS -- a key drives exactly one target, because one keystroke doing
 * two things is worse than losing the old binding: the second effect is
 * invisible until it matters.
 *
 * Seeds its defaults LAZILY so the table is usable with no session behind
 * it: a unit test with no settings store gets the documented default map.
 * Persisted as one packed "bindings" key holding only the entries that
 * differ from the defaults ("<target>.k:<keysym>" / "<target>.b:<button>").
 *
 * Keysyms are X11's, folded to lower case before lookup so a binding made
 * with 'z' still fires while Shift is held for something else. The numeric
 * keypad's keys are NOT folded onto the main block's (player 2 lives there),
 * except its Enter, which is Enter.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hid_keys.h"
#include "session_internal.h"

typedef struct {
    uint32_t keysym;
    int button;
} slot;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static slot s_table[CHF_TARGET_COUNT];
static slot s_defaults[CHF_TARGET_COUNT];
static int s_seeded;
static struct chfsession *s_store;   /* whose settings hold "bindings" */

/* ---- defaults ------------------------------------------------------------- */

static void seed_key(int target, uint32_t keysym)
{
    s_defaults[target].keysym = keysym;
}

static void seed_button(int target, int button)
{
    s_defaults[target].button = button;
}

/* The defaults. Player 1, the right controller: the arrow keys tilt the stick,
 * Z and X twist it (counter-clockwise, clockwise), C pulls the knob up and
 * Space pushes it down. Player 2, the left controller: the numeric keypad's
 * 8 2 4 6 tilt it (forward, back, left, right), 7 and 9 twist, . pulls and 0
 * pushes. The console's buttons 1-4 (TIME, MODE, HOLD, START) are the
 * number keys 1 2 3 4, F1 is RESET (MAME's) and Escape is Reset to CONFIG.
 * F5/F7/F8/F9/F11/F12 are the family's window and debugger keys and stay free
 * of the machine.
 *
 * Gamepad, the same on both players: the D-pad tilts the stick (the left
 * analog stick does too: gamepad_sdl.c), LB and RB twist, East pulls, South
 * pushes. The console's buttons go on West (TIME), Back (MODE), North (HOLD)
 * and Start (START). With "right_stick" on the right analog stick is the
 * knob's other axes too. */
static void compute_defaults_locked(void)
{
    static const struct { int act; uint32_t key; int button; } p1[CHF_ACT_PER_PORT] = {
        { CHF_ACT_RIGHT,     CHF_KEYSYM_RIGHT, CHF_PAD_BTN_DPAD_RIGHT },
        { CHF_ACT_LEFT,      CHF_KEYSYM_LEFT,  CHF_PAD_BTN_DPAD_LEFT },
        { CHF_ACT_BACK,      CHF_KEYSYM_DOWN,  CHF_PAD_BTN_DPAD_DOWN },
        { CHF_ACT_FORWARD,   CHF_KEYSYM_UP,    CHF_PAD_BTN_DPAD_UP },
        { CHF_ACT_TWIST_CCW, 'z',              CHF_PAD_BTN_LEFT_SHOULDER },
        { CHF_ACT_TWIST_CW,  'x',              CHF_PAD_BTN_RIGHT_SHOULDER },
        { CHF_ACT_PULL,      'c',              CHF_PAD_BTN_EAST },
        { CHF_ACT_PUSH,      CHF_KEYSYM_SPACE, CHF_PAD_BTN_SOUTH },
    };
    static const uint32_t p2_keys[CHF_ACT_PER_PORT] = {
        CHF_KEYSYM_KP_6, CHF_KEYSYM_KP_4, CHF_KEYSYM_KP_2, CHF_KEYSYM_KP_8,
        CHF_KEYSYM_KP_7, CHF_KEYSYM_KP_9, CHF_KEYSYM_KP_PERIOD, CHF_KEYSYM_KP_0,
    };
    static const uint32_t key_keys[CHF_KEY_COUNT] = { '1', '2', '3', '4' };
    static const int key_buttons[CHF_KEY_COUNT] = {
        CHF_PAD_BTN_WEST, CHF_PAD_BTN_BACK, CHF_PAD_BTN_NORTH, CHF_PAD_BTN_START,
    };
    int t, a, k;

    for (t = 0; t < CHF_TARGET_COUNT; t++) {
        s_defaults[t].keysym = 0;
        s_defaults[t].button = CHF_PAD_BTN_NONE;
    }

    for (a = 0; a < CHF_ACT_PER_PORT; a++) {
        seed_key(CHF_TARGET_PORT(0, p1[a].act), p1[a].key);
        seed_key(CHF_TARGET_PORT(1, p1[a].act), p2_keys[p1[a].act]);
        seed_button(CHF_TARGET_PORT(0, p1[a].act), p1[a].button);
        seed_button(CHF_TARGET_PORT(1, p1[a].act), p1[a].button);
    }
    for (k = 0; k < CHF_KEY_COUNT; k++) {
        seed_key(CHF_TARGET_KEY(k), key_keys[k]);
        seed_button(CHF_TARGET_KEY(k), key_buttons[k]);
    }

    seed_key(CHF_TARGET_SWITCH(CHF_SW_RESET), CHF_KEYSYM_F1);
    seed_key(CHF_TARGET_SYSACT(CHF_SYSACT_RESET_CONFIG), CHF_KEYSYM_ESCAPE);
}

static uint32_t fold(uint32_t keysym)
{
    if (keysym >= 'A' && keysym <= 'Z') return keysym + 32;
    if (keysym == CHF_KEYSYM_KP_ENTER) return CHF_KEYSYM_RETURN;
    return keysym;
}

/* ---- persistence ---------------------------------------------------------- */

static void apply_persisted_locked(const char *packed)
{
    const char *p = packed;
    while (p && *p) {
        int target = -1; char kind = 0; long value = 0;
        const char *end = strchr(p, ' ');
        if (sscanf(p, "%d.%c:%ld", &target, &kind, &value) == 3
            && target >= 0 && target < CHF_TARGET_COUNT) {
            if (kind == 'k') s_table[target].keysym = (uint32_t)value;
            else if (kind == 'b') s_table[target].button = (int)value;
        }
        if (!end) break;
        p = end + 1;
    }
}

static void pack_locked(char *out, size_t outsz)
{
    size_t len = 0;
    int t;
    out[0] = '\0';
    for (t = 0; t < CHF_TARGET_COUNT; t++) {
        if (s_table[t].keysym != s_defaults[t].keysym)
            len += (size_t)snprintf(out + len, outsz - len, "%s%d.k:%u",
                                    len ? " " : "", t, s_table[t].keysym);
        if (len >= outsz) break;
        if (s_table[t].button != s_defaults[t].button)
            len += (size_t)snprintf(out + len, outsz - len, "%s%d.b:%d",
                                    len ? " " : "", t, s_table[t].button);
        if (len >= outsz) break;
    }
}

static void persist_locked(void)
{
    char packed[8192];
    if (!s_store) return;
    pack_locked(packed, sizeof packed);
    chfsession_set_str(s_store, "bindings", packed);
}

static void ensure_seeded_locked(void)
{
    if (s_seeded) return;
    compute_defaults_locked();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_seeded = 1;
}

void bindings_init(struct chfsession *s)
{
    pthread_mutex_lock(&s_lock);
    compute_defaults_locked();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_seeded = 1;
    s_store = s;
    if (s)
        apply_persisted_locked(chfsession_get_str(s, "bindings", ""));
    pthread_mutex_unlock(&s_lock);
}

/* ---- lookups -------------------------------------------------------------- */

static int target_for_key_locked(uint32_t keysym)
{
    int t;
    if (!keysym) return -1;
    for (t = 0; t < CHF_TARGET_COUNT; t++)
        if (s_table[t].keysym == keysym) return t;
    return -1;
}

int chfsession_target_for_key(chfsession *s, uint32_t keysym)
{
    int t;
    (void)s;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    t = target_for_key_locked(fold(keysym));
    pthread_mutex_unlock(&s_lock);
    return t;
}

/* Gamepad buttons are scoped to the pad's port: the same button index is
 * free to mean different things on different ports, so only that port's
 * targets and the machine-wide switches/actions are searched. */
int chfsession_target_for_button(chfsession *s, int port, int button)
{
    int t, found = -1;
    (void)s;
    if (button == CHF_PAD_BTN_NONE) return -1;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    if (port < 0 || port >= CHF_PLAYERS) port = 0;
    for (t = 0; t < CHF_ACT_PER_PORT; t++)
        if (s_table[CHF_TARGET_PORT(port, t)].button == button) {
            found = CHF_TARGET_PORT(port, t);
            break;
        }
    if (found < 0)
        for (t = CHF_PLAYERS * CHF_ACT_PER_PORT; t < CHF_TARGET_COUNT; t++)
            if (s_table[t].button == button) { found = t; break; }
    pthread_mutex_unlock(&s_lock);
    return found;
}

chf_binding chfsession_binding_get(chfsession *s, int target)
{
    chf_binding b = { 0, CHF_PAD_BTN_NONE };
    (void)s;
    if (target < 0 || target >= CHF_TARGET_COUNT) return b;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    b.keysym = s_table[target].keysym;
    b.button = s_table[target].button;
    pthread_mutex_unlock(&s_lock);
    return b;
}

/* A key may legitimately drive several targets (two players' controls can
 * be put on one key by hand). An explicit rebinding is the user saying
 * "this key does exactly this", so it displaces every holder -- and names
 * them all, comma-separated, so nothing is lost silently. */
static void describe_stolen(const int *victims, int nvictims, char *stolen,
                            int stolensz)
{
    int i, len = 0;
    if (!stolen || stolensz <= 0) return;
    stolen[0] = '\0';
    for (i = 0; i < nvictims && len < stolensz; i++)
        len += snprintf(stolen + len, (size_t)(stolensz - len), "%s%s",
                        i ? ", " : "", chf_target_name(victims[i]));
}

void chfsession_binding_set_key(chfsession *s, int target, uint32_t keysym,
                                  char *stolen, int stolensz)
{
    int victims[CHF_TARGET_COUNT], nvictims = 0, t;
    (void)s;
    if (target < 0 || target >= CHF_TARGET_COUNT) return;
    keysym = fold(keysym);
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    if (keysym) {
        for (t = 0; t < CHF_TARGET_COUNT; t++)
            if (t != target && s_table[t].keysym == keysym) {
                s_table[t].keysym = 0;
                victims[nvictims++] = t;
            }
    }
    s_table[target].keysym = keysym;
    persist_locked();
    pthread_mutex_unlock(&s_lock);
    describe_stolen(victims, nvictims, stolen, stolensz);
}

void chfsession_binding_set_button(chfsession *s, int target, int button,
                                     char *stolen, int stolensz)
{
    int victims[CHF_TARGET_COUNT], nvictims = 0, t;
    int port = target / CHF_ACT_PER_PORT;   /* 2 and up: buttons, RESET, actions */
    (void)s;
    if (target < 0 || target >= CHF_TARGET_COUNT) return;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    if (button != CHF_PAD_BTN_NONE) {
        for (t = 0; t < CHF_TARGET_COUNT; t++) {
            int tport = t / CHF_ACT_PER_PORT;
            /* steal only within the same scope: this port, or the
             * machine-wide targets, which every pad reaches */
            if (t != target && s_table[t].button == button
                && (tport == port || tport >= CHF_PLAYERS || port >= CHF_PLAYERS)) {
                s_table[t].button = CHF_PAD_BTN_NONE;
                victims[nvictims++] = t;
            }
        }
    }
    s_table[target].button = button;
    persist_locked();
    pthread_mutex_unlock(&s_lock);
    describe_stolen(victims, nvictims, stolen, stolensz);
}

void chfsession_bindings_reset(chfsession *s)
{
    (void)s;
    pthread_mutex_lock(&s_lock);
    compute_defaults_locked();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_seeded = 1;
    if (s_store) chfsession_set_str(s_store, "bindings", "");
    pthread_mutex_unlock(&s_lock);
}

/* ---- names ---------------------------------------------------------------- */

static const char *const act_names[CHF_ACT_PER_PORT] = {
    "Right", "Left", "Back", "Forward", "Twist CCW", "Twist CW", "Pull", "Push",
};
static const char *const switch_names[CHF_SW_COUNT] = {
    "RESET",
};
static const char *const sysact_names[CHF_SYSACT_COUNT] = {
    "Reset to CONFIG", "Debugger Stop",
};

/* A console button's name: "1 TIME". */
static const char *key_name(int key, char *buf, size_t bufsz)
{
    const char *label = chfsession_key_label(CHF_MACHINE_CHANNELF, key);
    const char *legend = chfsession_key_legend(CHF_MACHINE_CHANNELF, key);
    if (!label) {
        snprintf(buf, bufsz, "%d", key);
        return buf;
    }
    snprintf(buf, bufsz, "%s %s", label, legend ? legend : "");
    return buf;
}

const char *chf_target_name(int target)
{
    static char buf[64];
    char kbuf[32];
    if (target < 0 || target >= CHF_TARGET_COUNT) return "";
    if (target < CHF_PLAYERS * CHF_ACT_PER_PORT) {
        snprintf(buf, sizeof buf, "Player %d: %s", target / CHF_ACT_PER_PORT + 1,
                 act_names[target % CHF_ACT_PER_PORT]);
        return buf;
    }
    target -= CHF_PLAYERS * CHF_ACT_PER_PORT;
    if (target < CHF_KEY_COUNT) {
        snprintf(buf, sizeof buf, "Console: %s", key_name(target, kbuf, sizeof kbuf));
        return buf;
    }
    target -= CHF_KEY_COUNT;
    if (target < CHF_SW_COUNT) return switch_names[target];
    return sysact_names[target - CHF_SW_COUNT];
}

const char *chf_target_short_name(int target)
{
    static char buf[32];
    if (target < 0 || target >= CHF_TARGET_COUNT) return "";
    if (target < CHF_PLAYERS * CHF_ACT_PER_PORT) return act_names[target % CHF_ACT_PER_PORT];
    target -= CHF_PLAYERS * CHF_ACT_PER_PORT;
    if (target < CHF_KEY_COUNT) return key_name(target, buf, sizeof buf);
    target -= CHF_KEY_COUNT;
    if (target < CHF_SW_COUNT) return switch_names[target];
    return sysact_names[target - CHF_SW_COUNT];
}

static const char *const pad_button_names[CHF_PAD_BTN_COUNT] = {
    "A", "B", "X", "Y", "Back", "Guide", "Start", "Left Stick", "Right Stick",
    "Left Shoulder", "Right Shoulder", "D-pad Up", "D-pad Down", "D-pad Left",
    "D-pad Right", "Left Trigger", "Right Trigger",
};

const char *chf_pad_button_name(int button)
{
    static char buf[32];
    if (CHF_PAD_BTN_IS_NAMED(button)) return pad_button_names[button];
    if (CHF_PAD_BTN_IS_RAW(button)) {
        snprintf(buf, sizeof buf, "Button %d", button - CHF_PAD_BTN_RAW_BASE);
        return buf;
    }
    if (CHF_PAD_BTN_IS_HAT(button)) {
        static const char *const dirs[CHF_PAD_HAT_DIRS] =
            { "Up", "Up-Right", "Right", "Down-Right", "Down", "Down-Left",
              "Left", "Up-Left" };
        int h = (button - CHF_PAD_HAT_BASE) / CHF_PAD_HAT_DIRS;
        int d = (button - CHF_PAD_HAT_BASE) % CHF_PAD_HAT_DIRS;
        snprintf(buf, sizeof buf, "Hat %d %s", h, dirs[d]);
        return buf;
    }
    return "";
}

int chfsession_keysym_name(uint32_t keysym, char *dst, int dstsz)
{
    const char *name = NULL;
    if (!dst || dstsz <= 0) return 0;
    switch (keysym) {
    case 0: name = ""; break;
    case CHF_KEYSYM_UP: name = "Up"; break;
    case CHF_KEYSYM_DOWN: name = "Down"; break;
    case CHF_KEYSYM_LEFT: name = "Left"; break;
    case CHF_KEYSYM_RIGHT: name = "Right"; break;
    case CHF_KEYSYM_ESCAPE: name = "Escape"; break;
    case CHF_KEYSYM_RETURN: name = "Return"; break;
    case CHF_KEYSYM_BACKSPACE: name = "Backspace"; break;
    case CHF_KEYSYM_TAB: name = "Tab"; break;
    case CHF_KEYSYM_SPACE: name = "Space"; break;
    case CHF_KEYSYM_LSHIFT: name = "Left Shift"; break;
    case CHF_KEYSYM_RSHIFT: name = "Right Shift"; break;
    case CHF_KEYSYM_LCTRL: name = "Left Ctrl"; break;
    case CHF_KEYSYM_RCTRL: name = "Right Ctrl"; break;
    case CHF_KEYSYM_LALT: name = "Left Alt"; break;
    case CHF_KEYSYM_RALT: name = "Right Alt"; break;
    case CHF_KEYSYM_KP_ENTER: name = "Keypad Enter"; break;
    case CHF_KEYSYM_KP_MULTIPLY: name = "Keypad *"; break;
    case CHF_KEYSYM_KP_DIVIDE: name = "Keypad /"; break;
    case CHF_KEYSYM_KP_PERIOD: name = "Keypad ."; break;
    case 0xffab: name = "Keypad +"; break;
    case 0xffad: name = "Keypad -"; break;
    case 0xffff: name = "Delete"; break;
    case 0xff63: name = "Insert"; break;
    case 0xff50: name = "Home"; break;
    case 0xff57: name = "End"; break;
    case 0xff55: name = "Page Up"; break;
    case 0xff56: name = "Page Down"; break;
    case 0xffe5: name = "Caps Lock"; break;
    default: break;
    }
    if (name) return snprintf(dst, (size_t)dstsz, "%s", name);
    if (keysym >= CHF_KEYSYM_F1 && keysym <= CHF_KEYSYM_F12)
        return snprintf(dst, (size_t)dstsz, "F%u", keysym - CHF_KEYSYM_F1 + 1);
    if (keysym >= CHF_KEYSYM_KP_0 && keysym <= CHF_KEYSYM_KP_9)
        return snprintf(dst, (size_t)dstsz, "Keypad %u", keysym - CHF_KEYSYM_KP_0);
    if (keysym >= 0x21 && keysym <= 0x7e) {
        char c = (char)keysym;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        return snprintf(dst, (size_t)dstsz, "%c", c);
    }
    if (keysym >= CHFSESSION_KEYSYM_HID_BASE
        && keysym <= CHFSESSION_KEYSYM_HID_BASE + CHFSESSION_HID_USAGE_MAX) {
        const char *hid = chf_hid_usage_name(keysym - CHFSESSION_KEYSYM_HID_BASE);
        if (hid) return snprintf(dst, (size_t)dstsz, "%s", hid);
        return snprintf(dst, (size_t)dstsz, "HID 0x%02X",
                        (unsigned)(keysym - CHFSESSION_KEYSYM_HID_BASE));
    }
    return snprintf(dst, (size_t)dstsz, "Key 0x%X", keysym);
}

/* ---- the keyboard translator --------------------------------------------- */

int chfsession_key_sysaction(chfsession *s, uint32_t keysym)
{
    int t;
    t = chfsession_target_for_key(s, keysym);
    if (t < CHF_TARGET_SYSACT(0)) return -1;
    return t - CHF_TARGET_SYSACT(0);
}

int chfsession_key(chfsession *s, uint32_t keysym, int down)
{
    uint32_t folded = fold(keysym);
    int t;

    if (!s) return 0;
    if (down) {
        int hit = 0;
        /* every target the key drives; a system action is the frontend's
         * to fire and is not a machine input */
        pthread_mutex_lock(&s_lock);
        ensure_seeded_locked();
        for (t = 0; t < CHF_TARGET_SYSACT(0); t++) {
            if (s_table[t].keysym != folded) continue;
            hit = 1;
            if (s->held_keysym[t] == folded) continue;  /* auto-repeat */
            s->held_keysym[t] = folded;
            pthread_mutex_unlock(&s_lock);
            chfsession_press(s, t, 1);
            pthread_mutex_lock(&s_lock);
        }
        pthread_mutex_unlock(&s_lock);
        return hit;
    }
    {
        int hit = 0;
        /* remembered by the key, so a release clears exactly what its press
         * asserted even if the binding changed in between */
        for (t = 0; t < CHF_TARGET_COUNT; t++) {
            if (s->held_keysym[t] == folded) {
                s->held_keysym[t] = 0;
                chfsession_press(s, t, 0);
                hit = 1;
            }
        }
        return hit;
    }
}
