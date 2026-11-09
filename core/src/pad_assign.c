/*
 * pad_assign -- the remembered pad -> port table (see pad_assign.h).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <string.h>

#include "pad_assign.h"

static int hex_value(int ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* A GUID as lower-case hex into dst; 0 if empty, too long or not hex. */
static int guid_norm(char *dst, const char *src, size_t len)
{
    size_t idx;
    if (!src || len == 0 || len >= CHF_PAD_GUID_MAX) return 0;
    for (idx = 0; idx < len; idx++) {
        if (hex_value((unsigned char)src[idx]) < 0) return 0;
        dst[idx] = (char)tolower((unsigned char)src[idx]);
    }
    dst[len] = '\0';
    return 1;
}

static int needs_escape(unsigned char ch)
{
    return ch < 0x20 || ch == 0x7F || ch == ';' || ch == '|' || ch == '=' || ch == '%';
}

/* The exact entry for a normalised GUID and a name, or -1. */
static int find_exact(const chf_pad_table *tab, const char *guid, const char *name)
{
    int idx;
    for (idx = tab->count - 1; idx >= 0; idx--)
        if (!strcmp(tab->entry[idx].guid, guid) && !strcmp(tab->entry[idx].name, name))
            return idx;
    return -1;
}

static void erase_at(chf_pad_table *tab, int idx)
{
    memmove(&tab->entry[idx], &tab->entry[idx + 1],
            (size_t)(tab->count - idx - 1) * sizeof tab->entry[0]);
    tab->count--;
}

void chf_pad_table_clear(chf_pad_table *tab)
{
    memset(tab, 0, sizeof *tab);
}

int chf_pad_table_find(const chf_pad_table *tab, const char *guid, const char *name)
{
    char norm[CHF_PAD_GUID_MAX];
    int idx;
    if (!tab || !name) return -1;
    if (guid && guid_norm(norm, guid, strlen(guid)))
        for (idx = tab->count - 1; idx >= 0; idx--)
            if (!strcmp(tab->entry[idx].guid, norm)) return idx;
    for (idx = tab->count - 1; idx >= 0; idx--)
        if (!strcmp(tab->entry[idx].name, name)) return idx;
    return -1;
}

int chf_pad_table_set(chf_pad_table *tab, const char *guid, const char *name, int port)
{
    char norm[CHF_PAD_GUID_MAX];
    int idx;
    if (!tab || !guid || !name || port < 0 || strlen(name) >= CHF_PAD_NAME_MAX ||
        !guid_norm(norm, guid, strlen(guid)))
        return -1;
    idx = find_exact(tab, norm, name);
    if (idx >= 0) {
        chf_pad_entry ent = tab->entry[idx];
        if (ent.port == port) return 0;
        ent.port = port;
        erase_at(tab, idx);      /* a changed entry is the newest */
        tab->entry[tab->count++] = ent;
        return 1;
    }
    if (tab->count >= CHF_PAD_ASSIGN_MAX) erase_at(tab, 0);
    idx = tab->count++;
    memcpy(tab->entry[idx].guid, norm, sizeof tab->entry[idx].guid);
    memcpy(tab->entry[idx].name, name, strlen(name) + 1);
    tab->entry[idx].port = port;
    return 1;
}

int chf_pad_table_remove(chf_pad_table *tab, const char *guid, const char *name,
                         int nkeep, const char *const *keepguid, const char *const *keepname)
{
    int removed = 0, idx, k;
    if (!tab || !name) return 0;
    /* every entry that find() would hand this device, so that none is left
     * to apply it on the next run (a stale GUID of the same pad included) --
     * but never one that find() also hands a pad that keeps its assignment */
    while ((idx = chf_pad_table_find(tab, guid, name)) >= 0) {
        for (k = 0; k < nkeep; k++)
            if (chf_pad_table_find(tab, keepguid[k], keepname[k]) == idx) return removed;
        erase_at(tab, idx);
        removed = 1;
    }
    return removed;
}

int chf_pad_table_encode(const chf_pad_table *tab, char *dst, size_t dstsz)
{
    static const char digits[] = "0123456789ABCDEF";
    size_t len = 0;
    int idx;
    char tail[16];

    if (!tab || !dst || dstsz == 0) return -1;
#define PUT(ch) do { if (len + 1 >= dstsz) { dst[0] = '\0'; return -1; } dst[len++] = (char)(ch); } while (0)
    for (idx = 0; idx < tab->count; idx++) {
        const chf_pad_entry *ent = &tab->entry[idx];
        const char *src;
        int num = ent->port, nd = 0;

        if (idx) PUT(';');
        for (src = ent->guid; *src; src++) PUT(*src);
        PUT('|');
        for (src = ent->name; *src; src++) {
            unsigned char ch = (unsigned char)*src;
            if (needs_escape(ch)) {
                PUT('%');
                PUT(digits[ch >> 4]);
                PUT(digits[ch & 15]);
            } else {
                PUT(ch);
            }
        }
        PUT('=');
        do { tail[nd++] = (char)('0' + num % 10); num /= 10; } while (num && nd < (int)sizeof tail);
        while (nd) PUT(tail[--nd]);
    }
#undef PUT
    dst[len] = '\0';
    return (int)len;
}

/* One "<guid>|<name>=<port>" entry; 1 if it parsed. */
static int parse_entry(chf_pad_entry *ent, const char *beg, const char *end, int nports)
{
    const char *bar, *eq, *src;
    size_t nlen = 0;
    long port = 0;

    for (bar = beg; bar < end && *bar != '|'; bar++) {}
    if (bar >= end) return 0;
    for (eq = bar + 1; eq < end && *eq != '='; eq++) {}
    if (eq >= end) return 0;
    if (!guid_norm(ent->guid, beg, (size_t)(bar - beg))) return 0;

    for (src = bar + 1; src < eq; src++) {
        unsigned char ch = (unsigned char)*src;
        if (nlen + 1 >= CHF_PAD_NAME_MAX || ch == '|') return 0;
        if (ch == '%') {
            int hi, lo;
            if (src + 2 >= eq) return 0;
            hi = hex_value((unsigned char)src[1]);
            lo = hex_value((unsigned char)src[2]);
            if (hi < 0 || lo < 0) return 0;
            ch = (unsigned char)(hi * 16 + lo);
            if (ch == 0) return 0;
            src += 2;
        }
        ent->name[nlen++] = (char)ch;
    }
    ent->name[nlen] = '\0';

    if (eq + 1 >= end || end - (eq + 1) > 3) return 0;
    for (src = eq + 1; src < end; src++) {
        if (*src < '0' || *src > '9') return 0;
        port = port * 10 + (*src - '0');
    }
    if (port >= nports) return 0;
    ent->port = (int)port;
    return 1;
}

int chf_pad_table_decode(chf_pad_table *tab, const char *text, int nports)
{
    const char *beg = text;
    chf_pad_entry ent;

    chf_pad_table_clear(tab);
    if (!text) return 0;
    while (*beg) {
        const char *end = beg;
        while (*end && *end != ';') end++;
        if (end > beg && parse_entry(&ent, beg, end, nports))
            chf_pad_table_set(tab, ent.guid, ent.name, ent.port);
        beg = *end ? end + 1 : end;
    }
    return tab->count;
}
