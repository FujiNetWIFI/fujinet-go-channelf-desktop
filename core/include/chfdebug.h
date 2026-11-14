/*
 * chfdebug -- the debugger contract: a Fairchild F8 (F3850) / Channel F video
 * debugger engine over the core (core/debugger/) behind a C API the four
 * native debugger windows share. Same shape as the Arcadia, Odyssey 2 and
 * Studio II siblings' debug headers, so the windows carry over; this is that
 * engine with the F8, the Channel F's VRAM and its four I/O ports, and the
 * FujiNet cartridge's mailbox at $F800-$FFFF.
 *
 * Frontends consume ONLY this header (and chfsession.h, which it includes):
 * everything the windows show -- CPU, scratchpad, memory, disassembly, the
 * VRAM and its palettes, the ports, the cartridge, the FujiNet packet trace --
 * comes through the calls below.
 *
 * The engine only engages while a window holds it attached:
 * chfdebug_attach() when the window opens -- which also stops the machine, as
 * on every sibling -- and chfdebug_detach() when it closes, which lets the
 * machine run on at full speed with no hook installed. The FujiNet trace is
 * the exception: it records from session start.
 *
 * Calls are safe from the UI thread at any time. Inspection is meant for the
 * stopped state; while running it answers with whatever is current.
 *
 * ---- the machine, in one paragraph ----
 * The F3850 CPU has an accumulator A, a status register W (ICB O Z C S), a
 * 6-bit ISAR addressing its 64-byte scratchpad (r9 = J, r10/r11 = H, r12/r13
 * = K, r14/r15 = Q; only r0-r11 are directly addressable) and NO address
 * registers of its own: every chip on the bus (the console's 3851 PSUs and the
 * cartridge) keeps its own PC0, PC1 (the one return address), DC0 and DC1 and
 * updates them from the 5-bit ROMC code each cycle. Program space is 64K:
 * the BIOS at $0000-$07FF, the cartridge from $0800. There is no RAM in the
 * console and the CPU cannot read the video RAM: it writes a pixel by putting
 * the colour on port 1, the column on port 4, the row on port 5 (which also
 * carries the sound bits) and strobing port 0. The FujiNet cartridge is a
 * bus peer that serves write cycles, so it gives the console 30K of RAM at
 * $8000 and its mailbox at $F800-$FFFF.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CHFDEBUG_H
#define CHFDEBUG_H

#include <stddef.h>
#include <stdint.h>

#include "chfsession.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Lazily created; lives as long as the session. */
chfdebug *chfdebug_get(chfsession *s);

/* ---- attach / stop / go ---------------------------------------------------- */
/* Engage the engine and stop at the next instruction. Idempotent. */
void chfdebug_attach(chfdebug *d);
/* Disengage and let the machine run. Breakpoints are kept for the next
 * attach. */
void chfdebug_detach(chfdebug *d);
int  chfdebug_is_attached(chfdebug *d);

int  chfdebug_is_stopped(chfdebug *d);
void chfdebug_stop(chfdebug *d);         /* break at the next instruction */
void chfdebug_resume(chfdebug *d);       /* "run" */
/* Why the machine last stopped ("stopped at $0802", "breakpoint: exec $0802",
 * "wrote $8010 = $12 (was $11) at $0A32", "VRAM write row 12 col 40 = 2 (was 0)
 * at $082E", "out port 5 = $C0 (was $00) at $0837", "scratch write r20 = $02
 * (was $01) at $0864", "step at $0803", "frame at $0860", ...) and the PC the
 * machine stopped at, -1 if none or running. A write names its writer (the
 * instruction before the stop) and the value it replaced; a reason without an
 * address of its own ends " at $xxxx" (the PC). chfdebug_is_stopped() turns
 * true only once the reason of that stop is recorded, so a poller that sees
 * it stopped reads the right one. Writes at most dstsz-1 bytes; returns the
 * length. */
int  chfdebug_stop_reason(chfdebug *d, char *dst, int dstsz, int *address);
/* Bumped by every stop (pause, breakpoint, step), after its reason is
 * recorded and before chfdebug_is_stopped() turns true; frontends recentre
 * their views on the PC when it changes. */
uint64_t chfdebug_stop_serial(chfdebug *d);
/* Bumped whenever a command ran or the machine stopped/resumed or something
 * was edited, so a window knows when to refresh. */
unsigned chfdebug_generation(chfdebug *d);

/* ---- the prompt ------------------------------------------------------------ */
/* Run one command; "help" lists them: step [n], over, out, frame [n],
 * scanline [n], run, stop, runto <addr>, break <addr>[-<end>] [cond],
 * bpr/bpw <addr>[-<end>] [cond] (data reads/writes of PROG space), bpi/bpo
 * <port>[-<end>] [cond] (port reads/writes), bpa <addr>[-<end>] [cond] (a
 * PROG read or write: Access), bpv <row>,<col>[-<row>,<col>]
 * [cond] (VRAM writes to a cell range), bps <reg>[-<end>] [cond] (scratchpad
 * writes), delete <id|addr>, enable/disable <id>, clear, breaks, print
 * <expr>, regs, set <reg> <expr>, mem <space> <addr> [len] (prog, scratch,
 * vram, cart, io), poke <space> <addr> <val...>, scratch, isar, kstack, vram
 * [row], pal, video, ports, sound, input, cart, mailbox, bios, romc
 * [on|off|n], label <addr> <name>, labels [file], disasm [addr] [n], trace
 * [on|off|clear|n], watch [[space] expr|low-high], unwatch <n|all>, find [space]
 * [hex] <bytes> | find [space] text <string>, save <kind> <file>.
 * Expressions: numbers ($0800, 0x800, 800h, 2048, %1010, o17 octal),
 * registers (a w is isar pc pc0 pc1 dc0 dc1 j h k q hu hl ku kl qu ql, the
 * flags icb o z c s, r0-r63), [addr] a PROG byte, p[addr] s[reg] v[row*128+col]
 * c[off] io[port] the five spaces, labels, vpos hpos frame cycles, in a
 * breakpoint condition `value`, `addr` and `old` (the value before a write;
 * the same as `value` for a read, an IN or an execute) (and `row` `col` for
 * bpv, `port` for bpi/bpo); C operators with C precedence. Output text into dst; returns
 * its length. */
int  chfdebug_command(chfdebug *d, const char *command, char *dst, int dstsz);
/* Completions for a prefix (commands, registers and labels), one per line.
 * Returns the count. */
int  chfdebug_completions(chfdebug *d, const char *prefix, char *dst, int dstsz);

/* ---- stepping shortcuts (the toolbar) -------------------------------------- */
/* All of these act only while stopped. (The Channel F never interrupts: no
 * chip on its bus drives /INTREQ.) */
void chfdebug_step(chfdebug *d);          /* F7: one instruction */
/* F8: over a PI or a PK (run until the instruction after it is reached again
 * with the K stack -- r59 -- where it was); any other instruction is a plain
 * step. */
void chfdebug_step_over(chfdebug *d);
/* Shift+F8: until the routine returns: a POP (PC0 = PC1), or a PK/LR P0,Q
 * that leaves it, at the depth it was called from (PI and PK count one
 * deeper, POP one shallower). */
void chfdebug_step_out(chfdebug *d);
void chfdebug_scanline(chfdebug *d, int n);   /* until the beam has moved n lines */
void chfdebug_frame(chfdebug *d, int n);      /* until n more frames are complete */
/* Run until PC == addr. */
void chfdebug_run_to(chfdebug *d, uint16_t addr);

/* ---- CPU --------------------------------------------------------------------- */
typedef struct {
    int pc;                 /* the instruction about to run (the CPU has already
                               fetched its opcode: PC0 = pc + 1) */
    uint16_t pc0, pc1;      /* the program counter as the bus sees it, and the
                               one return address (PI copies PC0 to PC1, POP back) */
    uint16_t dc0, dc1;      /* the data counter and its alternate (XDC swaps) */
    uint8_t a;              /* the accumulator */
    uint8_t w;              /* status: bit 0 S (sign: 1 = POSITIVE), 1 C, 2 Z,
                               3 O, 4 ICB (interrupts enabled) */
    int sign, carry, zero, overflow, icb;   /* W decoded (sign = 1: positive) */
    uint8_t isar;           /* 6 bits; shown in octal as upper.lower */
    uint8_t r[64];          /* the scratchpad */
    uint16_t j_h_k_q[4];    /* r9 (J, 8 bits), H = r10:r11, K = r12:r13, Q = r14:r15 */
    uint8_t dbus;           /* the data bus: the opcode just fetched */
    int k_depth;            /* the BIOS K stack's depth, (r59 - 40) / 2; -1 when r59
                               is not a K-stack pointer (odd, or outside 40-58) */
    uint16_t k_stack[10];   /* its saved returns, oldest first (k_depth of them) */
    uint64_t cycles;        /* CPU clocks since power-on (4 per short cycle, 6 per long) */
    uint64_t instructions;
    unsigned illegal_count; /* illegal opcodes executed */
    int illegal_pc;         /* the last one's address, -1 if none */
    int vpos, hpos;         /* where the beam is: the line, and the CPU cycle in it */
    uint32_t frame;
    int machine;            /* CHF_MACHINE_* */
    int bios;               /* CHF_ROM_SL31253 / CHF_ROM_SL90025, -1 the stand-in */
} chfdebug_cpu;
void chfdebug_cpu_get(chfdebug *d, chfdebug_cpu *out);
/* Registers for chfdebug_cpu_set() and the prompt's `set`. */
typedef enum {
    CHF_REG_A = 0, CHF_REG_W, CHF_REG_ISAR,
    CHF_REG_PC, CHF_REG_PC0, CHF_REG_PC1, CHF_REG_DC0, CHF_REG_DC1,
    CHF_REG_J, CHF_REG_H, CHF_REG_K, CHF_REG_Q,
    CHF_REG_SIGN, CHF_REG_CARRY, CHF_REG_ZERO, CHF_REG_OVERFLOW, CHF_REG_ICB,
    CHF_REG_R0,                                  /* ... CHF_REG_R0 + 63: the scratchpad */
    CHF_REG_COUNT = CHF_REG_R0 + 64
} chfdebug_reg;
/* Only while stopped. Setting PC (or PC0) refetches the opcode there. */
void chfdebug_cpu_set(chfdebug *d, int reg, int value);

/* ---- memory ------------------------------------------------------------------- */
/* The five address spaces. Reads have no side effects (no mailbox hotspot
 * fires). */
typedef enum {
    CHF_SPACE_PROG = 0,   /* the CPU's view, $0000-$FFFF, as a read would see it */
    CHF_SPACE_SCRATCH,    /* the 64-byte scratchpad, r0-r63 */
    CHF_SPACE_VRAM,       /* the video RAM, one byte per cell (0-3) at row * 128 + col,
                             8192 bytes */
    CHF_SPACE_CART,       /* the cartridge's own memory: the FujiNet cart's live image
                             (the 16K window then its APPBANK pages, 64K) then its
                             32K arena ($8000-$FFFF as the console sees it); a
                             Videocart's ROM then its RAM (chess/multi 2K, the 2102's
                             1024 bits as 128 bytes) */
    CHF_SPACE_IO          /* the ports as the CPU would read them now, 0-255 */
} chfdebug_space;
/* Size of a space in bytes (the CART space's depends on the cartridge). */
int  chfdebug_space_size(chfdebug *d, int space);
/* Fill dst with n bytes from addr; returns n, or 0 when the machine is not
 * running. Addresses past a space's end read $FF. */
int  chfdebug_read(chfdebug *d, int space, uint32_t addr, uint8_t *dst, int n);
/* A debugger edit. PROG: the FujiNet cart's RAM and image, a Videocart's RAM;
 * the BIOS and ROM are not writable (ignored), nor are the mailbox's hotspots
 * (an edit never fires one). SCRATCH: the register. VRAM: the cell (value &
 * 3; it shows in the next frame). CART: the cartridge's memory. IO: the port's
 * output latch, with the write's effects (port 0's strobe draws a pixel,
 * port 5 changes the sound), as if the CPU had written it. */
void chfdebug_write(chfdebug *d, int space, uint32_t addr, uint8_t value);

/* ---- disassembly -------------------------------------------------------------- */
typedef struct {
    uint16_t address;
    int is_pc;
    int has_breakpoint;      /* an enabled execute breakpoint covers it */
    int bp_disabled;         /* only a disabled one does (the grey dot) */
    int length;              /* 1-3 */
    char bytes[16];          /* "29 08 02" */
    char label[48];
    char disasm[64];         /* MAME's text: "JMP   $0802", "LR    A,KU" */
    char comment[64];        /* a branch target's label */
} chfdebug_line;
/* Up to `max` lines starting at `addr` (PROG space; $4800-$67FF shows the
 * APPBANK page in the window, tagged in the comment). Returns the count;
 * *pc_line is the index of the PC's line in `out`, -1 when not among them. */
int  chfdebug_disassemble(chfdebug *d, uint16_t addr, chfdebug_line *out,
                              int max, int *pc_line);
/* The address `rows` disassembly rows before (negative) or after `addr` --
 * for scrolling, and for "Follow PC" (PC a third of the way down). Backwards
 * is a best guess. */
int  chfdebug_row_address(chfdebug *d, uint16_t addr, int rows);
/* Address of a label, or -1. Label of an address into dst (may be empty). */
int  chfdebug_label_address(chfdebug *d, const char *label);
int  chfdebug_address_label(chfdebug *d, uint16_t addr, char *dst, int dstsz);
int  chfdebug_set_label(chfdebug *d, uint16_t addr, const char *label);
/* Load a symbol file: Macroassembler AS (-g MAP's .map, or the -L listing's
 * symbol table), or a plain "AAAA name" list. With path NULL,
 * <cart>.map/.lst next to the opened cartridge. Built-in labels: the BIOS's
 * entry points (RESET $0000, CLRSCRN $00D0, PUSHK $0107, POPK $011E, and the
 * others MAME's disassembly of the real BIOS names when one is in use), the
 * cartridge's CART_SIG $0800 / CART_ENTRY $0802 / FUJI_CLAIM $47FC /
 * APPBANK_WIN $4800, a Videocart's RAM ($2800, the bank latch $3000), and the
 * FujiNet mailbox (CART_RAM $8000, REPLY $F800, the $FC00 status page:
 * ACKSEQ STATUS ERR REPLY_CMD RXLEN BOOT_STATE BOOT_PCT BOOT_ERR MAGIC
 * PROTO_VER TICK, REGSEL $FD00, APPBANK_SEL $FD80, SWAP $FDFE, REGDATA
 * $FE00, TXDATA $FF00). Message into msg; returns labels loaded or -1. */
int  chfdebug_load_symbols(chfdebug *d, const char *path, char *msg, int msgsz);

/* ---- video: the VRAM, its palettes and the four ports ---------------------------
 *
 * The Channel F's picture is a 128 x 64 VRAM of 2-bit values. The CPU writes
 * one cell at a time: port 1 bits 7-6 the value (complemented), port 4 bits
 * 6-0 the column (complemented), port 5 bits 5-0 the row (complemented; bits
 * 7-6 are the sound), then a strobe on port 0 (bit 5; the BIOS writes $60
 * then $50, MAME takes any write with bit 5 set). Each ROW picks its own
 * palette from the values in columns 125 and 126: palette =
 * (v[126] & 2) | (v[125] >> 1) -- 0 black and white, 1 light blue, 2 light
 * grey, 3 light green backgrounds, the other three values blue, red and green.
 * The TV shows columns 4-105 and rows 4-61; the rest is hidden, and programs
 * keep data there. The picture is drawn from the whole VRAM at the vertical
 * blank (MAME's screen update), so a frame shows the VRAM as it was then;
 * the BEAM view shows instead what a CRT would, row by row as the beam
 * passed it.
 *
 * Every view is an XRGB image; `scale` multiplies it (1 and up). */

#define CHFDEBUG_VRAM_COLS 128
#define CHFDEBUG_VRAM_ROWS 64
#define CHFDEBUG_VIS_COL0  4          /* the visible area: columns 4-105 */
#define CHFDEBUG_VIS_COLS  102
#define CHFDEBUG_VIS_ROW0  4          /* rows 4-61 */
#define CHFDEBUG_VIS_ROWS  58
#define CHFDEBUG_PAL_COL_A 125        /* the palette columns */
#define CHFDEBUG_PAL_COL_B 126

typedef struct {
    /* the ports as the CPU last wrote them (the output latches, raw) */
    uint8_t port0, port1, port4, port5;
    /* ... decoded */
    int value;                /* the colour value the next strobe writes: (~port1 >> 6) & 3 */
    int col;                  /* ~port4 & $7F */
    int row;                  /* ~port5 & $3F */
    int sound_mode;           /* port5 >> 6: 0 off, 1 1 kHz, 2 500 Hz, 3 120 Hz */
    int strobe;               /* port 0 bit 5: the write strobe is up now */
    int controllers_enabled;  /* port 0 bit 6 clear: the hand controllers are read */
    uint8_t panel_latch;      /* port 0 bits 0-3 */
    /* the ports as the CPU would read them now (~external | latch) */
    uint8_t in0, in1, in4, in5;
    /* per row: the palette (0-3) from columns 125/126, and its background RGB */
    uint8_t row_palette[CHFDEBUG_VRAM_ROWS];
    uint32_t row_background[CHFDEBUG_VRAM_ROWS];
    int palette_rows[4];      /* how many of the 64 rows use each palette */
    /* timing */
    int line, lines;          /* the beam line now, and lines per frame (264 / 312) */
    int cycle_in_line, cycles_per_line;
    uint32_t frame;
    uint32_t strobes_frame;   /* VRAM writes this frame so far, and in the last whole one */
    uint32_t strobes_last;
    uint32_t changes_last;    /* ... of those, writes that changed a cell */
    int is_pal;
} chfdebug_video;
void chfdebug_video_get(chfdebug *d, chfdebug_video *out);

/* The eight colours and the four palettes (MAME's pens, XRGB). */
uint32_t chfdebug_colour_rgb(int pen);            /* pen 0-7 */
const char *chfdebug_colour_name(int pen);         /* "black", "white", "red", "green",
                                                      "blue", "light grey",
                                                      "light green", "light blue" */
int chfdebug_palette_pen(int palette, int value);  /* palette 0-3, value 0-3 -> pen 0-7 */
const char *chfdebug_palette_name(int palette);    /* "black & white", "light blue", ... */

typedef enum {
    CHFDEBUG_VIEW_FRAME = 0,  /* the last frame the machine finished (what the display shows):
                                 CHFSESSION_FB_WIDTH x CHFSESSION_FB_HEIGHT */
    CHFDEBUG_VIEW_LIVE,       /* the VRAM as it is now, drawn the same way */
    CHFDEBUG_VIEW_BEAM,       /* what a CRT shows: rows the beam has passed this frame from
                                 the VRAM as it was when the beam drew them, the rest from
                                 the last frame, dimmed */
    CHFDEBUG_VIEW_VRAM,       /* the whole 128 x 64 VRAM, 2 x 4 pixels a cell (256 x 256),
                                 the hidden columns and rows hatched */
    CHFDEBUG_VIEW_VALUES,     /* the whole VRAM's raw values, one false colour each
                                 (0 dark grey, 1 blue, 2 red, 3 green), palette-free */
    CHFDEBUG_VIEW_PALETTES    /* the four palettes, a row of four swatches each */
} chfdebug_view_kind;
/* Overlays, drawn on the picture before it is scaled. */
#define CHFDEBUG_F_VISIBLE  0x001u    /* the visible area's outline (VRAM and VALUES) */
#define CHFDEBUG_F_GRID     0x002u    /* cell borders (needs scale >= 2 to be seen) */
#define CHFDEBUG_F_PALCOLS  0x004u    /* mark columns 125-127 (VRAM and VALUES) */
#define CHFDEBUG_F_BEAM     0x008u    /* the beam's line */
#define CHFDEBUG_F_CURSOR   0x010u    /* the cell the next strobe writes (port 4/5) */
#define CHFDEBUG_F_HEAT     0x020u    /* cells written this frame, brighter the more */
#define CHFDEBUG_F_DIRTY    0x040u    /* cells that changed since the last frame */
#define CHFDEBUG_F_PALROWS  0x080u    /* a tick at the left of rows whose palette changed
                                         this frame */
#define CHFDEBUG_VIEW_MAX_W 256
#define CHFDEBUG_VIEW_MAX_H 256
#define CHFDEBUG_VIEW_MAX_PIXELS (CHFDEBUG_VIEW_MAX_W * CHFDEBUG_VIEW_MAX_H)
/* A view as an XRGB image; ends with the pixel size in *w,*h (dst NULL asks
 * for the size only). Size dst by CHFDEBUG_VIEW_MAX_PIXELS * scale * scale.
 * Returns 1 when drawn. */
int  chfdebug_video_view(chfdebug *d, int view, unsigned flags, int scale,
                         uint32_t *dst, size_t cap, int *w, int *h);
/* The VRAM cell under pixel (x, y) of a view drawn at `scale`, or -1 (not on
 * a cell, or a view with none). */
int  chfdebug_video_hit(chfdebug *d, int view, int x, int y, int scale, int *row, int *col);

/* One cell, for the hover and pin readout. */
typedef struct {
    int row, col;
    int visible;              /* inside columns 4-105 and rows 4-61 */
    const char *hidden_why;   /* "palette column", "left border", ... or NULL */
    int value;                /* 0-3 */
    int palette;              /* the row's */
    int pen;                  /* chfdebug_palette_pen(palette, value) */
    uint32_t rgb;
    uint32_t writes;          /* strobes that hit it since the engine attached */
    int last_pc;              /* the last writer's PC, -1 if never written */
    uint32_t last_frame;
    int last_line;
    uint64_t last_cycle;
    int last_old;             /* the value before that write */
} chfdebug_vram_cell;
int  chfdebug_vram_cell_get(chfdebug *d, int row, int col, chfdebug_vram_cell *out);

/* The VRAM write log: every strobe while attached, newest last. */
typedef struct {
    uint64_t cycle;
    uint32_t frame;
    int line;
    uint16_t pc;
    uint8_t row, col, value, old;
} chfdebug_vram_write;
typedef struct {
    int row0, row1, col0, col1;   /* inclusive; 0, 63, 0, 127 for all */
    int changes_only;             /* only writes that changed the cell */
    int this_frame;               /* only this frame's */
    int palette_cols;             /* only columns 125-127 */
} chfdebug_vram_filter;
/* The newest `max` matching (filter NULL: all), oldest first; returns the
 * count. */
int  chfdebug_vram_writes(chfdebug *d, const chfdebug_vram_filter *filter,
                          chfdebug_vram_write *out, int max);
void chfdebug_vram_writes_clear(chfdebug *d);

/* ---- inputs, sound and the console ---------------------------------------------- */
typedef struct {
    int machine;                         /* CHF_MACHINE_* */
    int is_pal;
    uint8_t controller[2];               /* chf_action bits held, as latched at the frame
                                            ([0] player 1 = right = port 1, [1] player 2 =
                                            left = port 4; 1 = held) */
    uint8_t panel;                       /* the console buttons latched, bit 0 TIME .. 3 START */
    int reset_held;                      /* the console's RESET */
    /* sound */
    int sound_mode;                      /* 0 off, 1 1 kHz, 2 500 Hz, 3 120 Hz */
    int sound_level;                     /* the envelope now, 0-32767 */
    int sound_forced;                    /* in the ~2 ms forced-on time after a change */
    uint32_t sound_changes;              /* mode changes since power-on */
    uint64_t audio_samples;              /* produced since power-on */
    int cart_type;                       /* enum chf_cart_type (core/chf/carts.h) */
    /* a 2102 Videocart (maze, hangman): its I/O ports and address latch */
    int has_2102;
    int ram_port;                        /* $20 or $24 */
    uint16_t ram_addr;                   /* the 10-bit address latched */
    int ram_bit;                         /* the bit at it */
} chfdebug_io;
void chfdebug_io_get(chfdebug *d, chfdebug_io *out);

/* Sound mode changes (port 5 bits 7-6) while attached, oldest first. */
typedef struct {
    uint64_t cycle;
    uint32_t frame;
    uint16_t pc;
    uint8_t old_mode, mode;
} chfdebug_sound_event;
int  chfdebug_sound_events(chfdebug *d, chfdebug_sound_event *out, int max);

/* Every port access while attached (OUTS/OUT/INS/IN), oldest first. */
typedef struct {
    uint64_t cycle;
    uint32_t frame;
    uint16_t pc;
    uint8_t port, value;
    uint8_t is_write;
} chfdebug_io_event;
int  chfdebug_io_events(chfdebug *d, chfdebug_io_event *out, int max);
void chfdebug_io_events_clear(chfdebug *d);

/* ---- the cartridge ------------------------------------------------------------ */
typedef struct {
    int present;
    int type;                            /* enum chf_cart_type (core/chf/carts.h) */
    const char *type_name;               /* "FujiNet", "Videocart", "Maze (2102)", ... */
    const char *softlist_name;           /* MAME's software list name ("maze"), or NULL */
    const char *description;             /* "Videocart 10: Maze, Jailbreak, Blind-man's-bluff, Trailblazer" */
    /* a Videocart */
    uint32_t rom_size, rom_crc;
    uint32_t ram_size;                   /* 2048 chess/multi, 1024 bits for a 2102, 0 none */
    int bank, half;                      /* multi_old / multi: the bank latch */
    /* the FujiNet cartridge (type FujiNet) */
    int link_up, link_off, mailbox, app_live, booted_game, armed;
    int staged_app, staged_mailbox;
    uint32_t live_size, live_crc, staged_size, staged_crc;
    int swaps;
    int npages, page;                    /* APPBANK pages, and the one at $4800 (-1 none) */
    int boot_state, boot_pct, boot_err;
    uint8_t ackseq, status, last_error, reply_cmd;
    uint16_t rxlen;
    uint16_t tick;                       /* the 60 Hz tick at $FC0E/F */
    uint32_t queue_depth;
    uint8_t status_page[0x10];           /* $FC00-$FC0F as the console reads it */
    uint8_t mailbox_page[0x800];         /* $F800-$FFFF as the console reads it (side-effect free) */
} chfdebug_cart;
void chfdebug_cart_get(chfdebug *d, chfdebug_cart *out);
/* One line: "FujiNet cartridge: CONFIG, link up". */
int  chfdebug_cart_info(chfdebug *d, char *dst, int dstsz);

/* The FujiNet cartridge's events (register writes, TX bytes, page selects,
 * swaps) while attached, oldest first. */
typedef struct {
    uint64_t cycle;
    uint32_t frame;
    uint16_t pc;
    uint8_t kind;                        /* 1 REG, 2 TX, 3 BANK, 4 SWAP */
    uint8_t reg, value;
} chfdebug_cart_event;
int  chfdebug_cart_events(chfdebug *d, chfdebug_cart_event *out, int max);

/* ---- FujiNet: the cartridge's socket, as FujiBus packets ------------------------ */
typedef struct {
    uint64_t serial;
    int      to_host;       /* 1: FujiNet -> Channel F, 0: Channel F -> FujiNet */
    int      device, command, length;   /* from the FujiBus header, -1 if short */
    int      nbytes;
    uint8_t  data[64];      /* the decoded packet's first bytes */
    char     text[96];      /* "Channel F -> FujiNet 70 F4 READ HOST SLOTS (6 bytes)" */
    uint32_t repeat;        /* the same exchange back to back, shown "xN" */
} chfdebug_fuji_packet;
/* Recorded from session start, attached or not. The newest `max`, oldest
 * first; returns the count. */
int  chfdebug_fujinet_trace(chfdebug *d, chfdebug_fuji_packet *out, int max);
void chfdebug_fujinet_trace_clear(chfdebug *d);
/* The cartridge's link and mailbox, as text lines. Returns length. */
int  chfdebug_fujinet_state(chfdebug *d, char *dst, int dstsz);

/* ---- breakpoints -------------------------------------------------------------- */
typedef enum {
    CHFDEBUG_BP_EXEC = 1,    /* the instruction at the address is about to run */
    CHFDEBUG_BP_READ = 2,    /* a data read of PROG space (not an instruction fetch) */
    CHFDEBUG_BP_WRITE = 4,   /* a write to PROG space (ROMC 05: the cart's RAM) */
    CHFDEBUG_BP_IN = 8,      /* a port read; start/end are ports */
    CHFDEBUG_BP_OUT = 16,    /* a port write; start/end are ports */
    CHFDEBUG_BP_VRAM = 32,   /* a VRAM write (a strobe); start/end are row * 128 + col,
                                the cells of the rectangle between them */
    CHFDEBUG_BP_SCRATCH = 64,/* a scratchpad write; start/end are registers 0-63 */
    /* Read and write of PROG space as one entry ("Access"): READ | WRITE */
    CHFDEBUG_BP_ACCESS = 6
} chfdebug_bp_type;
typedef struct {
    int id;
    int type;                /* one chfdebug_bp_type */
    int space;               /* the space start/end are in: PROG, IO, VRAM or SCRATCH */
    uint16_t start, end;
    int enabled;
    unsigned hits;
    char condition[128];     /* may use value, addr, row, col, port, registers, flags,
                                vpos, hpos, frame, labels, p[] s[] v[] c[] io[] */
} chfdebug_breakpoint;
/* Execute breakpoint at addr: added if absent, removed if present. Returns
 * 1 when one is now set. */
int  chfdebug_breakpoint_toggle(chfdebug *d, uint16_t addr);
int  chfdebug_breakpoint_check(chfdebug *d, uint16_t addr);
/* Returns the new id, or -1 if the condition does not parse, the type is
 * invalid or the range is outside its space. */
int  chfdebug_breakpoint_add(chfdebug *d, int type, int space, uint16_t start,
                            uint16_t end, const char *condition);
void chfdebug_breakpoint_remove(chfdebug *d, int id);
void chfdebug_breakpoint_enable(chfdebug *d, int id, int enabled);
int  chfdebug_breakpoint_list(chfdebug *d, chfdebug_breakpoint *out, int max);
void chfdebug_breakpoint_clear(chfdebug *d);
/* Zero every hit count. */
void chfdebug_breakpoint_clear_hits(chfdebug *d);

/* ---- symbols (built-ins and loaded, for a Symbols tab) ------------------------- */
/* The console's port names (SYM_BANK_PORT) are listed too, as ports: their
 * bank is CHFDEBUG_SYM_PORT and addr is the port. They resolve by name and
 * never label a disassembly line. */
#define CHFDEBUG_SYM_ANY  (-1)
#define CHFDEBUG_SYM_PORT (-2)
int  chfdebug_symbols_count(chfdebug *d);
/* Entry `index` (0 .. count-1): address (or port), name and the bank
 * (CHFDEBUG_SYM_ANY, or CHFDEBUG_SYM_PORT for a port). Returns 1, or 0 past
 * the end. The order is the table's; it can change when symbols are loaded,
 * cleared or looked up. */
int  chfdebug_symbols_get(chfdebug *d, int index, uint16_t *addr, char *name, int namesz,
                          int *bank);
/* Indices of the entries whose name contains `text` (case-insensitive; empty
 * matches all), ordered by address then name. Returns the count written. */
int  chfdebug_symbols_filter(chfdebug *d, const char *text, int *out, int max);
/* Drop every loaded symbol and user label; the built-ins stay. */
void chfdebug_clear_symbols(chfdebug *d);

/* ---- the trace ring ----------------------------------------------------------- */
typedef struct {
    uint16_t pc;
    uint8_t a, w, isar;
    uint16_t dc0;
    uint8_t len, bytes[3];
    uint64_t cycles;
    int16_t vpos, hpos;
    uint16_t pc1, dc1;       /* the return address, and the alternate data counter */
    uint8_t k;               /* r59, the BIOS K stack pointer */
    int16_t page;            /* the FujiNet APPBANK page showing at $4800 when pc is in
                                the window ($4800-$67FF), else -1 */
    uint32_t frame;
} chfdebug_trace;
/* The ring keeps the newest 65536; it is allocated on the first enable, has
 * its own lock (a reader never waits for a frame) and records nothing while
 * the machine is parked or the console's RESET is held. */
void chfdebug_trace_enable(chfdebug *d, int on);
int  chfdebug_trace_enabled(chfdebug *d);
/* Newest first; returns entries copied. */
int  chfdebug_trace_read(chfdebug *d, chfdebug_trace *out, int max);
void chfdebug_trace_clear(chfdebug *d);
/* Entries held. */
int  chfdebug_trace_count(chfdebug *d);
/* "0802  JMP   $0A40   A=00 W=11 IS=o25 DC0=8000 DC1=0000 PC1=0000 K=40". Returns
 * the length. */
int  chfdebug_trace_format(const chfdebug_trace *t, char *dst, int dstsz);
enum { CHFDEBUG_TRACE_ALL = 0, CHFDEBUG_TRACE_BRANCHES, CHFDEBUG_TRACE_RANGE };
/* 1 if the entry passes the filter (BRANCHES: jumps, branches, calls, returns
 * and LR P0,Q; RANGE: pc in low..high). */
int  chfdebug_trace_match(const chfdebug_trace *t, int filter, uint16_t low, uint16_t high);

/* ---- memory search and the memory map ----------------------------------------- */
#define CHFDEBUG_SEARCH_NOCASE 1
/* Stopped only (-1 while running). Finds up to max start addresses in
 * from..to of one space (bounding the START address; the match may run past
 * `to`, never past the space's end), where (mem & mask) == (pat & mask) for
 * all patlen bytes. Returns the count. */
int  chfdebug_search(chfdebug *d, int space, const uint8_t *pat, int patlen, uint8_t mask,
                     int flags, uint32_t from, uint32_t to, uint32_t *out, int max);
/* The next match after `from` (before it if backward), wrapping once.
 * Returns 1 and *found, 0 for none, -1 while running. */
int  chfdebug_search_next(chfdebug *d, int space, const uint8_t *pat, int patlen, uint8_t mask,
                          int flags, uint32_t from, int backward, uint32_t *found);

typedef struct {
    uint16_t low, high;
    int readable, writable;
    char name[48];
} chfdebug_region;
/* Ordered, gap-free regions covering PROG $0000-$FFFF as mapped now: the
 * BIOS ($0000-$07FF: its two halves, or one "BIOS stand-in"), the cartridge
 * from $0800 by type (a Videocart's ROM then open bus, chess' 2K of RAM, a
 * multicart's bank window "bank n/N" with its RAM and write-only bank latch;
 * the FujiNet cartridge's ROM window, APPBANK "page n/N", open bus, RAM and
 * the mailbox's REPLY / STATUS / REGSEL / APPBANK_SEL / SWAP / REGDATA /
 * TXDATA pages; its arena is plain RAM when the mailbox is dead). Returns
 * the count (under 24). */
int  chfdebug_memory_map(chfdebug *d, chfdebug_region *out, int max);

/* ---- files -------------------------------------------------------------------- */
/* Save with the path a native file picker supplied. kind: "dis" (PROG space
 * $0000-$47FF as code, as mapped now), "mailbox" (the cart's $F800-$FFFF),
 * "vram" (8192 bytes, one per cell), "scratch" (the 64 registers), "cart"
 * (the cartridge image), "screen" (the frame as a PPM), "vramshot" (the
 * whole VRAM view as a PPM), "regs" (CPU, ports and I/O state as text).
 * Message into msg; returns 0 or -1. */
int  chfdebug_save(chfdebug *d, const char *kind, const char *path,
                       char *msg, int msgsz);

#ifdef __cplusplus
}
#endif

#endif /* CHFDEBUG_H */
