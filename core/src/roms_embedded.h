/*
 * roms_embedded -- the table tools/roms/embed-roms.py generates into the
 * build tree: empty (the shipping configuration, WITH_CHF_ROMS=OFF) or the
 * recognised images a developer put in tools/roms/local/ (NOT redistributable;
 * see COMPLIANCE.md).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CHF_ROMS_EMBEDDED_H
#define CHF_ROMS_EMBEDDED_H

#include <stdint.h>

typedef struct {
    const char *file;          /* MAME's file name: "sl31253.rom" */
    const uint8_t *data;
    uint32_t size;
} chf_embedded_rom;

extern const chf_embedded_rom chf_embedded_roms[];
extern const unsigned chf_embedded_rom_count;

#endif /* CHF_ROMS_EMBEDDED_H */
