/* carts.c -- the Videocart mappers: standard, Maze and Hangman (2102 RAM on
 * the PSU ports), Schach (2K RAM) and the two multicarts.
 *
 * Transposed from MAME's src/devices/bus/chanf/{slot,rom}.cpp (BSD-3-Clause,
 * copyright-holders Fabio Priuli -- see COMPLIANCE.md), after Sean Riddle's
 * documentation of the 2102. The type of a plain image comes from the
 * software list's hashes (carts_table.h, generated from hash/channelf.xml:
 * identification data only), else MAME's by-path rule.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "carts.h"

#include <string.h>

#include "carts_table.h"

uint32_t chf_crc32(const uint8_t *p, uint32_t n)
{
    static uint32_t table[256];
    static int ready;
    uint32_t crc = 0xffffffffu;

    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;

            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = 1;
    }
    while (n--)
        crc = table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

/* ---- chanf_rom_device ------------------------------------------------------ */

static uint8_t rom_read(chf_cart_t *c, uint16_t offset, int peek)
{
    (void)peek;
    return offset < c->rom_size ? c->rom[offset] : 0xff;
}

/* ---- the 2102 (Maze and Hangman): common_read_2102 / common_write_2102 ----- */

static uint8_t ram2102_read(chf_cart_t *c, uint8_t port, int peek)
{
    const unsigned offset = (unsigned)(port - c->io_lo);

    if (offset == 0) {
        if (c->rw == 0) {
            const uint8_t d0 = c->ram[c->addr_latch & 0x3ff] & 1;

            if (!peek) {
                c->addr = c->addr_latch;
                c->data0 = d0;
            }
            return (uint8_t)((c->latch[0] & 0x7f) | (d0 << 7));
        }
        return c->latch[0];
    }
    return c->latch[1];
}

static void ram2102_write(chf_cart_t *c, uint8_t port, uint8_t data)
{
    const unsigned offset = (unsigned)(port - c->io_lo);

    if (offset == 0) {
        c->latch[0] = data;
        c->rw = data & 1;
        /* bits 2 and 3 of the address come from this write */
        c->addr_latch = (uint16_t)((c->addr_latch & 0x3f3) | (((data >> 2) & 1) << 2) |
                                   (((data >> 1) & 1) << 3));
        c->addr = c->addr_latch;
        c->data0 = (data >> 3) & 1;
        if (c->rw == 1)
            c->ram[c->addr & 0x3ff] = c->data0;
    } else {
        /* all the other bits come from this write, shuffled; the data is 8
         * bits, so bits 8 and 9 of the swap are always 0 */
        unsigned a;

        c->latch[1] = data;
        a = ((data >> 7 & 1) << 9) | ((data >> 6 & 1) << 8) | ((data >> 5 & 1) << 7) |
            ((data >> 3 & 1) << 6) | ((data >> 2 & 1) << 5) | ((data >> 1 & 1) << 4) |
            ((data >> 4 & 1) << 1) | (data & 1);
        c->addr_latch = (uint16_t)((c->addr_latch & 0x0c) | a);
    }
}

static void ram2102_reset(chf_cart_t *c)
{
    c->latch[0] = c->latch[1] = 0;
    c->addr = c->addr_latch = 0;
    c->rw = c->data0 = 0;
}

/* ---- the 3853's 2K (Schach, the multicarts): common_read/write_3853 -------- */

static uint8_t ram3853_read(chf_cart_t *c, uint16_t offset, int peek)
{
    (void)peek;
    return offset < c->ram_size ? c->ram[offset] : 0xff;
}

static void ram3853_write(chf_cart_t *c, uint16_t offset, uint8_t data)
{
    if (offset < c->ram_size)
        c->ram[offset] = data;
}

/* ---- the multicarts --------------------------------------------------------- */

static uint8_t multi_old_read(chf_cart_t *c, uint16_t offset, int peek)
{
    uint32_t a;

    (void)peek;
    if (offset >= 0x2000)
        return 0xff;
    a = (uint32_t)offset + (uint32_t)c->bank * 0x2000u;
    return a < c->rom_size ? c->rom[a] : 0xff;
}

static void multi_old_bank(chf_cart_t *c, uint16_t offset, uint8_t data)
{
    (void)offset;
    c->bank = data & 0x1f;
}

static uint8_t multi_read(chf_cart_t *c, uint16_t offset, int peek)
{
    uint32_t a;

    (void)peek;
    if (offset >= 0x2000)
        return 0xff;
    a = (uint32_t)offset + (uint32_t)c->bank * 0x2000u + (uint32_t)c->half * 0x1000u;
    return a < c->rom_size ? c->rom[a] : 0xff;
}

static void multi_bank(chf_cart_t *c, uint16_t offset, uint8_t data)
{
    (void)offset;
    c->bank = data & 0x1f;
    c->half = (data >> 5) & 1;
}

static void multi_reset(chf_cart_t *c)
{
    c->bank = c->half = 0;
}

static const chf_cart_ops_t ops_std = { .read_rom = rom_read };
static const chf_cart_ops_t ops_2102 = {
    .read_rom = rom_read, .io_read = ram2102_read, .io_write = ram2102_write, .reset = ram2102_reset,
};
static const chf_cart_ops_t ops_chess = {
    .read_rom = rom_read, .read_ram = ram3853_read, .write_ram = ram3853_write,
};
static const chf_cart_ops_t ops_multi_old = {
    .read_rom = multi_old_read, .read_ram = ram3853_read, .write_ram = ram3853_write,
    .write_bank = multi_old_bank, .reset = multi_reset,
};
static const chf_cart_ops_t ops_multi = {
    .read_rom = multi_read, .read_ram = ram3853_read, .write_ram = ram3853_write,
    .write_bank = multi_bank, .reset = multi_reset,
};

/* ---- identification ------------------------------------------------------- */

static const chf_softlist_entry *softlist_find(const uint8_t *img, uint32_t size)
{
    uint32_t crc;

    if (!img || !size)
        return NULL;
    crc = chf_crc32(img, size);
    for (unsigned i = 0; i < sizeof k_chf_softlist / sizeof k_chf_softlist[0]; i++)
        if (k_chf_softlist[i].crc == crc && k_chf_softlist[i].size == size)
            return &k_chf_softlist[i];
    return NULL;
}

int chf_cart_identify(const uint8_t *img, uint32_t size)
{
    const chf_softlist_entry *e = softlist_find(img, size);

    if (e)
        return e->type;
    /* get_default_card_software: 256K is a multicart, everything else is
     * taken to be on a PCB with RAM at $2800 (homebrew written for Schach) */
    return size == 0x40000 ? CHF_CART_MULTI : CHF_CART_CHESS;
}

const char *chf_cart_softlist_name(const uint8_t *img, uint32_t size)
{
    const chf_softlist_entry *e = softlist_find(img, size);

    return e ? e->name : NULL;
}

const char *chf_cart_softlist_description(const uint8_t *img, uint32_t size)
{
    const chf_softlist_entry *e = softlist_find(img, size);

    return e ? e->description : NULL;
}

const char *chf_cart_softlist_series(const uint8_t *img, uint32_t size)
{
    const chf_softlist_entry *e = softlist_find(img, size);

    return e ? e->series : NULL;
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
    c->ram_lo = 1;                  /* no RAM window, no bank window, no I/O */
    c->ram_hi = 0;
    c->bank_lo = 1;
    c->bank_hi = 0;
    c->io_lo = 1;
    c->io_hi = 0;
    if (!rom || size == 0) {
        *err = "the cartridge image is empty";
        return -1;
    }
    switch (type) {
    case CHF_CART_STD:
        c->ops = &ops_std;
        return 0;
    case CHF_CART_MAZE:             /* machine_start: IO 0x24-0x25 */
    case CHF_CART_HANGMAN:          /* machine_start: IO 0x20-0x21 */
        c->ops = &ops_2102;
        c->io_lo = type == CHF_CART_MAZE ? 0x24 : 0x20;
        c->io_hi = (uint8_t)(c->io_lo + 1);
        c->ram_size = 1024;
        return 0;
    case CHF_CART_CHESS:
        c->ops = &ops_chess;
        break;
    case CHF_CART_MULTI_OLD:
        c->ops = &ops_multi_old;
        break;
    case CHF_CART_MULTI:
        c->ops = &ops_multi;
        break;
    default:
        *err = "unknown cartridge type";
        return -1;
    }
    /* chess and the multicarts: 2K of RAM at $2800-$2FFF; multi's bank latch
     * is write-only at $3000-$3FFF */
    c->ram_lo = 0x2800;
    c->ram_hi = 0x2fff;
    c->ram_size = 0x800;
    if (type != CHF_CART_CHESS) {
        c->bank_lo = 0x3000;
        c->bank_hi = 0x3fff;
    }
    return 0;
}

void chf_cart_reset(chf_cart_t *c)
{
    if (c && c->ops && c->ops->reset)
        c->ops->reset(c);
}

const char *chf_cart_type_name(int type)
{
    switch (type) {
    case CHF_CART_STD:       return "Standard";
    case CHF_CART_MAZE:      return "Maze (2102 RAM)";
    case CHF_CART_HANGMAN:   return "Hangman (2102 RAM)";
    case CHF_CART_CHESS:     return "Chess (2K RAM)";
    case CHF_CART_MULTI_OLD: return "Multigame (earlier)";
    case CHF_CART_MULTI:     return "Multigame";
    case CHF_CART_FUJINET:   return "FujiNet";
    default:                 return "none";
    }
}

/* ---- the debugger's CART space ------------------------------------------- */

static uint32_t ram_bytes(const chf_cart_t *c)
{
    if (c->type == CHF_CART_MAZE || c->type == CHF_CART_HANGMAN)
        return 128;
    return c->ram_size;
}

uint32_t chf_cart_space_size(const chf_cart_t *c)
{
    return c ? c->rom_size + ram_bytes(c) : 0;
}

uint8_t chf_cart_space_peek(const chf_cart_t *c, uint32_t addr)
{
    if (!c)
        return 0xff;
    if (addr < c->rom_size)
        return c->rom[addr];
    addr -= c->rom_size;
    if (addr >= ram_bytes(c))
        return 0xff;
    if (c->type == CHF_CART_MAZE || c->type == CHF_CART_HANGMAN) {
        uint8_t v = 0;

        for (unsigned b = 0; b < 8; b++)
            v = (uint8_t)(v | ((c->ram[addr * 8 + b] & 1) << b));
        return v;
    }
    return c->ram[addr];
}

void chf_cart_space_poke(chf_cart_t *c, uint32_t addr, uint8_t data)
{
    if (!c || addr < c->rom_size)
        return;                     /* the ROM image is the caller's, read-only here */
    addr -= c->rom_size;
    if (addr >= ram_bytes(c))
        return;
    if (c->type == CHF_CART_MAZE || c->type == CHF_CART_HANGMAN) {
        for (unsigned b = 0; b < 8; b++)
            c->ram[addr * 8 + b] = (data >> b) & 1;
        return;
    }
    c->ram[addr] = data;
}
