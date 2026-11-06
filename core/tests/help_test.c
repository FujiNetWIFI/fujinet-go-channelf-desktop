/*
 * help_test -- the Help covers what the app has: every Preferences setting
 * and choice, every machine, every ROM, every default key and gamepad button,
 * the app's shortcuts (and the Mac's), the two ports, the debugger's tabs
 * and prompt commands, and every context a Help button opens. Every miss is
 * listed before it fails.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chfhelp.h"
#include "chfsession.h"
#include "test_tmpdir.h"

#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); exit(1); } } while (0)

static const unsigned platforms[] = {
    CHFHELP_GNOME, CHFHELP_KDE, CHFHELP_WINDOWS, CHFHELP_MACOS
};
static const char *const platform_names[] = { "GNOME", "KDE", "Windows", "macOS" };

/* What the frontends' Help buttons and menus open. */
static const char *const contexts[] = {
    "main", "nobios", "import-sd",
    "prefs/Machine", "prefs/Display", "prefs/Input", "prefs/FujiNet", "prefs/ROMs",
    "debugger/prompt", "debugger/cpu", "debugger/disassembly", "debugger/video",
    "debugger/io", "debugger/breakpoints", "debugger/cart", "debugger/fujinet",
    "controllers", "fujilog",
};

/* The prompt's commands (core/include/chfdebug.h). */
static const char *const debug_commands[] = {
    "help", "step", "over", "out", "frame", "scanline", "run", "stop", "runto", "break",
    "bpr", "bpw", "bpi", "bpo", "bpv", "bps", "delete", "enable", "disable", "clear", "breaks",
    "print", "regs", "set", "mem", "poke", "scratch", "isar", "kstack", "vram", "pal", "video",
    "ports", "sound", "input", "cart", "mailbox", "bios", "romc", "label", "labels", "disasm",
    "trace", "save",
};

/* The Preferences pages: what each one has, as the frontends label it. */
static const struct {
    const char *context;
    const char *labels[10];
} pages[] = {
    { "prefs/Machine", { "Machine", "BIOS", NULL } },
    { "prefs/Display", { "Aspect", "Television (4:3)", "Square pixels", "Smooth scaling",
                         "Enable audio", "Volume", NULL } },
    { "prefs/Input", { "Enable gamepads", "Right stick works the knob", "Gamepads", "Automatic",
                       "Player 1", "Player 2", "Controls", NULL } },
    { "prefs/FujiNet", { "Enable FujiNet", "Use external FujiNet", "Address", "Web UI", NULL } },
    { "prefs/ROMs", { "Import", "Import a Folder", "Imported", "Not imported", NULL } },
};

/* The settings keys the pages write. */
static const char *const settings_keys[] = {
    "machine", "bios", "right_stick", "enable_fujinet", "fujinet_external", "fujinet_external_addr",
    "enable_audio", "enable_gamepad", "volume", "cart", "aspect", "smooth",
};

/* The default keys' rows, in the order of chf_action. */
static const char *const act_rows[CHF_ACT_PER_PORT] = {
    "Stick right", "Stick left", "Stick back", "Stick forward",
    "Twist counter-clockwise", "Twist clockwise", "Pull", "Push",
};

/* The app's shortcuts, as a PC and as a Mac shows them. */
static const struct { const char *pc, *mac; } shortcuts[] = {
    { "Ctrl+O", "⌘O" }, { "Ctrl+R", "⌘R" }, { "Ctrl+,", "⌘," }, { "Ctrl+Q", "⌘Q" },
    { "Ctrl+Shift+H", "⌘?" }, { "F9", "F9" }, { "F11", "F11" }, { "F12", "F12" },
};

static int misses;

/* Lower case, every run of white space one space. */
static void
fold(const char *in, char *out, size_t outsz)
{
    size_t n = 0;
    int space = 0;
    for (; *in && n + 1 < outsz; in++) {
        unsigned char c = (unsigned char) *in;
        if (c == ' ' || c == '\n' || c == '\t') {
            space = 1;
            continue;
        }
        if (space && n)
            out[n++] = ' ';
        space = 0;
        out[n++] = (char) ((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    out[n] = 0;
}

static char section[1 << 18];
static char raw[1 << 18];

/* The folded text of a context's (or slug's) section on platform p; raw
 * keeps the lines. */
static const char *
section_text(int p, const char *context)
{
    int t, b;
    section[0] = 0;
    raw[0] = 0;
    if (chfhelp_find(context, &t, &b) == 0) {
        chfhelp_plain_text(platforms[p], t, b, raw, sizeof raw);
        fold(raw, section, sizeof section);
    }
    return section;
}

static void
require(int p, const char *context, const char *what, const char *text)
{
    char want[512];
    fold(text, want, sizeof want);
    if (!strstr(section_text(p, context), want)) {
        fprintf(stderr, "MISSING (%s) %s: %s \"%s\"\n", platform_names[p], context, what, text);
        misses++;
    }
}

/* A line of the section (a table row: cells joined by " | ") with every one
 * of these, folded, in that order. */
static void
require_row(int p, const char *context, const char *a, const char *b, const char *c)
{
    char line[1024], fa[256], fb[256], fc[256];
    const char *s, *e;
    int ok = 0;

    section_text(p, context);
    fold(a, fa, sizeof fa);
    fold(b ? b : "", fb, sizeof fb);
    fold(c ? c : "", fc, sizeof fc);
    for (s = raw; *s && !ok; s = *e ? e + 1 : e) {
        char *x, *y;
        size_t n;
        e = strchr(s, '\n');
        if (!e)
            e = s + strlen(s);
        n = (size_t) (e - s) < sizeof line - 1 ? (size_t) (e - s) : sizeof line - 1;
        memcpy(line, s, n);
        line[n] = 0;
        fold(line, line, sizeof line);
        x = strstr(line, fa);
        if (!x)
            continue;
        y = b && *fb ? strstr(x + strlen(fa), fb) : x + strlen(fa);
        if (!y)
            continue;
        if (c && *fc && !strstr(y + (b && *fb ? strlen(fb) : 0), fc))
            continue;
        ok = 1;
    }
    if (!ok) {
        fprintf(stderr, "MISSING (%s) %s: a row \"%s\" | \"%s\" | \"%s\"\n", platform_names[p], context,
                a, b ? b : "", c ? c : "");
        misses++;
    }
}

static void
check_prefs(chfsession *s, int p)
{
    char buf[128];

    for (size_t k = 0; k < sizeof pages / sizeof *pages; k++)
        for (int i = 0; pages[k].labels[i]; i++)
            require(p, pages[k].context, "label", pages[k].labels[i]);

    for (int m = 0; m < CHF_MACHINE_COUNT; m++)
        require(p, "machines", "machine", chf_machine_name(m));
    for (int c = 0; c < CHF_BIOS_CHOICE_COUNT; c++)
        require(p, "prefs/Machine", "BIOS choice", chf_bios_choice_name(c));
    for (int i = 0; i < chfsession_rom_item_count(); i++) {
        const chf_rom_item *it = chfsession_rom_item(i);
        require(p, "prefs/ROMs", "ROM", it->name);
        require(p, "prefs/ROMs", "ROM group", it->group);
        snprintf(buf, sizeof buf, "%s", it->file);
        require(p, "bios-and-roms", "ROM file", buf);
    }
    for (size_t k = 0; k < sizeof settings_keys / sizeof *settings_keys; k++)
        require(p, "the-settings-file", "setting", settings_keys[k]);
    (void) s;
}

/* Every default key and gamepad button, the console's buttons and the
 * shortcuts. */
static void
check_controls(chfsession *s, int p)
{
    char name1[64], name2[64];

    CHECK(CHF_TARGET_COUNT == 23, "the Help says 23 controls");
    require(p, "changing-the-controls", "count", "23 in all");
    for (int a = 0; a < CHF_ACT_PER_PORT; a++) {
        chf_binding b1 = chfsession_binding_get(s, CHF_TARGET_PORT(0, a));
        chf_binding b2 = chfsession_binding_get(s, CHF_TARGET_PORT(1, a));
        chfsession_keysym_name(b1.keysym, name1, sizeof name1);
        chfsession_keysym_name(b2.keysym, name2, sizeof name2);
        require_row(p, "default-keys", act_rows[a], name1, name2);
    }
    for (int k = 0; k < CHF_KEY_COUNT; k++) {
        chf_binding b = chfsession_binding_get(s, CHF_TARGET_KEY(k));
        chfsession_keysym_name(b.keysym, name1, sizeof name1);
        require_row(p, "default-keys", chf_target_short_name(CHF_TARGET_KEY(k)), name1, NULL);
    }
    {
        chf_binding b = chfsession_binding_get(s, CHF_TARGET_SWITCH(CHF_SW_RESET));
        chfsession_keysym_name(b.keysym, name1, sizeof name1);
        require_row(p, "default-keys", "RESET", name1, NULL);
        b = chfsession_binding_get(s, CHF_TARGET_SYSACT(CHF_SYSACT_RESET_CONFIG));
        chfsession_keysym_name(b.keysym, name1, sizeof name1);
        require_row(p, "default-keys", chf_target_short_name(CHF_TARGET_SYSACT(CHF_SYSACT_RESET_CONFIG)), name1, NULL);
    }
    /* the gamepad's rows: the pad button and what it does */
    require_row(p, "gamepads", "D-pad", "the stick", NULL);
    require_row(p, "gamepads", "shoulder", "twist", NULL);
    require_row(p, "gamepads", "East", "Pull", NULL);
    require_row(p, "gamepads", "South", "Push", NULL);
    require_row(p, "gamepads", "West", "1 TIME", NULL);
    require_row(p, "gamepads", "Back", "2 MODE", NULL);
    require_row(p, "gamepads", "North", "3 HOLD", NULL);
    require_row(p, "gamepads", "Start", "4 START", NULL);
    CHECK(chfsession_binding_get(s, CHF_TARGET_PORT(0, CHF_ACT_PULL)).button == CHF_PAD_BTN_EAST
          && chfsession_binding_get(s, CHF_TARGET_PORT(0, CHF_ACT_PUSH)).button == CHF_PAD_BTN_SOUTH
          && chfsession_binding_get(s, CHF_TARGET_KEY(CHF_KEY_TIME)).button == CHF_PAD_BTN_WEST
          && chfsession_binding_get(s, CHF_TARGET_KEY(CHF_KEY_MODE)).button == CHF_PAD_BTN_BACK
          && chfsession_binding_get(s, CHF_TARGET_KEY(CHF_KEY_HOLD)).button == CHF_PAD_BTN_NORTH
          && chfsession_binding_get(s, CHF_TARGET_KEY(CHF_KEY_START)).button == CHF_PAD_BTN_START
          && chfsession_binding_get(s, CHF_TARGET_PORT(0, CHF_ACT_TWIST_CCW)).button == CHF_PAD_BTN_LEFT_SHOULDER
          && chfsession_binding_get(s, CHF_TARGET_PORT(0, CHF_ACT_TWIST_CW)).button == CHF_PAD_BTN_RIGHT_SHOULDER,
          "the gamepad rows are the default bindings");

    for (size_t k = 0; k < sizeof shortcuts / sizeof *shortcuts; k++)
        require(p, "default-keys", "shortcut", platforms[p] == CHFHELP_MACOS ? shortcuts[k].mac : shortcuts[k].pc);
}

/* The two ports, in the FujiNet topic. */
static void
check_ports(int p)
{
    char port[16];

    snprintf(port, sizeof port, "%d", CHFSESSION_BOIP_PORT);
    require(p, "fujinet", "BoIP port", port);
    snprintf(port, sizeof port, "%d", CHFSESSION_WEBUI_PORT);
    require(p, "fujinet", "web UI port", port);
    require(p, "an-external-fujinet", "default address", "127.0.0.1:11526");
}

/* Each command starts a `code` span in the prompt's section. */
static void
check_debugger(int p)
{
    int t, b;
    CHECK(chfhelp_find("debugger/prompt", &t, &b) == 0, "no debugger/prompt");
    const chfhelp_topic *tp = chfhelp_topic_at(t);
    int end = chfhelp_section_end(t, b);
    for (size_t k = 0; k < sizeof debug_commands / sizeof *debug_commands; k++) {
        const char *cmd = debug_commands[k];
        size_t n = strlen(cmd);
        int found = 0;
        for (int bl = b; bl < end && !found; bl++) {
            const chfhelp_block *blk = &tp->blocks[bl];
            if (!(blk->platforms & platforms[p]))
                continue;
            for (int i = 0; i < blk->nspans && !found; i++) {
                const char *c = blk->spans[i].text;
                if (blk->spans[i].kind != CHFHELP_CODE)
                    continue;
                found = !strncmp(c, cmd, n) && (c[n] == 0 || c[n] == ' ' || c[n] == '/');
            }
        }
        if (!found) {
            fprintf(stderr, "MISSING (%s) debugger/prompt: command `%s`\n", platform_names[p], cmd);
            misses++;
        }
    }
    /* the eight tabs */
    require(p, "debugger", "tabs", "eight tabs");
}

int
main(void)
{
    char cfg[512], data[512];
    chfsession_paths paths;
    chfhelp_hit hits[16];
    chfsession *s;
    int t, b;

    test_tmpdir(cfg, sizeof cfg, "hcfg");
    test_tmpdir(data, sizeof data, "hdata");
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    paths.fujinet_lib = "";
    s = chfsession_new(&paths);
    CHECK(s, "new");
    CHECK(chfhelp_topic_count() > 0, "no topics");

    for (size_t k = 0; k < sizeof contexts / sizeof *contexts; k++)
        if (chfhelp_find(contexts[k], &t, &b) != 0) {
            fprintf(stderr, "MISSING context %s\n", contexts[k]);
            misses++;
        }
    for (int p = 0; p < 4; p++) {
        check_prefs(s, p);
        check_controls(s, p);
        check_ports(p);
        check_debugger(p);
    }

    /* lookup and search */
    CHECK(chfhelp_find("#preferences", &t, &b) == 0 && b == -1, "#preferences");
    CHECK(chfhelp_find("#the-settings-file", &t, &b) == 0 && b >= 0, "#the-settings-file");
    CHECK(chfhelp_search(CHFHELP_GNOME, "stand-in", hits, 16) > 0, "search stand-in");
    CHECK(chfhelp_search(CHFHELP_GNOME, "zzqxj", hits, 16) == 0, "search nonsense");
    int n = chfhelp_search(CHFHELP_KDE, "breakpoints", hits, 16);
    CHECK(n > 0 && chfhelp_find("debugger/breakpoints", &t, &b) == 0
          && hits[0].topic == t && hits[0].block == b, "breakpoints is the first hit");
    for (int i = 1; i < n; i++)
        CHECK(hits[i - 1].score >= hits[i].score, "hits best first");

    /* the Mac sees its own keys */
    {
        char text[4096];
        CHECK(chfhelp_find("default-keys", &t, &b) == 0, "default-keys");
        chfhelp_plain_text(CHFHELP_MACOS, t, b, text, sizeof text);
        CHECK(strstr(text, "⌘O") != NULL, "the Mac's Open Cartridge key");
        chfhelp_plain_text(CHFHELP_GNOME, t, b, text, sizeof text);
        CHECK(strstr(text, "Ctrl+O") != NULL && !strstr(text, "⌘O"), "the PC's");
    }

    chfsession_free(s);
    CHECK(misses == 0, "%d missing from data/help/help.md", misses);
    printf("help ok: %d topics\n", chfhelp_topic_count());
    return 0;
}
