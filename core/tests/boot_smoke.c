/*
 * boot_smoke -- the paced host starts, paints and keeps real time.
 *
 * Starts the host thread (core/chf/host.h) on the tests' own BIOS and a Videocart that
 * draws a pattern (no ROM needed), waits for frames, and checks that the picture was
 * published (204 x 232 with lit pixels), that the frame rate is the machine's own -- the
 * NTSC console's 60.53 Hz and the PAL console's 50.08 Hz, within 2% -- and that a console
 * with only the stand-in BIOS parks on a Videocart. The siblings' hardest-won lesson:
 * verify the throttle (one once free-ran at >33000%).
 *
 * The rate is only held to 2% where the OS can sleep for a frame without gross overshoot:
 * a loaded CI virtual machine (the macOS runners above all) turns a 16.7 ms sleep into
 * 50, which says nothing about the pacing code. There the test checks what still means
 * something -- the machine is throttled, and the deadline ladder sleeps.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "test_chf.h"

static int failures;
static int accurate_sleeps = 1;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* a Videocart: plot (row 10, column 20, colour 2) once, then count in the scratchpad forever */
static const uint8_t k_draw[] = { 0x20, 0x7f, 0xb1, 0x20, 0xeb, 0xb4, 0x20, 0x35, 0xb5, 0x20, 0x60, 0xb0,
                                  0x20, 0x50, 0xb0, 0x42, 0x24, 0x01, 0x52, 0x90, 0xfb };

static void run(int machine, double want_hz)
{
    static uint32_t fb[CHF_FB_WIDTH * CHF_FB_HEIGHT];
    static uint8_t cart[0x800], lo[CF_BIOS_HALF], hi[CF_BIOS_HALF];
    chf_host_opts_t o;
    uint64_t serial = 0, f0, f1;
    int lit = 0;
    double t0, t1, hz;
    char what[160];

    test_bios(lo, hi);
    test_cart(cart, sizeof cart, k_draw, sizeof k_draw, 0);
    memset(&o, 0, sizeof o);
    o.machine = machine;
    o.bios_lo = lo;
    o.bios_hi = hi;
    o.bios_id = CHF_ROM_SL31253;
    o.cart = cart;
    o.cart_size = sizeof cart;
    o.cart_type = CHF_CART_STD;
    o.cart_app = false;
    if (chf_host_start(&o) != 0) {
        printf("FAIL: start: %s\n", chf_host_last_error());
        failures++;
        return;
    }
    for (int waited = 0; waited < 5000 && lit < 8; waited += 100) {
        test_sleep_ms(100);
        if (!chf_host_frame_copy(fb, &serial))
            continue;
        lit = 0;
        for (int i = 0; i < CHF_FB_WIDTH * CHF_FB_HEIGHT; i++)
            if (fb[i] != 0x101010)
                lit++;
    }
    check(serial != 0, "a frame was published");
    snprintf(what, sizeof what, "the Videocart's pixel is on the picture (%d non-background pixels)", lit);
    check(lit == 8, what);
    check(fb[(10 - 4) * 4 * CHF_FB_WIDTH + (20 - 4) * 2] == 0xfdfdfd, "at its cell, white (palette 0)");
    check(chf_host_missing(NULL, 0) == 0, "nothing is missing (the tests' BIOS is a BIOS)");

    f0 = chf_host_frame_count();
    t0 = now_s();
    test_sleep_ms(2000);
    f1 = chf_host_frame_count();
    t1 = now_s();
    hz = (double)(f1 - f0) / (t1 - t0);
    snprintf(what, sizeof what, "the host reports %.4f Hz (want %.4f)", chf_host_frame_rate(), want_hz);
    check(chf_host_frame_rate() > want_hz - 0.01 && chf_host_frame_rate() < want_hz + 0.01, what);
    chf_host_stop();
    {
        chf_host_pacing_t pc;
        chf_host_pacing(&pc);
        printf("  pacing: %llu sleeps, %.2f ms late on average, worst %.2f ms, %llu resyncs\n",
               (unsigned long long)pc.sleeps, pc.sleeps ? (double)pc.late_ns / (double)pc.sleeps / 1e6 : 0.0,
               (double)pc.worst_late_ns / 1e6, (unsigned long long)pc.resyncs);
        snprintf(what, sizeof what, "the machine is throttled: %.2f Hz, not free-running", hz);
        check(hz < want_hz * 1.10 && pc.sleeps > 0, what);
        snprintf(what, sizeof what, "it paces at %.2f Hz (want %.2f)", hz, want_hz);
        if (accurate_sleeps)
            check(hz > want_hz * 0.98 && hz < want_hz * 1.02, what);
        else
            printf("note: %s -- not asserted, this OS's sleeps are coarse\n", what);
    }
}

/* How fast this machine runs the core with no host and no throttle. */
static double capacity(void)
{
    static test_rig r;
    static uint8_t cart[0x800];
    double t0, t1;
    const int frames = 120;

    test_cart(cart, sizeof cart, k_draw, sizeof k_draw, 0);
    test_rig_init(&r, CHF_MACHINE_CHANNELF, CHF_CART_STD, cart, sizeof cart);
    t0 = now_s();
    for (int f = 0; f < frames; f++) {
        chf_machine_run_frame(&r.m);
        r.m.out_frames = 0;
    }
    t1 = now_s();
    return frames / (t1 - t0);
}

int main(void)
{
    /* ten 16.7 ms sleeps: 167 ms on a machine that can sleep for a frame */
    {
        double t0 = now_s(), took;
        for (int i = 0; i < 10; i++)
            nanosleep(&(struct timespec){ 0, 16666667L }, NULL);
        took = now_s() - t0;
        accurate_sleeps = took < 0.25;
        printf("  ten 16.7 ms sleeps took %.3f s (%s)\n", took,
               accurate_sleeps ? "accurate" : "coarse: the rate is reported, not asserted");
    }
    printf("  the bare core runs %.0f frames/s here\n", capacity());
    run(CHF_MACHINE_CHANNELF, 60.5307);
    run(CHF_MACHINE_VIDEOPLAY, 50.0801);
    {
        /* with only the stand-in, a Videocart parks the console and the host says so */
        static uint8_t cart[0x800];
        chf_host_opts_t o;
        char why[120];

        test_cart(cart, sizeof cart, k_draw, sizeof k_draw, 0);
        memset(&o, 0, sizeof o);
        o.machine = CHF_MACHINE_CHANNELF;
        o.bios_id = -1;
        o.cart = cart;
        o.cart_size = sizeof cart;
        o.cart_type = CHF_CART_STD;
        check(chf_host_start(&o) == 0, "the stand-in starts with a Videocart");
        check(chf_host_missing(why, sizeof why) == CHF_MISSING_BIOS && why[0], "and says it needs the BIOS");
        chf_host_stop();
    }
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
