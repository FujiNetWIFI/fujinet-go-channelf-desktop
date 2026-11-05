/* The Help covers what the app has: every Preferences setting and choice on
   every machine, every ROM, the debugger's commands, and every context a
   Help button opens. Every miss is listed before it fails. */
#include "test_util.h"

#include "chfhelp.h"

static const unsigned platforms[] = {
    CHFHELP_GNOME, CHFHELP_KDE, CHFHELP_WINDOWS, CHFHELP_MACOS
};
static const char *const platform_names[] = { "GNOME", "KDE", "Windows", "macOS" };

/* What the frontends' Help buttons and menus open. */
static const char *const contexts[] = {
    "main", "nobios",
    "prefs/Machine", "prefs/Display", "prefs/Sound", "prefs/Storage", "prefs/Ports",
    "prefs/FujiNet", "prefs/ROMs",
    "debugger/console", "debugger/cpu", "debugger/disassembly", "debugger/breakpoints",
    "debugger/interrupts", "debugger/fujinet",
    "controllers", "fujilog",
};

/* The console's commands (core/src/debug.cpp help_text). */
static const char *const debug_commands[] = {
    "r", "d", "e", "u", "g", "t", "p", "bp", "bpr", "bpw", "bpa", "bpi", "bpo", "bpio",
    "bpint", "bl", "bc", "i", "o",
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

/* The folded text of a context's section on platform p. */
static const char *
section_text(int p, const char *context)
{
    static char raw[1 << 18];
    int t, b;
    section[0] = 0;
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

/* "Intel 8088 4.77 MHz" -> "Intel 8088": the speeds move with 86Box. */
static void
cpu_family(char *label)
{
    for (int k = 0; k < 2; k++) {
        char *sp = strrchr(label, ' ');
        if (sp)
            *sp = 0;
    }
}

static void
check_prefs(msdossession *s, int p)
{
    int machine = msdossession_pref_find("machine");
    int hdc = msdossession_pref_find("hdc");
    char ctx[64], buf[128];

    for (int m = 0; m < MSDOS_M_COUNT; m++) {
        msdossession_pref_set_choice(s, machine, m);
        require(p, "prefs/Machine", "machine", msdos_machine_name(m));
        for (int i = 0; i < msdossession_pref_count(); i++) {
            const msdos_pref *pr = msdossession_pref(i);
            snprintf(ctx, sizeof ctx, "prefs/%s", pr->page);
            require(p, ctx, "label", pr->label);
            require(p, ctx, "group", pr->group);
            if (pr->type != MSDOS_PREF_CHOICE || i == machine
                || !strcmp(pr->key, "hd_c_type") || !strcmp(pr->key, "hd_d_type"))
                continue;
            for (int c = 0; msdossession_pref_choice(s, i, c); c++) {
                snprintf(buf, sizeof buf, "%s", msdossession_pref_choice(s, i, c));
                if (!strcmp(pr->key, "cpu"))
                    cpu_family(buf);
                require(p, ctx, pr->key, buf);
            }
        }
        /* the drive types follow the controller: MFM or IDE */
        int was = msdossession_pref_get_choice(s, hdc);
        for (int c = 0; msdossession_pref_choice(s, hdc, c); c++) {
            msdossession_pref_set_choice(s, hdc, c);
            int t = msdossession_pref_find("hd_c_type");
            for (int k = 0; msdossession_pref_choice(s, t, k); k++)
                require(p, "prefs/Storage", "hd type", msdossession_pref_choice(s, t, k));
        }
        msdossession_pref_set_choice(s, hdc, was);
    }
    for (int i = 0; i < msdossession_rom_item_count(); i++) {
        require(p, "prefs/ROMs", "ROM", msdossession_rom_item(i)->name);
        require(p, "prefs/ROMs", "ROM group", msdossession_rom_item(i)->group);
    }
}

/* Each command starts a `code` span in the console's section ("bpi/bpo/bpio
   port" names three). */
static void
check_debugger(int p)
{
    int t, b;
    CHECK(chfhelp_find("debugger/console", &t, &b) == 0, "no debugger/console");
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
                while (*c && !found) {
                    found = !strncmp(c, cmd, n) && (c[n] == 0 || c[n] == ' ' || c[n] == '/');
                    while (*c && *c != '/' && *c != ' ')
                        c++;
                    if (*c != '/')
                        break;
                    c++;
                }
            }
        }
        if (!found) {
            fprintf(stderr, "MISSING (%s) debugger/console: command `%s`\n", platform_names[p], cmd);
            misses++;
        }
    }
}

int
main(void)
{
    msdossession *s = test_session_ex("help", 0, 0);
    chfhelp_hit hits[16];
    int t, b;
    CHECK(s, "new");
    CHECK(chfhelp_topic_count() > 0, "no topics");

    for (size_t k = 0; k < sizeof contexts / sizeof *contexts; k++)
        if (chfhelp_find(contexts[k], &t, &b) != 0) {
            fprintf(stderr, "MISSING context %s\n", contexts[k]);
            misses++;
        }
    for (int p = 0; p < 4; p++) {
        check_prefs(s, p);
        check_debugger(p);
    }

    /* lookup and search */
    CHECK(chfhelp_find("#preferences", &t, &b) == 0 && b == -1, "#preferences");
    CHECK(chfhelp_search(CHFHELP_GNOME, "write-protect", hits, 16) > 0, "search write-protect");
    CHECK(chfhelp_search(CHFHELP_GNOME, "zzqxj", hits, 16) == 0, "search nonsense");
    int n = chfhelp_search(CHFHELP_KDE, "breakpoints", hits, 16);
    CHECK(n > 0 && chfhelp_find("debugger/breakpoints", &t, &b) == 0
          && hits[0].topic == t && hits[0].block == b, "breakpoints is the first hit");
    for (int i = 1; i < n; i++)
        CHECK(hits[i - 1].score >= hits[i].score, "hits best first");

    msdossession_free(s);
    CHECK(misses == 0, "%d missing from data/help/help.md", misses);
    printf("help ok: %d topics\n", chfhelp_topic_count());
    return 0;
}
