/* fujinet_cart.c -- the FujiNet Channel F cartridge. See fujinet_cart.h.
 *
 * Transposed from the firmware's MAME device (fujinet-firmware
 * pico/channelf/emu/fujinet.cpp, BSD-3-Clause, copyright-holders Thomas
 * Cherryhomes) onto the staged cartridge sources. The names follow it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fujinet_cart.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* the cartridge firmware's own sources (core/chf/fuji-generated) */
#include "fuji_mailbox.h"
#include "fujibus.h"
#include "fujimail.h"
#include "fujitcp.h"
#include "chfmap.h"
#include "arcapp.h"
#include "fujiconfigrom.h"      /* _configrom, FUJI_CONFIGROM_SIZE */

#include "fujitcp_host.h"

#define QUEUE_SIZE 1024         /* console hotspot reads waiting for the worker */
#define MAX_POKES  4096

struct chf_fujinet {
    chf_cart_t base;            /* the slot interface; first */

    /* Two slots, as on the cartridge: slot[live] is the one served, the push
     * lands in the other. Each holds a whole image, flat (8K) or APPBANK. */
    uint8_t *slot[2];
    uint8_t npages[2];
    unsigned live;
    arcapp_t app;
    uint8_t ffpage[FN_APP_PAGE];
    uint8_t *rx[2];             /* per-stream push buffers */
    uint32_t rx_len[2], rx_cap[2];

    atomic_bool mailbox_live;   /* the running image owns the mailbox pages */
    bool powered;

    /* the image served first (the baked CONFIG or the opened app) */
    uint32_t live_size, live_crc;

    /* A staged image, built by whichever thread runs the transaction and taken
     * by the console's thread. loadlock covers it and the swap. */
    pthread_mutex_t loadlock;
    bool have_staged, staged_claims, staged_app;
    atomic_bool swap_armed;
    uint32_t staged_size, staged_crc;
    unsigned swaps;
    bool served_other;          /* anything but the baked CONFIG has run */

    /* The worker owns fujimail and the socket; the console's thread owns the
     * slots, the swap and the app state. The worker's pokes reach the slots
     * through pokes[], applied before the next fetch; the console's hotspot
     * reads reach the worker through the hq[] queue. */
    bool async;
    bool worker_running;
    pthread_t worker;
    pthread_mutex_t qlock;
    pthread_cond_t qcv;
    uint16_t hq[QUEUE_SIZE];
    unsigned qhead, qtail;
    atomic_bool stop;
    atomic_bool busy;           /* the worker is inside fujimail_read_hotspot() */
    pthread_mutex_t pklock;
    uint16_t pk_addr[MAX_POKES];
    uint8_t pk_data[MAX_POKES];
    unsigned npokes;
    atomic_bool poked;

    /* the 60 Hz tick's clock */
    const uint64_t *cycles;
    double cycle_hz;

    /* the link */
    char boip[96];
    bool have_boip, link_off;
    bool debug;
    unsigned pace_ms;
};

/* fujimail's port is C function pointers with no context argument; one slot,
 * one cart, as on the hardware. */
static chf_fujinet_t *s_cart = NULL;

/* set on the worker thread: its pokes queue for the console's thread */
static _Thread_local bool t_on_worker = false;

static void sleep_ms(unsigned ms)
{
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void sleep_us(unsigned us)
{
    struct timespec ts = { 0, (long)us * 1000L };
    nanosleep(&ts, NULL);
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void apply_poke(chf_fujinet_t *c, unsigned offset, uint8_t value);

/*-------------------------------------------------
    C port callbacks
-------------------------------------------------*/

static void poke(chf_fujinet_t *c, unsigned offset, uint8_t value)
{
    if (!t_on_worker) {
        apply_poke(c, offset, value);
        return;
    }
    pthread_mutex_lock(&c->pklock);
    if (c->npokes < MAX_POKES) {
        c->pk_addr[c->npokes] = (uint16_t)offset;
        c->pk_data[c->npokes] = value;
        c->npokes++;
    }
    atomic_store_explicit(&c->poked, true, memory_order_release);
    pthread_mutex_unlock(&c->pklock);
}

static void c_poke(unsigned offset, uint8_t value) { poke(s_cart, offset, value); }
static bool c_link_up(void) { return fujitcp_active(); }

/* Dial again if the link has dropped: MAME's device connects once and never
 * again; a FujiNet that restarts or a transaction that was cut short should
 * not leave the console without one. */
static fb_status_t c_transact(uint8_t device, uint8_t command,
                              const fb_param_t *params, unsigned nparams,
                              const uint8_t *payload, uint16_t payload_len,
                              uint32_t timeout_ms, fb_reply_t *reply)
{
    chf_fujinet_t *c = s_cart;
    fb_status_t st;

    if (!c->link_off && !fujitcp_active())
        fujitcp_init(c->have_boip ? c->boip : NULL);
    st = fujitcp_transact(device, command, params, nparams, payload, payload_len,
                          timeout_ms, reply);
    if (st == FB_ENOLINK)
        fujitcp_close();        /* the next transaction reconnects */
    return st;
}

static void c_send_bare(uint8_t device, uint8_t command, const uint8_t *payload, uint16_t len)
{
    chf_fujinet_t *c = s_cart;

    if (c->pace_ms && device == FUJI_DEVICEID_DBC && command == CMD_FUJI_ACK)
        sleep_ms(c->pace_ms);
    fujitcp_send_bare(device, command, payload, len);
}

/* Try to connect for up to `ms`: the listener can be a moment behind the
 * console on a first start (the session waits for it, but not for ever). */
static void c_wait_link(uint32_t ms)
{
    chf_fujinet_t *c = s_cart;
    const uint64_t deadline = now_ms() + ms;

    while (!c->link_off && !fujitcp_active() && !atomic_load(&c->stop)) {
        if (fujitcp_init(c->have_boip ? c->boip : NULL) == 0)
            break;
        if (now_ms() >= deadline)
            break;
        sleep_ms(100);
    }
    poke(c, FN_R_STATUS, fujitcp_active() ? FN_R_STATUS_LINK : 0);
}

static void c_on_txn(const fujimail_txn_t *t)
{
    char txt[40];
    unsigned k, m = 0;

    for (k = 0; k < t->rxlen && m < sizeof txt - 1; k++) {
        uint8_t ch = t->rx[k];

        if (ch == 0)
            break;
        txt[m++] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '.';
    }
    txt[m] = '\0';
    fprintf(stderr,
            "fujinet: dev=%02X cmd=%02X nparam=%u txlen=%u seq=%u"
            " -> err=%d reply=%02X rxlen=%u%s%s%s\n",
            t->device, t->command, t->nparam, t->txlen, t->seq,
            t->status, t->reply_cmd, t->rxlen,
            m ? " \"" : "", txt, m ? "\"" : "");
}

static void c_on_dbc(fujimail_dbc_ev_t ev, int stream, uint32_t expect, unsigned got, bool aborted)
{
    if (ev == FUJIMAIL_DBC_OPEN)
        fprintf(stderr, "fujinet: DBC open stream=%d size=%u\n", stream, expect);
    else
        fprintf(stderr, "fujinet: DBC close stream=%d got=%u%s\n",
                stream, got, aborted ? " ABORTED" : "");
}

static uint8_t c_stream_open(int stream, uint32_t size);
static void c_stream_write(int stream, const uint8_t *chunk, unsigned len);
static uint8_t c_stream_close(int stream, uint32_t got, bool aborted);
static void c_arm_swap(void);

static const fujimail_port_t debug_port = {
    .poke = c_poke, .link_up = c_link_up, .transact = c_transact,
    .send_bare = c_send_bare, .stream_open = c_stream_open,
    .stream_write = c_stream_write, .stream_close = c_stream_close,
    .arm_swap = c_arm_swap, .wait_link_ms = c_wait_link,
    .on_txn = c_on_txn, .on_dbc = c_on_dbc,
};

static const fujimail_port_t quiet_port = {
    .poke = c_poke, .link_up = c_link_up, .transact = c_transact,
    .send_bare = c_send_bare, .stream_open = c_stream_open,
    .stream_write = c_stream_write, .stream_close = c_stream_close,
    .arm_swap = c_arm_swap, .wait_link_ms = c_wait_link,
};

/*-------------------------------------------------
    the slots
-------------------------------------------------*/

/* fuji_cart_poke(): the running image's slot, and a staged one that claims the
 * mailbox, so its client never sees a stale status page. The tick is the
 * timer's. A game that does not claim keeps its bytes pristine. */
static void apply_poke(chf_fujinet_t *c, unsigned offset, uint8_t value)
{
    if (!atomic_load(&c->mailbox_live))
        return;
    if (offset == FN_R_TICK_LO || offset == FN_R_TICK_HI)
        return;
    c->slot[c->live][offset & 0x1fff] = value;
    if (c->have_staged && c->staged_claims)
        c->slot[c->live ^ 1][offset & 0x1fff] = value;
}

static void drain_pokes(chf_fujinet_t *c)
{
    uint16_t addr[MAX_POKES];
    uint8_t data[MAX_POKES];
    unsigned n;

    if (!atomic_load_explicit(&c->poked, memory_order_acquire))
        return;
    pthread_mutex_lock(&c->pklock);
    n = c->npokes;
    memcpy(addr, c->pk_addr, n * sizeof addr[0]);
    memcpy(data, c->pk_data, n);
    c->npokes = 0;
    atomic_store_explicit(&c->poked, false, memory_order_relaxed);
    pthread_mutex_unlock(&c->pklock);
    pthread_mutex_lock(&c->loadlock);
    for (unsigned i = 0; i < n; i++)
        apply_poke(c, addr[i], data[i]);
    pthread_mutex_unlock(&c->loadlock);
}

/* do_swap(): no repaint -- the status page was carried into the staged slot
 * when it was staged, exactly as the cartridge does it, so the protocol state
 * (ACKSEQ against fujimail's last sequence) stays one piece. */
static void do_swap(chf_fujinet_t *c)
{
    pthread_mutex_lock(&c->loadlock);
    if (!c->have_staged || !atomic_load(&c->swap_armed)) {
        pthread_mutex_unlock(&c->loadlock);
        return;
    }
    c->live ^= 1;
    arcapp_reset(&c->app, c->slot[c->live], c->npages[c->live], c->ffpage);
    atomic_store(&c->swap_armed, false);
    c->have_staged = false;
    atomic_store(&c->mailbox_live, c->staged_claims);
    c->swaps++;
    c->served_other = true;
    c->live_size = c->staged_size;
    c->live_crc = c->staged_crc;
    c->staged_app = false;
    pthread_mutex_unlock(&c->loadlock);
    fprintf(stderr, "fujinet: swapped in the staged image; mailbox %s for this session\n",
            c->staged_claims ? "kept" : "disabled");
}

/*-------------------------------------------------
    the push receiver (fujinet.cpp stream_*)
-------------------------------------------------*/

static uint8_t c_stream_open(int stream, uint32_t size)
{
    chf_fujinet_t *c = s_cart;
    uint8_t err = (stream == 0) ? chfmap_gate(size) : 0;

    if (err != 0)
        return err;
    c->rx_len[stream & 1] = 0;
    if (size && size > c->rx_cap[stream & 1]) {
        uint8_t *p = realloc(c->rx[stream & 1], size);

        if (!p)
            return FN_BOOT_ERR_TOOBIG;
        c->rx[stream & 1] = p;
        c->rx_cap[stream & 1] = size;
    }
    return 0;
}

static void c_stream_write(int stream, const uint8_t *chunk, unsigned len)
{
    chf_fujinet_t *c = s_cart;
    const unsigned s = (unsigned)stream & 1;

    if (c->rx_len[s] + len > c->rx_cap[s]) {
        uint32_t cap = c->rx_cap[s] ? c->rx_cap[s] : 4096;
        uint8_t *p;

        while (cap < c->rx_len[s] + len)
            cap *= 2;
        p = realloc(c->rx[s], cap);
        if (!p)
            return;
        c->rx[s] = p;
        c->rx_cap[s] = cap;
    }
    memcpy(c->rx[s] + c->rx_len[s], chunk, len);
    c->rx_len[s] += len;
}

static uint8_t c_stream_close(int stream, uint32_t got, bool aborted)
{
    chf_fujinet_t *c = s_cart;
    const unsigned s = (unsigned)stream & 1;
    chfmap_plan_t plan;
    uint8_t *staged;

    (void)got;          /* rx_len is the byte count that actually landed */
    if (aborted || stream != 0) {
        c->rx_len[s] = 0;       /* the .cfg sibling means nothing to this cartridge */
        return 0;
    }
    switch (chfmap_plan(c->rx[s], c->rx_len[s], &plan)) {
    case CHFMAP_OK:
        break;
    case CHFMAP_ETOOBIG:
        c->rx_len[s] = 0;
        return FN_BOOT_ERR_TOOBIG;
    case CHFMAP_ENOMAP:
    case CHFMAP_ENOCLAIM:
        c->rx_len[s] = 0;
        return FN_BOOT_ERR_NOMAP;
    default:
        c->rx_len[s] = 0;
        return FN_BOOT_ERR_TRUNCATED;
    }
    pthread_mutex_lock(&c->loadlock);
    staged = c->slot[c->live ^ 1];
    memset(staged, 0xff, CHFMAP_STORE);
    chfmap_apply(c->rx[s], &plan, staged);
    if (plan.mailbox_ok)
        chfmap_carry_status(staged, c->slot[c->live]);
    c->npages[c->live ^ 1] = plan.npages;
    c->staged_claims = plan.mailbox_ok;
    c->staged_app = plan.npages != 0;
    c->staged_size = c->rx_len[s];
    c->staged_crc = chf_crc32(c->rx[s], c->rx_len[s]);
    c->have_staged = true;
    pthread_mutex_unlock(&c->loadlock);
    c->rx_len[s] = 0;
    fprintf(stderr, "fujinet: staged %u bytes (%s, mailbox %s)\n", (unsigned)c->staged_size,
            plan.npages ? "APPBANK" : "flat", plan.mailbox_ok ? "claimed" : "not claimed");
    return 0;
}

static void c_arm_swap(void)
{
    chf_fujinet_t *c = s_cart;

    if (c->have_staged)
        atomic_store(&c->swap_armed, true);
}

/*-------------------------------------------------
    the slot interface
-------------------------------------------------*/

static uint16_t tick_now(const chf_fujinet_t *c)
{
    if (!c->cycles || c->cycle_hz <= 0.0)
        return 0;
    return (uint16_t)(uint64_t)((double)*c->cycles * 60.0 / c->cycle_hz);
}

static void queue_hotspot(chf_fujinet_t *c, unsigned img)
{
    if (!c->async) {
        fujimail_read_hotspot((uint16_t)img);
        return;
    }
    pthread_mutex_lock(&c->qlock);
    {
        unsigned next = (c->qhead + 1) % QUEUE_SIZE;

        if (next != c->qtail) {
            c->hq[c->qhead] = (uint16_t)img;
            c->qhead = next;
        }
    }
    pthread_cond_signal(&c->qcv);
    pthread_mutex_unlock(&c->qlock);
}

/* Wait (at most `limit_ms`) until the worker has handled every hotspot read
 * queued so far. */
static void wait_worker_idle(chf_fujinet_t *c, unsigned limit_ms)
{
    const uint64_t deadline = now_ms() + limit_ms;

    for (;;) {
        bool empty;

        pthread_mutex_lock(&c->qlock);
        empty = c->qhead == c->qtail;
        pthread_mutex_unlock(&c->qlock);
        if ((empty && !atomic_load(&c->busy)) || atomic_load(&c->stop) || now_ms() >= deadline)
            return;
        sleep_us(20);
    }
}

/* One cartridge access, in connector terms: a14 = console address & $3FFF.
 * Both read_rom and extra_rom funnel here so the alias behaviour ($4000,
 * $6000) and the open-bus holes ($3000, $5000, $7000) fall out of
 * chfmap_decode rather than being special-cased. */
static uint8_t serve(chf_fujinet_t *c, unsigned a14, int peek)
{
    int img = chfmap_decode(a14);
    uint8_t data;

    if (img < 0)
        return 0xff;                /* A12 high: chip select off, open bus */
    if (c->async)
        drain_pokes(c);

    /* the tick: the cartridge's timer repaints it; here it is emulated time */
    if (atomic_load_explicit(&c->mailbox_live, memory_order_relaxed) &&
        (img == FN_R_TICK_LO || img == FN_R_TICK_HI)) {
        const uint16_t tick = tick_now(c);

        return img == FN_R_TICK_LO ? (uint8_t)tick : (uint8_t)(tick >> 8);
    }

    data = arcapp_byte(&c->app, (unsigned)img);

    /* hotspot side effects: never for the debugger, never once the mailbox is
     * dead. The swap, bank selects and cart RAM writes are handled here
     * rather than in fujimail because they must happen inline in whatever
     * serves the bus (core1 does the same). */
    if (!peek && img >= FN_H_REGSEL) {
        if (img == FN_H_REGSEL + FN_HOT_SWAP) {
            /* The client wrote BOOTLOCK a few cycles ago; on the cartridge core0
             * has long since seen it. Here the worker may not have yet, so give
             * it the moment it needs rather than let the swap be missed. */
            if (!atomic_load(&c->swap_armed) && c->async)
                wait_worker_idle(c, 50);
            if (atomic_load(&c->swap_armed))
                do_swap(c);
        } else if (atomic_load(&c->mailbox_live) && !arcapp_hotspot(&c->app, (unsigned)img)) {
            queue_hotspot(c, (unsigned)img);
        }
    }
    return data;
}

static uint8_t fj_read_rom(chf_cart_t *base, uint16_t offset, int peek)
{
    return serve((chf_fujinet_t *)base->priv, offset & 0x0fff, peek);   /* $0000-$0FFF: block 1 */
}

static uint8_t fj_extra_rom(chf_cart_t *base, uint16_t offset, int peek)
{
    /* installed over console $2000-$7FFF; the offset is relative to $2000, so
     * rebuild the console address and let the decode sort it out */
    return serve((chf_fujinet_t *)base->priv, (offset + 0x2000) & 0x3fff, peek);
}

static const chf_cart_ops_t fujinet_ops = { fj_read_rom, fj_extra_rom };

/*-------------------------------------------------
    the worker
-------------------------------------------------*/

static void *worker_main(void *arg)
{
    chf_fujinet_t *c = arg;

    t_on_worker = true;
    for (;;) {
        uint16_t img;

        pthread_mutex_lock(&c->qlock);
        while (c->qhead == c->qtail && !atomic_load(&c->stop))
            pthread_cond_wait(&c->qcv, &c->qlock);
        if (atomic_load(&c->stop)) {
            pthread_mutex_unlock(&c->qlock);
            break;
        }
        img = c->hq[c->qtail];
        c->qtail = (c->qtail + 1) % QUEUE_SIZE;
        atomic_store(&c->busy, true);
        pthread_mutex_unlock(&c->qlock);

        /* reads queued before a swap are the old image's */
        if (atomic_load(&c->mailbox_live))
            fujimail_read_hotspot(img);
        atomic_store(&c->busy, false);
    }
    return NULL;
}

static void worker_start(chf_fujinet_t *c)
{
    atomic_store(&c->stop, false);
    c->qhead = c->qtail = 0;
    c->worker_running = false;
    if (!c->async)
        return;
    if (pthread_create(&c->worker, NULL, worker_main, c) == 0) {
        c->worker_running = true;
    } else {
        fprintf(stderr, "fujinet: no mailbox worker; servicing inline\n");
        c->async = false;
    }
}

/* A transaction in flight is cut short, so a power-off or exit never waits out
 * its timeout. */
static void worker_stop(chf_fujinet_t *c)
{
    if (!c->worker_running)
        return;
    pthread_mutex_lock(&c->qlock);
    atomic_store(&c->stop, true);
    pthread_cond_broadcast(&c->qcv);
    pthread_mutex_unlock(&c->qlock);
    fujitcp_abort();
    pthread_join(c->worker, NULL);
    c->worker_running = false;
    c->qhead = c->qtail = 0;
    pthread_mutex_lock(&c->pklock);
    c->npokes = 0;
    atomic_store(&c->poked, false);
    pthread_mutex_unlock(&c->pklock);
}

/*-------------------------------------------------
    create / power
-------------------------------------------------*/

chf_fujinet_t *chf_fujinet_create(void)
{
    chf_fujinet_t *c;

    if (s_cart)
        return NULL;
    c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->slot[0] = malloc(CHFMAP_STORE);
    c->slot[1] = malloc(CHFMAP_STORE);
    if (!c->slot[0] || !c->slot[1]) {
        free(c->slot[0]);
        free(c->slot[1]);
        free(c);
        return NULL;
    }
    memset(c->slot[0], 0xff, CHFMAP_STORE);
    memset(c->slot[1], 0xff, CHFMAP_STORE);
    memset(c->ffpage, 0xff, sizeof c->ffpage);
    c->base.ops = &fujinet_ops;
    c->base.type = CHF_CART_FUJINET;
    c->base.extra_lo = 0x2000;
    c->base.extra_hi = 0x7fff;
    c->base.priv = c;
    atomic_init(&c->mailbox_live, true);
    atomic_init(&c->swap_armed, false);
    atomic_init(&c->stop, false);
    atomic_init(&c->poked, false);
    atomic_init(&c->busy, false);
    pthread_mutex_init(&c->loadlock, NULL);
    pthread_mutex_init(&c->qlock, NULL);
    pthread_cond_init(&c->qcv, NULL);
    pthread_mutex_init(&c->pklock, NULL);
    arcapp_reset(&c->app, c->slot[0], 0, c->ffpage);
    s_cart = c;
    return c;
}

void chf_fujinet_destroy(chf_fujinet_t *c)
{
    if (!c)
        return;
    chf_fujinet_power_off(c);
    if (s_cart == c)
        s_cart = NULL;
    free(c->slot[0]);
    free(c->slot[1]);
    free(c->rx[0]);
    free(c->rx[1]);
    pthread_mutex_destroy(&c->loadlock);
    pthread_mutex_destroy(&c->qlock);
    pthread_cond_destroy(&c->qcv);
    pthread_mutex_destroy(&c->pklock);
    free(c);
}

chf_cart_t *chf_fujinet_cart(chf_fujinet_t *c)
{
    return c ? &c->base : NULL;
}

void chf_fujinet_set_clock(chf_fujinet_t *c, const uint64_t *cycles, double cycle_hz)
{
    if (!c)
        return;
    c->cycles = cycles;
    c->cycle_hz = cycle_hz;
}

static void set_why(char *why, int whysz, const char *text)
{
    if (why && whysz > 0)
        snprintf(why, (size_t)whysz, "%s", text);
}

static const char *plan_error(chfmap_err_t err)
{
    switch (err) {
    case CHFMAP_EEMPTY:    return "the cartridge image is empty";
    case CHFMAP_ETOOBIG:   return "the image is larger than the cartridge can hold (70 KB)";
    case CHFMAP_ENOMAP:    return "the image is over 8 KB but not 8 KB + k x 2 KB";
    case CHFMAP_ENOCLAIM:  return "an image over 8 KB must carry the FujiNet claim";
    default:               return "the image is not a cartridge this FujiNet cart can map";
    }
}

int chf_fujinet_check_image(const uint8_t *image, uint32_t len, bool *claims,
                            char *why, int whysz)
{
    chfmap_plan_t plan;
    chfmap_err_t err;

    if (claims)
        *claims = false;
    if (!image || !len) {
        set_why(why, whysz, "the cartridge image is empty");
        return 0;
    }
    err = chfmap_plan(image, len, &plan);
    if (err != CHFMAP_OK) {
        set_why(why, whysz, plan_error(err));
        return 0;
    }
    if (claims)
        *claims = plan.mailbox_ok;
    return 1;
}

int chf_fujinet_power_on(chf_fujinet_t *c, const uint8_t *image, uint32_t len,
                         const char *boip, bool sync, char *why, int whysz)
{
    chfmap_plan_t plan;
    chfmap_err_t err;
    const char *env;

    if (!c)
        return -1;
    chf_fujinet_power_off(c);

    c->debug = getenv("FUJINET_DEBUG") != NULL;
    env = getenv("FUJINET_PACE_MS");
    c->pace_ms = env ? (unsigned)atoi(env) : 0;
    memset(c->slot[0], 0xff, CHFMAP_STORE);
    memset(c->slot[1], 0xff, CHFMAP_STORE);
    c->npages[0] = c->npages[1] = 0;
    c->live = 0;
    c->have_staged = c->staged_claims = c->staged_app = false;
    atomic_store(&c->swap_armed, false);
    c->swaps = 0;
    c->served_other = false;
    c->staged_size = c->staged_crc = 0;
    c->rx_len[0] = c->rx_len[1] = 0;
    c->npokes = 0;
    atomic_store(&c->poked, false);

    if (!image) {
        image = _configrom;
        len = FUJI_CONFIGROM_SIZE;
    } else {
        c->served_other = true;
    }
    c->live_size = len;
    c->live_crc = chf_crc32(image, len);

    err = chfmap_plan(image, len, &plan);
    if (err != CHFMAP_OK) {
        set_why(why, whysz, plan_error(err));
        return -1;
    }
    chfmap_apply(image, &plan, c->slot[0]);
    atomic_store(&c->mailbox_live, plan.mailbox_ok);
    c->npages[0] = plan.npages;
    arcapp_reset(&c->app, c->slot[0], plan.npages, c->ffpage);
    if (plan.npages)
        fprintf(stderr, "fujinet: APPBANK image, %u pages; mailbox kept\n", plan.npages);
    else if (!plan.mailbox_ok)
        fprintf(stderr, "fujinet: image carries no claim signature; running with the mailbox dead\n");

    /* the link: "" = FujiNet switched off */
    c->have_boip = boip && boip[0];
    c->link_off = !c->have_boip;
    snprintf(c->boip, sizeof c->boip, "%s", c->have_boip ? boip : "");
    c->async = !sync;
    {
        const char *async = getenv("FUJINET_ASYNC");

        if (async)
            c->async = atoi(async) != 0;
    }
    fujitcp_close();                    /* fujinet-pc's BoIP port takes one client */
    if (plan.mailbox_ok) {
        fujimail_init(c->debug ? &debug_port : &quiet_port);
        if (!c->link_off)
            fujitcp_init(c->boip);      /* a miss is retried by the transaction */
        fujimail_paint();
    }
    c->powered = true;
    if (plan.mailbox_ok)
        worker_start(c);
    return 0;
}

void chf_fujinet_power_off(chf_fujinet_t *c)
{
    if (!c || !c->powered)
        return;
    worker_stop(c);
    fujitcp_close();
    c->powered = false;
}

void chf_fujinet_frame(chf_fujinet_t *c)
{
    if (c && c->async)
        drain_pokes(c);
}

/*-------------------------------------------------
    status
-------------------------------------------------*/

void chf_fujinet_status(chf_fujinet_t *c, chf_cart_status_t *out)
{
    const uint8_t *mb;

    memset(out, 0, sizeof *out);
    if (!c)
        return;
    out->powered = c->powered;
    out->mailbox = atomic_load(&c->mailbox_live);
    out->link_up = fujitcp_active();
    out->link_off = c->link_off;
    out->app_live = c->npages[c->live] != 0;
    out->booted_game = c->served_other;
    out->armed = atomic_load(&c->swap_armed);
    out->staged_app = c->have_staged && c->staged_app;
    out->staged_mailbox = c->have_staged && c->staged_claims;
    out->live_size = c->live_size;
    out->live_crc = c->live_crc;
    out->staged_size = c->staged_size;
    out->staged_crc = c->staged_crc;
    out->swaps = c->swaps;
    out->nbanks = c->npages[c->live];
    if (c->npages[c->live])
        out->bank = c->app.win == c->app.base
                        ? 0
                        : (unsigned)(1 + (c->app.win + FN_APP_WIN - (c->app.base + CHFMAP_WINDOW)) / FN_APP_PAGE);
    mb = c->slot[c->live];
    out->ackseq = mb[FN_R_ACKSEQ];
    out->status = mb[FN_R_STATUS];
    out->err = mb[FN_R_ERR];
    out->reply_cmd = mb[FN_R_REPLY_CMD];
    out->rxlen = (uint16_t)(mb[FN_R_RXLEN_LO] | mb[FN_R_RXLEN_HI] << 8);
    out->boot_state = mb[FN_R_BOOT_STATE];
    out->boot_pct = mb[FN_R_BOOT_PCT];
    out->boot_err = mb[FN_R_BOOT_ERR];
    pthread_mutex_lock(&c->qlock);
    out->queue = (c->qhead + QUEUE_SIZE - c->qtail) % QUEUE_SIZE;
    pthread_mutex_unlock(&c->qlock);
}

void chf_fujinet_mailbox(chf_fujinet_t *c, uint8_t out[CHF_FJ_MAILBOX_BYTES])
{
    if (!c) {
        memset(out, 0xff, CHF_FJ_MAILBOX_BYTES);
        return;
    }
    memcpy(out, &c->slot[c->live][FN_R_DATA], CHF_FJ_MAILBOX_BYTES);
    if (atomic_load(&c->mailbox_live)) {
        const uint16_t tick = tick_now(c);

        out[FN_R_TICK_LO - FN_R_DATA] = (uint8_t)tick;
        out[FN_R_TICK_HI - FN_R_DATA] = (uint8_t)(tick >> 8);
    }
}

uint8_t chf_fujinet_image_byte(chf_fujinet_t *c, uint32_t offset)
{
    if (!c || offset >= CHFMAP_STORE)
        return 0xff;
    if (offset == FN_R_TICK_LO || offset == FN_R_TICK_HI) {
        if (atomic_load(&c->mailbox_live))
            return offset == FN_R_TICK_LO ? (uint8_t)tick_now(c) : (uint8_t)(tick_now(c) >> 8);
    }
    return c->slot[c->live][offset];
}

void chf_fujinet_image_poke(chf_fujinet_t *c, uint32_t offset, uint8_t data)
{
    if (c && offset < CHFMAP_STORE)
        c->slot[c->live][offset] = data;
}

uint32_t chf_fujinet_copy_image(chf_fujinet_t *c, uint8_t *dst, uint32_t max)
{
    uint32_t n;

    if (!c || !dst)
        return 0;
    n = c->npages[c->live] ? (uint32_t)CHFMAP_WINDOW + (uint32_t)(c->npages[c->live] - 1) * CHFMAP_PAGE
                           : (uint32_t)CHFMAP_WINDOW;
    if (n > max)
        n = max;
    memcpy(dst, c->slot[c->live], n);
    return n;
}
