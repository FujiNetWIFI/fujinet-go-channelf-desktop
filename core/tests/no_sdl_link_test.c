/*
 * no_sdl_link_test -- the core links and runs on its own: no SDL, no
 * session, no frontend. Links chf_core alone (a session symbol would fail
 * the link), runs the staged wire codec's self test, plans the baked CONFIG
 * client with the staged planner, and boots the host for a few frames on the
 * app's stand-in BIOS (no ROM of any kind is needed).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "host.h"
#include "fujibus.h"
#include "chfmap.h"
#include "fujiconfigrom.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

int main(void)
{
    chfmap_plan_t plan;
    chf_host_opts_t ho;
    char why[160];
    bool claims = false;
    int waited = 0;

    check(fujibus_selftest(), "the staged fujibus codec passes its self test");
    check(chfmap_plan(_configrom, FUJI_CONFIGROM_SIZE, &plan) == CHFMAP_OK,
          "the staged chfmap plans the baked CONFIG client");
    check(plan.mailbox_ok && plan.size == CHFMAP_WINDOW, "CONFIG is a 16K image that claims the mailbox");
    check(chf_fujinet_check_image(_configrom, FUJI_CONFIGROM_SIZE, &claims, why, sizeof why) && claims,
          "and a FujiNet app (it claims the mailbox)");

    memset(&ho, 0, sizeof ho);
    ho.machine = CHF_MACHINE_CHANNELF;
    ho.bios_id = -1;                     /* the stand-in */
    ho.cart_type = CHF_CART_FUJINET;
    ho.cart_app = true;                  /* CONFIG */
    ho.boip_hostport = "";               /* FujiNet switched off: nothing is dialled */
    ho.cart_sync = true;
    check(chf_host_start(&ho) == 0, "the host starts CONFIG on the stand-in");
    while (waited < 10000 && chf_host_frame_count() < 5) {
        sleep_ms(10);
        waited += 10;
    }
    check(chf_host_frame_count() >= 5, "a BIOS-less Channel F runs frames");
    check(chf_host_missing(why, sizeof why) == 0, "and CONFIG needs nothing the stand-in lacks");
    chf_host_stop();
    check(!chf_host_is_running(), "and stops");

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
