/*
 * memtools -- see core/include/chfmemtools.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "chfmemtools.h"
#include "debugger_internal.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* the nominal top of each space (the CART space's real size depends on the
 * cartridge: 64K of image then the 32K arena at most) */
static long space_top(int space)
{
    static const long top[5] = { 0xFFFF, 63, 8191, 0x17FFF, 255 };
    return space >= 0 && space <= CHF_SPACE_IO ? top[space] : -1;
}

const char *chfmem_space_name(int space)
{
    static const char *const names[5] = { "PROG", "SCRATCH", "VRAM", "CART", "IO" };
    return space >= 0 && space <= CHF_SPACE_IO ? names[space] : "?";
}

/* ---- range parsing ------------------------------------------------------------ */

static int hexdigit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* a scratchpad register by name: r12, j, h, k, q, hu ... ql; -1 if it is none */
static int scratch_index(const char *tok)
{
    static const struct { const char *name; int reg; } names[] = {
        { "j", 9 }, { "h", 10 }, { "k", 12 }, { "q", 14 },
        { "hu", 10 }, { "hl", 11 }, { "ku", 12 }, { "kl", 13 }, { "qu", 14 }, { "ql", 15 },
    };

    if ((tok[0] == 'r' || tok[0] == 'R') && isdigit((unsigned char)tok[1]))
    {
        char *end;
        long num = strtol(tok + 1, &end, 10);
        if (*end == '\0')
            return num >= 0 && num < 64 ? (int)num : -2;
    }
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcasecmp(tok, names[i].name))
            return names[i].reg;
    return -1;
}

static int eval_addr(chfdebug *d, int space, const char *text, size_t len, long *out)
{
    char buf[128], err[64];
    long value;

    while (len && isspace((unsigned char)text[0])) { text++; len--; }
    while (len && isspace((unsigned char)text[len - 1])) len--;
    if (!len || len >= sizeof buf) return 0;
    memcpy(buf, text, len);
    buf[len] = '\0';

    if (space == CHF_SPACE_SCRATCH) {
        int reg = scratch_index(buf);
        size_t pos;
        if (reg == -2) return 0;
        if (reg >= 0) { *out = reg; return 1; }
        /* a bare number here is decimal, like "bps 59" */
        for (pos = 0; pos < len && isdigit((unsigned char)buf[pos]); pos++) ;
        if (pos == len) { *out = strtol(buf, NULL, 10); return *out <= space_top(space); }
    }
    if (space == CHF_SPACE_VRAM) {
        char *comma = strchr(buf, ',');
        if (comma) {
            long row, col;
            *comma = '\0';
            /* row and column are plain expressions: "10" is ten */
            if (chfdebug_eval(d, buf, &row, err, sizeof err) != 0 ||
                chfdebug_eval(d, comma + 1, &col, err, sizeof err) != 0 ||
                row < 0 || row > 63 || col < 0 || col > 127)
                return 0;
            *out = row * CHFDEBUG_VRAM_COLS + col;
            return 1;
        }
    }
    /* A bare 1-5 digit token is hex here (memory fields have always taken
     * "C000"), unless it names a label. */
    if (space != CHF_SPACE_SCRATCH && len <= (space == CHF_SPACE_CART ? 5u : 4u)) {
        size_t pos;
        for (pos = 0; pos < len && hexdigit(buf[pos]); pos++) ;
        if (pos == len && chfdebug_label_address(d, buf) < 0) {
            value = strtol(buf, NULL, 16);
            *out = value;
            return value <= space_top(space);
        }
    }
    if (chfdebug_eval(d, buf, &value, err, sizeof err) != 0)
        return 0;
    if (space == CHF_SPACE_PROG)
        value &= 0xFFFF;
    if (value < 0 || value > space_top(space))
        return 0;
    *out = value;
    return 1;
}

/* No operator characters: a number or a label. */
static int plain(const char *t, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++)
        if (strchr("+-*/%&|^<>=!~()[]{}", t[i])) return 0;
    return 1;
}

int chfsym_parse_range(chfdebug *d, int space, const char *text, uint32_t *low,
                       uint32_t *high)
{
    const char *dots, *cursor;
    size_t len;
    long first, last;

    if (!d || !text || !low || !high || space < 0 || space > CHF_SPACE_IO) return 0;
    while (isspace((unsigned char)*text)) text++;
    len = strlen(text);
    if (!len) return 0;

    dots = strstr(text, "..");
    if (dots) {
        if (!eval_addr(d, space, text, (size_t)(dots - text), &first) ||
            !eval_addr(d, space, dots + 2, strlen(dots + 2), &last))
            return 0;
    } else {
        int split = 0;
        for (cursor = text + 1; *cursor; cursor++) {
            if (*cursor != '-') continue;
            if (plain(text, (size_t)(cursor - text)) && plain(cursor + 1, strlen(cursor + 1)) &&
                eval_addr(d, space, text, (size_t)(cursor - text), &first) &&
                eval_addr(d, space, cursor + 1, strlen(cursor + 1), &last)) {
                split = 1;
                break;
            }
        }
        if (!split) {
            if (!eval_addr(d, space, text, len, &first)) return 0;
            last = first;
        }
    }
    if (first > last) { long swap = first; first = last; last = swap; }
    *low = (uint32_t)first;
    *high = (uint32_t)last;
    return 1;
}

/* ---- watches ------------------------------------------------------------------ */

static struct {
    pthread_mutex_t mtx;
    char text[CHFWATCH_MAX][96];
    uint8_t space[CHFWATCH_MAX];
    int count;
    unsigned generation;
} g_watch = { .mtx = PTHREAD_MUTEX_INITIALIZER };

int chfwatch_count(void)
{
    int n;
    pthread_mutex_lock(&g_watch.mtx);
    n = g_watch.count;
    pthread_mutex_unlock(&g_watch.mtx);
    return n;
}

int chfwatch_add(int space, const char *text)
{
    int idx = -1;

    if (!text || space < 0 || space > CHF_SPACE_IO) return -1;
    while (isspace((unsigned char)*text)) text++;
    if (!*text) return -1;
    pthread_mutex_lock(&g_watch.mtx);
    if (g_watch.count < CHFWATCH_MAX) {
        size_t len;
        idx = g_watch.count++;
        snprintf(g_watch.text[idx], sizeof g_watch.text[idx], "%s", text);
        g_watch.space[idx] = (uint8_t)space;
        len = strlen(g_watch.text[idx]);
        while (len && isspace((unsigned char)g_watch.text[idx][len - 1]))
            g_watch.text[idx][--len] = '\0';
        g_watch.generation++;
    }
    pthread_mutex_unlock(&g_watch.mtx);
    return idx;
}

void chfwatch_remove(int index)
{
    pthread_mutex_lock(&g_watch.mtx);
    if (index >= 0 && index < g_watch.count) {
        memmove(g_watch.text[index], g_watch.text[index + 1],
                (size_t)(g_watch.count - index - 1) * sizeof g_watch.text[0]);
        memmove(&g_watch.space[index], &g_watch.space[index + 1],
                (size_t)(g_watch.count - index - 1) * sizeof g_watch.space[0]);
        g_watch.count--;
        g_watch.generation++;
    }
    pthread_mutex_unlock(&g_watch.mtx);
}

void chfwatch_clear(void)
{
    pthread_mutex_lock(&g_watch.mtx);
    g_watch.count = 0;
    g_watch.generation++;
    pthread_mutex_unlock(&g_watch.mtx);
}

/* Valid until the next change; the windows are single-threaded users. */
const char *chfwatch_text(int index)
{
    const char *found = NULL;
    pthread_mutex_lock(&g_watch.mtx);
    if (index >= 0 && index < g_watch.count) found = g_watch.text[index];
    pthread_mutex_unlock(&g_watch.mtx);
    return found;
}

int chfwatch_space(int index)
{
    int space = CHF_SPACE_PROG;
    pthread_mutex_lock(&g_watch.mtx);
    if (index >= 0 && index < g_watch.count) space = g_watch.space[index];
    pthread_mutex_unlock(&g_watch.mtx);
    return space;
}

unsigned chfwatch_generation(void)
{
    unsigned gen;
    pthread_mutex_lock(&g_watch.mtx);
    gen = g_watch.generation;
    pthread_mutex_unlock(&g_watch.mtx);
    return gen;
}

int chfwatch_range(chfdebug *d, int index, uint32_t *low, uint32_t *high)
{
    char text[96];
    const char *text_ptr = chfwatch_text(index);

    if (!text_ptr) return 0;
    snprintf(text, sizeof text, "%s", text_ptr);
    return chfsym_parse_range(d, chfwatch_space(index), text, low, high);
}

/* an address as the space writes it */
static int addr_text(int space, uint32_t addr, char *dst, size_t n)
{
    switch (space) {
    case CHF_SPACE_SCRATCH: return snprintf(dst, n, "r%u", addr);
    case CHF_SPACE_VRAM:
        return snprintf(dst, n, "$%04X (row %u col %u)", addr, addr / CHFDEBUG_VRAM_COLS,
                        addr % CHFDEBUG_VRAM_COLS);
    case CHF_SPACE_CART:    return snprintf(dst, n, "$%05X", addr);
    case CHF_SPACE_IO:      return snprintf(dst, n, "$%02X", addr);
    default:                return snprintf(dst, n, "$%04X", addr);
    }
}

int chfwatch_format(chfdebug *d, int index, char *dst, int dstsz)
{
    char text[96], where[40];
    uint32_t low, high;
    const char *watch = chfwatch_text(index);
    const int space = chfwatch_space(index);
    int len = 0, i;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!watch) return 0;
    snprintf(text, sizeof text, "%s", watch);
    if (!chfsym_parse_range(d, space, text, &low, &high))
        return snprintf(dst, (size_t)dstsz, "%-14s ?", text);
    addr_text(space, low, where, sizeof where);
    len = snprintf(dst, (size_t)dstsz, "%-14s %s%s%s", text, space == CHF_SPACE_PROG ? "" : chfmem_space_name(space),
                 space == CHF_SPACE_PROG ? "" : " ", where);
    if (len >= dstsz) return dstsz - 1;
    if (!chfdebug_is_stopped(d))
        return len + snprintf(dst + len, (size_t)(dstsz - len), "  -");
    if (low == high) {
        uint8_t byte = 0;
        chfdebug_read(d, space, low, &byte, 1);
        return len + snprintf(dst + len, (size_t)(dstsz - len), "  $%02X (%u)", byte, byte);
    }
    {
        char top[40];
        addr_text(space, high, top, sizeof top);
        len += snprintf(dst + len, (size_t)(dstsz - len), "-%s ", top);
    }
    for (i = 0; i < 8 && low + (uint32_t)i <= high && len < dstsz - 4; i++) {
        uint8_t byte = 0;
        chfdebug_read(d, space, low + (uint32_t)i, &byte, 1);
        len += snprintf(dst + len, (size_t)(dstsz - len), " %02X", byte);
    }
    if (high - low >= 8 && len < dstsz - 4)
        len += snprintf(dst + len, (size_t)(dstsz - len), " ...");
    return len < dstsz ? len : dstsz - 1;
}

/* ---- search ------------------------------------------------------------------- */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int chfsearch_parse(int kind, const char *text, uint8_t *pat, int *count,
                    uint8_t *mask, int *flags)
{
    int cnt = 0;

    if (!text || !pat || !count || !mask || !flags) return 0;
    *mask = 0xFF;
    *flags = 0;
    if (kind == CHFSEARCH_TEXT) {
        const char *cursor;
        for (cursor = text; *cursor && cnt < CHFSEARCH_MAX; cursor++)
            pat[cnt++] = (uint8_t)*cursor;
        *flags = CHFDEBUG_SEARCH_NOCASE;
    } else {
        const char *cursor = text;
        while (*cursor) {
            int high, low;
            if (isspace((unsigned char)*cursor) || *cursor == ',') { cursor++; continue; }
            if (*cursor == '$') { cursor++; continue; }
            if (cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X')) { cursor += 2; continue; }
            high = hexval(*cursor);
            if (high < 0) return 0;
            low = hexval(cursor[1]);
            if (low < 0) { low = high; high = 0; cursor += 1; }   /* a lone digit is a byte */
            else cursor += 2;
            if (cnt >= CHFSEARCH_MAX) break;
            pat[cnt++] = (uint8_t)(high << 4 | low);
        }
    }
    *count = cnt;
    return cnt > 0;
}

/* ---- breakpoint description ----------------------------------------------------- */

int chfbp_describe(chfdebug *d, const chfdebug_breakpoint *b, char *dst,
                   int dstsz)
{
    static const struct { int bit; const char *name; } types[] = {
        { CHFDEBUG_BP_EXEC, "Exec" }, { CHFDEBUG_BP_READ, "Read" },
        { CHFDEBUG_BP_WRITE, "Write" }, { CHFDEBUG_BP_IN, "In" },
        { CHFDEBUG_BP_OUT, "Out" }, { CHFDEBUG_BP_VRAM, "VRAM" },
        { CHFDEBUG_BP_SCRATCH, "Scratch" },
    };
    char kinds[48] = "", label[48] = "";
    int len, i;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!b) return 0;
    for (i = 0; i < (int)(sizeof types / sizeof types[0]); i++) {
        /* read + write together is one "Access" entry */
        if ((b->type & CHFDEBUG_BP_ACCESS) == CHFDEBUG_BP_ACCESS) {
            if (types[i].bit == CHFDEBUG_BP_READ) {
                if (kinds[0]) strncat(kinds, "/", sizeof kinds - strlen(kinds) - 1);
                strncat(kinds, "Access", sizeof kinds - strlen(kinds) - 1);
                continue;
            }
            if (types[i].bit == CHFDEBUG_BP_WRITE) continue;
        }
        if (b->type & types[i].bit) {
            if (kinds[0]) strncat(kinds, "/", sizeof kinds - strlen(kinds) - 1);
            strncat(kinds, types[i].name, sizeof kinds - strlen(kinds) - 1);
        }
    }
    switch (b->type & (CHFDEBUG_BP_IN | CHFDEBUG_BP_OUT | CHFDEBUG_BP_VRAM | CHFDEBUG_BP_SCRATCH)) {
    case 0:
        /* a label only names a PROG address */
        chfdebug_address_label(d, b->start, label, sizeof label);
        if (b->start == b->end)
            len = snprintf(dst, (size_t)dstsz, "%s  $%04X", kinds, b->start);
        else
            len = snprintf(dst, (size_t)dstsz, "%s  $%04X-$%04X", kinds, b->start, b->end);
        break;
    case CHFDEBUG_BP_VRAM:
        len = snprintf(dst, (size_t)dstsz, "%s  row %u col %u", kinds, b->start / CHFDEBUG_VRAM_COLS,
                     b->start % CHFDEBUG_VRAM_COLS);
        if (b->start != b->end && len < dstsz)
            len += snprintf(dst + len, (size_t)(dstsz - len), " to row %u col %u",
                          b->end / CHFDEBUG_VRAM_COLS, b->end % CHFDEBUG_VRAM_COLS);
        break;
    case CHFDEBUG_BP_SCRATCH:
        if (b->start == b->end)
            len = snprintf(dst, (size_t)dstsz, "%s  r%u", kinds, b->start);
        else
            len = snprintf(dst, (size_t)dstsz, "%s  r%u-r%u", kinds, b->start, b->end);
        break;
    default:
        if (b->start == b->end)
            len = snprintf(dst, (size_t)dstsz, "%s  %u", kinds, b->start);
        else
            len = snprintf(dst, (size_t)dstsz, "%s  %u-%u", kinds, b->start, b->end);
        break;
    }
    if (len >= dstsz) return dstsz - 1;
    if (len < dstsz && label[0])
        len += snprintf(dst + len, (size_t)(dstsz - len), " (%s)", label);
    if (len < dstsz && b->condition[0])
        len += snprintf(dst + len, (size_t)(dstsz - len), "  if %s", b->condition);
    return len < dstsz ? len : dstsz - 1;
}

/* ---- memory map labels ---------------------------------------------------------- */

int chfmem_region_name(const chfdebug_region *map, int count, uint16_t addr,
                       char *dst, int dstsz)
{
    int i;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    for (i = 0; map && i < count; i++)
        if (addr >= map[i].low && addr <= map[i].high)
            return snprintf(dst, (size_t)dstsz, "%s", map[i].name);
    return 0;
}
