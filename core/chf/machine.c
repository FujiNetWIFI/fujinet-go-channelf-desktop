/* machine.c -- the Fairchild Channel F console: the F8's memory map and ports,
 * the panel and hand controllers, the screen's vertical blank, the cartridge
 * slot and the console's tone.
 *
 * Transposed from MAME's src/mame/fairchild/channelf.cpp (GPL-2.0-or-later,
 * copyright-holders Juergen Buchmueller, Frank Palazzolo, Sean Riddle; see
 * COMPLIANCE.md). See chf_internal.h for how the pieces are clocked.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "chf_internal.h"

#define ATTOS_PER_SEC UINT64_C(1000000000000000000)

/* ---- the machines ---------------------------------------------------------- */

int chf_machine_is_pal(int machine)
{
    return machine == CHF_MACHINE_VIDEOPLAY || machine == CHF_MACHINE_VIDEOPLAY2;
}

const char *chf_machine_name(int machine)
{
    switch (machine) {
    case CHF_MACHINE_CHANNELF:   return "Fairchild Channel F (NTSC)";
    case CHF_MACHINE_CHANNELF2:  return "Fairchild Channel F System II (NTSC)";
    case CHF_MACHINE_VIDEOPLAY:  return "Saba Videoplay (PAL)";
    case CHF_MACHINE_VIDEOPLAY2: return "Saba Videoplay 2 (PAL)";
    default:                     return "Channel F";
    }
}

/* ---- time ------------------------------------------------------------------
 * MAME's attoseconds: a CPU clock is 1e18 / hz (truncated), the nth clock is at
 * n * that, and the sound stream, the tick and the frame all ask "how many of
 * X have gone by at this time", exactly. The products overflow 64 bits within
 * seconds, hence 128. */

#ifdef __SIZEOF_INT128__
typedef unsigned __int128 cf_u128;
#endif

/* floor(a * b / c) */
static uint64_t mul_div(uint64_t a, uint64_t b, uint64_t c)
{
#ifdef __SIZEOF_INT128__
    return (uint64_t)(((cf_u128)a * b) / c);
#else
    return (uint64_t)((long double)a * (long double)b / (long double)c);
#endif
}

/* floor(cycles * cycle_as * rate / 1e18): how many ticks of a `rate` Hz clock
 * (MAME's attotime::as_ticks / the stream's sample index) have gone by */
static uint64_t cycles_to_ticks(uint64_t cycles, uint64_t cycle_as, uint32_t rate)
{
#ifdef __SIZEOF_INT128__
    return (uint64_t)(((cf_u128)cycles * cycle_as * rate) / ATTOS_PER_SEC);
#else
    return (uint64_t)((long double)cycles * (long double)cycle_as * (long double)rate / 1e18L);
#endif
}

void chf_machine_time(const chf_machine_t *m, uint64_t *sec, uint64_t *attos)
{
#ifdef __SIZEOF_INT128__
    const cf_u128 a = (cf_u128)m->cpu.cycles * m->cycle_as;

    *sec = (uint64_t)(a / ATTOS_PER_SEC);
    *attos = (uint64_t)(a % ATTOS_PER_SEC);
#else
    const long double a = (long double)m->cpu.cycles * (long double)m->cycle_as;
    const long double s = a / 1e18L;

    *sec = (uint64_t)s;
    *attos = (uint64_t)(a - (long double)*sec * 1e18L);
#endif
}

/* the CPU clock the frame k is waiting for: floor(k * frame_as / cycle_as) */
static uint64_t frame_limit(const chf_machine_t *m, uint64_t k)
{
    return mul_div(k, m->frame_as, m->cycle_as);
}

/* Where the beam is, in lines and CPU clocks into the line (screen_device::
 * vpos / hpos: relative to the last vertical blank, half a pixel ahead). */
void chf_machine_beam(const chf_machine_t *m, int *line, int *cycle_in_line)
{
    uint64_t delta, l;

#ifdef __SIZEOF_INT128__
    {
        const cf_u128 now = (cf_u128)m->cpu.cycles * m->cycle_as;
        const cf_u128 lastb = (cf_u128)m->frames_done * m->frame_as;

        delta = now > lastb ? (uint64_t)(now - lastb) : 0;
    }
#else
    {
        const long double now = (long double)m->cpu.cycles * (long double)m->cycle_as;
        const long double lastb = (long double)m->frames_done * (long double)m->frame_as;

        delta = now > lastb ? (uint64_t)(now - lastb) : 0;
    }
#endif
    delta = (delta + m->pixel_as / 2) % m->frame_as;
    l = delta / m->scan_as;
    if (line)
        *line = (int)((l + (uint64_t)m->vbstart) % (uint64_t)m->lines);
    if (cycle_in_line)
        *cycle_in_line = (int)((delta - l * m->scan_as) / m->cycle_as);
}

/* ---- helpers ---------------------------------------------------------------- */

static inline void bus_hook(chf_machine_t *m, unsigned flag, int kind, uint16_t addr, uint8_t data)
{
    if ((m->watch & flag) && m->bus_hook)
        m->bus_hook(m, m->bus_hook_user, kind, addr, data);
}

static void park(chf_machine_t *m, const char *why)
{
    if (!m->parked) {
        m->parked = true;
        snprintf(m->park_why, sizeof m->park_why, "%s", why);
    }
}

unsigned chf_machine_missing(const chf_machine_t *m, char *why, int whysz)
{
    if (why && whysz > 0)
        snprintf(why, (size_t)whysz, "%s", m->parked ? m->park_why : "");
    return m->parked ? CHF_MISSING_BIOS : 0;
}

/* The tone, brought up to the CPU's time (m_channel->update()): the stream
 * generates every sample up to and including the one the current time falls
 * in. Time zero never updates. */
static void sound_sync(chf_machine_t *m)
{
    if (m->cpu.cycles == 0)
        return;
    sound_run(&m->snd, cycles_to_ticks(m->cpu.cycles, m->cycle_as, SOUND_RATE) + 1, m->mono, &m->nmono,
              CHF_AUDIO_MAX);
}

int chf_machine_flush_audio(chf_machine_t *m)
{
    const float g = m->audio_gain;
    int n;

    sound_sync(m);
    n = m->nmono;
    for (int i = 0; i < n; i++) {
        m->out[2 * i] = m->mono[i] * g;
        m->out[2 * i + 1] = m->mono[i] * g;
    }
    m->audio_samples += (uint64_t)n;
    m->nmono = 0;
    m->out_frames = n;
    return n;
}

/* ---- the debugger's video bookkeeping ------------------------------------------ */

/* rows of the VRAM whose drawing the beam has begun since the last blank */
static int beam_rows_passed(const chf_machine_t *m)
{
    int line, n;

    chf_machine_beam(m, &line, NULL);
    if (line >= m->vbstart)
        return 0;               /* still in the blanking before line 0 */
    n = line / m->y_rpt + 1;
    return n > CF_VRAM_ROWS ? CF_VRAM_ROWS : n;
}

static void track_beam(chf_machine_t *m, int rows)
{
    chf_vtrack_t *t = m->track;

    if (rows > t->beam_rows) {
        memcpy(t->beam + (size_t)t->beam_rows * CF_VRAM_COLS, m->vid.vram + (size_t)t->beam_rows * CF_VRAM_COLS,
               (size_t)(rows - t->beam_rows) * CF_VRAM_COLS);
        t->beam_rows = rows;
    }
}

int chf_machine_track(chf_machine_t *m, bool on)
{
    if (!on) {
        free(m->track);
        m->track = NULL;
        return 0;
    }
    if (m->track)
        return 0;
    m->track = calloc(1, sizeof *m->track);
    if (!m->track)
        return -1;
    for (int i = 0; i < CF_VRAM_SIZE; i++)
        m->track->cell[i].pc = -1;
    memcpy(m->track->beam, m->vid.vram, CF_VRAM_SIZE);
    memcpy(m->track->frame_vram, m->vid.vram, CF_VRAM_SIZE);
    memcpy(m->track->prev_vram, m->vid.vram, CF_VRAM_SIZE);
    m->track->beam_rows = beam_rows_passed(m);
    return 0;
}

int chf_machine_vwrites(const chf_machine_t *m, chf_vwrite_t *out, int max)
{
    const chf_vtrack_t *t = m->track;
    uint64_t n;

    if (!t || max <= 0)
        return 0;
    n = t->total < CHF_VWRITE_RING ? t->total : CHF_VWRITE_RING;
    if (n > (uint64_t)max)
        n = (uint64_t)max;
    for (uint64_t i = 0; i < n; i++)
        out[i] = t->ring[(t->total - n + i) & (CHF_VWRITE_RING - 1)];
    return (int)n;
}

/* a VRAM store: log it before the cell changes */
static void vram_store(chf_machine_t *m, unsigned offs, uint8_t value)
{
    const uint8_t old = m->vid.vram[offs];
    int line;
    chf_vtrack_t *t = m->track;

    if (!t && !(m->watch & CHF_WATCH_VRAM))
        return;
    chf_machine_beam(m, &line, NULL);
    if (t)
        track_beam(m, beam_rows_passed(m));
    m->vw.cycle = m->cpu.cycles;
    m->vw.frame = (uint32_t)m->frames_done;
    m->vw.line = (int16_t)line;
    m->vw.pc = f8_pc(&m->cpu);
    m->vw.row = (uint8_t)(offs / CF_VRAM_COLS);
    m->vw.col = (uint8_t)(offs % CF_VRAM_COLS);
    m->vw.value = value;
    m->vw.old = old;
    if (t) {
        t->ring[t->total & (CHF_VWRITE_RING - 1)] = m->vw;
        t->total++;
        t->cell[offs].cycle = m->vw.cycle;
        t->cell[offs].frame = m->vw.frame;
        t->cell[offs].line = m->vw.line;
        t->cell[offs].pc = m->vw.pc;
        t->cell[offs].old = old;
        t->cell[offs].writes++;
        if (t->heat[offs] < 255)
            t->heat[offs]++;
        t->strobes_frame++;
        if (old != value)
            t->changes_frame++;
    }
}

/* ---- the CPU's buses (channelf_map, channelf_io) -------------------------------- */

static uint8_t mem_get(chf_machine_t *m, uint16_t addr, int peek)
{
    chf_cart_t *c = m->cart;

    if (addr < CF_BIOS_SIZE)
        return m->rom[addr];
    if (!c)
        return 0xff;
    if (c->ops->read_ram && addr >= c->ram_lo && addr <= c->ram_hi)
        return c->ops->read_ram(c, (uint16_t)(addr - c->ram_lo), peek);
    return chf_cart_read_rom(c, (uint16_t)(addr - 0x800), peek);
}

static void mem_put(chf_machine_t *m, uint16_t addr, uint8_t data)
{
    chf_cart_t *c = m->cart;

    if (addr < 0x800 || !c)
        return;                 /* the BIOS is ROM; the slot may be empty */
    if (c->ops->write_ram && addr >= c->ram_lo && addr <= c->ram_hi)
        c->ops->write_ram(c, (uint16_t)(addr - c->ram_lo), data);
    else if (c->ops->write_bank && addr >= c->bank_lo && addr <= c->bank_hi)
        c->ops->write_bank(c, (uint16_t)(addr - c->bank_lo), data);
}

static uint8_t io_get(chf_machine_t *m, unsigned port, int peek)
{
    chf_cart_t *c = m->cart;

    port &= 0xff;
    if (port == 0 || port == 1 || port == 4 || port == 5)
        return video_read(&m->vid, port, m->panel, m->ctl[0], m->ctl[1]);
    if (c && c->ops->io_read && port >= c->io_lo && port <= c->io_hi)
        return c->ops->io_read(c, (uint8_t)port, peek);
    return 0;                   /* nothing else answers */
}

static void io_put(chf_machine_t *m, unsigned port, uint8_t data)
{
    chf_cart_t *c = m->cart;

    port &= 0xff;
    if (port == 0 || port == 1 || port == 4 || port == 5) {
        if (port == 0 && (data & 0x20)) {
            const unsigned offs = (unsigned)m->vid.row_reg * CF_VRAM_COLS + m->vid.col_reg;

            vram_store(m, offs, m->vid.val_reg);
            video_write(&m->vid, port, data);
            bus_hook(m, CHF_WATCH_VRAM, CHF_BUS_VRAM, (uint16_t)offs, m->vid.val_reg);
            return;
        }
        video_write(&m->vid, port, data);
        if (port == 5) {
            /* port_5_w: m_custom->sound_w((data >> 6) & 3) */
            const int mode = (data >> 6) & 3;

            if (mode != m->snd.mode) {
                sound_sync(m);
                sound_set_mode(&m->snd, mode);
            }
        }
        return;
    }
    if (c && c->ops->io_write && port >= c->io_lo && port <= c->io_hi)
        c->ops->io_write(c, (uint8_t)port, data);
}

static uint8_t cpu_read(void *ctx, uint16_t addr, uint8_t romc)
{
    chf_machine_t *m = ctx;
    const uint8_t data = mem_get(m, addr, 0);

    if (romc == 0x02)
        bus_hook(m, CHF_WATCH_MEMR, CHF_BUS_MEMR, addr, data);
    return data;
}

static void cpu_write(void *ctx, uint16_t addr, uint8_t data)
{
    chf_machine_t *m = ctx;

    bus_hook(m, CHF_WATCH_MEMW, CHF_BUS_MEMW, addr, data);
    mem_put(m, addr, data);
}

static uint8_t cpu_in(void *ctx, uint8_t port)
{
    chf_machine_t *m = ctx;
    const uint8_t data = io_get(m, port, 0);

    bus_hook(m, CHF_WATCH_IOR, CHF_BUS_IOR, port, data);
    return data;
}

static void cpu_out(void *ctx, uint8_t port, uint8_t data)
{
    chf_machine_t *m = ctx;

    bus_hook(m, CHF_WATCH_IOW, CHF_BUS_IOW, port, data);
    io_put(m, port, data);
}

/* ---- power on ----------------------------------------------------------------- */

void chf_machine_init(chf_machine_t *m, const chf_machine_opts_t *opts)
{
    const bool pal = chf_machine_is_pal(opts->machine) != 0;
    const uint32_t cpu_hz = pal ? CF_PAL_CPU_HZ : CF_NTSC_CPU_HZ;
    const uint32_t pixel_hz = pal ? CF_PAL_PIXEL_HZ : CF_NTSC_PIXEL_HZ;

    memset(m, 0, sizeof *m);
    m->machine = opts->machine;
    m->is_pal = pal;
    m->route_gain = opts->machine == CHF_MACHINE_VIDEOPLAY2 ? 0.5f : 1.0f;
    m->audio_gain = m->route_gain;
    m->cart = opts->cart;
    m->fujinet = opts->fujinet;
    m->lines = pal ? CF_PAL_LINES : CF_NTSC_LINES;
    m->vbstart = pal ? CF_PAL_VBSTART : CF_NTSC_VBSTART;
    m->vbend = pal ? CF_PAL_VBEND : CF_NTSC_VBEND;
    m->y_rpt = pal ? 5 : 4;
    m->cycle_hz = (double)cpu_hz;

    /* HZ_TO_ATTOSECONDS: ATTOSECONDS_PER_SECOND / hz, truncated; the screen's
     * set_raw: that for a pixel, times htotal * vtotal for the frame */
    m->cycle_as = ATTOS_PER_SEC / cpu_hz;
    m->frame_as = (ATTOS_PER_SEC / pixel_hz) * CF_HTOTAL * (uint64_t)m->lines;
    m->scan_as = m->frame_as / (uint64_t)m->lines;
    m->pixel_as = m->frame_as / ((uint64_t)m->lines * CF_HTOTAL);

    if (opts->bios_lo && opts->bios_hi) {
        memcpy(m->rom, opts->bios_lo, CF_BIOS_HALF);
        memcpy(m->rom + CF_BIOS_HALF, opts->bios_hi, CF_BIOS_HALF);
        m->have_bios = true;
        m->bios_id = opts->bios_id;
    } else {
        memcpy(m->rom, chf_stub_rom, CF_BIOS_SIZE);
        m->bios_id = -1;
    }

    video_init(&m->vid);
    sound_init(&m->snd);

    m->cpu.ctx = m;
    m->cpu.read = cpu_read;
    m->cpu.write = cpu_write;
    m->cpu.in = cpu_in;
    m->cpu.out = cpu_out;
    f8_power_on(&m->cpu);

    m->next_limit = frame_limit(m, 1);
    if (m->fujinet) {
        chf_fujinet_set_clock(m->fujinet, &m->cpu.cycles, (double)cpu_hz);
        chf_fujinet_set_pc(m->fujinet, &m->cpu.pc0);
    }

    /* the stand-in has no BIOS to run a game: a Videocart of its own parks the
     * console at once (a FujiNet app, and the CONFIG client, run on it; see
     * chf_machine_check_cart for the FujiNet cartridge) */
    if (!m->have_bios && m->cart && !m->fujinet)
        park(m, "a Videocart needs the console's BIOS");
}

void chf_machine_check_cart(chf_machine_t *m)
{
    chf_cart_status_t st;

    if (!m->fujinet || m->have_bios)
        return;
    chf_fujinet_status(m->fujinet, &st);
    m->fj_swaps = st.swaps;
    if (st.powered && !st.mailbox)
        park(m, "a game needs the console's BIOS");
}

/* ---- inputs ----------------------------------------------------------------------- */

void chf_machine_reset(chf_machine_t *m)
{
    f8_reset(&m->cpu);
    if (m->cart && !m->fujinet)
        chf_cart_reset(m->cart);
}

void chf_machine_latch_inputs(chf_machine_t *m)
{
    m->panel = m->panel_live & 0x0f;
    m->ctl[0] = m->ctl_live[0];
    m->ctl[1] = m->ctl_live[1];
    if (m->reset_live != m->reset_held) {
        m->reset_held = m->reset_live;
        if (m->reset_held)
            chf_machine_reset(m);       /* the RESET key: the F8 only */
    }
}

/* ---- running --------------------------------------------------------------------- */

/* A game came in over the FujiNet cartridge while the stand-in is the BIOS: it
 * would call BIOS routines that are not there. Park the console; the session
 * says "Import BIOS". */
static void check_swap(chf_machine_t *m)
{
    chf_cart_status_t st;

    if (!m->fujinet || m->have_bios || m->parked)
        return;
    chf_fujinet_status(m->fujinet, &st);
    if (st.swaps != m->fj_swaps) {
        m->fj_swaps = st.swaps;
        if (!st.mailbox)
            park(m, "a game booted by FujiNet needs the console's BIOS");
    }
}

/* screen_update at the vertical blank: the whole VRAM at once */
static void fire_frame(chf_machine_t *m)
{
    chf_vtrack_t *t = m->track;

    if (t) {
        track_beam(m, CF_VRAM_ROWS);        /* the beam has drawn every row */
        memcpy(t->prev_vram, t->frame_vram, CF_VRAM_SIZE);
        memcpy(t->frame_vram, m->vid.vram, CF_VRAM_SIZE);
        memcpy(t->heat_last, t->heat, CF_VRAM_SIZE);
        memset(t->heat, 0, CF_VRAM_SIZE);
        t->strobes_last = t->strobes_frame;
        t->changes_last = t->changes_frame;
        t->strobes_frame = t->changes_frame = 0;
        t->beam_rows = 0;
    }
    video_render(&m->vid, m->fb);
    m->frames_done++;
    m->next_limit = frame_limit(m, m->frames_done + 1);
    check_swap(m);
}

/* fire every vertical blank the CPU has reached */
static void fire_frames(chf_machine_t *m)
{
    while (m->cpu.cycles >= m->next_limit)
        fire_frame(m);
}

static int step_inner(chf_machine_t *m)
{
    int cycles;

    fire_frames(m);
    /* run_frame stops at the blank that completes the frame, not an instruction later */
    if (m->frame_stop && m->frames_done >= m->frame_stop)
        return 0;
    /* a reset key held, or something the stand-in cannot run: the CPU does not
     * run, time still passes */
    if (m->reset_held || m->parked) {
        m->cpu.cycles += 4;
        return 4;
    }
    cycles = f8_step(&m->cpu);
    if (!m->have_bios && m->cpu.illegal != m->illegal_seen) {
        m->illegal_seen = m->cpu.illegal;
        if (m->cpu.illegal_pc >= 0 && m->cpu.illegal_pc < CF_BIOS_SIZE) {
            char why[64];

            snprintf(why, sizeof why, "the cartridge called the BIOS at $%04X", m->cpu.illegal_pc);
            park(m, why);
        }
    }
    return cycles;
}

int chf_machine_step_quiet(chf_machine_t *m)
{
    return step_inner(m);
}

int chf_machine_step(chf_machine_t *m)
{
    fire_frames(m);
    if (m->frame_stop && m->frames_done >= m->frame_stop)
        return 0;
    if (m->instr_hook)
        m->instr_hook(m, m->instr_hook_user);
    return step_inner(m);
}

void chf_machine_run_frame(chf_machine_t *m)
{
    const uint64_t target = m->frames_done + 1;

    chf_machine_latch_inputs(m);
    m->frame_stop = target;
    while (m->frames_done < target)
        chf_machine_step(m);
    m->frame_stop = 0;
    chf_fujinet_frame(m->fujinet);
    m->frame_count++;
    chf_machine_flush_audio(m);
}

void chf_machine_snapshot(chf_machine_t *m, uint32_t *dst)
{
    video_render(&m->vid, dst);
}

/* ---- the debugger's views ---------------------------------------------------------- */

uint32_t chf_machine_space_size(const chf_machine_t *m, int space)
{
    switch (space) {
    case CHF_PEEK_PROG:    return 0x10000;
    case CHF_PEEK_SCRATCH: return 64;
    case CHF_PEEK_VRAM:    return CF_VRAM_SIZE;
    case CHF_PEEK_IO:      return 256;
    case CHF_PEEK_CART:
        if (m->fujinet)
            return 0x18000;     /* the 64K image store, then the 32K arena */
        return chf_cart_space_size(m->cart);
    }
    return 0;
}

uint8_t chf_machine_port_read(chf_machine_t *m, unsigned port)
{
    return io_get(m, port, 1);
}

uint8_t chf_machine_peek(chf_machine_t *m, int space, uint32_t addr)
{
    switch (space) {
    case CHF_PEEK_PROG:
        if (addr >= 0x10000)
            return 0xff;
        if (m->fujinet && addr >= 0x800)
            return chf_fujinet_peek(m->fujinet, (uint16_t)addr);
        return mem_get(m, (uint16_t)addr, 1);
    case CHF_PEEK_SCRATCH:
        return addr < 64 ? m->cpu.r[addr] : 0xff;
    case CHF_PEEK_VRAM:
        return addr < CF_VRAM_SIZE ? m->vid.vram[addr] : 0xff;
    case CHF_PEEK_IO:
        return io_get(m, addr, 1);
    case CHF_PEEK_CART:
        if (m->fujinet) {
            if (addr < 0x10000)
                return chf_fujinet_image_byte(m->fujinet, addr);
            return addr < 0x18000 ? chf_fujinet_arena(m->fujinet, addr - 0x10000) : 0xff;
        }
        return chf_cart_space_peek(m->cart, addr);
    }
    return 0xff;
}

void chf_machine_poke(chf_machine_t *m, int space, uint32_t addr, uint8_t data)
{
    chf_cart_t *c = m->cart;

    switch (space) {
    case CHF_PEEK_PROG:
        if (addr < 0x800 || addr >= 0x10000)
            break;
        if (m->fujinet)
            chf_fujinet_poke(m->fujinet, (uint16_t)addr, data);
        else if (c && c->ops->write_ram && addr >= c->ram_lo && addr <= c->ram_hi)
            c->ops->write_ram(c, (uint16_t)(addr - c->ram_lo), data);
        break;
    case CHF_PEEK_SCRATCH:
        if (addr < 64)
            m->cpu.r[addr] = data;
        break;
    case CHF_PEEK_VRAM:
        if (addr < CF_VRAM_SIZE)
            m->vid.vram[addr] = data & 3;
        break;
    case CHF_PEEK_IO:
        io_put(m, addr, data);
        break;
    case CHF_PEEK_CART:
        if (m->fujinet) {
            if (addr < 0x10000)
                chf_fujinet_image_poke(m->fujinet, addr, data);
            else if (addr < 0x18000)
                chf_fujinet_arena_poke(m->fujinet, addr - 0x10000, data);
        } else {
            chf_cart_space_poke(c, addr, data);
        }
        break;
    }
}
