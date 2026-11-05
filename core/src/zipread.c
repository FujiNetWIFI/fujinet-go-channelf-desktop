/*
 * zipread.c -- see zipread.h.
 *
 * The INFLATE below is the canonical-Huffman, bit-at-a-time decoder of
 * Mark Adler's public-domain "puff" (zlib/contrib/puff), written out again
 * here: it is slow, small and easy to check, and a ROM import decodes a few
 * kilobytes. It enforces its output bound and detects short input, so a
 * corrupt entry fails instead of overrunning.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zipread.h"

/* ---- CRC-32 ---------------------------------------------------------------- */

uint32_t zipread_crc32(const uint8_t *p, size_t n)
{
    static const uint32_t nibble[16] = {
        0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu,
        0x76dc4190u, 0x6b6b51f4u, 0x4db26158u, 0x5005713cu,
        0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
        0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu,
    };
    uint32_t crc = 0xffffffffu;
    size_t i;

    for (i = 0; i < n; i++) {
        crc ^= p[i];
        crc = (crc >> 4) ^ nibble[crc & 0x0f];
        crc = (crc >> 4) ^ nibble[crc & 0x0f];
    }
    return ~crc;
}

/* ---- INFLATE ----------------------------------------------------------------- */

#define MAXBITS   15
#define MAXLCODES 286
#define MAXDCODES 30
#define MAXCODES  (MAXLCODES + MAXDCODES)
#define FIXLCODES 288

struct inflate_state {
    const uint8_t *in;
    size_t inlen, incnt;
    uint32_t bitbuf;
    int bitcnt;
    int eof;                      /* the input ran out */
    uint8_t *out;
    size_t outlen, outcnt;
};

struct huffman {
    short count[MAXBITS + 1];     /* codes of each length */
    short symbol[MAXCODES];       /* symbols in code order */
};

static int get_bits(struct inflate_state *s, int need)
{
    long val = s->bitbuf;

    while (s->bitcnt < need) {
        if (s->incnt == s->inlen) {
            s->eof = 1;
            return 0;
        }
        val |= (long)s->in[s->incnt++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = (uint32_t)(val >> need);
    s->bitcnt -= need;
    return (int)(val & ((1L << need) - 1));
}

static int stored_block(struct inflate_state *s)
{
    unsigned len, nlen;

    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->incnt + 4 > s->inlen)
        return -2;
    len = s->in[s->incnt] | (unsigned)s->in[s->incnt + 1] << 8;
    nlen = s->in[s->incnt + 2] | (unsigned)s->in[s->incnt + 3] << 8;
    s->incnt += 4;
    if (len != (~nlen & 0xffffu))
        return -2;
    if (s->incnt + len > s->inlen)
        return -2;
    if (s->outcnt + len > s->outlen)
        return 1;                 /* more output than the entry says */
    memcpy(s->out + s->outcnt, s->in + s->incnt, len);
    s->incnt += len;
    s->outcnt += len;
    return 0;
}

static int decode_symbol(struct inflate_state *s, const struct huffman *h)
{
    int len, code = 0, first = 0, index = 0, count;

    for (len = 1; len <= MAXBITS; len++) {
        code |= get_bits(s, 1);
        if (s->eof)
            return -1;
        count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -10;                   /* ran out of codes */
}

/* Build the decoding tables for `n` code lengths. Returns 0 for a complete
 * code, positive for an incomplete one, negative for an over-subscribed one. */
static int construct(struct huffman *h, const short *length, int n)
{
    int symbol, len, left;
    short offs[MAXBITS + 1];

    for (len = 0; len <= MAXBITS; len++)
        h->count[len] = 0;
    for (symbol = 0; symbol < n; symbol++)
        h->count[length[symbol]]++;
    if (h->count[0] == n)
        return 0;
    left = 1;
    for (len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return left;
    }
    offs[1] = 0;
    for (len = 1; len < MAXBITS; len++)
        offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (symbol = 0; symbol < n; symbol++)
        if (length[symbol] != 0)
            h->symbol[offs[length[symbol]]++] = (short)symbol;
    return left;
}

static int codes_block(struct inflate_state *s, const struct huffman *lencode,
                       const struct huffman *distcode)
{
    static const short lens[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const short lext[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    static const short dists[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
        8193, 12289, 16385, 24577 };
    static const short dext[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
    int symbol, len;
    unsigned dist;

    do {
        symbol = decode_symbol(s, lencode);
        if (symbol < 0)
            return symbol;
        if (symbol < 256) {
            if (s->outcnt == s->outlen)
                return 1;
            s->out[s->outcnt++] = (uint8_t)symbol;
        } else if (symbol > 256) {
            symbol -= 257;
            if (symbol >= 29)
                return -10;
            len = lens[symbol] + get_bits(s, lext[symbol]);
            symbol = decode_symbol(s, distcode);
            if (symbol < 0)
                return symbol;
            dist = (unsigned)(dists[symbol] + get_bits(s, dext[symbol]));
            if (s->eof)
                return -1;
            if (dist > s->outcnt)
                return -11;       /* distance before the start */
            if (s->outcnt + (size_t)len > s->outlen)
                return 1;
            while (len--) {
                s->out[s->outcnt] = s->out[s->outcnt - dist];
                s->outcnt++;
            }
        }
    } while (symbol != 256);
    return 0;
}

static int fixed_block(struct inflate_state *s)
{
    struct huffman lencode, distcode;
    short lengths[FIXLCODES];
    int symbol;

    for (symbol = 0; symbol < 144; symbol++) lengths[symbol] = 8;
    for (; symbol < 256; symbol++) lengths[symbol] = 9;
    for (; symbol < 280; symbol++) lengths[symbol] = 7;
    for (; symbol < FIXLCODES; symbol++) lengths[symbol] = 8;
    construct(&lencode, lengths, FIXLCODES);
    for (symbol = 0; symbol < MAXDCODES; symbol++) lengths[symbol] = 5;
    construct(&distcode, lengths, MAXDCODES);
    return codes_block(s, &lencode, &distcode);
}

static int dynamic_block(struct inflate_state *s)
{
    static const short order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    struct huffman lencode, distcode;
    short lengths[MAXCODES];
    int nlen, ndist, ncode, index, err, symbol, len;

    nlen = get_bits(s, 5) + 257;
    ndist = get_bits(s, 5) + 1;
    ncode = get_bits(s, 4) + 4;
    if (s->eof || nlen > MAXLCODES || ndist > MAXDCODES)
        return -3;

    for (index = 0; index < ncode; index++)
        lengths[order[index]] = (short)get_bits(s, 3);
    for (; index < 19; index++)
        lengths[order[index]] = 0;
    if (s->eof)
        return -1;
    if (construct(&lencode, lengths, 19) != 0)
        return -4;                /* the code-length code must be complete */

    index = 0;
    while (index < nlen + ndist) {
        symbol = decode_symbol(s, &lencode);
        if (symbol < 0)
            return symbol;
        if (symbol < 16) {
            lengths[index++] = (short)symbol;
        } else {
            len = 0;
            if (symbol == 16) {
                if (index == 0)
                    return -5;
                len = lengths[index - 1];
                symbol = 3 + get_bits(s, 2);
            } else if (symbol == 17) {
                symbol = 3 + get_bits(s, 3);
            } else {
                symbol = 11 + get_bits(s, 7);
            }
            if (s->eof)
                return -1;
            if (index + symbol > nlen + ndist)
                return -6;
            while (symbol--)
                lengths[index++] = (short)len;
        }
    }
    if (lengths[256] == 0)
        return -9;                /* no end-of-block code */

    err = construct(&lencode, lengths, nlen);
    if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1]))
        return -7;
    err = construct(&distcode, lengths + nlen, ndist);
    if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1]))
        return -8;
    return codes_block(s, &lencode, &distcode);
}

int zipread_inflate(const uint8_t *in, size_t insize, uint8_t *out, size_t outsize)
{
    struct inflate_state s;
    int last, type, err = 0;

    memset(&s, 0, sizeof s);
    s.in = in;
    s.inlen = insize;
    s.out = out;
    s.outlen = outsize;
    do {
        last = get_bits(&s, 1);
        type = get_bits(&s, 2);
        if (s.eof)
            return -1;
        if (type == 0)
            err = stored_block(&s);
        else if (type == 1)
            err = fixed_block(&s);
        else if (type == 2)
            err = dynamic_block(&s);
        else
            err = -1;
        if (err != 0)
            return err < 0 ? err : -100;
    } while (!last);
    return s.outcnt == outsize ? 0 : -101;
}

/* ---- the zip container --------------------------------------------------------- */

typedef struct {
    char *name;
    uint16_t flags, method;
    uint32_t crc, csize, usize, local_off;
} zip_entry;

struct zipread {
    FILE *fp;                     /* a file source, or */
    uint8_t *mem;                 /* a copy of a zip held in memory */
    size_t total;
    zip_entry *e;
    int n;
};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int src_read(const zipread *z, size_t off, void *dst, size_t n)
{
    if (off > z->total || n > z->total - off)
        return -1;
    if (z->mem) {
        memcpy(dst, z->mem + off, n);
        return 0;
    }
    if (fseek(z->fp, (long)off, SEEK_SET) != 0)
        return -1;
    return fread(dst, 1, n, z->fp) == n ? 0 : -1;
}

/* Parse the central directory of an opened source; 0 on success. */
static int parse(zipread *z)
{
    uint8_t head[4], *tail = NULL, *cd = NULL;
    size_t tailsize, tailoff, cd_size, cd_off, pos, i;
    int eocd = -1, n, k;

    if (z->total < 22 || src_read(z, 0, head, 4) != 0)
        return -1;
    /* a local header, or an empty zip's end record */
    if (!(head[0] == 'P' && head[1] == 'K' &&
          ((head[2] == 3 && head[3] == 4) || (head[2] == 5 && head[3] == 6))))
        return -1;

    tailsize = z->total < 22 + 65535 ? z->total : 22 + 65535;
    tailoff = z->total - tailsize;
    tail = malloc(tailsize);
    if (!tail || src_read(z, tailoff, tail, tailsize) != 0) {
        free(tail);
        return -1;
    }
    for (i = tailsize - 22 + 1; i-- > 0; )
        if (rd32(tail + i) == 0x06054b50u) {
            eocd = (int)i;
            break;
        }
    if (eocd < 0) {
        free(tail);
        return -1;
    }
    n = rd16(tail + eocd + 10);
    cd_size = rd32(tail + eocd + 12);
    cd_off = rd32(tail + eocd + 16);
    free(tail);
    if (n == 0xffff || cd_size == 0xffffffffu || cd_off == 0xffffffffu)
        return -1;                /* zip64 */
    if (cd_size > 16u * 1024 * 1024 || cd_off > z->total || cd_size > z->total - cd_off)
        return -1;

    cd = malloc(cd_size ? cd_size : 1);
    z->e = calloc((size_t)(n ? n : 1), sizeof *z->e);
    if (!cd || !z->e || src_read(z, cd_off, cd, cd_size) != 0) {
        free(cd);
        return -1;
    }
    pos = 0;
    for (k = 0; k < n; k++) {
        size_t namelen, extralen, commentlen;
        zip_entry *e = &z->e[z->n];

        if (pos + 46 > cd_size || rd32(cd + pos) != 0x02014b50u)
            break;
        namelen = rd16(cd + pos + 28);
        extralen = rd16(cd + pos + 30);
        commentlen = rd16(cd + pos + 32);
        if (pos + 46 + namelen + extralen + commentlen > cd_size)
            break;
        e->flags = rd16(cd + pos + 8);
        e->method = rd16(cd + pos + 10);
        e->crc = rd32(cd + pos + 16);
        e->csize = rd32(cd + pos + 20);
        e->usize = rd32(cd + pos + 24);
        e->local_off = rd32(cd + pos + 42);
        e->name = malloc(namelen + 1);
        if (!e->name)
            break;
        memcpy(e->name, cd + pos + 46, namelen);
        e->name[namelen] = '\0';
        for (i = 0; i < namelen; i++)         /* tolerate DOS separators */
            if (e->name[i] == '\\')
                e->name[i] = '/';
        z->n++;
        pos += 46 + namelen + extralen + commentlen;
    }
    free(cd);
    return 0;
}

zipread *zipread_open(const char *path)
{
    zipread *z;
    FILE *fp = path && *path ? fopen(path, "rb") : NULL;
    long size;

    if (!fp)
        return NULL;
    if (fseek(fp, 0, SEEK_END) != 0 || (size = ftell(fp)) < 22) {
        fclose(fp);
        return NULL;
    }
    z = calloc(1, sizeof *z);
    if (!z) {
        fclose(fp);
        return NULL;
    }
    z->fp = fp;
    z->total = (size_t)size;
    if (parse(z) != 0) {
        zipread_close(z);
        return NULL;
    }
    return z;
}

zipread *zipread_open_memory(const uint8_t *data, size_t size)
{
    zipread *z;

    if (!data || size < 22)
        return NULL;
    z = calloc(1, sizeof *z);
    if (!z)
        return NULL;
    z->mem = malloc(size);
    if (!z->mem) {
        free(z);
        return NULL;
    }
    memcpy(z->mem, data, size);
    z->total = size;
    if (parse(z) != 0) {
        zipread_close(z);
        return NULL;
    }
    return z;
}

void zipread_close(zipread *z)
{
    int i;

    if (!z)
        return;
    for (i = 0; i < z->n; i++)
        free(z->e[i].name);
    free(z->e);
    free(z->mem);
    if (z->fp)
        fclose(z->fp);
    free(z);
}

int zipread_count(const zipread *z) { return z ? z->n : 0; }

const char *zipread_name(const zipread *z, int i)
{
    return z && i >= 0 && i < z->n ? z->e[i].name : "";
}

uint32_t zipread_size(const zipread *z, int i)
{
    return z && i >= 0 && i < z->n ? z->e[i].usize : 0;
}

uint32_t zipread_crc(const zipread *z, int i)
{
    return z && i >= 0 && i < z->n ? z->e[i].crc : 0;
}

int zipread_extract(zipread *z, int i, uint8_t **data, uint32_t *size, uint32_t max)
{
    const zip_entry *e;
    uint8_t lh[30], *comp = NULL, *buf = NULL;
    size_t data_off;
    int rc = -1;

    if (!z || i < 0 || i >= z->n || !data || !size)
        return -1;
    e = &z->e[i];
    if ((e->flags & 1u) || e->usize > max)
        return -1;                /* encrypted, or bigger than the caller wants */
    if (e->method != 0 && e->method != 8)
        return -1;
    if (src_read(z, e->local_off, lh, 30) != 0 || rd32(lh) != 0x04034b50u)
        return -1;
    data_off = (size_t)e->local_off + 30 + rd16(lh + 26) + rd16(lh + 28);
    if (data_off > z->total || e->csize > z->total - data_off)
        return -1;

    buf = malloc(e->usize ? e->usize : 1);
    if (!buf)
        return -1;
    if (e->method == 0) {
        if (e->csize != e->usize || src_read(z, data_off, buf, e->usize) != 0)
            goto out;
    } else {
        comp = malloc(e->csize ? e->csize : 1);
        if (!comp || src_read(z, data_off, comp, e->csize) != 0)
            goto out;
        if (zipread_inflate(comp, e->csize, buf, e->usize) != 0)
            goto out;
    }
    if (zipread_crc32(buf, e->usize) != e->crc)
        goto out;
    *data = buf;
    *size = e->usize;
    buf = NULL;
    rc = 0;
out:
    free(comp);
    free(buf);
    return rc;
}
