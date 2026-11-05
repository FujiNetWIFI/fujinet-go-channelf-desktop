/*
 * session.c -- the frontend contract (core/include/chfsession.h) over the
 * Fairchild Channel F host (core/chf/host.h): a Signetics 2650 console with
 * no BIOS, so the only images are game cartridges and FujiNet apps.
 *
 * The C modules beside it (settings, paths, media, roms, keyboard, audio,
 * gamepads, bindings) share the session struct through session_internal.h;
 * this file owns the lifecycle, the power cycles, the cartridges, the input
 * routing and the cartridge status. Ported from the O2 sibling's
 * session.c.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "session_internal.h"
#include "bindings.h"
#include "chfdebug.h"
#include "host.h"
#include "zipread.h"
#include "carts.h"
#include "fuji_mailbox.h"
#include "chfmap.h"

/* The largest image the cartridge serves: an APPBANK image (8K and up to 31
 * pages of 2K), with room to say "too big" rather than truncate. */
#define CART_MAX (CHFMAP_STORE + 1u)

/* ---- the machines ----------------------------------------------------------- */

/* chf_machine_name() and chf_machine_is_pal() are the machine's (machine.c). */

static int clamp_machine(int machine)
{
    return machine >= 0 && machine < CHF_MACHINE_COUNT ? machine : CHF_MACHINE_ARCADIA;
}

/* ---- helpers shared with the C modules ----------------------------------- */

void session_set_error(struct chfsession *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->last_error, sizeof s->last_error, fmt, ap);
    va_end(ap);
}

void session_gamepad_event(struct chfsession *s, const char *text)
{
    pthread_mutex_lock(&s->pad_event_mtx);
    snprintf(s->pad_event, sizeof s->pad_event, "%s", text ? text : "");
    pthread_mutex_unlock(&s->pad_event_mtx);
}

int chfsession_gamepad_last_event(chfsession *s, char *dst, int dstsz)
{
    int n;
    if (!dst || dstsz <= 0)
        return 0;
    pthread_mutex_lock(&s->pad_event_mtx);
    n = snprintf(dst, (size_t)dstsz, "%s", s->pad_event);
    pthread_mutex_unlock(&s->pad_event_mtx);
    return n;
}

#ifdef CHF_GAMEPAD_STUB
int gamepad_start(struct chfsession *s) { session_set_error(s, "gamepad support not built"); return -1; }
void gamepad_stop(struct chfsession *s) { (void)s; }
int chfsession_gamepad_count(chfsession *s) { (void)s; return 0; }
int chfsession_gamepad_name(chfsession *s, int i, char *d, int n) { (void)s; (void)i; if (d && n > 0) d[0] = 0; return 0; }
void chfsession_gamepad_assign(chfsession *s, int i, int p) { (void)s; (void)i; (void)p; }
int chfsession_gamepad_assignment(chfsession *s, int i) { (void)s; (void)i; return -1; }
int chfsession_gamepad_effective_port(chfsession *s, int i) { (void)s; (void)i; return -1; }
unsigned chfsession_gamepad_generation(chfsession *s) { (void)s; return 0; }
void chfsession_gamepad_capture_begin(chfsession *s) { (void)s; }
void chfsession_gamepad_capture_cancel(chfsession *s) { (void)s; }
int chfsession_gamepad_capture_poll(chfsession *s, int *b) { (void)s; (void)b; return 0; }
#endif

/* ---- lifecycle ------------------------------------------------------------- */

chfsession *chfsession_new(const chfsession_paths *paths)
{
    struct chfsession *s;

    paths_capture_launch_dir();
    s = calloc(1, sizeof *s);
    if (!s)
        return NULL;

    pthread_mutex_init(&s->settings_mtx, NULL);
    pthread_mutex_init(&s->sysact_mtx, NULL);
    pthread_mutex_init(&s->pad_event_mtx, NULL);
    pthread_mutex_init(&s->input_mtx, NULL);

    if (paths_init(s, paths ? paths->config_dir : NULL,
                   paths ? paths->data_dir : NULL) != 0) {
        chfsession_free(s);
        return NULL;
    }
    settings_init(s);
    bindings_init(s);

    if (paths && paths->fujinet_lib) {
        snprintf(s->fujinet_lib, sizeof s->fujinet_lib, "%s", paths->fujinet_lib);
        s->fujinet_disabled = !paths->fujinet_lib[0];
    }
    if (paths && paths->fujinet_runtime_src)
        snprintf(s->fujinet_runtime_src, sizeof s->fujinet_runtime_src, "%s",
                 paths->fujinet_runtime_src);
    return s;
}

void chfsession_free(chfsession *s)
{
    if (!s)
        return;
    chfsession_stop(s);
    chfsession_settings_flush(s);
    settings_free_all(s);
    chfdebug_destroy(s);
    pthread_mutex_destroy(&s->settings_mtx);
    pthread_mutex_destroy(&s->sysact_mtx);
    pthread_mutex_destroy(&s->pad_event_mtx);
    pthread_mutex_destroy(&s->input_mtx);
    free(s);
}

void chfsession_default_opts(chfsession *s, chfsession_start_opts *opts)
{
    memset(opts, 0, sizeof *opts);
    opts->machine = clamp_machine(chfsession_get_int(s, "machine", CHF_MACHINE_ARCADIA));
    opts->analog_joystick = chfsession_get_int(s, "analog_joystick", 1);
    opts->enable_fujinet = chfsession_get_int(s, "enable_fujinet", 1);
    opts->external_fujinet = chfsession_get_int(s, "fujinet_external", 0);
    opts->boip_connect = chfsession_get_str(s, "fujinet_external_addr", NULL);
    opts->enable_audio = chfsession_get_int(s, "enable_audio", 1);
    opts->enable_gamepad = chfsession_get_int(s, "enable_gamepad", 1);
    opts->cart_path = chfsession_get_str(s, "cart", NULL);
    if (opts->cart_path && !opts->cart_path[0])
        opts->cart_path = NULL;
}

/* ---- images: what a cartridge file is ---------------------------------------- */

typedef struct {
    int app;                  /* a FujiNet app: claims the mailbox */
    int type;                 /* chf_cart_type of a game (CHF_CART_FUJINET for an app) */
    chfmap_plan_t plan;        /* what the FujiNet cartridge makes of the image */
    chfmap_err_t plan_err;
} cart_info;

static int path_is_zip(const char *path)
{
    size_t n = strlen(path);
    return n > 4 && (!strcmp(path + n - 4, ".zip") || !strcmp(path + n - 4, ".ZIP"));
}

static int name_has_cart_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".bin") || !strcasecmp(dot, ".rom") ||
                   !strcasecmp(dot, ".chf"));
}

/* A zip holding one cartridge image (MAME's romset zips): the one entry
 * with a cartridge extension, or the only entry there is. */
static uint8_t *read_cart_zip(const char *path, uint32_t *len, char *why, int whysz)
{
    zipread *z = zipread_open(path);
    uint8_t *buf = NULL;
    int pick = -1, ncart = 0, nfile = 0, only = -1;

    if (!z) {
        snprintf(why, (size_t)whysz, "cannot read %s as a zip", path);
        return NULL;
    }
    for (int i = 0; i < zipread_count(z); i++) {
        const char *name = zipread_name(z, i);
        size_t nl = strlen(name);
        if (nl == 0 || name[nl - 1] == '/')
            continue;
        nfile++;
        only = i;
        if (name_has_cart_ext(name)) {
            ncart++;
            pick = i;
        }
    }
    if (ncart != 1 && !(ncart == 0 && nfile == 1)) {
        snprintf(why, (size_t)whysz, "%s must hold exactly one cartridge image (found %d)",
                 path, ncart ? ncart : nfile);
        zipread_close(z);
        return NULL;
    }
    if (pick < 0)
        pick = only;
    if (zipread_extract(z, pick, &buf, len, CART_MAX) != 0 || !buf || *len == 0) {
        snprintf(why, (size_t)whysz, "cannot extract the cartridge from %s", path);
        free(buf);
        buf = NULL;
    }
    zipread_close(z);
    return buf;
}

uint8_t *session_read_cart(const char *path, uint32_t *len, char *why, int whysz)
{
    FILE *f;
    uint8_t *buf;
    size_t got;

    if (path_is_zip(path))
        return read_cart_zip(path, len, why, whysz);
    f = fopen(path, "rb");
    if (!f) {
        snprintf(why, (size_t)whysz, "cannot open %s", path);
        return NULL;
    }
    buf = malloc(CART_MAX + 1);
    if (!buf) {
        fclose(f);
        snprintf(why, (size_t)whysz, "out of memory");
        return NULL;
    }
    got = fread(buf, 1, CART_MAX + 1, f);
    fclose(f);
    if (got == 0) {
        free(buf);
        snprintf(why, (size_t)whysz, "%s is empty", path);
        return NULL;
    }
    if (got >= CART_MAX) {
        free(buf);
        snprintf(why, (size_t)whysz,
                 "the image is larger than the cartridge can serve (%u bytes)",
                 (unsigned)(CART_MAX - 1));
        return NULL;
    }
    *len = (uint32_t)got;
    return buf;
}

/* Identify an image. A FujiNet app is one the cartridge firmware's own
 * planner says claims the mailbox (an APPBANK image, or a bank layout with
 * "FUJI" at $F2C); anything else is a game, which is MAME's cartridge for its
 * size and contents (the mapper the host puts in the slot: standard or Golf).
 * 1 if the image can run, else 0 and why. */
static int analyse_image(const uint8_t *img, uint32_t len, cart_info *ci,
                         char *why, int whysz)
{
    const char *err = NULL;
    chf_cart_t tmp;

    memset(ci, 0, sizeof *ci);
    if (why && whysz > 0)
        why[0] = '\0';
    ci->plan_err = chfmap_plan(img, len, &ci->plan);
    if (ci->plan_err == CHFMAP_OK && ci->plan.mailbox_ok) {
        ci->app = 1;
        ci->type = CHF_CART_FUJINET;
        return 1;
    }
    ci->type = chf_cart_identify(img, len);
    if (chf_cart_init(&tmp, ci->type, img, len, &err) != 0) {
        if (why && whysz > 0)
            snprintf(why, (size_t)whysz, "%s", err ? err : "the cartridge cannot be mapped");
        return 0;
    }
    return 1;
}

static const char *chfmap_strerror(chfmap_err_t e)
{
    switch (e) {
    case CHFMAP_OK:       return "ok";
    case CHFMAP_EEMPTY:   return "the image is empty";
    case CHFMAP_ETOOBIG:  return "the image is larger than the biggest FujiNet app";
    case CHFMAP_ENOMAP:   return "over 8 KB, but not 8 KB plus whole 2 KB pages";
    case CHFMAP_ENOCLAIM: return "an image over 8 KB must claim the mailbox (\"FUJI\")";
    }
    return "the image cannot be mapped";
}

static int check_image(const char *path, cart_info *out, char *why, int whysz)
{
    uint8_t *img;
    uint32_t len = 0;
    char err[256];
    char abs[CHF_PATH_MAX];
    cart_info ci;
    int ok;

    if (why && whysz > 0)
        why[0] = '\0';
    memset(&ci, 0, sizeof ci);
    if (out)
        *out = ci;
    if (!path || !*path)
        return 0;
    path = paths_resolve(path, abs, sizeof abs);
    img = session_read_cart(path, &len, err, sizeof err);
    if (!img) {
        if (why && whysz > 0)
            snprintf(why, (size_t)whysz, "%s", err);
        return 0;
    }
    ok = analyse_image(img, len, &ci, why, whysz);
    free(img);
    if (out)
        *out = ci;
    return ok;
}

int chfsession_check_cart(const char *path, char *why, int whysz)
{
    return check_image(path, NULL, why, whysz);
}

int chfsession_cart_is_app(const char *path)
{
    cart_info ci;
    return check_image(path, &ci, NULL, 0) && ci.app;
}

/* Whether the FujiNet cartridge itself can serve the image (a network boot of
 * it from the SD folder): chfmap's plan accepts it (up to 8 KB flat, or an
 * APPBANK image of 8 KB plus 2 KB pages that claims the mailbox). A game the
 * slot's own mapper takes but chfmap does not still runs from the slot, so
 * check_cart allows it; the SD folder is for what CONFIG can boot. */
int session_cart_plan_check(const char *path, char *why, int whysz)
{
    cart_info ci;

    if (!check_image(path, &ci, why, whysz))
        return 0;
    if (ci.plan_err != CHFMAP_OK) {
        if (why && whysz > 0)
            snprintf(why, (size_t)whysz, "the FujiNet cartridge cannot serve it: %s",
                     chfmap_strerror(ci.plan_err));
        return 0;
    }
    return 1;
}

/* ---- powering the machine -------------------------------------------------------- */

/* Bring the machine up with s->opts and the given image (NULL: CONFIG). */
static int host_up(struct chfsession *s, const char *cart_path)
{
    chf_host_opts_t ho;
    cart_info ci;
    uint8_t *cart = NULL;
    uint32_t cart_len = 0;
    char why[256];
    int rc;

    memset(&ho, 0, sizeof ho);
    memset(&ci, 0, sizeof ci);

    if (cart_path && *cart_path) {
        cart = session_read_cart(cart_path, &cart_len, why, sizeof why);
        if (!cart) {
            session_set_error(s, "%s", why);
            return -1;
        }
        if (!analyse_image(cart, cart_len, &ci, why, sizeof why)) {
            session_set_error(s, "%s", why);
            free(cart);
            return -1;
        }
    }

    /* The cartridge dials FujiNet's BoIP port -- the in-process runtime's,
     * or a standalone fujinet-pc's when the runtime is switched off or
     * external. A session whose FujiNet is disabled outright (fujinet_lib "",
     * the tests) dials nothing, so it can never pick up someone else's FujiNet
     * (and Windows' two-second refusal of a closed port never holds up
     * power-on). */
    if (s->fujinet_disabled)
        s->boip_endpoint[0] = '\0';
    ho.machine = s->opts.machine;
    ho.cart = cart;
    ho.cart_size = cart_len;
    ho.cart_type = cart ? ci.type : CHF_CART_FUJINET;
    ho.cart_app = cart ? ci.app != 0 : true;
    ho.boip_hostport = s->boip_endpoint;
    ho.cart_sync = getenv("CHF_CART_SYNC") != NULL;

    rc = chf_host_start(&ho);
    if (rc != 0) {
        session_set_error(s, "%s", chf_host_last_error());
    } else {
        s->run_machine = s->opts.machine;
        s->run_game = cart && !ci.app;
    }
    free(cart);
    return rc;
}

/* The machine option the settings name now: a Preferences change takes
 * effect at the next power cycle, which reads it here. */
static void reload_machine_opts(struct chfsession *s)
{
    s->opts.machine = clamp_machine(chfsession_get_int(s, "machine", s->opts.machine));
}

/* A power cycle: the machine and the cartridge come up fresh; FujiNet stays
 * up. `cart_path` NULL boots CONFIG. */
static int power_cycle(struct chfsession *s, const char *cart_path)
{
    int rc;

    reload_machine_opts(s);

    chfdebug_before_power_cycle(s);
    chf_host_stop();
    rc = host_up(s, cart_path);
    if (rc != 0 && cart_path) {
        /* whatever was running is gone: put CONFIG back */
        char err[256];
        snprintf(err, sizeof err, "%s", s->last_error);
        host_up(s, NULL);
        snprintf(s->last_error, sizeof s->last_error, "%s", err);
    }
    chfdebug_after_power_cycle(s);
    return rc;
}

static void set_cart(struct chfsession *s, const char *path)
{
    if (path && *path)
        snprintf(s->cart_path, sizeof s->cart_path, "%s", path);
    else
        s->cart_path[0] = '\0';
    s->opts.cart_path = s->cart_path[0] ? s->cart_path : NULL;
    chfsession_set_str(s, "cart", s->cart_path);
}

static void adopt_opts(struct chfsession *s, const chfsession_start_opts *opts)
{
    s->opts = *opts;
    s->opts.machine = clamp_machine(opts->machine);
    if (opts->cart_path) {
        char abs[CHF_PATH_MAX];
        snprintf(s->cart_path, sizeof s->cart_path, "%s",
                 paths_resolve(opts->cart_path, abs, sizeof abs));
    } else
        s->cart_path[0] = '\0';
    s->opts.cart_path = s->cart_path[0] ? s->cart_path : NULL;
    snprintf(s->boip_addr, sizeof s->boip_addr, "%s",
             opts->boip_connect ? opts->boip_connect : "");
    s->opts.boip_connect = s->boip_addr;
}

int chfsession_start(chfsession *s, const chfsession_start_opts *opts)
{
    chfsession_start_opts local;

    if (s->running)
        return 0;
    s->last_error[0] = '\0';

    if (!opts) {
        chfsession_default_opts(s, &local);
        opts = &local;
    }
    adopt_opts(s, opts);

    memset(s->pad_src, 0, sizeof s->pad_src);
    memset(s->key_src, 0, sizeof s->key_src);
    memset(s->sw_src, 0, sizeof s->sw_src);
    memset(s->disc_x, 0, sizeof s->disc_x);
    memset(s->disc_y, 0, sizeof s->disc_y);
    chf_host_release_all();
    chfdebug_trace_fujinet(s);

    /* An external FujiNet is dialed at the address the settings name; a bad
     * address is an error, not a silent fall back to the built-in one. */
    s->fujinet_external = 0;
    snprintf(s->boip_endpoint, sizeof s->boip_endpoint, "127.0.0.1:%d",
             CHFSESSION_BOIP_PORT);
    if (s->opts.external_fujinet && !s->fujinet_disabled) {
        char host[64];
        int port;

        if (chfsession_parse_hostport(s->opts.boip_connect, host, sizeof host,
                                          &port) != 0) {
            session_set_error(s, "\"%s\" is not an address:port for the "
                              "external FujiNet (example: 127.0.0.1:%d)",
                              s->opts.boip_connect ? s->opts.boip_connect : "",
                              CHFSESSION_BOIP_PORT);
            return -1;
        }
        s->fujinet_external = 1;
        snprintf(s->boip_endpoint, sizeof s->boip_endpoint, "%s:%d", host,
                 port);
    }

    /* FujiNet FIRST: it listens and the cartridge dials in, so the listener
     * has to exist before the machine's first transaction or the CONFIG
     * client boots reporting no link. Failing to start is NOT fatal (the
     * cartridge also redials when a transaction finds the link down). An
     * external FujiNet is someone else's to start. */
    if (s->opts.enable_fujinet && !s->fujinet_disabled && !s->fujinet_external) {
        if (fujinet_start(s) == 0)
            fujinet_wait_for_boip(s, 3000);
    }

    /* A remembered cartridge that no longer loads (moved, an image the
     * console cannot take) must not leave the app unbootable: fall back to
     * CONFIG and say why. */
    if (host_up(s, s->opts.cart_path) != 0) {
        if (s->opts.cart_path) {
            fprintf(stderr, "chf: %s: %s; booting CONFIG\n", s->cart_path, s->last_error);
            set_cart(s, NULL);
            if (host_up(s, NULL) != 0) {
                fujinet_stop(s);
                return -1;
            }
        } else {
            fujinet_stop(s);
            return -1;
        }
    }

    s->running = 1;
    chfdebug_after_power_cycle(s);

    if (s->opts.enable_gamepad && gamepad_start(s) != 0) {
        fprintf(stderr, "chf: gamepads unavailable (%s)\n", s->last_error);
        s->last_error[0] = '\0';
    }
    if (s->opts.enable_audio && audio_start(s) != 0) {
        fprintf(stderr, "chf: audio unavailable (%s); continuing silent\n", s->last_error);
        s->last_error[0] = '\0';
    }
    return 0;
}

void chfsession_stop(chfsession *s)
{
    if (!s->running)
        return;
    audio_stop(s);
    gamepad_stop(s);
    chfdebug_before_power_cycle(s);
    chf_host_stop();
    fujinet_stop(s);
    s->fujinet_external = 0;
    s->running = 0;
}

int chfsession_is_running(const chfsession *s)
{
    return s->running;
}

const char *chfsession_last_error(const chfsession *s)
{
    return s->last_error;
}

int chfsession_restart(chfsession *s)
{
    chfsession_start_opts opts;

    chfsession_settings_flush(s);
    chfsession_default_opts(s, &opts);
    if (!s->running)
        return chfsession_start(s, &opts);

    /* FujiNet or the device toggles change what runs beside the machine:
     * a full restart. Anything else is a machine option: a power cycle. */
    if (!!opts.enable_fujinet != !!s->opts.enable_fujinet ||
        !!opts.external_fujinet != !!s->opts.external_fujinet ||
        strcmp(opts.boip_connect ? opts.boip_connect : "", s->boip_addr) ||
        !!opts.enable_audio != !!s->opts.enable_audio ||
        !!opts.enable_gamepad != !!s->opts.enable_gamepad) {
        chfsession_stop(s);
        return chfsession_start(s, &opts);
    }
    adopt_opts(s, &opts);
    return power_cycle(s, s->opts.cart_path);
}

/* ---- cartridges ------------------------------------------------------------ */

int chfsession_load_cart(chfsession *s, const char *path)
{
    char why[256];
    char abs[CHF_PATH_MAX];
    cart_info ci;

    if (!s->running || !path || !*path)
        return -1;
    path = paths_resolve(path, abs, sizeof abs);
    /* refuse before touching the running machine */
    if (!check_image(path, &ci, why, sizeof why)) {
        session_set_error(s, "%s", why);
        return -1;
    }
    if (power_cycle(s, path) != 0) {
        set_cart(s, NULL);
        return -1;
    }
    set_cart(s, path);
    return 0;
}

const char *chfsession_cart_path(const chfsession *s)
{
    return s->cart_path;
}

int chfsession_reset(chfsession *s)
{
    if (!s->running)
        return -1;
    chf_host_reset_pulse();
    return 0;
}

int chfsession_reset_to_config(chfsession *s)
{
    if (!s->running)
        return -1;
    set_cart(s, NULL);
    return power_cycle(s, NULL);
}

int chfsession_eject(chfsession *s)
{
    return chfsession_reset_to_config(s);
}

void chfsession_set_paused(chfsession *s, int paused)
{
    if (s->running)
        chf_host_set_paused(paused != 0);
}

int chfsession_is_paused(const chfsession *s)
{
    return s->running && chf_host_is_paused();
}

/* ---- video / audio --------------------------------------------------------- */

int chfsession_copy_frame(chfsession *s, uint32_t *dst, uint64_t *serial_inout)
{
    if (!s->running)
        return 0;
    return chf_host_frame_copy(dst, serial_inout);
}

void chfsession_set_frame_callback(chfsession *s, chfsession_frame_cb cb, void *ctx)
{
    (void)s;
    chf_host_set_frame_callback(cb, ctx);
}

uint64_t chfsession_frame_count(chfsession *s)
{
    return s->running ? chf_host_frame_count() : 0;
}

int chfsession_refresh_rate(chfsession *s)
{
    const int machine = s->running ? s->run_machine
        : clamp_machine(chfsession_get_int(s, "machine", CHF_MACHINE_ARCADIA));
    return chf_machine_is_pal(machine) ? 50 : 60;
}

void chfsession_notify_vsync(chfsession *s, int64_t frame_time_ns)
{
    if (s->running)
        chf_host_notify_vsync(frame_time_ns);
}

int chfsession_render_audio(chfsession *s, float *out, int nframes)
{
    int got = 0;

    if (nframes <= 0)
        return 0;
    if (s->running)
        got = chf_host_audio_copy(out, nframes);
    if (got < nframes)
        memset(out + 2 * got, 0, sizeof(float) * 2 * (size_t)(nframes - got));
    {
        const float gain = (float)chfsession_get_int(s, "volume", 100) / 100.0f;
        if (gain < 0.999f)
            for (int i = 0; i < 2 * got; i++)
                out[i] *= gain;
    }
    return nframes;
}

void chfsession_set_volume(chfsession *s, int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    chfsession_set_int(s, "volume", percent);
}

/* ---- input ----------------------------------------------------------------- */

/* The console's view of a disc's digital side: opposite directions cancel
 * (MAME's inputs are 8-way for the same reason). input_mtx held. */
static unsigned stick_mask_locked(const struct chfsession *s, int port)
{
    unsigned mask = 0;

    for (int a = 0; a < CHF_ACT_PER_PORT; a++)
        if (s->pad_src[port][a])
            mask |= 1u << a;
    if ((mask & (1u << CHF_ACT_UP)) && (mask & (1u << CHF_ACT_DOWN)))
        mask &= ~((1u << CHF_ACT_UP) | (1u << CHF_ACT_DOWN));
    if ((mask & (1u << CHF_ACT_LEFT)) && (mask & (1u << CHF_ACT_RIGHT)))
        mask &= ~((1u << CHF_ACT_LEFT) | (1u << CHF_ACT_RIGHT));
    return mask;
}

static void push_stick_locked(struct chfsession *s, int port)
{
    /* chf_action's order is the host's bit order: UP RIGHT DOWN LEFT FIRE */
    chf_host_stick_set(port, (uint8_t)stick_mask_locked(s, port));
}

/* The analog disc: the strongest deflection any pad on the port holds
 * (a non-zero analog value overrides the digital bits in the core). */
static void push_disc_locked(struct chfsession *s, int port)
{
    int bx = 0, by = 0, best = 0;

    for (int o = 0; o <= CHF_MAX_PADS; o++) {
        int mag = abs(s->disc_x[o][port]) + abs(s->disc_y[o][port]);
        if (mag > best) {
            best = mag;
            bx = s->disc_x[o][port];
            by = s->disc_y[o][port];
        }
    }
    chf_host_disc_set(port, bx, by);
}

static void push_matrix_locked(struct chfsession *s)
{
    uint64_t matrix = 0;

    for (int k = 0; k < CHF_KEY_COUNT; k++)
        if (s->key_src[k])
            matrix |= 1ull << k;
    chf_host_keys_set(matrix);
}

static void push_reset_locked(struct chfsession *s)
{
    chf_host_reset_set(s->sw_src[CHF_SW_RESET] != 0);
}

static void set_src(uint32_t *word, uint32_t src, int down)
{
    if (down)
        *word |= src;
    else
        *word &= ~src;
}

/* One source of one target; sysactions are not held inputs and are left to
 * the caller. */
static void source_press(struct chfsession *s, int target, uint32_t src, int down)
{
    pthread_mutex_lock(&s->input_mtx);
    if (target < 2 * CHF_ACT_PER_PORT) {
        const int port = target / CHF_ACT_PER_PORT;
        set_src(&s->pad_src[port][target % CHF_ACT_PER_PORT], src, down);
        push_stick_locked(s, port);
    } else if (target < CHF_TARGET_SWITCH(0)) {
        set_src(&s->key_src[target - CHF_TARGET_KEY(0)], src, down);
        push_matrix_locked(s);
    } else if (target < CHF_TARGET_SYSACT(0)) {
        set_src(&s->sw_src[target - CHF_TARGET_SWITCH(0)], src, down);
        push_reset_locked(s);
    }
    pthread_mutex_unlock(&s->input_mtx);
}

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
    if (port < 0 || port > 1)
        return 0;
    pthread_mutex_lock(&s->input_mtx);
    held = stick_mask_locked(s, port);
    pthread_mutex_unlock(&s->input_mtx);
    return held;
}

void chfsession_disc_get(chfsession *s, int port, int *x, int *y)
{
    int bx = 0, by = 0, best = 0;

    if (port >= 0 && port <= 1) {
        pthread_mutex_lock(&s->input_mtx);
        for (int o = 0; o <= CHF_MAX_PADS; o++) {
            int mag = abs(s->disc_x[o][port]) + abs(s->disc_y[o][port]);
            if (mag > best) {
                best = mag;
                bx = s->disc_x[o][port];
                by = s->disc_y[o][port];
            }
        }
        pthread_mutex_unlock(&s->input_mtx);
    }
    if (x) *x = bx;
    if (y) *y = by;
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

/* Everything the keyboard and the controllers window hold, let go (focus
 * loss): what a key's press asserted, whichever way it was asserted. A
 * gamepad's own holds are left to the gamepad. */
static void release_keyboard_sources(struct chfsession *s)
{
    pthread_mutex_lock(&s->input_mtx);
    for (int port = 0; port < 2; port++) {
        for (int a = 0; a < CHF_ACT_PER_PORT; a++)
            s->pad_src[port][a] &= ~SRC_KEYBOARD;
        push_stick_locked(s, port);
    }
    for (int k = 0; k < CHF_KEY_COUNT; k++)
        s->key_src[k] &= ~SRC_KEYBOARD;
    push_matrix_locked(s);
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

void session_gamepad_disc(struct chfsession *s, int owner, int port, int x, int y)
{
    if (!s->running || owner < 0 || owner >= CHF_MAX_PADS || port < 0 || port > 1)
        return;
    pthread_mutex_lock(&s->input_mtx);
    s->disc_x[owner][port] = x;
    s->disc_y[owner][port] = y;
    push_disc_locked(s, port);
    pthread_mutex_unlock(&s->input_mtx);
}

void chfsession_disc_set(chfsession *s, int port, int x, int y)
{
    if (!s->running || port < 0 || port > 1)
        return;
    if (x < -128) x = -128;
    if (x > 127) x = 127;
    if (y < -128) y = -128;
    if (y > 127) y = 127;
    pthread_mutex_lock(&s->input_mtx);
    s->disc_x[CHF_MAX_PADS][port] = x;
    s->disc_y[CHF_MAX_PADS][port] = y;
    push_disc_locked(s, port);
    pthread_mutex_unlock(&s->input_mtx);
}

void session_gamepad_stick(struct chfsession *s, int owner, int port, int act, int down)
{
    if (!s->running || owner < 0 || owner >= CHF_MAX_PADS || port < 0 || port > 1
        || act < 0 || act >= CHF_ACT_PER_PORT)
        return;
    source_press(s, CHF_TARGET_PORT(port, act), SRC_STICK(owner), down);
}

void chfsession_set_analog(chfsession *s, int joystick)
{
    s->opts.analog_joystick = joystick ? 1 : 0;
    chfsession_set_int(s, "analog_joystick", s->opts.analog_joystick);
}

/* ---- FujiNet --------------------------------------------------------------- */

int chfsession_fujinet_running(const chfsession *s)
{
    return s->fujinet_running;
}

const char *chfsession_fujinet_webui_url(const chfsession *s)
{
    return s->fujinet_external ? "" : s->webui_url;
}

int chfsession_parse_hostport(const char *text, char *host, int hostsz,
                              int *port)
{
    char buf[80], *colon, *end;
    const char *name = "127.0.0.1";
    long num = CHFSESSION_BOIP_PORT;
    size_t len, i;

    if (!text) text = "";
    while (*text == ' ' || *text == '\t') text++;
    len = strlen(text);
    while (len && (text[len - 1] == ' ' || text[len - 1] == '\t' ||
                   text[len - 1] == '\n' || text[len - 1] == '\r'))
        len--;
    if (len >= sizeof buf) return -1;
    memcpy(buf, text, len);
    buf[len] = '\0';

    if (len) {
        colon = strrchr(buf, ':');
        if (colon) {
            *colon = '\0';
            name = buf;
            if (!*name) return -1;
            text = colon + 1;
        } else {
            text = buf;
        }
        if (!*text) return -1;
        num = strtol(text, &end, 10);
        if (*end || num < 1 || num > 65535) return -1;
        if (colon) {
            /* A host name or an IPv4 literal: letters, digits, '-', '_' and
             * '.'; a name of only digits and dots must be a whole address. */
            int digits_only = 1, octet[4], used = 0, k;
            for (i = 0; name[i]; i++) {
                char ch = name[i];
                int alnum = (ch >= '0' && ch <= '9') ||
                            (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
                if (!alnum && ch != '-' && ch != '_' && ch != '.') return -1;
                if (!(ch >= '0' && ch <= '9') && ch != '.') digits_only = 0;
            }
            if (name[0] == '.' || name[i - 1] == '.' || strstr(name, "..")) return -1;
            if (digits_only) {
                if (sscanf(name, "%d.%d.%d.%d%n", &octet[0], &octet[1],
                           &octet[2], &octet[3], &used) != 4 || name[used])
                    return -1;
                for (k = 0; k < 4; k++)
                    if (octet[k] < 0 || octet[k] > 255) return -1;
            }
        }
    }
    if ((int)strlen(name) >= hostsz) return -1;
    memcpy(host, name, strlen(name) + 1);
    *port = (int)num;
    return 0;
}

int chfsession_fujinet_external(const chfsession *s)
{
    return s->fujinet_external;
}

const char *chfsession_boip_endpoint(const chfsession *s)
{
    return s->boip_endpoint[0] ? s->boip_endpoint : "127.0.0.1:" "11526";
}

int chfsession_cart_link_up(chfsession *s)
{
    chf_cart_status_t st;
    if (!s->running)
        return -1;
    chf_host_cart_status(&st);
    return st.link_up ? 1 : 0;
}

int chfsession_cart_status(chfsession *s, char *dst, int dstsz)
{
    chf_cart_status_t st;

    if (!dst || dstsz <= 0)
        return 0;
    dst[0] = '\0';
    if (!s->running)
        return snprintf(dst, (size_t)dstsz, "stopped");
    /* a game's own cartridge has no mailbox and no FujiNet behind it */
    if (s->run_game)
        return snprintf(dst, (size_t)dstsz, "game running; mailbox closed");
    chf_host_cart_status(&st);
    if (!st.powered)
        return snprintf(dst, (size_t)dstsz, "starting");
    if (st.boot_state == FN_BOOT_XFER)
        return snprintf(dst, (size_t)dstsz, "loading %d%%", st.boot_pct);
    if (!st.mailbox)
        return snprintf(dst, (size_t)dstsz, "game running; mailbox closed");
    if (st.link_up)
        return snprintf(dst, (size_t)dstsz,
                        (st.status & FN_R_STATUS_BUSY) ? "connected (busy)" : "connected");
    return snprintf(dst, (size_t)dstsz, "link down");
}

int chfsession_cart_booted_game(chfsession *s)
{
    chf_cart_status_t st;
    if (!s->running)
        return 0;
    if (s->run_game)
        return 1;
    chf_host_cart_status(&st);
    return st.booted_game ? 1 : 0;
}

int chfsession_cart_type_name(chfsession *s, char *dst, int dstsz)
{
    chf_cart_status_t st;
    chf_cart_t *c;
    int type = CHF_CART_NONE;
    uint32_t size = 0;
    char what[96];

    if (!dst || dstsz <= 0)
        return 0;
    dst[0] = '\0';
    if (!s->running)
        return 0;

    chf_host_cart_status(&st);
    chf_host_lock();
    c = chf_host_cart();
    if (c) {
        type = c->type;
        size = c->rom_size;
    }
    chf_host_unlock();

    if (s->run_game || (type != CHF_CART_FUJINET && type != CHF_CART_NONE)) {
        if (size >= 1024)
            snprintf(what, sizeof what, "%s, %u KB", chf_cart_type_name(type),
                     (unsigned)((size + 1023) / 1024));
        else
            snprintf(what, sizeof what, "%s", chf_cart_type_name(type));
    } else if (!st.booted_game) {
        snprintf(what, sizeof what, "FujiNet CONFIG");
    } else if (st.mailbox) {
        snprintf(what, sizeof what, "FujiNet app");
    } else {
        snprintf(what, sizeof what, "FujiNet cartridge, game");
    }
    return snprintf(dst, (size_t)dstsz, "%s", what);
}

/* ---- paths ----------------------------------------------------------------- */

const char *chfsession_config_path(const chfsession *s) { return s->config_dir; }
const char *chfsession_data_path(const chfsession *s) { return s->data_dir; }
const char *chfsession_carts_path(const chfsession *s) { return s->carts_dir; }
const char *chfsession_sd_path(const chfsession *s) { return s->fujinet_sd; }

/* ---- debugger -------------------------------------------------------------- */

chfdebug *chfsession_debugger(chfsession *s)
{
    return chfdebug_get(s);
}
