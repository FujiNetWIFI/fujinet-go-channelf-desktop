/*
 * chf_drive -- drive the whole app headless, like a user: a session with
 * the in-process FujiNet, a script of presses and waits, and screenshots.
 * For exploring CONFIG and checking a flow end to end; built, not
 * registered.
 *
 *   chf_drive [--cart image] [--bios file|zip|folder]... [--import-sd file]... SCRIPT
 *
 * SCRIPT is a sequence of words:
 *   wait:MS            let the machine run
 *   press:TARGET[:MS]  hold a control (default 100 ms), then release it;
 *                      TARGET is a console button (1-4, time, mode, hold,
 *                      start), a controller contact (p1 or p2, then right,
 *                      left, back, forward, ccw, cw, pull or push: p1push),
 *                      reset, or config (Reset to CONFIG)
 *   shot:NAME          write the current frame to NAME.ppm
 *   status             print the cartridge's status
 *   until:TEXT:MS      wait up to MS for the status to contain TEXT
 *
 * Settings and data go in a fresh tree under $TMPDIR; --bios imports a
 * BIOS ROM or zip or folder into it (never the user's own ROM directory). The FujiNet runtime
 * takes stdout and stderr for its Console Log and changes directory, so
 * this prints to its own copy of stdout and resolves NAME against the
 * directory it started in.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "chfsession.h"
#include "test_tmpdir.h"

static FILE *out;               /* stdout, before the runtime took it */
static char cwd[1024];

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* a console button (1-4 or its legend), pNcontact, reset; config is a system
 * action (-2) */
static int target_of(const char *name)
{
    static const char *const acts[CHF_ACT_PER_PORT] = {
        "right", "left", "back", "forward", "ccw", "cw", "pull", "push",
    };

    if (name[0] == 'p' && (name[1] == '1' || name[1] == '2'))
    {
        for (int a = 0; a < CHF_ACT_PER_PORT; a++)
            if (!strcmp(name + 2, acts[a]))
                return CHF_TARGET_PORT(name[1] - '1', a);
        return -1;
    }
    if (!strcmp(name, "reset"))
        return CHF_TARGET_SWITCH(CHF_SW_RESET);
    if (!strcmp(name, "config"))
        return -2;
    for (int k = 0; k < CHF_KEY_COUNT; k++)
    {
        const char *l = chfsession_key_legend(CHF_MACHINE_CHANNELF, k);
        char low[16];
        int i = 0;
        for (; l[i] && i < 15; i++)
            low[i] = (char)((l[i] >= 'A' && l[i] <= 'Z') ? l[i] + 32 : l[i]);
        low[i] = 0;
        if (!strcmp(low, name) || !strcmp(chfsession_key_label(CHF_MACHINE_CHANNELF, k), name))
            return CHF_TARGET_KEY(k);
    }
    return -1;
}

static void shot(chfsession *s, const char *name)
{
    static uint32_t px[CHFSESSION_FB_WIDTH * CHFSESSION_FB_HEIGHT];
    uint64_t serial = 0;
    char path[1100];
    FILE *f;

    chfsession_copy_frame(s, px, &serial);
    if (name[0] == '/')
        snprintf(path, sizeof path, "%s.ppm", name);
    else
        snprintf(path, sizeof path, "%s/%s.ppm", cwd, name);
    f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", CHFSESSION_FB_WIDTH, CHFSESSION_FB_HEIGHT);
    for (int i = 0; i < CHFSESSION_FB_WIDTH * CHFSESSION_FB_HEIGHT; i++)
    {
        uint8_t rgb[3] = { (uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i] };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    fprintf(out, "shot %s\n", path);
}

int main(int argc, char **argv)
{
    char cfg[512], data[512], st[160], dest[1200];
    const char *cart = NULL;
    chfsession_paths p;
    chfsession *s;
    chfsession_start_opts o;
    int i;

    if (argc < 2)
    {
        fprintf(stderr, "usage: chf_drive [--cart image] [--bios file|zip|folder]... [--import-sd file]... SCRIPT\n");
        return 2;
    }
    out = fdopen(dup(STDOUT_FILENO), "w");
    if (!out || !getcwd(cwd, sizeof cwd))
        return 1;
    setvbuf(out, NULL, _IOLBF, 0);
    test_tmpdir(cfg, sizeof cfg, "drcfg");
    test_tmpdir(data, sizeof data, "drdata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg;
    p.data_dir = data;
    s = chfsession_new(&p);
    if (!s)
        return 1;

    for (i = 1; i < argc - 1; i++)
    {
        if (!strcmp(argv[i], "--cart"))
            cart = argv[++i];
        else if (!strcmp(argv[i], "--bios"))
        {
            char msg[512];
            chfsession_rom_import(s, argv[++i], msg, sizeof msg);
            fprintf(out, "bios: %s\n", msg);
        }
        else if (!strcmp(argv[i], "--import-sd"))
            i++;   /* after start: the SD tree exists once FujiNet is up */
        else
            break;
    }
    chfsession_default_opts(s, &o);
    o.enable_audio = 0;
    o.enable_gamepad = 0;
    o.cart_path = cart;
    if (chfsession_start(s, &o) != 0)
    {
        fprintf(out, "start: %s\n", chfsession_last_error(s));
        return 1;
    }
    for (int k = 1; k < argc - 1; k++)
        if (!strcmp(argv[k], "--import-sd"))
        {
            if (chfsession_import_cart_to_sd(s, argv[k + 1], dest, sizeof dest) == 0)
                fprintf(out, "imported %s\n", dest);
            else
                fprintf(out, "import failed: %s\n", chfsession_last_error(s));
            k++;
        }

    {
        char *script = strdup(argv[argc - 1]);
        for (char *w = strtok(script, " \n"); w; w = strtok(NULL, " \n"))
        {
            if (!strncmp(w, "wait:", 5))
                sleep_ms(atoi(w + 5));
            else if (!strncmp(w, "press:", 6))
            {
                char name[32];
                int ms = 100, t;
                const char *c = strchr(w + 6, ':');
                snprintf(name, sizeof name, "%.*s", c ? (int)(c - (w + 6)) : (int)strlen(w + 6), w + 6);
                if (c)
                    ms = atoi(c + 1);
                t = target_of(name);
                if (t == -2) { chfsession_sysaction(s, CHF_SYSACT_RESET_CONFIG); continue; }
                if (t < 0) { fprintf(out, "no target %s\n", name); continue; }
                chfsession_press(s, t, 1);
                sleep_ms(ms);
                chfsession_press(s, t, 0);
                sleep_ms(60);
            }
            else if (!strncmp(w, "shot:", 5))
                shot(s, w + 5);
            else if (!strcmp(w, "status"))
            {
                chfsession_cart_status(s, st, sizeof st);
                fprintf(out, "status: %s (booted %d)\n", st,
                        chfsession_cart_booted_game(s));
            }
            else if (!strncmp(w, "until:", 6))
            {
                char text[64];
                const char *c = strrchr(w + 6, ':');
                int ms = c ? atoi(c + 1) : 5000, waited = 0;
                snprintf(text, sizeof text, "%.*s", c ? (int)(c - (w + 6)) : (int)strlen(w + 6), w + 6);
                for (;;)
                {
                    chfsession_cart_status(s, st, sizeof st);
                    if (strstr(st, text) || waited >= ms)
                        break;
                    sleep_ms(50);
                    waited += 50;
                }
                fprintf(out, "until %s: %s after %d ms\n", text, st, waited);
            }
        }
        free(script);
    }
    chfsession_stop(s);
    chfsession_free(s);
    return 0;
}
