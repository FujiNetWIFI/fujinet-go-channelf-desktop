/* carts.c -- the cartridge mappers: standard and Golf.
 *
 * Transposed from MAME's src/devices/bus/channelf/{slot,rom}.cpp
 * (BSD-3-Clause, copyright-holders Fabio Priuli -- see COMPLIANCE.md). The
 * software list's PCB feature is replaced by the one image CRC the list gives
 * the Golf mapper.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "carts.h"

#include <string.h>

uint32_t chf_crc32(const uint8_t *p, uint32_t n)
{
    static uint32_t table[256];
    static int ready;

    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;

            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = 1;
    }
    uint32_t crc = 0xffffffffu;
    while (n--)
        crc = table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

/* arcadia_rom_device::read_rom / extra_rom (the Golf cart inherits both) */
static uint8_t rom_read(chf_cart_t *c, uint16_t offset, int peek)
{
    (void)peek;
    return offset < c->rom_size ? c->rom[offset] : 0xff;
}

static uint8_t rom_extra(chf_cart_t *c, uint16_t offset, int peek)
{
    (void)peek;
    return (uint32_t)offset + 0x1000 < c->rom_size ? c->rom[offset + 0x1000] : 0xff;
}

static const chf_cart_ops_t rom_ops = { rom_read, rom_extra };

/* golf.bin, 6144 bytes, from hash/channelf.xml (slot "golf") */
#define GOLF_CRC  0xa23c7a01u
#define GOLF_SIZE 6144u

int chf_cart_identify(const uint8_t *img, uint32_t size)
{
    if (size == GOLF_SIZE && chf_crc32(img, size) == GOLF_CRC)
        return CHF_CART_GOLF;
    return CHF_CART_STD;
}

int chf_cart_init(chf_cart_t *c, int type, const uint8_t *rom, uint32_t size, const char **err)
{
    static const char *dummy;

    if (!err)
        err = &dummy;
    memset(c, 0, sizeof *c);
    c->type = type;
    c->rom = rom;
    c->rom_size = size;
    if (!rom || size == 0) {
        *err = "the cartridge image is empty";
        return -1;
    }
    switch (type) {
    case CHF_CART_STD:
        c->extra_lo = 0x2000;
        c->extra_hi = 0x7fff;
        c->ops = &rom_ops;
        return 0;
    case CHF_CART_GOLF:
        c->extra_lo = 0x4000;
        c->extra_hi = 0x4fff;
        c->ops = &rom_ops;
        return 0;
    default:
        *err = "unknown cartridge type";
        return -1;
    }
}

const char *chf_cart_type_name(int type)
{
    switch (type) {
    case CHF_CART_STD:     return "Standard";
    case CHF_CART_GOLF:    return "Golf";
    case CHF_CART_FUJINET: return "FujiNet";
    default:               return "none";
    }
}
