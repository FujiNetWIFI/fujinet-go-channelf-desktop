/* fujinet_cart.c -- the FujiNet Channel F cartridge. See fujinet_cart.h.
 *
 * Transposed from the firmware's MAME device (fujinet-firmware
 * pico/channelf/emu/fujinet.cpp, BSD-3-Clause, copyright-holders Thomas
 * Cherryhomes) onto the staged cartridge sources: the bus decode
 * (channelf_cart.h's chf_read / chf_write), the image planner (chfmap.c), the
 * mailbox service (fujimail.c) and the wire codec (fujibus.c). The names follow
 * the device's.
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
#include "channelf_cart.h"      /* chf_mem_t, chf_read, chf_write, CHF_EV_* */
#include "fuji_mailbox.h"
#include "fujibus.h"
#include "fujimail.h"
#include "fujitcp.h"
#include "chfmap.h"
#include "fujiconfigrom.h"      /* _configrom, FUJI_CONFIGROM_SIZE */

#include "fujitcp_host.h"

#define QUEUE_SIZE 1024         /* console mailbox events waiting for the worker */
#define MAX_POKES  4096
#define EV_RING    1024         /* power of two */
#define HQ_PAINT   0xffff       /* in the event queue: run fujimail_paint() */

struct chf_fujinet {
    chf_cart_t base;            /* the slot interface; first */

    /* Two slots, as on the cartridge: slot[live] is served, a push lands in
     * the other. Each holds a whole image: the 16K window, then APPBANK pages. */
    uint8_t *slot[2];
    uint8_t npages[2];
    unsigned live;
    uint8_t *arena;             /* 32K at $8000: RAM and the painted mailbox */
    uint8_t *rx[2];             /* per-stream push buffers */
    uint32_t rx_len[2], rx_cap[2];

    chf_mem_t mem;             /* the shared decode's view of the above */
    bool powered;

    /* the image served first (the baked CONFIG or the opened app) */
    uint32_t live_size, live_crc;

    /* A staged image, built by whichever thread runs the transaction and taken
     * by the console's thread. loadlock covers it and the swap. */
    pthread_mutex_t loadlock;
    bool have_staged, staged_claims;
    chfmap_plan_t staged_plan;
    atomic_bool swap_armed;
    uint32_t staged_size, staged_crc;
    unsigned swaps;
    bool served_other;          /* anything but the baked CONFIG has run */

    /* The worker owns fujimail and the socket; the console's thread owns the
     * slots, the arena, the swap and the page. The worker's pokes reach the
     * arena through pokes[], applied before the next fetch; the console's
     * mailbox writes reach the worker through the hq[] queue. */
    bool async;
    bool worker_running;
    pthread_t worker;
    pthread_mutex_t qlock;
    pthread_cond_t qcv;
    uint16_t hq[QUEUE_SIZE];
    unsigned qhead, qtail;
    atomic_bool stop;
    atomic_bool busy;           /* the worker is inside fujimail */
    pthread_mutex_t pklock;
    uint16_t pk_addr[MAX_POKES];
    uint8_t pk_data[MAX_POKES];
    unsigned npokes;
    atomic_bool poked;

    /* the events the debugger watches */
    chf_fj_event_t ev[EV_RING];
    uint64_t evn;

    /* the 60 Hz tick's clock: the CPU's cycle counter, and its PC0 */
    const uint64_t *cycles;
    const uint16_t *pc0;
    uint64_t cycle_as;          /* attoseconds a CPU clock lasts */

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

/*-------------------------------------------------
    the tick
-------------------------------------------------*/

/* machine().time().as_ticks(60): floor(seconds * 60), from the CPU's cycle
 * count and MAME's integer attoseconds per cycle */
static uint16_t tick_now(const chf_fujinet_t *c)
{
    uint64_t t;

    if (!c->cycles || !c->cycle_as)
        return 0;
#ifdef __SIZEOF_INT128__
    t = (uint64_t)(((unsigned __int128)*c->cycles * c->cycle_as * 60u) / UINT64_C(1000000000000000000));
#else
    t = (uint64_t)((long double)*c->cycles * (long double)c->cycle_as * 60.0L / 1e18L);
#endif
    return (uint16_t)t;
}

/*-------------------------------------------------
    C port callbacks
-------------------------------------------------*/

/* poke(): a byte of the arena. The worker's go through the queue. */
static void poke(chf_fujinet_t *c, unsigned offset, uint8_t value)
{
    if (!t_on_worker) {
        if (offset < FN_ARENA_SIZE)
            c->arena[offset] = value;
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
    the slots, the page and the mailbox switch
-------------------------------------------------*/

/* set_image(): called at power-on and after every swap, so a swap always maps
 * page 0 */
static void set_image(chf_fujinet_t *c)
{
    const uint8_t np = c->npages[c->live];

    c->mem.rom = c->slot[c->live];
    c->mem.rom_size = CHFMAP_WINDOW;
    c->mem.npages = np;
    c->mem.pages = np ? c->slot[c->live] + CHFMAP_WINDOW : NULL;
    c->mem.bank = c->mem.pages;
}

/* set_mailbox(): the arena always answers, since the swap stub runs from it;
 * only the mailbox decode comes and goes */
static void set_mailbox(chf_fujinet_t *c, bool live)
{
    c->mem.ram = c->arena;
    c->mem.ram_base = FN_ARENA_BASE;
    c->mem.ram_size = FN_ARENA_SIZE;
    c->mem.mailbox = live;
}

static void apply_pokes(chf_fujinet_t *c, const uint16_t *addr, const uint8_t *data, unsigned n)
{
    /* only while the mailbox is live: a transaction in flight when a swap made
     * the arena plain RAM must not scribble on it */
    if (!c->mem.mailbox)
        return;
    for (unsigned i = 0; i < n; i++)
        if (addr[i] < FN_ARENA_SIZE)
            c->arena[addr[i]] = data[i];
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
    apply_pokes(c, addr, data, n);
}

/* hand an event to fujimail: inline, or through the worker's queue */
static void queue_hotspot(chf_fujinet_t *c, unsigned img)
{
    if (!c->async) {
        if (img == HQ_PAINT)
            fujimail_paint();
        else
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

/* Wait (at most `limit_ms`) until the worker has handled every event queued
 * so far. */
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

/* do_swap(): the staged image becomes the served one, on page 0, and the
 * mailbox follows the new image's claim. Inline in whatever serves the bus. */
static void do_swap(chf_fujinet_t *c)
{
    bool live;

    pthread_mutex_lock(&c->loadlock);
    if (!c->have_staged || !atomic_load(&c->swap_armed)) {
        pthread_mutex_unlock(&c->loadlock);
        return;
    }
    c->live ^= 1;
    c->npages[c->live ^ 1] = 0;
    set_image(c);
    atomic_store(&c->swap_armed, false);
    c->have_staged = false;
    live = c->staged_claims;
    set_mailbox(c, live);
    c->swaps++;
    c->served_other = true;
    c->live_size = c->staged_size;
    c->live_crc = c->staged_crc;
    pthread_mutex_unlock(&c->loadlock);
    if (live)
        queue_hotspot(c, HQ_PAINT);
    fprintf(stderr, "fujinet: swapped in the staged image (%u bytes); mailbox %s for this session\n",
            (unsigned)c->staged_size, live ? "kept" : "disabled");
}

/*-------------------------------------------------
    the push receiver (fujinet.cpp stream_*)
-------------------------------------------------*/

static uint8_t c_stream_open(int stream, uint32_t size)
{
    chf_fujinet_t *c = s_cart;
    const unsigned s = (unsigned)stream & 1;
    uint8_t err = (stream == FN_STREAM_ROM) ? chfmap_gate(size) : 0;

    if (err != 0)
        return err;
    if (stream == FN_STREAM_ROM) {
        pthread_mutex_lock(&c->loadlock);
        c->have_staged = false;         /* its slot is about to be overwritten */
        pthread_mutex_unlock(&c->loadlock);
    }
    c->rx_len[s] = 0;
    if (size && size > c->rx_cap[s]) {
        uint8_t *p = realloc(c->rx[s], size);

        if (!p)
            return FN_BOOT_ERR_TOOBIG;
        c->rx[s] = p;
        c->rx_cap[s] = size;
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
    if (stream != FN_STREAM_ROM) {
        c->rx_len[s] = 0;       /* the .cfg sibling: there are no mappers here, so it is dropped */
        return 0;
    }
    if (aborted) {
        c->rx_len[s] = 0;
        return 0;
    }
    if (c->rx_len[s] == 0 || chfmap_plan(c->rx[s], c->rx_len[s], &plan) != CHFMAP_OK) {
        c->rx_len[s] = 0;
        return FN_BOOT_ERR_NOMAP;
    }
    pthread_mutex_lock(&c->loadlock);
    staged = c->slot[c->live ^ 1];
    memset(staged, 0xff, CHFMAP_STORE);
    chfmap_apply(c->rx[s], &plan, staged);
    c->npages[c->live ^ 1] = plan.npages;
    c->staged_plan = plan;
    c->staged_claims = plan.mailbox_ok;
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

/* read_rom(): the driver maps $0800-$FFFF here. Reads have no side effects. */
static uint8_t fj_read_rom(chf_cart_t *base, uint16_t offset, int peek)
{
    chf_fujinet_t *c = base->priv;
    const uint16_t a = (uint16_t)(offset + FN_ROM_BASE);

    (void)peek;
    if (c->async)
        drain_pokes(c);
    /* The cart paints the tick from a timer; here it is emulated time. */
    if (c->mem.mailbox && (a == FN_ARENA_BASE + FN_R_TICK_LO || a == FN_ARENA_BASE + FN_R_TICK_HI)) {
        const uint16_t t = tick_now(c);

        return a == FN_ARENA_BASE + FN_R_TICK_LO ? (uint8_t)t : (uint8_t)(t >> 8);
    }
    return chf_read(&c->mem, a);
}

static void record(chf_fujinet_t *c, uint8_t kind, uint8_t reg, uint8_t value)
{
    chf_fj_event_t *e = &c->ev[c->evn & (EV_RING - 1)];

    e->cycle = c->cycles ? *c->cycles : 0;
    e->pc = c->pc0 ? (uint16_t)(*c->pc0 - 1) : 0;
    e->kind = kind;
    e->reg = reg;
    e->value = value;
    c->evn++;
}

/* write_ram(): every store at $0800-$FFFF (ROMC 05). */
static void fj_write_ram(chf_cart_t *base, uint16_t offset, uint8_t data)
{
    chf_fujinet_t *c = base->priv;
    uint8_t reg = 0, val = 0;
    chf_ev_t ev;

    if (c->async)
        drain_pokes(c);
    ev = chf_write(&c->mem, (uint16_t)(offset + FN_ROM_BASE), data, &reg, &val);

    switch (ev) {
    case CHF_EV_REG:
        /* expand into the REGSEL/REGDATA pair fujimail decodes */
        record(c, 1, reg, val);
        queue_hotspot(c, (unsigned)(FN_H_REGSEL + reg));
        queue_hotspot(c, (unsigned)(FN_H_REGDATA + val));
        break;

    case CHF_EV_TX:
        record(c, 2, 0, val);
        queue_hotspot(c, (unsigned)(FN_H_DATA + val));
        break;

    case CHF_EV_SWAP:
        /* The client wrote BOOTLOCK a few cycles ago; on the cartridge core0
         * has long since seen it. Here the worker may not have yet, so give it
         * the moment it needs rather than let the swap be missed. */
        if (!atomic_load(&c->swap_armed) && c->async)
            wait_worker_idle(c, 50);
        record(c, 4, 0, atomic_load(&c->swap_armed) ? 1 : 0);
        if (atomic_load(&c->swap_armed))
            do_swap(c);
        break;

    case CHF_EV_BANK:
        /* chf_write already switched the page */
        record(c, 3, 0, val);
        if (c->debug)
            fprintf(stderr, "fujinet: page %u\n", (unsigned)val);
        break;

    default:
        break;
    }
}

static const chf_cart_ops_t fujinet_ops = { .read_rom = fj_read_rom, .write_ram = fj_write_ram };

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

        if (img == HQ_PAINT)
            fujimail_paint();
        else if (c->mem.mailbox)        /* events queued before a swap are the old image's */
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
    c->arena = calloc(1, FN_ARENA_SIZE);
    if (!c->slot[0] || !c->slot[1] || !c->arena) {
        free(c->slot[0]);
        free(c->slot[1]);
        free(c->arena);
        free(c);
        return NULL;
    }
    memset(c->slot[0], 0xff, CHFMAP_STORE);
    memset(c->slot[1], 0xff, CHFMAP_STORE);

    /* the slot: $0800-$FFFF is the cartridge's; reads come from read_rom,
     * every store from write_ram */
    c->base.ops = &fujinet_ops;
    c->base.type = CHF_CART_FUJINET;
    c->base.ram_lo = FN_ROM_BASE;
    c->base.ram_hi = 0xffff;
    c->base.bank_lo = 1;
    c->base.bank_hi = 0;
    c->base.io_lo = 1;
    c->base.io_hi = 0;
    c->base.priv = c;

    atomic_init(&c->swap_armed, false);
    atomic_init(&c->stop, false);
    atomic_init(&c->poked, false);
    atomic_init(&c->busy, false);
    pthread_mutex_init(&c->loadlock, NULL);
    pthread_mutex_init(&c->qlock, NULL);
    pthread_cond_init(&c->qcv, NULL);
    pthread_mutex_init(&c->pklock, NULL);
    set_image(c);
    set_mailbox(c, false);          /* until an image claims it */
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
    free(c->arena);
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
    c->cycle_as = cycle_hz > 0.0 ? UINT64_C(1000000000000000000) / (uint64_t)(cycle_hz + 0.5) : 0;
}

void chf_fujinet_set_pc(chf_fujinet_t *c, const uint16_t *pc0)
{
    if (c)
        c->pc0 = pc0;
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
    case CHFMAP_ETOOBIG:   return "the image is larger than the cartridge can hold (a 16 KB window and up to six 8 KB pages)";
    case CHFMAP_ENOSIG:    return "the image has no $55 at $0800, so the console's BIOS would not start it";
    case CHFMAP_ENOCLAIM:  return "an image with APPBANK pages must carry the FujiNet claim";
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
    memset(c->arena, 0x00, FN_ARENA_SIZE);
    c->npages[0] = c->npages[1] = 0;
    c->live = 0;
    c->have_staged = c->staged_claims = false;
    memset(&c->staged_plan, 0, sizeof c->staged_plan);
    atomic_store(&c->swap_armed, false);
    c->swaps = 0;
    c->served_other = false;
    c->staged_size = c->staged_crc = 0;
    c->rx_len[0] = c->rx_len[1] = 0;
    c->npokes = 0;
    c->evn = 0;
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
    c->npages[0] = plan.npages;
    set_image(c);
    set_mailbox(c, plan.mailbox_ok);
    if (plan.npages)
        fprintf(stderr, "fujinet: APPBANK image, %u pages; mailbox kept\n", (unsigned)plan.npages);
    else
        fprintf(stderr, "fujinet: %u-byte image loaded, mailbox %s\n", (unsigned)len,
                plan.mailbox_ok ? "live" : "dead");

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
    } else {
        fprintf(stderr, "fujinet: no claim signature at $47FC; running with the mailbox dead\n");
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
    out->page = -1;
    if (!c)
        return;
    out->powered = c->powered;
    out->mailbox = c->mem.mailbox;
    out->link_up = fujitcp_active();
    out->link_off = c->link_off;
    out->app_live = c->npages[c->live] != 0;
    out->booted_game = c->served_other;
    out->armed = atomic_load(&c->swap_armed);
    out->staged_app = c->have_staged && c->staged_plan.npages != 0;
    out->staged_mailbox = c->have_staged && c->staged_claims;
    out->live_size = c->live_size;
    out->live_crc = c->live_crc;
    out->staged_size = c->staged_size;
    out->staged_crc = c->staged_crc;
    out->swaps = c->swaps;
    out->npages = c->npages[c->live];
    if (c->mem.pages && c->mem.bank)
        out->page = (int)((c->mem.bank - c->mem.pages) / FN_APP_PAGE);
    out->tick = c->mem.mailbox ? tick_now(c) : 0;
    mb = c->arena;
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

uint8_t chf_fujinet_peek(chf_fujinet_t *c, uint16_t addr)
{
    if (!c)
        return 0xff;
    if (c->mem.mailbox && (addr == FN_ARENA_BASE + FN_R_TICK_LO || addr == FN_ARENA_BASE + FN_R_TICK_HI)) {
        const uint16_t t = tick_now(c);

        return addr == FN_ARENA_BASE + FN_R_TICK_LO ? (uint8_t)t : (uint8_t)(t >> 8);
    }
    return chf_read(&c->mem, addr);
}

void chf_fujinet_poke(chf_fujinet_t *c, uint16_t addr, uint8_t data)
{
    if (!c || addr < FN_ROM_BASE)
        return;
    if (addr >= FN_ARENA_BASE) {
        /* an edit never fires a hotspot */
        if (!c->mem.mailbox || addr - FN_ARENA_BASE < FN_H_REGSEL)
            c->arena[addr - FN_ARENA_BASE] = data;
        return;
    }
    if ((uint32_t)(addr - FN_ROM_BASE) < c->mem.rom_size) {
        c->slot[c->live][addr - FN_ROM_BASE] = data;
    } else if (c->mem.bank && (uint16_t)(addr - FN_APP_WIN) < FN_APP_PAGE) {
        ((uint8_t *)c->mem.bank)[addr - FN_APP_WIN] = data;
    }
}

void chf_fujinet_mailbox(chf_fujinet_t *c, uint8_t out[CHF_FJ_MAILBOX_BYTES])
{
    if (!c) {
        memset(out, 0xff, CHF_FJ_MAILBOX_BYTES);
        return;
    }
    for (unsigned i = 0; i < CHF_FJ_MAILBOX_BYTES; i++)
        out[i] = chf_fujinet_peek(c, (uint16_t)(FN_ARENA_BASE + FN_R_DATA + i));
}

uint8_t chf_fujinet_image_byte(chf_fujinet_t *c, uint32_t offset)
{
    if (!c || offset >= CHFMAP_STORE)
        return 0xff;
    return c->slot[c->live][offset];
}

void chf_fujinet_image_poke(chf_fujinet_t *c, uint32_t offset, uint8_t data)
{
    if (c && offset < CHFMAP_STORE)
        c->slot[c->live][offset] = data;
}

uint8_t chf_fujinet_arena(chf_fujinet_t *c, uint32_t offset)
{
    if (!c || offset >= FN_ARENA_SIZE)
        return 0xff;
    if (c->mem.mailbox && (offset == FN_R_TICK_LO || offset == FN_R_TICK_HI)) {
        const uint16_t t = tick_now(c);

        return offset == FN_R_TICK_LO ? (uint8_t)t : (uint8_t)(t >> 8);
    }
    return c->arena[offset];
}

void chf_fujinet_arena_poke(chf_fujinet_t *c, uint32_t offset, uint8_t data)
{
    if (c && offset < FN_ARENA_SIZE)
        c->arena[offset] = data;
}

int chf_fujinet_events(chf_fujinet_t *c, uint64_t *cursor, chf_fj_event_t *out, int max)
{
    int n = 0;

    if (!c || !cursor)
        return 0;
    if (c->evn > EV_RING && *cursor < c->evn - EV_RING)
        *cursor = c->evn - EV_RING;     /* fell behind the ring */
    if (*cursor > c->evn)
        *cursor = c->evn;
    while (*cursor < c->evn && n < max)
        out[n++] = c->ev[(*cursor)++ & (EV_RING - 1)];
    return n;
}

uint32_t chf_fujinet_copy_image(chf_fujinet_t *c, uint8_t *dst, uint32_t max)
{
    uint32_t n;

    if (!c || !dst)
        return 0;
    n = c->live_size < CHFMAP_STORE ? c->live_size : CHFMAP_STORE;
    if (n > max)
        n = max;
    memcpy(dst, c->slot[c->live], n);
    return n;
}
