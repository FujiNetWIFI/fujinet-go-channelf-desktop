/*
 * test_zip.h -- write a stored (method 0) zip of a few entries, for the
 * tests that open a cartridge from a zip.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CHF_TEST_ZIP_H
#define CHF_TEST_ZIP_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static inline uint32_t test_zip_crc(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    return ~c;
}

static inline void test_zip_le(FILE *f, uint32_t v, int bytes)
{
    for (int i = 0; i < bytes; i++)
        fputc((int)((v >> (8 * i)) & 0xff), f);
}

/* Writes `n` entries (names[i], data[i], sizes[i]) to path; 1 on success. */
static inline int test_zip_write(const char *path, int n, const char *const *names,
                                 const uint8_t *const *data, const uint32_t *sizes)
{
    FILE *f = fopen(path, "wb");
    uint32_t offs[16], dir_start, dir_len = 0;
    long pos = 0;

    if (!f || n > 16) return 0;
    for (int i = 0; i < n; i++) {
        uint32_t nl = (uint32_t)strlen(names[i]);
        offs[i] = (uint32_t)pos;
        test_zip_le(f, 0x04034b50u, 4); test_zip_le(f, 10, 2); test_zip_le(f, 0, 2);
        test_zip_le(f, 0, 2); test_zip_le(f, 0, 2); test_zip_le(f, 0x21, 2);
        test_zip_le(f, test_zip_crc(data[i], sizes[i]), 4);
        test_zip_le(f, sizes[i], 4); test_zip_le(f, sizes[i], 4);
        test_zip_le(f, nl, 2); test_zip_le(f, 0, 2);
        fwrite(names[i], 1, nl, f);
        fwrite(data[i], 1, sizes[i], f);
        pos += 30 + nl + sizes[i];
    }
    dir_start = (uint32_t)pos;
    for (int i = 0; i < n; i++) {
        uint32_t nl = (uint32_t)strlen(names[i]);
        test_zip_le(f, 0x02014b50u, 4); test_zip_le(f, 20, 2); test_zip_le(f, 10, 2);
        test_zip_le(f, 0, 2); test_zip_le(f, 0, 2); test_zip_le(f, 0, 2); test_zip_le(f, 0x21, 2);
        test_zip_le(f, test_zip_crc(data[i], sizes[i]), 4);
        test_zip_le(f, sizes[i], 4); test_zip_le(f, sizes[i], 4);
        test_zip_le(f, nl, 2); test_zip_le(f, 0, 2); test_zip_le(f, 0, 2);
        test_zip_le(f, 0, 2); test_zip_le(f, 0, 2); test_zip_le(f, 0, 4);
        test_zip_le(f, offs[i], 4);
        fwrite(names[i], 1, nl, f);
        dir_len += 46 + nl;
    }
    test_zip_le(f, 0x06054b50u, 4); test_zip_le(f, 0, 2); test_zip_le(f, 0, 2);
    test_zip_le(f, (uint32_t)n, 2); test_zip_le(f, (uint32_t)n, 2);
    test_zip_le(f, dir_len, 4); test_zip_le(f, dir_start, 4); test_zip_le(f, 0, 2);
    return fclose(f) == 0;
}

#endif /* CHF_TEST_ZIP_H */
