/*
 * prompt.c -- the debugger's command line (chfdebug_command) and its Tab
 * completion. Every command is a thin layer over the public API, so a
 * window's buttons and the prompt can never disagree.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "debugger_internal.h"

typedef struct {
    char *dst;
    int cap, len;
} outbuf;

static void out(outbuf *o, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (o->len >= o->cap - 1)
        return;
    va_start(ap, fmt);
    n = vsnprintf(o->dst + o->len, (size_t)(o->cap - o->len), fmt, ap);
    va_end(ap);
    if (n > 0)
        o->len += n < o->cap - o->len ? n : o->cap - o->len - 1;
}

static const char *const commands[] = {
    "help", "step", "over", "out", "run", "stop", "runto", "scanline", "frame",
    "break", "bpr", "bpw", "delete", "enable", "disable", "clear", "breaks", "print",
    "regs", "set", "mem", "poke", "uvi", "objects", "screen", "io", "keys", "cart",
    "mailbox", "label", "labels", "disasm", "trace", "save", NULL,
};

static const char *help_text =
    "step [n]               run n instructions (F7)\n"
    "over                   step over a branch to subroutine (F8)\n"
    "out                    run to the routine's RETC / RETE (Shift+F8)\n"
    "run | stop             go / break\n"
    "runto <addr>           run until PC = addr\n"
    "scanline [n] | frame [n]   run until the beam has moved n lines / n frames\n"
    "break <addr>[-<end>] [cond]            execute breakpoint\n"
    "bpr | bpw <addr>[-<end>] [cond]        PROG-space read / write (the UVI is $1800-$1AFF)\n"
    "delete <id|addr> | enable <id> | disable <id> | clear | breaks\n"
    "print <expr>           e.g. print r3, print u[$FC], print p[$1900] & 8\n"
    "regs                   CPU registers, status, return stack\n"
    "set <reg> <expr>       e.g. set pc $0412, set r2 $09, set rs 1, set psl 0x0a\n"
    "mem [space] <addr> [len]    dump memory (side-effect free); space is\n"
    "                       prog (default), uvi or cart\n"
    "poke [space] <addr> <v...>  write memory (prog takes the UVI window and cart RAM)\n"
    "uvi | objects | screen the 2637: registers; the four objects; the character screen\n"
    "io | keys              the console's inputs; the keypads and sticks\n"
    "cart | mailbox         the cartridge; the FujiNet mailbox page\n"
    "label <addr> [name] | labels [file]\n"
    "disasm [addr] [n]\n"
    "trace [on|off|clear|n] record / show the last n instructions\n"
    "save <dis|mailbox|uvi|font|cart|screen|regs> <file>\n"
    "Expressions: $0400 0x400 400h 1024 %1010; registers r0 r1-r3 (the set in use)\n"
    "r1'-r3' (set 1) pc iar psu psl sp cc c com wc rs idc ovf f ii s; [addr] a PROG\n"
    "byte; p[addr] u[off] c[off] PROG / UVI / CART bytes; labels; vpos hpos frame\n"
    "cycles; in a condition, value and addr; C operators.\n";

static char *next_tok(char **p)
{
    char *s = *p, *t;
    while (*s && isspace((unsigned char)*s))
        s++;
    if (!*s)
    {
        *p = s;
        return NULL;
    }
    t = s;
    while (*s && !isspace((unsigned char)*s))
        s++;
    if (*s)
        *s++ = '\0';
    *p = s;
    return t;
}

static char *rest_of(char *p)
{
    while (*p && isspace((unsigned char)*p))
        p++;
    return p;
}

static int eval(chfdebug *d, outbuf *o, const char *text, long *v)
{
    char err[96];
    if (chfdebug_eval(d, text, v, err, sizeof err) != 0)
    {
        out(o, "error: %s\n", err);
        return -1;
    }
    return 0;
}

/* "<addr>" or "<addr>-<end>" */
static int range(chfdebug *d, outbuf *o, char *tok, long *a, long *b)
{
    char *dash = strchr(tok + 1, '-');
    if (dash)
    {
        *dash = '\0';
        if (eval(d, o, tok, a) || eval(d, o, dash + 1, b))
            return -1;
        return 0;
    }
    if (eval(d, o, tok, a))
        return -1;
    *b = *a;
    return 0;
}

/* prog, uvi, cart (and the short forms), else -1 */
static int space_by_name(const char *n)
{
    static const struct { const char *name; int space; } map[] = {
        {"prog", CHF_SPACE_PROG}, {"p", CHF_SPACE_PROG}, {"program", CHF_SPACE_PROG},
        {"uvi", CHF_SPACE_UVI}, {"u", CHF_SPACE_UVI}, {"vdc", CHF_SPACE_UVI},
        {"cart", CHF_SPACE_CART}, {"c", CHF_SPACE_CART}, {"rom", CHF_SPACE_CART},
    };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcasecmp(map[i].name, n))
            return map[i].space;
    return -1;
}

static const char *const space_label[3] = { "prog", "uvi", "cart" };

static void show_regs(chfdebug *d, outbuf *o)
{
    chfdebug_cpu c;
    static const char *const ccs[] = { "zero", "positive", "negative", "11" };
    chfdebug_cpu_get(d, &c);
    out(o, "PC=%04X (page %04X iar %04X)  PSU=%02X [S=%d F=%d II=%d SP=%d]  PSL=%02X [CC=%s IDC=%d "
           "RS=%d WC=%d OVF=%d COM=%d C=%d]\n",
        c.pc, c.page, c.iar, c.psu, c.sense, c.flag, c.ii, c.sp, c.psl, ccs[c.cc & 3], c.idc, c.rs,
        c.wc, c.ovf, c.com, c.c);
    out(o, "R0=%02X", c.r0);
    for (int b = 0; b < 2; b++)
    {
        out(o, "   bank %d%s:", b, b == c.rs ? " (in use)" : "");
        for (int i = 1; i < 4; i++)
            out(o, " R%d%s=%02X", i, b ? "'" : "", c.r[b][i]);
    }
    out(o, "\nreturn stack (SP %d):", c.sp);
    for (int i = 0; i < 8; i++)
        out(o, "%s%04X%s", i == c.sp ? " [" : " ", c.ras[i], i == c.sp ? "]" : "");
    out(o, "\n%s%s\n", c.halted ? "halted  " : "", c.illegal_count ? "illegal opcodes seen" : "");
    out(o, "line %d pixel %d  frame %u  cycles %llu  instructions %llu\n", c.vpos, c.hpos, c.frame,
        (unsigned long long)c.cycles, (unsigned long long)c.instructions);
}

static void dump(outbuf *o, int space, uint32_t addr, const uint8_t *b, int n)
{
    const int w = space == CHF_SPACE_PROG ? 4 : space == CHF_SPACE_UVI ? 3 : 5;
    const unsigned mask = space == CHF_SPACE_PROG ? 0x7fff : space == CHF_SPACE_UVI ? 0xfff : 0xfffff;

    for (int i = 0; i < n; i += 16)
    {
        out(o, "%0*X ", w, (addr + (uint32_t)i) & mask);
        for (int j = 0; j < 16; j++)
            out(o, j < n - i ? " %02X" : "   ", b[i + j]);
        out(o, "  ");
        for (int j = 0; j < 16 && i + j < n; j++)
            out(o, "%c", (b[i + j] >= 0x20 && b[i + j] < 0x7f) ? b[i + j] : '.');
        out(o, "\n");
    }
}

/* the registers the UVI and the cartridge decode in a dumped range */
static void dump_names(chfdebug *d, outbuf *o, int space, uint32_t addr, int n)
{
    for (int i = 0; i < n; i++)
    {
        char name[48], text[160];
        const uint32_t a = addr + (uint32_t)i;

        if (space == CHF_SPACE_UVI && a >= 0xf0 && a < 0x300 && !(a >= 0x180 && a < 0x1c0))
        {
            chfdebug_uvi_describe(d, a, name, sizeof name, text, sizeof text);
            if (text[0])
                out(o, "  %03X %-10s %s\n", a, name, text);
        }
        else if (space == CHF_SPACE_PROG)
        {
            const char *nm = chf_mailbox_name((uint16_t)a);
            if (nm)
                out(o, "  %04X %s\n", a, nm);
        }
    }
}

static void list_breaks(chfdebug *d, outbuf *o)
{
    chfdebug_breakpoint bp[128];
    int n = chfdebug_breakpoint_list(d, bp, 128);
    if (!n)
    {
        out(o, "no breakpoints\n");
        return;
    }
    for (int i = 0; i < n; i++)
    {
        char kinds[16] = "";
        if (bp[i].type & CHFDEBUG_BP_EXEC)  strcat(kinds, "x");
        if (bp[i].type & CHFDEBUG_BP_READ)  strcat(kinds, "r");
        if (bp[i].type & CHFDEBUG_BP_WRITE) strcat(kinds, "w");
        out(o, "#%d %-3s ", bp[i].id, kinds);
        if (bp[i].start == bp[i].end)
            out(o, "$%04X", bp[i].start);
        else
            out(o, "$%04X-$%04X", bp[i].start, bp[i].end);
        out(o, "%s%s%s  hits %u\n", bp[i].enabled ? "" : " (disabled)",
            bp[i].condition[0] ? " if " : "", bp[i].condition, bp[i].hits);
    }
}

/* break / bpr / bpw : "<addr>[-<end>] [condition]" in PROG space */
static void add_bp(chfdebug *d, outbuf *o, int type, char *args)
{
    char *t = next_tok(&args);
    long a, b;
    int id;

    if (!t)
    {
        out(o, "usage: <addr>[-<end>] [condition]\n");
        return;
    }
    if (range(d, o, t, &a, &b))
        return;
    if (a < 0 || b < 0 || a > 0x7fff || b > 0x7fff)
    {
        out(o, "error: address out of range\n");
        return;
    }
    id = chfdebug_breakpoint_add(d, type, CHF_SPACE_PROG, (uint16_t)a, (uint16_t)b, rest_of(args));
    if (id < 0)
        out(o, "error: the condition does not parse, or the address is outside PROG space\n");
    else
    {
        out(o, "breakpoint #%d\n", id);
        if ((type & CHFDEBUG_BP_WRITE) && !(a <= 0x1aff && b >= 0x1800))
            out(o, "(only the UVI at $1800-$1AFF takes writes; this one will not fire)\n");
    }
}

static int reg_by_name(const char *n)
{
    static const struct { const char *name; int reg; } map[] = {
        {"r0", CHF_REG_R0}, {"r1", CHF_REG_R1}, {"r2", CHF_REG_R2}, {"r3", CHF_REG_R3},
        {"r1'", CHF_REG_R1B}, {"r2'", CHF_REG_R2B}, {"r3'", CHF_REG_R3B},
        {"pc", CHF_REG_PC}, {"iar", CHF_REG_IAR}, {"psu", CHF_REG_PSU}, {"psl", CHF_REG_PSL},
        {"sp", CHF_REG_SP}, {"f", CHF_REG_F}, {"ii", CHF_REG_II}, {"cc", CHF_REG_CC},
        {"idc", CHF_REG_IDC}, {"rs", CHF_REG_RS}, {"wc", CHF_REG_WC}, {"ovf", CHF_REG_OVF},
        {"com", CHF_REG_COM}, {"c", CHF_REG_C},
    };
    if (!strncasecmp(n, "ras", 3) && n[3] >= '0' && n[3] <= '7' && !n[4])
        return CHF_REG_RAS0 + (n[3] - '0');
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcasecmp(map[i].name, n))
            return map[i].reg;
    return -1;
}

static void show_io(chfdebug *d, outbuf *o)
{
    chfdebug_io io;
    chfdebug_io_get(d, &io);
    out(o, "%s, cartridge: %s\n", io.is_pal ? "Channel F (PAL)" : "Channel F (NTSC)",
        io.cart_type == 3 ? "FujiNet" : io.cart_type == 2 ? "Golf" : io.cart_type == 1 ? "standard" : "none");
    out(o, "SENSE (vblank) %d   FLAG %d   RESET %s   audio samples %llu\n", io.sense, io.flag,
        io.reset_held ? "held" : "up", (unsigned long long)io.audio_samples);
    out(o, "keypad columns $1900-$1907:");
    for (int i = 0; i < 8; i++)
        out(o, " %02X", io.pad[i]);
    out(o, "   panel $1908: %02X\n", io.panel);
    out(o, "pots: stick 1 x %02X y %02X, stick 2 x %02X y %02X\n", io.pot[0][0], io.pot[0][1],
        io.pot[1][0], io.pot[1][1]);
}

static void show_keys(chfdebug *d, outbuf *o)
{
    static const char *const names[12] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "CLEAR", "0", "ENTER" };
    chfdebug_io io;
    chfdebug_io_get(d, &io);
    for (int p = 0; p < 2; p++)
    {
        out(o, "keypad %d:", p + 1);
        for (int k = 0; k < 12; k++)
            if (io.keys & (1ull << (p * 12 + k)))
                out(o, " %s", names[k]);
        out(o, "%s\n", (io.keys >> (p * 12)) & 0xfff ? "" : " (none)");
    }
    out(o, "panel:%s%s%s%s\n", io.keys & (1ull << 24) ? " START" : "", io.keys & (1ull << 25) ? " OPTION" : "",
        io.keys & (1ull << 26) ? " SELECT" : "", (io.keys >> 24) & 7 ? "" : " (none)");
    for (int s = 0; s < 2; s++)
    {
        const uint8_t j = io.sticks[s];
        out(o, "stick %d:%s%s%s%s%s%s", s + 1, j & 1 ? " UP" : "", j & 2 ? " RIGHT" : "",
            j & 4 ? " DOWN" : "", j & 8 ? " LEFT" : "", j & 16 ? " FIRE" : "", j ? "" : " (centred)");
        if (io.disc[s][0] || io.disc[s][1])
            out(o, "  disc %d,%d", io.disc[s][0], io.disc[s][1]);
        out(o, "\n");
    }
}

int chfdebug_command(chfdebug *d, const char *command, char *dst, int dstsz)
{
    char line[512];
    char *p = line, *cmd;
    outbuf o = { dst, dstsz, 0 };
    long v, w;

    if (!d || !dst || dstsz <= 0)
        return 0;
    dst[0] = '\0';
    snprintf(line, sizeof line, "%s", command ? command : "");
    cmd = next_tok(&p);
    if (!cmd)
        return 0;
    for (char *c = cmd; *c; c++)
        *c = (char)tolower((unsigned char)*c);

    if (!strcmp(cmd, "help") || !strcmp(cmd, "?"))
        out(&o, "%s", help_text);
    else if (!strcmp(cmd, "step") || !strcmp(cmd, "s"))
    {
        char *t = next_tok(&p);
        v = 1;
        if (t && eval(d, &o, t, &v))
            return o.len;
        if (!chfdebug_is_stopped(d))
            out(&o, "the machine is running (stop first)\n");
        else
            chfdebug_step_n(d, (int)v);
    }
    else if (!strcmp(cmd, "over") || !strcmp(cmd, "o"))
        chfdebug_step_over(d);
    else if (!strcmp(cmd, "out"))
        chfdebug_step_out(d);
    else if (!strcmp(cmd, "run") || !strcmp(cmd, "go") || !strcmp(cmd, "g"))
        chfdebug_resume(d);
    else if (!strcmp(cmd, "stop"))
        chfdebug_stop(d);
    else if (!strcmp(cmd, "runto"))
    {
        char *t = next_tok(&p);
        if (!t || eval(d, &o, t, &v))
            return o.len;
        chfdebug_run_to(d, (uint16_t)v);
    }
    else if (!strcmp(cmd, "scanline") || !strcmp(cmd, "frame"))
    {
        char *t = next_tok(&p);
        v = 1;
        if (t && eval(d, &o, t, &v))
            return o.len;
        if (!strcmp(cmd, "frame"))
            chfdebug_frame(d, (int)v);
        else
            chfdebug_scanline(d, (int)v);
    }
    else if (!strcmp(cmd, "break") || !strcmp(cmd, "bp"))
        add_bp(d, &o, CHFDEBUG_BP_EXEC, p);
    else if (!strcmp(cmd, "bpr"))
        add_bp(d, &o, CHFDEBUG_BP_READ, p);
    else if (!strcmp(cmd, "bpw"))
        add_bp(d, &o, CHFDEBUG_BP_WRITE, p);
    else if (!strcmp(cmd, "delete") || !strcmp(cmd, "enable") || !strcmp(cmd, "disable"))
    {
        char *t = next_tok(&p);
        if (!t)
            out(&o, "usage: %s <id>\n", cmd);
        else
        {
            chfdebug_breakpoint bp[128];
            const int n = chfdebug_breakpoint_list(d, bp, 128);
            const int is_id = t[0] == '#';
            long id = strtol(is_id ? t + 1 : t, NULL, 0);
            int found = 0;

            /* "#3" is an id; a plain number is one when a breakpoint has it,
             * else an address (delete only) */
            for (int i = 0; i < n; i++)
                if (bp[i].id == id)
                    found = 1;
            if (!found && !is_id && !strcmp(cmd, "delete") && eval(d, &o, t, &v) == 0)
            {
                for (int i = 0; i < n; i++)
                    if ((bp[i].type & CHFDEBUG_BP_EXEC) && v >= bp[i].start && v <= bp[i].end)
                    {
                        chfdebug_breakpoint_remove(d, bp[i].id);
                        found = 1;
                    }
                if (!found)
                    out(&o, "no breakpoint #%ld or at $%04lX\n", id, v & 0x7fff);
            }
            else if (!found)
                out(&o, "no breakpoint #%ld\n", id);
            else if (!strcmp(cmd, "delete"))
                chfdebug_breakpoint_remove(d, (int)id);
            else
                chfdebug_breakpoint_enable(d, (int)id, !strcmp(cmd, "enable"));
        }
    }
    else if (!strcmp(cmd, "clear"))
        chfdebug_breakpoint_clear(d);
    else if (!strcmp(cmd, "breaks") || !strcmp(cmd, "bl"))
        list_breaks(d, &o);
    else if (!strcmp(cmd, "print") || !strcmp(cmd, "p"))
    {
        if (eval(d, &o, rest_of(p), &v) == 0)
        {
            char bin[17];
            const int bits = (v >= 0 && v < 256) ? 8 : 16;
            for (int i = 0; i < bits; i++)
                bin[i] = (char)('0' + ((v >> (bits - 1 - i)) & 1));
            bin[bits] = '\0';
            out(&o, v >= 0 && v < 256 ? "$%02lX  %ld  %%%s\n" : "$%04lX  %ld  %%%s\n",
                (unsigned long)v & 0xffff, v, bin);
        }
    }
    else if (!strcmp(cmd, "regs") || !strcmp(cmd, "r"))
        show_regs(d, &o);
    else if (!strcmp(cmd, "set"))
    {
        char *r = next_tok(&p);
        int reg = r ? reg_by_name(r) : -1;
        if (reg < 0)
            out(&o, "usage: set <reg> <expr>   (r0 r1-r3 r1'-r3' pc iar psu psl sp f ii cc idc rs wc "
                    "ovf com c ras0-ras7)\n");
        else if (eval(d, &o, rest_of(p), &v) == 0)
        {
            if (!chfdebug_is_stopped(d))
                out(&o, "the machine is running (stop first)\n");
            else
            {
                chfdebug_cpu_set(d, reg, (int)v);
                show_regs(d, &o);
            }
        }
    }
    else if (!strcmp(cmd, "mem") || !strcmp(cmd, "m"))
    {
        char *t = next_tok(&p), *l;
        uint8_t buf[256];
        int space = CHF_SPACE_PROG;
        if (t && space_by_name(t) >= 0)
        {
            space = space_by_name(t);
            t = next_tok(&p);
        }
        l = next_tok(&p);
        w = 64;
        if (!t || eval(d, &o, t, &v) || (l && eval(d, &o, l, &w)))
        {
            if (!t)
                out(&o, "usage: mem [prog|uvi|cart] <addr> [len]\n");
            return o.len;
        }
        if (w > 256)
            w = 256;
        if (w < 1)
            w = 1;
        chfdebug_read(d, space, (uint32_t)v, buf, (int)w);
        out(&o, "%s space\n", space_label[space]);
        dump(&o, space, (uint32_t)v, buf, (int)w);
        dump_names(d, &o, space, (uint32_t)v, (int)w);
    }
    else if (!strcmp(cmd, "poke"))
    {
        char *t = next_tok(&p), *b;
        int n = 0, space = CHF_SPACE_PROG;
        if (t && space_by_name(t) >= 0)
        {
            space = space_by_name(t);
            t = next_tok(&p);
        }
        if (!t || eval(d, &o, t, &v))
        {
            if (!t)
                out(&o, "usage: poke [prog|uvi|cart] <addr> <v...>\n");
            return o.len;
        }
        while ((b = next_tok(&p)) != NULL)
        {
            if (eval(d, &o, b, &w))
                return o.len;
            chfdebug_write(d, space, (uint32_t)(v + n), (uint8_t)w);
            n++;
        }
        out(&o, "%d byte%s at %s $%lX\n", n, n == 1 ? "" : "s", space_label[space],
            (unsigned long)v);
    }
    else if (!strcmp(cmd, "uvi") || !strcmp(cmd, "vdc"))
        o.len += chfdebug_text_uvi(dst + o.len, dstsz - o.len);
    else if (!strcmp(cmd, "objects"))
        o.len += chfdebug_text_objects(dst + o.len, dstsz - o.len);
    else if (!strcmp(cmd, "screen"))
        o.len += chfdebug_text_screen(dst + o.len, dstsz - o.len);
    else if (!strcmp(cmd, "io"))
        show_io(d, &o);
    else if (!strcmp(cmd, "keys"))
        show_keys(d, &o);
    else if (!strcmp(cmd, "cart") || !strcmp(cmd, "mailbox"))
    {
        chfdebug_cart c;
        char info[200];
        chfdebug_cart_get(d, &c);
        chfdebug_cart_info(d, info, sizeof info);
        out(&o, "%s\n", info);
        if (!strcmp(cmd, "cart"))
        {
            out(&o, "type %s, image %u bytes", c.type_name ? c.type_name : "?", c.rom_size);
            if (c.type == CHF_CART_FUJINET)
                out(&o, ", CRC %08X%s\n", c.live_crc,
                    c.mailbox ? ", claims the mailbox" : ", no mailbox");
            else
                out(&o, "\n");
            if (c.type == CHF_CART_FUJINET)
            {
                out(&o, "%s: %d, page %d in the $0800 window\n", c.app_live ? "APPBANK pages" : "banks",
                    c.nbanks, c.fbank);
                out(&o, "staged: %s%s  swaps %d\n",
                    c.staged_size ? (c.staged_mailbox ? "an app" : "a game") : "nothing",
                    c.armed ? " (armed)" : "", c.swaps);
            }
        }
        else if (c.type == CHF_CART_FUJINET)
        {
            out(&o, "ACKSEQ %02X  status %02X  err %d  reply %02X  rxlen %u  queue %u\n", c.ackseq,
                c.status, c.last_error, c.reply_cmd, c.rxlen, c.queue_depth);
            out(&o, "boot state %d %d%% err %d\n", c.boot_state, c.boot_pct, c.boot_err);
        }
        if (!strcmp(cmd, "mailbox"))
        {
            out(&o, "$2B00-$2FFF as the console reads it (reply slice, status page, hotspots):\n");
            dump(&o, CHF_SPACE_PROG, 0x2b00, c.mailbox_page, 0x120);
        }
    }
    else if (!strcmp(cmd, "label"))
    {
        char *t = next_tok(&p), *n = next_tok(&p);
        if (!t || eval(d, &o, t, &v))
            return o.len;
        chfdebug_set_label(d, (uint16_t)(v & 0x7fff), n);
    }
    else if (!strcmp(cmd, "labels"))
    {
        char msg[256];
        char *f = rest_of(p);
        chfdebug_load_symbols(d, *f ? f : NULL, msg, sizeof msg);
        out(&o, "%s\n", msg);
    }
    else if (!strcmp(cmd, "disasm") || !strcmp(cmd, "d"))
    {
        char *t = next_tok(&p), *n = next_tok(&p);
        chfdebug_line lines[64];
        chfdebug_cpu c;
        int pcl, cnt;
        chfdebug_cpu_get(d, &c);
        v = c.pc;
        w = 16;
        if ((t && eval(d, &o, t, &v)) || (n && eval(d, &o, n, &w)))
            return o.len;
        if (w > 64)
            w = 64;
        if (w < 1)
            w = 1;
        cnt = chfdebug_disassemble(d, (uint16_t)v, lines, (int)w, &pcl);
        for (int i = 0; i < cnt; i++)
        {
            if (lines[i].label[0])
                out(&o, "%s:\n", lines[i].label);
            out(&o, "%c%c%04X  %-9s %-22s%s%s\n", lines[i].is_pc ? '>' : ' ',
                lines[i].has_breakpoint ? '*' : ' ', lines[i].address, lines[i].bytes,
                lines[i].disasm, lines[i].comment[0] ? "; " : "", lines[i].comment);
        }
    }
    else if (!strcmp(cmd, "trace"))
    {
        char *t = next_tok(&p);
        if (t && !strcmp(t, "on"))
            chfdebug_trace_enable(d, 1);
        else if (t && !strcmp(t, "off"))
            chfdebug_trace_enable(d, 0);
        else if (t && !strcmp(t, "clear"))
            chfdebug_trace_clear(d);
        else
        {
            chfdebug_trace tr[64];
            int n;
            w = 16;
            if (t && eval(d, &o, t, &w))
                return o.len;
            if (w > 64)
                w = 64;
            if (w < 1)
                w = 1;
            n = chfdebug_trace_read(d, tr, (int)w);
            if (!n)
                out(&o, "trace is %s and empty\n", chfdebug_trace_enabled(d) ? "on" : "off");
            for (int i = n - 1; i >= 0; i--)
            {
                chfdebug_line l;
                int pcl;
                chfdebug_disassemble(d, tr[i].pc, &l, 1, &pcl);
                out(&o, "%10llu %3d:%3d  %04X  %-22s R0=%02X R1=%02X R2=%02X R3=%02X PSU=%02X PSL=%02X\n",
                    (unsigned long long)tr[i].cycles, tr[i].vpos, tr[i].hpos, tr[i].pc, l.disasm,
                    tr[i].r0, tr[i].r1, tr[i].r2, tr[i].r3, tr[i].psu, tr[i].psl);
            }
        }
    }
    else if (!strcmp(cmd, "save"))
    {
        char *k = next_tok(&p);
        char *f = rest_of(p);
        char msg[300];
        if (!k || !*f)
            out(&o, "usage: save <dis|mailbox|uvi|font|cart|screen|regs> <file>\n");
        else
        {
            chfdebug_save(d, k, f, msg, sizeof msg);
            out(&o, "%s\n", msg);
        }
    }
    else
        out(&o, "unknown command '%s' (try help)\n", cmd);

    return o.len;
}

int chfdebug_completions(chfdebug *d, const char *prefix, char *dst, int dstsz)
{
    outbuf o = { dst, dstsz, 0 };
    const char *word;
    size_t n;
    int count = 0;
    const int first_word = !strchr(prefix ? prefix : "", ' ');

    if (!d || !dst || dstsz <= 0)
        return 0;
    dst[0] = '\0';
    if (!prefix)
        prefix = "";
    word = strrchr(prefix, ' ');
    word = word ? word + 1 : prefix;
    n = strlen(word);

    if (first_word)
    {
        for (int i = 0; commands[i]; i++)
            if (!strncmp(commands[i], word, n))
            {
                out(&o, "%s\n", commands[i]);
                count++;
            }
        return count;
    }
    if (!strncmp(prefix, "save ", 5) && !strchr(prefix + 5, ' '))
    {
        static const char *const kinds[] = { "dis", "mailbox", "uvi", "font", "cart", "screen", "regs", NULL };
        for (int i = 0; kinds[i]; i++)
            if (!strncmp(kinds[i], word, n))
            {
                out(&o, "%s\n", kinds[i]);
                count++;
            }
        return count;
    }
    if ((!strncmp(prefix, "mem ", 4) && !strchr(prefix + 4, ' ')) ||
        (!strncmp(prefix, "poke ", 5) && !strchr(prefix + 5, ' ')))
    {
        static const char *const spaces[] = { "prog", "uvi", "cart", NULL };
        for (int i = 0; spaces[i]; i++)
            if (!strncmp(spaces[i], word, n))
            {
                out(&o, "%s\n", spaces[i]);
                count++;
            }
        return count;
    }
    for (int i = 0; chfdebug_reg_names[i]; i++)
        if (n && !strncmp(chfdebug_reg_names[i], word, n))
        {
            out(&o, "%s\n", chfdebug_reg_names[i]);
            count++;
        }
    {
        symtab *t = chfdebug_symbols(d);
        const char *name;
        chf_host_lock();
        for (int i = 0; (name = symtab_name_at(t, i)) != NULL && count < 200; i++)
            if (n && !strncasecmp(name, word, n))
            {
                out(&o, "%s\n", name);
                count++;
            }
        chf_host_unlock();
    }
    return count;
}
