/*
 * debugger.c -- the chfdebug engine (core/include/chfdebug.h).
 *
 * Built on the core's two hooks: the machine's instruction hook
 * (chf_internal.h: called at every instruction boundary BEFORE the
 * instruction, with the opcode already fetched, so a breakpoint on any
 * address hits) and its bus hook (core/chf/host.h), fired on the access kinds
 * the engine asks for: PROG-space data reads and writes only while a
 * breakpoint of that kind exists; port reads and writes and VRAM stores for
 * as long as the engine is attached (the I/O and sound logs, the VRAM
 * breakpoints). Attached, the instruction hook checks the stop request, the
 * breakpoints and the step mode; stopping PARKS the emulation thread right
 * there (a condition variable) until a window resumes it -- the CPU's own
 * progress is what stops, there is no "paused" flag a run loop polls.
 * Detached, neither hook is installed and the video bookkeeping is freed, so
 * a session that never opens the debugger pays nothing. The design and the
 * feature set (prompt, conditions, breakpoints, scanline and frame steps,
 * symbols, save) are the Odyssey 2, Arcadia and Studio II siblings'.
 *
 * The F8 has five address spaces here (PROG, SCRATCH, VRAM, CART, IO) and the
 * CPU has no register write hook, so a SCRATCH breakpoint compares the 64
 * registers at each instruction boundary (it sees a change, not a store of
 * the value already there). The F8 never interrupts on this console.
 *
 * Threading. Everything public is called from UI threads; the hooks run on
 * the emulation thread. Every public call that reads or edits the machine
 * holds the host's run lock (chf_host_lock), which the emulation thread
 * holds while it runs a frame and lets go of between frames and while it is
 * parked here -- so a window refreshing live values while the machine runs
 * waits a moment instead of tearing, and a stopped machine is simply the
 * UI's. Lock order: the run lock, then d->lock (the hooks run with the run
 * lock held and take d->lock). The symbol table, the event rings and the
 * video bookkeeping are only ever touched under the run lock.
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
#include "fuji_mailbox.h"
#include "fujitcp_host.h"
#include "host.h"

#define MAX_BP     128
#define TRACE_N    4096
#define ROMC_N     4096
#define IOEV_N     4096
#define SNDEV_N    1024
#define CARTEV_N   1024
#define PROG_SIZE  0x10000

enum { MODE_RUN = 0, MODE_STEP, MODE_OVER, MODE_OUT, MODE_RUNTO, MODE_SCANLINE, MODE_FRAME };

/* an access a breakpoint is matched against, and a condition sees */
typedef struct {
    int type;                    /* CHFDEBUG_BP_* */
    unsigned addr;               /* PROG address, port, VRAM offset or register */
    unsigned value;
    int row, col;                /* VRAM */
} access_t;

struct chfdebug {
    pthread_mutex_t lock;
    pthread_cond_t cond;

    atomic_int attached;
    atomic_int stopped;          /* the emulation thread is parked */
    atomic_int stop_req;
    atomic_int mode;
    long count;                  /* step n / scanline n */
    uint16_t target;             /* run to; step over: the return address */
    uint8_t target_k;            /* step over: the K stack pointer (r59) it returns with */
    int out_depth;               /* step out: calls entered since the step began */
    uint8_t out_k[18];           /* ... and r59 as each of them began (level 0: as the step began) */
    int out_pending;             /* step out: the leaving instruction just ran */
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
    uint8_t map_in[256], map_out[256];
    int have_vram_bp, have_scratch_bp;
    atomic_int break_pending;
    char break_reason[128];

    /* the access a condition sees as `value` / `addr` / `row` / `col` / `port` */
    access_t acc;
    int in_access;

    /* the instruction the hook last saw (the writer of the access in flight),
     * and the registers as that boundary left them (SCRATCH breakpoints) */
    uint16_t cur_pc;
    uint8_t snap[64];
    int snap_valid;
    int in_edit;                 /* a debugger edit is running: no logging, no breakpoints */

    atomic_int trace_on;
    chfdebug_trace ring[TRACE_N];
    int ring_head, ring_count;

    chfdebug_io_event io_ev[IOEV_N];
    uint64_t io_total;
    chfdebug_sound_event snd_ev[SNDEV_N];
    uint64_t snd_total;
    chfdebug_cart_event cart_ev[CARTEV_N];
    uint64_t cart_total;
    uint64_t cart_cursor;        /* into the cartridge's own ring */
    uint64_t cart_pull_frame;

    int romc_on;
    chfdebug_romc romc_ring[ROMC_N];
    int romc_head, romc_count;

    symtab syms;
    char cart_path[1024];

    /* the FujiNet trace, fed on the transport's thread */
    pthread_mutex_t fuji_lock;
    fujitrace fuji;
};

static chfdebug *g_traced;       /* the engine the transport's tap feeds */
static chfdebug *g_hooked;       /* the engine the CPU's hooks call */

chf_machine_t *chfdebug_machine(void)
{
    return chf_host_is_running() ? chf_host_machine() : NULL;
}

static void bump(chfdebug *d)
{
    atomic_fetch_add(&d->generation, 1);
}

uint16_t chfdebug_machine_pc(const chf_machine_t *m)
{
    return f8_pc(&m->cpu);
}

static uint8_t pbyte(chf_machine_t *m, uint16_t a)
{
    return chf_machine_peek(m, CHF_PEEK_PROG, a);
}

static uint32_t frame_of_cycle(const chf_machine_t *m, uint64_t cycle)
{
#ifdef __SIZEOF_INT128__
    return (uint32_t)(((unsigned __int128)cycle * m->cycle_as) / m->frame_as);
#else
    return (uint32_t)((long double)cycle * (long double)m->cycle_as / (long double)m->frame_as);
#endif
}

/* The registers as they are now are the baseline a SCRATCH breakpoint compares
 * the next boundary with. Run lock held. */
static void snap_refresh(chfdebug *d)
{
    chf_machine_t *m = chfdebug_machine();

    if (m)
    {
        memcpy(d->snap, m->cpu.r, sizeof d->snap);
        d->snap_valid = 1;
    }
    else
        d->snap_valid = 0;
}

/* ---- expressions ---------------------------------------------------------- */

const char *const chfdebug_reg_names[] = {
    "a", "w", "is", "isar", "pc", "pc0", "pc1", "dc0", "dc1", "j", "h", "k", "q",
    "hu", "hl", "ku", "kl", "qu", "ql", "icb", "o", "z", "c", "s",
    "vpos", "hpos", "frame", "cycles", "value", "addr", "row", "col", "port", NULL,
};

static uint8_t expr_read(void *user, int space, uint32_t addr)
{
    chf_machine_t *m = chfdebug_machine();
    (void)user;
    if (!m || space < 0 || space > CHF_SPACE_IO)
        return 0xff;
    if (addr >= chf_machine_space_size(m, space))
        return 0xff;
    return chf_machine_peek(m, space, addr);
}

/* "r0".."r63" -> n, else -1 */
static int scratch_name(const char *name)
{
    char *end;
    long n;

    if (name[0] != 'r' || !isdigit((unsigned char)name[1]))
        return -1;
    n = strtol(name + 1, &end, 10);
    return (*end == '\0' && n >= 0 && n < 64) ? (int)n : -1;
}

static int expr_name(void *user, const char *raw, long *out)
{
    chfdebug *d = user;
    chf_machine_t *m = chfdebug_machine();
    char name[64];
    size_t i;
    int n;

    for (i = 0; raw[i] && i + 1 < sizeof name; i++)
        name[i] = (char)tolower((unsigned char)raw[i]);
    name[i] = '\0';

    if (d->in_access)
    {
        const access_t *a = &d->acc;
        if (!strcmp(name, "value")) { *out = (long)a->value; return 1; }
        if (!strcmp(name, "addr"))  { *out = (long)a->addr; return 1; }
        if (!strcmp(name, "row") && a->type == CHFDEBUG_BP_VRAM) { *out = a->row; return 1; }
        if (!strcmp(name, "col") && a->type == CHFDEBUG_BP_VRAM) { *out = a->col; return 1; }
        if (!strcmp(name, "port") && (a->type & (CHFDEBUG_BP_IN | CHFDEBUG_BP_OUT))) { *out = (long)a->addr; return 1; }
    }
    if (m)
    {
        const f8_t *c = &m->cpu;
        const uint8_t *r = c->r;

        if ((n = scratch_name(name)) >= 0) { *out = r[n]; return 1; }
        if (!strcmp(name, "a"))    { *out = c->a; return 1; }
        if (!strcmp(name, "w"))    { *out = c->w; return 1; }
        if (!strcmp(name, "is") || !strcmp(name, "isar")) { *out = c->is & 63; return 1; }
        if (!strcmp(name, "pc"))   { *out = f8_pc(c); return 1; }
        if (!strcmp(name, "pc0"))  { *out = c->pc0; return 1; }
        if (!strcmp(name, "pc1"))  { *out = c->pc1; return 1; }
        if (!strcmp(name, "dc0"))  { *out = c->dc0; return 1; }
        if (!strcmp(name, "dc1"))  { *out = c->dc1; return 1; }
        if (!strcmp(name, "j"))    { *out = r[9]; return 1; }
        if (!strcmp(name, "h"))    { *out = (r[10] << 8) | r[11]; return 1; }
        if (!strcmp(name, "k"))    { *out = (r[12] << 8) | r[13]; return 1; }
        if (!strcmp(name, "q"))    { *out = (r[14] << 8) | r[15]; return 1; }
        if (!strcmp(name, "hu"))   { *out = r[10]; return 1; }
        if (!strcmp(name, "hl"))   { *out = r[11]; return 1; }
        if (!strcmp(name, "ku"))   { *out = r[12]; return 1; }
        if (!strcmp(name, "kl"))   { *out = r[13]; return 1; }
        if (!strcmp(name, "qu"))   { *out = r[14]; return 1; }
        if (!strcmp(name, "ql"))   { *out = r[15]; return 1; }
        if (!strcmp(name, "icb"))  { *out = (c->w & F8_ICB) != 0; return 1; }
        if (!strcmp(name, "o"))    { *out = (c->w & F8_OVR) != 0; return 1; }
        if (!strcmp(name, "z"))    { *out = (c->w & F8_ZERO) != 0; return 1; }
        if (!strcmp(name, "c"))    { *out = (c->w & F8_CARRY) != 0; return 1; }
        if (!strcmp(name, "s"))    { *out = (c->w & F8_SIGN) != 0; return 1; }
        if (!strcmp(name, "vpos") || !strcmp(name, "hpos"))
        {
            int vp, hp;
            chf_machine_beam(m, &vp, &hp);
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
    const f8_t *c = &m->cpu;
    int vp, hp;

    t->pc = pc;
    t->a = c->a;
    t->w = c->w;
    t->isar = (uint8_t)(c->is & 63);
    t->dc0 = c->dc0;
    t->bytes[0] = pbyte(m, pc);
    t->len = (uint8_t)dasmf8_length(t->bytes[0]);
    t->bytes[1] = t->len > 1 ? pbyte(m, (uint16_t)(pc + 1)) : 0;
    t->bytes[2] = t->len > 2 ? pbyte(m, (uint16_t)(pc + 2)) : 0;
    t->cycles = c->cycles;
    chf_machine_beam(m, &vp, &hp);
    t->vpos = (int16_t)vp;
    t->hpos = (int16_t)hp;
    d->ring_head = (d->ring_head + 1) % TRACE_N;
    if (d->ring_count < TRACE_N)
        d->ring_count++;
}

static void romc_record(void *ctx, const f8_t *c, uint8_t romc, uint8_t dbus)
{
    chfdebug *d = g_hooked;
    chfdebug_romc *e;

    (void)ctx;
    if (!d || !d->romc_on)
        return;
    e = &d->romc_ring[d->romc_head];
    e->cycles = c->cycles;
    e->pc0 = c->pc0;
    e->romc = romc;
    e->dbus = dbus;
    d->romc_head = (d->romc_head + 1) % ROMC_N;
    if (d->romc_count < ROMC_N)
        d->romc_count++;
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
    /* what a window edited while parked is not a write by the program */
    memcpy(d->snap, m->cpu.r, sizeof d->snap);
    d->snap_valid = 1;
}

/* Does `b` cover the access? */
static int covers(const chfdebug_breakpoint *b, const access_t *a)
{
    switch (a->type)
    {
    case CHFDEBUG_BP_VRAM:
    {
        const int r0 = b->start / CF_VRAM_COLS, c0 = b->start % CF_VRAM_COLS;
        const int r1 = b->end / CF_VRAM_COLS, c1 = b->end % CF_VRAM_COLS;
        const int rlo = r0 < r1 ? r0 : r1, rhi = r0 < r1 ? r1 : r0;
        const int clo = c0 < c1 ? c0 : c1, chi = c0 < c1 ? c1 : c0;
        return a->row >= rlo && a->row <= rhi && a->col >= clo && a->col <= chi;
    }
    default:
        return a->addr >= b->start && a->addr <= b->end;
    }
}

static void access_reason(const access_t *a, char *dst, size_t n)
{
    switch (a->type)
    {
    case CHFDEBUG_BP_READ:
        snprintf(dst, n, "read prog $%04X = $%02X", a->addr, a->value & 0xff);
        break;
    case CHFDEBUG_BP_WRITE:
        snprintf(dst, n, "write prog $%04X = $%02X", a->addr, a->value & 0xff);
        break;
    case CHFDEBUG_BP_IN:
        snprintf(dst, n, "in port %u = $%02X", a->addr, a->value & 0xff);
        break;
    case CHFDEBUG_BP_OUT:
        snprintf(dst, n, "out port %u = $%02X", a->addr, a->value & 0xff);
        break;
    case CHFDEBUG_BP_VRAM:
        snprintf(dst, n, "VRAM write row %d col %d = %u", a->row, a->col, a->value & 3);
        break;
    default:
        snprintf(dst, n, "scratch write r%u = $%02X", a->addr, a->value & 0xff);
        break;
    }
}

/* One breakpoint check for an access (d->lock and the run lock held).
 * Returns 1 and arms the stop when one hits. */
static int hit_check(chfdebug *d, const access_t *a)
{
    int hit = 0;

    if (atomic_load(&d->break_pending))
        return 1;
    d->acc = *a;
    d->in_access = 1;
    for (int i = 0; i < d->nbp; i++)
    {
        chfdebug_breakpoint *b = &d->bp[i];
        if (!b->enabled || !(b->type & a->type))
            continue;
        if (!covers(b, a))
            continue;
        if (!cond_true(d, b))
            continue;
        b->hits++;
        access_reason(a, d->break_reason, sizeof d->break_reason);
        atomic_store(&d->break_pending, 1);
        hit = 1;
        break;
    }
    d->in_access = 0;
    return hit;
}

/* The cartridge's own event ring, drained into ours. */
static void cart_pull(chfdebug *d, chf_machine_t *m)
{
    chf_fj_event_t ev[64];
    int n;

    if (!m->fujinet)
        return;
    while ((n = chf_fujinet_events(m->fujinet, &d->cart_cursor, ev, 64)) > 0)
    {
        for (int i = 0; i < n; i++)
        {
            chfdebug_cart_event *o = &d->cart_ev[d->cart_total % CARTEV_N];
            o->cycle = ev[i].cycle;
            o->frame = frame_of_cycle(m, ev[i].cycle);
            o->pc = ev[i].pc;
            o->kind = ev[i].kind;
            o->reg = ev[i].reg;
            o->value = ev[i].value;
            d->cart_total++;
        }
    }
    d->cart_pull_frame = m->frames_done;
}

/* SCRATCH breakpoints: the registers the last instruction changed */
static void scratch_check(chfdebug *d, chf_machine_t *m)
{
    if (!d->snap_valid)
    {
        memcpy(d->snap, m->cpu.r, sizeof d->snap);
        d->snap_valid = 1;
        return;
    }
    if (memcmp(d->snap, m->cpu.r, sizeof d->snap) == 0)
        return;
    pthread_mutex_lock(&d->lock);
    for (unsigned r = 0; r < 64; r++)
    {
        access_t a;
        if (d->snap[r] == m->cpu.r[r])
            continue;
        a.type = CHFDEBUG_BP_SCRATCH;
        a.addr = r;
        a.value = m->cpu.r[r];
        a.row = a.col = 0;
        if (hit_check(d, &a))
            break;
    }
    pthread_mutex_unlock(&d->lock);
    memcpy(d->snap, m->cpu.r, sizeof d->snap);
}

/* step out: a call is entered; remember where the K stack stood */
static uint8_t *out_k_at(chfdebug *d)
{
    return &d->out_k[d->out_depth < 17 ? d->out_depth : 17];
}

static void out_enter(chfdebug *d, uint8_t k)
{
    d->out_depth++;
    *out_k_at(d) = k;
}

/* Does the step mode say stop here? Fills why. */
static int mode_stops(chfdebug *d, chf_machine_t *m, uint16_t pc, const char **why)
{
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
        if (pc == d->target && m->cpu.r[59] == d->target_k)
        {
            *why = "step over";
            return 1;
        }
        break;
    case MODE_OUT:
    {
        const uint8_t op = pbyte(m, pc);
        const uint8_t k = m->cpu.r[59] & 63;

        if (d->out_pending)
        {
            /* the POP (or the jump through Q or K) has run: this is the
             * caller's next instruction */
            d->out_pending = 0;
            *why = "step out";
            return 1;
        }
        if (op == 0x28)                          /* PI: one deeper */
            out_enter(d, k);
        else if (op == 0x0c)
        {
            /* PK is a call through K -- unless the routine has popped its K
             * stack back to where it began and K holds the way home: the
             * idiom `PI POPK / PK` returns */
            const int returns = k < *out_k_at(d) || (d->out_depth > 0 && k == *out_k_at(d));

            if (!returns)
                out_enter(d, k);
            else if (d->out_depth > 0)
                d->out_depth--;
            else
                d->out_pending = 1;
        }
        else if (op == 0x1c)                     /* POP: one shallower, or out */
        {
            if (d->out_depth > 0)
                d->out_depth--;
            else
                d->out_pending = 1;
        }
        else if (op == 0x0d && d->out_depth == 0)   /* LR P0,Q: the routine leaves by a jump */
            d->out_pending = 1;
        break;
    }
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
        chf_machine_beam(m, &vp, &hp);
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

static void on_instruction(chfdebug *d, chf_machine_t *m)
{
    const uint16_t pc = f8_pc(&m->cpu);
    char why[128];
    const char *mode_why = NULL;
    int stop = 0;

    if (d->have_scratch_bp)
        scratch_check(d, m);
    d->cur_pc = pc;
    if (m->frames_done != d->cart_pull_frame)
        cart_pull(d, m);

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
    else if (d->map_exec[pc])
    {
        pthread_mutex_lock(&d->lock);
        for (int i = 0; i < d->nbp; i++)
        {
            chfdebug_breakpoint *b = &d->bp[i];
            if (b->enabled && (b->type & CHFDEBUG_BP_EXEC) && pc >= b->start && pc <= b->end)
            {
                d->acc.type = CHFDEBUG_BP_EXEC;
                d->acc.addr = pc;
                d->acc.value = pbyte(m, pc);
                d->in_access = 1;
                if (cond_true(d, b))
                {
                    b->hits++;
                    snprintf(why, sizeof why, "breakpoint: exec $%04X", pc);
                    stop = 1;
                }
                d->in_access = 0;
                if (stop)
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
    /* a poke or a PC change while stopped is simply what the opcode in the
     * CPU is: f8_set_pc refetched it */
}

/* the machine's instruction hook, on the emulation thread */
static void instr_hook(chf_machine_t *m, void *user)
{
    chfdebug *d = user;

    if (d)
        on_instruction(d, m);
}

static void bus_hook(chf_machine_t *m, void *user, int kind, uint16_t addr, uint8_t data)
{
    chfdebug *d = user;
    access_t a;

    if (d->in_edit)
        return;
    memset(&a, 0, sizeof a);
    a.value = data;
    switch (kind)
    {
    case CHF_BUS_MEMR:
    case CHF_BUS_MEMW:
        a.type = kind == CHF_BUS_MEMR ? CHFDEBUG_BP_READ : CHFDEBUG_BP_WRITE;
        a.addr = addr;
        if (atomic_load(&d->break_pending) || !(d->map_mem[addr] & a.type))
            return;
        break;
    case CHF_BUS_IOR:
    case CHF_BUS_IOW:
    {
        chfdebug_io_event *e = &d->io_ev[d->io_total % IOEV_N];

        e->cycle = m->cpu.cycles;
        e->frame = (uint32_t)m->frames_done;
        e->pc = d->cur_pc;
        e->port = (uint8_t)addr;
        e->value = data;
        e->is_write = kind == CHF_BUS_IOW;
        d->io_total++;
        if (kind == CHF_BUS_IOW && (addr & 0xff) == 5)
        {
            const int mode = (data >> 6) & 3;
            if (mode != m->snd.mode)
            {
                chfdebug_sound_event *s = &d->snd_ev[d->snd_total % SNDEV_N];
                s->cycle = m->cpu.cycles;
                s->frame = (uint32_t)m->frames_done;
                s->pc = d->cur_pc;
                s->old_mode = (uint8_t)m->snd.mode;
                s->mode = (uint8_t)mode;
                d->snd_total++;
            }
        }
        a.type = kind == CHF_BUS_IOR ? CHFDEBUG_BP_IN : CHFDEBUG_BP_OUT;
        a.addr = addr & 0xff;
        if (atomic_load(&d->break_pending) ||
            !(kind == CHF_BUS_IOR ? d->map_in : d->map_out)[a.addr])
            return;
        break;
    }
    case CHF_BUS_VRAM:
        /* the machine logged the store with PC0 - 1; for a two-byte OUT that is
         * the operand */
        chfdebug_vram_log_fix_pc(m, addr, d->cur_pc);
        if (!d->have_vram_bp || atomic_load(&d->break_pending))
            return;
        a.type = CHFDEBUG_BP_VRAM;
        a.addr = addr;
        a.row = addr / CF_VRAM_COLS;
        a.col = addr % CF_VRAM_COLS;
        break;
    default:
        return;
    }
    pthread_mutex_lock(&d->lock);
    hit_check(d, &a);
    pthread_mutex_unlock(&d->lock);
}

/* What the machine's bus hook must watch, from the enabled breakpoints: the
 * ports and the VRAM always (the logs), the PROG data accesses only on demand. */
static uint8_t watch_bits(chfdebug *d)
{
    uint8_t w = CHF_WATCH_IOR | CHF_WATCH_IOW | CHF_WATCH_VRAM;
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
    memset(d->map_in, 0, sizeof d->map_in);
    memset(d->map_out, 0, sizeof d->map_out);
    d->have_vram_bp = d->have_scratch_bp = 0;
    for (int i = 0; i < d->nbp; i++)
    {
        const chfdebug_breakpoint *b = &d->bp[i];
        if (!b->enabled)
            continue;
        if (b->type & CHFDEBUG_BP_EXEC)
            for (unsigned a = b->start; a <= b->end; a++)
                d->map_exec[a] = 1;
        if (b->type & (CHFDEBUG_BP_READ | CHFDEBUG_BP_WRITE))
            for (unsigned a = b->start; a <= b->end; a++)
                d->map_mem[a] |= (uint8_t)(b->type & (CHFDEBUG_BP_READ | CHFDEBUG_BP_WRITE));
        if (b->type & CHFDEBUG_BP_IN)
            for (unsigned a = b->start; a <= b->end && a < 256; a++)
                d->map_in[a] = 1;
        if (b->type & CHFDEBUG_BP_OUT)
            for (unsigned a = b->start; a <= b->end && a < 256; a++)
                d->map_out[a] = 1;
        if (b->type & CHFDEBUG_BP_VRAM)
            d->have_vram_bp = 1;
        if (b->type & CHFDEBUG_BP_SCRATCH)
            d->have_scratch_bp = 1;
    }
    snap_refresh(d);
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
    chf_machine_t *m;

    if (!chf_host_is_running())
        return;
    m = chf_host_machine();
    chf_machine_track(m, true);
    g_hooked = d;
    d->cur_pc = f8_pc(&m->cpu);
    snap_refresh(d);
    /* the logs are of what happens while attached */
    d->io_total = d->snd_total = d->cart_total = 0;
    /* only what happens from here on */
    d->cart_cursor = 0;
    if (m->fujinet)
    {
        chf_fj_event_t ev[64];
        while (chf_fujinet_events(m->fujinet, &d->cart_cursor, ev, 64) > 0)
            ;
    }
    d->cart_pull_frame = m->frames_done;
    if (d->romc_on)
        m->cpu.romc_hook = romc_record;
    chf_host_set_bus_hook(bus_hook, d, watch_bits(d));
    chf_host_set_instr_hook(instr_hook, d);
}

static void unhook(chfdebug *d)
{
    chf_machine_t *m;

    if (g_hooked == d)
        g_hooked = NULL;
    if (!chf_host_is_running())
        return;
    m = chf_host_machine();
    m->cpu.romc_hook = NULL;
    chf_host_set_instr_hook(NULL, NULL);
    chf_host_set_bus_hook(NULL, NULL, 0);
    chf_machine_track(m, false);
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
        d->in_edit = 0;
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
    return n < dstsz ? n : dstsz - 1;
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
        const uint16_t pc = f8_pc(&m->cpu);
        const uint8_t op = pbyte(m, pc);

        /* a PI or a PK comes back to the instruction after it, with the K
         * stack (r59) where it was -- which tells a deeper activation of the
         * same routine from the one called here. The BIOS's own push and pop
         * of K come back with it two entries up and down. */
        if (dasmf8_op_flags(op) & F8_DASM_CALL)
        {
            uint8_t ob[3] = { op, pbyte(m, (uint16_t)(pc + 1)), pbyte(m, (uint16_t)(pc + 2)) };
            char text[32];
            unsigned flags = 0;
            int target = -1;
            unsigned k = m->cpu.r[59];

            dasmf8(text, sizeof text, pc, ob, &flags, &target);
            if (op == 0x0c)
                target = (m->cpu.r[12] << 8) | m->cpu.r[13];       /* PK calls through K */
            d->target = (uint16_t)(pc + dasmf8_length(op));
            if (target == 0x0107)                                    /* PUSHK */
                k = ((k & 63) + 2) & 63;
            else if (target == 0x011e)                               /* POPK */
                k = ((k & 63) - 2) & 63;
            d->target_k = (uint8_t)k;
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
        /* the first POP at the depth this routine runs at (PI and PK enter a
         * deeper one, POP leaves it; so do PK and LR P0,Q when they jump home
         * with the K stack popped) ends the step; the hook sees the
         * instructions AFTER the one the machine is stopped at, so that one
         * is judged here */
        const uint8_t op = pbyte(m, f8_pc(&m->cpu));

        d->out_depth = 0;
        d->out_pending = 0;
        d->out_k[0] = m->cpu.r[59] & 63;
        if (op == 0x28 || op == 0x0c)
            out_enter(d, d->out_k[0]);
        else if (op == 0x1c || op == 0x0d)
            d->out_pending = 1;
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
        chf_machine_beam(m, &d->last_vpos, &hp);
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
    d->target = addr;
    go(d, MODE_RUNTO);
}

/* ---- CPU ---------------------------------------------------------------------- */

static void cpu_get_locked(chf_machine_t *m, chfdebug_cpu *o)
{
    const f8_t *c;
    int p;

    memset(o, 0, sizeof *o);
    o->illegal_pc = -1;
    o->k_depth = -1;
    o->bios = -1;
    if (!m)
        return;
    c = &m->cpu;
    o->pc = f8_pc(c);
    o->pc0 = c->pc0;
    o->pc1 = c->pc1;
    o->dc0 = c->dc0;
    o->dc1 = c->dc1;
    o->a = c->a;
    o->w = c->w;
    o->sign = (c->w & F8_SIGN) != 0;
    o->carry = (c->w & F8_CARRY) != 0;
    o->zero = (c->w & F8_ZERO) != 0;
    o->overflow = (c->w & F8_OVR) != 0;
    o->icb = (c->w & F8_ICB) != 0;
    o->isar = (uint8_t)(c->is & 63);
    memcpy(o->r, c->r, sizeof o->r);
    o->j_h_k_q[0] = c->r[9];
    o->j_h_k_q[1] = (uint16_t)((c->r[10] << 8) | c->r[11]);
    o->j_h_k_q[2] = (uint16_t)((c->r[12] << 8) | c->r[13]);
    o->j_h_k_q[3] = (uint16_t)((c->r[14] << 8) | c->r[15]);
    o->dbus = c->dbus;
    p = c->r[59];
    if (!(p & 1) && p >= 40 && p <= 58)
    {
        o->k_depth = (p - 40) / 2;
        for (int i = 0; i < o->k_depth; i++)
            o->k_stack[i] = (uint16_t)((c->r[40 + 2 * i] << 8) | c->r[41 + 2 * i]);
    }
    o->cycles = c->cycles;
    o->instructions = c->instructions;
    o->illegal_count = c->illegal;
    if (c->illegal)
        o->illegal_pc = c->illegal_pc;
    chf_machine_beam(m, &o->vpos, &o->hpos);
    o->frame = (uint32_t)m->frames_done;
    o->machine = m->machine;
    o->bios = m->bios_id;
}

void chfdebug_cpu_get(chfdebug *d, chfdebug_cpu *o)
{
    (void)d;
    chf_host_lock();
    cpu_get_locked(chfdebug_machine(), o);
    chf_host_unlock();
}

static void set_w_bit(f8_t *c, unsigned bit, int v)
{
    c->w = (uint8_t)(v ? c->w | bit : c->w & ~bit);
}

void chfdebug_cpu_set(chfdebug *d, int reg, int v)
{
    chf_machine_t *m;
    f8_t *c;

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
    if (reg >= CHF_REG_R0 && reg < CHF_REG_R0 + 64)
        c->r[reg - CHF_REG_R0] = (uint8_t)v;
    else
    {
        switch (reg)
        {
        case CHF_REG_A:    c->a = (uint8_t)v; break;
        case CHF_REG_W:    c->w = (uint8_t)(v & 0x1f); break;
        case CHF_REG_ISAR: c->is = (uint8_t)(v & 63); break;
        case CHF_REG_PC:   f8_set_pc(c, (uint16_t)v); break;
        case CHF_REG_PC0:  f8_set_pc(c, (uint16_t)(v - 1)); break;   /* PC0 = v: the opcode is at v - 1 */
        case CHF_REG_PC1:  c->pc1 = (uint16_t)v; break;
        case CHF_REG_DC0:  c->dc0 = (uint16_t)v; break;
        case CHF_REG_DC1:  c->dc1 = (uint16_t)v; break;
        case CHF_REG_J:    c->r[9] = (uint8_t)v; break;
        case CHF_REG_H:    c->r[10] = (uint8_t)(v >> 8); c->r[11] = (uint8_t)v; break;
        case CHF_REG_K:    c->r[12] = (uint8_t)(v >> 8); c->r[13] = (uint8_t)v; break;
        case CHF_REG_Q:    c->r[14] = (uint8_t)(v >> 8); c->r[15] = (uint8_t)v; break;
        case CHF_REG_SIGN:     set_w_bit(c, F8_SIGN, v); break;
        case CHF_REG_CARRY:    set_w_bit(c, F8_CARRY, v); break;
        case CHF_REG_ZERO:     set_w_bit(c, F8_ZERO, v); break;
        case CHF_REG_OVERFLOW: set_w_bit(c, F8_OVR, v); break;
        case CHF_REG_ICB:      set_w_bit(c, F8_ICB, v); break;
        default: break;
        }
    }
    snap_refresh(d);
    pthread_mutex_lock(&d->lock);
    d->cur_pc = f8_pc(c);
    d->reason_addr = f8_pc(c);
    pthread_mutex_unlock(&d->lock);
    chf_host_unlock();
    bump(d);
}

/* ---- memory ------------------------------------------------------------------- */

int chfdebug_space_size(chfdebug *d, int space)
{
    chf_machine_t *m;
    int n = 0;

    (void)d;
    if (space < 0 || space > CHF_SPACE_IO)
        return 0;
    chf_host_lock();
    m = chfdebug_machine();
    if (m)
        n = (int)chf_machine_space_size(m, space);
    chf_host_unlock();
    return n;
}

int chfdebug_read(chfdebug *d, int space, uint32_t addr, uint8_t *dst, int n)
{
    chf_machine_t *m;

    (void)d;
    if (space < 0 || space > CHF_SPACE_IO || n <= 0)
        return 0;
    chf_host_lock();
    m = chfdebug_machine();
    if (m)
    {
        const uint32_t size = chf_machine_space_size(m, space);
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

    if (space < 0 || space > CHF_SPACE_IO)
        return;
    chf_host_lock();
    m = chfdebug_machine();
    if (m && addr < chf_machine_space_size(m, space))
    {
        /* an edit is not the program's access: no log, no breakpoint */
        d->in_edit = 1;
        chf_machine_poke(m, space, addr, value);
        d->in_edit = 0;
        if (space == CHF_SPACE_SCRATCH)
            snap_refresh(d);
    }
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
    return n < dstsz ? n : dstsz - 1;
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

/* the FujiNet cartridge's APPBANK page showing at $4800 now, -1 when none */
static int appbank_page(chf_machine_t *m)
{
    chf_cart_status_t st;

    if (!m->fujinet)
        return -1;
    chf_fujinet_status(m->fujinet, &st);
    return st.mailbox && st.npages ? st.page : -1;
}

/* the port an IN / OUT / INS / OUTS names, or -1 */
static int port_operand(uint8_t op, uint8_t operand)
{
    if (op == 0x26 || op == 0x27)
        return operand;
    if ((op & 0xf0) == 0xa0 || (op & 0xf0) == 0xb0)
        return op & 0x0f;
    return -1;
}

static void fill_line(chfdebug *d, chf_machine_t *m, uint16_t addr, chfdebug_line *l, uint16_t pc,
                      int page)
{
    uint8_t ob[3];
    unsigned flags = 0;
    int target = -1, port;
    const char *lab;

    memset(l, 0, sizeof *l);
    ob[0] = pbyte(m, addr);
    ob[1] = pbyte(m, (uint16_t)(addr + 1));
    ob[2] = pbyte(m, (uint16_t)(addr + 2));
    l->length = dasmf8(l->disasm, sizeof l->disasm, addr, ob, &flags, &target);
    l->address = addr;
    l->is_pc = addr == pc;
    l->has_breakpoint = d->map_exec[addr] != 0;
    for (int i = 0, o = 0; i < l->length && o < (int)sizeof l->bytes - 3; i++)
        o += snprintf(l->bytes + o, sizeof l->bytes - (size_t)o, "%s%02X", i ? " " : "",
                      ob[i]);
    lab = symtab_lookup(&d->syms, addr, SYM_BANK_ANY);
    if (lab)
        snprintf(l->label, sizeof l->label, "%s", lab);
    if (target >= 0)
    {
        const char *t = symtab_lookup(&d->syms, (uint16_t)target, SYM_BANK_ANY);
        if (t)
            snprintf(l->comment, sizeof l->comment, "%s", t);
    }
    else if ((port = port_operand(ob[0], ob[1])) >= 0)
    {
        const char *t = symtab_port_name(&d->syms, (unsigned)port);
        if (t)
            snprintf(l->comment, sizeof l->comment, "port %s", t);
    }
    if (page >= 0 && addr >= FN_APP_WIN && addr < FN_APP_WIN + FN_APP_PAGE)
    {
        const size_t n = strlen(l->comment);
        snprintf(l->comment + n, sizeof l->comment - n, "%sAPPBANK page %d", n ? "  " : "", page);
    }
}

int chfdebug_disassemble(chfdebug *d, uint16_t addr, chfdebug_line *out, int max, int *pc_line)
{
    chf_machine_t *m;
    int n = 0;

    if (pc_line)
        *pc_line = -1;
    if (!d)
        return 0;
    chf_host_lock();
    m = chfdebug_machine();
    if (m)
    {
        const uint16_t pc = f8_pc(&m->cpu);
        const int page = appbank_page(m);

        while (n < max)
        {
            fill_line(d, m, addr, &out[n], pc, page);
            if (out[n].is_pc && pc_line)
                *pc_line = n;
            addr = (uint16_t)(addr + out[n].length);
            n++;
        }
    }
    chf_host_unlock();
    return n;
}

static int insn_len(chf_machine_t *m, uint16_t addr)
{
    return dasmf8_length(pbyte(m, addr));
}

static int row_address_locked(chf_machine_t *m, uint16_t addr, int rows)
{
    if (!m || rows == 0)
        return addr;
    if (rows > 0)
    {
        for (int i = 0; i < rows; i++)
            addr = (uint16_t)(addr + insn_len(m, addr));
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
        uint16_t a = (uint16_t)(addr - back);
        while (count < 256)
        {
            const int dist = (uint16_t)(addr - a);
            if (dist == 0 || dist > back)
                break;
            starts[count++] = a;
            a = (uint16_t)(a + insn_len(m, a));
        }
        if (a == addr && count >= rows)
            return starts[count - rows];
    }
    return (uint16_t)(addr - rows);
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
    o->page = -1;
    o->type_name = chf_cart_type_name(CHF_CART_NONE);
    o->description = "No cartridge";
    if (!m)
        return;
    c = m->cart;
    o->present = c != NULL;
    if (!c)
        return;
    o->type = c->type;
    o->type_name = chf_cart_type_name(c->type);
    o->description = o->type_name;
    o->rom_size = c->rom_size;
    if (m->fujinet)
    {
        chf_cart_status_t st;
        chf_fujinet_status(m->fujinet, &st);
        o->type = CHF_CART_FUJINET;
        o->type_name = chf_cart_type_name(CHF_CART_FUJINET);
        o->description = "FujiNet cartridge";
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
        o->npages = (int)st.npages;
        o->page = st.page;
        o->rom_size = st.live_size;
        o->rom_crc = st.live_crc;
        o->boot_state = st.boot_state;
        o->boot_pct = st.boot_pct;
        o->boot_err = st.boot_err;
        o->ackseq = st.ackseq;
        o->status = st.status;
        o->last_error = st.err;
        o->reply_cmd = st.reply_cmd;
        o->rxlen = st.rxlen;
        o->tick = st.tick;
        o->queue_depth = st.queue;
        chf_fujinet_mailbox(m->fujinet, o->mailbox_page);
        memcpy(o->status_page, o->mailbox_page + (FN_R_BASE - FN_R_DATA), sizeof o->status_page);
    }
    else
    {
        if (c->rom && c->rom_size)
        {
            o->softlist_name = chf_cart_softlist_name(c->rom, c->rom_size);
            if (chf_cart_softlist_description(c->rom, c->rom_size))
                o->description = chf_cart_softlist_description(c->rom, c->rom_size);
            o->rom_crc = chf_crc32(c->rom, c->rom_size);
        }
        o->ram_size = c->ram_size;
        o->bank = c->bank;
        o->half = c->half;
        for (unsigned i = 0; i < sizeof o->mailbox_page; i++)
            o->mailbox_page[i] = pbyte(m, (uint16_t)(0xf800 + i));
        memcpy(o->status_page, o->mailbox_page + (FN_R_BASE - FN_R_DATA), sizeof o->status_page);
    }
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
        n = snprintf(dst, (size_t)dstsz, "No cartridge");
    else if (c.type == CHF_CART_FUJINET)
    {
        const char *what = !c.booted_game ? "CONFIG" : c.mailbox ? "app (mailbox live)" : "game image";
        const char *link = c.link_off ? "off" : c.link_up ? "up" : "down";
        if (c.live_size)
            n = snprintf(dst, (size_t)dstsz, "FujiNet cartridge: %s, link %s, %u bytes", what, link,
                         c.live_size);
        else
            n = snprintf(dst, (size_t)dstsz, "FujiNet cartridge: %s, link %s", what, link);
    }
    else
        n = snprintf(dst, (size_t)dstsz, "%s cartridge, %u bytes", c.type_name, c.rom_size);
    return n < dstsz ? n : dstsz - 1;
}

/* the cartridge's events (register writes, TX bytes, page selects, swaps) */
int chfdebug_cart_events(chfdebug *d, chfdebug_cart_event *out, int max)
{
    chf_machine_t *m;
    int n = 0;

    if (!d || !out || max <= 0)
        return 0;
    chf_host_lock();
    m = chfdebug_machine();
    if (m && atomic_load(&d->attached))
    {
        uint64_t have = d->cart_total < CARTEV_N ? d->cart_total : CARTEV_N;

        cart_pull(d, m);
        have = d->cart_total < CARTEV_N ? d->cart_total : CARTEV_N;
        n = have < (uint64_t)max ? (int)have : max;
        for (int i = 0; i < n; i++)
            out[i] = d->cart_ev[(d->cart_total - (uint64_t)n + (uint64_t)i) % CARTEV_N];
    }
    chf_host_unlock();
    return n;
}

/* ---- the logs: port accesses and the tone -------------------------------------------- */

int chfdebug_io_events(chfdebug *d, chfdebug_io_event *out, int max)
{
    int n = 0;

    if (!d || !out || max <= 0)
        return 0;
    chf_host_lock();
    {
        const uint64_t have = d->io_total < IOEV_N ? d->io_total : IOEV_N;
        n = have < (uint64_t)max ? (int)have : max;
        for (int i = 0; i < n; i++)
            out[i] = d->io_ev[(d->io_total - (uint64_t)n + (uint64_t)i) % IOEV_N];
    }
    chf_host_unlock();
    return n;
}

void chfdebug_io_events_clear(chfdebug *d)
{
    if (!d)
        return;
    chf_host_lock();
    d->io_total = 0;
    chf_host_unlock();
    bump(d);
}

int chfdebug_sound_events(chfdebug *d, chfdebug_sound_event *out, int max)
{
    int n = 0;

    if (!d || !out || max <= 0)
        return 0;
    chf_host_lock();
    {
        const uint64_t have = d->snd_total < SNDEV_N ? d->snd_total : SNDEV_N;
        n = have < (uint64_t)max ? (int)have : max;
        for (int i = 0; i < n; i++)
            out[i] = d->snd_ev[(d->snd_total - (uint64_t)n + (uint64_t)i) % SNDEV_N];
    }
    chf_host_unlock();
    return n;
}

/* ---- the ROMC stream --------------------------------------------------------------- */

void chfdebug_romc_enable(chfdebug *d, int on)
{
    chf_machine_t *m;

    if (!d)
        return;
    chf_host_lock();
    d->romc_on = on != 0;
    if (on)
        d->romc_head = d->romc_count = 0;
    m = chfdebug_machine();
    if (m && atomic_load(&d->attached))
        m->cpu.romc_hook = on ? romc_record : NULL;
    chf_host_unlock();
    bump(d);
}

int chfdebug_romc_enabled(chfdebug *d)
{
    return d && d->romc_on;
}

int chfdebug_romc_read(chfdebug *d, chfdebug_romc *out, int max)
{
    int n;

    if (!d)
        return 0;
    chf_host_lock();
    n = d->romc_count < max ? d->romc_count : max;
    for (int i = 0; i < n; i++)
        out[n - 1 - i] = d->romc_ring[(d->romc_head - 1 - i + ROMC_N) % ROMC_N];
    chf_host_unlock();
    return n;
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
        "  TICK       %u\n"
        "  queue      %u\n"
        "\n"
        "Boot         state %d  %d%%\n"
        "             error %d\n"
        "Pages        %d APPBANK, %d at $4800\n"
        "Staged       %s%s  %u bytes, CRC %08X\n"
        "Swaps        %d\n",
        c.link_off ? "off (FujiNet is switched off)" : c.link_up ? "up" : "down",
        !c.booted_game ? "CONFIG" : c.mailbox ? "an app" : "a game", c.app_live ? " (APPBANK)" : "",
        c.live_size, c.live_crc,
        c.ackseq, c.status, c.status & 1, (c.status >> 1) & 1, c.last_error, c.reply_cmd,
        c.rxlen, c.tick, c.queue_depth,
        c.boot_state, c.boot_pct, c.boot_err,
        c.npages, c.page,
        c.staged_size ? (c.staged_mailbox ? "an app" : "a game") : "nothing",
        c.staged_size && c.armed ? ", armed" : "", c.staged_size, c.staged_crc, c.swaps);
    return n < dstsz ? n : dstsz - 1;
}

/* ---- breakpoints ---------------------------------------------------------------- */

static int space_of_type(int type)
{
    if (type & (CHFDEBUG_BP_EXEC | CHFDEBUG_BP_READ | CHFDEBUG_BP_WRITE))
        return CHF_SPACE_PROG;
    if (type & (CHFDEBUG_BP_IN | CHFDEBUG_BP_OUT))
        return CHF_SPACE_IO;
    if (type & CHFDEBUG_BP_VRAM)
        return CHF_SPACE_VRAM;
    return CHF_SPACE_SCRATCH;
}

int chfdebug_breakpoint_add(chfdebug *d, int type, int space, uint16_t start, uint16_t end,
                           const char *cond)
{
    chfdebug_breakpoint *b;
    char err[96];
    unsigned limit;
    int id;

    if (!d || type <= 0 || (type & ~(CHFDEBUG_BP_EXEC | CHFDEBUG_BP_READ | CHFDEBUG_BP_WRITE |
                                     CHFDEBUG_BP_IN | CHFDEBUG_BP_OUT | CHFDEBUG_BP_VRAM |
                                     CHFDEBUG_BP_SCRATCH)))
        return -1;
    /* the PROG classes may be combined; every other class stands alone */
    if ((type & (CHFDEBUG_BP_EXEC | CHFDEBUG_BP_READ | CHFDEBUG_BP_WRITE)) &&
        (type & ~(CHFDEBUG_BP_EXEC | CHFDEBUG_BP_READ | CHFDEBUG_BP_WRITE)))
        return -1;
    if ((type & (type - 1)) && space_of_type(type) != CHF_SPACE_PROG)
        return -1;
    if (space != space_of_type(type))
        return -1;
    if (cond && *cond && expr_eval(cond, NULL, NULL, err, sizeof err) != 0)
        return -1;
    switch (space)
    {
    case CHF_SPACE_IO:      limit = 256; break;
    case CHF_SPACE_VRAM:    limit = CF_VRAM_SIZE; break;
    case CHF_SPACE_SCRATCH: limit = 64; break;
    default:                limit = PROG_SIZE; break;
    }
    if (start >= limit || end >= limit)
        return -1;
    if (end < start && type != CHFDEBUG_BP_VRAM)
    {
        uint16_t t = start;
        start = end;
        end = t;
    }
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
    return d && d->map_exec[addr] != 0;
}

int chfdebug_breakpoint_toggle(chfdebug *d, uint16_t addr)
{
    int found = -1;
    if (!d)
        return 0;
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

static int write_ppm(const char *path, const uint32_t *px, int w, int h, char *msg, int msgsz)
{
    FILE *f = fopen(path, "wb");

    if (!f)
        return fail_msg(msg, msgsz, "Cannot write ", path, NULL);
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++)
    {
        uint8_t rgb[3] = { (uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i] };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    if (msg && msgsz > 0)
        snprintf(msg, (size_t)msgsz, "Saved %s", path);
    return 0;
}

static void save_regs(chf_machine_t *m, FILE *f)
{
    chfdebug_cpu c;
    char name[64];

    cpu_get_locked(m, &c);
    fprintf(f, "PC=%04X  PC0=%04X PC1=%04X  DC0=%04X DC1=%04X  A=%02X  W=%02X [I=%d O=%d Z=%d C=%d S=%d]  "
               "ISAR=%02o.%o\n",
            c.pc, c.pc0, c.pc1, c.dc0, c.dc1, c.a, c.w, c.icb, c.overflow, c.zero, c.carry, c.sign,
            c.isar >> 3, c.isar & 7);
    fprintf(f, "J=%02X H=%04X K=%04X Q=%04X  dbus=%02X\n", c.j_h_k_q[0], c.j_h_k_q[1], c.j_h_k_q[2],
            c.j_h_k_q[3], c.dbus);
    fprintf(f, "scratchpad:\n");
    for (int r = 0; r < 64; r += 8)
    {
        fprintf(f, "  r%-2d:", r);
        for (int i = 0; i < 8; i++)
            fprintf(f, " %02X", c.r[r + i]);
        fprintf(f, "\n");
    }
    if (c.k_depth >= 0)
    {
        fprintf(f, "K stack: depth %d, r59=%02X:", c.k_depth, c.r[59]);
        for (int i = 0; i < c.k_depth; i++)
            fprintf(f, " %04X", c.k_stack[i]);
        fprintf(f, "\n");
    }
    else
        fprintf(f, "K stack: r59=%02X is not a K-stack pointer\n", c.r[59]);
    fprintf(f, "cycles=%llu instructions=%llu frame=%u line=%d cycle=%d illegal=%u\n",
            (unsigned long long)c.cycles, (unsigned long long)c.instructions, c.frame, c.vpos,
            c.hpos, c.illegal_count);
    snprintf(name, sizeof name, "%s", chf_machine_name(c.machine));
    fprintf(f, "machine=%s  bios=%s\n", name, c.bios < 0 ? "stand-in" : "imported");
    {
        char buf[2048];
        chfdebug_text_ports(buf, sizeof buf);
        fputs(buf, f);
        chfdebug_text_sound(NULL, buf, sizeof buf);
        fputs(buf, f);
        chfdebug_text_input(buf, sizeof buf);
        fputs(buf, f);
    }
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
    if (!strcmp(kind, "vram"))
        return write_bytes(path, m->vid.vram, CF_VRAM_SIZE, msg, msgsz);
    if (!strcmp(kind, "scratch"))
        return write_bytes(path, m->cpu.r, 64, msg, msgsz);
    if (!strcmp(kind, "cart"))
    {
        uint8_t *buf;
        uint32_t size;
        int rc;

        if (m->fujinet)
        {
            buf = malloc(CHFMAP_STORE);
            if (!buf)
                return fail_msg(msg, msgsz, "Out of memory.", NULL, NULL);
            size = chf_fujinet_copy_image(m->fujinet, buf, CHFMAP_STORE);
        }
        else
        {
            size = m->cart ? m->cart->rom_size : 0;
            buf = malloc(size ? size : 1);
            if (!buf)
                return fail_msg(msg, msgsz, "Out of memory.", NULL, NULL);
            if (size)
                memcpy(buf, m->cart->rom, size);
        }
        rc = write_bytes(path, buf, size, msg, msgsz);
        free(buf);
        return rc;
    }
    if (!strcmp(kind, "screen"))
        return write_ppm(path, m->fb, CHF_FB_WIDTH, CHF_FB_HEIGHT, msg, msgsz);
    if (!strcmp(kind, "vramshot"))
    {
        static uint32_t px[CHFDEBUG_VIEW_MAX_PIXELS];
        int w = 0, h = 0;

        if (!chfdebug_video_view(d, CHFDEBUG_VIEW_VRAM, 0, 1, px, CHFDEBUG_VIEW_MAX_PIXELS, &w, &h))
            return fail_msg(msg, msgsz, "Cannot draw the VRAM.", NULL, NULL);
        return write_ppm(path, px, w, h, msg, msgsz);
    }
    if (!strcmp(kind, "dis") || !strcmp(kind, "regs"))
    {
        FILE *f = fopen(path, "w");
        if (!f)
            return fail_msg(msg, msgsz, "Cannot write ", path, NULL);
        if (!strcmp(kind, "dis"))
        {
            const uint16_t pc = f8_pc(&m->cpu);
            const int page = appbank_page(m);
            unsigned a = 0;
            while (a < 0x4800)
            {
                chfdebug_line l;
                fill_line(d, m, (uint16_t)a, &l, pc, page);
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
    return fail_msg(msg, msgsz, "Unknown save kind ", kind,
                    " (dis, mailbox, vram, scratch, cart, screen, vramshot, regs)");
}

int chfdebug_save(chfdebug *d, const char *kind, const char *path, char *msg, int msgsz)
{
    int rc;
    chf_host_lock();
    rc = save_locked(d, chfdebug_machine(), kind, path, msg, msgsz);
    chf_host_unlock();
    return rc;
}
