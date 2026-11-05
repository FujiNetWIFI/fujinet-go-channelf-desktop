/*
 * no_sdl_link_test -- the core links and runs on its own: no SDL, no
 * session, no frontend. Links chf_core alone (a session symbol would fail
 * the link), runs the staged wire codec's self test, plans the baked CONFIG
 * client with the staged planner, and boots a machine for a frame (the console has
 * no BIOS, so there is nothing else to supply).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "chf_internal.h"
#include "fujibus.h"
#include "chfmap.h"
#include "fujiconfigrom.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

int main(void)
{
    static chf_machine_t m;
    chfmap_plan_t plan;
    chf_machine_opts_t mo;
    char why[160];
    chf_fujinet_t *fj;

    check(fujibus_selftest(), "the staged fujibus codec passes its self test");
    check(chfmap_plan(_configrom, FUJI_CONFIGROM_SIZE, &plan) == CHFMAP_OK,
          "the staged chfmap plans the baked CONFIG client");
    check(plan.mailbox_ok && plan.size == 0x2000, "CONFIG is an 8K image that claims the mailbox");
    {
        bool claims = false;

        check(chf_fujinet_check_image(_configrom, FUJI_CONFIGROM_SIZE, &claims, why, sizeof why) &&
              claims, "and a FujiNet app (it claims the mailbox)");
    }

    fj = chf_fujinet_create();
    check(fj != NULL, "the cartridge device creates");
    check(chf_fujinet_power_on(fj, NULL, 0, "", true, why, sizeof why) == 0,
          "it powers on with CONFIG");
    memset(&mo, 0, sizeof mo);
    mo.machine = CHF_MACHINE_ARCADIA;
    mo.cart = chf_fujinet_cart(fj);
    mo.fujinet = fj;
    chf_machine_init(&m, &mo);
    chf_machine_run_frame(&m);
    check(m.frame_count == 1, "a BIOS-less Channel F runs a frame");
    check(m.cpu.instructions > 1000, "and the 2650 ran");
    chf_fujinet_destroy(fj);

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
