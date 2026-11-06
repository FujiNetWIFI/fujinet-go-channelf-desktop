/*
 * symbols.c -- see symbols.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "symbols.h"
#include "fuji_mailbox.h"

void symtab_init(symtab *t)
{
    memset(t, 0, sizeof *t);
    symtab_add_builtins(t);
}

void symtab_free(symtab *t)
{
    free(t->v);
    memset(t, 0, sizeof *t);
}

void symtab_clear_user(symtab *t)
{
    int w = 0;
    for (int i = 0; i < t->n; i++)
        if (t->v[i].builtin)
            t->v[w++] = t->v[i];
    t->n = w;
}

void symtab_remove_at(symtab *t, uint16_t addr)
{
    int w = 0;
    for (int i = 0; i < t->n; i++)
        if (t->v[i].builtin || t->v[i].addr != addr)
            t->v[w++] = t->v[i];
    t->n = w;
}

int symtab_add(symtab *t, uint16_t addr, int bank, const char *name, int builtin)
{
    sym_entry *e;

    if (!name || !*name)
        return -1;
    if (t->n == t->cap)
    {
        int cap = t->cap ? t->cap * 2 : 256;
        sym_entry *v = realloc(t->v, (size_t)cap * sizeof *v);
        if (!v)
            return -1;
        t->v = v;
        t->cap = cap;
    }
    e = &t->v[t->n++];
    e->addr = addr;
    e->bank = (int16_t)bank;
    e->builtin = (uint8_t)(builtin != 0);
    e->seq = t->seq++;
    {
        /* a longer name is clipped to the table's width */
        size_t len = strlen(name);
        if (len >= sizeof e->name)
            len = sizeof e->name - 1;
        memcpy(e->name, name, len);
        e->name[len] = '\0';
    }
    t->sorted = 0;
    return 0;
}

static int cmp_addr(const void *a, const void *b)
{
    const sym_entry *x = a, *y = b;
    if (x->addr != y->addr)
        return (int)x->addr - (int)y->addr;
    /* user labels ahead of built-ins at the same address: the program's own
     * name for a place is the better one */
    if (x->builtin != y->builtin)
        return (int)x->builtin - (int)y->builtin;
    /* qsort is not stable everywhere (Windows'): keep the order they came in */
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

static void ensure_sorted(symtab *t)
{
    if (!t->sorted)
    {
        qsort(t->v, (size_t)t->n, sizeof *t->v, cmp_addr);
        t->sorted = 1;
    }
}

/* First index with v[i].addr >= addr. */
static int lower_bound(symtab *t, uint16_t addr)
{
    int lo = 0, hi = t->n;
    while (lo < hi)
    {
        int mid = (lo + hi) / 2;
        if (t->v[mid].addr < addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static const sym_entry *best_at(symtab *t, int i, uint16_t addr, int bank)
{
    const sym_entry *any = NULL;
    for (; i < t->n && t->v[i].addr == addr; i++)
    {
        const sym_entry *e = &t->v[i];
        if (e->bank == SYM_BANK_PORT)
            continue;
        if (e->bank == SYM_BANK_ANY)
        {
            if (!any)
                any = e;
        }
        else if (bank == SYM_BANK_ANY || e->bank == bank)
        {
            return e;   /* the label of the bank that is mapped (or a guess) */
        }
    }
    return any;
}

const char *symtab_lookup(symtab *t, uint16_t addr, int bank)
{
    const sym_entry *e;
    ensure_sorted(t);
    e = best_at(t, lower_bound(t, addr), addr, bank);
    return e ? e->name : NULL;
}

const char *symtab_lookup_near(symtab *t, uint16_t addr, int bank, int max_offset,
                               uint16_t *offset)
{
    ensure_sorted(t);
    for (int d = 0; d <= max_offset && d <= addr; d++)
    {
        uint16_t a = (uint16_t)(addr - d);
        const sym_entry *e = best_at(t, lower_bound(t, a), a, bank);
        if (e && !e->builtin)
        {
            if (offset)
                *offset = (uint16_t)d;
            return e->name;
        }
        if (e && d == 0)
        {
            if (offset)
                *offset = 0;
            return e->name;
        }
    }
    return NULL;
}

int symtab_find(symtab *t, const char *name)
{
    for (int i = 0; i < t->n; i++)
    {
        const char *a = t->v[i].name, *b = name;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b))
        {
            a++;
            b++;
        }
        if (!*a && !*b)
            return t->v[i].addr;
    }
    return -1;
}

const char *symtab_port_name(symtab *t, unsigned port)
{
    for (int i = 0; i < t->n; i++)
        if (t->v[i].bank == SYM_BANK_PORT && t->v[i].addr == port)
            return t->v[i].name;
    return NULL;
}

const char *symtab_name_at(symtab *t, int i)
{
    return (i >= 0 && i < t->n) ? t->v[i].name : NULL;
}

/* ---- loaders ---- */

static char *trim(char *s)
{
    char *e;
    while (*s && isspace((unsigned char)*s))
        s++;
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = '\0';
    return s;
}

static int is_ident(const char *s)
{
    if (!*s || !(isalpha((unsigned char)*s) || *s == '_' || *s == '.' || *s == '@'))
        return 0;
    for (; *s; s++)
        if (!(isalnum((unsigned char)*s) || *s == '_' || *s == '.' || *s == '@' || *s == '$'))
            return 0;
    return 1;
}

/* The F8 addresses 64K: a larger value is a constant, not an address. */
static void add_value(symtab *t, unsigned long long v, const char *name, int *count)
{
    if (v > 0xFFFFull)
        return;
    symtab_add(t, (uint16_t)v, SYM_BANK_ANY, name, 0);
    (*count)++;
}

int symtab_load(symtab *t, const char *path, char *msg, int msgsz)
{
    FILE *f = fopen(path, "r");
    char line[1024];
    int count = 0, wla_section = 0, as_section = 0, as_listing = 0, lines = 0;

    if (!f)
    {
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "Cannot open %s", path);
        return -1;
    }

    while (fgets(line, sizeof line, f))
    {
        char *s = trim(line);
        char name[256];
        unsigned bank, addr;
        unsigned long long v;
        char kind[32];

        lines++;
        if (!*s || *s == ';' || *s == '#')
            continue;

        /* Macroassembler AS -g MAP: "Symbols in Segment CODE", then
         * "name  Int  HEX ..." rows (other segments are values, not places) */
        if (strncmp(s, "Symbols in Segment ", 19) == 0)
        {
            as_section = strcmp(trim(s + 19), "CODE") == 0 ? 1 : 2;
            continue;
        }
        if (as_section)
        {
            char type[16], val[64];
            if (as_section == 1 && sscanf(s, "%255s %15s %63s", name, type, val) == 3 &&
                strcmp(type, "Int") == 0 && is_ident(name))
            {
                char *end;
                v = strtoull(val, &end, 16);
                if (*end == '\0')
                    add_value(t, v, name, &count);
            }
            continue;
        }

        /* Macroassembler AS listing (-L): after "Symbol Table", rows of
         * " name : value C |" (C = code), two or more to a line */
        if (strstr(s, "Symbol Table") != NULL)
        {
            as_listing = 1;
            continue;
        }
        if (as_listing)
        {
            char *cell = s;
            while (cell && *cell)
            {
                char *bar = strchr(cell, '|');
                char seg[8];
                if (bar)
                    *bar = '\0';
                if (sscanf(cell, " %255[^ :] : %llx %7s", name, &v, seg) == 3 &&
                    strcmp(seg, "C") == 0)
                {
                    char *nm = name;
                    if (*nm == '*')
                        nm++;       /* unused, but still a place */
                    if (is_ident(nm))
                        add_value(t, v, nm, &count);
                }
                cell = bar ? bar + 1 : NULL;
            }
            continue;
        }

        /* WLA-DX: [labels] / [definitions] sections */
        if (*s == '[')
        {
            wla_section = strncmp(s, "[labels]", 8) == 0 ? 1 : 2;
            continue;
        }
        if (wla_section == 1)
        {
            if (sscanf(s, "%x:%x %255s", &bank, &addr, name) == 3 && is_ident(name))
            {
                symtab_add(t, (uint16_t)addr, (int)bank, name, 0);
                count++;
            }
            continue;
        }
        if (wla_section == 2)
            continue;

        /* z88dk: "name = $AAAA ; addr, public, , module, section, file:line"
         * -- only addr rows are places; const rows are values */
        if (sscanf(s, "%255s = $%llx ; %31[^,]", name, &v, kind) == 3)
        {
            if (strcmp(trim(kind), "addr") == 0 && is_ident(name))
                add_value(t, v, name, &count);
            continue;
        }

        /* SDCC .noi: "DEF name 0xAAAA" */
        if (sscanf(s, "DEF %255s 0x%llx", name, &v) == 2 && is_ident(name))
        {
            add_value(t, v, name, &count);
            continue;
        }

        /* plain: "AAAA name", "$AAAA name", "name = $AAAA", "name equ AAAAh" */
        {
            char a[64], b[256];
            char *end;
            if (sscanf(s, "%63s %255s", a, b) == 2)
            {
                const char *num = a, *nm = b;
                if (*num == '$')
                    num++;
                v = strtoull(num, &end, 16);
                if (*end == '\0' && end != num && is_ident(nm))
                {
                    add_value(t, v, nm, &count);
                    continue;
                }
            }
            if (sscanf(s, "%255s %63s", b, a) == 2 && is_ident(b))
            {
                char rest[64];
                if (sscanf(s, "%*s = $%llx", &v) == 1 || sscanf(s, "%*s equ $%llx", &v) == 1 ||
                    sscanf(s, "%*s EQU $%llx", &v) == 1)
                {
                    add_value(t, v, b, &count);
                    continue;
                }
                if (sscanf(s, "%*s %*s %63s", rest) == 1)
                {
                    size_t n = strlen(rest);
                    if (n > 1 && (rest[n - 1] == 'h' || rest[n - 1] == 'H'))
                    {
                        rest[n - 1] = '\0';
                        v = strtoull(rest, &end, 16);
                        if (*end == '\0')
                        {
                            add_value(t, v, b, &count);
                            continue;
                        }
                    }
                }
            }
        }
    }
    fclose(f);

    if (msg && msgsz > 0)
    {
        const char *base = strrchr(path, '/');
        snprintf(msg, (size_t)msgsz, "%d label%s from %s", count, count == 1 ? "" : "s",
                 base ? base + 1 : path);
    }
    (void)lines;
    return count;
}

/* ---- the mailbox ranges ---- */

const char *chf_mailbox_name(uint16_t addr)
{
    static char buf[32];

    if (addr < FN_ARENA_BASE + FN_RAM_TOP)
        return NULL;
    if (addr >= FN_ARENA_BASE + FN_R_DATA && addr < FN_ARENA_BASE + FN_R_DATA + FN_R_SLICE_LEN)
        snprintf(buf, sizeof buf, "REPLY+%04X", addr - (FN_ARENA_BASE + FN_R_DATA));
    else if (addr >= FN_ARENA_BASE + FN_R_BASE && addr < FN_ARENA_BASE + FN_H_REGSEL)
        snprintf(buf, sizeof buf, "STATUS_PAGE+%02X", addr - (FN_ARENA_BASE + FN_R_BASE));
    else if (addr >= FN_ARENA_BASE + FN_H_REGSEL && addr < FN_ARENA_BASE + FN_H_REGSEL + FN_HOT_BANK)
        snprintf(buf, sizeof buf, "REGSEL+%02X", addr - (FN_ARENA_BASE + FN_H_REGSEL));
    else if (addr >= FN_ARENA_BASE + FN_H_REGSEL + FN_HOT_BANK &&
             addr <= FN_ARENA_BASE + FN_H_REGSEL + FN_HOT_BANK_LAST)
        snprintf(buf, sizeof buf, "APPBANK_SEL+%02X",
                 addr - (FN_ARENA_BASE + FN_H_REGSEL + FN_HOT_BANK));
    else if (addr == FN_ARENA_BASE + FN_H_REGSEL + FN_HOT_SWAP)
        return "SWAP";
    else if (addr >= FN_ARENA_BASE + FN_H_REGSEL && addr < FN_ARENA_BASE + FN_H_REGSEL + 0x100)
        snprintf(buf, sizeof buf, "REGSEL+%02X", addr - (FN_ARENA_BASE + FN_H_REGSEL));
    else if (addr >= FN_ARENA_BASE + FN_H_REGDATA && addr < FN_ARENA_BASE + FN_H_REGDATA + 0x100)
        snprintf(buf, sizeof buf, "REGDATA+%02X", addr - (FN_ARENA_BASE + FN_H_REGDATA));
    else if (addr >= FN_ARENA_BASE + FN_H_DATA)
        snprintf(buf, sizeof buf, "TXDATA+%02X", addr - (FN_ARENA_BASE + FN_H_DATA));
    else
        return NULL;
    return buf;
}
