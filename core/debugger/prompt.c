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
    "break", "bpr", "bpw", "bpi", "bpo", "bpv", "bps", "delete", "enable", "disable", "clear",
    "breaks", "print", "regs", "set", "mem", "poke", "scratch", "isar", "kstack", "vram", "pal",
    "video", "ports", "sound", "input", "cart", "mailbox", "bios", "romc", "label", "labels",
    "disasm", "trace", "save", NULL,
};

static const char *help_text =
    "step [n]               run n instructions (F7)\n"
    "over                   step over a PI or PK (F8)\n"
    "out                    run until the routine's POP (Shift+F8)\n"
    "run | stop             go / break\n"
    "runto <addr>           run until PC = addr\n"
    "scanline [n] | frame [n]   run until the beam has moved n lines / n frames\n"
    "break <addr>[-<end>] [cond]            execute breakpoint\n"
    "bpr | bpw <addr>[-<end>] [cond]        PROG-space data read / write (the cartridge's RAM)\n"
    "bpi | bpo <port>[-<end>] [cond]        port read / write (IN, INS / OUT, OUTS)\n"
    "bpv <row>,<col>[-<row>,<col>] [cond]   VRAM write (a strobe) into a cell range\n"
    "bps <reg>[-<end>] [cond]               scratchpad write: r12, r12-r15, 59\n"
    "delete <id|addr> | enable <id> | disable <id> | clear | breaks\n"
    "print <expr>           e.g. print r12, print s[59], print p[$8000] & 8, print v[10*128+20]\n"
    "regs                   A, W, ISAR, PC0/PC1, DC0/DC1, J H K Q, the K stack\n"
    "set <reg> <expr>       e.g. set pc $0802, set a 0, set isar o25, set k $1234, set r12 5\n"
    "mem <space> <addr> [len]    dump memory (side-effect free); space is prog (default),\n"
    "                       scratch, vram, cart or io\n"
    "poke <space> <addr> <v...>  write memory (prog takes the cartridge's RAM; io acts as\n"
    "                       the CPU writing the port: port 0's strobe draws a pixel)\n"
    "scratch | isar | kstack    the 64 registers as a grid; ISAR and its cell; the K stack\n"
    "vram [row] | pal | video   the VRAM (a row, or the picture); the palettes; ports and beam\n"
    "ports | sound | input  the four ports; the tone; the controllers, panel and RESET\n"
    "cart | mailbox         the cartridge; the FujiNet mailbox ($F800-$FFFF)\n"
    "bios                   which BIOS runs\n"
    "romc [on|off|n]        record / show the last n ROMC bus cycles\n"
    "label <addr> [name] | labels [file]\n"
    "disasm [addr] [n]\n"
    "trace [on|off|clear|n] record / show the last n instructions\n"
    "save <dis|mailbox|vram|scratch|cart|screen|vramshot|regs> <file>\n"
    "Expressions: $0800 0x800 800h 2048 %1010 o17; registers a w is isar pc pc0 pc1 dc0 dc1\n"
    "j h k q hu hl ku kl qu ql, flags icb o z c s, r0-r63; [addr] a PROG byte; p[addr] s[reg]\n"
    "v[row*128+col] c[off] io[port] the five spaces; labels and port names (PANEL, ROW_SOUND);\n"
    "vpos hpos frame cycles; in a condition value and addr (row and col for bpv, port for\n"
    "bpi/bpo); C operators.\n";

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

/* a scratchpad register: "r12" or "12" (a register's NAME is its number
 * here, not its contents) */
static int reg_number(chfdebug *d, outbuf *o, char *tok, long *v)
{
    if ((tok[0] == 'r' || tok[0] == 'R') && isdigit((unsigned char)tok[1]))
    {
        char *end;
        *v = strtol(tok + 1, &end, 10);
        if (*end == '\0')
            return 0;
    }
    return eval(d, o, tok, v);
}

/* prog, scratch, vram, cart, io (and the short forms), else -1 */
static int space_by_name(const char *n)
{
    static const struct { const char *name; int space; } map[] = {
        {"prog", CHF_SPACE_PROG}, {"p", CHF_SPACE_PROG}, {"program", CHF_SPACE_PROG},
        {"scratch", CHF_SPACE_SCRATCH}, {"s", CHF_SPACE_SCRATCH}, {"reg", CHF_SPACE_SCRATCH},
        {"vram", CHF_SPACE_VRAM}, {"v", CHF_SPACE_VRAM},
        {"cart", CHF_SPACE_CART}, {"c", CHF_SPACE_CART}, {"rom", CHF_SPACE_CART},
        {"io", CHF_SPACE_IO}, {"port", CHF_SPACE_IO},
    };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcasecmp(map[i].name, n))
            return map[i].space;
    return -1;
}

static const char *const space_label[5] = { "prog", "scratch", "vram", "cart", "io" };

static void show_regs(chfdebug *d, outbuf *o)
{
    chfdebug_cpu c;
    chfdebug_line l;
    int pcl;

    chfdebug_cpu_get(d, &c);
    out(o, "PC=%04X  PC0=%04X PC1=%04X  DC0=%04X DC1=%04X  A=%02X  ISAR=o%02o (%d)\n", c.pc, c.pc0,
        c.pc1, c.dc0, c.dc1, c.a, c.isar, c.isar);
    out(o, "W=%02X [ICB=%d O=%d Z=%d C=%d S=%d (1 = positive)]\n", c.w, c.icb, c.overflow, c.zero,
        c.carry, c.sign);
    out(o, "J=%02X  H=%04X  K=%04X  Q=%04X  dbus=%02X\n", c.j_h_k_q[0], c.j_h_k_q[1], c.j_h_k_q[2],
        c.j_h_k_q[3], c.dbus);
    if (c.k_depth >= 0)
    {
        out(o, "K stack: depth %d (r59=%d)", c.k_depth, c.r[59]);
        for (int i = 0; i < c.k_depth; i++)
            out(o, " %04X", c.k_stack[i]);
        out(o, "\n");
    }
    else
        out(o, "K stack: r59=%d is not a pointer into it\n", c.r[59]);
    if (chfdebug_disassemble(d, (uint16_t)c.pc, &l, 1, &pcl) == 1)
        out(o, "next: %04X  %-9s %s\n", l.address, l.bytes, l.disasm);
    out(o, "%s%s line %d cycle %d  frame %u  cycles %llu  instructions %llu\n",
        c.illegal_count ? "illegal opcodes seen  " : "", chf_machine_name(c.machine), c.vpos, c.hpos,
        c.frame, (unsigned long long)c.cycles, (unsigned long long)c.instructions);
}

static void dump(outbuf *o, int space, uint32_t addr, const uint8_t *b, int n)
{
    static const int width[5] = { 4, 2, 4, 5, 2 };
    static const unsigned mask[5] = { 0xffff, 0xff, 0xffff, 0xfffff, 0xff };

    for (int i = 0; i < n; i += 16)
    {
        out(o, "%0*X ", width[space], (addr + (uint32_t)i) & mask[space]);
        for (int j = 0; j < 16; j++)
            out(o, j < n - i ? " %02X" : "   ", b[i + j]);
        out(o, "  ");
        for (int j = 0; j < 16 && i + j < n; j++)
            out(o, "%c", (b[i + j] >= 0x20 && b[i + j] < 0x7f) ? b[i + j] : '.');
        out(o, "\n");
    }
}

/* the mailbox's names in a dumped range */
static void dump_names(outbuf *o, int space, uint32_t addr, int n)
{
    if (space != CHF_SPACE_PROG)
        return;
    for (int i = 0; i < n; i++)
    {
        const char *nm = chf_mailbox_name((uint16_t)(addr + (uint32_t)i));
        if (nm)
            out(o, "  %04X %s\n", (unsigned)(addr + (uint32_t)i) & 0xffff, nm);
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
        if (bp[i].type & CHFDEBUG_BP_EXEC)    strcat(kinds, "x");
        if (bp[i].type & CHFDEBUG_BP_READ)    strcat(kinds, "r");
        if (bp[i].type & CHFDEBUG_BP_WRITE)   strcat(kinds, "w");
        if (bp[i].type & CHFDEBUG_BP_IN)      strcat(kinds, "in");
        if (bp[i].type & CHFDEBUG_BP_OUT)     strcat(kinds, "out");
        if (bp[i].type & CHFDEBUG_BP_VRAM)    strcat(kinds, "vram");
        if (bp[i].type & CHFDEBUG_BP_SCRATCH) strcat(kinds, "reg");
        out(o, "#%d %-4s ", bp[i].id, kinds);
        switch (bp[i].space)
        {
        case CHF_SPACE_IO:
            if (bp[i].start == bp[i].end)
                out(o, "port %u", bp[i].start);
            else
                out(o, "ports %u-%u", bp[i].start, bp[i].end);
            break;
        case CHF_SPACE_VRAM:
            out(o, "row %d col %d", bp[i].start / CHFDEBUG_VRAM_COLS, bp[i].start % CHFDEBUG_VRAM_COLS);
            if (bp[i].start != bp[i].end)
                out(o, " to row %d col %d", bp[i].end / CHFDEBUG_VRAM_COLS, bp[i].end % CHFDEBUG_VRAM_COLS);
            break;
        case CHF_SPACE_SCRATCH:
            if (bp[i].start == bp[i].end)
                out(o, "r%u", bp[i].start);
            else
                out(o, "r%u-r%u", bp[i].start, bp[i].end);
            break;
        default:
            if (bp[i].start == bp[i].end)
                out(o, "$%04X", bp[i].start);
            else
                out(o, "$%04X-$%04X", bp[i].start, bp[i].end);
            break;
        }
        out(o, "%s%s%s  hits %u\n", bp[i].enabled ? "" : " (disabled)",
            bp[i].condition[0] ? " if " : "", bp[i].condition, bp[i].hits);
    }
}

/* break / bpr / bpw / bpi / bpo / bps : "<a>[-<end>] [condition]" */
static void add_bp(chfdebug *d, outbuf *o, int type, int space, char *args)
{
    char *t = next_tok(&args);
    long a, b;
    int id;

    if (!t)
    {
        out(o, "usage: <addr>[-<end>] [condition]\n");
        return;
    }
    if (type == CHFDEBUG_BP_VRAM)
    {
        /* <row>,<col>[-<row>,<col>] */
        char *dash = strchr(t, '-');
        char *comma = strchr(t, ',');
        long r0, c0, r1, c1;
        char *c1s;

        if (!comma)
        {
            out(o, "usage: bpv <row>,<col>[-<row>,<col>] [condition]\n");
            return;
        }
        if (dash)
            *dash = '\0';
        *comma = '\0';
        if (eval(d, o, t, &r0) || eval(d, o, comma + 1, &c0))
            return;
        r1 = r0;
        c1 = c0;
        if (dash)
        {
            c1s = strchr(dash + 1, ',');
            if (!c1s)
            {
                out(o, "usage: bpv <row>,<col>[-<row>,<col>] [condition]\n");
                return;
            }
            *c1s = '\0';
            if (eval(d, o, dash + 1, &r1) || eval(d, o, c1s + 1, &c1))
                return;
        }
        if (r0 < 0 || r0 > 63 || r1 < 0 || r1 > 63 || c0 < 0 || c0 > 127 || c1 < 0 || c1 > 127)
        {
            out(o, "error: row 0-63, column 0-127\n");
            return;
        }
        a = r0 * CHFDEBUG_VRAM_COLS + c0;
        b = r1 * CHFDEBUG_VRAM_COLS + c1;
    }
    else if (type == CHFDEBUG_BP_SCRATCH)
    {
        char *dash = strchr(t + 1, '-');
        if (dash)
            *dash = '\0';
        if (reg_number(d, o, t, &a) || (dash && reg_number(d, o, dash + 1, &b)))
            return;
        if (!dash)
            b = a;
    }
    else if (range(d, o, t, &a, &b))
        return;
    if (a < 0 || b < 0 || a > 0xffff || b > 0xffff)
    {
        out(o, "error: address out of range\n");
        return;
    }
    id = chfdebug_breakpoint_add(d, type, space, (uint16_t)a, (uint16_t)b, rest_of(args));
    if (id < 0)
        out(o, "error: the condition does not parse, or the range is outside its space\n");
    else
        out(o, "breakpoint #%d\n", id);
}

/* "set <reg>": a chfdebug_reg, or a scratchpad register to poke */
static int reg_by_name(const char *n, int *scratch)
{
    static const struct { const char *name; int reg; } map[] = {
        {"a", CHF_REG_A}, {"w", CHF_REG_W}, {"is", CHF_REG_ISAR}, {"isar", CHF_REG_ISAR},
        {"pc", CHF_REG_PC}, {"pc0", CHF_REG_PC0}, {"pc1", CHF_REG_PC1},
        {"dc0", CHF_REG_DC0}, {"dc1", CHF_REG_DC1},
        {"j", CHF_REG_J}, {"h", CHF_REG_H}, {"k", CHF_REG_K}, {"q", CHF_REG_Q},
        {"s", CHF_REG_SIGN}, {"sign", CHF_REG_SIGN}, {"c", CHF_REG_CARRY}, {"carry", CHF_REG_CARRY},
        {"z", CHF_REG_ZERO}, {"zero", CHF_REG_ZERO}, {"o", CHF_REG_OVERFLOW},
        {"overflow", CHF_REG_OVERFLOW}, {"icb", CHF_REG_ICB},
    };
    static const struct { const char *name; int r; } halves[] = {
        {"hu", 10}, {"hl", 11}, {"ku", 12}, {"kl", 13}, {"qu", 14}, {"ql", 15},
    };

    *scratch = -1;
    if ((n[0] == 'r' || n[0] == 'R') && isdigit((unsigned char)n[1]))
    {
        char *end;
        long r = strtol(n + 1, &end, 10);
        if (*end == '\0' && r >= 0 && r < 64)
            return CHF_REG_R0 + (int)r;
    }
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcasecmp(map[i].name, n))
            return map[i].reg;
    for (unsigned i = 0; i < sizeof halves / sizeof halves[0]; i++)
        if (!strcasecmp(halves[i].name, n))
        {
            *scratch = halves[i].r;
            return CHF_REG_R0 + halves[i].r;
        }
    return -1;
}

static void show_cart(chfdebug *d, outbuf *o, int mailbox)
{
    chfdebug_cart c;
    char info[200];

    chfdebug_cart_get(d, &c);
    chfdebug_cart_info(d, info, sizeof info);
    out(o, "%s\n", info);
    if (!mailbox)
    {
        out(o, "type %s", c.type_name ? c.type_name : "?");
        if (c.description && c.type != CHF_CART_FUJINET)
            out(o, " (%s)", c.description);
        out(o, ", image %u bytes", c.rom_size);
        if (c.rom_crc)
            out(o, ", CRC %08X", c.rom_crc);
        out(o, "\n");
        if (c.type == CHF_CART_FUJINET)
        {
            out(o, "%s%s; APPBANK pages: %d, page %d at $4800\n",
                c.mailbox ? "claims the mailbox" : "no mailbox", c.app_live ? " (APPBANK image)" : "",
                c.npages, c.page);
            out(o, "staged: %s%s  swaps %d\n",
                c.staged_size ? (c.staged_mailbox ? "an app" : "a game") : "nothing",
                c.armed ? " (armed)" : "", c.swaps);
        }
        else if (c.ram_size)
            out(o, "RAM %u %s\n", c.ram_size,
                c.type == CHF_CART_MAZE || c.type == CHF_CART_HANGMAN ? "bits (2102)" : "bytes");
        if (c.type == CHF_CART_MULTI || c.type == CHF_CART_MULTI_OLD)
            out(o, "bank %d half %d\n", c.bank, c.half);
    }
    if (c.type == CHF_CART_FUJINET)
    {
        out(o, "status page $FC00: ACKSEQ %02X  STATUS %02X  ERR %d  REPLY_CMD %02X  RXLEN %u  "
               "BOOT %d %d%% err %d  TICK %u  queue %u\n",
            c.ackseq, c.status, c.last_error, c.reply_cmd, c.rxlen, c.boot_state, c.boot_pct,
            c.boot_err, c.tick, c.queue_depth);
    }
    if (mailbox)
    {
        out(o, "$F800-$FFFF as the console reads it (reply, status page $FC00, hotspots):\n");
        dump(o, CHF_SPACE_PROG, 0xfc00, c.status_page, 16);
        dump(o, CHF_SPACE_PROG, 0xf800, c.mailbox_page, 0x40);
    }
}

static void show_romc(chfdebug *d, outbuf *o, char *arg)
{
    chfdebug_romc r[64];
    long w = 16;
    int n;

    if (arg && !strcmp(arg, "on"))
    {
        chfdebug_romc_enable(d, 1);
        out(o, "recording the ROMC stream\n");
        return;
    }
    if (arg && !strcmp(arg, "off"))
    {
        chfdebug_romc_enable(d, 0);
        return;
    }
    if (arg && eval(d, o, arg, &w))
        return;
    if (w > 64)
        w = 64;
    if (w < 1)
        w = 1;
    n = chfdebug_romc_read(d, r, (int)w);
    if (!n)
        out(o, "romc is %s and empty\n", chfdebug_romc_enabled(d) ? "on" : "off (romc on)");
    for (int i = 0; i < n; i++)
        out(o, "%10llu  PC0=%04X  ROMC %02X  dbus %02X\n", (unsigned long long)r[i].cycles, r[i].pc0,
            r[i].romc, r[i].dbus);
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
        add_bp(d, &o, CHFDEBUG_BP_EXEC, CHF_SPACE_PROG, p);
    else if (!strcmp(cmd, "bpr"))
        add_bp(d, &o, CHFDEBUG_BP_READ, CHF_SPACE_PROG, p);
    else if (!strcmp(cmd, "bpw"))
        add_bp(d, &o, CHFDEBUG_BP_WRITE, CHF_SPACE_PROG, p);
    else if (!strcmp(cmd, "bpi"))
        add_bp(d, &o, CHFDEBUG_BP_IN, CHF_SPACE_IO, p);
    else if (!strcmp(cmd, "bpo"))
        add_bp(d, &o, CHFDEBUG_BP_OUT, CHF_SPACE_IO, p);
    else if (!strcmp(cmd, "bpv"))
        add_bp(d, &o, CHFDEBUG_BP_VRAM, CHF_SPACE_VRAM, p);
    else if (!strcmp(cmd, "bps"))
        add_bp(d, &o, CHFDEBUG_BP_SCRATCH, CHF_SPACE_SCRATCH, p);
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
                    out(&o, "no breakpoint #%ld or at $%04lX\n", id, v & 0xffff);
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
            out(&o, v >= 0 && v < 256 ? "$%02lX  %ld  o%lo  %%%s\n" : "$%04lX  %ld  o%lo  %%%s\n",
                (unsigned long)v & 0xffff, v, (unsigned long)v & 0xffff, bin);
        }
    }
    else if (!strcmp(cmd, "regs") || !strcmp(cmd, "r"))
        show_regs(d, &o);
    else if (!strcmp(cmd, "set"))
    {
        char *r = next_tok(&p);
        int scratch;
        int reg = r ? reg_by_name(r, &scratch) : -1;
        if (reg < 0)
            out(&o, "usage: set <reg> <expr>   (a w isar pc pc0 pc1 dc0 dc1 j h k q hu hl ku kl qu ql "
                    "icb o z c s r0-r63)\n");
        else if (eval(d, &o, rest_of(p), &v) == 0)
        {
            if (!chfdebug_is_stopped(d))
                out(&o, "the machine is running (stop first)\n");
            else
            {
                if (scratch >= 0)
                    chfdebug_write(d, CHF_SPACE_SCRATCH, (uint32_t)scratch, (uint8_t)v);
                else
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
                out(&o, "usage: mem [prog|scratch|vram|cart|io] <addr> [len]\n");
            return o.len;
        }
        if (w > 256)
            w = 256;
        if (w < 1)
            w = 1;
        chfdebug_read(d, space, (uint32_t)v, buf, (int)w);
        out(&o, "%s space\n", space_label[space]);
        dump(&o, space, (uint32_t)v, buf, (int)w);
        dump_names(&o, space, (uint32_t)v, (int)w);
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
                out(&o, "usage: poke [prog|scratch|vram|cart|io] <addr> <v...>\n");
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
    else if (!strcmp(cmd, "scratch"))
        o.len += chfdebug_text_scratch(dst + o.len, dstsz - o.len);
    else if (!strcmp(cmd, "isar"))
    {
        chfdebug_cpu c;
        chfdebug_cpu_get(d, &c);
        out(&o, "ISAR = o%02o (%d)  upper o%o lower o%o  -> r%d = $%02X\n", c.isar, c.isar, c.isar >> 3,
            c.isar & 7, c.isar, c.r[c.isar]);
    }
    else if (!strcmp(cmd, "kstack"))
        o.len += chfdebug_text_kstack(dst + o.len, dstsz - o.len);
    else if (!strcmp(cmd, "vram"))
    {
        char *t = next_tok(&p);
        v = -1;
        if (t && eval(d, &o, t, &v))
            return o.len;
        o.len += chfdebug_text_vram((int)v, dst + o.len, dstsz - o.len);
    }
    else if (!strcmp(cmd, "pal"))
        o.len += chfdebug_text_pal(dst + o.len, dstsz - o.len);
    else if (!strcmp(cmd, "video"))
        o.len += chfdebug_text_video(dst + o.len, dstsz - o.len);
    else if (!strcmp(cmd, "ports"))
        o.len += chfdebug_text_ports(dst + o.len, dstsz - o.len);
    else if (!strcmp(cmd, "sound"))
        o.len += chfdebug_text_sound(d, dst + o.len, dstsz - o.len);
    else if (!strcmp(cmd, "input") || !strcmp(cmd, "io") || !strcmp(cmd, "keys"))
        o.len += chfdebug_text_input(dst + o.len, dstsz - o.len);
    else if (!strcmp(cmd, "cart"))
        show_cart(d, &o, 0);
    else if (!strcmp(cmd, "mailbox"))
        show_cart(d, &o, 1);
    else if (!strcmp(cmd, "bios"))
        o.len += chfdebug_text_bios(dst + o.len, dstsz - o.len);
    else if (!strcmp(cmd, "romc"))
        show_romc(d, &o, next_tok(&p));
    else if (!strcmp(cmd, "label"))
    {
        char *t = next_tok(&p), *n = next_tok(&p);
        if (!t || eval(d, &o, t, &v))
            return o.len;
        chfdebug_set_label(d, (uint16_t)(v & 0xffff), n);
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
                out(&o, "%10llu %3d:%3d  %04X  %-22s A=%02X W=%02X IS=o%02o DC0=%04X\n",
                    (unsigned long long)tr[i].cycles, tr[i].vpos, tr[i].hpos, tr[i].pc, l.disasm,
                    tr[i].a, tr[i].w, tr[i].isar, tr[i].dc0);
            }
        }
    }
    else if (!strcmp(cmd, "save"))
    {
        char *k = next_tok(&p);
        char *f = rest_of(p);
        char msg[300];
        if (!k || !*f)
            out(&o, "usage: save <dis|mailbox|vram|scratch|cart|screen|vramshot|regs> <file>\n");
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
        static const char *const kinds[] = { "dis", "mailbox", "vram", "scratch", "cart", "screen",
                                             "vramshot", "regs", NULL };
        for (int i = 0; kinds[i]; i++)
            if (!strncmp(kinds[i], word, n))
            {
                out(&o, "%s\n", kinds[i]);
                count++;
            }
        return count;
    }
    if (!strncmp(prefix, "romc ", 5) && !strchr(prefix + 5, ' '))
    {
        static const char *const opts[] = { "on", "off", NULL };
        for (int i = 0; opts[i]; i++)
            if (!strncmp(opts[i], word, n))
            {
                out(&o, "%s\n", opts[i]);
                count++;
            }
        return count;
    }
    if ((!strncmp(prefix, "mem ", 4) && !strchr(prefix + 4, ' ')) ||
        (!strncmp(prefix, "poke ", 5) && !strchr(prefix + 5, ' ')))
    {
        static const char *const spaces[] = { "prog", "scratch", "vram", "cart", "io", NULL };
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
