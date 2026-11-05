/*
 * chf_drive -- drive the whole app headless, like a user: a session with
 * the in-process FujiNet, a script of presses and waits, and screenshots.
 * For exploring CONFIG and checking a flow end to end; built, not
 * registered.
 *
 *   chf_drive [--cart image] [--rom file|zip|folder]... [--import-sd file]... SCRIPT
 *
 * SCRIPT is a sequence of words:
 *   wait:MS            let the machine run
 *   press:TARGET[:MS]  hold a control (default 100 ms), then release it;
 *                      TARGET is a key's label (q, 7, enter, clear, space, yes,
 *                      no, plus, minus, times, divide, equals, dot, ?), j1up,
 *                      j1down, j1left, j1right, j1fire (and j2...), reset, or
 *                      config (Reset to CONFIG)
 *   shot:NAME          write the current frame to NAME.ppm
 *   status             print the cartridge's status
 *   until:TEXT:MS      wait up to MS for the status to contain TEXT
 *   text               print the cart's text engine screen
 *
 * Settings and data go in a fresh tree under $TMPDIR; --bios imports a
 * ROM or zip or folder into it (never the user's own ROM directory). The FujiNet runtime
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
#include "chfdebug.h"
#include "test_tmpdir.h"

static FILE *out;               /* stdout, before the runtime took it */
static char cwd[1024];

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* a key's label, jNdir / jNfire, reset; config is a system action (-2) */
static int target_of(const char *name)
{
    int n;
    const chf_keycap *caps = chfsession_keyboard_layout(CHF_MACHINE_ARCADIA, &n, NULL, NULL);
    static const char *const dirs[] = { "up", "right", "down", "left", "fire" };

    if (name[0] == 'j' && (name[1] == '1' || name[1] == '2'))
    {
        for (int d = 0; d < 5; d++)
            if (!strcmp(name + 2, dirs[d]))
                return CHF_TARGET_PORT(name[1] - '1', d);
        return -1;
    }
    if (!strcmp(name, "reset"))
        return CHF_TARGET_SWITCH(CHF_SW_RESET);
    if (!strcmp(name, "config"))
        return -2;
    for (int i = 0; i < n; i++)
    {
        const char *l = caps[i].label;
        char low[16];
        int k = 0;
        for (; l[k] && k < 15; k++)
            low[k] = (char)((l[k] >= 'A' && l[k] <= 'Z') ? l[k] + 32 : l[k]);
        low[k] = 0;
        if (caps[i].target >= 0 && !strcmp(low, name))
            return CHF_TARGET_KEY(caps[i].target);
    }
    if (!strcmp(name, "plus"))   return CHF_TARGET_KEY(16);
    if (!strcmp(name, "minus"))  return CHF_TARGET_KEY(40);
    if (!strcmp(name, "times"))  return CHF_TARGET_KEY(41);
    if (!strcmp(name, "divide")) return CHF_TARGET_KEY(42);
    if (!strcmp(name, "equals")) return CHF_TARGET_KEY(43);
    if (!strcmp(name, "dot"))    return CHF_TARGET_KEY(39);
    if (!strcmp(name, "space"))  return CHF_TARGET_KEY(12);
    return -1;
}

static void shot(chfsession *s, const char *name)
{
    static uint32_t px[CHFSESSION_FB_WIDTH * CHFSESSION_FB_HEIGHT];
    uint64_t serial = 0;
    char path[512];
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
        fprintf(stderr, "usage: chf_drive [--cart image] [--bios file] [--import-sd file]... SCRIPT\n");
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
            else if (!strcmp(w, "text"))
            {
                static char text[4096];
                chfdebug_command(chfsession_debugger(s), "text", text, sizeof text);
                fprintf(out, "%s", text);
            }
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
