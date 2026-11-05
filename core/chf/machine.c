/* machine.c -- the Fairchild Channel F console: the memory map, the keypads
 * and panel, the pots, the 2637 UVI's line timer, SENSE, the cartridge slot and
 * the mix of the console's sound.
 *
 * Transposed from MAME's src/mame/channelf/channelf.cpp (GPL-2.0-or-later,
 * copyright-holders Peter Trauner; see COMPLIANCE.md). See chf_internal.h for
 * how the pieces are clocked.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <math.h>
#include <string.h>

#include "chf_internal.h"
#include "chfmap.h"

/* ---- the machines ---------------------------------------------------------- */

int chf_machine_is_pal(int machine)
{
    return machine == CHF_MACHINE_ARCADIA_PAL;
}

const char *chf_machine_name(int machine)
{
    switch (machine) {
    case CHF_MACHINE_ARCADIA: return "Fairchild Channel F (NTSC)";
    case CHF_MACHINE_ARCADIA_PAL:  return "Channel F clone (PAL)";
    default:               return "Channel F";
    }
}

/* ---- the line timer --------------------------------------------------------
 * MAME: timer period attotime::from_hz(262 * 60), the CPU's period
 * HZ_TO_ATTOSECONDS(895000); the scheduler lets the CPU run floor(delta /
 * period) cycles to the next timer (never less than the instruction in
 * progress) and fires the timer then. So the k-th tick waits for the CPU cycle
 * count to reach floor(k * line_as / cycle_as). */

static uint64_t line_limit(const chf_machine_t *m, uint64_t k)
{
#ifdef __SIZEOF_INT128__
    return (uint64_t)(((unsigned __int128)k * m->line_as) / m->cycle_as);
#else
    return (uint64_t)((long double)k * (long double)m->line_as / (long double)m->cycle_as);
#endif
}

/* ---- helpers -------------------------------------------------------------- */

static inline void bus_hook(chf_machine_t *m, unsigned flag, int kind, uint16_t addr, uint8_t data)
{
    if ((m->watch & flag) && m->bus_hook)
        m->bus_hook(m, m->bus_hook_user, kind, addr, data);
}

/* the sound chip, brought up to the CPU's time first (m_channel->update()) */
static void sound_sync(chf_machine_t *m)
{
    sound_run(&m->snd, (uint64_t)((double)m->cpu.cycles * SOUND_RATE / m->cycle_hz));
}

static void uvi_store(chf_machine_t *m, unsigned off, uint8_t data)
{
    /* video_w: $18FD and $18FE also drive the beeper (offset & 3) */
    if (off == 0xfd || off == 0xfe) {
        sound_sync(m);
        sound_write(&m->snd, off & 3, data);
    }
    uvi_write(&m->uvi, off, data);
}

/* ---- the CPU's buses (arcadia_mem) ----------------------------------------- */

static uint8_t mem_get(chf_machine_t *m, uint16_t addr, int peek)
{
    addr &= 0x7fff;
    if (addr < 0x1000)
        return chf_cart_read_rom(m->cart, addr, peek);
    if (addr >= 0x1800 && addr <= 0x1aff)
        return uvi_read(&m->uvi, addr - 0x1800);
    if (m->cart && addr >= m->cart->extra_lo && addr <= m->cart->extra_hi)
        return chf_cart_extra_rom(m->cart, (uint16_t)(addr - m->cart->extra_lo), peek);
    return 0;               /* unmapped */
}

static uint8_t cpu_fetch(void *user, uint16_t addr)
{
    return mem_get(user, addr, 0);
}

static uint8_t cpu_read(void *user, uint16_t addr)
{
    chf_machine_t *m = user;
    uint8_t data = mem_get(m, addr, 0);

    bus_hook(m, CHF_WATCH_MEMR, CHF_BUS_MEMR, addr & 0x7fff, data);
    return data;
}

static void cpu_write(void *user, uint16_t addr, uint8_t data)
{
    chf_machine_t *m = user;

    addr &= 0x7fff;
    bus_hook(m, CHF_WATCH_MEMW, CHF_BUS_MEMW, addr, data);
    if (addr >= 0x1800 && addr <= 0x1aff)
        uvi_store(m, addr - 0x1800, data);
    /* everything else: the cartridge is ROM, the rest unmapped */
}

/* "The Read/Write 2650 CPU Port-Commands do not appear to be connected to
 * anything in hardware." */
static uint8_t cpu_io_read(void *user, int port)
{
    chf_machine_t *m = user;

    bus_hook(m, CHF_WATCH_IOR, CHF_BUS_IOR, (uint16_t)port, 0);
    return 0;
}

static void cpu_io_write(void *user, int port, uint8_t data)
{
    chf_machine_t *m = user;

    bus_hook(m, CHF_WATCH_IOW, CHF_BUS_IOW, (uint16_t)port, data);
}

/* vsync_r: ASSERT_LINE from line 216 */
static int cpu_sense(void *user)
{
    chf_machine_t *m = user;

    return uvi_vblank(&m->uvi);
}

/* ---- power on ----------------------------------------------------------------- */

void chf_machine_init(chf_machine_t *m, const chf_machine_opts_t *opts)
{
    const int lines = chf_machine_is_pal(opts->machine) ? CHF_PAL_LINES : CHF_NTSC_LINES;
    const int fps = chf_machine_is_pal(opts->machine) ? CHF_PAL_FRAME_HZ : CHF_NTSC_FRAME_HZ;

    memset(m, 0, sizeof *m);
    m->machine = opts->machine;
    m->is_pal = chf_machine_is_pal(opts->machine);
    m->cycle_hz = m->is_pal ? CHF_PAL_CPU_HZ : CHF_NTSC_CPU_HZ;
    m->audio_gain = CHF_AUDIO_BOOST;
    m->cart = opts->cart;
    m->fujinet = opts->fujinet;

    /* attotime::from_hz(): ATTOSECONDS_PER_SECOND / hz, truncated */
    m->line_as = (uint64_t)(1e18 / ((double)lines * fps));
    m->cycle_as = (uint64_t)(1e18 / m->cycle_hz);

    uvi_init(&m->uvi, lines);
    sound_init(&m->snd);

    s2650_init(&m->cpu);
    m->cpu.ctx = m;
    m->cpu.read = cpu_read;
    m->cpu.write = cpu_write;
    m->cpu.fetch = cpu_fetch;
    m->cpu.io_read = cpu_io_read;
    m->cpu.io_write = cpu_io_write;
    m->cpu.sense = cpu_sense;
    s2650_reset(&m->cpu);

    m->line_k = 0;
    m->next_limit = line_limit(m, 1);
    if (m->fujinet)
        chf_fujinet_set_clock(m->fujinet, &m->cpu.cycles, m->cycle_hz);
}

/* ---- inputs ----------------------------------------------------------------------- */

static uint8_t pot_from_analog(int a, uint8_t centre)
{
    int v;

    if (a < 0)
        v = centre + a * (int)centre / 128;
    else
        v = centre + a * (255 - (int)centre) / 127;
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

void chf_machine_latch_inputs(chf_machine_t *m)
{
    static const struct { uint8_t col, bit; } map[CHF_K_PAD_COUNT] = {
        [CHF_K_1] = {0, 8}, [CHF_K_2] = {1, 8}, [CHF_K_3] = {2, 8},
        [CHF_K_4] = {0, 4}, [CHF_K_5] = {1, 4}, [CHF_K_6] = {2, 4},
        [CHF_K_7] = {0, 2}, [CHF_K_8] = {1, 2}, [CHF_K_9] = {2, 2},
        [CHF_K_CLEAR] = {0, 1}, [CHF_K_0] = {1, 1}, [CHF_K_ENTER] = {2, 1},
    };
    uvi_inputs_t *in = &m->uvi.in;

    m->keys = m->keys_live & CHF_KEYS_MASK;
    m->joy[0] = m->joy_live[0] & 0x1f;
    m->joy[1] = m->joy_live[1] & 0x1f;
    for (int s = 0; s < 2; s++)
        for (int a = 0; a < 2; a++)
            m->disc[s][a] = m->disc_live[s][a];

    for (int p = 0; p < 2; p++) {
        uint8_t col[4] = { 0xf0, 0xf0, 0xf0, 0xf0 };

        for (int k = 0; k < CHF_K_PAD_COUNT; k++)
            if (m->keys & (1ull << (p * 12 + k)))
                col[map[k].col] |= map[k].bit;
        /* the fire button is wired to keypad 2 */
        if (m->joy[p] & CHF_STICK_FIRE)
            col[1] |= 8;
        memcpy(in->pad + 4 * p, col, 4);
    }
    in->panel = (uint8_t)((m->keys >> 24) & 7);

    /* the pots: MAME's $1FF is player 1 (centre $6F), $1FE player 2 (centre $80) */
    for (int s = 0; s < 2; s++) {
        const uint8_t centre = s == 0 ? 0x6f : 0x80;
        const uint8_t j = m->joy[s];

        if (m->disc[s][0] || m->disc[s][1]) {
            in->pot[s][0] = pot_from_analog(m->disc[s][0], centre);
            in->pot[s][1] = pot_from_analog(m->disc[s][1], centre);
        } else {
            /* video_r: a later test wins, so right beats left and up beats down */
            uint8_t x = centre, y = centre;

            if (j & CHF_STICK_LEFT)  x = 0;
            if (j & CHF_STICK_RIGHT) x = 0xff;
            if (j & CHF_STICK_DOWN)  y = 0xff;
            if (j & CHF_STICK_UP)    y = 0;
            in->pot[s][0] = x;
            in->pot[s][1] = y;
        }
    }

    if (m->reset_live != m->reset_held) {
        m->reset_held = m->reset_live;
        if (m->reset_held)
            s2650_reset(&m->cpu);       /* the RESET key: the 2650 only */
    }
}

/* ---- running --------------------------------------------------------------------- */

/* fire every line tick the CPU has reached; a completed frame is copied out */
static void fire_ticks(chf_machine_t *m)
{
    while (m->cpu.cycles >= m->next_limit) {
        uvi_line(&m->uvi);
        m->line_k++;
        m->next_limit = line_limit(m, m->line_k + 1);
        if (m->uvi.line_count % (uint64_t)m->uvi.lines == 0) {
            memcpy(m->fb, m->uvi.bitmap, sizeof m->fb);
            m->frames_done++;
        }
    }
}

static int step_inner(chf_machine_t *m)
{
    int cycles;

    fire_ticks(m);
    /* run_frame stops at the tick that completes the frame, not an instruction later */
    if (m->frame_stop && m->frames_done >= m->frame_stop)
        return 0;
    /* a reset key held: the CPU does not run, time still passes */
    if (m->reset_held) {
        m->cpu.cycles += 6;
        return 6;
    }
    cycles = s2650_step(&m->cpu);
    return cycles;
}

int chf_machine_step_quiet(chf_machine_t *m)
{
    return step_inner(m);
}

int chf_machine_step(chf_machine_t *m)
{
    fire_ticks(m);
    if (m->frame_stop && m->frames_done >= m->frame_stop)
        return 0;
    if (m->instr_hook)
        m->instr_hook(m, m->instr_hook_user);
    return step_inner(m);
}

/* Mix the console's sound for [audio_cycles, t) into m->out (stereo float). */
static void mix_audio(chf_machine_t *m, uint64_t t)
{
    float mono[CHF_AUDIO_MAX];
    double exact;
    int n, i;

    if (t <= m->audio_cycles) {
        m->out_frames = 0;
        return;
    }
    exact = (double)(t - m->audio_cycles) * CHF_AUDIO_RATE / m->cycle_hz + m->audio_frac;
    n = (int)exact;
    m->audio_frac = exact - n;
    if (n > CHF_AUDIO_MAX)
        n = CHF_AUDIO_MAX;
    sound_run(&m->snd, (uint64_t)((double)t * SOUND_RATE / m->cycle_hz));
    sound_mix(&m->snd, mono, n, CHF_AUDIO_RATE);
    for (i = 0; i < n; i++) {
        m->out[2 * i] = mono[i] * m->audio_gain;
        m->out[2 * i + 1] = mono[i] * m->audio_gain;
    }
    m->out_frames = n;
    m->audio_samples += (uint64_t)n;
    m->audio_cycles = t;
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
    mix_audio(m, m->cpu.cycles);
}

void chf_machine_snapshot(chf_machine_t *m, uint32_t *dst)
{
    uvi_frame_in_progress(&m->uvi, dst);
}

/* ---- the debugger's views ---------------------------------------------------------- */

uint8_t chf_machine_pad(const chf_machine_t *m, int n)
{
    return uvi_read(&m->uvi, 0x100 + (unsigned)n);
}

uint8_t chf_machine_peek(chf_machine_t *m, int space, uint32_t addr)
{
    switch (space) {
    case CHF_PEEK_PROG:
        return mem_get(m, (uint16_t)addr, 1);
    case CHF_PEEK_UVI:
        return uvi_read(&m->uvi, addr & 0x3ff);
    case CHF_PEEK_CART:
        if (m->fujinet)
            return chf_fujinet_image_byte(m->fujinet, addr);
        return m->cart && addr < m->cart->rom_size ? m->cart->rom[addr] : 0xff;
    default:
        return 0xff;
    }
}

void chf_machine_poke(chf_machine_t *m, int space, uint32_t addr, uint8_t data)
{
    switch (space) {
    case CHF_PEEK_PROG:
        addr &= 0x7fff;
        if (addr >= 0x1800 && addr <= 0x1aff)
            uvi_store(m, addr - 0x1800, data);
        else if (m->fujinet) {
            const int img = chfmap_decode(addr & 0x3fff);

            if (img >= 0)
                chf_fujinet_image_poke(m->fujinet, (uint32_t)img, data);
        }
        break;
    case CHF_PEEK_UVI:
        uvi_store(m, addr & 0x3ff, data);
        break;
    case CHF_PEEK_CART:
        if (m->fujinet)
            chf_fujinet_image_poke(m->fujinet, addr, data);
        break;
    }
}
