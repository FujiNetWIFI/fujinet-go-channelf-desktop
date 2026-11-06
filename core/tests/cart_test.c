/*
 * cart_test -- the Videocarts, and the FujiNet cartridge device and its transport
 * against a stand-in FujiNet on loopback, with no FujiNet runtime: the case the Linux
 * CI build (WITH_FUJINET=OFF) would otherwise never exercise.
 *
 * The slot is a set of function pointers (carts.h); the console side here is the bus
 * itself, with no CPU in the way: a read at $0800 up is read_rom(addr - $0800) unless
 * the cart has a RAM window there, a store is write_ram / write_bank, exactly how
 * machine.c routes them (and MAME's driver installs them).
 *
 * Videocarts: the standard ROM-only cart, Maze and Hangman's 2102 on the PSU ports,
 * Schach's 2K at $2800, the two multicarts' bank latches at $3000-$3FFF, the table
 * of the software list the type comes from, RESET. Expected values come from MAME's
 * rom.cpp as documented there (the 2102 is checked by property: every one of its 1024
 * cells distinct and each holding what was written, not by repeating the bit shuffle).
 *
 * The FujiNet cartridge: the connector decode ($0800-$47FF the ROM window, $4800 the
 * APPBANK page window, $8000-$F7FF 30K of RAM, the painted mailbox, the write-only
 * hotspot pages), the 60 Hz tick from the CPU's clock, a MOUNT_IMAGE over the wire
 * with a pushed image (staged, armed by BOOTLOCK, swapped in by a store to $FDFE),
 * APPBANK page selects, a game in the FujiNet slot, and the trace tap held to the bytes
 * actually on the wire. The stand-in answers like fujinet-pc's ROM media: a pushed
 * stream (OPEN, WRITE x N, CLOSE, each waiting for its ACK) and then the reply. POSIX
 * only (the loopback listener); SKIPs (77) elsewhere. The mapper checks run everywhere.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "carts.h"
#include "carts_table.h"
#include "chfmap.h"
#include "fuji_mailbox.h"
#include "fujiconfigrom.h"
#include "fujinet_cart.h"
#include "fujitcp_host.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        failures++;
}

/* ---- the console's side of the slot ---- */

static uint8_t cpu_read(chf_cart_t *c, uint16_t a, int peek)
{
    if (a < 0x800)
        return 0x00;                                      /* the BIOS: not this module's */
    if (c->ops->read_ram && a >= c->ram_lo && a <= c->ram_hi)
        return c->ops->read_ram(c, (uint16_t)(a - c->ram_lo), peek);
    return chf_cart_read_rom(c, (uint16_t)(a - 0x800), peek);
}

static void cpu_write(chf_cart_t *c, uint16_t a, uint8_t d)
{
    if (a < 0x800)
        return;
    if (c->ops->write_ram && a >= c->ram_lo && a <= c->ram_hi)
        c->ops->write_ram(c, (uint16_t)(a - c->ram_lo), d);
    else if (c->ops->write_bank && a >= c->bank_lo && a <= c->bank_hi)
        c->ops->write_bank(c, (uint16_t)(a - c->bank_lo), d);
}

static uint8_t io_read(chf_cart_t *c, uint8_t port, int peek)
{
    return c->ops->io_read && port >= c->io_lo && port <= c->io_hi ? c->ops->io_read(c, port, peek) : 0;
}

static void io_write(chf_cart_t *c, uint8_t port, uint8_t d)
{
    if (c->ops->io_write && port >= c->io_lo && port <= c->io_hi)
        c->ops->io_write(c, port, d);
}

/* ---- the plain mappers (MAME's slot.cpp / rom.cpp) ---- */

static uint8_t rom256k[0x40000];
static uint8_t rom_small[3072];

static void model_2102(chf_cart_t *c, uint8_t pa, uint8_t pb, const char *name)
{
    /* every (B byte, A bits 1-2) combination is a cell: write a pseudo-random bit into each, read them all
     * back: if two combinations aliased, or a bit went astray, a read would not match what was written last */
    static uint8_t want[1024];
    int bad = 0, ones = 0;
    char what[120];

    for (unsigned i = 0; i < 1024; i++) {
        const unsigned b = i & 0xff, ax = i >> 8;
        const uint8_t d = (uint8_t)(((i * 2654435761u) >> 13) & 1);

        want[i] = d;
        io_write(c, pb, (uint8_t)b);
        io_write(c, pa, (uint8_t)(1 | ((ax & 1) << 1) | ((ax >> 1) << 2) | (d << 3)));   /* bit 0: write */
    }
    for (unsigned i = 0; i < 1024; i++) {
        const unsigned b = i & 0xff, ax = i >> 8;

        io_write(c, pb, (uint8_t)b);
        io_write(c, pa, (uint8_t)(((ax & 1) << 1) | ((ax >> 1) << 2)));                   /* bit 0 clear: read */
        bad += (io_read(c, pa, 0) >> 7) != want[i];
        ones += want[i];
    }
    snprintf(what, sizeof what, "%s: 1024 distinct cells, each reads back what was written (%d ones)", name, ones);
    check(!bad, what);
    io_write(c, pb, 0x12);
    io_write(c, pa, 0x0c);
    check(io_read(c, pb, 0) == 0x12, "port B reads its own latch");
    check(io_read(c, pa, 1) == io_read(c, pa, 1) && c->addr_latch < 1024, "a peek of the data port changes nothing");
    chf_cart_reset(c);
    check(c->latch[0] == 0 && c->latch[1] == 0 && c->addr_latch == 0, "RESET clears the latches (MAME's device_reset)");
}

static void mapper_tests(void)
{
    static uint8_t img[8192], small[4096];
    chf_cart_t c;
    const char *err = NULL;

    for (unsigned i = 0; i < sizeof img; i++)
        img[i] = (uint8_t)(i * 7 + (i >> 8));
    memcpy(small, img, sizeof small);
    for (unsigned i = 0; i < sizeof rom256k; i++)
        rom256k[i] = (uint8_t)((i >> 13) ^ (i & 0xff) ^ (i >> 12 & 0x10));

    check(chf_crc32((const uint8_t *)"123456789", 9) == 0xcbf43926u, "CRC-32 is the zip polynomial");

    /* the standard cart */
    check(chf_cart_init(&c, CHF_CART_STD, small, 3072, &err) == 0, "a standard cart sets up");
    check(c.ram_hi < c.ram_lo && c.bank_hi < c.bank_lo && c.io_hi < c.io_lo, "with no RAM, bank latch or I/O");
    check(cpu_read(&c, 0x0800, 0) == small[0] && cpu_read(&c, 0x0800 + 3071, 0) == small[3071], "the ROM is at $0800");
    check(cpu_read(&c, 0x0800 + 3072, 0) == 0xff && cpu_read(&c, 0x2800, 0) == 0xff && cpu_read(&c, 0xffff, 0) == 0xff,
          "past its end it reads $FF, $2800 included");
    cpu_write(&c, 0x0900, 0x12);
    cpu_write(&c, 0x2800, 0x12);
    cpu_write(&c, 0x3000, 0x12);
    check(cpu_read(&c, 0x0900, 0) == small[0x100] && cpu_read(&c, 0x2800, 0) == 0xff, "stores go nowhere");
    check(chf_cart_init(&c, CHF_CART_STD, NULL, 0, &err) != 0 && err, "an empty image is refused with a reason");
    check(chf_cart_init(&c, 77, img, sizeof img, &err) != 0, "an unknown type is refused");

    /* Maze: the 2102 on ports $24/$25; Hangman: $20/$21 */
    chf_cart_init(&c, CHF_CART_MAZE, rom_small, 2048, &err);
    check(c.io_lo == 0x24 && c.io_hi == 0x25 && c.ram_size == 1024, "Maze: a 2102 on ports $24 and $25");
    model_2102(&c, 0x24, 0x25, "Maze");
    check(io_read(&c, 0x20, 0) == 0 && io_read(&c, 0x21, 0) == 0, "Maze answers nothing on Hangman's ports");
    chf_cart_init(&c, CHF_CART_HANGMAN, rom_small, 3072, &err);
    check(c.io_lo == 0x20 && c.io_hi == 0x21, "Hangman: ports $20 and $21");
    model_2102(&c, 0x20, 0x21, "Hangman");
    check(chf_cart_space_size(&c) == 3072 + 128, "its CART space is the ROM then the 1024 bits as 128 bytes");
    chf_cart_space_poke(&c, 3072 + 5, 0xa5);
    check(chf_cart_space_peek(&c, 3072 + 5) == 0xa5 && c.ram[5 * 8] == 1 && c.ram[5 * 8 + 1] == 0 && c.ram[5 * 8 + 2] == 1,
          "the bits pack least significant first");

    /* Schach: 2K at $2800 */
    chf_cart_init(&c, CHF_CART_CHESS, img, 6144, &err);
    check(c.ram_lo == 0x2800 && c.ram_hi == 0x2fff && c.ram_size == 0x800 && c.bank_hi < c.bank_lo, "chess: 2K of RAM at $2800-$2FFF");
    check(cpu_read(&c, 0x2800, 0) == 0 && cpu_read(&c, 0x2fff, 0) == 0, "it reads 0 until written");
    cpu_write(&c, 0x2800, 0x5a);
    cpu_write(&c, 0x2fff, 0xa5);
    cpu_write(&c, 0x3000, 0x77);
    check(cpu_read(&c, 0x2800, 0) == 0x5a && cpu_read(&c, 0x2fff, 0) == 0xa5 && cpu_read(&c, 0x3000, 0) == 0xff,
          "it stores and reads back; $3000 is not RAM");
    check(cpu_read(&c, 0x1fff, 0) == img[0x17ff] && cpu_read(&c, 0x2000, 0) == 0xff && cpu_read(&c, 0x27ff, 0) == 0xff,
          "the 6K ROM below the window is read as ROM, $FF past its end (the window replaces only $2800-$2FFF)");
    check(chf_cart_space_size(&c) == c.rom_size + 0x800 && chf_cart_space_peek(&c, c.rom_size) == 0x5a,
          "CART space: the ROM, then the RAM");
    chf_cart_reset(&c);
    check(cpu_read(&c, 0x2800, 0) == 0x5a, "RESET leaves the RAM contents");

    /* the multicarts */
    chf_cart_init(&c, CHF_CART_MULTI_OLD, rom256k, sizeof rom256k, &err);
    check(c.bank_lo == 0x3000 && c.bank_hi == 0x3fff && c.ram_lo == 0x2800, "multi_old: bank latch over $3000-$3FFF, RAM at $2800");
    check(cpu_read(&c, 0x0800, 0) == rom256k[0] && cpu_read(&c, 0x27ff, 0) == rom256k[0x1fff], "bank 0 first");
    cpu_write(&c, 0x3000, 5);
    check(c.bank == 5 && cpu_read(&c, 0x0800, 0) == rom256k[5 * 0x2000] && cpu_read(&c, 0x1234, 0) == rom256k[5 * 0x2000 + 0x0a34],
          "a store in the latch window selects an 8K bank of the 256K");
    cpu_write(&c, 0x3fff, 0x1f | 0xe0);
    check(c.bank == 31 && cpu_read(&c, 0x27ff, 0) == rom256k[31 * 0x2000 + 0x1fff], "anywhere in it, only the low 5 bits count (31)");
    cpu_write(&c, 0x4000, 2);
    check(c.bank == 31, "a store outside the window is ignored");
    cpu_write(&c, 0x3000, 0x20 | 3);
    check(c.bank == 3 && c.half == 0, "multi_old has no half bank: bit 5 is ignored");
    check(cpu_read(&c, 0x3000, 0) == 0xff && cpu_read(&c, 0x8000, 0) == 0xff, "past $2FFF it reads $FF");
    cpu_write(&c, 0x2900, 0x66);
    check(cpu_read(&c, 0x2900, 0) == 0x66, "the RAM at $2800");
    chf_cart_reset(&c);
    check(c.bank == 0 && cpu_read(&c, 0x2900, 0) == 0x66, "RESET returns the bank to 0 (MAME's device_reset) and keeps the RAM");

    chf_cart_init(&c, CHF_CART_MULTI, rom256k, sizeof rom256k, &err);
    cpu_write(&c, 0x3000, 0x20 | 4);
    check(c.bank == 4 && c.half == 1 && cpu_read(&c, 0x0800, 0) == rom256k[4 * 0x2000 + 0x1000],
          "multi: bit 5 selects the second 4K of the bank");
    cpu_write(&c, 0x3000, 4);
    check(c.half == 0 && cpu_read(&c, 0x0800, 0) == rom256k[4 * 0x2000], "and clears again");
    check(cpu_read(&c, 0x2800 + 3, 0) == 0, "RAM at $2800");

    /* identification */
    check(chf_cart_identify(img, sizeof img) == CHF_CART_CHESS, "a plain image is MAME's chess type (RAM at $2800: the by-path rule)");
    check(chf_cart_identify(rom256k, sizeof rom256k) == CHF_CART_MULTI, "a 256K image is a multicart");
    check(chf_cart_softlist_name(img, sizeof img) == NULL && chf_cart_softlist_description(img, sizeof img) == NULL,
          "an unknown image has no software list entry");
    {
        const unsigned n = (unsigned)(sizeof k_chf_softlist / sizeof k_chf_softlist[0]);
        int sorted = 1, types = 1, sizes = 1, names = 1, counts[CHF_CART_TYPE_COUNT] = { 0 };

        for (unsigned i = 0; i < n; i++) {
            const chf_softlist_entry *e = &k_chf_softlist[i];

            sorted &= i == 0 || k_chf_softlist[i - 1].crc < e->crc;
            types &= e->type >= CHF_CART_STD && e->type <= CHF_CART_MULTI;
            sizes &= e->size == 2048 || e->size == 3072 || e->size == 4096 || e->size == 6144 || e->size == 262144;
            names &= e->name[0] && e->description[0];
            counts[e->type]++;
        }
        check(n == 43 && sorted && types && sizes && names, "the generated table: 43 entries, CRCs unique, sizes and types valid, all named");
        check(counts[CHF_CART_MAZE] == 2 && counts[CHF_CART_HANGMAN] == 3 && counts[CHF_CART_CHESS] == 1 &&
                  counts[CHF_CART_MULTI] == 1 && counts[CHF_CART_MULTI_OLD] == 1 && counts[CHF_CART_STD] == 35,
              "of the list's PCBs: 2 Maze, 3 Hangman, 1 Schach, 1 + 1 multicarts, 35 standard");
    }
    check(!strcmp(chf_cart_type_name(CHF_CART_MAZE), "Maze (2102 RAM)") && !strcmp(chf_cart_type_name(CHF_CART_FUJINET), "FujiNet"),
          "the types have names");
}

#ifdef _WIN32
int main(void)
{
    mapper_tests();
    printf("%d failure(s)\n", failures);
    if (failures)
        return 1;
    printf("SKIP: the loopback stand-in is POSIX only\n");
    return 77;
}
#else

/* ---- the wire, as the trace saw it and as the stand-in did ---- */

typedef struct {
    uint8_t b[262144];
    size_t n;
} wire;

static wire traced_wire[2];     /* by the trace's dir: 0 read, 1 sent */

static void keep_trace(void *user, int dir, const uint8_t *buf, size_t len)
{
    wire *w = &traced_wire[dir ? 1 : 0];
    (void)user;
    if (w->n + len <= sizeof w->b) {
        memcpy(w->b + w->n, buf, len);
        w->n += len;
    }
}

/* A FujiBus packet with no params, SLIP-framed, as FujiNet sends one. */
static size_t slip_frame(uint8_t dev, uint8_t cmd, const uint8_t *data, size_t len, uint8_t *out)
{
    uint8_t pkt[1100];
    uint16_t ck = 0;
    size_t n = 6 + len, i, o = 0;

    pkt[0] = dev;
    pkt[1] = cmd;
    pkt[2] = (uint8_t)(n & 0xFF);
    pkt[3] = (uint8_t)(n >> 8);
    pkt[4] = 0;
    pkt[5] = 0;
    if (len)
        memcpy(pkt + 6, data, len);
    for (i = 0; i < n; i++) {
        ck = (uint16_t)(ck + pkt[i]);
        ck = (uint16_t)((ck >> 8) + (ck & 0xFF));
    }
    pkt[4] = (uint8_t)ck;
    out[o++] = 0xC0;
    for (i = 0; i < n; i++) {
        if (pkt[i] == 0xC0 || pkt[i] == 0xDB) {
            out[o++] = 0xDB;
            out[o++] = pkt[i] == 0xC0 ? 0xDC : 0xDD;
        } else {
            out[o++] = pkt[i];
        }
    }
    out[o++] = 0xC0;
    return o;
}

typedef struct {
    int listen_fd, fd;
    wire got, sent;             /* the stand-in's side of the wire */
    int ok;
} fake_fujinet;

static int fake_read_frame(fake_fujinet *f)
{
    int ends = 0;
    uint8_t c;
    while (ends < 2) {
        if (recv(f->fd, &c, 1, 0) != 1) return -1;
        if (f->got.n < sizeof f->got.b) f->got.b[f->got.n++] = c;
        if (c == 0xC0) ends++;
    }
    return 0;
}

static void fake_send(fake_fujinet *f, uint8_t dev, uint8_t cmd, const uint8_t *data, size_t len)
{
    uint8_t frame[2400];
    size_t n = slip_frame(dev, cmd, data, len, frame);
    memcpy(f->sent.b + f->sent.n, frame, n);
    f->sent.n += n;
    send(f->fd, frame, n, 0);
}

/* the image the stand-in pushes: the baked CONFIG itself (a flat 16K FujiNet app),
 * so the swap is to something that claims the mailbox */
static const uint8_t *pushed = _configrom;
#define PUSH_LEN FUJI_CONFIGROM_SIZE

static void *fake_main(void *arg)
{
    uint8_t open_hdr[5] = { 0, (uint8_t)PUSH_LEN, (uint8_t)(PUSH_LEN >> 8),
                            (uint8_t)(PUSH_LEN >> 16), (uint8_t)(PUSH_LEN >> 24) };
    static const uint8_t reply[1] = { 0x00 };
    fake_fujinet *f = arg;

    f->fd = accept(f->listen_fd, NULL, NULL);
    if (f->fd < 0 || fake_read_frame(f) != 0) return NULL;      /* MOUNT_IMAGE */
    fake_send(f, 0xFF, 0x4F, open_hdr, sizeof open_hdr);
    if (fake_read_frame(f) != 0) return NULL;                   /* its ACK */
    for (size_t off = 0; off < PUSH_LEN; off += 512) {
        fake_send(f, 0xFF, 0x57, pushed + off, PUSH_LEN - off < 512 ? PUSH_LEN - off : 512);
        if (fake_read_frame(f) != 0) return NULL;
    }
    fake_send(f, 0xFF, 0x43, NULL, 0);
    if (fake_read_frame(f) != 0) return NULL;
    fake_send(f, 0x70, 0x06, reply, sizeof reply);
    f->ok = 1;
    return NULL;
}

/* ---- the FujiNet cartridge ---- */

static chf_cart_t *fcart;

/* the mailbox register write: one store to $FD00 + register (the cart synthesizes the pair) */
static void reg_write(uint8_t reg, uint8_t val)
{
    cpu_write(fcart, (uint16_t)(0xfd00 + reg), val);
}

static void connector_tests(void)
{
    check(cpu_read(fcart, 0x0800, 1) == _configrom[0] && cpu_read(fcart, 0x47ff, 1) == _configrom[0x3fff],
          "$0800-$47FF is the 16K ROM window");
    check(cpu_read(fcart, 0x4800, 1) == 0xff && cpu_read(fcart, 0x67ff, 1) == 0xff && cpu_read(fcart, 0x6800, 1) == 0xff &&
              cpu_read(fcart, 0x7fff, 1) == 0xff,
          "the APPBANK window and $6800-$7FFF read $FF on a flat image");
    check(cpu_read(fcart, 0x8000, 1) == 0 && cpu_read(fcart, 0xf7ff, 1) == 0, "$8000-$F7FF is RAM, clear at power-on");
    cpu_write(fcart, 0x8000, 0x5a);
    cpu_write(fcart, 0xf7ff, 0xa5);
    cpu_write(fcart, 0x0900, 0x11);
    cpu_write(fcart, 0x4900, 0x22);
    check(cpu_read(fcart, 0x8000, 1) == 0x5a && cpu_read(fcart, 0xf7ff, 1) == 0xa5, "it reads back what the console stores");
    check(cpu_read(fcart, 0x0900, 1) == _configrom[0x100] && cpu_read(fcart, 0x4900, 1) == 0xff,
          "stores to the ROM window and the page window are swallowed");
    check(cpu_read(fcart, 0xfd00, 0) == 0xff && cpu_read(fcart, 0xfe40, 0) == 0xff && cpu_read(fcart, 0xff80, 0) == 0xff,
          "the register, REGDATA and TX pages are write-only: they read $FF");
    check(cpu_read(fcart, 0xfc09, 1) == 'F' && cpu_read(fcart, 0xfc0a, 1) == 'N', "the status page carries the cart's magic");
    check(cpu_read(fcart, 0xfc0b, 1) == FN_PROTO_VER && FN_PROTO_VER == 2, "and protocol 2");
}

int main(void)
{
    fake_fujinet f;
    pthread_t th;
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    char boip[64], why[200];
    chf_cart_status_t st;
    chf_fujinet_t *fj;
    uint64_t cycles = 0;
    int one = 1;

    mapper_tests();

    memset(&f, 0, sizeof f);
    f.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    setsockopt(f.listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (f.listen_fd < 0 || bind(f.listen_fd, (struct sockaddr *)&a, sizeof a) != 0 ||
        listen(f.listen_fd, 1) != 0 || getsockname(f.listen_fd, (struct sockaddr *)&a, &al) != 0) {
        printf("SKIP: no loopback listener here\n");
        return failures ? 1 : 77;
    }
    snprintf(boip, sizeof boip, "127.0.0.1:%d", ntohs(a.sin_port));
    pthread_create(&th, NULL, fake_main, &f);

    fujitcp_set_trace(keep_trace, NULL);

    fj = chf_fujinet_create();
    check(fj != NULL && chf_fujinet_create() == NULL, "the cart creates, once per process");
    fcart = chf_fujinet_cart(fj);
    check(fcart->type == CHF_CART_FUJINET && fcart->ram_lo == 0x0800 && fcart->ram_hi == 0xffff && fcart->ops->write_ram &&
              !fcart->ops->read_ram,
          "it claims $0800-$FFFF for stores (write_ram) and serves reads through read_rom");
    chf_fujinet_set_clock(fj, &cycles, 1789772.0);
    check(chf_fujinet_power_on(fj, NULL, 0, boip, true, why, sizeof why) == 0, "it powers on with CONFIG, the mailbox inline");
    chf_fujinet_status(fj, &st);
    check(st.mailbox && st.link_up && !st.app_live && st.npages == 0 && st.page == -1, "and dials the stand-in (a flat image)");
    connector_tests();

    /* the tick: floor(seconds * 60) from the CPU's own clock (1e18 / 1789772 attoseconds a clock) */
    cycles = 1789772ull * 2 + 10;
    check(cpu_read(fcart, 0xfc0e, 1) == 120 && cpu_read(fcart, 0xfc0f, 1) == 0, "the 60 Hz tick follows the CPU clock");
    cycles = 1789772ull * 2000 + 10;
    check(cpu_read(fcart, 0xfc0e, 1) == (120000 & 0xff) && cpu_read(fcart, 0xfc0f, 1) == ((120000 >> 8) & 0xff),
          "and wraps as a 16-bit count");
    chf_fujinet_status(fj, &st);
    check(st.tick == (uint16_t)120000, "the status reports it too");

    /* MOUNT_IMAGE: device $70, command $F8, no parameters, then SEQ */
    reg_write(FN_REG_DEVICE, 0x70);
    reg_write(FN_REG_CMD, 0xF8);
    reg_write(FN_REG_NPARAM, 0);
    reg_write(FN_REG_DATA_RST, 0);
    reg_write(FN_REG_SEQ, 1);
    pthread_join(th, NULL);
    check(f.ok, "the stand-in saw the request, pushed the image, and replied");
    chf_fujinet_frame(fj);
    chf_fujinet_status(fj, &st);
    check(st.ackseq == 1 && st.reply_cmd == 0x06, "ACKSEQ answers SEQ, with an ACK");
    check(cpu_read(fcart, 0xfc00, 1) == 1 && cpu_read(fcart, 0xf800, 1) == 0, "as the console reads it ($FC00, and the reply window at $F800)");
    check(st.boot_state == FN_BOOT_READY, "the push is staged: READY");
    check(st.staged_mailbox && !st.staged_app && st.staged_size == PUSH_LEN, "a flat FujiNet app of the pushed size, staged");
    check(!st.armed && st.swaps == 0, "not armed, and nothing has taken over yet");
    {
        chf_fj_event_t ev[64];
        uint64_t cursor = 0;
        int n = chf_fujinet_events(fj, &cursor, ev, 64), regs = 0;

        for (int i = 0; i < n; i++)
            regs += ev[i].kind == 1;
        check(n >= 5 && regs == 5 && ev[0].kind == 1 && ev[0].reg == FN_REG_DEVICE && ev[0].value == 0x70,
              "the event ring has the five register writes, in order");
        check(chf_fujinet_events(fj, &cursor, ev, 64) == 0, "and the cursor has consumed them");
    }

    /* the swap hotspot before the client armed it does nothing */
    cpu_write(fcart, 0xfd00 + FN_HOT_SWAP, 0);
    chf_fujinet_status(fj, &st);
    check(st.swaps == 0, "a store to $FDFE unarmed changes nothing");
    reg_write(FN_REG_BOOTLOCK, FN_BOOTLOCK_MAGIC);
    chf_fujinet_status(fj, &st);
    check(st.armed, "writing BOOTLOCK arms the swap");
    check(cpu_read(fcart, 0xfdfe, 0) == 0xff, "reading the hotspot swaps nothing (it reads $FF)");
    chf_fujinet_status(fj, &st);
    check(st.swaps == 0 && st.armed, "...");
    cpu_write(fcart, 0xfdfe, 0);
    chf_fujinet_status(fj, &st);
    check(st.swaps == 1 && !st.armed && st.mailbox && st.booted_game, "the console's store at $FDFE swaps it in, keeping the mailbox");
    check(cpu_read(fcart, 0x0800, 1) == _configrom[0] && cpu_read(fcart, 0x47ff, 1) == _configrom[0x3fff],
          "and the cart serves the new image");
    check(cpu_read(fcart, 0xfc09, 1) == 'F' && cpu_read(fcart, 0xfc00, 1) == 0,
          "the status page was repainted for the new client (ACKSEQ back to 0, magic present)");

    /* the trace, held to the wire */
    check(traced_wire[1].n == f.got.n && memcmp(traced_wire[1].b, f.got.b, f.got.n) == 0,
          "the trace's sent bytes are exactly what the stand-in received");
    check(traced_wire[0].n == f.sent.n && memcmp(traced_wire[0].b, f.sent.b, f.sent.n) == 0,
          "the trace's read bytes are exactly what the stand-in sent");

    /* an APPBANK image: 16K and two 8K pages, claimed */
    {
        static uint8_t app[16384 + 2 * 8192];

        memset(app, 0xff, sizeof app);
        app[0] = 0x55;
        memcpy(app + FN_ROM_CLAIM, "FUJI", 4);
        memset(app + 16384, 0xa1, 8192);
        memset(app + 16384 + 8192, 0xb2, 8192);
        check(chf_fujinet_power_on(fj, app, sizeof app, "", true, why, sizeof why) == 0, "an APPBANK image (16K + 2 pages) powers on");
        chf_fujinet_status(fj, &st);
        check(st.app_live && st.npages == 2 && st.page == 0, "two pages, page 0 mapped");
        check(cpu_read(fcart, 0x4800, 1) == 0xa1 && cpu_read(fcart, 0x67ff, 1) == 0xa1, "$4800-$67FF is page 0");
        cpu_write(fcart, 0xfd80 + 1, 0);
        chf_fujinet_status(fj, &st);
        check(st.page == 1 && cpu_read(fcart, 0x4800, 1) == 0xb2, "a store to $FD81 selects page 1");
        cpu_write(fcart, 0xfd80 + 5, 0);
        chf_fujinet_status(fj, &st);
        check(st.page == -1 && cpu_read(fcart, 0x4800, 1) == 0xff, "a page it does not have reads $FF");
        cpu_write(fcart, 0xfd80, 0);
        check(cpu_read(fcart, 0x4800, 1) == 0xa1, "and page 0 again");
        check(chf_fujinet_check_image(app, sizeof app - 1, NULL, why, sizeof why) == 0 && why[0],
              "an image that is not 16K + whole pages is refused with a reason");
    }

    /* power cycling the cart with a game: no claim, no mailbox, plain ROM */
    {
        static uint8_t game[20000];

        for (unsigned i = 0; i < sizeof game; i++)
            game[i] = (uint8_t)(i ^ 0x5a);
        game[0] = 0x55;
        check(chf_fujinet_power_on(fj, game, 4096, "", true, why, sizeof why) == 0,
              "a plain game image powers on in the FujiNet slot");
        chf_fujinet_status(fj, &st);
        check(!st.mailbox && st.booted_game, "with the mailbox dead");
        check(cpu_read(fcart, 0x0810, 1) == game[0x10] && cpu_read(fcart, 0x0800 + 4096, 1) == 0xff,
              "it is served as a 4K ROM padded with $FF");
        cpu_write(fcart, 0xfd05, 0x77);
        check(cpu_read(fcart, 0xfd05, 1) == 0x77 && cpu_read(fcart, 0xfc0e, 1) == 0, "the arena is plain RAM, with no tick painted");
        game[0] = 0x00;
        check(chf_fujinet_power_on(fj, game, sizeof game, "", true, why, sizeof why) != 0 && why[0],
              "an image without the $55 the BIOS looks for is refused with a reason");
        game[0] = 0x55;
        check(chf_fujinet_power_on(fj, game, 20000, "", true, why, sizeof why) != 0 && why[0],
              "as is one that is not 16K + k x 8K");
    }

    fujitcp_set_trace(NULL, NULL);
    chf_fujinet_destroy(fj);
    close(f.listen_fd);
    if (f.fd >= 0)
        close(f.fd);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
#endif
