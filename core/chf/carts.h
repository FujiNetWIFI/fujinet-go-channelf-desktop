/* carts.h -- the cartridge slot and its Videocarts.
 *
 * Transposed from MAME's Channel F cartridge bus (src/devices/bus/chanf/
 * slot.cpp, rom.cpp; BSD-3-Clause, copyright-holders Fabio Priuli -- see
 * COMPLIANCE.md).
 *
 * The slot is a set of function pointers, because the FujiNet cartridge
 * (fujinet_cart.c) sits in the same slot and the machine must not know which
 * it has. Each type sees what MAME's device_channelf_cart_interface sees:
 * read_rom(offset) for $0800 up (offset from $0800), read_ram/write_ram for
 * the RAM the driver installs (chess and multi: $2800-$2FFF), write_bank for
 * multi's $3000-$3FFF latch, and the 2102's two I/O ports for maze ($24/$25)
 * and hangman ($20/$21). A cart that has none of a hook leaves it NULL.
 *
 * A read made by the debugger passes peek = 1: it must change nothing (the
 * FujiNet cart's hotspots have side effects; MAME's side_effects_disabled()).
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * (transposed from MAME; see COMPLIANCE.md)
 */

#ifndef CHF_CARTS_H
#define CHF_CARTS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum chf_cart_type {
    CHF_CART_NONE = 0,
    CHF_CART_STD,       /* chanf_rom_device: ROM at $0800, $FF past its end */
    CHF_CART_MAZE,      /* chanf_maze_device: a 2102 (1024 x 1) on I/O $24/$25 */
    CHF_CART_HANGMAN,   /* chanf_hangman_device: a 2102 on I/O $20/$21 */
    CHF_CART_CHESS,     /* chanf_chess_device: 2K RAM at $2800-$2FFF */
    CHF_CART_MULTI_OLD, /* chanf_multi_old_device: 8K banks by $3000-$3FFF writes, RAM at $2800 */
    CHF_CART_MULTI,     /* chanf_multi_final_device: + the half-bank bit */
    CHF_CART_FUJINET,   /* the FujiNet cartridge (fujinet_cart.c) */
    CHF_CART_TYPE_COUNT
};

typedef struct chf_cart chf_cart_t;

typedef struct {
    uint8_t (*read_rom)(chf_cart_t *c, uint16_t offset, int peek);   /* $0800 up, offset from $0800 */
    uint8_t (*read_ram)(chf_cart_t *c, uint16_t offset, int peek);   /* RAM window, offset from ram_lo */
    void    (*write_ram)(chf_cart_t *c, uint16_t offset, uint8_t data);
    void    (*write_bank)(chf_cart_t *c, uint16_t offset, uint8_t data);  /* multi: $3000-$3FFF, offset from bank_lo */
    uint8_t (*io_read)(chf_cart_t *c, uint8_t port, int peek);       /* a 2102's ports; port is the absolute port */
    void    (*io_write)(chf_cart_t *c, uint8_t port, uint8_t data);
    /* MAME's device_reset, run on a console RESET: a 2102's latches and a
     * multicart's bank return to their power-on state (the RAM contents stay).
     * NULL for a cart that has none (the FujiNet cartridge does not reset). */
    void    (*reset)(chf_cart_t *c);
} chf_cart_ops_t;

struct chf_cart {
    const chf_cart_ops_t *ops;
    int type;                       /* enum chf_cart_type */

    /* the RAM window the machine routes to read_ram/write_ram, [ram_lo, ram_hi]
     * (none when ram_hi < ram_lo); a write-only window of the same kind
     * routed to write_bank, [bank_lo, bank_hi]; the I/O ports routed to
     * io_read/io_write, [io_lo, io_hi] (none when io_hi < io_lo). A cart with a
     * write_ram but no read_ram (the FujiNet cartridge) still has its reads
     * served by read_rom. */
    uint16_t ram_lo, ram_hi;
    uint16_t bank_lo, bank_hi;
    uint8_t io_lo, io_hi;

    /* the image (not owned) */
    const uint8_t *rom;
    uint32_t rom_size;

    /* a Videocart's RAM and latches (the debugger's CART space and Cart tab) */
    uint8_t ram[0x800];             /* chess / multi: 2K; a 2102: 1024 bits, one per byte's bit 0 */
    uint32_t ram_size;
    int bank, half;                 /* multi_old / multi */
    uint16_t addr_latch;            /* the 2102's 10-bit address */
    uint16_t addr;                  /* ... and the address a read or write last used */
    uint8_t latch[2];               /* the 2102's port latches */
    uint8_t rw, data0;              /* the 2102's write line (port A bit 0) and data bit */

    void *priv;                     /* fujinet: its own state */
};

/* What a plain image is: by its CRC-32 against MAME's software list
 * (carts_table.h, generated from hash/channelf.xml: hashes and names only),
 * else MAME's by-path rule (a 256K image is a multi, anything else chess). */
int chf_cart_identify(const uint8_t *img, uint32_t size);
/* The software list's short name and description for an image, or NULL. */
const char *chf_cart_softlist_name(const uint8_t *img, uint32_t size);
const char *chf_cart_softlist_description(const uint8_t *img, uint32_t size);
/* ... and its series ("Videocart 10", "SABA 20"; "" when it has none), or NULL. */
const char *chf_cart_softlist_series(const uint8_t *img, uint32_t size);

/* Set up c for `type` over an image of `size` bytes (not copied). Returns 0,
 * or -1 and a message in *err. */
int chf_cart_init(chf_cart_t *c, int type, const uint8_t *rom, uint32_t size, const char **err);

const char *chf_cart_type_name(int type);

/* A console RESET reaching the cart (see chf_cart_ops_t.reset). */
void chf_cart_reset(chf_cart_t *c);

/* A cart's own memory as the debugger's CART space shows it: the ROM image,
 * then the RAM (chess and multi: 2048 bytes; a 2102: its 1024 bits packed
 * into 128 bytes, address bit i in bit i % 8 of byte i / 8). chf_cart_space_size
 * is the total; peek/poke are side-effect free. Not for the FujiNet cartridge,
 * whose image and arena have their own accessors (fujinet_cart.h). */
uint32_t chf_cart_space_size(const chf_cart_t *c);
uint8_t chf_cart_space_peek(const chf_cart_t *c, uint32_t addr);
void chf_cart_space_poke(chf_cart_t *c, uint32_t addr, uint8_t data);

/* CRC-32 (IEEE: the zip/MAME polynomial). */
uint32_t chf_crc32(const uint8_t *p, uint32_t n);

static inline uint8_t chf_cart_read_rom(chf_cart_t *c, uint16_t off, int peek)
{
    return c && c->ops->read_rom ? c->ops->read_rom(c, off, peek) : 0xff;
}

#ifdef __cplusplus
}
#endif

#endif /* CHF_CARTS_H */
