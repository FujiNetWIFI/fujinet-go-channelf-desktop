/*
 * debugger.c -- the chfdebug engine (core/include/chfdebug.h).
 *
 * Built on the core's two hooks: the 2650's instruction hook (s2650.h:
 * called with the address of the instruction about to be fetched, AFTER any
 * interrupt entry, so a breakpoint on a handler's first instruction hits),
 * and the machine's bus hook (core/chf/host.h), fired on PROG-space data
 * reads and writes only while a breakpoint of that kind exists. Attached, the
 * instruction hook checks the stop request, the breakpoints and the step
 * mode; stopping PARKS the emulation thread right there (a condition
 * variable) until a window resumes it -- the CPU's own progress is what
 * stops, there is no "paused" flag a run loop polls. Detached, neither hook
 * is installed, so a session that never opens the debugger pays nothing. The
 * design and the feature set (prompt, conditions, breakpoints, scanline and
 * frame steps, symbols, save) are the Odyssey 2, SMS and Studio II siblings'.
 *
 * The 2650 has no I/O space the Channel F uses and no RAM outside the UVI, so
 * every data breakpoint is a PROG-space one and the bus hook covers all of
 * them: a read or write of the UVI at $1800-$1AFF is a PROG access.
 *
 * Threading. Everything public is called from UI threads; the hooks run on
 * the emulation thread. Every public call that reads or edits the machine
 * holds the host's run lock (chf_host_lock), which the emulation thread
 * holds while it runs a frame and lets go of between frames and while it is
 * parked here -- so a window refreshing live values while the machine runs
 * waits a moment instead of tearing, and a stopped machine is simply the
 * UI's. Lock order: the run lock, then d->lock (the hooks run with the run
 * lock held and take d->lock). The symbol table is only ever touched under
 * the run lock. The 2650 stops between instructions with nothing fetched
 * yet, so a PC change while stopped is just the CPU's page and iar.
 *
 * The FujiNet trace stands apart: the transport feeds it (fujitrace.c) from
 * session start, attached or not, on its own thread and under its own lock.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "debugger_internal.h"
#include "expr.h"
#include "fujitrace.h"

#include "chfmap.h"
#include "dasm2650.h"
#include "fuji_mailbox.h"
#include "fujitcp_host.h"
#include "host.h"

#define MAX_BP     128
#define TRACE_N    4096
#define PROG_SIZE  0x8000
#define UVI_REGS   0x300

enum { MODE_RUN = 0, MODE_STEP, MODE_OVER, MODE_OUT, MODE_RUNTO, MODE_SCANLINE, MODE_FRAME };

struct chfdebug {
    pthread_mutex_t lock;
    pthread_cond_t cond;

    atomic_int attached;
    atomic_int stopped;          /* the emulation thread is parked */
    atomic_int stop_req;
    atomic_int mode;
    long count;                  /* step n / scanline n */
    uint16_t target;             /* run to; step over: the return address */
    int target_sp;               /* step over: the SP it returns to; step out: the SP it leaves */
    int out_pending;             /* step out: a return ran, judge it at the next boundary */
    int out_sp;                  /* ... and the SP it ran at */
    int last_vpos;
    uint64_t target_frame;
    int was_attached;            /* across a power cycle */

    char reason[128];
    int reason_addr;
    atomic_uint generation;

    chfdebug_breakpoint bp[MAX_BP];
    int nbp, next_id;
    uint8_t map_exec[PROG_SIZE];     /* nonzero: an enabled exec bp covers it */
    uint8_t map_mem[PROG_SIZE];      /* CHFDEBUG_BP_READ / _WRITE bits */
    atomic_int break_pending;
    char break_reason[128];

    /* the access a condition sees as `value` / `addr` */
    long acc_value, acc_addr;
    int in_access;

    atomic_int trace_on;
    chfdebug_trace ring[TRACE_N];
    int ring_head, ring_count;

    symtab syms;
    char cart_path[1024];

    /* the FujiNet trace, fed on the transport's thread */
    pthread_mutex_t fuji_lock;
    fujitrace fuji;
};

static chfdebug *g_traced;       /* the engine the transport's tap feeds */
static chfdebug *g_hooked;       /* the engine the CPU's hook calls */

chf_machine_t *chfdebug_machine(void)
{
    return chf_host_is_running() ? chf_host_machine() : NULL;
}

static void bump(chfdebug *d)
{
    atomic_fetch_add(&d->generation, 1);
}

/* the address of the instruction about to run */
static uint16_t cur_pc(const chf_machine_t *m)
{
    return s2650_pc(&m->cpu);
}

static uint8_t pbyte(chf_machine_t *m, uint16_t a)
{
    return chf_machine_peek(m, CHF_PEEK_PROG, a & 0x7fff);
}

/* The address after `len` bytes from a: the 2650's IAR wraps inside its 8K page. */
static uint16_t next_in_page(uint16_t a, int len)
{
    return (uint16_t)((a & 0x6000) | ((a + len) & 0x1fff));
}

/* ---- the beam --------------------------------------------------------------- */

void chfdebug_beam(chf_machine_t *m, int *vpos, int *hpos)
{
    const double lc = (double)m->line_as / (double)m->cycle_as;   /* CPU cycles a line takes */
    double frac = lc > 0 ? ((double)m->cpu.cycles - ((double)m->next_limit - lc)) / lc : 0;

    if (frac < 0)
        frac = 0;
    if (frac > 0.999)
        frac = 0.999;
    *vpos = m->uvi.line;
    *hpos = (int)(frac * UVI_WIDTH);
}

/* ---- expressions ---------------------------------------------------------- */

const char *const chfdebug_reg_names[] = {
    "r0", "r1", "r2", "r3", "r1'", "r2'", "r3'", "pc", "iar", "psu", "psl", "sp", "cc",
    "c", "com", "wc", "rs", "idc", "ovf", "f", "ii", "s", "vpos", "hpos", "frame", "cycles",
    "value", "addr", NULL,
};

static uint8_t expr_read(void *user, int space, uint32_t addr)
{
    chf_machine_t *m = chfdebug_machine();
    (void)user;
    if (!m || space < 0 || space > 2)
        return 0xff;
    if (space == CHF_SPACE_PROG)
        return addr < PROG_SIZE ? chf_machine_peek(m, CHF_PEEK_PROG, addr) : 0xff;
    if (space == CHF_SPACE_UVI)
        return addr < UVI_REGS ? chf_machine_peek(m, CHF_PEEK_UVI, addr) : 0xff;
    return chf_machine_peek(m, CHF_PEEK_CART, addr);
}

/* r1-r3 (the set in use), r1'-r3' (set 1) */
static int reg_name(const char *name, int *n, int *primed)
{
    if (name[0] != 'r' || name[1] < '0' || name[1] > '3')
        return 0;
    *n = name[1] - '0';
    if (name[2] == '\0')
        *primed = 0;
    else if (name[2] == '\'' && name[3] == '\0' && *n > 0)
        *primed = 1;
    else
        return 0;
    return 1;
}

static int expr_name(void *user, const char *raw, long *out)
{
    chfdebug *d = user;
    chf_machine_t *m = chfdebug_machine();
    char name[64];
    int n, primed;
    size_t i;

    for (i = 0; raw[i] && i + 1 < sizeof name; i++)
        name[i] = (char)tolower((unsigned char)raw[i]);
    name[i] = '\0';

    if (!strcmp(name, "value") && d->in_access) { *out = d->acc_value; return 1; }
    if (!strcmp(name, "addr") && d->in_access) { *out = d->acc_addr; return 1; }
    if (m)
    {
        const s2650_t *c = &m->cpu;
        if (reg_name(name, &n, &primed))
        {
            *out = primed ? s2650_reg_bank(c, 1, n) : s2650_reg(c, n);
            return 1;
        }
        if (!strcmp(name, "pc"))  { *out = cur_pc(m); return 1; }
        if (!strcmp(name, "iar")) { *out = c->iar; return 1; }
        if (!strcmp(name, "psu")) { *out = c->psu; return 1; }
        if (!strcmp(name, "psl")) { *out = c->psl; return 1; }
        if (!strcmp(name, "sp"))  { *out = c->psu & S2650_SP; return 1; }
        if (!strcmp(name, "cc"))  { *out = (c->psl & S2650_CC) >> 6; return 1; }
        if (!strcmp(name, "c"))   { *out = (c->psl & S2650_C) != 0; return 1; }
        if (!strcmp(name, "com")) { *out = (c->psl & S2650_COM) != 0; return 1; }
        if (!strcmp(name, "wc"))  { *out = (c->psl & S2650_WC) != 0; return 1; }
        if (!strcmp(name, "rs"))  { *out = (c->psl & S2650_RS) != 0; return 1; }
        if (!strcmp(name, "idc")) { *out = (c->psl & S2650_IDC) != 0; return 1; }
        if (!strcmp(name, "ovf")) { *out = (c->psl & S2650_OVF) != 0; return 1; }
        if (!strcmp(name, "f"))   { *out = (c->psu & S2650_FO) != 0; return 1; }
        if (!strcmp(name, "ii"))  { *out = (c->psu & S2650_II) != 0; return 1; }
        if (!strcmp(name, "s"))   { *out = uvi_vblank(&m->uvi); return 1; }
        if (!strcmp(name, "vpos") || !strcmp(name, "hpos"))
        {
            int vp, hp;
            chfdebug_beam(m, &vp, &hp);
            *out = name[0] == 'v' ? vp : hp;
            return 1;
        }
        if (!strcmp(name, "frame"))  { *out = (long)m->frames_done; return 1; }
        if (!strcmp(name, "cycles")) { *out = (long)c->cycles; return 1; }
    }
    {
        int a = symtab_find(&d->syms, raw);
        if (a >= 0)
        {
            *out = a;
            return 1;
        }
    }
    return 0;
}

int chfdebug_eval(chfdebug *d, const char *text, long *out, char *err, int errsz)
{
    expr_ctx ctx = { expr_name, expr_read, d };
    int rc;
    chf_host_lock();
    rc = expr_eval(text, &ctx, out, err, errsz);
    chf_host_unlock();
    return rc;
}

/* ---- the hooks (emulation thread) ----------------------------------------- */

static void trace_record(chfdebug *d, chf_machine_t *m, uint16_t pc)
{
    chfdebug_trace *t = &d->ring[d->ring_head];
    const s2650_t *c = &m->cpu;
    int vp, hp;

    t->pc = pc;
    t->r0 = s2650_reg(c, 0);
    t->r1 = s2650_reg(c, 1);
    t->r2 = s2650_reg(c, 2);
    t->r3 = s2650_reg(c, 3);
    t->psu = c->psu;
    t->psl = c->psl;
    t->bytes[0] = pbyte(m, pc);
    t->len = (uint8_t)dasm2650_length(t->bytes[0]);
    t->bytes[1] = t->len > 1 ? pbyte(m, next_in_page(pc, 1)) : 0;
    t->bytes[2] = t->len > 2 ? pbyte(m, dasm2650_operand2_addr(pc)) : 0;
    t->cycles = c->cycles;
    chfdebug_beam(m, &vp, &hp);
    t->vpos = (int16_t)vp;
    t->hpos = (int16_t)hp;
    d->ring_head = (d->ring_head + 1) % TRACE_N;
    if (d->ring_count < TRACE_N)
        d->ring_count++;
}

static int cond_true(chfdebug *d, const chfdebug_breakpoint *b)
{
    long v = 0;
    char err[64];
    if (!b->condition[0])
        return 1;
    if (chfdebug_eval(d, b->condition, &v, err, sizeof err) != 0)
        return 1;   /* a condition that no longer evaluates stops, rather than hides */
    return v != 0;
}

/* Park until a window resumes. Called on the emulation thread, with the run
 * lock held and d->lock not; lets go of the run lock while parked. */
static void park(chfdebug *d, chf_machine_t *m, uint16_t pc, const char *reason)
{
    /* the picture as far as the beam has drawn it, and the cart's latest */
    chf_host_publish_frame();
    chf_fujinet_frame(m->fujinet);

    pthread_mutex_lock(&d->lock);
    snprintf(d->reason, sizeof d->reason, "%s", reason);
    d->reason_addr = pc;
    atomic_store(&d->mode, MODE_RUN);
    atomic_store(&d->stop_req, 0);
    atomic_store(&d->stopped, 1);
    bump(d);
    /* parked, the machine is the windows' */
    chf_host_park_release();
    while (atomic_load(&d->stopped) && atomic_load(&d->attached))
        pthread_cond_wait(&d->cond, &d->lock);
    atomic_store(&d->stopped, 0);
    pthread_mutex_unlock(&d->lock);
    chf_host_park_reacquire();
}

/* One breakpoint check for a data access (d->lock and the run lock held).
 * Returns 1 and arms the stop when one hits. */
static int hit_check(chfdebug *d, int type, unsigned addr, unsigned value)
{
    int hit = 0;

    if (atomic_load(&d->break_pending))
        return 1;
    d->acc_value = (long)value;
    d->acc_addr = (long)addr;
    d->in_access = 1;
    for (int i = 0; i < d->nbp; i++)
    {
        chfdebug_breakpoint *b = &d->bp[i];
        if (!b->enabled || !(b->type & type))
            continue;
        if (addr < b->start || addr > b->end)
            continue;
        if (!cond_true(d, b))
            continue;
        b->hits++;
        snprintf(d->break_reason, sizeof d->break_reason, "%s prog $%04X = $%02X",
                 type == CHFDEBUG_BP_READ ? "read" : "write", addr, value & 0xff);
        atomic_store(&d->break_pending, 1);
        hit = 1;
        break;
    }
    d->in_access = 0;
    return hit;
}

/* Does the step mode say stop here? Fills why. */
static int mode_stops(chfdebug *d, chf_machine_t *m, uint16_t pc, const char **why)
{
    const int sp = m->cpu.psu & S2650_SP;

    switch (atomic_load(&d->mode))
    {
    case MODE_STEP:
        if (--d->count <= 0)
        {
            *why = "step";
            return 1;
        }
        break;
    case MODE_OVER:
        if (pc == d->target && sp == d->target_sp)
        {
            *why = "step over";
            return 1;
        }
        break;
    case MODE_OUT:
        if (d->out_pending)
        {
            d->out_pending = 0;
            /* a conditional return not taken leaves the stack as it was */
            if (sp != d->out_sp)
            {
                *why = "step out";
                return 1;
            }
        }
        if (dasm2650_op_flags(pbyte(m, pc)) & DASM2650_STEP_OUT)
        {
            if (sp <= d->target_sp)
            {
                d->out_pending = 1;
                d->out_sp = sp;
            }
        }
        break;
    case MODE_RUNTO:
        if (pc == d->target)
        {
            *why = "run to";
            return 1;
        }
        break;
    case MODE_SCANLINE:
    {
        int vp, hp;
        chfdebug_beam(m, &vp, &hp);
        if (vp != d->last_vpos)
        {
            d->last_vpos = vp;
            if (--d->count <= 0)
            {
                *why = "scanline";
                return 1;
            }
        }
        break;
    }
    case MODE_FRAME:
        if (m->frames_done >= d->target_frame)
        {
            *why = "frame";
            return 1;
        }
        break;
    default:
        break;
    }
    return 0;
}

static void on_instruction(chfdebug *d, chf_machine_t *m, uint16_t pc)
{
    char why[128];
    const char *mode_why = NULL;
    int stop = 0;

    if (atomic_load_explicit(&d->trace_on, memory_order_relaxed))
        trace_record(d, m, pc);

    if (atomic_load(&d->break_pending))
    {
        atomic_store(&d->break_pending, 0);
        pthread_mutex_lock(&d->lock);
        snprintf(why, sizeof why, "%s", d->break_reason);
        pthread_mutex_unlock(&d->lock);
        stop = 1;
    }
    else if (atomic_load(&d->stop_req))
    {
        snprintf(why, sizeof why, "stopped");
        stop = 1;
    }
    else if (d->map_exec[pc & 0x7fff])
    {
        pthread_mutex_lock(&d->lock);
        for (int i = 0; i < d->nbp; i++)
        {
            chfdebug_breakpoint *b = &d->bp[i];
            if (b->enabled && (b->type & CHFDEBUG_BP_EXEC) && pc >= b->start && pc <= b->end &&
                cond_true(d, b))
            {
                b->hits++;
                snprintf(why, sizeof why, "breakpoint: exec $%04X", pc);
                stop = 1;
                break;
            }
        }
        pthread_mutex_unlock(&d->lock);
    }
    if (!stop && mode_stops(d, m, pc, &mode_why))
    {
        snprintf(why, sizeof why, "%s", mode_why);
        stop = 1;
    }
    if (stop)
        park(d, m, pc, why);
    /* a poke or a PC change while stopped is simply what the fetch that
     * follows this hook sees */
}

/* s2650_t.instruction_hook: the CPU's callback, on the emulation thread */
static void cpu_hook(void *ctx, uint16_t pc)
{
    chfdebug *d = g_hooked;

    if (d)
        on_instruction(d, ctx, pc);
}

static void bus_hook(chf_machine_t *m, void *user, int kind, uint16_t addr, uint8_t data)
{
    chfdebug *d = user;
    int type;

    (void)m;
    if (atomic_load(&d->break_pending))
        return;
    if (kind == CHF_BUS_MEMR)
        type = CHFDEBUG_BP_READ;
    else if (kind == CHF_BUS_MEMW)
        type = CHFDEBUG_BP_WRITE;
    else
        return;
    addr &= 0x7fff;
    if (!(d->map_mem[addr] & type))
        return;
    pthread_mutex_lock(&d->lock);
    hit_check(d, type, addr, data);
    pthread_mutex_unlock(&d->lock);
}

/* What the machine's bus hook must watch, from the enabled breakpoints. */
static uint8_t watch_bits(chfdebug *d)
{
    uint8_t w = 0;
    for (int i = 0; i < d->nbp; i++)
    {
        const chfdebug_breakpoint *b = &d->bp[i];
        if (!b->enabled)
            continue;
        if (b->type & CHFDEBUG_BP_READ)
            w |= CHF_WATCH_MEMR;
        if (b->type & CHFDEBUG_BP_WRITE)
            w |= CHF_WATCH_MEMW;
    }
    return w;
}

/* Rebuild the lookup maps and the bus watch. Called with the run lock and
 * d->lock held (the hooks read the maps under the run lock). */
static void rebuild_locked(chfdebug *d)
{
    memset(d->map_exec, 0, sizeof d->map_exec);
    memset(d->map_mem, 0, sizeof d->map_mem);
    for (int i = 0; i < d->nbp; i++)
    {
        const chfdebug_breakpoint *b = &d->bp[i];
        if (!b->enabled)
            continue;
        if (b->type & CHFDEBUG_BP_EXEC)
            for (unsigned a = b->start; a <= b->end && a < PROG_SIZE; a++)
                d->map_exec[a] = 1;
        if (b->type & (CHFDEBUG_BP_READ | CHFDEBUG_BP_WRITE))
            for (unsigned a = b->start; a <= b->end && a < PROG_SIZE; a++)
                d->map_mem[a] |= (uint8_t)(b->type & (CHFDEBUG_BP_READ | CHFDEBUG_BP_WRITE));
    }
    if (atomic_load(&d->attached) && chf_host_is_running())
        chf_host_set_bus_hook(bus_hook, d, watch_bits(d));
}

/* ---- lifecycle -------------------------------------------------------------- */

chfdebug *chfdebug_create(void)
{
    chfdebug *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    pthread_mutex_init(&d->lock, NULL);
    pthread_cond_init(&d->cond, NULL);
    pthread_mutex_init(&d->fuji_lock, NULL);
    fujitrace_init(&d->fuji);
    d->next_id = 1;
    d->reason_addr = -1;
    symtab_init(&d->syms);
    return d;
}

void chfdebug_free(chfdebug *d)
{
    if (!d)
        return;
    if (g_traced == d)
    {
        fujitcp_set_trace(NULL, NULL);
        g_traced = NULL;
    }
    chfdebug_detach(d);
    symtab_free(&d->syms);
    pthread_mutex_destroy(&d->lock);
    pthread_cond_destroy(&d->cond);
    pthread_mutex_destroy(&d->fuji_lock);
    free(d);
}

/* the transport's thread: the cartridge's worker, or the emulation thread
 * in sync mode */
static void fuji_tap(void *user, int dir, const uint8_t *buf, size_t len)
{
    chfdebug *d = user;
    pthread_mutex_lock(&d->fuji_lock);
    fujitrace_feed(&d->fuji, dir, buf, len);
    pthread_mutex_unlock(&d->fuji_lock);
}

void chfdebug_fujinet_install(chfdebug *d)
{
    if (!d)
        return;
    g_traced = d;
    fujitcp_set_trace(fuji_tap, d);
}

/* run lock held */
static void hook(chfdebug *d)
{
    if (!chf_host_is_running())
        return;
    chf_host_set_bus_hook(bus_hook, d, watch_bits(d));
    g_hooked = d;
    chf_host_machine()->cpu.instruction_hook = cpu_hook;
}

static void unhook(chfdebug *d)
{
    if (g_hooked == d)
        g_hooked = NULL;
    if (!chf_host_is_running())
        return;
    chf_host_machine()->cpu.instruction_hook = NULL;
    chf_host_set_bus_hook(NULL, NULL, 0);
}

void chfdebug_attach(chfdebug *d)
{
    if (!d)
        return;
    chf_host_lock();
    pthread_mutex_lock(&d->lock);
    if (!atomic_load(&d->attached))
    {
        atomic_store(&d->attached, 1);
        atomic_store(&d->stop_req, 1);
        atomic_store(&d->mode, MODE_RUN);
        atomic_store(&d->break_pending, 0);
        hook(d);
    }
    bump(d);
    pthread_mutex_unlock(&d->lock);
    chf_host_unlock();
}

void chfdebug_detach(chfdebug *d)
{
    if (!d)
        return;
    chf_host_lock();
    pthread_mutex_lock(&d->lock);
    atomic_store(&d->attached, 0);
    atomic_store(&d->stop_req, 0);
    atomic_store(&d->mode, MODE_RUN);
    unhook(d);
    pthread_cond_broadcast(&d->cond);
    bump(d);
    pthread_mutex_unlock(&d->lock);
    chf_host_unlock();
    /* wait for the parked thread to leave the hook */
    for (int i = 0; i < 2000 && atomic_load(&d->stopped); i++)
    {
        struct timespec ts = { 0, 1000000L };
        nanosleep(&ts, NULL);
    }
}

int chfdebug_is_attached(chfdebug *d)
{
    return d && atomic_load(&d->attached);
}

void chfdebug_power_down(chfdebug *d)
{
    if (!d)
        return;
    d->was_attached = atomic_load(&d->attached);
    chfdebug_detach(d);
}

void chfdebug_power_up(chfdebug *d)
{
    int want;

    if (!d)
        return;
    /* attached across the power cycle, or attached before there was a
     * machine to hook (a window opened before the session started) */
    want = d->was_attached || atomic_load(&d->attached);
    atomic_store(&d->attached, 0);
    if (want)
        chfdebug_attach(d);
    d->was_attached = 0;
}

void chfdebug_set_cart_path(chfdebug *d, const char *path)
{
    if (d)
        snprintf(d->cart_path, sizeof d->cart_path, "%s", path ? path : "");
}

symtab *chfdebug_symbols(chfdebug *d)
{
    return &d->syms;
}

int chfdebug_is_stopped(chfdebug *d)
{
    return d && atomic_load(&d->stopped);
}

void chfdebug_stop(chfdebug *d)
{
    if (!d)
        return;
    if (!atomic_load(&d->attached))
        chfdebug_attach(d);
    atomic_store(&d->stop_req, 1);
}

/* Leave the parked state with a mode. */
static void go(chfdebug *d, int mode)
{
    pthread_mutex_lock(&d->lock);
    atomic_store(&d->mode, mode);
    atomic_store(&d->stop_req, 0);
    atomic_store(&d->stopped, 0);
    pthread_cond_broadcast(&d->cond);
    bump(d);
    pthread_mutex_unlock(&d->lock);
}

void chfdebug_resume(chfdebug *d)
{
    if (d && atomic_load(&d->stopped))
        go(d, MODE_RUN);
}

int chfdebug_stop_reason(chfdebug *d, char *dst, int dstsz, int *address)
{
    int n;
    if (!d || !dst || dstsz <= 0)
        return 0;
    pthread_mutex_lock(&d->lock);
    n = snprintf(dst, (size_t)dstsz, "%s", atomic_load(&d->stopped) ? d->reason : "running");
    if (address)
        *address = atomic_load(&d->stopped) ? d->reason_addr : -1;
    pthread_mutex_unlock(&d->lock);
    return n;
}

unsigned chfdebug_generation(chfdebug *d)
{
    return d ? atomic_load(&d->generation) : 0;
}

/* ---- stepping ---------------------------------------------------------------- */

void chfdebug_step_n(chfdebug *d, int n)
{
    if (!d || !atomic_load(&d->stopped))
        return;
    d->count = n > 0 ? n : 1;
    go(d, MODE_STEP);
}

void chfdebug_step(chfdebug *d)
{
    chfdebug_step_n(d, 1);
}

void chfdebug_step_over(chfdebug *d)
{
    chf_machine_t *m;
    int over = 0;

    if (!d)
        return;
    chf_host_lock();
    m = chfdebug_machine();
    if (m && atomic_load(&d->stopped))
    {
        const uint16_t pc = cur_pc(m);
        const uint8_t op = pbyte(m, pc);

        /* a branch to subroutine comes back to the instruction after it, with
         * the return stack where it was */
        if (dasm2650_op_flags(op) & DASM2650_STEP_OVER)
        {
            d->target = next_in_page(pc, dasm2650_length(op));
            d->target_sp = m->cpu.psu & S2650_SP;
            over = 1;
        }
    }
    chf_host_unlock();
    if (over)
        go(d, MODE_OVER);
    else
        chfdebug_step(d);
}

void chfdebug_step_out(chfdebug *d)
{
    chf_machine_t *m;
    int ok = 0;

    if (!d)
        return;
    chf_host_lock();
    m = chfdebug_machine();
    if (m && atomic_load(&d->stopped))
    {
        /* the first return to run at this stack depth or shallower ends the
         * step; anything deeper is a call (or an interrupt) of its own */
        d->target_sp = m->cpu.psu & S2650_SP;
        d->out_pending = 0;
        ok = 1;
    }
    chf_host_unlock();
    if (ok)
        go(d, MODE_OUT);
}

void chfdebug_scanline(chfdebug *d, int n)
{
    chf_machine_t *m;
    int ok = 0;

    if (!d)
        return;
    chf_host_lock();
    m = chfdebug_machine();
    if (m && atomic_load(&d->stopped))
    {
        int hp;
        chfdebug_beam(m, &d->last_vpos, &hp);
        d->count = n > 0 ? n : 1;
        ok = 1;
    }
    chf_host_unlock();
    if (ok)
        go(d, MODE_SCANLINE);
}

void chfdebug_frame(chfdebug *d, int n)
{
    chf_machine_t *m;
    int ok = 0;

    if (!d)
        return;
    chf_host_lock();
    m = chfdebug_machine();
    if (m && atomic_load(&d->stopped))
    {
        d->target_frame = m->frames_done + (uint64_t)(n > 0 ? n : 1);
        ok = 1;
    }
    chf_host_unlock();
    if (ok)
        go(d, MODE_FRAME);
}

void chfdebug_run_to(chfdebug *d, uint16_t addr)
{
    if (!d || !atomic_load(&d->stopped))
        return;
    d->target = (uint16_t)(addr & 0x7fff);
    go(d, MODE_RUNTO);
}

/* ---- CPU ---------------------------------------------------------------------- */

static void cpu_get_locked(chf_machine_t *m, chfdebug_cpu *o)
{
    const s2650_t *c;

    memset(o, 0, sizeof *o);
    o->illegal_pc = -1;
    if (!m)
        return;
    c = &m->cpu;
    o->pc = cur_pc(m);
    o->page = c->page;
    o->iar = c->iar;
    for (int b = 0; b < 2; b++)
        for (int n = 0; n < 4; n++)
            o->r[b][n] = s2650_reg_bank(c, b, n);
    o->r0 = c->reg[0];
    o->psu = c->psu;
    o->psl = c->psl;
    o->sense = uvi_vblank(&m->uvi);
    o->flag = (c->psu & S2650_FO) != 0;
    o->ii = (c->psu & S2650_II) != 0;
    o->sp = c->psu & S2650_SP;
    o->cc = (c->psl & S2650_CC) >> 6;
    o->idc = (c->psl & S2650_IDC) != 0;
    o->rs = (c->psl & S2650_RS) != 0;
    o->wc = (c->psl & S2650_WC) != 0;
    o->ovf = (c->psl & S2650_OVF) != 0;
    o->com = (c->psl & S2650_COM) != 0;
    o->c = (c->psl & S2650_C) != 0;
    for (int i = 0; i < 8; i++)
        o->ras[i] = c->ras[i] & 0x7fff;
    o->halted = c->halt != 0;
    o->irq_line = c->irq_state != 0;
    o->cycles = c->cycles;
    o->instructions = c->instructions;
    o->illegal_count = c->illegal_count;
    if (c->illegal_count)
        o->illegal_pc = c->illegal_pc;
    chfdebug_beam(m, &o->vpos, &o->hpos);
    o->frame = (uint32_t)m->frames_done;
    o->machine = m->machine;
}

void chfdebug_cpu_get(chfdebug *d, chfdebug_cpu *o)
{
    (void)d;
    chf_host_lock();
    cpu_get_locked(chfdebug_machine(), o);
    chf_host_unlock();
}

void chfdebug_cpu_set(chfdebug *d, int reg, int v)
{
    chf_machine_t *m;
    s2650_t *c;

    if (!d)
        return;
    chf_host_lock();
    m = chfdebug_machine();
    if (!m || !atomic_load(&d->stopped))
    {
        chf_host_unlock();
        return;
    }
    c = &m->cpu;
    if (reg >= CHF_REG_RAS0 && reg < CHF_REG_RAS0 + 8)
        c->ras[reg - CHF_REG_RAS0] = (uint16_t)(v & 0x7fff);
    else
    {
        switch (reg)
        {
        case CHF_REG_R0:  c->reg[0] = (uint8_t)v; break;
        case CHF_REG_R1: case CHF_REG_R2: case CHF_REG_R3:
            c->reg[reg - CHF_REG_R0] = (uint8_t)v;
            break;
        case CHF_REG_R1B: case CHF_REG_R2B: case CHF_REG_R3B:
            s2650_set_reg_bank(c, 1, reg - CHF_REG_R1B + 1, (uint8_t)v);
            break;
        case CHF_REG_PC:  s2650_set_pc(c, (uint16_t)(v & 0x7fff)); break;
        case CHF_REG_IAR: c->iar = (uint16_t)(v & 0x1fff); break;
        case CHF_REG_PSU: s2650_set_psu(c, (uint8_t)v); break;
        case CHF_REG_PSL: s2650_set_psl(c, (uint8_t)v); break;
        case CHF_REG_SP:  s2650_set_psu(c, (uint8_t)((c->psu & ~S2650_SP) | (v & S2650_SP))); break;
        case CHF_REG_F:
            s2650_set_psu(c, (uint8_t)(v ? c->psu | S2650_FO : c->psu & ~S2650_FO));
            break;
        case CHF_REG_II:
            s2650_set_psu(c, (uint8_t)(v ? c->psu | S2650_II : c->psu & ~S2650_II));
            break;
        case CHF_REG_CC:
            s2650_set_psl(c, (uint8_t)((c->psl & ~S2650_CC) | ((v & 3) << 6)));
            break;
        case CHF_REG_IDC: case CHF_REG_RS: case CHF_REG_WC: case CHF_REG_OVF:
        case CHF_REG_COM: case CHF_REG_C:
        {
            static const uint8_t bit[] = { S2650_IDC, S2650_RS, S2650_WC, S2650_OVF, S2650_COM, S2650_C };
            const uint8_t b = bit[reg - CHF_REG_IDC];
            s2650_set_psl(c, (uint8_t)(v ? c->psl | b : c->psl & ~b));
            break;
        }
        default: break;
        }
    }
    pthread_mutex_lock(&d->lock);
    d->reason_addr = cur_pc(m);
    pthread_mutex_unlock(&d->lock);
    chf_host_unlock();
    bump(d);
}

/* ---- I/O ------------------------------------------------------------------------ */

void chfdebug_io_get(chfdebug *d, chfdebug_io *o)
{
    chf_machine_t *m;

    (void)d;
    memset(o, 0, sizeof *o);
    chf_host_lock();
    m = chfdebug_machine();
    if (m)
    {
        o->machine = m->machine;
        o->is_pal = m->is_pal;
        o->keys = m->keys;
        o->sticks[0] = m->joy[0];
        o->sticks[1] = m->joy[1];
        for (int s = 0; s < 2; s++)
            for (int a = 0; a < 2; a++)
            {
                o->disc[s][a] = m->disc[s][a];
                o->pot[s][a] = m->uvi.in.pot[s][a];
            }
        for (int i = 0; i < 8; i++)
            o->pad[i] = m->uvi.in.pad[i];
        o->panel = m->uvi.in.panel;
        o->reset_held = m->reset_held;
        o->sense = uvi_vblank(&m->uvi);
        o->flag = (m->cpu.psu & S2650_FO) != 0;
        o->cart_type = m->cart ? m->cart->type : 0;
        o->audio_samples = m->audio_samples;
    }
    chf_host_unlock();
}

/* ---- memory ------------------------------------------------------------------- */

static int space_size_locked(chf_machine_t *m, int space)
{
    switch (space)
    {
    case CHF_SPACE_PROG: return PROG_SIZE;
    case CHF_SPACE_UVI:  return UVI_REGS;
    case CHF_SPACE_CART:
        if (!m || !m->cart)
            return 0;
        return m->fujinet ? CHFMAP_STORE : (int)m->cart->rom_size;
    default:             return 0;
    }
}

int chfdebug_space_size(chfdebug *d, int space)
{
    int n;
    (void)d;
    chf_host_lock();
    n = space_size_locked(chfdebug_machine(), space);
    chf_host_unlock();
    return n;
}

int chfdebug_read(chfdebug *d, int space, uint32_t addr, uint8_t *dst, int n)
{
    chf_machine_t *m;
    (void)d;
    if (space < 0 || space > 2)
        return 0;
    chf_host_lock();
    m = chfdebug_machine();
    if (m)
    {
        const uint32_t size = (uint32_t)space_size_locked(m, space);
        for (int i = 0; i < n; i++)
        {
            const uint32_t a = addr + (uint32_t)i;
            dst[i] = a < size ? chf_machine_peek(m, space, a) : 0xff;
        }
    }
    chf_host_unlock();
    return m ? n : 0;
}

void chfdebug_write(chfdebug *d, int space, uint32_t addr, uint8_t value)
{
    chf_machine_t *m;
    if (space < 0 || space > 2)
        return;
    chf_host_lock();
    m = chfdebug_machine();
    if (m && addr < (uint32_t)space_size_locked(m, space))
        chf_machine_poke(m, space, addr, value);
    chf_host_unlock();
    if (m)
        bump(d);
}

/* ---- labels ---------------------------------------------------------------------- */

int chfdebug_label_address(chfdebug *d, const char *label)
{
    int a;
    if (!d)
        return -1;
    chf_host_lock();
    a = symtab_find(&d->syms, label);
    chf_host_unlock();
    return a;
}

int chfdebug_address_label(chfdebug *d, uint16_t addr, char *dst, int dstsz)
{
    const char *l;
    int n;
    if (!d || !dst || dstsz <= 0)
        return 0;
    chf_host_lock();
    l = symtab_lookup(&d->syms, addr, SYM_BANK_ANY);
    n = snprintf(dst, (size_t)dstsz, "%s", l ? l : "");
    chf_host_unlock();
    return n;
}

int chfdebug_set_label(chfdebug *d, uint16_t addr, const char *label)
{
    if (!d)
        return -1;
    chf_host_lock();
    symtab_remove_at(&d->syms, addr);
    if (label && *label)
        symtab_add(&d->syms, addr, SYM_BANK_ANY, label, 0);
    chf_host_unlock();
    bump(d);
    return 0;
}

int chfdebug_load_symbols(chfdebug *d, const char *path, char *msg, int msgsz)
{
    int n;
    if (!d)
        return -1;
    if (!path || !*path)
    {
        static const char *const exts[] = { ".map", ".lst", ".sym", NULL };
        char cand[1100];
        const char *dot;
        size_t stem;

        if (!d->cart_path[0])
        {
            if (msg && msgsz > 0)
                snprintf(msg, (size_t)msgsz, "No cartridge file is open; choose a symbol file.");
            return -1;
        }
        dot = strrchr(d->cart_path, '.');
        stem = dot ? (size_t)(dot - d->cart_path) : strlen(d->cart_path);
        for (int i = 0; exts[i]; i++)
        {
            FILE *f;
            snprintf(cand, sizeof cand, "%.*s%s", (int)stem, d->cart_path, exts[i]);
            f = fopen(cand, "r");
            if (f)
            {
                fclose(f);
                chf_host_lock();
                n = symtab_load(&d->syms, cand, msg, msgsz);
                chf_host_unlock();
                bump(d);
                return n;
            }
        }
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "No .map, .lst or .sym next to %s", d->cart_path);
        return -1;
    }
    chf_host_lock();
    n = symtab_load(&d->syms, path, msg, msgsz);
    chf_host_unlock();
    bump(d);
    return n;
}

/* ---- disassembly ------------------------------------------------------------------ */

/* Decode one instruction; *target is the 15-bit place a direct branch goes to
 * (-1 for any other). */
static int decode_at(chf_machine_t *m, uint16_t addr, char *text, size_t textsz, uint32_t *flags,
                     int *target)
{
    uint8_t ob[3];
    int len;

    ob[0] = pbyte(m, addr);
    ob[1] = pbyte(m, next_in_page(addr, 1));
    ob[2] = pbyte(m, dasm2650_operand2_addr(addr));
    len = dasm2650_flags(addr, ob, text, textsz, flags);
    *target = dasm2650_target(addr, ob);
    return len;
}

static void fill_line(chfdebug *d, chf_machine_t *m, uint16_t addr, chfdebug_line *l, uint16_t pc)
{
    uint32_t flags;
    int target;
    const char *lab;

    memset(l, 0, sizeof *l);
    addr &= 0x7fff;
    l->length = decode_at(m, addr, l->disasm, sizeof l->disasm, &flags, &target);
    l->address = addr;
    l->is_pc = addr == pc;
    l->has_breakpoint = d->map_exec[addr] != 0;
    for (int i = 0, o = 0; i < l->length && o < (int)sizeof l->bytes - 3; i++)
        o += snprintf(l->bytes + o, sizeof l->bytes - (size_t)o, "%s%02X", i ? " " : "",
                      pbyte(m, next_in_page(addr, i)));
    lab = symtab_lookup(&d->syms, addr, SYM_BANK_ANY);
    if (lab)
        snprintf(l->label, sizeof l->label, "%s", lab);
    if (target >= 0)
    {
        const char *t = symtab_lookup(&d->syms, (uint16_t)target, SYM_BANK_ANY);
        if (t)
            snprintf(l->comment, sizeof l->comment, "%s", t);
    }
}

int chfdebug_disassemble(chfdebug *d, uint16_t addr, chfdebug_line *out, int max, int *pc_line)
{
    chf_machine_t *m;
    uint16_t pc;
    int n = 0;

    if (pc_line)
        *pc_line = -1;
    if (!d)
        return 0;
    chf_host_lock();
    m = chfdebug_machine();
    if (m)
    {
        pc = cur_pc(m);
        addr &= 0x7fff;
        while (n < max)
        {
            fill_line(d, m, addr, &out[n], pc);
            if (out[n].is_pc && pc_line)
                *pc_line = n;
            addr = next_in_page(addr, out[n].length);
            n++;
        }
    }
    chf_host_unlock();
    return n;
}

static int insn_len(chf_machine_t *m, uint16_t addr)
{
    return dasm2650_length(pbyte(m, addr));
}

static int row_address_locked(chf_machine_t *m, uint16_t addr, int rows)
{
    addr &= 0x7fff;
    if (!m || rows == 0)
        return addr;
    if (rows > 0)
    {
        for (int i = 0; i < rows; i++)
            addr = next_in_page(addr, insn_len(m, addr));
        return addr;
    }
    /* Backwards: the longest run of instructions that lands exactly on
     * addr, from as far back as the rows could take. Instructions are one to
     * three bytes, so `rows` rows are at most 3 * rows back. */
    rows = -rows;
    for (int back = rows * 3; back >= 1; back--)
    {
        uint16_t starts[256];
        int count = 0;
        uint16_t a = (uint16_t)((addr & 0x6000) | ((addr - back) & 0x1fff));
        while (count < 256)
        {
            const int dist = (addr - a) & 0x1fff;
            if (dist == 0 || dist > back)
                break;
            starts[count++] = a;
            a = next_in_page(a, insn_len(m, a));
        }
        if (a == addr && count >= rows)
            return starts[count - rows];
    }
    return (addr & 0x6000) | ((addr - rows) & 0x1fff);
}

int chfdebug_row_address(chfdebug *d, uint16_t addr, int rows)
{
    int a;
    (void)d;
    chf_host_lock();
    a = row_address_locked(chfdebug_machine(), addr, rows);
    chf_host_unlock();
    return a;
}

/* ---- the cartridge ----------------------------------------------------------------- */

static void cart_get_locked(chf_machine_t *m, chfdebug_cart *o)
{
    const chf_cart_t *c;

    memset(o, 0, sizeof *o);
    if (!m)
        return;
    c = m->cart;
    o->present = c != NULL;
    if (!c)
    {
        o->type_name = chf_cart_type_name(CHF_CART_NONE);
        return;
    }
    o->type = c->type;
    o->type_name = chf_cart_type_name(c->type);
    o->rom_size = c->rom_size;
    if (m->fujinet)
    {
        chf_cart_status_t st;
        chf_fujinet_status(m->fujinet, &st);
        o->type = CHF_CART_FUJINET;
        o->type_name = chf_cart_type_name(CHF_CART_FUJINET);
        o->link_up = st.link_up;
        o->link_off = st.link_off;
        o->mailbox = st.mailbox;
        o->app_live = st.app_live;
        o->booted_game = st.booted_game;
        o->armed = st.armed;
        o->staged_app = st.staged_app;
        o->staged_mailbox = st.staged_mailbox;
        o->live_size = st.live_size;
        o->live_crc = st.live_crc;
        o->staged_size = st.staged_size;
        o->staged_crc = st.staged_crc;
        o->swaps = (int)st.swaps;
        o->nbanks = (int)st.nbanks;
        o->fbank = (int)st.bank;
        o->rom_size = st.live_size;
        o->boot_state = st.boot_state;
        o->boot_pct = st.boot_pct;
        o->boot_err = st.boot_err;
        o->ackseq = st.ackseq;
        o->status = st.status;
        o->last_error = st.err;
        o->reply_cmd = st.reply_cmd;
        o->rxlen = st.rxlen;
        o->queue_depth = st.queue;
        chf_fujinet_mailbox(m->fujinet, o->mailbox_page);
    }
    else
        for (unsigned i = 0; i < sizeof o->mailbox_page; i++)
            o->mailbox_page[i] = pbyte(m, (uint16_t)(0x2b00 + i));
}

void chfdebug_cart_get(chfdebug *d, chfdebug_cart *o)
{
    (void)d;
    chf_host_lock();
    cart_get_locked(chfdebug_machine(), o);
    chf_host_unlock();
}

int chfdebug_cart_info(chfdebug *d, char *dst, int dstsz)
{
    chfdebug_cart c;
    int n;

    chfdebug_cart_get(d, &c);
    if (!c.present)
        return snprintf(dst, (size_t)dstsz, "No cartridge");
    if (c.type == CHF_CART_FUJINET)
    {
        const char *what = !c.booted_game ? "CONFIG" : c.mailbox ? "app (mailbox live)" : "game image";
        const char *link = c.link_off ? "off" : c.link_up ? "up" : "down";
        if (c.live_size)
            n = snprintf(dst, (size_t)dstsz, "FujiNet cartridge: %s, link %s, %u bytes", what, link,
                         c.live_size);
        else
            n = snprintf(dst, (size_t)dstsz, "FujiNet cartridge: %s, link %s", what, link);
        return n < dstsz ? n : dstsz - 1;
    }
    n = snprintf(dst, (size_t)dstsz, "%s cartridge, %u bytes", c.type_name, c.rom_size);
    return n < dstsz ? n : dstsz - 1;
}

/* ---- FujiNet --------------------------------------------------------------------- */

int chfdebug_fujinet_trace(chfdebug *d, chfdebug_fuji_packet *out, int max)
{
    int n;
    if (!d)
        return 0;
    pthread_mutex_lock(&d->fuji_lock);
    n = fujitrace_read(&d->fuji, out, max);
    pthread_mutex_unlock(&d->fuji_lock);
    return n;
}

void chfdebug_fujinet_trace_clear(chfdebug *d)
{
    if (!d)
        return;
    pthread_mutex_lock(&d->fuji_lock);
    fujitrace_clear(&d->fuji);
    pthread_mutex_unlock(&d->fuji_lock);
}

int chfdebug_fujinet_state(chfdebug *d, char *dst, int dstsz)
{
    chfdebug_cart c;
    int n;

    chfdebug_cart_get(d, &c);
    if (c.type != CHF_CART_FUJINET)
        return snprintf(dst, (size_t)dstsz, "The cartridge in the slot is not the FujiNet cartridge.\n");
    n = snprintf(dst, (size_t)dstsz,
        "Link         %s\n"
        "Image        %s%s, %u bytes, CRC %08X\n"
        "\n"
        "Mailbox\n"
        "  ACKSEQ     $%02X\n"
        "  STATUS     $%02X  (link %d, busy %d)\n"
        "  last error %u\n"
        "  reply cmd  $%02X\n"
        "  RXLEN      %u\n"
        "  queue      %u\n"
        "\n"
        "Boot         state %d  %d%%\n"
        "             error %d\n"
        "Staged       %s%s  %u bytes, CRC %08X\n"
        "Swaps        %d\n",
        c.link_off ? "off (FujiNet is switched off)" : c.link_up ? "up" : "down",
        !c.booted_game ? "CONFIG" : c.mailbox ? "an app" : "a game", c.app_live ? " (APPBANK)" : "",
        c.live_size, c.live_crc,
        c.ackseq, c.status, c.status & 1, (c.status >> 1) & 1, c.last_error, c.reply_cmd,
        c.rxlen, c.queue_depth,
        c.boot_state, c.boot_pct, c.boot_err,
        c.staged_size ? (c.staged_mailbox ? "an app" : "a game") : "nothing",
        c.staged_size && c.armed ? ", armed" : "", c.staged_size, c.staged_crc, c.swaps);
    return n < dstsz ? n : dstsz - 1;
}

/* ---- breakpoints ---------------------------------------------------------------- */

int chfdebug_breakpoint_add(chfdebug *d, int type, int space, uint16_t start, uint16_t end,
                           const char *cond)
{
    chfdebug_breakpoint *b;
    char err[96];
    int id;

    if (!d || !type || (type & ~(CHFDEBUG_BP_EXEC | CHFDEBUG_BP_READ | CHFDEBUG_BP_WRITE)))
        return -1;
    if (space != CHF_SPACE_PROG)
        return -1;
    if (cond && *cond && expr_eval(cond, NULL, NULL, err, sizeof err) != 0)
        return -1;
    if (end < start)
    {
        uint16_t t = start;
        start = end;
        end = t;
    }
    if (start >= PROG_SIZE)
        return -1;
    if (end >= PROG_SIZE)
        end = PROG_SIZE - 1;
    chf_host_lock();
    pthread_mutex_lock(&d->lock);
    if (d->nbp >= MAX_BP)
    {
        pthread_mutex_unlock(&d->lock);
        chf_host_unlock();
        return -1;
    }
    b = &d->bp[d->nbp++];
    memset(b, 0, sizeof *b);
    b->id = id = d->next_id++;
    b->type = type;
    b->space = space;
    b->start = start;
    b->end = end;
    b->enabled = 1;
    snprintf(b->condition, sizeof b->condition, "%s", cond ? cond : "");
    rebuild_locked(d);
    bump(d);
    pthread_mutex_unlock(&d->lock);
    chf_host_unlock();
    return id;
}

void chfdebug_breakpoint_remove(chfdebug *d, int id)
{
    if (!d)
        return;
    chf_host_lock();
    pthread_mutex_lock(&d->lock);
    for (int i = 0; i < d->nbp; i++)
    {
        if (d->bp[i].id == id)
        {
            memmove(&d->bp[i], &d->bp[i + 1], (size_t)(d->nbp - i - 1) * sizeof d->bp[0]);
            d->nbp--;
            break;
        }
    }
    rebuild_locked(d);
    bump(d);
    pthread_mutex_unlock(&d->lock);
    chf_host_unlock();
}

void chfdebug_breakpoint_enable(chfdebug *d, int id, int enabled)
{
    if (!d)
        return;
    chf_host_lock();
    pthread_mutex_lock(&d->lock);
    for (int i = 0; i < d->nbp; i++)
        if (d->bp[i].id == id)
            d->bp[i].enabled = enabled != 0;
    rebuild_locked(d);
    bump(d);
    pthread_mutex_unlock(&d->lock);
    chf_host_unlock();
}

int chfdebug_breakpoint_list(chfdebug *d, chfdebug_breakpoint *out, int max)
{
    int n;
    if (!d)
        return 0;
    pthread_mutex_lock(&d->lock);
    n = d->nbp < max ? d->nbp : max;
    memcpy(out, d->bp, (size_t)n * sizeof *out);
    pthread_mutex_unlock(&d->lock);
    return n;
}

void chfdebug_breakpoint_clear(chfdebug *d)
{
    if (!d)
        return;
    chf_host_lock();
    pthread_mutex_lock(&d->lock);
    d->nbp = 0;
    rebuild_locked(d);
    bump(d);
    pthread_mutex_unlock(&d->lock);
    chf_host_unlock();
}

int chfdebug_breakpoint_check(chfdebug *d, uint16_t addr)
{
    return d && d->map_exec[addr & 0x7fff] != 0;
}

int chfdebug_breakpoint_toggle(chfdebug *d, uint16_t addr)
{
    int found = -1;
    if (!d)
        return 0;
    addr &= 0x7fff;
    pthread_mutex_lock(&d->lock);
    for (int i = 0; i < d->nbp; i++)
        if (d->bp[i].type == CHFDEBUG_BP_EXEC && d->bp[i].start == addr && d->bp[i].end == addr)
        {
            found = d->bp[i].id;
            break;
        }
    pthread_mutex_unlock(&d->lock);
    if (found >= 0)
    {
        chfdebug_breakpoint_remove(d, found);
        return 0;
    }
    return chfdebug_breakpoint_add(d, CHFDEBUG_BP_EXEC, CHF_SPACE_PROG, addr, addr, NULL) >= 0;
}

/* ---- trace ---------------------------------------------------------------------- */

void chfdebug_trace_enable(chfdebug *d, int on)
{
    if (d)
    {
        atomic_store(&d->trace_on, on != 0);
        bump(d);
    }
}

int chfdebug_trace_enabled(chfdebug *d)
{
    return d && atomic_load(&d->trace_on);
}

/* The ring is written by the instruction hook, under the run lock. */
int chfdebug_trace_read(chfdebug *d, chfdebug_trace *out, int max)
{
    int n;
    if (!d)
        return 0;
    chf_host_lock();
    n = d->ring_count < max ? d->ring_count : max;
    for (int i = 0; i < n; i++)
        out[i] = d->ring[(d->ring_head - 1 - i + TRACE_N) % TRACE_N];
    chf_host_unlock();
    return n;
}

void chfdebug_trace_clear(chfdebug *d)
{
    if (!d)
        return;
    chf_host_lock();
    d->ring_head = d->ring_count = 0;
    chf_host_unlock();
    bump(d);
}

/* ---- files ------------------------------------------------------------------------ */

static int fail_msg(char *msg, int msgsz, const char *text, const char *arg, const char *tail)
{
    if (msg && msgsz > 0)
        snprintf(msg, (size_t)msgsz, "%s%s%s", text, arg ? arg : "", tail ? tail : "");
    return -1;
}

static int write_bytes(const char *path, const uint8_t *data, size_t n, char *msg, int msgsz)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(data, 1, n, f) != n)
    {
        if (f)
            fclose(f);
        return fail_msg(msg, msgsz, "Cannot write ", path, NULL);
    }
    fclose(f);
    if (msg && msgsz > 0)
        snprintf(msg, (size_t)msgsz, "Saved %zu bytes to %s", n, path);
    return 0;
}

/* is the address backed by anything a listing should show? */
static int code_mapped(const chf_machine_t *m, unsigned a)
{
    if (a < 0x1000)
        return 1;
    return m->cart && a >= m->cart->extra_lo && a <= m->cart->extra_hi;
}

static void save_regs(chf_machine_t *m, FILE *f)
{
    chfdebug_cpu c;
    uvi_snapshot s;

    cpu_get_locked(m, &c);
    fprintf(f, "PC=%04X (page %04X iar %04X)  PSU=%02X [S=%d F=%d II=%d SP=%d]  "
               "PSL=%02X [CC=%d IDC=%d RS=%d WC=%d OVF=%d COM=%d C=%d]\n",
            c.pc, c.page, c.iar, c.psu, c.sense, c.flag, c.ii, c.sp, c.psl, c.cc, c.idc, c.rs,
            c.wc, c.ovf, c.com, c.c);
    fprintf(f, "R0=%02X", c.r0);
    for (int b = 0; b < 2; b++)
    {
        fprintf(f, "  bank %d:", b);
        for (int i = 1; i < 4; i++)
            fprintf(f, " R%d%s=%02X", i, b ? "'" : "", c.r[b][i]);
    }
    fprintf(f, "\nreturn stack (SP %d):", c.sp);
    for (int i = 0; i < 8; i++)
        fprintf(f, " %04X", c.ras[i]);
    fprintf(f, "\ncycles=%llu instructions=%llu frame=%u line=%d pixel=%d halted=%d irq=%d "
               "illegal=%u\n",
            (unsigned long long)c.cycles, (unsigned long long)c.instructions, c.frame, c.vpos,
            c.hpos, c.halted, c.irq_line, c.illegal_count);
    fprintf(f, "machine=%s\n", chf_machine_name(c.machine));

    uvi_snapshot_get(&m->uvi, &s);
    fprintf(f, "UVI line=%d charline=%d first=%d shift=%d rows=%d cell=%d graphics=%d "
               "multicolor=%d screen=%s chars=%s\n",
            s.line, s.charline, s.ypos, s.shift, s.lines26 ? 26 : 13, s.doublescan ? 16 : 8,
            s.graphics, s.multicolor, uvi_colour_name(s.screen_colour),
            uvi_colour_name(s.char_colour));
    for (int i = 0; i < 4; i++)
        fprintf(f, "OBJ%d x=%d y=%d colour=%s %s UDC=%02X\n", i, s.obj[i].x, s.obj[i].y,
                uvi_colour_name(s.obj[i].colour), s.obj[i].double_height ? "8x16" : "8x8", 0x38 + i);
    fprintf(f, "collisions chars=%02X objects=%02X  sound pitch=%d vol=%d tone=%d noise=%d\n",
            s.coll_bg, s.coll_spr, s.sound_freq, s.sound_volume, s.sound_on, s.sound_noise);
    fprintf(f, "I/O keys=%07llX sticks=%02X,%02X reset=%d\n", (unsigned long long)m->keys,
            m->joy[0], m->joy[1], m->reset_held);
}

static int save_locked(chfdebug *d, chf_machine_t *m, const char *kind, const char *path,
                       char *msg, int msgsz)
{
    if (!d || !m || !kind || !path)
        return fail_msg(msg, msgsz, "The machine is not running.", NULL, NULL);
    if (!strcmp(kind, "mailbox"))
    {
        chfdebug_cart c;
        cart_get_locked(m, &c);
        return write_bytes(path, c.mailbox_page, sizeof c.mailbox_page, msg, msgsz);
    }
    if (!strcmp(kind, "uvi"))
    {
        uint8_t buf[UVI_REGS];
        for (unsigned i = 0; i < sizeof buf; i++)
            buf[i] = chf_machine_peek(m, CHF_PEEK_UVI, i);
        return write_bytes(path, buf, sizeof buf, msg, msgsz);
    }
    if (!strcmp(kind, "font"))
        return write_bytes(path, &m->uvi.chars[0][0], sizeof m->uvi.chars, msg, msgsz);
    if (!strcmp(kind, "cart"))
    {
        const int size = space_size_locked(m, CHF_SPACE_CART);
        uint8_t *buf = malloc((size_t)(size > 0 ? size : 1));
        int rc;

        if (!buf)
            return fail_msg(msg, msgsz, "Out of memory.", NULL, NULL);
        for (int i = 0; i < size; i++)
            buf[i] = chf_machine_peek(m, CHF_PEEK_CART, (uint32_t)i);
        rc = write_bytes(path, buf, (size_t)size, msg, msgsz);
        free(buf);
        return rc;
    }
    if (!strcmp(kind, "screen"))
    {
        FILE *f = fopen(path, "wb");
        if (!f)
            return fail_msg(msg, msgsz, "Cannot write ", path, NULL);
        fprintf(f, "P6\n%d %d\n255\n", CHF_FB_WIDTH, CHF_FB_HEIGHT);
        for (int i = 0; i < CHF_FB_WIDTH * CHF_FB_HEIGHT; i++)
        {
            uint8_t px[3] = { (uint8_t)(m->fb[i] >> 16), (uint8_t)(m->fb[i] >> 8), (uint8_t)m->fb[i] };
            fwrite(px, 1, 3, f);
        }
        fclose(f);
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "Saved %s", path);
        return 0;
    }
    if (!strcmp(kind, "dis") || !strcmp(kind, "regs"))
    {
        FILE *f = fopen(path, "w");
        if (!f)
            return fail_msg(msg, msgsz, "Cannot write ", path, NULL);
        if (!strcmp(kind, "dis"))
        {
            const uint16_t pc = cur_pc(m);
            unsigned a = 0;
            while (a < PROG_SIZE)
            {
                chfdebug_line l;
                if (!code_mapped(m, a))
                {
                    a++;
                    continue;
                }
                fill_line(d, m, (uint16_t)a, &l, pc);
                if (l.label[0])
                    fprintf(f, "%s:\n", l.label);
                fprintf(f, "  %04X  %-9s %-22s%s%s\n", l.address, l.bytes, l.disasm,
                        l.comment[0] ? "; " : "", l.comment);
                a += (unsigned)l.length;
            }
        }
        else
            save_regs(m, f);
        fclose(f);
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "Saved %s", path);
        return 0;
    }
    return fail_msg(msg, msgsz, "Unknown save kind ", kind, " (dis, mailbox, uvi, font, cart, screen, regs)");
}

int chfdebug_save(chfdebug *d, const char *kind, const char *path, char *msg, int msgsz)
{
    int rc;
    chf_host_lock();
    rc = save_locked(d, chfdebug_machine(), kind, path, msg, msgsz);
    chf_host_unlock();
    return rc;
}
