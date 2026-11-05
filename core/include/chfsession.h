/*
 * chfsession -- the toolkit-agnostic desktop session for FujiNet Go Channel F.
 *
 * Owns the emulator (the Fairchild Channel F's F3850 (F8) core on its own
 * paced thread -- see core/chf/host.h), the FujiNet cartridge, the SDL audio
 * and gamepad backends, the in-process FujiNet runtime, the shared settings
 * store, the remappable key/pad bindings, the imported BIOS and the media
 * path layout. No ROM ships with the app: CONFIG and FujiNet apps boot on its
 * own stand-in BIOS; commercial Videocarts need the user's imported BIOS.
 * Frontends (GTK4, Qt6, AppKit, Win32) drive this API and do only windowing,
 * painting and event translation. A frontend that needs something which is
 * not one of those three things belongs here instead.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CHFSESSION_H
#define CHFSESSION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The picture: MAME's visible area for the NTSC driver, VRAM columns 4-105 and
 * rows 4-61 (102 x 58 cells) drawn 2 pixels wide and 4 lines tall = 204 x
 * 232. The PAL machines draw the same cells 5 lines tall on the TV; the frame
 * is the same 204 x 232 (chfsession_display_aspect gives the shape).
 * Frontends show it at the TV's 4:3 or with square pixels, per the "aspect"
 * setting. */
#define CHFSESSION_FB_WIDTH  204
#define CHFSESSION_FB_HEIGHT 232

/* FujiNet's BoIP listener and its web admin UI. High ports of this app's own
 * so a standalone fujinet-pc, or a sibling FujiNet Go app, never collides:
 * ADAM uses 65216/65214, Apple II 1985/64001, CoCo 65504/64002,
 * Intellivision 65503/64003, MSX 65505/64003, Astrocade 11500/11501,
 * ColecoVision 11502/11503, Atari 2600 11504/11505, NES 11506/11507, SMS
 * 11508/11509, Atari 7800 11510/11511, H89 11512/11513, Atari 800
 * 11514/11515, MS-DOS 11516/11517, Studio II 11518/11519, Odyssey 2
 * 11520/11521, Arcadia 2001 11522/11523, Atari 5200 11524/11525; the
 * firmware's MAME dev harness for the Channel F uses 9995.
 *
 * Direction: FujiNet LISTENS and the Channel F FujiNet cartridge dials in, as
 * on every sibling. That decides startup ordering -- see chfsession_start. */
#define CHFSESSION_BOIP_PORT  11526
#define CHFSESSION_WEBUI_PORT 11527

/* The host audio device rate; the core streams the console's tones at it
 * (mono, mirrored to both channels). */
#define CHFSESSION_AUDIO_RATE 48000

/* The accent colour every frontend uses for its highlights (the Map target,
 * held controls, the debugger's current line, the FujiNet status dot): the
 * yellow of the Videocarts (#F2C300). Dark text sits on it; on the light
 * themes a frontend that draws text in the accent uses the darker variant
 * (#8A6D00). */
#define CHFSESSION_ACCENT_RGB      0xF2C300
#define CHFSESSION_ACCENT_DARK_RGB 0x8A6D00

/* The machines, MAME's four parents (channelf.cpp). NTSC: the F3850 at
 * 3.579545 MHz / 2, 264 lines at 60.53 Hz. PAL: 2 MHz, 312 lines at 50.08 Hz.
 * The pairs differ in their default BIOS (the first 1K at $0000) and, for the
 * Videoplay 2 group, a half-level sound output. Setting "machine". */
typedef enum {
    CHF_MACHINE_CHANNELF = 0,         /* Fairchild Channel F (1976, NTSC): sl31253 */
    CHF_MACHINE_CHANNELF2,            /* Fairchild Channel F System II (1978, NTSC): sl90025 */
    CHF_MACHINE_VIDEOPLAY,            /* Saba Videoplay / Luxor Video Entertainment System (PAL): sl31253 */
    CHF_MACHINE_VIDEOPLAY2,           /* Saba Videoplay 2 / Luxor / ITT / Ingelen (PAL): sl90025, sound 0.5 */
    CHF_MACHINE_COUNT
} chf_machine;
const char *chf_machine_name(int machine);        /* "Fairchild Channel F (NTSC)" */
int chf_machine_is_pal(int machine);
/* The shape of the picture on the TV, width : height, for the "aspect"
 * setting: 0 the TV's 4:3; 1 square pixels (NTSC 204:232, PAL 204:290). */
void chfsession_display_aspect(int machine, int square_pixels, int *num, int *den);

typedef struct chfsession chfsession;
typedef struct chfdebug chfdebug;

/* All members optional (NULL = default).
 *  config_dir:  default $XDG_CONFIG_HOME/fujinet-go-channelf
 *  data_dir:    default $XDG_DATA_HOME/fujinet-go-channelf
 *  fujinet_lib: path to libfujinet.so/.dylib/.dll; default searches
 *               $FUJINET_LIB, the executable's directory, the install
 *               libdir, then tools/fujinet/work/out. "" disables FujiNet.
 *  fujinet_runtime_src: directory holding the pristine fnconfig.ini + data/
 *               + SD/ used to provision the user's runtime tree on first
 *               start (a macOS app passes its bundle's runtime dir). */
typedef struct {
    const char *config_dir;
    const char *data_dir;
    const char *fujinet_lib;
    const char *fujinet_runtime_src;
} chfsession_paths;

chfsession *chfsession_new(const chfsession_paths *paths);
void chfsession_free(chfsession *s);

/* ---- settings (shared INI; one store for every frontend of this target) --- */
int         chfsession_get_int(chfsession *s, const char *key, int def);
void        chfsession_set_int(chfsession *s, const char *key, int value);
const char *chfsession_get_str(chfsession *s, const char *key,
                               const char *def);
void        chfsession_set_str(chfsession *s, const char *key,
                               const char *value);
void        chfsession_settings_flush(chfsession *s);

/* ---- lifecycle ------------------------------------------------------------ */
typedef struct {
    const char *cart_path;    /* cartridge image (.bin/.chf, or a zip of
                                 exactly one); NULL boots the CONFIG client */
    int machine;              /* chf_machine */
    int bios;                 /* chf_bios_choice: which BIOS runs */
    int right_stick;          /* a gamepad's right stick twists (x) and
                                 pulls/pushes (y) the knob */
    int enable_fujinet;       /* start the in-process FujiNet runtime */
    int external_fujinet;     /* 1: don't start it; dial another FujiNet
                               * instance at boip_connect instead */
    const char *boip_connect; /* "host:port" or "port" the cartridge dials
                               * (external only); NULL/"" = 127.0.0.1:11526 */
    int enable_audio;         /* open the SDL audio device */
    int enable_gamepad;       /* start the SDL gamepad thread */
} chfsession_start_opts;

/* Fills opts from the settings store (keys: cart machine bios right_stick
 * enable_fujinet enable_audio enable_gamepad fujinet_external
 * fujinet_external_addr). */
void chfsession_default_opts(chfsession *s, chfsession_start_opts *opts);

/* Starts FujiNet (if enabled) and then the machine.
 *
 * The order is not arbitrary: FujiNet listens and the cartridge dials in, so
 * the listener has to exist before the machine's first transaction or the
 * CONFIG client boots reporting no link. start() brings FujiNet up first and
 * waits briefly for the port.
 *
 * Returns 0, or -1 with chfsession_last_error() set. FujiNet failing to
 * start is NOT fatal: the machine boots with the cartridge reporting the
 * link down, which is far more useful than refusing to run. */
int  chfsession_start(chfsession *s, const chfsession_start_opts *opts);
void chfsession_stop(chfsession *s);
int  chfsession_is_running(const chfsession *s);
const char *chfsession_last_error(const chfsession *s);
/* Power-cycle with the current settings (stop + default_opts + start):
 * what Preferences does after a machine option changes. */
int  chfsession_restart(chfsession *s);

/* ---- cartridges ------------------------------------------------------------
 * The FujiNet cartridge is always in the slot; CONFIG is its resident image.
 *
 * load_cart puts a local image (.bin or .chf) into the slot in place of
 * CONFIG and power-cycles the console onto it: a FujiNet app (a 16K window,
 * with or without APPBANK pages, carrying the FUJI claim at $47FC) is served
 * by the FujiNet cartridge with the mailbox live; a game runs alone as its
 * own Videocart (std, maze, hangman, chess, multi_old or multi, identified by
 * CRC against MAME's software list, else by MAME's by-path rule). A zip
 * holding exactly one image is read in place. A game needs the real BIOS: with
 * only the stand-in it is refused (chfsession_needs_bios). The path is
 * remembered in the "cart" setting, so it boots again next start. Returns 0
 * or -1 + error. */
int  chfsession_load_cart(chfsession *s, const char *path);
const char *chfsession_cart_path(const chfsession *s);
/* 1 if the image can run; else 0 and why. */
int  chfsession_check_cart(const char *path, char *why, int whysz);
/* 1 if the image is a FujiNet app (claims the mailbox). */
int  chfsession_cart_is_app(const char *path);
/* A description of the cartridge now running ("Videocart, 2 KB", "Maze
 * (2102 RAM)", "FujiNet CONFIG", "FujiNet app", ...); returns the length. */
int  chfsession_cart_type_name(chfsession *s, char *dst, int dstsz);

/* The console's RESET button: the F8's reset (ROMC 08: PC0 to $0000). The
 * cartridge has no reset pin, so a running FujiNet app restarts from the
 * cart's current image with its mailbox, page and RAM intact. Pressed and
 * released for a few frames (the controllers window and the menus); the bound
 * key (F1) holds it for as long as it is down. */
int  chfsession_reset(chfsession *s);
/* Reset to CONFIG: a power cycle. Ejects an opened cartridge (clears the
 * "cart" setting) and boots CONFIG, exactly as at power-on -- on the
 * hardware, only taking the console's power away brings CONFIG back.
 * Escape by default. Every power cycle (this, Eject, Open Cartridge) takes
 * the machine and BIOS settings as they are now, so an imported BIOS boots
 * here. */
int  chfsession_reset_to_config(chfsession *s);
/* Eject: the same as Reset to CONFIG (there is no "no cartridge" state
 * worth having on a FujiNet cartridge). */
int  chfsession_eject(chfsession *s);
/* What the running configuration is missing, as a mask of CHF_MISSING_*: the
 * stand-in BIOS met a game (or a call into a BIOS routine it does not have).
 * The frontends say "Import BIOS". Cleared by the next power cycle that fixes
 * it. */
#define CHF_MISSING_BIOS  0x01
unsigned chfsession_missing(chfsession *s);
/* Same as chfsession_missing() & CHF_MISSING_BIOS. */
int  chfsession_needs_bios(chfsession *s);
/* Why the stand-in stopped, e.g. "the cartridge called the BIOS at $0679",
 * "a Videocart needs the console's BIOS"; returns the length (0 if none). */
int  chfsession_missing_reason(chfsession *s, char *dst, int dstsz);
/* Pause/resume the machine (the debugger's Run/Stop is separate). */
void chfsession_set_paused(chfsession *s, int paused);
int  chfsession_is_paused(const chfsession *s);

/* ---- video ---------------------------------------------------------------
 * copy_frame copies the latest frame into dst (FB_WIDTH*FB_HEIGHT uint32
 * XRGB8888 pixels) iff its serial differs from *serial_inout, updates it and
 * returns 1; returns 0 when unchanged, leaving dst alone. Pass 0 to force a
 * copy. */
int  chfsession_copy_frame(chfsession *s, uint32_t *dst, uint64_t *serial_inout);
/* Ask to be told when a new frame is ready, for a frontend with no frame
 * clock of its own: cb(ctx) runs on the machine thread after each frame is
 * published (a frame identical to the previous one is not published again).
 * Keep it quick (post an event and return) and do not call into the session
 * from it. Passing NULL stops the calls, and returns only after any call in
 * flight has finished, so ctx may be freed right after. */
typedef void (*chfsession_frame_cb)(void *ctx);
void chfsession_set_frame_callback(chfsession *s, chfsession_frame_cb cb, void *ctx);
/* Frames the machine has run since it started. Unlike copy_frame's serial it
 * counts every frame, including the ones identical to their predecessor:
 * use it to ask "is the machine running", not "is the picture changing". */
uint64_t chfsession_frame_count(chfsession *s);
/* The machine's refresh rate, rounded (NTSC 60 Hz for 60.53; PAL 50 Hz). */
int  chfsession_refresh_rate(chfsession *s);

/* Feed the UI's frame-clock ticks (CLOCK_MONOTONIC ns). While a steady
 * stream near the console's refresh rate arrives, the machine phase-locks
 * one frame per tick; otherwise it paces on the wall clock. A frontend with
 * no frame clock simply never calls this. */
void chfsession_notify_vsync(chfsession *s, int64_t frame_time_ns);

/* ---- audio ---------------------------------------------------------------
 * Owned by the session (SDL) when opts.enable_audio was set. A frontend
 * that wants the device itself can pull interleaved stereo float frames at
 * CHFSESSION_AUDIO_RATE instead. Returns the frames written (silence is
 * written for any shortfall). */
int  chfsession_render_audio(chfsession *s, float *out, int nframes);
void chfsession_set_volume(chfsession *s, int percent);

/* ---- input ---------------------------------------------------------------
 * Every control the machine has, flattened into one target index so the
 * bindings table, the settings store and the frontends can all name them:
 * per player (1, then 2) the hand controller's eight contacts, then the four
 * console buttons, then RESET, then the session's own actions.
 *
 * The hand controller is one knob with four axes, each a pair of contacts:
 * the stick tilts on two (RIGHT/LEFT, BACK/FORWARD: eight ways with the
 * diagonals), the knob twists (counter-clockwise / clockwise) and moves along
 * the handle (PULL up / PUSH down). Any combination may be held at once. The
 * action number IS the hardware bit (1 << act) of the controller byte the
 * CPU reads, active low on the port:
 *
 *   Player 1 = the RIGHT controller, read on CPU port 1 (MAME's PLAYER 1)
 *   Player 2 = the LEFT controller, read on CPU port 4 (MAME's PLAYER 2)
 *
 * The console buttons are bits 0-3 of port 0, read the same way. */
typedef enum {
    CHF_ACT_RIGHT = 0,                /* $01 */
    CHF_ACT_LEFT,                     /* $02 */
    CHF_ACT_BACK,                     /* $04: toward the player ("down") */
    CHF_ACT_FORWARD,                  /* $08: away ("up") */
    CHF_ACT_TWIST_CCW,                /* $10: twist left (counter-clockwise) */
    CHF_ACT_TWIST_CW,                 /* $20: twist right (clockwise) */
    CHF_ACT_PULL,                     /* $40: pull the knob up */
    CHF_ACT_PUSH,                     /* $80: push the knob down (the plunger) */
    CHF_ACT_PER_PORT
} chf_action;
#define CHF_PLAYERS 2

/* Console buttons, CHF_TARGET_KEY(k), k = 0..3, bit k of port 0. Named as the
 * case prints them (MAME's port names swap MODE and HOLD; the bit is the
 * button number minus one either way). */
#define CHF_KEY_COUNT 4
#define CHF_KEY_TIME   0              /* button 1 */
#define CHF_KEY_MODE   1              /* button 2 */
#define CHF_KEY_HOLD   2              /* button 3 */
#define CHF_KEY_START  3              /* button 4 */

typedef enum {
    CHF_SW_RESET = 0,                 /* F1 by default */
    CHF_SW_COUNT
} chf_switch;

typedef enum {
    CHF_SYSACT_RESET_CONFIG = 0,      /* Escape by default */
    CHF_SYSACT_DEBUG_STOP,            /* stop in the debugger */
    CHF_SYSACT_COUNT
} chf_sysaction;

#define CHF_TARGET_PORT(port, act) ((port) * CHF_ACT_PER_PORT + (act))
#define CHF_TARGET_KEY(key)        (CHF_PLAYERS * CHF_ACT_PER_PORT + (key))
#define CHF_TARGET_SWITCH(sw)      (CHF_PLAYERS * CHF_ACT_PER_PORT + CHF_KEY_COUNT + (sw))
#define CHF_TARGET_SYSACT(sa)      (CHF_PLAYERS * CHF_ACT_PER_PORT + CHF_KEY_COUNT + CHF_SW_COUNT + (sa))
#define CHF_TARGET_COUNT           (CHF_PLAYERS * CHF_ACT_PER_PORT + CHF_KEY_COUNT + CHF_SW_COUNT + CHF_SYSACT_COUNT)

/* Apply one control directly. `down` is press/release. The controllers and
 * the buttons are sampled at the frame boundary, as MAME's input ports are. */
void chfsession_press(chfsession *s, int target, int down);
void chfsession_sysaction(chfsession *s, int sysact);
/* Release everything the keyboard holds (focus loss). */
void chfsession_release_all(chfsession *s);
/* What the console sees held on a controller right now, from every source
 * (keyboard, gamepads, the pointer), one bit per chf_action (1 = held). For
 * the controllers window's highlights. */
unsigned chfsession_buttons_held(chfsession *s, int port);
/* The pointer's own source (the controllers window's mouse): the chf_action
 * bits it holds on `port`, ORed with the others. Pass 0 when the drag ends or
 * the window loses the pointer. */
void chfsession_pointer_set(chfsession *s, int port, unsigned mask);
/* One notch of a twist from a mouse wheel or a trackpad: dir < 0
 * counter-clockwise, > 0 clockwise; held for 6 frames, so a program that
 * polls once a frame sees it. */
void chfsession_twist_pulse(chfsession *s, int port, int dir);
/* Whether console button `key` (0-3) is held, from any source. */
int chfsession_key_held(chfsession *s, int key);
/* Whether a console switch is held (chf_switch). */
int chfsession_switch_held(chfsession *s, int sw);

/* ---- the on-screen controllers ---------------------------------------------
 * One layout for every toolkit: the console's button panel (RESET and the
 * four numbered buttons with their legends) and both hand controllers, in
 * layout units (1.0 = a console button's width) from the top-left. Each
 * controller is drawn as two views of the same knob: from above (the STICK
 * area: eight direction wedges round the knob, which shifts as it tilts and
 * rotates as it twists, and the TWIST arrows either side) and from the side
 * (the PULL / PUSH arrows along the handle). `target` is the binding target
 * the part drives (CHF_TARGET_*), or -1 for the STICK area, whose direction
 * comes from chfsession_stick_from_vector. Static; valid for the process. */
typedef enum {
    CHF_CTL_BUTTON = 0,     /* a console button: TIME, MODE, HOLD, START */
    CHF_CTL_RESET,          /* the console's RESET */
    CHF_CTL_STICK,          /* the knob from above: drag to tilt, left-press
                               on the cap to push, right-press to pull, wheel
                               to twist */
    CHF_CTL_DIRECTION,      /* one of the eight wedges round the knob (press
                               and hold); `act_mask` holds its one or two bits */
    CHF_CTL_TWIST,          /* a twist arrow (CCW or CW) */
    CHF_CTL_AXIS,           /* a PULL or PUSH arrow on the side view */
    CHF_CTL_SIDE,           /* the side view's handle (drawn, not pressed) */
    CHF_CTL_CAPTION         /* text: "Right controller - Player 1" */
} chf_control_kind;

typedef struct {
    int kind;                /* chf_control_kind */
    const char *label;       /* "1", "RESET", "PULL", "Right controller" */
    const char *legend;      /* "TIME", "MODE", "HOLD", "START", or NULL */
    int port;                /* 0 player 1 (right), 1 player 2 (left), -1 console */
    int target;              /* binding target, or -1 */
    unsigned act_mask;       /* the chf_action bits it drives (controller parts) */
    float x, y, w, h;
} chf_control;

/* The layout; *n is its entry count; width/height (layout units) are its
 * extent so a toolkit can size its drawing area. Every machine prints the
 * same panel. */
const chf_control *chfsession_controls_layout(int machine, int *n, float *width, float *height);
/* The topmost control under a point in layout units, or -1. */
int chfsession_controls_hit(int machine, float x, float y);
/* The stick's direction bits for a pointer at (dx, dy) from the knob's centre
 * (+x right, +y toward the player, in any unit) with `radius` the knob's
 * reach: nothing inside a quarter of it, else one of eight 45-degree sectors
 * (RIGHT, LEFT, BACK, FORWARD and their diagonals). */
unsigned chfsession_stick_from_vector(float dx, float dy, float radius);
/* The label a console button has ("1"), its legend ("TIME"), or NULL. */
const char *chfsession_key_label(int machine, int key);
const char *chfsession_key_legend(int machine, int key);

/* ---- keyboard translation ------------------------------------------------
 * keysym is an X11/xkb keysym (== a GDK keyval; Qt, Win32 and AppKit map
 * through the HID tables below), so one bindings table serves every
 * frontend. Returns 1 if the key drives a target (and has been applied /
 * released), 0 if it should be ignored. System actions are reported through
 * chfsession_key_sysaction instead and left to the frontend. */
int  chfsession_key(chfsession *s, uint32_t keysym, int down);
/* The system action a keysym is bound to, or -1. */
int  chfsession_key_sysaction(chfsession *s, uint32_t keysym);

/* Non-printing keys in the keysym space the frontends translate to. */
enum {
    CHF_KEYSYM_NONE = 0,
    CHF_KEYSYM_UP = 0xff52, CHF_KEYSYM_DOWN = 0xff54,
    CHF_KEYSYM_LEFT = 0xff51, CHF_KEYSYM_RIGHT = 0xff53,
    CHF_KEYSYM_ESCAPE = 0xff1b, CHF_KEYSYM_RETURN = 0xff0d,
    CHF_KEYSYM_BACKSPACE = 0xff08, CHF_KEYSYM_TAB = 0xff09,
    CHF_KEYSYM_SPACE = 0x20,
    CHF_KEYSYM_F1 = 0xffbe, CHF_KEYSYM_F2, CHF_KEYSYM_F3, CHF_KEYSYM_F4,
    CHF_KEYSYM_F5, CHF_KEYSYM_F6, CHF_KEYSYM_F7, CHF_KEYSYM_F8,
    CHF_KEYSYM_F9, CHF_KEYSYM_F10, CHF_KEYSYM_F11, CHF_KEYSYM_F12,
    CHF_KEYSYM_LSHIFT = 0xffe1, CHF_KEYSYM_RSHIFT = 0xffe2,
    CHF_KEYSYM_LCTRL = 0xffe3, CHF_KEYSYM_RCTRL = 0xffe4,
    CHF_KEYSYM_LALT = 0xffe9, CHF_KEYSYM_RALT = 0xffea,
    CHF_KEYSYM_KP_0 = 0xffb0, CHF_KEYSYM_KP_1, CHF_KEYSYM_KP_2,
    CHF_KEYSYM_KP_3, CHF_KEYSYM_KP_4, CHF_KEYSYM_KP_5, CHF_KEYSYM_KP_6,
    CHF_KEYSYM_KP_7, CHF_KEYSYM_KP_8, CHF_KEYSYM_KP_9,
    CHF_KEYSYM_KP_ENTER = 0xff8d, CHF_KEYSYM_KP_MULTIPLY = 0xffaa,
    CHF_KEYSYM_KP_DIVIDE = 0xffaf, CHF_KEYSYM_KP_PERIOD = 0xffae
};
/* Native key codes for the platforms whose toolkits do not deliver keysyms:
 * Windows scan code (set 1, with the E0 flag), Linux evdev code (GTK/Qt
 * keycode minus 8) and macOS virtual key code, each mapped to a keysym. 0
 * when unknown. */
uint32_t chfsession_keysym_from_win_scancode(unsigned scancode, int extended);
uint32_t chfsession_keysym_from_evdev(unsigned code);
uint32_t chfsession_keysym_from_macos_keycode(unsigned keycode);
/* Name for a keysym ("F1", "Space", "a", "Keypad 5"); returns length. */
int chfsession_keysym_name(uint32_t keysym, char *dst, int dstsz);

/* ---- remappable bindings --------------------------------------------------
 * Every target can be driven by one keyboard key and one gamepad button.
 * Rebinding STEALS: a key drives exactly one target, because one keystroke
 * doing two things is worse than losing the old binding. Persisted in the
 * settings store under "bindings" as only the entries that differ from the
 * defaults. */
typedef enum {
    CHF_PAD_BTN_NONE = -1,
    CHF_PAD_BTN_SOUTH = 0, CHF_PAD_BTN_EAST, CHF_PAD_BTN_WEST,
    CHF_PAD_BTN_NORTH, CHF_PAD_BTN_BACK, CHF_PAD_BTN_GUIDE,
    CHF_PAD_BTN_START, CHF_PAD_BTN_LEFT_STICK, CHF_PAD_BTN_RIGHT_STICK,
    CHF_PAD_BTN_LEFT_SHOULDER, CHF_PAD_BTN_RIGHT_SHOULDER,
    CHF_PAD_BTN_DPAD_UP, CHF_PAD_BTN_DPAD_DOWN, CHF_PAD_BTN_DPAD_LEFT,
    CHF_PAD_BTN_DPAD_RIGHT,
    CHF_PAD_BTN_LEFT_TRIGGER, CHF_PAD_BTN_RIGHT_TRIGGER,
    CHF_PAD_BTN_COUNT,
    /* Raw joystick bands for devices SDL has no gamepad mapping for (a
     * plain HID adapter): button index, and hat directions (hat*8 + dir). */
    CHF_PAD_BTN_RAW_BASE = 64, CHF_PAD_BTN_RAW_LAST = 127,
    CHF_PAD_HAT_BASE = 192, CHF_PAD_HAT_LAST = 255
} chf_pad_button;
#define CHF_PAD_HAT_DIRS 8
#define CHF_PAD_BTN_IS_NAMED(b) ((b) >= 0 && (b) < CHF_PAD_BTN_COUNT)
#define CHF_PAD_BTN_IS_RAW(b)   ((b) >= CHF_PAD_BTN_RAW_BASE && (b) <= CHF_PAD_BTN_RAW_LAST)
#define CHF_PAD_BTN_IS_HAT(b)   ((b) >= CHF_PAD_HAT_BASE && (b) <= CHF_PAD_HAT_LAST)

typedef struct {
    uint32_t keysym;     /* 0 = no key */
    int      button;     /* chf_pad_button, NONE = no gamepad button */
} chf_binding;

const char *chf_target_name(int target);           /* "Player 1: Up" */
const char *chf_target_short_name(int target);     /* "Up" */
chf_binding chfsession_binding_get(chfsession *s, int target);
/* Bind; the previous holder of the key/button (if any) is described into
 * `stolen` (may be NULL). keysym 0 / button NONE unbinds. */
void chfsession_binding_set_key(chfsession *s, int target, uint32_t keysym,
                                char *stolen, int stolensz);
void chfsession_binding_set_button(chfsession *s, int target, int button,
                                   char *stolen, int stolensz);
void chfsession_bindings_reset(chfsession *s);
const char *chf_pad_button_name(int button);
/* The target a keysym / pad button drives, or -1. */
int chfsession_target_for_key(chfsession *s, uint32_t keysym);
int chfsession_target_for_button(chfsession *s, int port, int button);

/* Map mode: after begin(), the next gamepad button pressed on any pad is
 * reported by poll() (returns 1 and the button). cancel() disarms. */
void chfsession_gamepad_capture_begin(chfsession *s);
void chfsession_gamepad_capture_cancel(chfsession *s);
int  chfsession_gamepad_capture_poll(chfsession *s, int *button);

/* A gamepad's left stick is always the controller's stick (an eight-way
 * switch with hysteresis: in at 0.5 of full deflection, out at 0.35). With
 * "right_stick" on (the default) the right stick is the knob's other two
 * axes: x twists (left = counter-clockwise), y pulls (up) and pushes (down),
 * with the same thresholds. Takes effect at once. */
void chfsession_set_right_stick(chfsession *s, int on);

/* ---- gamepads (SDL, hotplugged; started by chfsession_start) -------------
 * Pads are assigned to ports in connection order unless assigned
 * explicitly. A pad that disconnects and reconnects gets its port back. */
int  chfsession_gamepad_count(chfsession *s);
int  chfsession_gamepad_name(chfsession *s, int idx, char *dst, int dstsz);
void chfsession_gamepad_assign(chfsession *s, int idx, int port); /* -1 = auto */
int  chfsession_gamepad_assignment(chfsession *s, int idx);
int  chfsession_gamepad_effective_port(chfsession *s, int idx);
/* Bumped on every add/remove so a frontend can refresh its lists cheaply. */
unsigned chfsession_gamepad_generation(chfsession *s);
/* The last hot-plug event as text ("Connected: 8BitDo SN30 Pro (player 1)",
 * "Disconnected: ..."), for a toast; returns length, 0 if none yet. */
int  chfsession_gamepad_last_event(chfsession *s, char *dst, int dstsz);

/* ---- cross-thread system actions ------------------------------------------
 * The gamepad thread cannot call into a UI toolkit, so a system action it
 * resolves is posted here and a frontend's timer takes it. */
void chfsession_sysaction_post(chfsession *s, int sysact);
int  chfsession_sysaction_take(chfsession *s, int *out);

/* ---- the ROMs ----------------------------------------------------------------
 * Never shipped (see COMPLIANCE.md). CONFIG and every FujiNet app boot on the
 * app's own stand-in BIOS; a Videocart needs the real one. The console's BIOS
 * is two 1K ROMs: the first half at $0000 (one of two versions) and sl31254 at
 * $0400. Import copies a user's own dumps into the ROM directory, each
 * identified by size and CRC-32 against MAME's table whatever it is called,
 * and stored under MAME's name. The input can be a file, a zip (MAME's
 * channelf.zip works) or a folder (searched 8 levels deep); a 2K file that is
 * a first half followed by sl31254 is split. */
typedef enum {
    CHF_ROM_SL31253 = 0,      /* sl31253.rom: Channel F's $0000-$03FF (04694ed9) */
    CHF_ROM_SL90025,          /* sl90025.rom: System II / Luxor $0000-$03FF (015c1e38) */
    CHF_ROM_SL31254,          /* sl31254.rom: $0400-$07FF, every machine (9c047ba3) */
    CHF_ROM_COUNT
} chf_rom_id;

typedef struct {
    const char *name;         /* "Channel F BIOS, first half (sl31253.rom)" */
    const char *group;        /* "Console" */
    const char *file;         /* stored as */
    uint32_t size, crc;
} chf_rom_item;

int chfsession_rom_item_count(void);
const chf_rom_item *chfsession_rom_item(int i);
/* 1 if item i is in the ROM directory. */
int chfsession_rom_item_available(chfsession *s, int i);

/* Which BIOS runs, setting "bios": AUTO takes the machine's own first half,
 * else the other one, and the stand-in when neither (or sl31254) is imported. */
typedef enum {
    CHF_BIOS_AUTO = 0,
    CHF_BIOS_SL31253,
    CHF_BIOS_SL90025,
    CHF_BIOS_STANDIN,         /* always the app's own (FujiNet apps only) */
    CHF_BIOS_CHOICE_COUNT
} chf_bios_choice;
const char *chf_bios_choice_name(int choice);
/* 1 if a real BIOS would run on `machine` with the current "bios" setting. */
int chfsession_have_bios(chfsession *s, int machine);
/* The BIOS the running machine uses: CHF_ROM_SL31253 / CHF_ROM_SL90025, or -1
 * for the stand-in. */
int chfsession_bios_in_use(chfsession *s);
/* Import from a file, a zip or a folder. Returns how many items it imported
 * (0 = recognised nothing), or -1 with the error in `msg`; `msg` always says
 * what happened (names the items, warns about an unrecognised dump of the
 * right size). Takes effect at the next power cycle. */
int chfsession_rom_import(chfsession *s, const char *path, char *msg, int msgsz);
/* 1 if `path` is a ROM this app imports (a dropped file or zip), by size and
 * CRC (a zip: by any of its entries). */
int chfsession_media_is_rom(const char *path);
const char *chfsession_roms_path(const chfsession *s);

/* ---- FujiNet -------------------------------------------------------------*/
/* Parses the "Use external FujiNet" address field: "host:port" (host a name
 * or an IPv4 literal) or a bare "port". Empty text means the default,
 * 127.0.0.1:11526. Returns 0 and fills host/port, or -1 when the text is not
 * an address:port (nothing is written then). */
int         chfsession_parse_hostport(const char *text, char *host,
                                          int hostsz, int *port);
int         chfsession_fujinet_running(const chfsession *s);
/* 1 when the session was started to dial an external FujiNet. */
int         chfsession_fujinet_external(const chfsession *s);
/* "host:port" the cartridge dials (the running session's, or the default
 * 127.0.0.1:11526 before the first start). */
const char *chfsession_boip_endpoint(const chfsession *s);
const char *chfsession_fujinet_webui_url(const chfsession *s);
int         chfsession_fujinet_copy_log(chfsession *s, char *dst, int max);
/* The cartridge's link to FujiNet: 1 up, 0 down, -1 not running. */
int         chfsession_cart_link_up(chfsession *s);
/* Status text from the cartridge ("connected", "link down", "loading 42%",
 * "game running; mailbox closed", ...). Returns length. */
int         chfsession_cart_status(chfsession *s, char *dst, int dstsz);
/* 1 once anything other than CONFIG is in the cartridge (a network boot or
 * an opened cartridge file). */
int         chfsession_cart_booted_game(chfsession *s);

/* ---- media ---------------------------------------------------------------
 * Import a cartridge into FujiNet's SD folder so CONFIG can boot it through
 * the cartridge. It must be one the FujiNet cartridge can serve: a ROM image
 * of up to 16K (served flat at $0800), or a FujiNet app with APPBANK pages; a
 * Videocart with its own RAM or banking (maze, hangman, chess, multi) cannot
 * run there and is refused with the reason. A zip of one is unpacked. Returns
 * 0 and the destination path, or -1 with the error. */
int  chfsession_import_cart_to_sd(chfsession *s, const char *src_path,
                                  char *dest_out, int dest_sz);
/* Routes a dropped file: BIOS ROMs (also zips of them) to the ROM directory
 * (as rom_import), cartridge images (.bin .chf .zip) to the cartridge
 * directory (the returned path is usable with load_cart), anything else to
 * the FujiNet SD folder. */
int  chfsession_import_media(chfsession *s, const char *src_path,
                             char *dest_out, int dest_sz);
int  chfsession_media_is_cartridge(const char *path);

const char *chfsession_config_path(const chfsession *s);
const char *chfsession_data_path(const chfsession *s);
const char *chfsession_carts_path(const chfsession *s);
/* FujiNet's SD folder (Media > Open SD Folder shows it in the file manager). */
const char *chfsession_sd_path(const chfsession *s);

/* ---- debugger ------------------------------------------------------------*/
chfdebug *chfsession_debugger(chfsession *s);

#ifdef __cplusplus
}
#endif

#endif /* CHFSESSION_H */
