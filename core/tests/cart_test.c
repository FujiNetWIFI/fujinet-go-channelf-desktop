/*
 * cart_test -- the cartridge slot's mappers, and the FujiNet cartridge device
 * and its transport against a stand-in FujiNet on loopback, with no FujiNet
 * runtime: the case the Linux CI build (WITH_FUJINET=OFF) would otherwise
 * never exercise.
 *
 * The console side is the bus itself: reads of the cartridge window through
 * the slot interface (carts.h) with no CPU in the way -- the console's $0000-
 * $0FFF as read_rom and $2000-$7FFF as extra_rom, exactly how MAME's driver
 * installs them. The mailbox is a set of read hotspots ($2D00 + register to
 * arm one, $2E00 + value to write it, $2F00 + byte to append to the TX
 * stream), the status page a window of repainted ROM ($2C00), the swap a read of
 * $2DFE. The firmware's host tests cover the protocol against a stub port; this
 * covers the port: the transposed device, the connector decode ($3000/$5000/
 * $7000 open bus, $4000/$6000 aliasing the two blocks), the portable
 * transport, the staging and the swap, and the trace tap held to the bytes
 * actually on the wire.
 *
 * The stand-in answers one MOUNT_IMAGE the way fujinet-pc's ROM media does:
 * a pushed stream (OPEN, WRITE x N, CLOSE, each waiting for its ACK) of the
 * baked CONFIG image, and then the reply. POSIX only (the loopback listener);
 * SKIPs (77) elsewhere. The mapper checks run everywhere.
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

#include "chfmap.h"
#include "carts.h"
#include "fuji_mailbox.h"
#include "fujinet_cart.h"
#include "fujitcp_host.h"
#include "fujiconfigrom.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

/* ---- the plain mappers (MAME's slot.cpp / rom.cpp) ---- */

static void mapper_tests(void)
{
    static uint8_t img[8192], small[4096], golf[6144];
    chf_cart_t c;
    const char *err = NULL;

    for (unsigned i = 0; i < sizeof img; i++)
        img[i] = (uint8_t)(i * 7 + (i >> 8));
    memcpy(small, img, sizeof small);
    memcpy(golf, img, sizeof golf);

    check(chf_crc32((const uint8_t *)"123456789", 9) == 0xcbf43926u, "CRC-32 is the zip polynomial");
    check(chf_cart_identify(img, sizeof img) == CHF_CART_STD, "a plain image is a standard cart");
    check(chf_cart_init(&c, CHF_CART_STD, img, sizeof img, &err) == 0, "a standard cart sets up");
    check(c.extra_lo == 0x2000 && c.extra_hi == 0x7fff, "its second block is installed over $2000-$7FFF");
    check(chf_cart_read_rom(&c, 0, 0) == img[0] && chf_cart_read_rom(&c, 0xfff, 0) == img[0xfff],
          "read_rom is the first 4K");
    check(chf_cart_extra_rom(&c, 0, 0) == img[0x1000] && chf_cart_extra_rom(&c, 0xfff, 0) == img[0x1fff],
          "extra_rom($2000) is the second 4K");
    check(chf_cart_extra_rom(&c, 0x1000, 0) == 0xff && chf_cart_extra_rom(&c, 0x5fff, 0) == 0xff,
          "and past the image it reads $FF");
    chf_cart_init(&c, CHF_CART_STD, small, sizeof small, &err);
    check(chf_cart_read_rom(&c, 0x123, 0) == small[0x123] && chf_cart_extra_rom(&c, 0, 0) == 0xff,
          "a 4K cart has nothing at $2000");

    check(chf_cart_init(&c, CHF_CART_GOLF, golf, sizeof golf, &err) == 0, "a Golf cart sets up");
    check(c.extra_lo == 0x4000 && c.extra_hi == 0x4fff, "its extra block is installed over $4000-$4FFF only");
    check(chf_cart_extra_rom(&c, 0x100, 0) == golf[0x1100] && chf_cart_extra_rom(&c, 0x7ff, 0) == golf[0x17ff],
          "extra_rom($4000) is the image from $1000");
    check(chf_cart_extra_rom(&c, 0x800, 0) == 0xff, "past the 6K image it is $FF");
    check(chf_cart_init(&c, CHF_CART_STD, NULL, 0, &err) != 0 && err, "an empty image is refused with a reason");
    check(chf_cart_init(&c, 77, img, sizeof img, &err) != 0, "an unknown type is refused");
    check(!strcmp(chf_cart_type_name(CHF_CART_GOLF), "Golf"), "the type has a name");
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
static size_t slip_frame(uint8_t dev, uint8_t cmd, const uint8_t *data,
                         size_t len, uint8_t *out)
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

static void fake_send(fake_fujinet *f, uint8_t dev, uint8_t cmd,
                      const uint8_t *data, size_t len)
{
    uint8_t frame[2400];
    size_t n = slip_frame(dev, cmd, data, len, frame);
    memcpy(f->sent.b + f->sent.n, frame, n);
    f->sent.n += n;
    send(f->fd, frame, n, 0);
}

/* the image the stand-in pushes: the baked CONFIG itself (a flat 8K FujiNet
 * app), so the swap is to something that claims the mailbox */
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

/* ---- the console's side: reads of the cartridge window ---- */

/* image offsets from $1000 up are console $2000 and up */
#define CON(img) ((uint16_t)((img) + 0x1000))

static chf_cart_t *cart;

/* a console read at CPU address a, as the machine's memory map does it */
static uint8_t cpu_read(uint16_t a, int peek)
{
    if (a < 0x1000)
        return chf_cart_read_rom(cart, a, peek);
    if (a >= cart->extra_lo && a <= cart->extra_hi)
        return chf_cart_extra_rom(cart, (uint16_t)(a - cart->extra_lo), peek);
    return 0;
}

/* the mailbox register write: a REGSEL read, then a REGDATA read */
static void reg_write(uint8_t reg, uint8_t val)
{
    (void)cpu_read(CON(FN_H_REGSEL + reg), 0);
    (void)cpu_read(CON(FN_H_REGDATA + val), 0);
}

static void connector_tests(chf_fujinet_t *fj)
{
    check(cpu_read(0x0000, 1) == _configrom[0] && cpu_read(0x0fff, 1) == _configrom[0xfff],
          "console $0000-$0FFF is the first 4K block");
    check(cpu_read(0x2000, 1) == _configrom[0x1000] && cpu_read(0x2aff, 1) == _configrom[0x1aff],
          "console $2000-$2FFF is the second block");
    check(cpu_read(0x3000, 1) == 0xff && cpu_read(0x3abc, 1) == 0xff &&
          cpu_read(0x5000, 1) == 0xff && cpu_read(0x7fff, 1) == 0xff,
          "$3000, $5000 and $7000 are open bus (A12 high deselects the cart)");
    check(cpu_read(0x4000, 1) == _configrom[0] && cpu_read(0x4abc, 1) == _configrom[0xabc],
          "$4000 aliases block 1 (A14 is not on the connector)");
    check(cpu_read(0x6000, 1) == _configrom[0x1000] && cpu_read(0x6abc, 1) == _configrom[0x1abc],
          "$6000 aliases block 2");
    (void)fj;
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
    check(fj != NULL, "the cart creates");
    cart = chf_fujinet_cart(fj);
    chf_fujinet_set_clock(fj, &cycles, 895000.0);
    check(chf_fujinet_power_on(fj, NULL, 0, boip, true, why, sizeof why) == 0,
          "it powers on with CONFIG, the mailbox inline");
    chf_fujinet_status(fj, &st);
    check(st.mailbox && st.link_up && !st.app_live, "and dials the stand-in (a flat image)");
    connector_tests(fj);
    check(cpu_read(CON(FN_R_MAGIC0), 1) == 'F' && cpu_read(CON(FN_R_MAGIC1), 1) == 'N',
          "the status page carries the cart's magic");
    check(cpu_read(CON(FN_R_PROTO_VER), 1) == FN_PROTO_VER, "and protocol 2");

    cycles = 895000ull * 2;
    check(cpu_read(0x2C0E, 1) == 120 && cpu_read(0x2C0F, 1) == 0, "the 60 Hz tick follows the CPU clock");
    cycles = 895000ull * 2000;
    check(cpu_read(0x2C0E, 1) == (120000 & 0xff) && cpu_read(0x2C0F, 1) == ((120000 >> 8) & 0xff),
          "and wraps as a 16-bit count");
    check(cpu_read(0x6C0E, 1) == (120000 & 0xff), "also through the $6000 alias");

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
    check(st.boot_state == FN_BOOT_READY, "the push is staged: READY");
    check(st.staged_mailbox && !st.staged_app && st.staged_size == PUSH_LEN,
          "a FujiNet app of the pushed size, staged");
    check(!st.armed && st.swaps == 0, "not armed, and nothing has taken over yet");

    /* the swap hotspot before the client armed it does nothing */
    (void)cpu_read(CON(FN_H_REGSEL + FN_HOT_SWAP), 0);
    chf_fujinet_status(fj, &st);
    check(st.swaps == 0, "a stray read of the swap hotspot, unarmed, changes nothing");
    reg_write(FN_REG_BOOTLOCK, FN_BOOTLOCK_MAGIC);
    chf_fujinet_status(fj, &st);
    check(st.armed, "writing BOOTLOCK arms the swap");
    (void)cpu_read(CON(FN_H_REGSEL + FN_HOT_SWAP), 1);
    chf_fujinet_status(fj, &st);
    check(st.swaps == 0 && st.armed, "a debugger's peek of the hotspot does not swap");
    (void)cpu_read(0x4000 + CON(FN_H_REGSEL + FN_HOT_SWAP), 0);
    chf_fujinet_status(fj, &st);
    check(st.swaps == 1 && !st.armed && st.mailbox,
          "the console's read at $6DFE (the alias of $2DFE) swaps it in, keeping the mailbox");
    check(cpu_read(0x0000, 1) == _configrom[0] && cpu_read(0x0fff, 1) == _configrom[0xfff] &&
          cpu_read(0x2000, 1) == _configrom[0x1000],
          "and the cart serves the new image");
    check(cpu_read(CON(FN_R_ACKSEQ), 1) == 1, "the status page was carried across the swap");

    /* the trace, held to the wire */
    check(traced_wire[1].n == f.got.n && memcmp(traced_wire[1].b, f.got.b, f.got.n) == 0,
          "the trace's sent bytes are exactly what the stand-in received");
    check(traced_wire[0].n == f.sent.n && memcmp(traced_wire[0].b, f.sent.b, f.sent.n) == 0,
          "the trace's read bytes are exactly what the stand-in sent");

    /* power cycling the cart with a game: no claim, no mailbox, plain ROM */
    {
        static uint8_t game[4096];
        uint8_t *probe = NULL;

        (void)probe;
        for (unsigned i = 0; i < sizeof game; i++)
            game[i] = (uint8_t)(i ^ 0x5a);
        check(chf_fujinet_power_on(fj, game, sizeof game, "", true, why, sizeof why) == 0,
              "a plain game image powers on in the FujiNet slot");
        chf_fujinet_status(fj, &st);
        check(!st.mailbox && st.booted_game, "with the mailbox dead");
        check(cpu_read(0x0010, 1) == game[0x10] && cpu_read(0x2000, 1) == 0xff,
              "it is served as a 4K ROM padded with $FF");
        check(cpu_read(0x2C0E, 1) == 0xff, "and no tick is painted");
        check(chf_fujinet_power_on(fj, game, 9000, "", true, why, sizeof why) != 0 && why[0],
              "an image that is not 8K + k x 2K is refused with a reason");
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
