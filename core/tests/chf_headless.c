/* chf_headless.c -- the machine with no host thread: boots it, runs N frames,
 * writes what it made. Side B of the MAME A/B runs (tools/ab/), and the way to
 * look at the core without a window. Built, not registered.
 *
 *   chf_headless [-machine ntsc|pal]
 *               [-cart F]      an app/CONFIG image on the FujiNet cartridge
 *               [-game F]      a game: its own mapper cartridge, no FujiNet
 *               [-boip HOST:PORT]  dial a FujiNet (default: none, link down)
 *               [-frames N] [-hash] [-ppm N FILE] [-raw FILE] [-buslog FILE]
 *               [-wav FILE] [-gain G]
 *               [-key FRAME:BIT:DOWN] [-joy FRAME:STICK:BITS]
 *               [-disc FRAME:STICK:X:Y] [-reset FRAME:DOWN]
 *
 * -hash prints "frame fnv64" for every frame (the A/B compares these with a
 * MAME run's); -ppm writes frame N as a PPM; -raw writes every frame as
 * 192x262 32-bit little-endian ARGB, in order (tools/ab/cart_ab.py compares it
 * with the same dump from MAME); -wav records the sound as mono 16-bit at
 * 48 kHz; -buslog lists every UVI write ($1800-$1AFF) with the CPU cycle it
 * happened at. With no -cart/-game CONFIG, the baked image, boots; its link is
 * down (nothing is dialled) unless -boip is given.
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

static FILE *s_buslog;

static void log_access(chf_machine_t *m, void *user, int kind, uint16_t addr, uint8_t data)
{
    (void)user;
    if (addr >= 0x1800 && addr <= 0x1aff)
        fprintf(s_buslog, "%c %llu %04x %02x %d\n", kind == CHF_BUS_MEMW ? 'W' : 'R',
                (unsigned long long)m->cpu.cycles, (unsigned)addr, (unsigned)data, m->uvi.line);
}

typedef struct { int frame, bit, down; } key_ev;
typedef struct { int frame, stick, bits; } joy_ev;
typedef struct { int frame, stick, x, y; } disc_ev;
typedef struct { int frame, down; } reset_ev;

int main(int argc, char **argv)
{
    static chf_machine_t m;
    chf_fujinet_t *fj;
    chf_cart_t mapper;
    chf_machine_opts_t mo;
    uint8_t *cart = NULL, *game = NULL;
    uint32_t cart_n = 0, game_n = 0;
    const char *ppm_path = NULL, *wav_path = NULL, *boip = "", *raw_path = NULL, *buslog_path = NULL;
    FILE *raw = NULL;
    int ppm_frame = -1, frames = 60, hash = 0, machine = CHF_MACHINE_ARCADIA;
    float gain = 1.0f;
    key_ev keys[256];
    joy_ev joys[256];
    disc_ev discs[256];
    reset_ev resets[64];
    int nkeys = 0, njoys = 0, ndiscs = 0, nresets = 0, i, f;
    FILE *wav = NULL;
    uint32_t wav_bytes = 0;
    char why[160];
    const char *err = NULL;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (!strcmp(a, "-machine") && i + 1 < argc)
            machine = !strcmp(argv[++i], "pal") ? CHF_MACHINE_ARCADIA_PAL : CHF_MACHINE_ARCADIA;
        else if (!strcmp(a, "-cart") && i + 1 < argc) cart = slurp(argv[++i], &cart_n);
        else if (!strcmp(a, "-game") && i + 1 < argc) game = slurp(argv[++i], &game_n);
        else if (!strcmp(a, "-frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(a, "-hash")) hash = 1;
        else if (!strcmp(a, "-boip") && i + 1 < argc) boip = argv[++i];
        else if (!strcmp(a, "-gain") && i + 1 < argc) gain = (float)atof(argv[++i]);
        else if (!strcmp(a, "-ppm") && i + 2 < argc) {
            ppm_frame = atoi(argv[++i]);
            ppm_path = argv[++i];
        } else if (!strcmp(a, "-wav") && i + 1 < argc) wav_path = argv[++i];
        else if (!strcmp(a, "-raw") && i + 1 < argc) raw_path = argv[++i];
        else if (!strcmp(a, "-buslog") && i + 1 < argc) buslog_path = argv[++i];
        else if (!strcmp(a, "-key") && i + 1 < argc && nkeys < 256) {
            key_ev k;
            if (sscanf(argv[++i], "%d:%d:%d", &k.frame, &k.bit, &k.down) == 3)
                keys[nkeys++] = k;
        } else if (!strcmp(a, "-joy") && i + 1 < argc && njoys < 256) {
            joy_ev j;
            if (sscanf(argv[++i], "%d:%d:%i", &j.frame, &j.stick, &j.bits) == 3)
                joys[njoys++] = j;
        } else if (!strcmp(a, "-disc") && i + 1 < argc && ndiscs < 256) {
            disc_ev d;
            if (sscanf(argv[++i], "%d:%d:%d:%d", &d.frame, &d.stick, &d.x, &d.y) == 4)
                discs[ndiscs++] = d;
        } else if (!strcmp(a, "-reset") && i + 1 < argc && nresets < 64) {
            reset_ev r;
            if (sscanf(argv[++i], "%d:%d", &r.frame, &r.down) == 2)
                resets[nresets++] = r;
        } else {
            fprintf(stderr, "chf_headless: unknown option %s\n", a);
            return 2;
        }
    }

    memset(&mo, 0, sizeof mo);
    mo.machine = machine;

    fj = chf_fujinet_create();
    if (game) {
        if (chf_cart_init(&mapper, chf_cart_identify(game, game_n), game, game_n, &err) != 0) {
            fprintf(stderr, "chf_headless: %s\n", err);
            return 2;
        }
        mo.cart = &mapper;
    } else {
        if (chf_fujinet_power_on(fj, cart, cart_n, boip, true, why, sizeof why) != 0) {
            fprintf(stderr, "chf_headless: %s\n", why);
            return 2;
        }
        mo.cart = chf_fujinet_cart(fj);
        mo.fujinet = fj;
    }
    chf_machine_init(&m, &mo);
    m.audio_gain = gain;

    if (raw_path)
        raw = fopen(raw_path, "wb");
    if (buslog_path) {
        s_buslog = fopen(buslog_path, "w");
        m.bus_hook = log_access;
        m.watch = CHF_WATCH_MEMR | CHF_WATCH_MEMW;
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
                m.keys_live = keys[k].down ? (m.keys_live | 1ull << keys[k].bit)
                                           : (m.keys_live & ~(1ull << keys[k].bit));
        for (k = 0; k < njoys; k++)
            if (joys[k].frame == f)
                m.joy_live[joys[k].stick & 1] = (uint8_t)joys[k].bits;
        for (k = 0; k < ndiscs; k++)
            if (discs[k].frame == f) {
                m.disc_live[discs[k].stick & 1][0] = (int8_t)discs[k].x;
                m.disc_live[discs[k].stick & 1][1] = (int8_t)discs[k].y;
            }
        for (k = 0; k < nresets; k++)
            if (resets[k].frame == f)
                m.reset_live = resets[k].down != 0;
        chf_machine_run_frame(&m);
        if (s_buslog)
            fprintf(s_buslog, "F %d %llu\n", f, (unsigned long long)m.cpu.cycles);
        if (hash)
            printf("%d %016llx\n", f, (unsigned long long)fnv64(m.fb, CHF_FB_WIDTH * CHF_FB_HEIGHT));
        if (raw) {
            static uint32_t argb[CHF_FB_WIDTH * CHF_FB_HEIGHT];
            int px;

            for (px = 0; px < CHF_FB_WIDTH * CHF_FB_HEIGHT; px++)
                argb[px] = 0xff000000u | (m.fb[px] & 0xffffffu);
            fwrite(argb, sizeof argb[0], CHF_FB_WIDTH * CHF_FB_HEIGHT, raw);
        }
        if (f == ppm_frame && ppm_path)
            write_ppm(ppm_path, m.fb);
        if (wav) {
            int16_t pcm[CHF_AUDIO_MAX];
            int n;

            for (n = 0; n < m.out_frames; n++) {
                float s = m.out[2 * n];
                pcm[n] = (int16_t)(s > 1.0f ? 32767 : s < -1.0f ? -32767 : (int)(s * 32767.0f));
            }
            fwrite(pcm, 2, (size_t)m.out_frames, wav);
            wav_bytes += (uint32_t)m.out_frames * 2u;
        }
    }
    if (raw)
        fclose(raw);
    if (s_buslog)
        fclose(s_buslog);
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
    fprintf(stderr, "chf_headless: %d frames, pc=%04x, %llu instructions\n", frames,
            (unsigned)s2650_pc(&m.cpu), (unsigned long long)m.cpu.instructions);
    chf_fujinet_destroy(fj);
    return 0;
}
