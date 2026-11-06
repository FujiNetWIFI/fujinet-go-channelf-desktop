/* chf_headless.c -- the machine with no host thread: boots it, runs N frames,
 * writes what it made. Side B of the MAME A/B runs (tools/ab/), and the way to
 * look at the core without a window. Built, not registered.
 *
 *   chf_headless [-machine channelf|channlf2|sabavdpl|sabavpl2]
 *               [-bios-dir DIR] [-bios sl31253|sl90025]
 *               [-cart F]      an app/CONFIG image on the FujiNet cartridge
 *               [-game F]      a game: its own Videocart, no FujiNet
 *               [-type std|maze|hangman|chess|multi_old|multi]  (with -game)
 *               [-boip HOST:PORT]  dial a FujiNet (default: none, link down)
 *               [-frames N] [-hash] [-ppm N FILE] [-raw FILE] [-vram FILE]
 *               [-iolog FILE] [-tick FILE] [-wav FILE] [-gain G]
 *               [-key FRAME:BIT:DOWN] [-ctl FRAME:PLAYER:MASK] [-reset FRAME:DOWN]
 *               [-softreset FRAME] [-async]
 *
 * The BIOS is the real one from -bios-dir (or $CHF_BIOS_DIR: sl31253.rom,
 * sl90025.rom and sl31254.rom); without one the app's stand-in runs. -bios picks
 * the first half (default: the machine's own).
 *
 * -hash prints "frame fnv64" for every frame (the A/B compares these with a
 * MAME run's); -ppm writes frame N as a PPM; -raw writes every frame as
 * 204 x 232 32-bit little-endian ARGB, in order; -vram writes, per frame, the
 * 8192 VRAM bytes then the port 0/1/4/5 latches (4 bytes), the way
 * tools/ab/chf_frames.lua does; -iolog lists every port write with the CPU
 * clock it happened at ("W <cycle> <port> <data>", and "F <frame> <cycle>" per
 * frame); -tick lists the FujiNet cartridge's 60 Hz tick, "<frame> <tick at
 * the vertical blank's exact time> <tick the console reads at $FC0E/F now>"; -wav records the sound as mono
 * 16-bit at 48 kHz. With no -cart/-game
 * CONFIG, the baked image, boots; its link is down (nothing is dialled) unless
 * -boip is given. Events: -key = the console buttons (bit 0 TIME .. 3 START),
 * -ctl = a hand controller's contacts (player 0 = right/port 1, 1 = left/port
 * 4), at the start of that frame; -reset holds or releases the console's RESET
 * button there; -softreset resets the F8 at once (MAME's soft reset) after that
 * frame has run.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chf_internal.h"

static uint8_t *slurp(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;

    if (!f) {
        fprintf(stderr, "chf_headless: cannot open %s\n", path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n ? (size_t)n : 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "chf_headless: cannot read %s\n", path);
        exit(2);
    }
    fclose(f);
    *size = (uint32_t)n;
    return buf;
}

static uint64_t fnv64(const uint32_t *px, size_t n)
{
    uint64_t h = 14695981039346656037ull;
    size_t i;

    for (i = 0; i < n; i++) {
        h ^= px[i] & 0xffffff;
        h *= 1099511628211ull;
    }
    return h;
}

static void write_ppm(const char *path, const uint32_t *px)
{
    FILE *f = fopen(path, "wb");
    int i;

    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", CHF_FB_WIDTH, CHF_FB_HEIGHT);
    for (i = 0; i < CHF_FB_WIDTH * CHF_FB_HEIGHT; i++) {
        uint8_t rgb[3] = { (uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i] };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static FILE *s_iolog;

static void log_access(chf_machine_t *m, void *user, int kind, uint16_t addr, uint8_t data)
{
    (void)user;
    if (kind == CHF_BUS_IOW)
        fprintf(s_iolog, "W %llu %u %u\n", (unsigned long long)m->cpu.cycles, (unsigned)addr, (unsigned)data);
}

typedef struct { int frame, a, b; } ev_t;

static int type_by_name(const char *n)
{
    if (!strcmp(n, "std"))       return CHF_CART_STD;
    if (!strcmp(n, "maze"))      return CHF_CART_MAZE;
    if (!strcmp(n, "hangman"))   return CHF_CART_HANGMAN;
    if (!strcmp(n, "chess"))     return CHF_CART_CHESS;
    if (!strcmp(n, "multi_old")) return CHF_CART_MULTI_OLD;
    if (!strcmp(n, "multi"))     return CHF_CART_MULTI;
    fprintf(stderr, "chf_headless: unknown cart type %s\n", n);
    exit(2);
}

int main(int argc, char **argv)
{
    static chf_machine_t m;
    static uint8_t bios_lo[CF_BIOS_HALF], bios_hi[CF_BIOS_HALF];
    chf_fujinet_t *fj = NULL;
    chf_cart_t mapper;
    chf_machine_opts_t mo;
    uint8_t *cart = NULL, *game = NULL;
    uint32_t cart_n = 0, game_n = 0;
    const char *ppm_path = NULL, *wav_path = NULL, *boip = "", *raw_path = NULL, *iolog_path = NULL;
    const char *tick_path = NULL, *vram_path = NULL, *bios_dir = getenv("CHF_BIOS_DIR"), *bios_name = NULL;
    FILE *raw = NULL, *vramf = NULL, *tickf = NULL;
    int ppm_frame = -1, frames = 60, hash = 0, machine = CHF_MACHINE_CHANNELF, type = CHF_CART_NONE;
    bool sync = true;
    float gain = 1.0f;
    ev_t keys[256], ctls[256], resets[64];
    int softresets[64];
    int nkeys = 0, nctls = 0, nresets = 0, nsoft = 0, i, f;
    FILE *wav = NULL;
    uint32_t wav_bytes = 0;
    char why[160];
    const char *err = NULL;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (!strcmp(a, "-machine") && i + 1 < argc) {
            const char *n = argv[++i];

            machine = !strcmp(n, "channlf2") ? CHF_MACHINE_CHANNELF2
                    : !strcmp(n, "sabavdpl") ? CHF_MACHINE_VIDEOPLAY
                    : !strcmp(n, "sabavpl2") ? CHF_MACHINE_VIDEOPLAY2 : CHF_MACHINE_CHANNELF;
        } else if (!strcmp(a, "-bios-dir") && i + 1 < argc) bios_dir = argv[++i];
        else if (!strcmp(a, "-bios") && i + 1 < argc) bios_name = argv[++i];
        else if (!strcmp(a, "-cart") && i + 1 < argc) cart = slurp(argv[++i], &cart_n);
        else if (!strcmp(a, "-game") && i + 1 < argc) game = slurp(argv[++i], &game_n);
        else if (!strcmp(a, "-type") && i + 1 < argc) type = type_by_name(argv[++i]);
        else if (!strcmp(a, "-frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(a, "-hash")) hash = 1;
        else if (!strcmp(a, "-async")) sync = false;
        else if (!strcmp(a, "-boip") && i + 1 < argc) boip = argv[++i];
        else if (!strcmp(a, "-gain") && i + 1 < argc) gain = (float)atof(argv[++i]);
        else if (!strcmp(a, "-ppm") && i + 2 < argc) {
            ppm_frame = atoi(argv[++i]);
            ppm_path = argv[++i];
        } else if (!strcmp(a, "-wav") && i + 1 < argc) wav_path = argv[++i];
        else if (!strcmp(a, "-raw") && i + 1 < argc) raw_path = argv[++i];
        else if (!strcmp(a, "-vram") && i + 1 < argc) vram_path = argv[++i];
        else if (!strcmp(a, "-iolog") && i + 1 < argc) iolog_path = argv[++i];
        else if (!strcmp(a, "-tick") && i + 1 < argc) tick_path = argv[++i];
        else if (!strcmp(a, "-key") && i + 1 < argc && nkeys < 256) {
            ev_t k;
            if (sscanf(argv[++i], "%d:%d:%d", &k.frame, &k.a, &k.b) == 3)
                keys[nkeys++] = k;
        } else if (!strcmp(a, "-ctl") && i + 1 < argc && nctls < 256) {
            ev_t c;
            if (sscanf(argv[++i], "%d:%d:%i", &c.frame, &c.a, &c.b) == 3)
                ctls[nctls++] = c;
        } else if (!strcmp(a, "-reset") && i + 1 < argc && nresets < 64) {
            ev_t r;
            if (sscanf(argv[++i], "%d:%d", &r.frame, &r.a) == 2)
                resets[nresets++] = r;
        } else if (!strcmp(a, "-softreset") && i + 1 < argc && nsoft < 64) {
            softresets[nsoft++] = atoi(argv[++i]);
        } else {
            fprintf(stderr, "chf_headless: unknown option %s\n", a);
            return 2;
        }
    }

    memset(&mo, 0, sizeof mo);
    mo.machine = machine;
    mo.bios_id = -1;
    if (bios_dir && bios_dir[0]) {
        const int pal2 = machine == CHF_MACHINE_CHANNELF2 || machine == CHF_MACHINE_VIDEOPLAY2;
        const char *first = bios_name ? bios_name : (pal2 ? "sl90025" : "sl31253");
        char path[1024];
        uint32_t n;
        uint8_t *p;

        snprintf(path, sizeof path, "%s/%s.rom", bios_dir, first);
        p = slurp(path, &n);
        memcpy(bios_lo, p, n < CF_BIOS_HALF ? n : CF_BIOS_HALF);
        free(p);
        snprintf(path, sizeof path, "%s/sl31254.rom", bios_dir);
        p = slurp(path, &n);
        memcpy(bios_hi, p, n < CF_BIOS_HALF ? n : CF_BIOS_HALF);
        free(p);
        mo.bios_lo = bios_lo;
        mo.bios_hi = bios_hi;
        mo.bios_id = !strcmp(first, "sl90025") ? CHF_ROM_SL90025 : CHF_ROM_SL31253;
    }

    if (game) {
        if (chf_cart_init(&mapper, type != CHF_CART_NONE ? type : chf_cart_identify(game, game_n), game, game_n,
                          &err) != 0) {
            fprintf(stderr, "chf_headless: %s\n", err);
            return 2;
        }
        mo.cart = &mapper;
    } else {
        fj = chf_fujinet_create();
        if (chf_fujinet_power_on(fj, cart, cart_n, boip, sync, why, sizeof why) != 0) {
            fprintf(stderr, "chf_headless: %s\n", why);
            return 2;
        }
        mo.cart = chf_fujinet_cart(fj);
        mo.fujinet = fj;
    }
    chf_machine_init(&m, &mo);
    chf_machine_check_cart(&m);
    m.audio_gain = wav_path ? gain * m.route_gain : m.audio_gain;

    if (raw_path)
        raw = fopen(raw_path, "wb");
    if (vram_path)
        vramf = fopen(vram_path, "wb");
    if (tick_path)
        tickf = fopen(tick_path, "w");
    if (iolog_path) {
        s_iolog = fopen(iolog_path, "w");
        m.bus_hook = log_access;
        m.watch = CHF_WATCH_IOW;
    }
    if (wav_path) {
        uint8_t hdr[44] = { 0 };
        wav = fopen(wav_path, "wb");
        if (wav)
            fwrite(hdr, 1, sizeof hdr, wav);
    }

    for (f = 0; f < frames; f++) {
        int k;

        for (k = 0; k < nkeys; k++)
            if (keys[k].frame == f)
                m.panel_live = keys[k].b ? (m.panel_live | 1u << keys[k].a) : (m.panel_live & ~(1u << keys[k].a));
        for (k = 0; k < nctls; k++)
            if (ctls[k].frame == f)
                m.ctl_live[ctls[k].a & 1] = (uint8_t)ctls[k].b;
        for (k = 0; k < nresets; k++)
            if (resets[k].frame == f)
                m.reset_live = resets[k].a != 0;
        chf_machine_run_frame(&m);
        for (k = 0; k < nsoft; k++)
            if (softresets[k] == f)
                chf_machine_reset(&m);
        if (s_iolog)
            fprintf(s_iolog, "F %d %llu\n", f, (unsigned long long)m.cpu.cycles);
        if (hash)
            printf("%d %016llx\n", f, (unsigned long long)fnv64(m.fb, CHF_FB_WIDTH * CHF_FB_HEIGHT));
        if (raw) {
            static uint32_t argb[CHF_FB_WIDTH * CHF_FB_HEIGHT];
            int px;

            for (px = 0; px < CHF_FB_WIDTH * CHF_FB_HEIGHT; px++)
                argb[px] = 0xff000000u | (m.fb[px] & 0xffffffu);
            fwrite(argb, sizeof argb[0], CHF_FB_WIDTH * CHF_FB_HEIGHT, raw);
        }
        if (vramf) {
            fwrite(m.vid.vram, 1, CF_VRAM_SIZE, vramf);
            fwrite(m.vid.latch, 1, 4, vramf);
        }
        if (tickf) {
            /* the tick at the vertical blank's own time (what MAME's frame notifier reads: the
             * time is exact there), then what the console reads now, a few clocks later */
            const unsigned boundary = (unsigned)(((unsigned __int128)m.frames_done * m.frame_as * 60u) /
                                                 UINT64_C(1000000000000000000)) & 0xffff;

            fprintf(tickf, "%d %u %u\n", f, boundary,
                    (unsigned)chf_machine_peek(&m, CHF_PEEK_PROG, 0xfc0f) << 8 |
                        chf_machine_peek(&m, CHF_PEEK_PROG, 0xfc0e));
        }
        if (f == ppm_frame && ppm_path)
            write_ppm(ppm_path, m.fb);
        if (wav) {
            int16_t pcm[CHF_AUDIO_MAX];
            int n;

            for (n = 0; n < m.out_frames; n++) {
                float s = m.out[2 * n];
                {   /* MAME's wav writer: clamp(int(sample * 32768)) */
                    int v = (int)(s * 32768.0f);

                    pcm[n] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                }
            }
            fwrite(pcm, 2, (size_t)m.out_frames, wav);
            wav_bytes += (uint32_t)m.out_frames * 2u;
        }
    }
    if (raw)
        fclose(raw);
    if (vramf)
        fclose(vramf);
    if (tickf)
        fclose(tickf);
    if (s_iolog)
        fclose(s_iolog);
    if (wav) {
        uint8_t h[44];
        uint32_t rate = CHF_AUDIO_RATE, byterate = CHF_AUDIO_RATE * 2u, riff = 36 + wav_bytes;

        memcpy(h, "RIFF", 4); memcpy(h + 4, &riff, 4); memcpy(h + 8, "WAVEfmt ", 8);
        h[16] = 16; h[17] = h[18] = h[19] = 0; h[20] = 1; h[21] = 0; h[22] = 1; h[23] = 0;
        memcpy(h + 24, &rate, 4); memcpy(h + 28, &byterate, 4);
        h[32] = 2; h[33] = 0; h[34] = 16; h[35] = 0; memcpy(h + 36, "data", 4);
        memcpy(h + 40, &wav_bytes, 4);
        fseek(wav, 0, SEEK_SET);
        fwrite(h, 1, 44, wav);
        fclose(wav);
    }
    fprintf(stderr, "chf_headless: %d frames, pc=%04x, %llu instructions%s%s\n", frames, (unsigned)f8_pc(&m.cpu),
            (unsigned long long)m.cpu.instructions, m.parked ? ", PARKED: " : "", m.parked ? m.park_why : "");
    if (fj)
        chf_fujinet_destroy(fj);
    free(cart);
    free(game);
    return 0;
}
