/*
 * zipread -- a minimal, dependency-free zip reader: stored and deflated
 * entries, with its own INFLATE (no zlib: the release builds are static and
 * portable, and a ROM import needs nothing more).
 *
 * Reads what MAME's zips are (and anything `zip` or Python's zipfile make):
 * the central directory at the end, method 0 or 8, directory paths in the
 * names, a data descriptor after the data. Encrypted entries, zip64 and the
 * other methods are listed but refuse to extract.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CHF_ZIPREAD_H
#define CHF_ZIPREAD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zipread zipread;

/* Open a zip file or a zip held in memory (copied: `data` need not outlive
 * the call). NULL when it is not a zip. */
zipread *zipread_open(const char *path);
zipread *zipread_open_memory(const uint8_t *data, size_t size);
void zipread_close(zipread *z);

int zipread_count(const zipread *z);
const char *zipread_name(const zipread *z, int i);     /* as stored, '/'-separated */
uint32_t zipread_size(const zipread *z, int i);        /* uncompressed bytes */
uint32_t zipread_crc(const zipread *z, int i);         /* the directory's CRC-32 */

/* Extracts entry i into a malloc'd buffer (caller frees) and checks its
 * size and CRC-32. Returns 0, or -1 when the entry is unsupported, corrupt,
 * larger than `max`, or out of memory. */
int zipread_extract(zipread *z, int i, uint8_t **data, uint32_t *size, uint32_t max);

/* The raw INFLATE (RFC 1951) the reader uses, exposed for the tests: decodes
 * `in` into `out` of exactly `outsize` bytes. Returns 0 when the stream is
 * valid and produces exactly that many bytes, else a negative code. */
int zipread_inflate(const uint8_t *in, size_t insize, uint8_t *out, size_t outsize);

/* CRC-32 (IEEE, the zip and MAME polynomial). */
uint32_t zipread_crc32(const uint8_t *p, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* CHF_ZIPREAD_H */
