/*
 * chfsession's private state. Not installed; only the core/src sources
 * include it. One struct shared by the session's C modules (settings,
 * paths, media, roms, audio, gamepads, bindings).
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
    char roms_dir[CHF_PATH_MAX];       /* <data>/roms: the imported BIOS */
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
                                        * (a mapper), not the FujiNet cartridge */
    int run_bios;                      /* CHF_ROM_SL31253 / _SL90025, -1: the stand-in */
    int refused_bios;                  /* a game was refused for want of the BIOS
                                        * (cleared by the next power cycle) */
    char refused_why[256];
    /* keys the keyboard currently holds, by target, so a release clears
     * exactly what its press asserted even if the binding changed meanwhile */
    uint32_t held_keysym[CHF_TARGET_COUNT];

    /* What drives each controller contact, console button and RESET, one bit
     * per source (SRC_*), so that letting go of one source never releases
     * another's. input_mtx covers these arrays and the calls that hand the
     * result to the host. */
    pthread_mutex_t input_mtx;
    uint32_t pad_src[CHF_PLAYERS][CHF_ACT_PER_PORT];
    uint32_t key_src[CHF_KEY_COUNT];
    uint32_t sw_src[CHF_SW_COUNT];

    /* twist pulses (the wheel): each held until the machine has run this many
     * frames (0: not pulsing); a small thread lets them go (input.c) */
    uint64_t pulse_until[CHF_PLAYERS][2];
    pthread_t pulse_thread;
    pthread_cond_t pulse_cond;
    int pulse_thread_up;
    int pulse_quit;

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
 * and the controllers window's buttons together, the pointer's own drag
 * (chfsession_pointer_set), a wheel pulse, and for each gamepad (by the stable
 * owner number its slot was given, 0 to CHF_MAX_PADS - 1) its buttons and its
 * analog sticks / hat separately. So letting go of one never releases
 * another, even two pads or two buttons bound to the one key. */
#define CHF_MAX_PADS  8
#define SRC_KEYBOARD 1u
#define SRC_PAD(owner)   (1u << (1 + (owner)))
#define SRC_STICK(owner) (1u << (1 + CHF_MAX_PADS + (owner)))
#define SRC_POINTER  (1u << (1 + 2 * CHF_MAX_PADS))
#define SRC_PULSE    (1u << (2 + 2 * CHF_MAX_PADS))

/* paths.c: mkdir -p. */
int paths_mkdir_p(const char *path);

/* roms.c */
/* The BIOS bytes, malloc'd (caller frees), or NULL when the item is not in the
 * ROM directory (or embedded). */
uint8_t *roms_load(struct chfsession *s, int id, uint32_t *size);
/* Copies a developer build's embedded images into the ROM directory on first
 * run (WITH_CHF_ROMS=ON); nothing in a shipped build. */
void roms_provision_embedded(struct chfsession *s);
/* The first half that runs on `machine` under `choice` (a chf_bios_choice),
 * or -1 for the stand-in; and whether sl31254 is there too. */
int  roms_bios_for(struct chfsession *s, int machine, int choice);
int  roms_import_ex(struct chfsession *s, const char *path, char *msg, int msgsz,
                    char *first_dest, int destsz);

/* input.c: the controllers, the console's buttons and RESET, from every
 * source. */
void input_init(struct chfsession *s);
/* Let go of every source and tell the host (a start). */
void input_clear(struct chfsession *s);
/* A power cycle replaced the machine: hand it what is still held. */
void input_after_power_cycle(struct chfsession *s);
/* Stop the pulse thread, if it is up (stop / free). */
void input_stop(struct chfsession *s);

/* audio_sdl.c */
int  audio_start(struct chfsession *s);
void audio_stop(struct chfsession *s);

/* gamepad_sdl.c */
int  gamepad_start(struct chfsession *s);
void gamepad_stop(struct chfsession *s);
/* Called by the gamepad thread for the pad that owns `owner`. press: a pad
 * button drives a target (a controller contact, a console button, RESET) --
 * the pad's SRC_PAD source. stick: an analog stick or hat drives a controller
 * contact -- its SRC_STICK source. */
void session_gamepad_press(struct chfsession *s, int owner, int target, int down);
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
