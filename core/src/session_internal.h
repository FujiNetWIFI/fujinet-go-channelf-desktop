/*
 * chfsession's private state. Not installed; only the core/src sources
 * include it. One struct shared by the session's C modules (settings,
 * paths, media, audio, gamepads, bindings).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CHF_SESSION_INTERNAL_H
#define CHF_SESSION_INTERNAL_H

#include <pthread.h>
#include <stdint.h>

#include "chfsession.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CHF_PATH_MAX 1024

typedef struct setting_kv {
    char *key;
    char *val;
    struct setting_kv *next;
} setting_kv;

struct chfsession {
    char config_dir[CHF_PATH_MAX];
    char data_dir[CHF_PATH_MAX];
    char carts_dir[CHF_PATH_MAX];
    char settings_file[CHF_PATH_MAX];

    setting_kv *settings;
    pthread_mutex_t settings_mtx;
    int settings_dirty;

    char last_error[256];

    /* ---- FujiNet runtime (fujinet_runtime.c) ---- */
    char fujinet_root[CHF_PATH_MAX];    /* <data>/fujinet */
    char fujinet_config[CHF_PATH_MAX];  /* .../fnconfig.ini */
    char fujinet_sd[CHF_PATH_MAX];      /* .../SD */
    char fujinet_data[CHF_PATH_MAX];    /* .../data */
    char fujinet_lib[CHF_PATH_MAX];     /* resolved libfujinet path, "" until then */
    int  fujinet_disabled;              /* the caller passed fujinet_lib "" */
    char fujinet_runtime_src[CHF_PATH_MAX]; /* caller-given pristine tree, or "" */
    char webui_url[64];                 /* http://127.0.0.1:11527/ */
    int  fujinet_running;
    int  fujinet_external;              /* dialing a FujiNet that isn't ours */
    char boip_endpoint[80];             /* "host:port" the cartridge dials */
    char boip_addr[80];                 /* the external address as typed */

    /* cross-thread system-action latch (see chfsession_sysaction_post) */
    pthread_mutex_t sysact_mtx;
    unsigned sysact_pending;

    /* the running configuration */
    chfsession_start_opts opts;
    char cart_path[CHF_PATH_MAX];
    int run_machine;                   /* chf_machine the host was started with */
    int run_game;                      /* the slot holds a game's own cartridge
                                        * (a mapper), not the FujiNet cart */
    /* keys the keyboard currently holds, by target, so a release clears
     * exactly what its press asserted even if the binding changed meanwhile */
    uint32_t held_keysym[CHF_TARGET_COUNT];

    /* What drives each stick direction, matrix key and console button, one
     * bit per source (SRC_*), so that letting go of one source never
     * releases another's. input_mtx covers these arrays and the calls that
     * hand the result to the host. */
    pthread_mutex_t input_mtx;
    uint32_t pad_src[2][CHF_ACT_PER_PORT];
    uint32_t key_src[CHF_KEY_COUNT];
    uint32_t sw_src[CHF_SW_COUNT];
    /* each pad's analog disc deflection per port, -128..127 (0 = centre) */
    int disc_x[9][2];     /* [CHF_MAX_PADS] = the pointer (chfsession_disc_set) */
    int disc_y[9][2];

    void *audio;              /* audio_sdl.c state, NULL until started */
    void *gamepad;            /* gamepad_sdl.c state, NULL until started */
    void *debugger;           /* chfdebug, lazily created */
    int running;

    /* the last gamepad hot-plug event, for a frontend toast */
    pthread_mutex_t pad_event_mtx;
    char pad_event[128];
};

void settings_init(struct chfsession *s);
void settings_free_all(struct chfsession *s);

int paths_init(struct chfsession *s, const char *config_dir,
               const char *data_dir);
/* The in-process FujiNet makes its runtime root the process's working
 * directory (its PC firmware opens data/ relative to it), so a path a user
 * gave relative to where they started the app -- a command-line argument --
 * is resolved against the directory the first session was created in.
 * paths_resolve returns `path` itself when it is absolute, else `buf`. */
void paths_capture_launch_dir(void);
const char *paths_resolve(const char *path, char *buf, size_t bufsz);
/* Locate libfujinet and provision the runtime tree (fnconfig.ini + data/ +
 * SD/) into <data>/fujinet on first run. Returns 0, or -1 if no runtime is
 * available (not fatal to the session -- see fujinet_start). */
int paths_provision_fujinet(struct chfsession *s);

void session_set_error(struct chfsession *s, const char *fmt, ...);
/* A cartridge image from a file or a zip of one: malloc'd (caller frees),
 * or NULL with why. */
uint8_t *session_read_cart(const char *path, uint32_t *len, char *why, int whysz);
/* 1 if the FujiNet cartridge can serve the image (plans it), else 0 and why. */
int  session_cart_plan_check(const char *path, char *why, int whysz);

/* fujinet_runtime.c */
int  fujinet_start(struct chfsession *s);
void fujinet_stop(struct chfsession *s);
/* Block (up to timeout_ms) until the BoIP port accepts, so the cartridge's
 * first dial-out finds the listener. Returns 0 once up, -1 on timeout. */
int  fujinet_wait_for_boip(struct chfsession *s, int timeout_ms);

/* Who is holding an input down (the bits of the *_src arrays): the keyboard
 * and the controllers window together, and for each gamepad (by the stable
 * owner number its slot was given, 0 to CHF_MAX_PADS - 1) its buttons and its
 * analog stick / hat separately. So letting go of one never releases another,
 * even two pads or two buttons bound to the one key. */
#define CHF_MAX_PADS  8
#define SRC_KEYBOARD 1u
#define SRC_PAD(owner)   (1u << (1 + (owner)))
#define SRC_STICK(owner) (1u << (1 + CHF_MAX_PADS + (owner)))

/* paths.c: mkdir -p. */
int paths_mkdir_p(const char *path);

/* audio_sdl.c */
int  audio_start(struct chfsession *s);
void audio_stop(struct chfsession *s);

/* gamepad_sdl.c */
int  gamepad_start(struct chfsession *s);
void gamepad_stop(struct chfsession *s);
/* Called by the gamepad thread for the pad that owns `owner`. press: a pad
 * button drives a target (a disc direction or fire, a console key, RESET)
 * -- the pad's SRC_PAD source. stick: the analog stick or hat drives a stick
 * direction -- its SRC_STICK source. */
void session_gamepad_press(struct chfsession *s, int owner, int target, int down);
/* The analog disc of a pad (left stick): x/y -128..127, 0 = centre. */
void session_gamepad_disc(struct chfsession *s, int owner, int port, int x, int y);
void session_gamepad_stick(struct chfsession *s, int owner, int port, int act, int down);
/* Called by the gamepad thread on a connect/disconnect. */
void session_gamepad_event(struct chfsession *s, const char *text);

/* debug.c: the session tells the debugger when the machine goes away and
 * comes back (a power cycle replaces it), and starts the FujiNet trace
 * before the first. */
void chfdebug_trace_fujinet(struct chfsession *s);
void chfdebug_before_power_cycle(struct chfsession *s);
void chfdebug_after_power_cycle(struct chfsession *s);
void chfdebug_destroy(struct chfsession *s);

#ifdef __cplusplus
}
#endif

#endif /* CHF_SESSION_INTERNAL_H */
