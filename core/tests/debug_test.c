/*
 * debug_test -- the debugger contract (core/include/chfdebug.h) against a
 * running machine: attach stops it (before the machine exists, too), the CPU
 * view and its edits (PC refetches), every step kind, disassembly with labels
 * and the APPBANK tag, breakpoints of every class with conditions, the five
 * spaces, the video views, hit test, cell readout, write log and beam view
 * against a known VRAM program, the port and sound logs, the cartridge struct
 * with the FujiNet status page and its tick, symbols in every format, the
 * trace ring, Save, the prompt and its completion, the FujiNet trace (wired
 * through the cartridge's real transport to a stand-in FujiNet on loopback),
 * detach, and a power cycle with the debugger attached.
 *
 * The machine is the host's (core/chf/host.h) on the tests' own BIOS (test_chf.h:
 * a jump to the cartridge's entry) and a FujiNet app written out in machine code
 * below, so there is no ROM, no assembler and no dependence on pixels beyond the
 * ones the program draws. A last section runs CONFIG on the app's stand-in BIOS
 * and says so when the stand-in cannot yet run it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "test_chf.h"
#include "test_tmpdir.h"
#include "chfmap.h"
#include "debugger_internal.h"
#include "fujitrace.h"

static int failures;
static int last_ok = 1;
static void check_(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
    last_ok = ok;
}
#define check(ok, what) check_((ok) != 0, what)
/* what the last failed check saw */
#define show(fmt, ...) do { if (!last_ok) printf("      " fmt "\n", __VA_ARGS__); } while (0)

static int wait_stopped(chfdebug *d, int want, int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms) {
        if (!!chfdebug_is_stopped(d) == want) return 1;
        test_sleep_ms(2); waited += 2;
    }
    return 0;
}

/* Resume and wait for the next stop. */
static int run_until_stop(chfdebug *d, int timeout_ms)
{
    unsigned g = chfdebug_generation(d);
    int waited = 0;
    chfdebug_resume(d);
    while (waited < timeout_ms) {
        if (chfdebug_is_stopped(d) && chfdebug_generation(d) != g) return 1;
        test_sleep_ms(1); waited += 1;
    }
    return 0;
}

static int stop_now(chfdebug *d)
{
    chfdebug_stop(d);
    return wait_stopped(d, 1, 3000);
}

static chfdebug_cpu cpu(chfdebug *d)
{
    chfdebug_cpu c;
    chfdebug_cpu_get(d, &c);
    return c;
}

static char reason[128];
static int why(chfdebug *d)
{
    int addr = -1;
    chfdebug_stop_reason(d, reason, sizeof reason, &addr);
    return addr;
}

static long file_size(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) { fputs(text, f); fclose(f); }
}

static int find_line(const chfdebug_line *l, int n, const char *prefix)
{
    for (int i = 0; i < n; i++)
        if (strncmp(l[i].disasm, prefix, strlen(prefix)) == 0) return i;
    return -1;
}

/* Run to the exec breakpoint at addr (and take it away again). */
static int run_to_exec(chfdebug *d, uint16_t addr)
{
    int id = chfdebug_breakpoint_add(d, CHFDEBUG_BP_EXEC, CHF_SPACE_PROG, addr, addr, "");
    int ok = id > 0 && run_until_stop(d, 5000) && cpu(d).pc == addr;
    if (id > 0) chfdebug_breakpoint_remove(d, id);
    return ok;
}

/* ---- a stand-in FujiNet on loopback (POSIX) ----------------------------------- */

#ifndef _WIN32
typedef struct { int listen_fd, fd, frames, port; atomic_int stop; } fake_fujinet;

/* A SLIP-framed FujiBus ACK from the FUJI device with one payload byte. */
static size_t ack_frame(uint8_t *out)
{
    uint8_t pkt[7] = { 0x70, 0x06, 7, 0, 0, 0, 0 };
    uint16_t ck = 0;
    size_t o = 0;
    for (int i = 0; i < 7; i++) {
        ck = (uint16_t)(ck + pkt[i]);
        ck = (uint16_t)((ck >> 8) + (ck & 0xFF));
    }
    pkt[4] = (uint8_t)ck;
    out[o++] = 0xC0;
    for (int i = 0; i < 7; i++) out[o++] = pkt[i];
    out[o++] = 0xC0;
    return o;
}

/* Answers every request frame with an ACK until the console hangs up, then
 * waits for the next console (the test power-cycles the machine) until told to
 * stop. */
static void *fake_main(void *arg)
{
    fake_fujinet *f = arg;
    uint8_t c, reply[16];

    for (;;) {
        int ends = 0;

        f->fd = accept(f->listen_fd, NULL, NULL);
        if (f->fd < 0)
            break;
        if (atomic_load(&f->stop)) {
            close(f->fd);
            break;
        }
        while (recv(f->fd, &c, 1, 0) == 1) {
            if (c != 0xC0) continue;
            if (++ends == 2) {          /* a frame: leading and trailing delimiter */
                send(f->fd, reply, ack_frame(reply), 0);
                f->frames++;
                ends = 0;
            }
        }
        close(f->fd);
    }
    return NULL;
}
#endif

/* ---- the app under test --------------------------------------------------------
 *
 * A FujiNet app (a 16K window with the claim, and two APPBANK pages) on the
 * tests' BIOS, which jumps to $0802. Hand-assembled; every address below is a
 * console address and the checks lean on them.
 *
 *   0802 DCI $8000 / LIS 0 / ST            arena write 1 (at 0806)
 *   0807 DCI $8000 / LM / AI 1             arena read (at 080A)
 *   080D DCI $8000 / ST                    arena write 2 (at 0810)
 *   0811 plot row 10, col 20 = 2 with OUTS, the strobe at 081C
 *   0820 plot row 12, col 40 = 1 with the two-byte OUT, the strobe at 082E
 *   0834 LI $40 / OUTS 5 (1 kHz) ; 0837 LI 0 / OUTS 5 (off)
 *   083A INS 0 / IN 1                      port reads
 *   083D PI $0900                          a call that calls (sub1 -> sub3)
 *   0840 K = $0950 / PK                    a call through K (sub2)
 *   0847 JMP $0860
 *   0860 idle: r20 += 1 through ISAR, BR idle   (the scratchpad write at 0864)
 *   0900 sub1: LR K,P / PI sub3 / LR P,K / POP
 *   0910 sub3: LI 7 / POP        0950 sub2: LI 6 / POP
 *   0970 a FujiBus transaction (device $70, command $F4), then JMP idle
 *   09A0 select APPBANK page 1, JMP idle
 *   0A00 sub4: LR K,P / PI PUSHK / LI 8 / PI POPK / PK   (returns through K)   0A20 PI sub4, JMP idle
 *   09C0 the K stack: K = $1234, PUSHK, K = $5678, PUSHK, POPK, POPK, JMP idle
 */

#define APP_SIZE  (0x4000 + 2 * 0x2000)
#define A_ENTRY   0x0802
#define A_IDLE    0x0860
#define A_TXN     0x0970
#define A_PAGE    0x09A0
#define A_KBLK    0x09C0

static uint8_t g_lo[CF_BIOS_HALF], g_hi[CF_BIOS_HALF];
static uint8_t g_app[APP_SIZE];

static void put_(uint16_t addr, const uint8_t *code, size_t n)
{
    memcpy(g_app + (addr - 0x800), code, n);
}
#define PUT(addr, ...) do { static const uint8_t b_[] = { __VA_ARGS__ }; put_(addr, b_, sizeof b_); } while (0)

static void build_app(void)
{
    test_bios(g_lo, g_hi);
    memset(g_app, 0xff, sizeof g_app);
    g_app[0] = 0x55;
    g_app[1] = 0x00;
    memcpy(g_app + FN_ROM_CLAIM, FN_R_CLAIM_SIG, FN_R_CLAIM_LEN);
    memset(g_app + 0x4000, 0xA0, 0x2000);               /* APPBANK page 0 */
    memset(g_app + 0x6000, 0xA1, 0x2000);               /* page 1 */

    PUT(0x0802, 0x2a, 0x80, 0x00, 0x70, 0x17,                   /* DCI $8000 / LIS 0 / ST */
                0x2a, 0x80, 0x00, 0x16, 0x24, 0x01,             /* DCI / LM / AI 1 */
                0x2a, 0x80, 0x00, 0x17);                        /* DCI / ST */
    PUT(0x0811, 0x20, 0x7f, 0xb1, 0x20, 0xeb, 0xb4, 0x20, 0x35, 0xb5,
                0x20, 0x60, 0xb0, 0x20, 0x50, 0xb0);            /* row 10 col 20 = 2 */
    PUT(0x0820, 0x20, 0xbf, 0x27, 0x01, 0x20, 0xd7, 0x27, 0x04, 0x20, 0x33, 0x27, 0x05,
                0x20, 0x60, 0x27, 0x00, 0x20, 0x50, 0x27, 0x00); /* row 12 col 40 = 1 */
    PUT(0x0834, 0x20, 0x40, 0xb5, 0x20, 0x00, 0xb5);            /* sound 1 kHz, off */
    PUT(0x083a, 0xa0, 0x26, 0x01);                              /* INS 0 / IN 1 */
    PUT(0x083d, 0x28, 0x09, 0x00);                              /* PI $0900 */
    PUT(0x0840, 0x20, 0x09, 0x04, 0x20, 0x50, 0x05, 0x0c);      /* K = $0950 / PK */
    PUT(0x0847, 0x29, 0x08, 0x60);                              /* JMP idle */
    PUT(0x0860, 0x62, 0x6c, 0x4c, 0x1f, 0x5c, 0x90, 0xfa);      /* idle */
    PUT(0x0900, 0x08, 0x28, 0x09, 0x10, 0x09, 0x1c);            /* sub1 */
    PUT(0x0910, 0x20, 0x07, 0x1c);                              /* sub3 */
    PUT(0x0950, 0x20, 0x06, 0x1c);                              /* sub2 */
    PUT(0x0970, 0x20, 0x70, 0x2a, 0xfd, 0x00, 0x17,             /* REG_DEVICE = $70 */
                0x20, 0xf4, 0x2a, 0xfd, 0x01, 0x17,             /* REG_CMD = $F4 */
                0x70, 0x2a, 0xfd, 0x02, 0x17,                   /* REG_NPARAM = 0 */
                0x2a, 0xfc, 0x00, 0x16, 0x1f, 0x51,             /* r1 = ACKSEQ + 1 */
                0x2a, 0xfd, 0x10, 0x17,                         /* REG_SEQ = r1: launch */
                0x2a, 0xfc, 0x00, 0x16, 0xe1, 0x94, 0xfa,       /* wait for ACKSEQ == r1 */
                0x29, 0x08, 0x60);
    PUT(0x09a0, 0x71, 0x2a, 0xfd, 0x81, 0x17, 0x29, 0x08, 0x60); /* page 1, idle */
    PUT(0x09c0, 0x20, 0x12, 0x04, 0x20, 0x34, 0x05,             /* K = $1234 */
                0x28, 0x01, 0x07,                               /* PI PUSHK */
                0x20, 0x56, 0x04, 0x20, 0x78, 0x05,             /* K = $5678 */
                0x28, 0x01, 0x07,                               /* PI PUSHK */
                0x28, 0x01, 0x1e, 0x28, 0x01, 0x1e,             /* PI POPK, PI POPK */
                0x29, 0x08, 0x60);
    PUT(0x0a00, 0x08, 0x28, 0x01, 0x07, 0x20, 0x08, 0x28, 0x01, 0x1e, 0x0c);   /* sub4: returns with PK */
    PUT(0x0a20, 0x28, 0x0a, 0x00, 0x29, 0x08, 0x60);
}

static chf_host_opts_t host_opts(const char *boip)
{
    chf_host_opts_t ho;

    memset(&ho, 0, sizeof ho);
    ho.machine = CHF_MACHINE_CHANNELF;
    ho.bios_lo = g_lo;
    ho.bios_hi = g_hi;
    ho.bios_id = CHF_ROM_SL31253;
    ho.cart = g_app;
    ho.cart_size = APP_SIZE;
    ho.cart_type = CHF_CART_STD;
    ho.cart_app = true;
    ho.boip_hostport = boip && boip[0] ? boip : NULL;
    ho.cart_sync = true;            /* the transaction runs inline: deterministic */
    return ho;
}

/* Start the machine with the engine attached so it stops on its first
 * instruction. The host's thread takes the run lock as it starts and runs a
 * frame at once, so a hook that goes in after chf_host_start returns can be late
 * (the O2 lesson: never assume where the machine stopped). Holding the run lock
 * across the start and the attach makes the thread wait for the hook; the loop
 * only guards against a host that starts some other way. Returns 1 when the
 * machine stopped on its very first instruction. */
static int boot(chfdebug *d, const chf_host_opts_t *ho)
{
    for (int i = 0; i < 40; i++) {
        chfdebug_cpu c;
        int started;

        chf_host_lock();
        started = chf_host_start(ho) == 0;
        if (started)
            chfdebug_power_up(d);
        chf_host_unlock();
        if (!started) {
            printf("FAIL: the host does not start: %s\n", chf_host_last_error());
            failures++;
            return 0;
        }
        if (!wait_stopped(d, 1, 5000)) {
            printf("      boot %d: not stopped after 5 s (attached %d, running %d)\n", i, chfdebug_is_attached(d),
                   chf_host_is_running());
            return 0;
        }
        c = cpu(d);
        if (c.pc == 0 && c.instructions == 0) {
            if (i)
                printf("note: the hook was late %d time(s)\n", i);
            return 1;
        }
        if (i == 39)
            printf("      boot: last try stopped at pc %04X after %llu instructions\n", c.pc,
                   (unsigned long long)c.instructions);
        chfdebug_power_down(d);
        chf_host_stop();
        test_sleep_ms(5);
    }
    return 0;
}

/* ---- the tests ----------------------------------------------------------------- */

static void test_no_machine(chfdebug *d)
{
    chfdebug_cpu c;
    chfdebug_video v;
    chfdebug_io io;
    chfdebug_cart cart;
    chfdebug_vram_cell cell;
    uint8_t b[4];
    char out[8192];
    char name[64];
    int w = 0, h = 0, row = -1, col = -1, n;

    check(!chfdebug_is_attached(d) && !chfdebug_is_stopped(d), "a new engine is detached");
    c = cpu(d);
    check(c.pc == 0 && c.cycles == 0 && c.k_depth == -1 && c.illegal_pc == -1 && c.bios == -1,
          "no machine: a zero CPU view");
    check(chfdebug_read(d, CHF_SPACE_PROG, 0, b, 4) == 0, "no machine: no memory");
    n = 0;
    for (int s = 0; s <= CHF_SPACE_IO; s++)
        n += chfdebug_space_size(d, s);
    check(n == 0, "no machine: no space has a size");
    chfdebug_video_get(d, &v);
    check(v.line == 0 && v.lines == 0 && v.row_background[0] == 0, "no machine: a zero video view");
    check(chfdebug_video_view(d, CHFDEBUG_VIEW_FRAME, 0, 1, NULL, 0, &w, &h) == 1 && w == CHFSESSION_FB_WIDTH &&
          h == CHFSESSION_FB_HEIGHT, "the frame view's size needs no machine");
    check(chfdebug_video_view(d, CHFDEBUG_VIEW_VRAM, 0, 2, NULL, 0, &w, &h) == 1 && w == 512 && h == 512,
          "the VRAM view is 256 x 256, doubled");
    check(chfdebug_video_view(d, CHFDEBUG_VIEW_PALETTES, 0, 1, NULL, 0, &w, &h) == 1 && w > 0 && h > 0,
          "the palettes view has a size");
    check(chfdebug_video_view(d, CHFDEBUG_VIEW_LIVE, 0, 1, (uint32_t *)out, sizeof out / 4, &w, &h) == 0,
          "no machine: nothing drawn");
    n = chfdebug_video_hit(d, CHFDEBUG_VIEW_FRAME, 0, 0, 1, &row, &col);
    check(n == 4 * 128 + 4 && row == 4 && col == 4, "the frame's top left pixel is the cell at row 4, column 4");
    n = chfdebug_video_hit(d, CHFDEBUG_VIEW_FRAME, 203, 231, 1, &row, &col);
    check(n == 61 * 128 + 105 && row == 61 && col == 105, "and its bottom right is row 61, column 105");
    check(chfdebug_video_hit(d, CHFDEBUG_VIEW_FRAME, 204, 0, 1, &row, &col) == -1 &&
          chfdebug_video_hit(d, CHFDEBUG_VIEW_FRAME, -1, 0, 1, &row, &col) == -1, "off the picture is -1");
    n = chfdebug_video_hit(d, CHFDEBUG_VIEW_VRAM, 255, 255, 1, &row, &col);
    check(n == 63 * 128 + 127 && row == 63 && col == 127, "the VRAM view reaches row 63, column 127");
    n = chfdebug_video_hit(d, CHFDEBUG_VIEW_VALUES, 41 * 2 * 3 + 2, 10 * 4 * 3 + 5, 3, &row, &col);
    check(n == 10 * 128 + 41 && row == 10 && col == 41, "hit test at scale 3");
    check(chfdebug_video_hit(d, CHFDEBUG_VIEW_PALETTES, 5, 5, 1, &row, &col) == -1, "the palettes have no cells");
    check(chfdebug_vram_cell_get(d, 10, 20, &cell) == -1 && cell.last_pc == -1, "no machine: no cell");
    check(chfdebug_vram_writes(d, NULL, (chfdebug_vram_write *)out, 4) == 0, "no machine: no write log");
    chfdebug_io_get(d, &io);
    check(io.controller[0] == 0 && !io.has_2102, "no machine: a zero I/O view");
    chfdebug_cart_get(d, &cart);
    check(!cart.present && cart.type_name != NULL && cart.description != NULL, "no machine: no cartridge");
    check(chfdebug_cart_info(d, out, sizeof out) > 0 && strcmp(out, "No cartridge") == 0, "no machine: a cartridge line");
    chfdebug_command(d, "regs", out, sizeof out);
    check(out[0] != '\0', "no machine: the prompt still answers");
    chfdebug_command(d, "print $10 + o17", out, sizeof out);
    check(strncmp(out, "$1F", 3) == 0, "print takes $hex and octal");
    chfdebug_command(d, "print 800h + %1010", out, sizeof out);
    check(strncmp(out, "$080A", 5) == 0, "and 800h and %binary");

    /* the built-in labels */
    check(chfdebug_label_address(d, "RESET") == 0 && chfdebug_label_address(d, "CLRSCRN") == 0xD0 &&
          chfdebug_label_address(d, "PUSHK") == 0x107 && chfdebug_label_address(d, "POPK") == 0x11E,
          "the BIOS entry points");
    check(chfdebug_label_address(d, "CART_SIG") == 0x800 && chfdebug_label_address(d, "CART_ENTRY") == 0x802 &&
          chfdebug_label_address(d, "FUJI_CLAIM") == 0x47FC && chfdebug_label_address(d, "APPBANK_WIN") == 0x4800,
          "the cartridge's signature, entry, claim and APPBANK window");
    check(chfdebug_label_address(d, "VCART_RAM") == 0x2800 && chfdebug_label_address(d, "VCART_BANK") == 0x3000,
          "a Videocart's RAM and bank latch");
    check(chfdebug_label_address(d, "CART_RAM") == 0x8000 && chfdebug_label_address(d, "REPLY") == 0xF800 &&
          chfdebug_label_address(d, "ACKSEQ") == 0xFC00 && chfdebug_label_address(d, "STATUS") == 0xFC01 &&
          chfdebug_label_address(d, "ERR") == 0xFC02 && chfdebug_label_address(d, "REPLY_CMD") == 0xFC03 &&
          chfdebug_label_address(d, "RXLEN") == 0xFC04 && chfdebug_label_address(d, "BOOT_STATE") == 0xFC06 &&
          chfdebug_label_address(d, "BOOT_PCT") == 0xFC07 && chfdebug_label_address(d, "BOOT_ERR") == 0xFC08 &&
          chfdebug_label_address(d, "MAGIC") == 0xFC09 && chfdebug_label_address(d, "PROTO_VER") == 0xFC0B &&
          chfdebug_label_address(d, "TICK") == 0xFC0E, "the mailbox and its status page");
    check(chfdebug_label_address(d, "REGSEL") == 0xFD00 && chfdebug_label_address(d, "APPBANK_SEL") == 0xFD80 &&
          chfdebug_label_address(d, "SWAP") == 0xFDFE && chfdebug_label_address(d, "REGDATA") == 0xFE00 &&
          chfdebug_label_address(d, "TXDATA") == 0xFF00, "and its hotspots");
    check(chfdebug_label_address(d, "PANEL") == 0 && chfdebug_label_address(d, "RIGHT_C") == 1 &&
          chfdebug_label_address(d, "COLOR") == 1 && chfdebug_label_address(d, "LEFT_C") == 4 &&
          chfdebug_label_address(d, "COLUMN") == 4 && chfdebug_label_address(d, "ROW_SOUND") == 5 &&
          chfdebug_label_address(d, "RAM20") == 0x20 && chfdebug_label_address(d, "RAM21") == 0x21 &&
          chfdebug_label_address(d, "RAM24") == 0x24 && chfdebug_label_address(d, "RAM25") == 0x25,
          "the port names resolve");
    chfdebug_address_label(d, 0, name, sizeof name);
    check(strcmp(name, "RESET") == 0, "address $0000 is RESET, not the PANEL port");
    chfdebug_address_label(d, 5, name, sizeof name);
    check(name[0] == '\0', "and a port name never labels a program address");
    {
        int labelled = 0;
        for (int a = 0; a < 0x800; a++) {
            chfdebug_address_label(d, (uint16_t)a, name, sizeof name);
            if (name[0]) labelled++;
        }
        check(labelled == 4, "no label inside the BIOS but its four entry points");
    }
    check(chfdebug_label_address(d, "nonesuch") == -1, "an unknown label");
    chfdebug_command(d, "print ROW_SOUND + CART_ENTRY", out, sizeof out);
    check(strncmp(out, "$0807", 5) == 0, "labels and port names in an expression");
}


/* The first run of the program, breakpoint by breakpoint, in program order. */
static void test_program_run(chfdebug *d)
{
    char out[8192];
    chfdebug_cpu c, c2;
    chfdebug_breakpoint bps[16];
    int n, id, id2;

    /* the breakpoints set before the machine existed: an execute breakpoint on
     * the entry (with a condition on A: it is 0 on reset) and an OUT breakpoint */
    n = chfdebug_breakpoint_list(d, bps, 16);
    check(n == 2 && bps[0].type == CHFDEBUG_BP_EXEC && bps[0].start == A_ENTRY && bps[0].space == CHF_SPACE_PROG &&
          bps[1].type == CHFDEBUG_BP_OUT && bps[1].space == CHF_SPACE_IO && bps[1].start == 5,
          "the breakpoints set before the machine are listed with their space and range");
    check(run_until_stop(d, 5000), "run to the first breakpoint");
    c = cpu(d);
    check(c.pc == A_ENTRY && c.instructions == 1 && strcmp((why(d), reason), "breakpoint: exec $0802") == 0,
          "the entry: 'breakpoint: exec $0802', one instruction (the BIOS's jump) after reset");
    show("pc %04X instructions %llu reason '%s'", c.pc, (unsigned long long)c.instructions, reason);
    n = chfdebug_breakpoint_list(d, bps, 16);
    check(bps[0].hits == 1 && bps[1].hits == 0, "its hit was counted");
    chfdebug_breakpoint_clear(d);
    check(chfdebug_breakpoint_list(d, bps, 16) == 0, "cleared");

    /* access breakpoints on the arena's first byte, in program order:
     * ST at 0806 (value 0), LM at 080A, ST at 0810 (value 1) */
    id = chfdebug_breakpoint_add(d, CHFDEBUG_BP_EXEC, CHF_SPACE_PROG, 0x0806, 0x0806, "a == 0 && is == 0");
    id2 = chfdebug_breakpoint_add(d, CHFDEBUG_BP_READ, CHF_SPACE_PROG, 0x8000, 0x80ff, "value == 0 && addr == $8000");
    check(id > 0 && id2 > 0 && chfdebug_breakpoint_add(d, CHFDEBUG_BP_WRITE, CHF_SPACE_PROG, 0x8000, 0x8000, "value == 1") > 0,
          "an exec, a read and a write breakpoint, with conditions");
    check(run_until_stop(d, 5000) && cpu(d).pc == 0x0806, "stops at $0806");
    check(strcmp((why(d), reason), "breakpoint: exec $0806") == 0, "the exec breakpoint's reason");
    c = cpu(d);
    check(c.a == 0 && c.zero == 0 && c.dc0 == 0x8000 && c.dbus == 0x17, "A is 0 from LIS, DC0 $8000, the opcode ST fetched");
    check(run_until_stop(d, 5000) && cpu(d).pc == 0x080B, "the read stops after the LM, at $080B");
    check(strcmp((why(d), reason), "read prog $8000 = $00") == 0, "'read prog $8000 = $00'");
    show("reason '%s'", reason);
    check(run_until_stop(d, 5000) && cpu(d).pc == 0x0811, "the write stops after the ST, at $0811");
    check(strcmp((why(d), reason), "write prog $8000 = $01") == 0, "'write prog $8000 = $01'");
    show("reason '%s'", reason);
    n = chfdebug_breakpoint_list(d, bps, 16);
    check(n == 3 && bps[0].hits == 1 && bps[1].hits == 1 && bps[2].hits == 1, "each hit exactly once");
    check(chfdebug_breakpoint_check(d, 0x0806) && !chfdebug_breakpoint_check(d, 0x0807), "breakpoint_check is the exec map");
    chfdebug_breakpoint_clear(d);

    /* the arena really holds 1 now; both spaces agree */
    {
        uint8_t v1, v2;
        chfdebug_read(d, CHF_SPACE_PROG, 0x8000, &v1, 1);
        chfdebug_read(d, CHF_SPACE_CART, 0x10000, &v2, 1);
        check(v1 == 1 && v2 == 1, "the arena byte is 1 in PROG and in the CART space's arena");
    }

    /* VRAM breakpoints: a cell, then a rectangle with a condition on col */
    id = chfdebug_breakpoint_add(d, CHFDEBUG_BP_VRAM, CHF_SPACE_VRAM, 10 * 128 + 20, 10 * 128 + 20, "value == 2");
    check(id > 0, "a VRAM breakpoint on row 10, column 20");
    check(run_until_stop(d, 5000) && cpu(d).pc == 0x081D, "stops after the strobe, at $081D");
    check(strcmp((why(d), reason), "VRAM write row 10 col 20 = 2") == 0, "'VRAM write row 10 col 20 = 2'");
    show("reason '%s'", reason);
    chfdebug_breakpoint_remove(d, id);
    id = chfdebug_breakpoint_add(d, CHFDEBUG_BP_VRAM, CHF_SPACE_VRAM, 11 * 128 + 38, 13 * 128 + 44, "col == 40 && row == 12");
    check(id > 0, "a VRAM rectangle, rows 11-13, columns 38-44");
    check(run_until_stop(d, 5000) && cpu(d).pc == 0x0830, "stops after the OUT-form strobe, at $0830");
    check(strcmp((why(d), reason), "VRAM write row 12 col 40 = 1") == 0, "'VRAM write row 12 col 40 = 1'");
    chfdebug_breakpoint_remove(d, id);

    /* OUT and IN breakpoints */
    id = chfdebug_breakpoint_add(d, CHFDEBUG_BP_OUT, CHF_SPACE_IO, 5, 5, "value == $40 && port == 5");
    check(id > 0 && run_until_stop(d, 5000) && cpu(d).pc == 0x0837, "an OUT breakpoint stops after the OUTS 5 of $40");
    check(strcmp((why(d), reason), "out port 5 = $40") == 0, "'out port 5 = $40'");
    chfdebug_breakpoint_remove(d, id);
    id = chfdebug_breakpoint_add(d, CHFDEBUG_BP_IN, CHF_SPACE_IO, 0, 1, "");
    check(id > 0 && run_until_stop(d, 5000) && cpu(d).pc == 0x083B, "an IN breakpoint on ports 0-1 stops after INS 0");
    check(strncmp((why(d), reason), "in port 0 = $", 13) == 0, "'in port 0 = $..'");
    check(run_until_stop(d, 5000) && cpu(d).pc == 0x083D && strncmp((why(d), reason), "in port 1 = $", 13) == 0,
          "and after IN 1");
    chfdebug_breakpoint_remove(d, id);

    /* a scratchpad breakpoint: the idle loop's r20 */
    id = chfdebug_breakpoint_add(d, CHFDEBUG_BP_SCRATCH, CHF_SPACE_SCRATCH, 20, 20, "value == 2");
    check(id > 0, "a scratchpad breakpoint on r20");
    check(run_until_stop(d, 5000), "runs into the idle loop and stops");
    c = cpu(d);
    check(c.r[20] == 2 && c.pc == 0x0865 && strcmp((why(d), reason), "scratch write r20 = $02") == 0,
          "'scratch write r20 = $02', after the store");
    show("pc %04X r20 %02X reason '%s'", c.pc, c.r[20], reason);
    chfdebug_breakpoint_remove(d, id);
    (void)c2; (void)out;
}


/* What the program did to the ports, the sound and the VRAM, since attach. */
static void test_logs(chfdebug *d)
{
    static const struct { uint8_t port, value, wr; uint16_t pc; } exp[14] = {
        { 1, 0x7f, 1, 0x0813 }, { 4, 0xeb, 1, 0x0816 }, { 5, 0x35, 1, 0x0819 }, { 0, 0x60, 1, 0x081c },
        { 0, 0x50, 1, 0x081f }, { 1, 0xbf, 1, 0x0822 }, { 4, 0xd7, 1, 0x0826 }, { 5, 0x33, 1, 0x082a },
        { 0, 0x60, 1, 0x082e }, { 0, 0x50, 1, 0x0832 }, { 5, 0x40, 1, 0x0836 }, { 5, 0x00, 1, 0x0839 },
        { 0, 0x5f, 0, 0x083a }, { 1, 0xbf, 0, 0x083b },
    };
    chfdebug_io_event ev[64];
    chfdebug_sound_event se[8];
    chfdebug_vram_write w[16];
    chfdebug_vram_filter f;
    chfdebug_vram_cell cell;
    int n, ok = 1;

    n = chfdebug_io_events(d, ev, 64);
    check(n == 14, "the I/O log has the program's 12 port writes and 2 reads");
    show("%d events", n);
    for (int i = 0; i < n && i < 14; i++) {
        if (ev[i].port != exp[i].port || ev[i].value != exp[i].value || ev[i].is_write != exp[i].wr ||
            ev[i].pc != exp[i].pc || (i && ev[i].cycle <= ev[i - 1].cycle)) {
            ok = 0;
            printf("      event %d: port %u value %02X wr %d pc %04X cycle %llu\n", i, ev[i].port, ev[i].value,
                   ev[i].is_write, ev[i].pc, (unsigned long long)ev[i].cycle);
        }
    }
    check(ok, "each with its port, value, direction and the PC of the instruction (the two-byte OUT's own address)");
    n = chfdebug_io_events(d, ev, 3);
    check(n == 3 && ev[0].port == 5 && ev[0].value == 0x00 && ev[2].port == 1 && !ev[2].is_write,
          "a smaller buffer gets the newest, oldest first");

    n = chfdebug_sound_events(d, se, 8);
    check(n == 2 && se[0].old_mode == 0 && se[0].mode == 1 && se[0].pc == 0x0836 &&
          se[1].old_mode == 1 && se[1].mode == 0 && se[1].pc == 0x0839 && se[1].cycle > se[0].cycle,
          "the sound log: off to 1 kHz, and back");

    n = chfdebug_vram_writes(d, NULL, w, 16);
    check(n == 2 && w[0].row == 10 && w[0].col == 20 && w[0].value == 2 && w[0].old == 0 && w[0].pc == 0x081c &&
          w[1].row == 12 && w[1].col == 40 && w[1].value == 1 && w[1].old == 0 && w[1].pc == 0x082e &&
          w[1].cycle > w[0].cycle && w[0].frame == 0 && w[0].line >= 0 && w[0].line < 264,
          "the VRAM log has both pixels, oldest first, with the writing instruction's PC");
    show("n %d: (%u,%u)=%u pc %04X / (%u,%u)=%u pc %04X", n, w[0].row, w[0].col, w[0].value, w[0].pc,
         w[1].row, w[1].col, w[1].value, w[1].pc);
    check(chfdebug_vram_writes(d, NULL, w, 1) == 1 && w[0].row == 12, "a short buffer keeps the newest");
    memset(&f, 0, sizeof f);
    f.row0 = 12; f.row1 = 12; f.col0 = 0; f.col1 = 127;
    check(chfdebug_vram_writes(d, &f, w, 16) == 1 && w[0].col == 40, "filtered by row");
    f.row0 = 0; f.row1 = 63; f.col0 = 0; f.col1 = 30;
    check(chfdebug_vram_writes(d, &f, w, 16) == 1 && w[0].col == 20, "filtered by column");
    f.col1 = 127; f.changes_only = 1;
    check(chfdebug_vram_writes(d, &f, w, 16) == 2, "changes only: both changed their cell");
    f.changes_only = 0; f.this_frame = 1;
    check(chfdebug_vram_writes(d, &f, w, 16) == 2, "this frame: no frame has ended yet");
    f.this_frame = 0; f.palette_cols = 1;
    check(chfdebug_vram_writes(d, &f, w, 16) == 0, "palette columns: none written");

    check(chfdebug_vram_cell_get(d, 10, 20, &cell) == 0 && cell.value == 2 && cell.writes == 1 && cell.last_pc == 0x081c &&
          cell.last_old == 0 && cell.visible && cell.hidden_why == NULL && cell.palette == 0 &&
          cell.pen == 1 && cell.rgb == chfdebug_colour_rgb(1) && cell.last_frame == 0,
          "cell (10, 20): value 2, written once at $081C, visible, white in palette 0");
    check(chfdebug_vram_cell_get(d, 12, 40, &cell) == 0 && cell.value == 1 && cell.last_pc == 0x082e,
          "cell (12, 40) was written by the two-byte OUT at $082E");
    check(chfdebug_vram_cell_get(d, 20, 60, &cell) == 0 && cell.writes == 0 && cell.last_pc == -1 && cell.value == 0,
          "a cell never written: no writer");
    check(chfdebug_vram_cell_get(d, 3, 50, &cell) == 0 && !cell.visible && strcmp(cell.hidden_why, "top border") == 0 &&
          chfdebug_vram_cell_get(d, 62, 50, &cell) == 0 && strcmp(cell.hidden_why, "bottom border") == 0 &&
          chfdebug_vram_cell_get(d, 10, 3, &cell) == 0 && strcmp(cell.hidden_why, "left border") == 0 &&
          chfdebug_vram_cell_get(d, 10, 110, &cell) == 0 && strcmp(cell.hidden_why, "right border") == 0 &&
          chfdebug_vram_cell_get(d, 10, 125, &cell) == 0 && strcmp(cell.hidden_why, "palette column") == 0 &&
          chfdebug_vram_cell_get(d, 10, 126, &cell) == 0 && strcmp(cell.hidden_why, "palette column") == 0,
          "hidden cells say why");
    check(chfdebug_vram_cell_get(d, 64, 0, &cell) == -1 && chfdebug_vram_cell_get(d, 0, 128, &cell) == -1,
          "outside the VRAM is -1");

    chfdebug_vram_writes_clear(d);
    chfdebug_io_events_clear(d);
    check(chfdebug_vram_writes(d, NULL, w, 16) == 0 && chfdebug_io_events(d, ev, 4) == 0, "the logs clear");
    check(chfdebug_vram_cell_get(d, 10, 20, &cell) == 0 && cell.writes == 1, "but a cell remembers its writes");
}

/* The video registers, the six views, the overlays and the hit test. */
static void test_video(chfdebug *d)
{
    static uint32_t px[CHFDEBUG_VIEW_MAX_PIXELS * 9], live[CHFDEBUG_VIEW_MAX_PIXELS], beam[CHFDEBUG_VIEW_MAX_PIXELS];
    chfdebug_video v;
    chfdebug_cpu c, c2;
    int w = 0, h = 0, row, col, n, s;

    /* the ports, as the program left them */
    chfdebug_video_get(d, &v);
    check(v.port0 == 0x50 && v.port1 == 0xbf && v.port4 == 0xd7 && v.port5 == 0x00, "the four latches as last written");
    check(v.value == 1 && v.col == 40 && v.row == 63 && v.sound_mode == 0 && !v.strobe && !v.controllers_enabled &&
          v.panel_latch == 0, "decoded: value 1, column 40, row 63, sound off, strobe low, controllers gated off");
    check(v.in0 == 0x5f && v.in1 == 0xbf && v.in4 == 0xd7 && v.in5 == 0x00,
          "and what the CPU would read now (~external | latch)");
    check(v.lines == 264 && !v.is_pal && v.cycles_per_line > 100 && v.cycles_per_line < 130, "NTSC: 264 lines");
    check(v.palette_rows[0] == 64 && v.row_palette[10] == 0 && v.row_background[10] == chfdebug_colour_rgb(0),
          "all 64 rows use palette 0, on a black background");
    check(v.strobes_frame == 2 && v.strobes_last == 0, "two VRAM strobes this frame");

    /* the first frame completes */
    c = cpu(d);
    chfdebug_frame(d, 1);
    check(wait_stopped(d, 1, 3000) && cpu(d).frame == c.frame + 1 && strcmp((why(d), reason), "frame") == 0,
          "frame+1");
    chfdebug_video_get(d, &v);
    check(v.strobes_last == 2 && v.changes_last == 2 && v.strobes_frame == 0, "the frame's strobes were counted at the blank");

    /* sizes, and the live picture */
    check(chfdebug_video_view(d, CHFDEBUG_VIEW_FRAME, 0, 1, px, CHFDEBUG_VIEW_MAX_PIXELS, &w, &h) == 1 &&
          w == 204 && h == 232, "the frame is 204 x 232");
    s = (10 - 4) * 4 * 204 + (20 - 4) * 2;
    check(px[s] == 0xfdfdfd && px[s + 1] == 0xfdfdfd && px[s + 3 * 204] == 0xfdfdfd && px[0] == 0x101010,
          "cell (10, 20) is white on the black the BIOS-less background shows");
    show("%06X %06X", px[s], px[0]);
    check(chfdebug_video_view(d, CHFDEBUG_VIEW_FRAME, 0, 3, px, CHFDEBUG_VIEW_MAX_PIXELS * 9, &w, &h) == 1 &&
          w == 612 && h == 696 && px[(24 * 3 + 2) * 612 + 32 * 3 + 4] == 0xfdfdfd, "scale 3");
    check(chfdebug_video_view(d, CHFDEBUG_VIEW_FRAME, 0, 1, px, 100, &w, &h) == 0, "a buffer too small draws nothing");
    check(chfdebug_video_view(d, CHFDEBUG_VIEW_LIVE, 0, 1, live, CHFDEBUG_VIEW_MAX_PIXELS, &w, &h) == 1 && w == 204 &&
          live[s] == 0xfdfdfd, "the live view");
    check(chfdebug_video_view(d, CHFDEBUG_VIEW_VRAM, 0, 1, px, CHFDEBUG_VIEW_MAX_PIXELS, &w, &h) == 1 && w == 256 && h == 256,
          "the VRAM view is 256 x 256");
    check(px[(10 * 4) * 256 + 20 * 2] == 0xfdfdfd && px[(12 * 4 + 3) * 256 + 40 * 2 + 1] == 0xfdfdfd,
          "both pixels are in it, 2 x 4 a cell");
    check(px[(3 * 4) * 256 + 3 * 2] != 0x101010 && px[(30 * 4) * 256 + 60 * 2] == 0x101010,
          "a hidden cell is hatched, a visible one is not");
    check(chfdebug_video_view(d, CHFDEBUG_VIEW_VALUES, 0, 1, px, CHFDEBUG_VIEW_MAX_PIXELS, &w, &h) == 1 &&
          px[(10 * 4) * 256 + 20 * 2] == 0xe03030 && px[(12 * 4) * 256 + 40 * 2] == 0x3a6df0 &&
          px[(30 * 4) * 256 + 60 * 2] == 0x303030, "the values view: 2 red, 1 blue, 0 dark grey");
    check(chfdebug_video_view(d, CHFDEBUG_VIEW_PALETTES, 0, 1, px, CHFDEBUG_VIEW_MAX_PIXELS, &w, &h) == 1 && w == 256 &&
          h == 128 && px[5] == 0x101010 && px[32 * 256 + 5] == 0xced0ff && px[(3 * 32 + 5) * 256 + 3 * 64 + 5] == 0x02cc5d &&
          px[(2 * 32 + 5) * 256 + 64 + 5] == 0x4b3ff3, "the palettes view: black, light blue, green, blue");

    /* overlays */
    chfdebug_video_view(d, CHFDEBUG_VIEW_VRAM, CHFDEBUG_F_VISIBLE, 1, px, CHFDEBUG_VIEW_MAX_PIXELS, &w, &h);
    check(px[20 * 256 + 4 * 2 - 1] == CHFSESSION_ACCENT_RGB && px[(4 * 4 - 1) * 256 + 100] == CHFSESSION_ACCENT_RGB &&
          px[20 * 256 + 106 * 2] == CHFSESSION_ACCENT_RGB && px[62 * 4 * 256 + 100] == CHFSESSION_ACCENT_RGB,
          "the visible area's outline, just outside the cells");
    chfdebug_video_view(d, CHFDEBUG_VIEW_VRAM, CHFDEBUG_F_PALCOLS, 1, px, CHFDEBUG_VIEW_MAX_PIXELS, &w, &h);
    check(px[50 * 256 + 125 * 2 - 1] == 0x00e5ff && px[50 * 256 + 127 * 2 - 1] == 0x00e5ff,
          "the palette columns are bracketed");
    chfdebug_video_view(d, CHFDEBUG_VIEW_VRAM, CHFDEBUG_F_CURSOR, 1, px, CHFDEBUG_VIEW_MAX_PIXELS, &w, &h);
    check(px[(63 * 4) * 256 + 40 * 2] == 0x00e5ff && px[(63 * 4 + 3) * 256 + 40 * 2 + 1] == 0x00e5ff,
          "the cursor is the cell the next strobe writes (row 63, column 40)");
    chfdebug_video_view(d, CHFDEBUG_VIEW_VRAM, CHFDEBUG_F_GRID, 1, px, CHFDEBUG_VIEW_MAX_PIXELS, &w, &h);
    check(px[(10 * 4 + 1) * 256 + 20 * 2] == 0xfdfdfd, "the grid needs scale 2");
    chfdebug_video_view(d, CHFDEBUG_VIEW_VRAM, CHFDEBUG_F_GRID, 2, px, CHFDEBUG_VIEW_MAX_PIXELS * 4, &w, &h);
    check(px[(10 * 4 * 2 + 3) * 512 + 20 * 4] != 0xfdfdfd && px[(10 * 4 * 2 + 3) * 512 + 20 * 4 + 2] == 0xfdfdfd,
          "at scale 2 the cell borders show");
    chfdebug_video_view(d, CHFDEBUG_VIEW_FRAME, CHFDEBUG_F_HEAT, 1, px, CHFDEBUG_VIEW_MAX_PIXELS, &w, &h);
    check(px[s] != 0xfdfdfd && px[0] == 0x101010, "heat tints the frame's cells written last frame");
    chfdebug_video_view(d, CHFDEBUG_VIEW_FRAME, CHFDEBUG_F_DIRTY, 1, px, CHFDEBUG_VIEW_MAX_PIXELS, &w, &h);
    check(px[s] != 0xfdfdfd && px[(12 - 4) * 4 * 204 + (40 - 4) * 2] != 0xfdfdfd && px[0] == 0x101010,
          "dirty cells: the two that changed since the frame before");
    chfdebug_video_view(d, CHFDEBUG_VIEW_LIVE, CHFDEBUG_F_DIRTY, 1, px, CHFDEBUG_VIEW_MAX_PIXELS, &w, &h);
    check(px[s] == 0xfdfdfd, "and none in the live view (nothing changed since the blank)");

    /* the beam: cells edited behind the beam show, ahead of it they do not */
    chfdebug_write(d, CHF_SPACE_VRAM, 30 * 128 + 50, 2);
    chfdebug_write(d, CHF_SPACE_VRAM, 40 * 128 + 50, 2);
    c = cpu(d);
    n = (130 - c.vpos + 264) % 264;
    chfdebug_scanline(d, n ? n : 264);
    check(wait_stopped(d, 1, 3000), "scanline steps to the middle of the picture");
    c2 = cpu(d);
    check(c2.vpos == 130 && c2.frame == c.frame, "line 130 of the same frame");
    show("vpos %d (from %d, n %d)", c2.vpos, c.vpos, n);
    chfdebug_video_view(d, CHFDEBUG_VIEW_LIVE, 0, 1, live, CHFDEBUG_VIEW_MAX_PIXELS, &w, &h);
    chfdebug_video_view(d, CHFDEBUG_VIEW_BEAM, 0, 1, beam, CHFDEBUG_VIEW_MAX_PIXELS, &w, &h);
    s = (30 - 4) * 4 * 204 + (50 - 4) * 2;
    n = (40 - 4) * 4 * 204 + (50 - 4) * 2;
    check(live[s] == 0xfdfdfd && live[n] == 0xfdfdfd && beam[s] == 0xfdfdfd && beam[n] != 0xfdfdfd && beam[n] <= 0x101010,
          "the beam view shows row 30 (passed) lit and row 40 (not yet) as last frame's, dimmed");
    show("beam %06X %06X live %06X %06X", beam[s], beam[n], live[s], live[n]);
    check(beam[(10 - 4) * 4 * 204 + (20 - 4) * 2] == 0xfdfdfd && beam[0] == 0x101010, "rows already passed are drawn undimmed");
    check(beam[(50 - 4) * 4 * 204 + 5] != 0x101010 && live[(50 - 4) * 4 * 204 + 5] == 0x101010, "rows ahead of the beam are dimmed");
    chfdebug_video_view(d, CHFDEBUG_VIEW_BEAM, CHFDEBUG_F_BEAM, 1, px, CHFDEBUG_VIEW_MAX_PIXELS, &w, &h);
    check(px[(130 - 16) * 204 + 3] == 0xff2020 && px[(129 - 16) * 204 + 3] != 0xff2020, "the beam line is drawn at line 130");
    chfdebug_write(d, CHF_SPACE_VRAM, 30 * 128 + 50, 0);
    chfdebug_write(d, CHF_SPACE_VRAM, 40 * 128 + 50, 0);

    /* palettes: a row's c125/c126 */
    chfdebug_write(d, CHF_SPACE_VRAM, 10 * 128 + 125, 2);          /* palette (0 & 2) | (2 >> 1) = 1 */
    chfdebug_write(d, CHF_SPACE_VRAM, 11 * 128 + 126, 2);          /* palette 2 */
    chfdebug_write(d, CHF_SPACE_VRAM, 12 * 128 + 125, 2);
    chfdebug_write(d, CHF_SPACE_VRAM, 12 * 128 + 126, 2);          /* palette 3 */
    chfdebug_video_get(d, &v);
    check(v.row_palette[10] == 1 && v.row_palette[11] == 2 && v.row_palette[12] == 3 && v.palette_rows[0] == 61 &&
          v.palette_rows[1] == 1 && v.row_background[10] == 0xced0ff && v.row_background[11] == 0xe0e0e0 &&
          v.row_background[12] == 0x91ffa6, "rows 10-12 take palettes 1-3: light blue, light grey, light green");
    chfdebug_video_view(d, CHFDEBUG_VIEW_LIVE, 0, 1, px, CHFDEBUG_VIEW_MAX_PIXELS, &w, &h);
    check(px[(10 - 4) * 4 * 204 + 5] == 0xced0ff && px[(10 - 4) * 4 * 204 + (20 - 4) * 2] == 0xff3153 &&
          px[(11 - 4) * 4 * 204 + 5] == 0xe0e0e0 && px[(12 - 4) * 4 * 204 + (40 - 4) * 2] == 0x4b3ff3,
          "the picture follows: background, and value 2 is red in palette 1, value 1 blue in palette 3");
    chfdebug_video_view(d, CHFDEBUG_VIEW_LIVE, CHFDEBUG_F_PALROWS, 1, px, CHFDEBUG_VIEW_MAX_PIXELS, &w, &h);
    check(px[(10 - 4) * 4 * 204 + 1] == CHFSESSION_ACCENT_RGB && px[(13 - 4) * 4 * 204 + 1] != CHFSESSION_ACCENT_RGB &&
          px[(11 - 4) * 4 * 204 + 3 * 204 + 0] == CHFSESSION_ACCENT_RGB, "a tick marks the rows whose palette changed");
    for (int r = 10; r <= 12; r++) {
        chfdebug_write(d, CHF_SPACE_VRAM, (uint32_t)(r * 128 + 125), 0);
        chfdebug_write(d, CHF_SPACE_VRAM, (uint32_t)(r * 128 + 126), 0);
    }
    chfdebug_video_get(d, &v);
    check(v.palette_rows[0] == 64, "back to palette 0");

    /* the hit test agrees with the drawing */
    check(chfdebug_video_hit(d, CHFDEBUG_VIEW_FRAME, 32, 24, 1, &row, &col) == 10 * 128 + 20 && row == 10 && col == 20 &&
          chfdebug_video_hit(d, CHFDEBUG_VIEW_VRAM, 40, 40, 1, &row, &col) == 10 * 128 + 20 &&
          chfdebug_video_hit(d, CHFDEBUG_VIEW_BEAM, 31 * 2, 24 * 2, 2, &row, &col) == 10 * 128 + 19, "pixels to cells");
    for (int view = CHFDEBUG_VIEW_FRAME; view <= CHFDEBUG_VIEW_VALUES; view++) {
        int bad = 0;
        for (int x = 0; x < 256; x += 7)
            for (int y = 0; y < 256; y += 5)
                if (chfdebug_video_hit(d, view, x * 2, y * 2, 2, &row, &col) >= 0) {
                    int x0 = (view <= CHFDEBUG_VIEW_BEAM) ? (col - 4) * 4 : col * 4;
                    int y0 = (view <= CHFDEBUG_VIEW_BEAM) ? (row - 4) * 8 : row * 8;
                    if (x * 2 < x0 || x * 2 >= x0 + 4 || y * 2 < y0 || y * 2 >= y0 + 8) bad++;
                }
        check(!bad, "every hit lies inside its cell's rectangle");
    }
    chfdebug_cpu_set(d, CHF_REG_PC, A_IDLE);
}

/* The CPU view, its edits (a PC change refetches the opcode), the K stack. */
static void test_cpu(chfdebug *d)
{
    char out[4096];
    chfdebug_cpu c, c2, saved;
    int i;

    chfdebug_cpu_set(d, CHF_REG_PC, A_IDLE);
    c = cpu(d);
    check(c.pc == A_IDLE && c.pc0 == A_IDLE + 1 && c.dbus == 0x62, "the CPU is on an opcode it has already fetched: PC0 = PC + 1, dbus = LISU 2");
    check(c.isar == 20 && c.r[20] >= 2 && c.machine == CHF_MACHINE_CHANNELF && c.bios == CHF_ROM_SL31253,
          "ISAR o24 = r20 (the idle loop's counter), the machine and the BIOS");
    check(c.j_h_k_q[2] == 0x0950 && c.r[12] == 0x09 && c.r[13] == 0x50, "K = $0950 (loaded for the PK)");
    check(c.pc1 == 0x0847, "PC1 is the PK's return address");
    check(c.cycles > 0 && c.instructions > 40 && c.illegal_count == 0 && c.illegal_pc == -1, "time, and no illegal opcode");
    check(c.vpos >= 0 && c.vpos < 264 && c.hpos >= 0 && c.hpos < 128, "the beam is in the NTSC raster");
    check(c.sign == (c.w & 1) && c.carry == ((c.w >> 1) & 1) && c.zero == ((c.w >> 2) & 1) &&
          c.overflow == ((c.w >> 3) & 1) && c.icb == ((c.w >> 4) & 1), "W and its decode agree");

    saved = c;
    chfdebug_cpu_set(d, CHF_REG_A, 0x5a);
    chfdebug_cpu_set(d, CHF_REG_W, 0x1f);
    chfdebug_cpu_set(d, CHF_REG_ISAR, 075);
    chfdebug_cpu_set(d, CHF_REG_PC1, 0x1234);
    chfdebug_cpu_set(d, CHF_REG_DC0, 0xabcd);
    chfdebug_cpu_set(d, CHF_REG_DC1, 0x4321);
    chfdebug_cpu_set(d, CHF_REG_J, 0x77);
    chfdebug_cpu_set(d, CHF_REG_H, 0x1122);
    chfdebug_cpu_set(d, CHF_REG_K, 0x3344);
    chfdebug_cpu_set(d, CHF_REG_Q, 0x5566);
    chfdebug_cpu_set(d, CHF_REG_R0 + 33, 0xc3);
    c2 = cpu(d);
    check(c2.a == 0x5a && c2.w == 0x1f && c2.sign && c2.carry && c2.zero && c2.overflow && c2.icb && c2.isar == 075 &&
          c2.pc1 == 0x1234 && c2.dc0 == 0xabcd && c2.dc1 == 0x4321, "A, W, ISAR, PC1, DC0 and DC1 take edits");
    check(c2.j_h_k_q[0] == 0x77 && c2.j_h_k_q[1] == 0x1122 && c2.j_h_k_q[2] == 0x3344 && c2.j_h_k_q[3] == 0x5566 &&
          c2.r[9] == 0x77 && c2.r[10] == 0x11 && c2.r[11] == 0x22 && c2.r[12] == 0x33 && c2.r[13] == 0x44 &&
          c2.r[14] == 0x55 && c2.r[15] == 0x66 && c2.r[33] == 0xc3, "J, H, K, Q and a scratchpad register");
    chfdebug_cpu_set(d, CHF_REG_W, 0);
    chfdebug_cpu_set(d, CHF_REG_SIGN, 1);
    chfdebug_cpu_set(d, CHF_REG_CARRY, 1);
    c2 = cpu(d);
    check(c2.w == 0x03 && c2.sign && c2.carry && !c2.zero, "the flags set one by one (W = S|C)");
    chfdebug_cpu_set(d, CHF_REG_ZERO, 1);
    chfdebug_cpu_set(d, CHF_REG_OVERFLOW, 1);
    chfdebug_cpu_set(d, CHF_REG_ICB, 1);
    chfdebug_cpu_set(d, CHF_REG_SIGN, 0);
    check(cpu(d).w == 0x1e, "and Z, O, ICB; S cleared");
    chfdebug_cpu_set(d, CHF_REG_W, 0xff);
    check(cpu(d).w == 0x1f, "W keeps its five bits");
    chfdebug_cpu_set(d, CHF_REG_ISAR, 0xff);
    check(cpu(d).isar == 63, "and ISAR its six");

    /* a PC change refetches the opcode there */
    chfdebug_cpu_set(d, CHF_REG_PC, A_ENTRY);
    c2 = cpu(d);
    check(c2.pc == A_ENTRY && c2.pc0 == A_ENTRY + 1 && c2.dbus == 0x2a, "PC = $0802: dbus is DCI, PC0 one past");
    chfdebug_step(d);
    check(wait_stopped(d, 1, 3000), "a step");
    c2 = cpu(d);
    check(c2.pc == 0x0805 && c2.dc0 == 0x8000 && c2.dbus == 0x70, "ran the DCI that was refetched, and sits at the LIS");
    chfdebug_cpu_set(d, CHF_REG_PC0, A_IDLE + 1);
    c2 = cpu(d);
    check(c2.pc == A_IDLE && c2.dbus == 0x62, "setting PC0 = PC + 1 puts the PC one back, opcode refetched");
    chfdebug_cpu_set(d, CHF_REG_PC, 0x0000);
    check(cpu(d).dbus == 0x29 && cpu(d).pc0 == 1, "PC = 0 is the BIOS's jump");

    /* the K stack: r59 and the entries from r40 up */
    check(cpu(d).k_depth == -1, "r59 = 0 is not a K-stack pointer");
    chfdebug_write(d, CHF_SPACE_SCRATCH, 59, 44);
    for (i = 0; i < 4; i++)
        chfdebug_write(d, CHF_SPACE_SCRATCH, (uint32_t)(40 + i), (uint8_t)(i % 2 ? 0x34 + i : 0x12 + i));
    c2 = cpu(d);
    check(c2.k_depth == 2 && c2.k_stack[0] == 0x1235 && c2.k_stack[1] == 0x1437,
          "r59 = 44: depth 2, with the two saved returns, oldest first");
    chfdebug_write(d, CHF_SPACE_SCRATCH, 59, 40);
    check(cpu(d).k_depth == 0, "r59 = 40: empty");
    chfdebug_write(d, CHF_SPACE_SCRATCH, 59, 58);
    check(cpu(d).k_depth == 9, "r59 = 58: nine");
    chfdebug_write(d, CHF_SPACE_SCRATCH, 59, 45);
    check(cpu(d).k_depth == -1, "odd: not a pointer");
    chfdebug_write(d, CHF_SPACE_SCRATCH, 59, 60);
    check(cpu(d).k_depth == -1, "past 58: not a pointer");
    chfdebug_write(d, CHF_SPACE_SCRATCH, 59, 38);
    check(cpu(d).k_depth == -1, "below 40: not a pointer");
    chfdebug_write(d, CHF_SPACE_SCRATCH, 59, 42);
    chfdebug_command(d, "kstack", out, sizeof out);
    check(strstr(out, "depth 1") && strstr(out, "1235"), "the prompt's kstack");
    for (i = 0; i < 4; i++)
        chfdebug_write(d, CHF_SPACE_SCRATCH, (uint32_t)(40 + i), 0);
    chfdebug_write(d, CHF_SPACE_SCRATCH, 59, 0);

    /* an illegal opcode, run from the cartridge's RAM */
    chfdebug_write(d, CHF_SPACE_PROG, 0x8010, 0x2d);
    chfdebug_cpu_set(d, CHF_REG_PC, 0x8010);
    check(cpu(d).dbus == 0x2d, "an opcode fetched from the arena");
    chfdebug_step(d);
    check(wait_stopped(d, 1, 3000) && cpu(d).illegal_count == 1 && cpu(d).illegal_pc == 0x8010, "an illegal opcode is counted, with its address");

    /* put it back */
    chfdebug_cpu_set(d, CHF_REG_A, saved.a);
    chfdebug_cpu_set(d, CHF_REG_W, saved.w);
    chfdebug_cpu_set(d, CHF_REG_ISAR, saved.isar);
    chfdebug_cpu_set(d, CHF_REG_PC1, saved.pc1);
    chfdebug_cpu_set(d, CHF_REG_DC0, saved.dc0);
    chfdebug_cpu_set(d, CHF_REG_DC1, saved.dc1);
    for (i = 9; i < 16; i++)
        chfdebug_cpu_set(d, CHF_REG_R0 + i, saved.r[i]);
    chfdebug_cpu_set(d, CHF_REG_R0 + 33, saved.r[33]);
    chfdebug_cpu_set(d, CHF_REG_PC, A_IDLE);
    c2 = cpu(d);
    check(c2.pc == saved.pc && c2.a == saved.a && c2.w == saved.w && c2.isar == saved.isar && c2.pc1 == saved.pc1 &&
          c2.dc0 == saved.dc0 && c2.dc1 == saved.dc1 && memcmp(c2.r, saved.r, sizeof c2.r) == 0, "the CPU is as it was");
}

static void test_disasm(chfdebug *d)
{
    chfdebug_line l[40];
    char name[64];
    int n, pcl = -1, i;

    n = chfdebug_disassemble(d, 0, l, 3, &pcl);
    check(n == 3 && l[0].address == 0 && l[0].length == 3 && strcmp(l[0].bytes, "29 08 02") == 0 &&
          strncmp(l[0].disasm, "JMP", 3) == 0 && strstr(l[0].disasm, "0802") && strcmp(l[0].label, "RESET") == 0,
          "the BIOS's first instruction: JMP $0802, labelled RESET");
    show("'%s' '%s' '%s'", l[0].bytes, l[0].disasm, l[0].label);
    check(strcmp(l[0].comment, "CART_ENTRY") == 0, "its comment is the target's label");
    n = chfdebug_disassemble(d, A_ENTRY, l, 12, &pcl);
    check(n == 12 && strcmp(l[0].label, "CART_ENTRY") == 0 && strncmp(l[0].disasm, "DCI", 3) == 0 && l[0].length == 3 &&
          strcmp(l[0].bytes, "2A 80 00") == 0, "the entry point: DCI $8000");
    for (i = 1, name[0] = 0; i < n; i++)
        if (l[i].address != l[i - 1].address + l[i - 1].length)
            name[0] = 1;
    check(!name[0], "consecutive lines abut, by each instruction's length");
    n = chfdebug_disassemble(d, 0x0811, l, 14, NULL);
    i = find_line(l, n, "OUTS");
    check(i >= 0 && l[i].address == 0x0813 && strcmp(l[i].comment, "port RIGHT_C") == 0, "OUTS 1 names its port");
    n = chfdebug_disassemble(d, 0x0820, l, 12, NULL);
    i = find_line(l, n, "OUT ");
    check(i >= 0 && l[i].address == 0x0822 && l[i].length == 2 && strcmp(l[i].bytes, "27 01") == 0 &&
          strcmp(l[i].comment, "port RIGHT_C") == 0, "and the two-byte OUT");
    n = chfdebug_disassemble(d, 0x0836, l, 5, NULL);
    check(strcmp(l[0].comment, "port ROW_SOUND") == 0, "OUTS 5 is ROW_SOUND");
    n = chfdebug_disassemble(d, 0x083d, l, 1, NULL);
    check(n == 1 && strncmp(l[0].disasm, "PI", 2) == 0 && l[0].comment[0] == '\0', "a PI to an unlabelled address has no comment");
    chfdebug_set_label(d, 0x0900, "sub1");
    chfdebug_set_label(d, A_IDLE, "idle");
    chfdebug_disassemble(d, 0x083d, l, 1, NULL);
    check(strcmp(l[0].comment, "sub1") == 0, "and one with a label has it");
    n = chfdebug_disassemble(d, A_IDLE, l, 8, &pcl);
    check(strcmp(l[0].label, "idle") == 0 && find_line(l, n, "BR") >= 0 && strcmp(l[find_line(l, n, "BR")].comment, "idle") == 0,
          "a label shows on its line and as a branch's comment");
    check(chfdebug_label_address(d, "IDLE") == A_IDLE && chfdebug_label_address(d, "sub1") == 0x900, "labels are case-insensitive");
    chfdebug_address_label(d, 0x0900, name, sizeof name);
    check(strcmp(name, "sub1") == 0, "address to label");

    /* the PC's line */
    chfdebug_cpu_set(d, CHF_REG_PC, 0x0863);
    n = chfdebug_disassemble(d, A_IDLE, l, 6, &pcl);
    check(pcl == 3 && l[3].is_pc && l[3].address == 0x0863 && !l[0].is_pc, "disassembly marks the PC's line");
    n = chfdebug_disassemble(d, 0x0800, l, 4, &pcl);
    check(pcl == -1, "and says -1 when the PC is not among the lines");
    chfdebug_cpu_set(d, CHF_REG_PC, A_IDLE);

    /* breakpoint marks */
    check(chfdebug_breakpoint_toggle(d, 0x0863) == 1 && chfdebug_breakpoint_check(d, 0x0863), "a toggled execute breakpoint is set");
    chfdebug_disassemble(d, A_IDLE, l, 6, NULL);
    check(l[3].has_breakpoint && !l[2].has_breakpoint, "and marked on its line");
    check(chfdebug_breakpoint_toggle(d, 0x0863) == 0 && !chfdebug_breakpoint_check(d, 0x0863), "toggled again it is gone");

    /* the APPBANK window shows a page, and says which */
    {
        chfdebug_cart cart;
        chfdebug_cart_get(d, &cart);
        n = chfdebug_disassemble(d, 0x4800, l, 3, NULL);
        check(n == 3 && cart.npages == 2 && strstr(l[0].comment, "APPBANK page") != NULL && l[0].address == 0x4800,
              "$4800 is tagged with the APPBANK page");
        show("page %d comment '%s' disasm '%s'", cart.page, l[0].comment, l[0].disasm);
        n = chfdebug_disassemble(d, 0x47fd, l, 5, NULL);
        check(l[0].comment[0] == '\0' || strstr(l[0].comment, "APPBANK") == NULL, "the fixed window below it is not");
    }

    /* scrolling: row_address forwards and back */
    check(chfdebug_row_address(d, 0x0802, 0) == 0x0802, "no rows is the same address");
    chfdebug_disassemble(d, 0x0802, l, 4, NULL);
    check(chfdebug_row_address(d, 0x0802, 3) == l[3].address && chfdebug_row_address(d, 0x0802, 4) == l[3].address + l[3].length,
          "rows forward");
    check(chfdebug_row_address(d, (uint16_t)(l[3].address), -3) == 0x0802, "and rows back, landing on an instruction boundary");
    check(chfdebug_row_address(d, 0x0811, -2) >= 0x0800, "backwards past the top stays sane");
}

/* The five spaces. */
static void test_memory(chfdebug *d)
{
    uint8_t b[16], v, w;
    chfdebug_video vid;
    chfdebug_cart cart;
    chfdebug_io io;
    chfdebug_cart_event cev[64];
    int before, i;

    check(chfdebug_space_size(d, CHF_SPACE_PROG) == 0x10000 && chfdebug_space_size(d, CHF_SPACE_SCRATCH) == 64 &&
          chfdebug_space_size(d, CHF_SPACE_VRAM) == 8192 && chfdebug_space_size(d, CHF_SPACE_CART) == 0x18000 &&
          chfdebug_space_size(d, CHF_SPACE_IO) == 256, "the five spaces' sizes (the FujiNet cartridge: 64K image + 32K arena)");

    /* PROG */
    check(chfdebug_read(d, CHF_SPACE_PROG, 0x0800, b, 4) == 4 && b[0] == 0x55 && b[2] == 0x2a && b[3] == 0x80, "PROG $0800 is the cartridge's $55 and its entry");
    check(chfdebug_read(d, CHF_SPACE_PROG, 0x0000, b, 3) == 3 && b[0] == 0x29 && b[1] == 0x08 && b[2] == 0x02, "the BIOS at $0000");
    check(chfdebug_read(d, CHF_SPACE_PROG, FN_ROM_CLAIM + 0x800, b, 4) == 4 && memcmp(b, "FUJI", 4) == 0, "the claim at $47FC");
    check(chfdebug_read(d, CHF_SPACE_PROG, 0xfc09, b, 3) == 3 && b[0] == 'F' && b[1] == 'N' && b[2] == FN_PROTO_VER,
          "the status page reads 'FN' and the protocol version");
    check(chfdebug_read(d, CHF_SPACE_PROG, 0xfffe, b, 4) == 4, "a read may run past $FFFF");
    check(b[2] == 0xff && b[3] == 0xff, "and reads $FF there");
    chfdebug_cart_get(d, &cart);
    chfdebug_read(d, CHF_SPACE_PROG, 0x4800, &v, 1);
    check(v == (uint8_t)(0xa0 + (cart.page > 0 ? cart.page : 0)), "$4800 shows the selected APPBANK page");
    show("page %d byte %02X", cart.page, v);
    chfdebug_read(d, CHF_SPACE_PROG, 0x6800, &v, 1);
    check(v == 0xff, "$6800 is unpopulated: $FF");

    before = chfdebug_cart_events(d, cev, 64);
    chfdebug_write(d, CHF_SPACE_PROG, 0x8040, 0x5a);
    chfdebug_read(d, CHF_SPACE_PROG, 0x8040, &v, 1);
    check(v == 0x5a, "PROG takes a write to the arena");
    chfdebug_write(d, CHF_SPACE_PROG, 0x0000, 0x00);
    chfdebug_read(d, CHF_SPACE_PROG, 0x0000, &v, 1);
    check(v == 0x29, "the BIOS is not writable");
    chfdebug_write(d, CHF_SPACE_PROG, 0x0ff0, 0x5c);
    chfdebug_read(d, CHF_SPACE_PROG, 0x0ff0, &v, 1);
    chfdebug_read(d, CHF_SPACE_CART, 0x07f0, &w, 1);
    check(v == 0x5c && w == 0x5c, "the cartridge's image takes a write, and the CART space shows it");
    chfdebug_write(d, CHF_SPACE_PROG, 0x0ff0, 0xff);
    chfdebug_write(d, CHF_SPACE_PROG, 0xfd00, 0x99);
    chfdebug_write(d, CHF_SPACE_PROG, 0xfdfe, 0x01);
    chfdebug_write(d, CHF_SPACE_PROG, 0xff00, 0x42);
    check(chfdebug_cart_events(d, cev, 64) == before, "an edit never fires a hotspot (no register, swap or TX event)");

    /* SCRATCH */
    chfdebug_write(d, CHF_SPACE_SCRATCH, 30, 0x99);
    chfdebug_read(d, CHF_SPACE_SCRATCH, 30, &v, 1);
    check(v == 0x99 && cpu(d).r[30] == 0x99, "the scratchpad takes a write and the CPU view agrees");
    chfdebug_read(d, CHF_SPACE_SCRATCH, 62, b, 4);
    check(b[2] == 0xff && b[3] == 0xff, "and reads $FF past r63");
    chfdebug_write(d, CHF_SPACE_SCRATCH, 30, 0);

    /* VRAM */
    chfdebug_write(d, CHF_SPACE_VRAM, 50 * 128 + 60, 7);
    chfdebug_read(d, CHF_SPACE_VRAM, 50 * 128 + 60, &v, 1);
    check(v == 3, "the VRAM stores the low two bits");
    chfdebug_write(d, CHF_SPACE_VRAM, 50 * 128 + 60, 0);
    chfdebug_read(d, CHF_SPACE_VRAM, 8190, b, 4);
    check(b[2] == 0xff && b[3] == 0xff, "and reads $FF past the end");
    chfdebug_read(d, CHF_SPACE_VRAM, 10 * 128 + 19, b, 4);
    check(b[0] == 0 && b[1] == 2, "(10,20) is 2");

    /* CART */
    check(chfdebug_read(d, CHF_SPACE_CART, 0, b, 3) == 3 && b[0] == 0x55 && b[2] == 0x2a, "CART $0000 is the image's first bytes");
    chfdebug_read(d, CHF_SPACE_CART, 0x4000, &v, 1);
    check(v == 0xa0, "the APPBANK pages follow the 16K window");
    chfdebug_read(d, CHF_SPACE_CART, 0x6000, &v, 1);
    check(v == 0xa1, "page 1");
    chfdebug_write(d, CHF_SPACE_CART, 0x10000 + 0x100, 0x77);
    chfdebug_read(d, CHF_SPACE_PROG, 0x8100, &v, 1);
    check(v == 0x77, "the arena follows the image: CART $10000 is PROG $8000");
    chfdebug_write(d, CHF_SPACE_CART, 0x10000 + 0x100, 0);
    chfdebug_read(d, CHF_SPACE_CART, 0x17ffe, b, 4);
    check(b[2] == 0xff, "and CART reads $FF past its end");

    /* IO: the ports as the CPU would read them */
    chfdebug_video_get(d, &vid);
    chfdebug_read(d, CHF_SPACE_IO, 0, b, 6);
    check(b[0] == vid.in0 && b[1] == vid.in1 && b[4] == vid.in4 && b[5] == vid.in5, "IO space reads the ports without disturbing them");
    chfdebug_read(d, CHF_SPACE_IO, 254, b, 4);
    check(b[2] == 0xff, "and $FF past port 255");
    /* a write acts as the CPU writing the port */
    chfdebug_write(d, CHF_SPACE_IO, 5, 0x40);
    chfdebug_io_get(d, &io);
    chfdebug_video_get(d, &vid);
    check(io.sound_mode == 1 && vid.port5 == 0x40 && vid.row == 0x3f, "IO write to port 5: the tone changes, the row latch too");
    chfdebug_write(d, CHF_SPACE_IO, 5, 0x00);
    check(cpu(d).pc == A_IDLE, "(still stopped where it was)");
    chfdebug_write(d, CHF_SPACE_IO, 1, 0x3f);                   /* value (~$3F >> 6) = 3 */
    chfdebug_write(d, CHF_SPACE_IO, 4, (uint8_t)~55);            /* column 55 */
    chfdebug_write(d, CHF_SPACE_IO, 5, (uint8_t)~25);            /* row 25 */
    i = chfdebug_breakpoint_add(d, CHFDEBUG_BP_VRAM, CHF_SPACE_VRAM, 25 * 128 + 55, 25 * 128 + 55, "");
    chfdebug_write(d, CHF_SPACE_IO, 0, 0x60);
    chfdebug_read(d, CHF_SPACE_VRAM, 25 * 128 + 55, &v, 1);
    check(v == 3, "IO write to port 0 with the strobe draws a pixel");
    chfdebug_write(d, CHF_SPACE_IO, 0, 0x50);
    {
        chfdebug_breakpoint bps[4];
        chfdebug_breakpoint_list(d, bps, 4);
        check(bps[0].hits == 0, "and an edit trips no breakpoint");
    }
    chfdebug_breakpoint_remove(d, i);
    chfdebug_write(d, CHF_SPACE_VRAM, 25 * 128 + 55, 0);
    chfdebug_write(d, CHF_SPACE_IO, 1, 0xbf);
    chfdebug_write(d, CHF_SPACE_IO, 4, 0xd7);
    chfdebug_write(d, CHF_SPACE_IO, 5, 0x00);
    chfdebug_vram_writes_clear(d);
    chfdebug_io_events_clear(d);
    {
        chfdebug_io_event ev[4];
        check(chfdebug_io_events(d, ev, 4) == 0, "(an edit is not in the port log)");
    }
}

/* Every way of stepping. */
static void test_stepping(chfdebug *d)
{
    char out[2048];
    chfdebug_cpu c, c2;
    chfdebug_trace tr[16];
    int n;

    /* step into a PI: the callee, one deeper, the return address in PC1 */
    chfdebug_cpu_set(d, CHF_REG_PC, 0x083d);
    c = cpu(d);
    chfdebug_step(d);
    check(wait_stopped(d, 1, 3000), "step into stops");
    c2 = cpu(d);
    check(c2.pc == 0x0900 && c2.pc1 == 0x0840 && c2.instructions == c.instructions + 1 && c2.a == 0x09 &&
          strcmp((why(d), reason), "step") == 0, "a step into a PI is in the routine, PC1 = the return address");

    /* step over the same PI: the routine and its nested call run, and it stops after */
    chfdebug_cpu_set(d, CHF_REG_PC, 0x083d);
    c = cpu(d);
    chfdebug_step_over(d);
    check(wait_stopped(d, 1, 3000), "step over stops");
    c2 = cpu(d);
    check(c2.pc == 0x0840 && c2.instructions == c.instructions + 7 && c2.a == 0x07 && strcmp((why(d), reason), "step over") == 0,
          "step over a PI runs it and the call inside it (7 instructions) and stops at the next one");
    show("pc %04X instr +%llu a %02X reason '%s'", c2.pc, (unsigned long long)(c2.instructions - c.instructions), c2.a, reason);

    /* step over a PK, through K */
    chfdebug_cpu_set(d, CHF_REG_PC, 0x0846);
    chfdebug_cpu_set(d, CHF_REG_K, 0x0950);
    c = cpu(d);
    chfdebug_step_over(d);
    check(wait_stopped(d, 1, 3000), "step over a PK stops");
    c2 = cpu(d);
    check(c2.pc == 0x0847 && c2.instructions == c.instructions + 3 && c2.a == 0x06, "a PK's routine runs and returns (PK, LI, POP)");

    /* a plain instruction is a plain step */
    chfdebug_cpu_set(d, CHF_REG_PC, 0x0802);
    c = cpu(d);
    chfdebug_step_over(d);
    check(wait_stopped(d, 1, 3000) && cpu(d).pc == 0x0805 && cpu(d).instructions == c.instructions + 1 &&
          strcmp((why(d), reason), "step") == 0, "step over a DCI is one step");

    /* step out: of a routine with a nested call, of the nested one, of a PK's */
    chfdebug_cpu_set(d, CHF_REG_PC, 0x0900);
    chfdebug_cpu_set(d, CHF_REG_PC1, 0x0840);
    c = cpu(d);
    chfdebug_step_out(d);
    check(wait_stopped(d, 1, 3000), "step out stops");
    c2 = cpu(d);
    check(c2.pc == 0x0840 && c2.instructions == c.instructions + 6 && strcmp((why(d), reason), "step out") == 0,
          "step out of sub1 runs its nested call too (6 instructions) and stops at the caller's next one");
    chfdebug_cpu_set(d, CHF_REG_PC, 0x0910);
    chfdebug_cpu_set(d, CHF_REG_PC1, 0x0904);
    c = cpu(d);
    chfdebug_step_out(d);
    check(wait_stopped(d, 1, 3000) && cpu(d).pc == 0x0904 && cpu(d).instructions == c.instructions + 2, "step out of the nested routine: one level");
    chfdebug_cpu_set(d, CHF_REG_PC, 0x0950);
    chfdebug_cpu_set(d, CHF_REG_PC1, 0x0847);
    chfdebug_step_out(d);
    check(wait_stopped(d, 1, 3000) && cpu(d).pc == 0x0847, "step out of a PK's routine");
    /* parked on the POP itself */
    chfdebug_cpu_set(d, CHF_REG_PC, 0x0952);
    chfdebug_cpu_set(d, CHF_REG_PC1, 0x0847);
    chfdebug_step_out(d);
    check(wait_stopped(d, 1, 3000) && cpu(d).pc == 0x0847, "step out while stopped on the POP");
    /* a routine that leaves by a jump through Q */
    chfdebug_write(d, CHF_SPACE_PROG, 0x8020, 0x20);
    chfdebug_write(d, CHF_SPACE_PROG, 0x8021, 0x05);
    chfdebug_write(d, CHF_SPACE_PROG, 0x8022, 0x0d);           /* LR P0,Q */
    chfdebug_cpu_set(d, CHF_REG_Q, A_IDLE);
    chfdebug_cpu_set(d, CHF_REG_PC, 0x8020);
    chfdebug_step_out(d);
    check(wait_stopped(d, 1, 3000) && cpu(d).pc == A_IDLE && strcmp((why(d), reason), "step out") == 0,
          "step out of a routine that returns with LR P0,Q");

    /* run to, step n, the prompt */
    chfdebug_cpu_set(d, CHF_REG_PC, A_IDLE);
    chfdebug_run_to(d, 0x0864);
    check(wait_stopped(d, 1, 3000) && cpu(d).pc == 0x0864 && strcmp((why(d), reason), "run to") == 0, "run to $0864");
    c = cpu(d);
    chfdebug_trace_clear(d);
    chfdebug_trace_enable(d, 1);
    chfdebug_command(d, "step 5", out, sizeof out);
    check(wait_stopped(d, 1, 3000) && cpu(d).instructions == c.instructions + 5 && strcmp((why(d), reason), "step") == 0,
          "`step 5` runs five instructions");
    n = chfdebug_trace_read(d, tr, 16);
    check(n == 5 && tr[0].pc == cpu(d).pc && tr[4].pc == 0x0865 && tr[3].pc == 0x0860 && tr[4].bytes[0] == 0x90 &&
          tr[4].len == 2 && tr[3].bytes[0] == 0x62 && tr[3].len == 1 && tr[1].isar == 20 && tr[0].cycles > tr[4].cycles,
          "the trace ring: the five instructions, newest first, with bytes, length, ISAR and time");
    show("n %d pcs %04X %04X %04X", n, tr[0].pc, tr[1].pc, tr[4].pc);
    chfdebug_trace_enable(d, 0);
    check(!chfdebug_trace_enabled(d) && chfdebug_trace_read(d, tr, 16) == 5, "trace off keeps the ring");
    chfdebug_trace_clear(d);
    check(chfdebug_trace_read(d, tr, 1) == 0, "trace clear");

    /* the beam */
    c = cpu(d);
    chfdebug_scanline(d, 1);
    check(wait_stopped(d, 1, 3000), "scanline stops");
    c2 = cpu(d);
    check(c2.vpos == (c.vpos + 1) % 264 && strcmp((why(d), reason), "scanline") == 0, "scanline+1 advances the beam one line");
    c = c2;
    chfdebug_command(d, "scanline 7", out, sizeof out);
    check(wait_stopped(d, 1, 3000) && cpu(d).vpos == (c.vpos + 7) % 264, "scanline 7 advances seven lines");
    c = cpu(d);
    chfdebug_scanline(d, 264);
    check(wait_stopped(d, 1, 3000) && cpu(d).vpos == c.vpos && cpu(d).frame == c.frame + 1, "264 scanlines is one frame");
    c = cpu(d);
    chfdebug_frame(d, 1);
    check(wait_stopped(d, 1, 3000) && cpu(d).frame == c.frame + 1 && strcmp((why(d), reason), "frame") == 0, "frame+1");
    c = cpu(d);
    chfdebug_command(d, "frame 3", out, sizeof out);
    check(wait_stopped(d, 1, 5000) && cpu(d).frame == c.frame + 3, "frame 3 advances three");
    check(cpu(d).vpos >= 248 && cpu(d).vpos <= 255, "(just after the vertical blank begins at line 248)");
    chfdebug_cpu_set(d, CHF_REG_PC, A_IDLE);

    /* stop and resume */
    chfdebug_resume(d);
    test_sleep_ms(60);
    check(!chfdebug_is_stopped(d), "resume with nothing to stop it runs on");
    check(strcmp((why(d), reason), "running") == 0 && why(d) == -1, "the reason is 'running', with no address");
    check(stop_now(d) && strcmp((why(d), reason), "stopped") == 0 && why(d) >= 0, "stop breaks at the next instruction");
}

static int wait_ackseq(chfdebug *d, int want, int timeout_ms)
{
    chfdebug_cart c;
    int waited = 0;

    while (waited < timeout_ms) {
        chfdebug_cart_get(d, &c);
        if (c.ackseq == want)
            return 1;
        test_sleep_ms(5);
        waited += 5;
    }
    return 0;
}

/* The cartridge, its status page and tick, its events, its page select, and
 * the FujiBus packets of a transaction with the stand-in FujiNet. */
static void test_cartridge(chfdebug *d, int fake_up)
{
    static chfdebug_fuji_packet pk[FUJITRACE_MAX];
    chfdebug_cart cart, c2;
    chfdebug_cart_event ev[128];
    char out[4096];
    uint8_t b[4];
    int n, i, seen[4] = { 0 };

    chfdebug_cart_get(d, &cart);
    check(cart.present && cart.type == CHF_CART_FUJINET && strcmp(cart.type_name, "FujiNet") == 0 && cart.mailbox &&
          cart.app_live && cart.booted_game && !cart.armed && cart.link_off == !fake_up,
          "the FujiNet cartridge, running an APPBANK app that claims the mailbox");
    check(cart.live_size == APP_SIZE && cart.live_crc == chf_crc32(g_app, APP_SIZE) && cart.rom_size == APP_SIZE &&
          cart.rom_crc == cart.live_crc, "its image: size and CRC");
    check(cart.npages == 2 && cart.page >= -1 && cart.page <= 1, "two APPBANK pages");
    check(cart.status_page[9] == 'F' && cart.status_page[10] == 'N' && cart.status_page[11] == FN_PROTO_VER &&
          memcmp(cart.mailbox_page + 0x400, cart.status_page, 16) == 0, "the status page: 'FN', the protocol version, in the mailbox copy too");
    check(cart.swaps == 0 && cart.staged_size == 0 && !cart.staged_app, "nothing staged");
    chfdebug_cart_info(d, out, sizeof out);
    check(strncmp(out, "FujiNet cartridge: app (mailbox live), link ", 44) == 0, "the one-line summary");
    show("'%s'", out);
    check(chfdebug_fujinet_state(d, out, sizeof out) > 0 && strstr(out, "Mailbox") && strstr(out, "APPBANK") && strstr(out, "TICK"),
          "the FujiNet state text");

    /* the 60 Hz tick at $FC0E/F counts machine time */
    n = cart.tick;
    chfdebug_frame(d, 6);
    check(wait_stopped(d, 1, 5000), "six frames");
    chfdebug_cart_get(d, &c2);
    check(c2.tick > n && c2.tick - n >= 5 && c2.tick - n <= 7 && (c2.status_page[14] | (c2.status_page[15] << 8)) == c2.tick,
          "the tick advanced by about six, and the status page shows it");
    show("tick %d -> %d, page bytes %02X%02X", n, c2.tick, c2.status_page[15], c2.status_page[14]);
    chfdebug_read(d, CHF_SPACE_PROG, 0xfc0e, b, 2);
    check((b[0] | (b[1] << 8)) == c2.tick, "and PROG $FC0E reads the same");

    /* an APPBANK page select: the page window shows it, an event records it */
    chfdebug_cpu_set(d, CHF_REG_PC, A_PAGE);
    check(run_to_exec(d, A_IDLE), "select page 1 (runs the select block)");
    chfdebug_cart_get(d, &c2);
    chfdebug_read(d, CHF_SPACE_PROG, 0x4800, b, 1);
    check(c2.page == 1 && b[0] == 0xa1, "page 1 is at $4800");
    {
        chfdebug_line l[2];
        chfdebug_disassemble(d, 0x4800, l, 2, NULL);
        check(strstr(l[0].comment, "APPBANK page 1") != NULL, "and the disassembly says so");
    }
    n = chfdebug_cart_events(d, ev, 128);
    for (i = 0; i < n; i++)
        if (ev[i].kind == 3 && ev[i].value == 1 && ev[i].pc == A_PAGE + 4)
            seen[0] = 1;
    check(seen[0], "a BANK event: kind 3, page 1, from the ST at $09A4");
    show("%d events", n);

    /* a transaction with the stand-in FujiNet: registers, the launch, the reply */
    if (!fake_up) {
        printf("note: no loopback FujiNet on this platform: the transaction is skipped\n");
        return;
    }
    chfdebug_fujinet_trace_clear(d);
    chfdebug_cpu_set(d, CHF_REG_PC, A_TXN);
    chfdebug_resume(d);
    check(wait_ackseq(d, 1, 8000), "the transaction completes (ACKSEQ echoes SEQ)");
    check(stop_now(d), "stop");
    chfdebug_cart_get(d, &c2);
    check(c2.ackseq == 1 && c2.reply_cmd == 0x06 && c2.last_error == 0 && (c2.status & 1) && c2.link_up,
          "ACK, no error, link up");
    show("ackseq %d reply %02X err %d status %02X", c2.ackseq, c2.reply_cmd, c2.last_error, c2.status);
    n = chfdebug_cart_events(d, ev, 128);
    for (i = 0; i < n; i++) {
        if (ev[i].kind == 1 && ev[i].reg == 0x00 && ev[i].value == 0x70 && ev[i].pc == 0x0975) seen[0] = 2;
        if (ev[i].kind == 1 && ev[i].reg == 0x01 && ev[i].value == 0xf4 && ev[i].pc == 0x097b) seen[1] = 1;
        if (ev[i].kind == 1 && ev[i].reg == 0x02 && ev[i].value == 0x00 && ev[i].pc == 0x0980) seen[2] = 1;
        if (ev[i].kind == 1 && ev[i].reg == 0x10 && ev[i].value == 0x01 && ev[i].pc == 0x098a) seen[3] = 1;
    }
    check(seen[0] == 2 && seen[1] && seen[2] && seen[3], "the register writes are events: DEVICE, CMD, NPARAM, SEQ, with their PCs");
    {
        uint64_t last = 0;
        int ordered = 1;
        for (i = 0; i < n; i++) {
            if (ev[i].cycle < last) ordered = 0;
            last = ev[i].cycle;
        }
        check(ordered, "oldest first");
    }
    n = chfdebug_fujinet_trace(d, pk, FUJITRACE_MAX);
    check(n >= 2, "the FujiNet trace has the request and the reply");
    if (n >= 2) {
        check(!pk[0].to_host && pk[0].device == 0x70 && pk[0].command == 0xf4 && strstr(pk[0].text, "70 F4 READ HOST SLOTS") != NULL,
              "the request: Channel F to FujiNet, device $70, READ HOST SLOTS");
        check(pk[1].to_host && pk[1].device == 0x70 && pk[1].command == 0x06, "the reply: an ACK from FujiNet");
        show("'%s' / '%s'", pk[0].text, pk[1].text);
    }
    chfdebug_fujinet_trace_clear(d);
    check(chfdebug_fujinet_trace(d, pk, FUJITRACE_MAX) == 0, "the trace clears");
    chfdebug_cpu_set(d, CHF_REG_PC, A_IDLE);
}

/* Symbol files in every format, user labels over built-in ones, the trace
 * of labels in a listing, Save. */
static void test_symbols_and_save(chfdebug *d, const char *tmp)
{
    char path[800], msg[256], name[64], text[4096];
    chfdebug_line l[8];
    uint8_t b[16];
    FILE *f;
    int n;

    snprintf(path, sizeof path, "%s/app.lst", tmp);
    write_text(path,
        " AS V1.42 Beta [Bld 306] - Source File app.asm - Page 1 - 11/05/2026 08:04:26 PM\n"
        "\n\n"
        "  Symbol Table (* = unused):\n"
        "  --------------------------\n"
        "\n"
        " MAIN :                          0802 C |  IDLE2 :                         0860 C |\n"
        "*UNUSED :                        0970 C |  VAR :                          8000 - |\n"
        " HUGE :                         1FFFF C |\n"
        "\n"
        "      5 symbols\n");
    n = chfdebug_load_symbols(d, path, msg, sizeof msg);
    check(n == 3, "an AS listing's symbol table: code symbols only (data and constants skipped)");
    show("n %d msg '%s'", n, msg);
    check(chfdebug_label_address(d, "MAIN") == 0x802 && chfdebug_label_address(d, "UNUSED") == 0x970 &&
          chfdebug_label_address(d, "VAR") == -1, "MAIN, and an unused one");
    chfdebug_address_label(d, 0x0802, name, sizeof name);
    check(strcmp(name, "MAIN") == 0, "a loaded label beats the built-in one at the same address");
    chfdebug_disassemble(d, 0x0802, l, 1, NULL);
    check(strcmp(l[0].label, "MAIN") == 0, "and the disassembly uses it");

    snprintf(path, sizeof path, "%s/app.map", tmp);
    /* as AS -g MAP writes it */
    write_text(path,
        "Segment CODE\n"
        "File app.asm\n"
        "    3:00000800     4:00000802     5:00000804 \n"
        "\n"
        "Symbols in Segment NOTHING\n"
        "DATA                                  Int    8000                      -1  0 0\n"
        "VERSION                               Int    142F                      -1  0 1\n"
        "\n"
        "Symbols in Segment CODE\n"
        "LOOPA                                 Int    900                       -1  1 0\n"
        "LOOPB                                 Int    9A0                       -1  0 0\n");
    n = chfdebug_load_symbols(d, path, msg, sizeof msg);
    check(n == 2 && chfdebug_label_address(d, "loopa") == 0x900 && chfdebug_label_address(d, "LOOPB") == 0x9a0 &&
          chfdebug_label_address(d, "DATA") == -1, "an AS map: the CODE segment only");

    snprintf(path, sizeof path, "%s/plain.sym", tmp);
    write_text(path, "0910 sub3a\n$0950 sub2a\nsub4 = $0970\nsub5 equ 0980h\n; a comment\nnot a symbol line\n");
    n = chfdebug_load_symbols(d, path, msg, sizeof msg);
    check(n == 4 && chfdebug_label_address(d, "sub3a") == 0x910 && chfdebug_label_address(d, "sub2a") == 0x950 &&
          chfdebug_label_address(d, "sub4") == 0x970 && chfdebug_label_address(d, "sub5") == 0x980, "a plain list: AAAA name, $AAAA name, name = $AAAA, name equ AAAAh");
    check(chfdebug_load_symbols(d, "/nonexistent/file.sym", msg, sizeof msg) == -1 && strstr(msg, "Cannot open"), "a missing file is an error with a message");

    /* the file next to the cartridge */
    snprintf(path, sizeof path, "%s/app.bin", tmp);
    chfdebug_set_cart_path(d, path);
    check(chfdebug_load_symbols(d, NULL, msg, sizeof msg) >= 0 && strstr(msg, "app.map"), "next to the cartridge: .map is found first");
    chfdebug_set_cart_path(d, "");
    check(chfdebug_load_symbols(d, NULL, msg, sizeof msg) == -1, "with no cartridge file there is nothing next to it");
    chfdebug_set_label(d, 0x0970, NULL);
    chfdebug_address_label(d, 0x0970, name, sizeof name);
    check(name[0] == '\0', "a label can be removed");

    /* Save */
    snprintf(path, sizeof path, "%s/s.dis", tmp);
    check(chfdebug_save(d, "dis", path, msg, sizeof msg) == 0 && file_size(path) > 4000, "save dis");
    f = fopen(path, "r");
    n = 0;
    if (f) {
        size_t got = fread(text, 1, sizeof text - 1, f);
        text[got] = 0;
        fclose(f);
        n = strstr(text, "RESET:") && strstr(text, "JMP") ? 1 : 0;
    }
    check(n, "the listing starts with the labelled BIOS jump");
    snprintf(path, sizeof path, "%s/s.mbx", tmp);
    check(chfdebug_save(d, "mailbox", path, msg, sizeof msg) == 0 && file_size(path) == 0x800, "save mailbox: $F800-$FFFF");
    snprintf(path, sizeof path, "%s/s.vram", tmp);
    check(chfdebug_save(d, "vram", path, msg, sizeof msg) == 0 && file_size(path) == 8192, "save vram: 8192 bytes");
    f = fopen(path, "rb");
    if (f) {
        fseek(f, 10 * 128 + 20, SEEK_SET);
        n = fread(b, 1, 1, f) == 1 && b[0] == 2;
        fclose(f);
    } else n = 0;
    check(n, "and the pixel is in it");
    snprintf(path, sizeof path, "%s/s.scr", tmp);
    check(chfdebug_save(d, "scratch", path, msg, sizeof msg) == 0 && file_size(path) == 64, "save scratch: 64 bytes");
    snprintf(path, sizeof path, "%s/s.cart", tmp);
    check(chfdebug_save(d, "cart", path, msg, sizeof msg) == 0 && file_size(path) == APP_SIZE, "save cart: the live image");
    snprintf(path, sizeof path, "%s/s.ppm", tmp);
    check(chfdebug_save(d, "screen", path, msg, sizeof msg) == 0 && file_size(path) == 15 + 204 * 232 * 3, "save screen: a PPM");
    f = fopen(path, "rb");
    if (f) { n = fread(text, 1, 15, f) == 15 && memcmp(text, "P6\n204 232\n255\n", 15) == 0; fclose(f); } else n = 0;
    check(n, "with its header");
    snprintf(path, sizeof path, "%s/s.vs.ppm", tmp);
    check(chfdebug_save(d, "vramshot", path, msg, sizeof msg) == 0 && file_size(path) == 15 + 256 * 256 * 3, "save vramshot: the VRAM view");
    snprintf(path, sizeof path, "%s/s.regs", tmp);
    check(chfdebug_save(d, "regs", path, msg, sizeof msg) == 0 && file_size(path) > 200, "save regs");
    f = fopen(path, "r");
    if (f) { size_t got = fread(text, 1, sizeof text - 1, f); text[got] = 0; fclose(f); n = strstr(text, "PC=") && strstr(text, "ISAR=") && strstr(text, "port 0") != NULL; } else n = 0;
    check(n, "with the CPU and the ports");
    check(chfdebug_save(d, "nonsense", path, msg, sizeof msg) == -1 && strstr(msg, "Unknown save kind"), "an unknown kind is refused with the list");
    check(chfdebug_save(d, "vram", "/nonexistent/dir/x", msg, sizeof msg) == -1 && strstr(msg, "Cannot write"), "an unwritable path is an error");
}

/* Every prompt command, and the completions. */
static void test_prompt(chfdebug *d, const char *tmp)
{
    char out[16384], path[800], cmd[900];
    chfdebug_breakpoint bps[16];
    chfdebug_cpu c;
    int n, id;

    chfdebug_cpu_set(d, CHF_REG_PC, A_IDLE);
    chfdebug_command(d, "help", out, sizeof out);
    check(strstr(out, "bpv") && strstr(out, "bps") && strstr(out, "bpi") && strstr(out, "romc") && strstr(out, "kstack") &&
          strstr(out, "vramshot"), "help lists the commands");
    chfdebug_command(d, "frobnicate", out, sizeof out);
    check(strstr(out, "unknown command"), "an unknown command is said so");
    check(chfdebug_command(d, "", out, sizeof out) == 0 && chfdebug_command(d, "   ", out, sizeof out) == 0, "an empty line is nothing");

    /* breakpoints */
    chfdebug_command(d, "break $0863 a == 1", out, sizeof out);
    check(strstr(out, "breakpoint #"), "break");
    chfdebug_command(d, "bpr $8000-$8003", out, sizeof out);
    chfdebug_command(d, "bpw CART_RAM", out, sizeof out);
    chfdebug_command(d, "bpi 0-1 value != 0", out, sizeof out);
    chfdebug_command(d, "bpo ROW_SOUND value == $40", out, sizeof out);
    check(strstr(out, "breakpoint #"), "bpo takes a port name");
    chfdebug_command(d, "bpv 10,20-12,40 value == 2", out, sizeof out);
    check(strstr(out, "breakpoint #"), "bpv");
    chfdebug_command(d, "bps r12-r15", out, sizeof out);
    check(strstr(out, "breakpoint #"), "bps takes a register range");
    chfdebug_command(d, "bps 20", out, sizeof out);
    chfdebug_command(d, "breaks", out, sizeof out);
    check(strstr(out, "$0863") && strstr(out, "$8000-$8003") && strstr(out, "ports 0-1") && strstr(out, "port 5") &&
          strstr(out, "row 10 col 20 to row 12 col 40") && strstr(out, "r12-r15") && strstr(out, "r20") && strstr(out, "if value == 2"),
          "breaks lists them all");
    n = chfdebug_breakpoint_list(d, bps, 16);
    check(n == 8 && bps[0].type == CHFDEBUG_BP_EXEC && bps[1].type == CHFDEBUG_BP_READ && bps[2].type == CHFDEBUG_BP_WRITE &&
          bps[3].type == CHFDEBUG_BP_IN && bps[4].type == CHFDEBUG_BP_OUT && bps[5].type == CHFDEBUG_BP_VRAM &&
          bps[6].type == CHFDEBUG_BP_SCRATCH && bps[6].space == CHF_SPACE_SCRATCH && bps[6].start == 12 && bps[6].end == 15 &&
          bps[5].start == 10 * 128 + 20 && bps[5].end == 12 * 128 + 40 && bps[4].start == 5, "each in its class, space and range");
    id = bps[0].id;
    snprintf(cmd, sizeof cmd, "disable #%d", id);
    chfdebug_command(d, cmd, out, sizeof out);
    chfdebug_breakpoint_list(d, bps, 16);
    check(!bps[0].enabled && !chfdebug_breakpoint_check(d, 0x0863), "disable");
    snprintf(cmd, sizeof cmd, "enable %d", id);
    chfdebug_command(d, cmd, out, sizeof out);
    chfdebug_breakpoint_list(d, bps, 16);
    check(bps[0].enabled && chfdebug_breakpoint_check(d, 0x0863), "enable");
    chfdebug_command(d, "delete $0863", out, sizeof out);
    check(chfdebug_breakpoint_list(d, bps, 16) == 7 && !chfdebug_breakpoint_check(d, 0x0863), "delete by address");
    chfdebug_breakpoint_list(d, bps, 16);
    snprintf(cmd, sizeof cmd, "delete #%d", bps[0].id);
    chfdebug_command(d, cmd, out, sizeof out);
    check(chfdebug_breakpoint_list(d, bps, 16) == 6, "delete by id");
    chfdebug_command(d, "delete 99", out, sizeof out);
    check(strstr(out, "no breakpoint"), "an unknown id");
    chfdebug_command(d, "clear", out, sizeof out);
    check(chfdebug_breakpoint_list(d, bps, 16) == 0, "clear");
    chfdebug_command(d, "bpv 64,0", out, sizeof out);
    check(strstr(out, "error"), "bpv refuses a row outside the VRAM");
    chfdebug_command(d, "bpo 300", out, sizeof out);
    check(strstr(out, "error"), "bpo refuses a port outside 0-255");
    chfdebug_command(d, "break $0800 ((", out, sizeof out);
    check(strstr(out, "error") && chfdebug_breakpoint_list(d, bps, 16) == 0, "an unparsable condition is refused");
    check(chfdebug_breakpoint_add(d, CHFDEBUG_BP_VRAM, CHF_SPACE_PROG, 0, 0, "") == -1 &&
          chfdebug_breakpoint_add(d, CHFDEBUG_BP_IN, CHF_SPACE_IO, 0, 256, "") == -1 &&
          chfdebug_breakpoint_add(d, CHFDEBUG_BP_SCRATCH, CHF_SPACE_SCRATCH, 0, 64, "") == -1 &&
          chfdebug_breakpoint_add(d, CHFDEBUG_BP_VRAM, CHF_SPACE_VRAM, 0, 8192, "") == -1 &&
          chfdebug_breakpoint_add(d, 0, CHF_SPACE_PROG, 0, 0, "") == -1 &&
          chfdebug_breakpoint_add(d, 128, CHF_SPACE_PROG, 0, 0, "") == -1 &&
          chfdebug_breakpoint_add(d, CHFDEBUG_BP_EXEC | CHFDEBUG_BP_IN, CHF_SPACE_PROG, 0, 0, "") == -1,
          "a wrong space, a range outside it, no type and a mixed type are refused");
    id = chfdebug_breakpoint_add(d, CHFDEBUG_BP_READ | CHFDEBUG_BP_WRITE, CHF_SPACE_PROG, 0x8000, 0x8000, "");
    check(id > 0, "read and write may be one breakpoint");
    chfdebug_breakpoint_remove(d, id);
    id = chfdebug_breakpoint_add(d, CHFDEBUG_BP_IN, CHF_SPACE_IO, 3, 3, "");
    chfdebug_breakpoint_enable(d, id, 0);
    chfdebug_breakpoint_list(d, bps, 16);
    check(!bps[0].enabled, "a disabled breakpoint");
    chfdebug_breakpoint_clear(d);

    /* print */
    chfdebug_command(d, "print p[$0800] + [$0801]", out, sizeof out);
    check(strncmp(out, "$55  85", 7) == 0, "print: PROG bytes, [addr] and p[addr]");
    chfdebug_command(d, "print v[10*128+20]", out, sizeof out);
    check(strncmp(out, "$02  2", 6) == 0, "print v[row*128+col]");
    chfdebug_command(d, "print s[20] == r20", out, sizeof out);
    check(strncmp(out, "$01  1", 6) == 0, "print s[reg] and r20");
    chfdebug_command(d, "print c[0] + c[$4000]", out, sizeof out);
    check(strncmp(out, "$F5  245", 8) == 0, "print c[off]");
    chfdebug_command(d, "print io[0] & $0F", out, sizeof out);
    check(strncmp(out, "$0F  15", 7) == 0, "print io[port]");
    chfdebug_command(d, "print pc == $0860", out, sizeof out);
    check(strncmp(out, "$01  1", 6) == 0, "print pc");
    chfdebug_command(d, "set k $0950", out, sizeof out);
    chfdebug_command(d, "print o17 + k + hu * 0", out, sizeof out);
    check(strncmp(out, "$095F", 5) == 0, "print octal, k and hu");
    chfdebug_command(d, "print nothing", out, sizeof out);
    check(strstr(out, "unknown name"), "print: an unknown name");
    chfdebug_command(d, "print value", out, sizeof out);
    check(strstr(out, "unknown name"), "`value` means nothing outside a breakpoint");
    chfdebug_command(d, "print 1 / 0", out, sizeof out);
    check(strstr(out, "division by zero"), "print: division by zero");
    chfdebug_command(d, "print vpos < 264 && hpos >= 0 && frame >= 0 && cycles > 0", out, sizeof out);
    check(strncmp(out, "$01  1", 6) == 0, "print vpos, hpos, frame, cycles");

    /* regs and set */
    chfdebug_command(d, "regs", out, sizeof out);
    check(strstr(out, "PC=0860") && strstr(out, "ISAR=o24") && strstr(out, "K stack") && strstr(out, "next:"), "regs");
    chfdebug_command(d, "set a $5a", out, sizeof out);
    chfdebug_command(d, "set is o25", out, sizeof out);
    chfdebug_command(d, "set h $1122", out, sizeof out);
    chfdebug_command(d, "set ku $77", out, sizeof out);
    chfdebug_command(d, "set kl $88", out, sizeof out);
    chfdebug_command(d, "set q $abcd", out, sizeof out);
    chfdebug_command(d, "set r40 $c3", out, sizeof out);
    chfdebug_command(d, "set dc0 $9000", out, sizeof out);
    chfdebug_command(d, "set c 1", out, sizeof out);
    chfdebug_command(d, "set icb 1", out, sizeof out);
    c = cpu(d);
    check(c.a == 0x5a && c.isar == 025 && c.j_h_k_q[1] == 0x1122 && c.j_h_k_q[2] == 0x7788 && c.j_h_k_q[3] == 0xabcd &&
          c.r[40] == 0xc3 && c.dc0 == 0x9000 && c.carry && c.icb, "set: a, is, h, ku, kl, q, r40, dc0, c, icb");
    chfdebug_command(d, "set pc $0802", out, sizeof out);
    check(strstr(out, "PC=0802") && cpu(d).dbus == 0x2a, "set pc, and the prompt shows the registers after");
    chfdebug_command(d, "set pc0 $0861", out, sizeof out);
    check(cpu(d).pc == 0x0860, "set pc0");
    chfdebug_command(d, "set bogus 1", out, sizeof out);
    check(strstr(out, "usage"), "set: an unknown register");
    chfdebug_command(d, "set a", out, sizeof out);
    chfdebug_command(d, "set w 0", out, sizeof out);
    chfdebug_command(d, "set a 0", out, sizeof out);
    chfdebug_command(d, "set r40 0", out, sizeof out);
    chfdebug_command(d, "set q 0", out, sizeof out);
    chfdebug_command(d, "set is o24", out, sizeof out);

    /* memory */
    chfdebug_command(d, "mem $0800 16", out, sizeof out);
    check(strncmp(out, "prog space\n0800  55 00 2A 80 00", 31) == 0, "mem prog");
    chfdebug_command(d, "mem $fc09 4", out, sizeof out);
    check(strstr(out, "FC09") && strstr(out, "46 4E 02"), "mem shows the status page: 'F', 'N', the protocol version");
    chfdebug_command(d, "mem scratch 0 16", out, sizeof out);
    check(strstr(out, "scratch space") != NULL, "mem scratch");
    chfdebug_command(d, "mem vram 10*128+16 8", out, sizeof out);
    check(strstr(out, "vram space") && strstr(out, "00 00 00 00 02 00 00 00") != NULL, "mem vram");
    show("%s", out);
    chfdebug_command(d, "mem cart 0 4", out, sizeof out);
    check(strstr(out, "cart space") && strstr(out, "00000  55 00 2A 80"), "mem cart");
    chfdebug_command(d, "mem io 0 8", out, sizeof out);
    check(strstr(out, "io space") != NULL, "mem io");
    chfdebug_command(d, "mem $fd00 2", out, sizeof out);
    check(strstr(out, "REGSEL+00") != NULL, "mem names the mailbox's hotspots");
    chfdebug_command(d, "poke $8100 1 2 3", out, sizeof out);
    check(strstr(out, "3 bytes at prog $8100") && test_peek(0x8102) == 3, "poke prog");
    chfdebug_command(d, "poke scratch 31 $99", out, sizeof out);
    check(cpu(d).r[31] == 0x99, "poke scratch");
    chfdebug_command(d, "poke vram 8000 3", out, sizeof out);
    chfdebug_command(d, "poke io 5 $40", out, sizeof out);
    check(chfdebug_command(d, "ports", out, sizeof out) > 0 && strstr(out, "sound 1 kHz"), "poke io 5 $40 sets the tone");
    chfdebug_command(d, "poke io 5 0", out, sizeof out);
    chfdebug_command(d, "poke scratch 31 0", out, sizeof out);
    chfdebug_command(d, "poke vram 8000 0", out, sizeof out);
    chfdebug_command(d, "poke $8100 0 0 0", out, sizeof out);

    /* the views */
    chfdebug_command(d, "scratch", out, sizeof out);
    check(strstr(out, "+7") && strstr(out, "r56 o70") && strstr(out, "[") && strstr(out, "ISAR"), "scratch: the 8 x 8 grid with the ISAR cell bracketed");
    chfdebug_command(d, "isar", out, sizeof out);
    check(strstr(out, "ISAR = o24 (20)") && strstr(out, "r20"), "isar");
    chfdebug_command(d, "vram 10", out, sizeof out);
    check(strstr(out, "row 10: palette 0 (black & white)") && strstr(out, "00000000 00000000 0000"), "vram <row>");
    chfdebug_command(d, "vram", out, sizeof out);
    check(strstr(out, "visible picture") && strstr(out, "p0") && strstr(out, "2"), "vram: the picture");
    chfdebug_command(d, "pal", out, sizeof out);
    check(strstr(out, "light blue") && strstr(out, "light green") && strstr(out, "#CED0FF") && strstr(out, "rows 64"), "pal");
    chfdebug_command(d, "video", out, sizeof out);
    check(strstr(out, "beam: line") && strstr(out, "NTSC") && strstr(out, "VRAM strobes") && strstr(out, "next strobe"), "video");
    chfdebug_command(d, "ports", out, sizeof out);
    check(strstr(out, "PANEL") && strstr(out, "RIGHT_C") && strstr(out, "LEFT_C") && strstr(out, "ROW_SOUND") && strstr(out, "column 40"), "ports");
    chfdebug_command(d, "sound", out, sizeof out);
    check(strstr(out, "tone: off") && strstr(out, "envelope"), "sound");
    chfdebug_command(d, "input", out, sizeof out);
    check(strstr(out, "right (player 1) controller") && strstr(out, "(none)") && strstr(out, "RESET up"), "input");
    chfdebug_command(d, "cart", out, sizeof out);
    check(strstr(out, "FujiNet cartridge") && strstr(out, "type FujiNet") && strstr(out, "APPBANK pages: 2"), "cart");
    chfdebug_command(d, "mailbox", out, sizeof out);
    check(strstr(out, "$F800-$FFFF") && strstr(out, "FC00") && strstr(out, "ACKSEQ"), "mailbox");
    chfdebug_command(d, "bios", out, sizeof out);
    check(strstr(out, "Fairchild Channel F") && strstr(out, "BIOS:"), "bios");

    /* the ROMC stream */
    chfdebug_command(d, "romc", out, sizeof out);
    check(strstr(out, "off") && strstr(out, "empty"), "romc is off, and empty");
    chfdebug_command(d, "romc on", out, sizeof out);
    chfdebug_command(d, "step 4", out, sizeof out);
    check(wait_stopped(d, 1, 3000), "step 4 with the ROMC stream on");
    chfdebug_command(d, "romc 6", out, sizeof out);
    check(strstr(out, "ROMC") && strstr(out, "PC0=") && strchr(out, '\n') != NULL, "romc n: the bus cycles");
    n = chfdebug_romc_enabled(d) ? 1 : 0;
    chfdebug_command(d, "romc off", out, sizeof out);
    check(n && !chfdebug_romc_enabled(d), "romc off");

    /* labels, disasm, trace, save */
    chfdebug_command(d, "label $0900 first", out, sizeof out);
    chfdebug_command(d, "disasm $0900 3", out, sizeof out);
    check(strstr(out, "first:") && strstr(out, "0900") && strstr(out, "LR"), "label and disasm");
    chfdebug_command(d, "label $0900", out, sizeof out);
    chfdebug_command(d, "print first", out, sizeof out);
    check(strstr(out, "unknown name"), "label with no name removes it");
    snprintf(path, sizeof path, "%s/p.sym", tmp);
    write_text(path, "0990 promptlbl\n");
    snprintf(cmd, sizeof cmd, "labels %s", path);
    chfdebug_command(d, cmd, out, sizeof out);
    check(strstr(out, "1 label") && chfdebug_label_address(d, "promptlbl") == 0x990, "labels <file>");
    chfdebug_command(d, "trace", out, sizeof out);
    check(strstr(out, "trace is off"), "trace shows its state when empty");
    chfdebug_command(d, "trace on", out, sizeof out);
    chfdebug_command(d, "step 3", out, sizeof out);
    check(wait_stopped(d, 1, 3000), "step 3 traced");
    chfdebug_command(d, "trace 8", out, sizeof out);
    check(strstr(out, "A=") && strstr(out, "IS=o") && strstr(out, "DC0="), "trace n");
    chfdebug_command(d, "trace off", out, sizeof out);
    chfdebug_command(d, "trace clear", out, sizeof out);
    snprintf(cmd, sizeof cmd, "save vram %s/prompt.vram", tmp);
    chfdebug_command(d, cmd, out, sizeof out);
    snprintf(path, sizeof path, "%s/prompt.vram", tmp);
    check(strstr(out, "Saved 8192 bytes") && file_size(path) == 8192, "save, from the prompt");
    chfdebug_command(d, "save", out, sizeof out);
    check(strstr(out, "usage"), "save with no arguments");

    /* running commands */
    chfdebug_command(d, "runto $0864", out, sizeof out);
    check(wait_stopped(d, 1, 3000) && cpu(d).pc == 0x0864 && strcmp((why(d), reason), "run to") == 0, "runto");
    chfdebug_command(d, "frame", out, sizeof out);
    check(wait_stopped(d, 1, 3000) && strcmp((why(d), reason), "frame") == 0, "frame");
    chfdebug_command(d, "scanline 3", out, sizeof out);
    check(wait_stopped(d, 1, 3000) && strcmp((why(d), reason), "scanline") == 0, "scanline");
    chfdebug_cpu_set(d, CHF_REG_PC, 0x083d);
    chfdebug_command(d, "over", out, sizeof out);
    check(wait_stopped(d, 1, 3000) && cpu(d).pc == 0x0840, "over");
    chfdebug_cpu_set(d, CHF_REG_PC, 0x0900);
    chfdebug_cpu_set(d, CHF_REG_PC1, 0x0840);
    chfdebug_command(d, "out", out, sizeof out);
    check(wait_stopped(d, 1, 3000) && cpu(d).pc == 0x0840, "out");
    chfdebug_cpu_set(d, CHF_REG_PC, A_IDLE);
    chfdebug_command(d, "step", out, sizeof out);
    check(wait_stopped(d, 1, 3000) && cpu(d).pc == 0x0861, "step");
    chfdebug_command(d, "run", out, sizeof out);
    test_sleep_ms(40);
    check(!chfdebug_is_stopped(d), "run");
    chfdebug_command(d, "step", out, sizeof out);
    check(strstr(out, "running"), "step while running says so");
    chfdebug_command(d, "set a 1", out, sizeof out);
    check(strstr(out, "running"), "set while running says so");
    chfdebug_command(d, "stop", out, sizeof out);
    check(wait_stopped(d, 1, 3000), "stop");
}

static int has_line(const char *list, const char *word)
{
    size_t n = strlen(word);
    for (const char *p = list; *p; ) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == n && strncmp(p, word, n) == 0) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

static void test_completions(chfdebug *d)
{
    char out[4096];
    int n;

    n = chfdebug_completions(d, "st", out, sizeof out);
    check(n == 2 && has_line(out, "step") && has_line(out, "stop"), "commands complete: st -> step, stop");
    n = chfdebug_completions(d, "bp", out, sizeof out);
    check(n == 6 && has_line(out, "bpi") && has_line(out, "bpo") && has_line(out, "bpr") && has_line(out, "bps") &&
          has_line(out, "bpv") && has_line(out, "bpw"), "bp -> the six breakpoint commands");
    n = chfdebug_completions(d, "print CART_", out, sizeof out);
    check(n >= 3 && has_line(out, "CART_ENTRY") && has_line(out, "CART_RAM") && has_line(out, "CART_SIG"), "labels complete");
    n = chfdebug_completions(d, "set p", out, sizeof out);
    check(has_line(out, "pc") && has_line(out, "pc0") && has_line(out, "pc1"), "registers complete");
    n = chfdebug_completions(d, "break ROW_", out, sizeof out);
    check(has_line(out, "ROW_SOUND"), "port names complete");
    n = chfdebug_completions(d, "save v", out, sizeof out);
    check(n == 2 && has_line(out, "vram") && has_line(out, "vramshot"), "save kinds complete");
    n = chfdebug_completions(d, "mem ", out, sizeof out);
    check(n == 5 && has_line(out, "prog") && has_line(out, "scratch") && has_line(out, "vram") && has_line(out, "cart") &&
          has_line(out, "io"), "memory spaces complete");
    n = chfdebug_completions(d, "poke s", out, sizeof out);
    check(n == 1 && has_line(out, "scratch"), "poke s -> scratch");
    n = chfdebug_completions(d, "romc o", out, sizeof out);
    check(n == 2 && has_line(out, "on") && has_line(out, "off"), "romc o -> on, off");
    n = chfdebug_completions(d, "zzz", out, sizeof out);
    check(n == 0 && out[0] == '\0', "nothing completes");
    n = chfdebug_completions(d, "", out, sizeof out);
    check(n > 40, "an empty prefix lists the commands");
}

/* Detach lets the machine run free; a power cycle with the engine attached
 * stops the new machine on its first instruction and keeps the breakpoints. */
static void test_detach_and_cycle(chfdebug *d, const chf_host_opts_t *ho)
{
    chfdebug_breakpoint bps[8];
    chfdebug_vram_write w[4];
    chfdebug_io_event ev[4];
    chfdebug_cpu c;
    chf_machine_t *m;
    uint64_t f0;
    int id, fresh, n, hooked;

    id = chfdebug_breakpoint_add(d, CHFDEBUG_BP_EXEC, CHF_SPACE_PROG, A_ENTRY, A_ENTRY, "");
    check(id > 0, "an execute breakpoint to keep across the detach");
    chfdebug_detach(d);
    check(!chfdebug_is_attached(d) && !chfdebug_is_stopped(d), "detach");
    chf_host_lock();
    m = chf_host_machine();
    hooked = m->instr_hook != NULL || m->bus_hook != NULL || m->track != NULL;
    chf_host_unlock();
    check(!hooked, "no hook and no video bookkeeping once detached");
    f0 = chf_host_frame_count();
    test_sleep_ms(300);
    check(chf_host_frame_count() > f0 + 5, "the machine runs on at full speed");
    c = cpu(d);
    check(c.cycles > 0 && c.frame > 0, "and the CPU view still answers while it runs");
    check(chfdebug_breakpoint_list(d, bps, 8) == 1, "the breakpoint is kept");
    chfdebug_stop(d);
    check(chfdebug_is_attached(d) && wait_stopped(d, 1, 3000), "stop on a detached engine attaches it and stops");
    check(strcmp((why(d), reason), "stopped") == 0, "'stopped'");

    /* a power cycle with the engine attached */
    chfdebug_power_down(d);
    check(!chfdebug_is_attached(d), "powering down detaches");
    chf_host_stop();
    check(cpu(d).cycles == 0 && cpu(d).pc == 0, "with the machine gone the CPU view is empty");
    fresh = boot(d, ho);
    check(fresh, "the new machine stops on its first instruction: the engine was attached across the power cycle");
    if (!fresh)
        return;
    check(chfdebug_is_attached(d) && chfdebug_vram_writes(d, NULL, w, 4) == 0 && chfdebug_io_events(d, ev, 4) == 0,
          "with empty logs");
    n = chfdebug_breakpoint_list(d, bps, 8);
    check(n == 1 && bps[0].type == CHFDEBUG_BP_EXEC, "and the breakpoint still set");
    check(run_until_stop(d, 5000) && cpu(d).pc == A_ENTRY && strcmp((why(d), reason), "breakpoint: exec $0802") == 0,
          "which hits again at the cartridge's entry");
    chfdebug_breakpoint_clear(d);
}

/* ---- Videocarts: a game in the slot instead of the FujiNet cartridge ----------- */

/* start a game cartridge (the program `code`) on the tests' BIOS, engine attached */
static int boot_game(chfdebug *d, int type, const uint8_t *code, size_t n, uint32_t size)
{
    static uint8_t img[0x4000];
    chf_host_opts_t ho;

    test_cart(img, size, code, n, 0);
    memset(&ho, 0, sizeof ho);
    ho.machine = CHF_MACHINE_CHANNELF;
    ho.bios_lo = g_lo;
    ho.bios_hi = g_hi;
    ho.bios_id = CHF_ROM_SL31253;
    ho.cart = img;
    ho.cart_size = size;
    ho.cart_type = type;
    ho.cart_app = false;
    chfdebug_power_down(d);
    chf_host_stop();
    chfdebug_attach(d);
    return boot(d, &ho);
}

static void test_videocarts(chfdebug *d, const char *tmp)
{
    /* DCI $2800 / LIS 0 / ST ; DCI $2800 / LM / AI 1 ; DCI $2801 / ST ; BR self */
    static const uint8_t chess[] = { 0x2a, 0x28, 0x00, 0x70, 0x17, 0x2a, 0x28, 0x00, 0x16, 0x24, 0x01,
                                     0x2a, 0x28, 0x01, 0x17, 0x90, 0xff };
    static const uint8_t multi[] = { 0x2a, 0x30, 0x00, 0x20, 0x23, 0x17, 0x90, 0xff };
    static const uint8_t spin[] = { T_SPIN };
    chfdebug_cart cart;
    chfdebug_io io;
    chfdebug_breakpoint bps[4];
    chfdebug_cart_event cev[4];
    char out[1024], path[800];
    uint8_t b[16], v;
    int id;

    /* chess: 2K of RAM at $2800 */
    check(boot_game(d, CHF_CART_CHESS, chess, sizeof chess, 0x800), "a Chess Videocart boots on the tests' BIOS, stopped at reset");
    chfdebug_cart_get(d, &cart);
    check(cart.present && cart.type == CHF_CART_CHESS && cart.rom_size == 0x800 && cart.ram_size == 0x800 && !cart.mailbox &&
          !cart.link_up && cart.rom_crc != 0 && cart.softlist_name == NULL && cart.description && cart.description[0],
          "cartridge: Chess, 2K image and 2K RAM, no mailbox, no software-list match");
    show("type %d rom %u ram %u desc '%s'", cart.type, cart.rom_size, cart.ram_size, cart.description);
    chfdebug_cart_info(d, out, sizeof out);
    check(strstr(out, "2048 bytes") && strstr(out, cart.type_name), "its one line");
    check(chfdebug_space_size(d, CHF_SPACE_CART) == 0x800 + 0x800, "the CART space is the ROM then the RAM");
    check(chfdebug_fujinet_state(d, out, sizeof out) > 0 && strstr(out, "not the FujiNet"), "and there is no FujiNet state");
    check(chfdebug_cart_events(d, cev, 4) == 0, "and no cartridge events");
    check(cart.mailbox_page[0] == 0xff && cart.mailbox_page[0x7ff] == 0xff, "the 'mailbox' of a game reads $FF");
    id = chfdebug_breakpoint_add(d, CHFDEBUG_BP_WRITE, CHF_SPACE_PROG, 0x2800, 0x2fff, "value == 1");
    check(id > 0 && run_until_stop(d, 5000), "a write breakpoint on the cartridge's RAM");
    check(strcmp((why(d), reason), "write prog $2801 = $01") == 0 && cpu(d).pc == 0x0811, "stops after the program's second store");
    show("pc %04X '%s'", cpu(d).pc, reason);
    chfdebug_breakpoint_remove(d, id);
    chfdebug_read(d, CHF_SPACE_PROG, 0x2800, b, 2);
    check(b[0] == 0 && b[1] == 1, "the RAM at $2800");
    chfdebug_read(d, CHF_SPACE_CART, 0x800 + 1, &v, 1);
    check(v == 1, "is the CART space after the 2K ROM");
    chfdebug_write(d, CHF_SPACE_PROG, 0x2810, 0xa5);
    chfdebug_read(d, CHF_SPACE_CART, 0x800 + 0x10, &v, 1);
    check(v == 0xa5, "PROG writes reach the RAM");
    chfdebug_write(d, CHF_SPACE_CART, 0x800 + 0x20, 0x5a);
    chfdebug_read(d, CHF_SPACE_PROG, 0x2820, &v, 1);
    check(v == 0x5a, "and CART writes do too");
    chfdebug_write(d, CHF_SPACE_PROG, 0x0802, 0x00);
    chfdebug_write(d, CHF_SPACE_CART, 0x0003, 0x00);
    chfdebug_read(d, CHF_SPACE_PROG, 0x0802, &v, 1);
    check(v == 0x2a, "the ROM is not writable");
    snprintf(path, sizeof path, "%s/game.cart", tmp);
    check(chfdebug_save(d, "cart", path, out, sizeof out) == 0 && file_size(path) == 0x800, "save cart: the 2K image");
    remove(path);
    chfdebug_command(d, "cart", out, sizeof out);
    check(strstr(out, "Chess") && strstr(out, "RAM 2048 bytes"), "the prompt's cart");

    /* the multicart's bank latch at $3000 */
    check(boot_game(d, CHF_CART_MULTI, multi, sizeof multi, 0x4000), "a multicart boots");
    chfdebug_cpu_set(d, CHF_REG_PC, 0x0000);
    check(run_to_exec(d, 0x0808), "runs to the spin after the bank write");
    chfdebug_cart_get(d, &cart);
    check(cart.type == CHF_CART_MULTI && cart.bank == 3 && cart.half == 1, "bank 3, half 1: the latch as the program wrote it");
    chfdebug_command(d, "cart", out, sizeof out);
    check(strstr(out, "bank 3 half 1"), "and the prompt says so");
    check(chfdebug_label_address(d, "VCART_BANK") == 0x3000, "(VCART_BANK is the latch's address)");

    /* a 2102: Maze on ports $24/$25, Hangman on $20/$21 */
    for (int pass = 0; pass < 2; pass++) {
        const int type = pass ? CHF_CART_HANGMAN : CHF_CART_MAZE;
        const int port = pass ? 0x20 : 0x24;

        check(boot_game(d, type, spin, sizeof spin, 0x800), pass ? "a Hangman Videocart boots" : "a Maze Videocart boots");
        chfdebug_io_get(d, &io);
        chfdebug_cart_get(d, &cart);
        check(io.has_2102 && io.ram_port == port && io.cart_type == type && cart.ram_size == 1024 && cart.type == type,
              "the 2102's ports and its 1024 bits");
        check(chfdebug_space_size(d, CHF_SPACE_CART) == 0x800 + 128, "the CART space: the ROM and the RAM packed into 128 bytes");
        chfdebug_write(d, CHF_SPACE_CART, 0x800, 0x04);              /* bit 2 of byte 0: cell 2 */
        chfdebug_read(d, CHF_SPACE_CART, 0x800, &v, 1);
        check(v == 0x04, "the RAM takes a byte (8 cells)");
        chfdebug_write(d, CHF_SPACE_IO, (uint32_t)port + 1, 0x10);    /* address bit 1 from data bit 4: address 2 */
        chfdebug_io_get(d, &io);
        check(io.ram_addr == 2 && io.ram_bit == 1, "an IO write to the second port latches address 2: bit 1");
        chfdebug_write(d, CHF_SPACE_IO, (uint32_t)port + 1, 0x01);    /* address 1 */
        chfdebug_io_get(d, &io);
        check(io.ram_addr == 1 && io.ram_bit == 0, "address 1: bit 0");
        chfdebug_read(d, CHF_SPACE_IO, (uint32_t)port, b, 2);
        check(b[1] == 0x01, "the IO space reads the port's latch");
    }
}

/* ---- the BIOS's own services ------------------------------------------------------ */

/* The app on a BIOS that has the four entry points (the stand-in, or the user's own
 * when CHF_BIOS_DIR has it): the K stack, PUSHK and POPK stepped over and into. */
static void test_bios_services(chfdebug *d, const char *label, const uint8_t *lo, const uint8_t *hi, int bios_id)
{
    chf_host_opts_t ho;
    chfdebug_cpu c;
    chfdebug_line l[8];
    char out[1024], what[200];

    ho = host_opts("");
    ho.bios_lo = lo;
    ho.bios_hi = hi;
    ho.bios_id = bios_id;
    ho.cart_sync = false;
    chfdebug_power_down(d);
    chf_host_stop();
    chfdebug_attach(d);
    snprintf(what, sizeof what, "%s: the app boots, stopped at reset", label);
    if (!boot(d, &ho)) {
        check(0, what);
        return;
    }
    check(1, what);
    if (chf_host_missing(out, sizeof out)) {
        printf("note: %s cannot run this app here (%s): skipped\n", label, out);
        return;
    }
    c = cpu(d);
    snprintf(what, sizeof what, "%s: the CPU view names the BIOS", label);
    check(c.bios == bios_id, what);
    check(run_to_exec(d, A_ENTRY), "run to the cartridge's entry");
    c = cpu(d);
    check(c.r[59] == 0x28 && c.k_depth == 0, "the BIOS leaves r59 = $28: the K stack is empty (depth 0)");
    show("r59 %02X depth %d", c.r[59], c.k_depth);
    chfdebug_command(d, "bios", out, sizeof out);
    check(strstr(out, bios_id < 0 ? "stand-in" : "imported") != NULL, "the prompt's bios says which");

    chfdebug_cpu_set(d, CHF_REG_PC, A_KBLK);
    chfdebug_command(d, "step 4", out, sizeof out);
    check(wait_stopped(d, 1, 3000) && cpu(d).pc == A_KBLK + 6 && cpu(d).j_h_k_q[2] == 0x1234, "K = $1234");
    chfdebug_disassemble(d, A_KBLK + 6, l, 1, NULL);
    check(strncmp(l[0].disasm, "PI", 2) == 0 && strcmp(l[0].comment, "PUSHK") == 0, "PI $0107 is labelled PUSHK");
    c = cpu(d);
    chfdebug_step_over(d);
    check(wait_stopped(d, 1, 5000), "step over PUSHK stops");
    {
        chfdebug_cpu c2 = cpu(d);
        check(c2.pc == A_KBLK + 9 && c2.k_depth == 1 && c2.k_stack[0] == 0x1234 && c2.r[59] == 0x2a &&
              strcmp((why(d), reason), "step over") == 0, "step over a PUSHK stops after it, one entry deeper: [$1234]");
        show("pc %04X depth %d r59 %02X reason '%s'", c2.pc, c2.k_depth, c2.r[59], reason);
        (void)c;
    }
    chfdebug_command(d, "step 4", out, sizeof out);
    check(wait_stopped(d, 1, 3000) && cpu(d).pc == A_KBLK + 0x0f && cpu(d).j_h_k_q[2] == 0x5678, "K = $5678");
    chfdebug_step_over(d);
    check(wait_stopped(d, 1, 5000) && cpu(d).k_depth == 2 && cpu(d).k_stack[0] == 0x1234 && cpu(d).k_stack[1] == 0x5678,
          "a second PUSHK: [$1234, $5678]");
    chfdebug_command(d, "kstack", out, sizeof out);
    check(strstr(out, "depth 2") && strstr(out, "1234") && strstr(out, "5678"), "the prompt's kstack");
    chfdebug_disassemble(d, A_KBLK + 0x12, l, 1, NULL);
    check(strcmp(l[0].comment, "POPK") == 0, "PI $011E is labelled POPK");
    chfdebug_step_over(d);
    check(wait_stopped(d, 1, 5000) && cpu(d).k_depth == 1 && cpu(d).j_h_k_q[2] == 0x5678, "step over POPK: back to one entry, K = $5678");
    chfdebug_step_over(d);
    check(wait_stopped(d, 1, 5000) && cpu(d).k_depth == 0 && cpu(d).j_h_k_q[2] == 0x1234 && cpu(d).pc == A_KBLK + 0x18,
          "and to none, K = $1234");

    /* into PUSHK and out of it */
    chfdebug_cpu_set(d, CHF_REG_PC, A_KBLK + 0x0f);
    chfdebug_step(d);
    check(wait_stopped(d, 1, 3000) && cpu(d).pc == 0x0107 && cpu(d).pc1 == A_KBLK + 0x12, "a step into PI $0107 is in PUSHK, PC1 the return");
    chfdebug_step_out(d);
    check(wait_stopped(d, 1, 5000) && cpu(d).pc == A_KBLK + 0x12 && strcmp((why(d), reason), "step out") == 0, "step out of PUSHK returns after the call");

    /* a routine that saves its return address on the K stack and leaves through K:
     * `PI POPK / PK` -- the PK is the return, not a call */
    chfdebug_write(d, CHF_SPACE_SCRATCH, 59, 0x28);               /* an empty K stack again */
    chfdebug_cpu_set(d, CHF_REG_PC, 0x0a20);
    chfdebug_step(d);
    check(wait_stopped(d, 1, 3000) && cpu(d).pc == 0x0a00 && cpu(d).pc1 == 0x0a23, "into sub4");
    chfdebug_step(d);                                          /* LR K,P: K = the return address */
    check(wait_stopped(d, 1, 3000), "LR K,P");
    chfdebug_step_over(d);                                     /* PUSHK */
    check(wait_stopped(d, 1, 5000) && cpu(d).pc == 0x0a04 && cpu(d).k_depth == 1 && cpu(d).k_stack[0] == 0x0a23,
          "sub4 has pushed its return address: depth 1, [$0A23]");
    show("pc %04X depth %d [%04X] r59 %02X K %04X pc1 %04X", cpu(d).pc, cpu(d).k_depth, cpu(d).k_stack[0], cpu(d).r[59],
         cpu(d).j_h_k_q[2], cpu(d).pc1);
    chfdebug_step_out(d);
    check(wait_stopped(d, 1, 5000) && cpu(d).pc == 0x0a23 && cpu(d).k_depth == 0 && strcmp((why(d), reason), "step out") == 0,
          "step out of it: the PK that goes home through K, with the K stack popped, is the return");
    show("pc %04X depth %d reason '%s'", cpu(d).pc, cpu(d).k_depth, reason);
    chfdebug_cpu_set(d, CHF_REG_PC, A_IDLE);
}

/* CONFIG on the app's stand-in BIOS, with the real transport to the loopback
 * FujiNet. The stand-in is a clean-room piece that lands separately: until it
 * can run CONFIG this reports and carries on. */
static void test_config(chfdebug *d, const char *boip, int fake_up)
{
    static chfdebug_fuji_packet pk[FUJITRACE_MAX];
    chf_host_opts_t ho;
    chfdebug_line l[64];
    char why_[160];
    int n, found = 0, i;

    memset(&ho, 0, sizeof ho);
    ho.machine = CHF_MACHINE_CHANNELF;
    ho.bios_id = -1;
    ho.boip_hostport = boip[0] ? boip : NULL;
    chfdebug_power_down(d);
    chf_host_stop();
    if (chf_host_start(&ho) != 0) {
        printf("note: CONFIG does not start (%s): skipped\n", chf_host_last_error());
        return;
    }
    test_sleep_ms(400);
    if (chf_host_missing(why_, sizeof why_) || chf_host_frame_count() < 5) {
        printf("note: the stand-in BIOS cannot run CONFIG here (%s): that part is skipped\n", why_[0] ? why_ : "no frames");
        chf_host_stop();
        return;
    }
    chfdebug_power_up(d);
    check(chfdebug_is_attached(d) && stop_now(d), "CONFIG on the stand-in BIOS: attach and stop");
    check(cpu(d).bios == -1, "the CPU view says the stand-in is the BIOS");
    chfdebug_command(d, "bios", why_, sizeof why_);
    check(strstr(why_, "stand-in") != NULL, "and so does the prompt");
    for (int base = 0x0802; base < 0x1000 && !found; base += 40) {
        n = chfdebug_disassemble(d, (uint16_t)base, l, 40, NULL);
        for (i = 0; i < n; i++)
            if (strcmp(l[i].comment, "CLRSCRN") == 0 || strcmp(l[i].comment, "PUSHK") == 0 || strcmp(l[i].comment, "POPK") == 0)
                found = 1;
    }
    check(found, "CONFIG's code calls a BIOS entry point, and the disassembly names it");
    if (fake_up) {
        int waited = 0;
        chfdebug_fujinet_trace_clear(d);
        chfdebug_resume(d);
        while (waited < 10000 && chfdebug_fujinet_trace(d, pk, FUJITRACE_MAX) < 2) {
            test_sleep_ms(50);
            waited += 50;
        }
        n = chfdebug_fujinet_trace(d, pk, FUJITRACE_MAX);
        check(n >= 2 && !pk[0].to_host && pk[0].device >= 0, "CONFIG talks to FujiNet: its first packets are in the trace");
        if (n >= 1)
            show("'%s'", pk[0].text);
        stop_now(d);
    }
}

int main(void)
{
    char tmp[512], boip[64] = "";
    chf_host_opts_t ho;
    chfdebug *d;
    int fresh, fake_up = 0;
#ifndef _WIN32
    fake_fujinet fake;
    pthread_t fake_th;
#endif

    setvbuf(stdout, NULL, _IOLBF, 0);
    test_tmpdir(tmp, sizeof tmp, "dbg");
    build_app();

    d = chfdebug_create();
    check(d != NULL, "an engine");
    if (!d) return 1;
    test_no_machine(d);

    /* ---- the FujiNet trace is installed first, from session start ---- */
    chfdebug_fujinet_install(d);
#ifndef _WIN32
    {
        struct sockaddr_in a;
        socklen_t al = sizeof a;
        int one = 1;

        memset(&fake, 0, sizeof fake);
        fake.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        setsockopt(fake.listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (fake.listen_fd >= 0 && bind(fake.listen_fd, (struct sockaddr *)&a, sizeof a) == 0 &&
            listen(fake.listen_fd, 1) == 0 &&
            getsockname(fake.listen_fd, (struct sockaddr *)&a, &al) == 0 &&
            pthread_create(&fake_th, NULL, fake_main, &fake) == 0) {
            fake.port = ntohs(a.sin_port);
            snprintf(boip, sizeof boip, "127.0.0.1:%d", fake.port);
            fake_up = 1;
        }
    }
#endif

    /* ---- attached before the machine exists: it stops on its first instruction ---- */
    chfdebug_attach(d);
    check(chfdebug_is_attached(d), "attached with no machine to hook yet");
    check(chfdebug_breakpoint_add(d, CHFDEBUG_BP_EXEC, CHF_SPACE_PROG, A_ENTRY, A_ENTRY, "a == 8 && pc == $802") > 0 &&
          chfdebug_breakpoint_add(d, CHFDEBUG_BP_OUT, CHF_SPACE_IO, 5, 5, "value == $40") > 0,
          "an execute and an OUT breakpoint, set before the machine");
    ho = host_opts(boip);
    fresh = boot(d, &ho);
    check(fresh, "the new machine stops in the debugger on its first instruction");
    if (!fresh) {
        chfdebug_cpu c = cpu(d);
        printf("      stopped %d pc %04X instr %llu attached %d\n", chfdebug_is_stopped(d), c.pc,
               (unsigned long long)c.instructions, chfdebug_is_attached(d));
        return 1;
    }
    test_program_run(d);
    test_logs(d);
    test_video(d);
    test_cpu(d);
    test_disasm(d);
    test_memory(d);
    test_stepping(d);
    test_cartridge(d, fake_up);
    test_symbols_and_save(d, tmp);
    test_prompt(d, tmp);
    test_completions(d);
    test_detach_and_cycle(d, &ho);
    test_videocarts(d, tmp);
    test_bios_services(d, "stand-in BIOS", NULL, NULL, -1);
    {
        /* the console's own BIOS, when the user's copy is at hand (CHF_BIOS_DIR) */
        static uint8_t lo[CF_BIOS_HALF], hi[CF_BIOS_HALF];
        const char *dir = getenv("CHF_BIOS_DIR");
        char p1[800], p2[800];
        FILE *f1, *f2;

        snprintf(p1, sizeof p1, "%s/sl31253.rom", dir ? dir : "");
        snprintf(p2, sizeof p2, "%s/sl31254.rom", dir ? dir : "");
        f1 = dir && *dir ? fopen(p1, "rb") : NULL;
        f2 = dir && *dir ? fopen(p2, "rb") : NULL;
        if (f1 && f2 && fread(lo, 1, sizeof lo, f1) == sizeof lo && fread(hi, 1, sizeof hi, f2) == sizeof hi)
            test_bios_services(d, "the console's BIOS", lo, hi, CHF_ROM_SL31253);
        else
            printf("note: no real BIOS (CHF_BIOS_DIR)\n");
        if (f1) fclose(f1);
        if (f2) fclose(f2);
    }
    test_config(d, boip, fake_up);

    chfdebug_detach(d);
    chf_host_stop();
#ifndef _WIN32
    if (fake_up) {
        atomic_store(&fake.stop, 1);
        shutdown(fake.listen_fd, SHUT_RDWR);
        close(fake.listen_fd);
        pthread_join(fake_th, NULL);
    }
#endif
    chfdebug_free(d);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
