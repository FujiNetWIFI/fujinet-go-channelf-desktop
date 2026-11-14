/*
 * Debugger window (AppKit) over this app's Fairchild F8 (F3850) / Channel F
 * debugger engine, via core/include/chfdebug.h and chfmemtools.h. Mirrors the
 * family's other debuggers (the Studio II, SMS, Astrocade, Apple II and
 * Intellivision macOS ones, and the GTK, Qt and Win32 windows), tab for tab:
 * Console, Disassembly, Memory, Breakpoints, Watches, Cart & I/O, Trace,
 * History, Video, Sound, Symbols, FujiNet. The registers sit in a strip under
 * the tabs (A, W with its S C Z O ICB bits, ISAR in octal, PC0, PC1, DC0, DC1
 * editable while stopped; the K stack depth read-only); any tab can be popped
 * out into a window of its own (Pop Out / Dock).
 *
 * The Memory tab has the five address spaces (PROG, SCRATCH, VRAM, CART, IO).
 * Go To, Find, Poke, the highlights, the watches and Show in Memory all work
 * in the space that is selected. With SCRATCH selected the dump gives way to
 * the 8 x 8 octal scratchpad grid (ISAR's cell outlined, changed cells in the
 * accent, the K stack's band r40-r59 shaded, a click picks a cell to edit)
 * and the BIOS K stack list.
 *
 * Opening the window attaches the engine, which stops the machine; closing
 * it (F12, the close button, the menu) detaches, and the machine runs on.
 *
 * A 100 ms NSTimer polls chfdebug_generation() and refreshes only the tab
 * on show, and only when the generation changed (a stop, a resume, a step, a
 * command, an edit). While the machine runs, that tab's live values refresh
 * twice a second (every fifth tick). After a step the window waits a moment
 * (~40 ms) for the stop to settle before it reads anything back; a longer run
 * (Frame, Step Out) is picked up by the poll when it stops. A new
 * chfdebug_stop_serial() (not a mere refresh) puts the Disassembly back on
 * the PC and the Memory tab on it too, and the status line says why it stopped.
 *
 * Keys work anywhere in the window (a local key monitor, so the console's
 * field takes them too): F5 stop/run, F7 step, F8 step over, Shift+F8 step
 * out, F12 close.
 *
 * CHF_DEBUGGER_TAB picks the tab shown first: an index (0 Console,
 * 1 Disassembly, 2 Memory, 3 Breakpoints, 4 Watches, 5 Cart & I/O, 6 Trace,
 * 7 History, 8 Video, 9 Sound, 10 Symbols, 11 FujiNet) or the start of a tab's
 * name ("cart", "memory"; the old "prompt" for the console, "cpu" and "ram"
 * for Memory, "io" for Cart & I/O). For captures: CHF_WATCH (a comma list; an
 * item may start "scratch:", "vram:", "cart:" or "io:" for another space),
 * CHF_WATCH_BREAK, CHF_MEM_GOTO (the same prefixes), CHF_HISTORY=1 and
 * CHF_POPOUT=<tab>. The Video tab reads CHF_DEBUGGER_VIDEO_*.
 *
 * Highlights: the PC's line is the Channel F yellow with dark ink on it, an
 * enabled breakpoint's marker a red dot and a disabled one's a grey ring,
 * labels and comments grey; the Run button is yellow while stopped.
 *
 * The Video tab (VideoTab.m, over chfvidtab.h) shows the picture in five views
 * with overlays, hover/pin inspection, the ports, the colours, the 64 VRAM
 * rows, the write log and the timing. The FujiNet tab shows every FujiBus
 * packet between the console and FujiNet, both ways, with hex, beside the
 * cartridge's mailbox (chfdebug_fujinet_*).
 *
 * NOT COMPILED OR RUN HERE (no Cocoa SDK on the Linux dev box): ported from
 * the Studio II macOS debugger, itself from the SMS, Astrocade, Apple II and
 * Intellivision ones, which the macOS CI runner compiled; checked on that
 * runner.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DebuggerWindow.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import "../KeyForward.h"
#import "../help/HelpWindow.h"
#import "VideoTab.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "chfdebug.h"
#include "chfmemtools.h"

#define DISASM_WINDOW 48
#define MAX_BPS 64
#define NUM_REGS 7             /* A W ISAR PC0 PC1 DC0 DC1 */
#define NUM_SPACES 5           /* PROG SCRATCH VRAM CART IO */
#define MEM_ROWS 40            /* the hex dump: 16 bytes a row */
#define MEM_BYTES (MEM_ROWS * 16)
#define MAX_REGIONS 64
#define FUJI_PACKETS_MAX 512   /* FujiNet packets shown */
#define IOEV_MAX 300           /* port accesses shown */
#define SNDEV_MAX 200          /* beeper mode changes shown */
#define CARTEV_MAX 200         /* cartridge events shown */
#define TAB_CONSOLE 0          /* the tabs, in order */
#define TAB_DISASM 1
#define TAB_MEMORY 2
#define TAB_BREAKS 3
#define TAB_WATCHES 4
#define TAB_CART 5
#define TAB_TRACE 6
#define TAB_HIST 7
#define TAB_VIDEO 8
#define TAB_SOUND 9
#define TAB_SYMBOLS 10
#define TAB_FUJINET 11
#define NUM_TABS 12
#define HIST_RING 65536        /* the engine's trace ring */
#define HIST_SHOWN 2000        /* the newest matching lines a tab shows */
#define TRACE_TICKS 4          /* 4 x 100 ms: the Trace tab's live refresh */
#define LIVE_TICKS 5           /* 5 x 100 ms: the live refresh while running */
#define SOUND_LEVEL_MAX 32767  /* the envelope's full scale */

static ChfDebuggerWindow *g_debugger;

/* The open panels' filters: a type per extension (a dynamic one for an
 * extension nothing has registered, which still matches by extension). */
static NSArray<UTType *> *typesForExtensions(NSArray<NSString *> *exts)
{
    NSMutableArray<UTType *> *types = [NSMutableArray array];
    for (NSString *ext in exts) {
        UTType *type = [UTType typeWithFilenameExtension:ext];
        if (type) [types addObject:type];
    }
    return types;
}

/* The ink on the accent fill: the Videocarts yellow is bright, so the text on it is dark. */
static NSColor *accentInk(void)
{
    return [NSColor colorWithSRGBRed:((CHFSESSION_ACCENT_DARK_RGB >> 16) & 0xff) / 255.0
                               green:((CHFSESSION_ACCENT_DARK_RGB >> 8) & 0xff) / 255.0
                                blue:(CHFSESSION_ACCENT_DARK_RGB & 0xff) / 255.0
                               alpha:1.0];
}

/* Toggled-on look for a push button: accent bezel and a dark title. Off
 * restores the plain bezel and title. Call again after setting a new title
 * on a button that is on (a plain title drops the attributed one). */
static void setButtonAccent(NSButton *button, BOOL on)
{
    if (!button) return;
    NSString *title = [button.title copy];
    if (on) {
        NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
        para.alignment = NSTextAlignmentCenter;
        button.bezelColor = ChfAccentColor();
        button.attributedTitle = [[NSAttributedString alloc]
            initWithString:title
                attributes:@{
                    NSFontAttributeName: button.font ?: [NSFont systemFontOfSize:[NSFont systemFontSize]],
                    NSForegroundColorAttributeName: accentInk(),
                    NSParagraphStyleAttributeName: para
                }];
    } else {
        button.bezelColor = nil;
        button.title = title;       /* drops the attributed title */
    }
}

/* An enabled breakpoint's marker: the same red in every toolkit's debugger. */
static NSColor *breakpointRed(void)
{
    return [NSColor colorWithSRGBRed:0xE0 / 255.0 green:0x1B / 255.0 blue:0x24 / 255.0 alpha:1.0];
}

/* A monospace text view for rows: a click reports its line, column and click
 * count; the wheel browses by row. The disassembly and the memory dump. */
@interface DasmTextView : NSTextView
@property (nonatomic, copy) void (^onClick)(int line, int column, NSInteger clicks);
@property (nonatomic, copy) void (^onScroll)(int rows);
@end

@implementation DasmTextView {
    CGFloat _wheel;     /* the wheel's fraction not yet scrolled */
}

- (void)mouseDown:(NSEvent *)event
{
    NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    NSUInteger index = [self characterIndexForInsertionAtPoint:point];
    NSString *text = self.string;
    if (index > text.length) index = text.length;
    int line = 0;
    NSUInteger lineStart = 0;
    for (NSUInteger i = 0; i < index; i++)
        if ([text characterAtIndex:i] == '\n') { line++; lineStart = i + 1; }
    if (self.onClick) self.onClick(line, (int)(index - lineStart), event.clickCount);
}

- (void)scrollWheel:(NSEvent *)event
{
    /* a notch is a few rows; a trackpad's pixels are many */
    _wheel += event.hasPreciseScrollingDeltas ? -event.scrollingDeltaY / 8.0 : -event.scrollingDeltaY * 3.0;
    const int rows = (int)_wheel;
    if (rows == 0) return;
    _wheel -= rows;
    if (self.onScroll) self.onScroll(rows);
}
@end

/* The Symbols list: a click reports the clicked line (the wheel scrolls it
 * as any text view does). */
@interface SymbolTextView : NSTextView
@property (nonatomic, copy) void (^onClickLine)(int line);
@end

@implementation SymbolTextView
- (void)mouseDown:(NSEvent *)event
{
    NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    NSUInteger index = [self characterIndexForInsertionAtPoint:point];
    NSString *text = self.string;
    if (index > text.length) index = text.length;
    int line = 0;
    for (NSUInteger i = 0; i < index; i++)
        if ([text characterAtIndex:i] == '\n') line++;
    if (self.onClickLine) self.onClickLine(line);
}
@end

/* ---- the controllers and the panel -------------------------------------------------- */

#define IO_CELL_W  44.0
#define IO_CELL_H  22.0
#define IO_GAP     26.0

/* Both hand controllers' eight contacts (a cell each, filled while held) and
 * the console's panel: the four numbered buttons and RESET. */
@interface ChfIoDiagramView : NSView
- (void)setIo:(const chfdebug_io *)io;
@end

@implementation ChfIoDiagramView {
    uint8_t _pads[2];
    uint8_t _panel;
    int _reset;
}
- (BOOL)isFlipped { return YES; }
- (NSSize)intrinsicContentSize
{
    return NSMakeSize(2 * 4 * IO_CELL_W + 3 * IO_GAP + 3 * 52, 66);
}

- (void)setIo:(const chfdebug_io *)io
{
    if (io->controller[0] == _pads[0] && io->controller[1] == _pads[1] && io->panel == _panel &&
        io->reset_held == _reset)
        return;
    _pads[0] = io->controller[0];
    _pads[1] = io->controller[1];
    _panel = io->panel;
    _reset = io->reset_held;
    [self setNeedsDisplay:YES];
}

- (void)cell:(NSRect)r text:(NSString *)text lit:(BOOL)lit
{
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(r, 1, 1) xRadius:3 yRadius:3];
    [(lit ? ChfAccentColor() : NSColor.controlBackgroundColor) setFill];
    [path fill];
    [NSColor.separatorColor setStroke];
    [path stroke];
    if (!text.length) return;
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = NSTextAlignmentCenter;
    para.lineBreakMode = NSLineBreakByClipping;
    NSDictionary *attrs = @{ NSFontAttributeName: [NSFont systemFontOfSize:9],
                             NSParagraphStyleAttributeName: para,
                             NSForegroundColorAttributeName: lit ? accentInk() : NSColor.labelColor };
    const NSSize size = [text sizeWithAttributes:attrs];
    [text drawInRect:NSMakeRect(r.origin.x + 1, NSMidY(r) - size.height / 2, r.size.width - 2, size.height)
      withAttributes:attrs];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    static NSString *const names[8] = { @"Right", @"Left", @"Back", @"Fwd", @"CCW", @"CW", @"Pull", @"Push" };
    static NSString *const buttons[4] = { @"1 TIME", @"2 MODE", @"3 HOLD", @"4 START" };
    NSDictionary *head = @{ NSFontAttributeName: [NSFont systemFontOfSize:9],
                            NSForegroundColorAttributeName: NSColor.secondaryLabelColor };
    const CGFloat top = 16;
    CGFloat left = 4;
    /* Player 2 (the left controller, CPU port 4) at the left, Player 1 (the
     * right one, port 1) at the right, as they sit in front of the player */
    for (int side = 0; side < 2; side++) {
        const int player = side == 0 ? 1 : 0;
        [(player == 0 ? @"Player 1: right controller (port 1)" : @"Player 2: left controller (port 4)")
            drawAtPoint:NSMakePoint(left, 2) withAttributes:head];
        for (int contact = 0; contact < 8; contact++)
            [self cell:NSMakeRect(left + (contact % 4) * IO_CELL_W, top + (contact / 4) * IO_CELL_H, IO_CELL_W, IO_CELL_H)
                  text:names[contact] lit:((_pads[player] >> contact) & 1u) != 0];
        left += 4 * IO_CELL_W + IO_GAP;
        if (side == 0) {
            /* the panel between the two controllers */
            [@"Console panel" drawAtPoint:NSMakePoint(left, 2) withAttributes:head];
            for (int button = 0; button < 4; button++)
                [self cell:NSMakeRect(left + (button % 2) * 52, top + (button / 2) * IO_CELL_H, 52, IO_CELL_H)
                      text:buttons[button] lit:((_panel >> button) & 1u) != 0];
            [self cell:NSMakeRect(left + 104, top, 52, IO_CELL_H) text:@"RESET" lit:_reset != 0];
            left += 3 * 52 + IO_GAP;
        }
    }
}
@end

/* ---- the scratchpad as an 8 x 8 octal grid ------------------------------------------ */

#define SC_HDR_W   16.0
#define SC_HDR_H   14.0
#define SC_CELL_W  56.0
#define SC_CELL_H  32.0

/* The 64 scratchpad registers, a cell each at row (r >> 3), column (r & 7) --
 * ISAR's octal upper.lower digits -- with the register's decimal number and
 * name (J, HU/HL, KU/KL, QU/QL) and its value. ISAR's cell is outlined, the
 * cells that changed at the last change are in the accent, and the K stack's
 * band (r40-r59) is shaded. A click picks a cell. The Memory tab also marks a
 * Go To / Find range (an accent outline) and the watched registers (a tint). */
@interface ChfScratchView : NSView
@property (nonatomic, copy) void (^onSelect)(int reg);
@property (nonatomic) int selected;
- (void)setRegs:(const uint8_t *)regs isar:(int)isar;
/* lo < 0: no range */
- (void)setHighlightLo:(int)lo hi:(int)hi;
- (void)setWatched:(const BOOL *)mask;
@end

@implementation ChfScratchView {
    uint8_t _cur[64], _prev[64];
    BOOL _have;
    int _isar;
    int _hiLo, _hiHi;
    BOOL _watched[64];
}
- (BOOL)isFlipped { return YES; }
- (NSSize)intrinsicContentSize
{
    return NSMakeSize(SC_HDR_W + 8 * SC_CELL_W + 2, SC_HDR_H + 8 * SC_CELL_H + 2);
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (!self) return nil;
    _selected = -1;
    _hiLo = -1;
    _hiHi = -1;
    return self;
}

- (void)setRegs:(const uint8_t *)regs isar:(int)isar
{
    BOOL dirty = NO;
    if (!_have) {
        memcpy(_cur, regs, 64);
        memcpy(_prev, regs, 64);
        _have = YES;
        dirty = YES;
    } else if (memcmp(_cur, regs, 64) != 0) {
        /* what changed is shown until the next change */
        memcpy(_prev, _cur, 64);
        memcpy(_cur, regs, 64);
        dirty = YES;
    }
    if (isar != _isar) {
        _isar = isar;
        dirty = YES;
    }
    if (dirty) [self setNeedsDisplay:YES];
}

- (void)setHighlightLo:(int)lo hi:(int)hi
{
    if (lo == _hiLo && hi == _hiHi) return;
    _hiLo = lo;
    _hiHi = hi;
    [self setNeedsDisplay:YES];
}

- (void)setWatched:(const BOOL *)mask
{
    if (!memcmp(_watched, mask, sizeof _watched)) return;
    memcpy(_watched, mask, sizeof _watched);
    [self setNeedsDisplay:YES];
}

static NSString *scratchName(int index)
{
    switch (index) {
    case 9:  return @" J";
    case 10: return @" HU";
    case 11: return @" HL";
    case 12: return @" KU";
    case 13: return @" KL";
    case 14: return @" QU";
    case 15: return @" QL";
    default: return @"";
    }
}

- (void)setSelected:(int)selected
{
    if (selected == _selected) return;
    _selected = selected;
    [self setNeedsDisplay:YES];
}

- (void)mouseDown:(NSEvent *)event
{
    const NSPoint point = [self convertPoint:[event locationInWindow] fromView:nil];
    const int col = (int)floor((point.x - SC_HDR_W) / SC_CELL_W), row = (int)floor((point.y - SC_HDR_H) / SC_CELL_H);
    if (col < 0 || col > 7 || row < 0 || row > 7) return;
    self.selected = row * 8 + col;
    if (self.onSelect) self.onSelect(row * 8 + col);
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    NSDictionary *head = @{ NSFontAttributeName: [NSFont monospacedSystemFontOfSize:9 weight:NSFontWeightRegular],
                            NSForegroundColorAttributeName: NSColor.secondaryLabelColor };
    NSDictionary *name = @{ NSFontAttributeName: [NSFont systemFontOfSize:8],
                            NSForegroundColorAttributeName: NSColor.secondaryLabelColor };
    NSDictionary *value = @{ NSFontAttributeName: [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightMedium],
                             NSForegroundColorAttributeName: NSColor.labelColor };
    NSDictionary *nameOn = @{ NSFontAttributeName: [NSFont systemFontOfSize:8],
                              NSForegroundColorAttributeName: accentInk() };
    NSDictionary *valueOn = @{ NSFontAttributeName: [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightMedium],
                               NSForegroundColorAttributeName: accentInk() };
    for (int i = 0; i < 8; i++) {
        [[NSString stringWithFormat:@"%d", i] drawAtPoint:NSMakePoint(SC_HDR_W + i * SC_CELL_W + SC_CELL_W / 2 - 3, 0)
                                           withAttributes:head];
        [[NSString stringWithFormat:@"%d", i] drawAtPoint:NSMakePoint(4, SC_HDR_H + i * SC_CELL_H + SC_CELL_H / 2 - 6)
                                           withAttributes:head];
    }
    const int isarCell = _isar & 63;
    for (int index = 0; index < 64; index++) {
        const NSRect cell = NSMakeRect(SC_HDR_W + (index & 7) * SC_CELL_W, SC_HDR_H + (index >> 3) * SC_CELL_H,
                                       SC_CELL_W, SC_CELL_H);
        const BOOL changed = _have && _cur[index] != _prev[index];
        const BOOL kband = index >= 40 && index <= 59;
        const NSRect inner = NSInsetRect(cell, 1, 1);
        if (changed) [ChfAccentColor() setFill];
        else if (_watched[index]) [NSColor.selectedTextBackgroundColor setFill];
        else if (kband) [[NSColor colorWithWhite:0.5 alpha:0.22] setFill];
        else [NSColor.controlBackgroundColor setFill];
        NSRectFill(inner);
        [NSColor.separatorColor setStroke];
        NSFrameRect(inner);
        [[NSString stringWithFormat:@"r%d%@", index, scratchName(index)]
            drawAtPoint:NSMakePoint(cell.origin.x + 4, cell.origin.y + 3) withAttributes:changed ? nameOn : name];
        [[NSString stringWithFormat:@"%02X", _cur[index]]
            drawAtPoint:NSMakePoint(cell.origin.x + 4, cell.origin.y + 14) withAttributes:changed ? valueOn : value];
    }
    /* a Go To / Find range */
    if (_hiLo >= 0) {
        for (int index = _hiLo; index <= _hiHi && index < 64; index++) {
            const NSRect cell = NSMakeRect(SC_HDR_W + (index & 7) * SC_CELL_W, SC_HDR_H + (index >> 3) * SC_CELL_H,
                                           SC_CELL_W, SC_CELL_H);
            NSBezierPath *mark = [NSBezierPath bezierPathWithRect:NSInsetRect(cell, 2.5, 2.5)];
            mark.lineWidth = 2;
            [ChfAccentTextColor() setStroke];
            [mark stroke];
        }
    }
    /* ISAR's cell, and the one picked for editing */
    const NSRect isarRect = NSMakeRect(SC_HDR_W + (isarCell & 7) * SC_CELL_W, SC_HDR_H + (isarCell >> 3) * SC_CELL_H,
                                       SC_CELL_W, SC_CELL_H);
    NSBezierPath *ring = [NSBezierPath bezierPathWithRect:NSInsetRect(isarRect, 1.5, 1.5)];
    ring.lineWidth = 2;
    [NSColor.labelColor setStroke];
    [ring stroke];
    if (_selected >= 0 && _selected < 64) {
        const NSRect picked = NSMakeRect(SC_HDR_W + (_selected & 7) * SC_CELL_W,
                                         SC_HDR_H + (_selected >> 3) * SC_CELL_H, SC_CELL_W, SC_CELL_H);
        NSBezierPath *dashed = [NSBezierPath bezierPathWithRect:NSInsetRect(picked, 3.5, 3.5)];
        dashed.lineWidth = 1;
        const CGFloat dash[2] = { 3, 2 };
        [dashed setLineDash:dash count:2 phase:0];
        [NSColor.controlAccentColor setStroke];
        [dashed stroke];
    }
}
@end

@interface ChfDebuggerWindow () <NSTabViewDelegate>
- (instancetype)initWithSession:(chfsession *)session;
- (void)show;
- (void)refreshNow;
@end

@implementation ChfDebuggerWindow {
    chfsession *_session;
    chfdebug *_dbg;
    NSWindow *_window;
    NSTimer *_tick;
    id _keyMonitor;
    unsigned _seenGen;
    BOOL _wasStopped;
    int _liveTicks;
    NSTabView *_tabs;

    NSButton *_runBtn;
    NSTextField *_status;
    NSTextField *_breakAt;

    NSTextView *_conOut;
    NSTextField *_conIn;
    NSMutableArray<NSString *> *_history;
    NSInteger _historyPos;

    NSTextField *_reg[NUM_REGS];
    NSButton *_flag[5];         /* S C Z O ICB, the bits of W */
    NSTextField *_kdepth;       /* the K stack's depth (r59), read-only */
    NSTextField *_beam;

    /* Memory: the five spaces */
    NSPopUpButton *_memSpace;
    DasmTextView *_mem;
    NSScrollView *_memScroll;
    NSView *_scratchPanel;      /* the grid, its edit row and the K stack list */
    ChfScratchView *_scratch;
    NSTextField *_cellLabel, *_cellVal;
    int _cellIdx;
    uint8_t _lastRegs[64];
    NSTextView *_kstack;
    NSTextField *_memGoto, *_memWrite;
    uint32_t _memBase[NUM_SPACES];      /* the dump's first address in each space */
    BOOL _memHl;                /* a range is highlighted: Go to, a hit, a watch */
    int _memHlSpace;
    uint32_t _memHlLo, _memHlHi;
    BOOL _holdMemOnce;          /* CHF_MEM_GOTO: the first stop leaves the dump alone */
    NSPopUpButton *_srchKind;
    NSTextField *_srchEntry, *_srchNote;

    uint64_t _seenStop;         /* chfdebug_stop_serial() last acted on */
    BOOL _scrolledAway;         /* the wheel moved the disassembly off the PC */
    int _traceTicks;

    NSStackView *_wtList;
    NSString *_wtSig;
    NSTextField *_wtEntry;
    NSPopUpButton *_wtSpace;

    NSTextView *_cartText, *_mailbox, *_cartLog, *_ioLog;
    ChfIoDiagramView *_ioDiag;
    NSTextField *_soundMode, *_soundInfo;
    NSProgressIndicator *_soundLevel;
    NSTextView *_soundLog;
    NSButton *_hRec[2];         /* Trace (0) and History (1) share one ring */
    NSPopUpButton *_hFilter[2];
    NSTextField *_hRange[2], *_hNote[2];
    NSTextView *_hText[2];

    NSTextField *_symFilter, *_symCount;
    SymbolTextView *_symList;
    NSMutableArray<NSNumber *> *_symAddrs;      /* the address of each line shown */
    NSMutableArray<NSNumber *> *_symBanks;      /* and its bank (CHFDEBUG_SYM_*) */

    NSTabViewItem *_items[NUM_TABS];    /* each tab's item, in order */
    NSWindow *_popped[NUM_TABS];        /* its own window while popped out */

    NSButton *_followPc;
    NSTextField *_jump;
    DasmTextView *_disasm;
    uint16_t _disasmAddr;
    uint16_t _lineAddr[DISASM_WINDOW];
    int _lineCount;
    int _selAddr;               /* the Disassembly line picked for Run to Cursor, or -1 */

    ChfVideoTab *_video;            /* the Video tab (VideoTab.m) */
    BOOL _videoShown;               /* it was on show at the last refresh */

    NSPopUpButton *_bpType;
    NSTextField *_bpStart, *_bpEnd, *_bpCond, *_bpErr, *_bpStop;
    NSStackView *_bpList;
    NSString *_bpSig;

    NSTextField *_fujiNote;
    NSTextView *_fujiText, *_fujiState;
    uint64_t _fujiLast;
    int _fujiCount;             /* what the log shows: -1 forces a rebuild */
}


/* The register fields, in the strip's order: A, W, ISAR (octal), then the
 * program and data counters, each with its chfdebug_reg and the hex digits it
 * shows (0: ISAR, in octal upper.lower). */
static const struct { NSString *__unsafe_unretained name; int reg; int digits; } kRegs[NUM_REGS] = {
    { @"A", CHF_REG_A, 2 },     { @"W", CHF_REG_W, 2 },     { @"ISAR", CHF_REG_ISAR, 0 },
    { @"PC0", CHF_REG_PC0, 4 }, { @"PC1", CHF_REG_PC1, 4 },
    { @"DC0", CHF_REG_DC0, 4 }, { @"DC1", CHF_REG_DC1, 4 },
};

/* W's bits as checkboxes (S is set when the result was positive), and the
 * register each sets. */
static const struct { NSString *__unsafe_unretained name; int reg; } kFlags[5] = {
    { @"S", CHF_REG_SIGN }, { @"C", CHF_REG_CARRY }, { @"Z", CHF_REG_ZERO },
    { @"O", CHF_REG_OVERFLOW }, { @"ICB", CHF_REG_ICB },
};

#define NUM_FILE_KINDS 8
static const char *const kFileKinds[NUM_FILE_KINDS] = {
    "dis", "mailbox", "vram", "scratch", "cart", "screen", "vramshot", "regs"
};
static NSString *const kFileTitles[NUM_FILE_KINDS] = {
    @"Disassembly…", @"Mailbox ($F800-$FFFF)…", @"VRAM…", @"Scratchpad…", @"Cartridge image…",
    @"Screen (PPM)…", @"Whole VRAM picture (PPM)…", @"Registers…"
};
static NSString *const kFileNames[NUM_FILE_KINDS] = {
    @"disassembly.asm", @"mailbox.bin", @"vram.bin", @"scratchpad.bin", @"cart.bin", @"screen.ppm",
    @"vram.ppm", @"registers.txt"
};

/* CHF_DEBUGGER_TAB takes an index or the start of one of these. */
static const char *const kTabNames[NUM_TABS] = {
    "console", "disassembly", "memory", "breakpoints", "watches", "cart", "trace", "history", "video", "sound",
    "symbols", "fujinet"
};
static NSString *const kTabTitles[NUM_TABS] = {
    @"Console", @"Disassembly", @"Memory", @"Breakpoints", @"Watches", @"Cart & I/O", @"Trace", @"History",
    @"Video", @"Sound", @"Symbols", @"FujiNet"
};
/* Help's contexts (data/help/help.md), one per tab */
static NSString *const kHelpTabs[NUM_TABS] = {
    @"console", @"disassembly", @"memory", @"breakpoints", @"watches", @"cart", @"trace", @"history", @"video",
    @"sound", @"symbols", @"fujinet"
};

/* The Memory tab's spaces, in chfdebug_space order. */
static NSString *const kSpaceTitles[NUM_SPACES] = {
    @"PROG (the CPU's view)", @"SCRATCH (r0-r63)", @"VRAM (row * 128 + col)", @"CART (the cartridge's memory)",
    @"IO (the ports)"
};

/* A tab by index or by the start of its name; -1 for none. */
static int tabFromText(const char *tab)
{
    if (!tab || !*tab) return -1;
    if (tab[0] >= '0' && tab[0] <= '9') return atoi(tab);
    /* the names of the old eight-tab window */
    if (!strncasecmp(tab, "prompt", strlen(tab)) && strlen(tab) > 1) return TAB_CONSOLE;
    if (!strcasecmp(tab, "cpu") || !strcasecmp(tab, "ram")) return TAB_MEMORY;
    if (!strncasecmp(tab, "io", 2) || !strncasecmp(tab, "i/o", 3)) return TAB_CART;
    for (int i = 0; i < NUM_TABS; i++)
        if (!strncasecmp(tab, kTabNames[i], strlen(tab))) return i;
    return -1;
}

/* ---- helpers --------------------------------------------------------------- */

/* The engine's text as a string: control bytes dropped (a console's CRs and
 * escapes), UTF-8 if it is, otherwise Latin-1. */
static NSString *cstr(const char *text)
{
    if (!text) return @"";
    const size_t length = strlen(text);
    char *copy = malloc(length + 1);
    if (!copy) return @"";
    size_t used = 0;
    for (size_t i = 0; i < length; i++) {
        const unsigned char c = (unsigned char)text[i];
        if (c >= 0x20 || c == '\n' || c == '\t') copy[used++] = (char)c;
    }
    copy[used] = '\0';
    NSString *result = [NSString stringWithUTF8String:copy];
    if (!result) result = [[NSString alloc] initWithBytes:copy length:used encoding:NSISOLatin1StringEncoding];
    free(copy);
    return result ?: @"";
}

/* Pads a column to width (never cuts a longer one). */
static NSString *padTo(NSString *text, NSUInteger width)
{
    return text.length >= width ? text : [text stringByPaddingToLength:width withString:@" " startingAtIndex:0];
}

/* $hex, 0xhex, hexh, #dec, or bare hex. */
static BOOL parseNum(NSString *text, long *out)
{
    NSString *t = [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    int base = 16;
    if ([t hasPrefix:@"$"]) t = [t substringFromIndex:1];
    else if ([t.lowercaseString hasPrefix:@"0x"]) t = [t substringFromIndex:2];
    else if ([t hasPrefix:@"#"]) { t = [t substringFromIndex:1]; base = 10; }
    else if (t.length > 1 && [t.lowercaseString hasSuffix:@"h"]) t = [t substringToIndex:t.length - 1];
    if (t.length == 0) return NO;
    char *end;
    long value = strtol(t.UTF8String, &end, base);
    if (*end) return NO;
    *out = value;
    return YES;
}

/* A value as the fields take it: a label, or $hex, 0xhex, hexh, #dec or bare
 * hex digits. On failure the reason is in *why. */
- (BOOL)evaluate:(NSString *)text into:(long *)out why:(NSString **)why
{
    NSString *t = [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    long value;
    if (t.length == 0) {
        if (why) *why = @"nothing entered";
        return NO;
    }
    const int label = chfdebug_label_address(_dbg, t.UTF8String);
    if (label >= 0) {
        *out = label;
        return YES;
    }
    if (parseNum(t, &value)) {
        *out = value;
        return YES;
    }
    if (why) *why = @"not a number or label";
    return NO;
}

/* An address: the above, within $0000-$FFFF. -1 when it is not one. */
- (int)resolveAddr:(NSString *)text why:(NSString **)why
{
    long value;
    if (![self evaluate:text into:&value why:why]) return -1;
    if (value < 0 || value > 0xFFFF) {
        if (why) *why = @"out of range ($0000-$FFFF)";
        return -1;
    }
    return (int)value;
}

/* ISAR as the CPU writes it in octal: "5.3" (upper.lower) or "53". */
static BOOL parseIsar(NSString *text, long *out)
{
    NSString *t = [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    const NSRange dot = [t rangeOfString:@"."];
    if (dot.location != NSNotFound) {
        NSString *upper = [t substringToIndex:dot.location], *lower = [t substringFromIndex:dot.location + 1];
        char *endUpper, *endLower;
        if (upper.length == 0 || lower.length == 0) return NO;
        const long hi = strtol(upper.UTF8String, &endUpper, 8), lo = strtol(lower.UTF8String, &endLower, 8);
        if (*endUpper || *endLower) return NO;
        *out = ((hi & 7) << 3) | (lo & 7);
        return YES;
    }
    if (t.length == 0) return NO;
    char *end;
    const long value = strtol(t.UTF8String, &end, 8);
    if (*end) return NO;
    *out = value & 63;
    return YES;
}

/* A space by the start of its name ("scratch", "vram", ...); -1 for none. */
static int spaceFromName(NSString *name)
{
    if (name.length == 0) return -1;
    for (int space = 0; space < NUM_SPACES; space++)
        if ([[NSString stringWithUTF8String:chfmem_space_name(space)] caseInsensitiveCompare:name] == NSOrderedSame)
            return space;
    return -1;
}

/* "scratch:r12" is space SCRATCH and "r12"; any other text keeps `fallback`. */
static int splitSpace(NSString *item, int fallback, NSString **rest)
{
    NSString *text = [item stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    const NSRange colon = [text rangeOfString:@":"];
    if (colon.location != NSNotFound) {
        const int space = spaceFromName([[text substringToIndex:colon.location]
            stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]]);
        if (space >= 0) {
            *rest = [[text substringFromIndex:colon.location + 1]
                stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
            return space;
        }
    }
    *rest = text;
    return fallback;
}

/* Hex digits of an address in a space's dump. */
static int memDigits(int space)
{
    switch (space) {
    case CHF_SPACE_SCRATCH:
    case CHF_SPACE_IO:    return 2;
    case CHF_SPACE_CART:  return 5;
    default:              return 4;
    }
}

static NSFont *monoFont(void)
{
    return [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
}

static NSDictionary *monoAttrs(void)
{
    return @{ NSFontAttributeName: monoFont(), NSForegroundColorAttributeName: NSColor.textColor };
}

/* The accent fill with dark ink on it: the PC's line, a marked row. */
static NSDictionary *accentAttrs(void)
{
    return @{ NSBackgroundColorAttributeName: ChfAccentColor(), NSForegroundColorAttributeName: accentInk() };
}

static NSTextView *monoView(NSScrollView **scrollOut, BOOL wrap)
{
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = !wrap;
    NSTextView *view = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
    view.editable = NO;
    view.richText = NO;
    view.font = monoFont();
    if (wrap) {
        view.autoresizingMask = NSViewWidthSizable;
    } else {
        view.horizontallyResizable = YES;
        view.textContainer.widthTracksTextView = NO;
        view.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
        view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    }
    scroll.documentView = view;
    *scrollOut = scroll;
    return view;
}

/* A monospace row view in a scroll view with no vertical bar: the wheel and
 * the clicks are the view's own (the disassembly, the memory dump). */
static DasmTextView *rowView(NSScrollView **scrollOut)
{
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = NO;
    scroll.hasHorizontalScroller = YES;
    DasmTextView *view = [[DasmTextView alloc] initWithFrame:NSMakeRect(0, 0, 600, 600)];
    view.editable = NO;
    view.richText = NO;
    view.selectable = NO;
    view.font = monoFont();
    view.horizontallyResizable = YES;
    view.textContainer.widthTracksTextView = NO;
    view.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
    view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    scroll.documentView = view;
    *scrollOut = scroll;
    return view;
}

/* Whether a view's reader is at its end (a log then follows new lines). */
static BOOL atEnd(NSTextView *view)
{
    NSScrollView *scroll = view.enclosingScrollView;
    if (!scroll) return YES;
    return NSMaxY(scroll.contentView.documentVisibleRect) >= NSHeight(view.frame) - 4;
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
static void setText(NSTextView *view, NSString *text, BOOL follow)
{
    if (![view.string isEqualToString:text]) {
        NSScrollView *scroll = view.enclosingScrollView;
        const NSPoint at = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
        view.string = text;
        if (scroll && !follow) {
            [scroll.contentView scrollToPoint:at];
            [scroll reflectScrolledClipView:scroll.contentView];
        }
    }
    if (follow) [view scrollRangeToVisible:NSMakeRange(view.string.length, 0)];
}

/* The same for styled text, always where the reader left it. */
static void setStyledText(NSTextView *view, NSAttributedString *text)
{
    NSScrollView *scroll = view.enclosingScrollView;
    const NSPoint at = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
    [view.textStorage setAttributedString:text];
    if (scroll) {
        [scroll.contentView scrollToPoint:at];
        [scroll reflectScrolledClipView:scroll.contentView];
    }
}

- (NSButton *)button:(NSString *)title action:(SEL)sel
{
    NSButton *b = [NSButton buttonWithTitle:title target:self action:sel];
    [b setRefusesFirstResponder:YES];
    return b;
}

- (NSTextField *)label:(NSString *)text
{
    return [NSTextField labelWithString:text];
}

- (NSTextField *)hint:(NSString *)text
{
    NSTextField *l = [self label:text];
    l.textColor = NSColor.secondaryLabelColor;
    /* a long hint gives way to a narrow window instead of widening it */
    l.lineBreakMode = NSLineBreakByTruncatingTail;
    [l setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                forOrientation:NSLayoutConstraintOrientationHorizontal];
    return l;
}

- (NSTextField *)field:(NSString *)placeholder width:(CGFloat)width action:(SEL)sel
{
    NSTextField *f = [[NSTextField alloc] init];
    f.placeholderString = placeholder;
    f.font = monoFont();
    f.target = self;
    f.action = sel;
    if (width > 0) [f.widthAnchor constraintEqualToConstant:width].active = YES;
    return f;
}

/* A pull-down button: `title` on its face, `items` underneath (each one's
 * tag is its index). */
- (NSPopUpButton *)pullDown:(NSString *)title items:(NSArray<NSString *> *)items action:(SEL)sel
{
    NSPopUpButton *p = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:YES];
    [p addItemWithTitle:title];
    p.lastItem.tag = -1;
    for (NSUInteger i = 0; i < items.count; i++) {
        [p addItemWithTitle:items[i]];
        p.lastItem.tag = (NSInteger)i;
    }
    p.target = self;
    p.action = sel;
    [p setRefusesFirstResponder:YES];
    return p;
}

static NSStackView *hstack(NSArray<NSView *> *views)
{
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    s.spacing = 6;
    return s;
}

static NSStackView *vstack(NSArray<NSView *> *views)
{
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationVertical;
    s.alignment = NSLayoutAttributeLeading;
    s.spacing = 6;
    return s;
}

static void fill(NSStackView *stack, NSView *view)
{
    [view.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
}

/* A drawn view at its own size: the stacks then neither squeeze nor
 * stretch it. */
static void fixSize(NSView *view)
{
    const NSSize size = view.intrinsicContentSize;
    [view.widthAnchor constraintEqualToConstant:size.width].active = YES;
    [view.heightAnchor constraintEqualToConstant:size.height].active = YES;
}

/* ---- lifetime -------------------------------------------------------------- */

+ (void)showForSession:(chfsession *)session
{
    if (!g_debugger) g_debugger = [[ChfDebuggerWindow alloc] initWithSession:session];
    [g_debugger show];
}

+ (void)toggleForSession:(chfsession *)session
{
    if (g_debugger && [g_debugger->_window isVisible]) {
        /* windowWillClose: detaches */
        [g_debugger->_window close];
        return;
    }
    [self showForSession:session];
}

- (void)show
{
    if (_window.isMiniaturized) [_window deminiaturize:nil];
    [_window makeKeyAndOrderFront:nil];
    for (int t = 0; t < NUM_TABS; t++)
        if (_popped[t]) [_popped[t] orderFront:nil];
    const unsigned gen = chfdebug_generation(_dbg);
    chfdebug_attach(_dbg);
    [self settleFrom:gen];
    [self refreshNow];
    if ([self currentTab] == TAB_CONSOLE) [_window makeFirstResponder:_conIn];
}

- (instancetype)initWithSession:(chfsession *)session
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _dbg = chfsession_debugger(session);
    _fujiCount = -1;
    _memBase[CHF_SPACE_PROG] = 0x0800 - 2 * 16;
    _symAddrs = [NSMutableArray array];
    _symBanks = [NSMutableArray array];
    _history = [NSMutableArray array];
    [self buildWindow];

    /* For captures and tests, like CHF_DEBUGGER_TAB */
    {
        const char *watch = getenv("CHF_WATCH");
        const char *watchBreak = getenv("CHF_WATCH_BREAK");
        const char *gotoText = getenv("CHF_MEM_GOTO");
        const char *history = getenv("CHF_HISTORY");
        const char *popOut = getenv("CHF_POPOUT");
        uint32_t lo, hi;
        NSString *rest = nil;
        if (watch && *watch) {
            for (NSString *item in [cstr(watch) componentsSeparatedByString:@","]) {
                const int space = splitSpace(item, CHF_SPACE_PROG, &rest);
                if (rest.length) chfwatch_add(space, rest.UTF8String);
            }
        }
        if (watchBreak && *watchBreak) {
            const int space = splitSpace(cstr(watchBreak), CHF_SPACE_PROG, &rest);
            chfwatch_add(space, rest.UTF8String);
            if (chfsym_parse_range(_dbg, space, rest.UTF8String, &lo, &hi)) [self breakOnChangeSpace:space lo:lo hi:hi];
        }
        if (gotoText && *gotoText) {
            const int space = splitSpace(cstr(gotoText), CHF_SPACE_PROG, &rest);
            if (chfsym_parse_range(_dbg, space, rest.UTF8String, &lo, &hi)) {
                _holdMemOnce = YES;
                [self showSpace:space lo:lo hi:hi];
            }
        }
        if (history && *history) chfdebug_trace_enable(_dbg, 1);
        if (popOut && *popOut) {
            const int t = tabFromText(popOut);
            if (t >= 0 && t < NUM_TABS) [self popOutTab:t];
        }
    }

    __weak ChfDebuggerWindow *weakSelf = self;
    _tick = [NSTimer timerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *t) {
        (void)t;
        [weakSelf onTick];
    }];
    [[NSRunLoop mainRunLoop] addTimer:_tick forMode:NSRunLoopCommonModes];

    /* F5/F7/F8/F12 wherever the focus is in this window, the console's
     * field included */
    _keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                        handler:^NSEvent *(NSEvent *event) {
        ChfDebuggerWindow *strongSelf = weakSelf;
        if (!strongSelf || ![strongSelf ownsWindow:event.window] || !strongSelf->_window.isVisible) return event;
        return [strongSelf handleKey:event] ? nil : event;
    }];
    return self;
}

- (void)dealloc
{
    [_tick invalidate];
    if (_keyMonitor) [NSEvent removeMonitor:_keyMonitor];
}

- (void)windowWillClose:(NSNotification *)note
{
    if (note.object != _window) return;
    chfdebug_detach(_dbg);
    for (int t = 0; t < NUM_TABS; t++)
        [_popped[t] orderOut:nil];     /* they come back with the window */
}

/* Closing a popped-out tab's window docks the tab again. */
- (BOOL)windowShouldClose:(NSWindow *)sender
{
    for (int t = 0; t < NUM_TABS; t++)
        if (sender == _popped[t]) {
            [self dockTab:t];
            return NO;
        }
    return YES;
}

/* The debugger's main window or one of its popped-out tabs. */
- (BOOL)ownsWindow:(NSWindow *)window
{
    if (!window) return NO;
    if (window == _window) return YES;
    for (int t = 0; t < NUM_TABS; t++)
        if (_popped[t] == window) return YES;
    return NO;
}

/* The tab the main window's notebook shows, by its place in the full order
 * (-1 if none). */
- (NSInteger)currentTab
{
    NSTabViewItem *selected = _tabs.selectedTabViewItem;
    for (int t = 0; t < NUM_TABS; t++)
        if (_items[t] == selected) return t;
    return -1;
}

/* On screen: the notebook's page, or a window of its own. */
- (BOOL)tabVisible:(int)tab
{
    return _popped[tab] != nil || [self currentTab] == tab;
}

/* A new stop (a pause, a breakpoint, a step): every view goes to the current
 * spot, even if the reader scrolled away. The History and Trace logs always
 * follow their newest line. */
- (void)recentreViews
{
    _scrolledAway = NO;
    _followPc.state = NSControlStateValueOn;
    if (_holdMemOnce) {
        _holdMemOnce = NO;      /* CHF_MEM_GOTO's first look stays */
        return;
    }
    chfdebug_cpu cpu;
    chfdebug_cpu_get(_dbg, &cpu);
    const uint32_t pcRow = (uint32_t)(cpu.pc & 0xFFF0);
    _memBase[CHF_SPACE_PROG] = pcRow >= 32 ? pcRow - 2 * 16 : 0;
    _memHl = NO;
    _selAddr = -1;
}

/* The poll: refresh on a stop, a resume or a command (the generation), and
 * the visible tab's live values twice a second while the machine runs. The
 * FujiNet tab also follows the trace twice a second while stopped, as the
 * FujiNet's packets arrive on their own. */
- (void)onTick
{
    if (![_window isVisible]) return;
    const unsigned gen = chfdebug_generation(_dbg);
    const BOOL stopped = chfdebug_is_stopped(_dbg) != 0;
    const uint64_t stopSerial = chfdebug_stop_serial(_dbg);
    if (gen != _seenGen || stopped != _wasStopped || stopSerial != _seenStop) {
        if (stopSerial != _seenStop) {
            _seenStop = stopSerial;
            [self recentreViews];
        }
        [self refreshNow];
    } else if (++_liveTicks >= LIVE_TICKS) {
        _liveTicks = 0;
        if (!stopped) {
            [self refreshStatus];
            [self refreshTab];
        } else if ([self tabVisible:TAB_FUJINET]) {
            [self refreshFujinet:NO];
        }
    } else if (!stopped && [self tabVisible:TAB_TRACE] && ++_traceTicks >= TRACE_TICKS) {
        _traceTicks = 0;        /* the Trace tab follows the machine */
        [self refreshHist:0];
    } else if (!stopped && [self tabVisible:TAB_VIDEO]) {
        [self refreshVideo];    /* the picture runs at the poll's rate */
    } else if (!stopped && [self tabVisible:TAB_SOUND]) {
        [self refreshSound];    /* and so does the level meter */
    }
}

/* Waits up to ~40 ms for a stop to land after the engine was asked to move:
 * the generation moves on and the machine reads as stopped. */
- (void)settleFrom:(unsigned)gen
{
    for (int i = 0; i < 20; i++) {
        if (chfdebug_generation(_dbg) != gen && chfdebug_is_stopped(_dbg)) break;
        usleep(2000);
    }
}

- (void)updateHelpContext
{
    const NSInteger index = [self currentTab];
    if (index >= 0 && index < NUM_TABS)
        ChfSetHelpContext(_window, [@"debugger/" stringByAppendingString:kHelpTabs[index]]);
}

- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    (void)item;
    [self updateHelpContext];
    [self refreshTab];
    if ([self currentTab] == TAB_CONSOLE) [_window makeFirstResponder:_conIn];
}

/* ---- keys -------------------------------------------------------------------- */

- (BOOL)handleKey:(NSEvent *)event
{
    NSString *chars = event.charactersIgnoringModifiers;
    if (chars.length != 1) return NO;
    const NSEventModifierFlags mods = event.modifierFlags;
    if (mods & (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption)) return NO;
    const BOOL shift = (mods & NSEventModifierFlagShift) != 0;
    switch ([chars characterAtIndex:0]) {
    case NSF5FunctionKey: [self toggleRun:nil]; return YES;
    case NSF7FunctionKey: [self step:nil]; return YES;
    case NSF8FunctionKey: if (shift) [self stepOut:nil]; else [self stepOver:nil]; return YES;
    case NSF12FunctionKey: [_window performClose:nil]; return YES;
    default: return NO;
    }
}

/* ---- construction ------------------------------------------------------------ */

- (NSView *)buildConsole
{
    NSScrollView *outScroll;
    _conOut = monoView(&outScroll, YES);
    _conOut.string = @"Debugger console. Type 'help' for every command; Tab completes, Up/Down walk the history.\n";
    _conIn = [self field:@"command (help, step, break $0812, bpw $8000, bps r12, print a, mem scratch 0 64, ...) — Tab completes, ↑↓ history"
                   width:0 action:@selector(runCommand:)];
    _conIn.delegate = self;

    NSStackView *v = vstack(@[outScroll, _conIn]);
    fill(v, outScroll);
    fill(v, _conIn);
    return v;
}

/* The registers, under the tabs on every one of them: A, W with its bits, ISAR
 * (octal) and PC0, PC1, DC0, DC1 (editable while stopped), the K stack depth
 * (r59) and the machine's state and beam. J, H, K and Q are the scratchpad's
 * r9-r15: the Memory tab's grid shows and edits them. */
- (NSView *)buildRegStrip
{
    NSMutableArray *row1 = [NSMutableArray array], *row2 = [NSMutableArray array];
    for (int i = 0; i < NUM_REGS; i++) {
        const int digits = kRegs[i].digits;
        const CGFloat width = digits == 4 ? 52 : (digits == 2 ? 36 : 44);
        NSMutableArray *row = i < 3 ? row1 : row2;
        [row addObject:[self label:kRegs[i].name]];
        _reg[i] = [self field:@"" width:width action:@selector(applyRegister:)];
        _reg[i].tag = i;
        _reg[i].toolTip = i == 2 ? @"Octal, upper.lower (5.3); edit while the machine is stopped, then Enter"
                                 : @"Edit while the machine is stopped (hex, $hex or a label), then Enter";
        [row addObject:_reg[i]];
        if (i == 1) {
            /* W's bits sit beside W */
            for (int bit = 0; bit < 5; bit++) {
                _flag[bit] = [NSButton checkboxWithTitle:kFlags[bit].name target:self action:@selector(flagClicked:)];
                _flag[bit].tag = bit;
                _flag[bit].toolTip = bit == 0 ? @"S is set when the result was positive"
                                              : @"Click while the machine is stopped to change it";
                [_flag[bit] setRefusesFirstResponder:YES];
                [row addObject:_flag[bit]];
            }
        }
    }
    _kdepth = [self label:@""];
    _kdepth.font = monoFont();
    _kdepth.toolTip = @"The BIOS K stack's depth: (r59 - 40) / 2. Edit r59 in the Memory tab's SCRATCH grid";
    [row2 addObject:_kdepth];
    _beam = [self hint:@""];

    return vstack(@[hstack(row1), hstack(row2), _beam]);
}

/* The five spaces, one dump (or, for the scratchpad, the grid and the K stack
 * list), Go To, Find and Poke. */
- (NSView *)buildMemory
{
    NSScrollView *memScroll;
    _mem = rowView(&memScroll);
    _memScroll = memScroll;
    __weak ChfDebuggerWindow *weakSelf = self;
    _mem.onScroll = ^(int rows) {
        ChfDebuggerWindow *strongSelf = weakSelf;
        if (!strongSelf || rows == 0) return;
        const int space = (int)strongSelf->_memSpace.indexOfSelectedItem;
        if (space < 0 || space >= NUM_SPACES) return;
        const int64_t base = (int64_t)strongSelf->_memBase[space] + (int64_t)rows * 16;
        strongSelf->_memBase[space] = base < 0 ? 0 : (uint32_t)base;
        [strongSelf refreshMem];
    };

    _memSpace = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_memSpace addItemsWithTitles:[NSArray arrayWithObjects:kSpaceTitles count:NUM_SPACES]];
    _memSpace.target = self;
    _memSpace.action = @selector(memSpaceChanged:);
    [_memSpace setRefusesFirstResponder:YES];
    _memGoto = [self field:@"address, label, expression or lo-hi" width:230 action:@selector(gotoMem:)];
    NSButton *go = [self button:@"Go To" action:@selector(gotoMem:)];
    NSButton *prev = [self button:@"◀" action:@selector(memPrev:)];
    NSButton *next = [self button:@"▶" action:@selector(memNext:)];
    NSStackView *memHead = hstack(@[[self label:@"Space"], _memSpace, [self label:@"Go to"], _memGoto, go, prev, next]);

    _srchKind = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_srchKind addItemsWithTitles:@[@"Hex", @"Text"]];
    _srchKind.toolTip = @"Hex bytes (F8 09) or text, one byte per character (any case)";
    [_srchKind setRefusesFirstResponder:YES];
    _srchEntry = [self field:@"hex bytes or text" width:220 action:@selector(searchNext:)];
    _srchNote = [self hint:@""];
    NSStackView *searchRow = hstack(@[[self label:@"Search"], _srchKind, _srchEntry,
                                      [self button:@"Find Next" action:@selector(searchNext:)],
                                      [self button:@"Find Previous" action:@selector(searchPrev:)], _srchNote]);

    _memWrite = [self field:@"address value" width:170 action:@selector(writeMem:)];
    NSStackView *edit = hstack(@[[self label:@"Poke"], _memWrite,
                                 [self hint:@"The cartridge's RAM and a Videocart's RAM take it in PROG; the BIOS, ROM and "
                                            @"the mailbox's hotspots do not. Several values go to consecutive addresses"]]);

    /* the scratchpad: the grid, an edit box for the picked register, the K stack */
    _scratch = [[ChfScratchView alloc] initWithFrame:NSZeroRect];
    fixSize(_scratch);
    _scratch.onSelect = ^(int reg) { [weakSelf pickCell:reg]; };
    _cellIdx = -1;
    _cellLabel = [self label:@"Click a cell to edit a register"];
    _cellVal = [self field:@"$00" width:44 action:@selector(setCell:)];
    NSStackView *cellRow = hstack(@[_cellLabel, _cellVal, [self button:@"Set" action:@selector(setCell:)]]);
    NSScrollView *kScroll;
    _kstack = monoView(&kScroll, NO);
    [kScroll.heightAnchor constraintEqualToConstant:118].active = YES;
    NSTextField *kTitle = [self label:@"K stack (the BIOS's saved returns in r40-r58; r59 points past them)"];
    kTitle.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    NSStackView *kColumn = vstack(@[kTitle, kScroll]);
    fill(kColumn, kScroll);
    [kColumn.widthAnchor constraintGreaterThanOrEqualToConstant:360].active = YES;
    NSStackView *gridColumn = vstack(@[_scratch, cellRow]);
    NSStackView *panel = hstack(@[gridColumn, kColumn]);
    panel.alignment = NSLayoutAttributeTop;
    panel.spacing = 16;
    _scratchPanel = panel;
    _scratchPanel.hidden = YES;

    NSStackView *v = vstack(@[memHead, searchRow, memScroll, _scratchPanel, edit]);
    fill(v, memScroll);
    return v;
}

- (NSView *)buildDisasm
{
    _followPc = [NSButton checkboxWithTitle:@"Follow PC" target:self action:@selector(followToggled:)];
    _followPc.state = NSControlStateValueOn;
    _jump = [self field:@"address or label" width:150 action:@selector(jumpTo:)];
    _selAddr = -1;
    NSStackView *row = hstack(@[_followPc, [self label:@"Jump to"], _jump,
                                [self hint:@"Click a line to pick it (Run to Cursor); click its dot or double-click it "
                                           @"for a breakpoint (red dot on, grey ring off); scroll to browse"]]);

    NSScrollView *scroll;
    _disasm = rowView(&scroll);
    __weak ChfDebuggerWindow *weakSelf = self;
    _disasm.onClick = ^(int line, int column, NSInteger clicks) {
        ChfDebuggerWindow *strongSelf = weakSelf;
        if (!strongSelf || line < 0 || line >= strongSelf->_lineCount) return;
        strongSelf->_selAddr = strongSelf->_lineAddr[line];
        /* the gutter and a double click set the breakpoint; any other click picks the line */
        if (column < 2 || clicks >= 2) {
            chfdebug_breakpoint_toggle(strongSelf->_dbg, strongSelf->_lineAddr[line]);
            if ([strongSelf tabVisible:TAB_BREAKS]) [strongSelf refreshBps];
        }
        [strongSelf refreshDisasm];
    };
    _disasm.onScroll = ^(int rows) {
        ChfDebuggerWindow *strongSelf = weakSelf;
        if (!strongSelf || rows == 0) return;
        strongSelf->_followPc.state = NSControlStateValueOff;
        strongSelf->_scrolledAway = YES;
        strongSelf->_disasmAddr = (uint16_t)chfdebug_row_address(strongSelf->_dbg, strongSelf->_disasmAddr, rows);
        [strongSelf refreshDisasm];
    };

    NSStackView *v = vstack(@[row, scroll]);
    fill(v, row);
    fill(v, scroll);
    return v;
}

/* The Video tab: a controller (core chfvidtab.h) behind its own views. */
- (NSView *)buildVideo
{
    _video = [[ChfVideoTab alloc] initWithDebugger:_dbg];
    return _video.view;
}

/* One table cell: a fixed-width monospace label, or (width 0) the flexible
 * one that takes what is left. */
static NSTextField *tableCell(NSString *text, CGFloat width, BOOL bold)
{
    NSTextField *l = [NSTextField labelWithString:text];
    l.font = bold ? [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightSemibold] : monoFont();
    l.lineBreakMode = NSLineBreakByTruncatingTail;
    if (width > 0) {
        [l.widthAnchor constraintEqualToConstant:width].active = YES;
    } else {
        [l setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
        [l setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                    forOrientation:NSLayoutConstraintOrientationHorizontal];
    }
    return l;
}

- (NSView *)buildBreaks
{
    /* the table's rows scroll: a session can collect more breakpoints than
     * the tab is tall. The stack is the scroll view's document, pinned to
     * the top and to the clip view's width so rows keep the tab's width. */
    _bpStop = [self label:@""];
    _bpStop.textColor = NSColor.secondaryLabelColor;
    NSStackView *header = hstack(@[tableCell(@"On  #", 70, YES), tableCell(@"Breakpoint", 0, YES),
                                   tableCell(@"Hits", 60, YES), tableCell(@"", 70, YES)]);
    header.distribution = NSStackViewDistributionFill;
    _bpList = vstack(@[]);
    _bpList.translatesAutoresizingMaskIntoConstraints = NO;
    NSScrollView *bpScroll = [[NSScrollView alloc] init];
    bpScroll.hasVerticalScroller = YES;
    bpScroll.drawsBackground = NO;
    bpScroll.documentView = _bpList;
    NSClipView *clip = bpScroll.contentView;
    [_bpList.topAnchor constraintEqualToAnchor:clip.topAnchor].active = YES;
    [_bpList.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor].active = YES;
    [_bpList.widthAnchor constraintEqualToAnchor:clip.widthAnchor].active = YES;

    _bpType = [[NSPopUpButton alloc] init];
    [_bpType addItemsWithTitles:@[@"Execute", @"Read", @"Write", @"Access", @"IN", @"OUT", @"VRAM", @"SCRATCH"]];
    _bpType.toolTip = @"Execute: an instruction about to run. Read, Write, Access (either): PROG space data. "
                      @"IN, OUT: a port. VRAM: a cell write (row,col). SCRATCH: a register write (r0-r63)";
    _bpStart = [self field:@"$0812, lo-hi, a label; port $20; row,col; r12" width:250 action:@selector(addBreakpoint:)];
    _bpEnd = [self field:@"end (optional)" width:120 action:@selector(addBreakpoint:)];
    _bpCond = [self field:@"condition (optional): a == 5" width:0 action:@selector(addBreakpoint:)];
    NSButton *add = [self button:@"Add" action:@selector(addBreakpoint:)];
    NSButton *clearHits = [self button:@"Clear Hits" action:@selector(clearHits:)];
    NSButton *clear = [self button:@"Clear Breakpoints" action:@selector(clearBreaks:)];
    NSStackView *row = hstack(@[_bpType, [self label:@"Start"], _bpStart, [self label:@"End"], _bpEnd,
                                [self label:@"Condition"], _bpCond, add, clearHits, clear]);
    row.distribution = NSStackViewDistributionFill;
    _bpErr = [self label:@""];
    _bpErr.textColor = NSColor.systemRedColor;
    NSTextField *hint = [self hint:@"Untick to disable. A condition is an expression, e.g. a == 5 or, on a read, write, "
                                   @"port, VRAM or SCRATCH break, value != old (addr is the address, old the value "
                                   @"before a write). Ports are 0-255, VRAM cells row,col, registers 0-63; the end "
                                   @"defaults to the start."];

    NSStackView *v = vstack(@[_bpStop, header, bpScroll, hint, row, _bpErr]);
    fill(v, header);
    fill(v, bpScroll);
    fill(v, row);
    [bpScroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    return v;
}

/* Watches: an entry, then a row for each with its value and three buttons. */
- (NSView *)buildWatches
{
    _wtEntry = [self field:@"address, label, expression or lo-hi (r12 in SCRATCH, row,col in VRAM)" width:0
                    action:@selector(addWatch:)];
    _wtSpace = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (int space = 0; space < NUM_SPACES; space++)
        [_wtSpace addItemWithTitle:[NSString stringWithUTF8String:chfmem_space_name(space)]];
    _wtSpace.toolTip = @"The space the watch reads (it follows the Memory tab's)";
    [_wtSpace setRefusesFirstResponder:YES];
    NSStackView *row = hstack(@[[self label:@"Watch"], _wtSpace, _wtEntry, [self button:@"Add" action:@selector(addWatch:)]]);
    row.distribution = NSStackViewDistributionFill;

    _wtList = vstack(@[]);
    _wtList.translatesAutoresizingMaskIntoConstraints = NO;
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.drawsBackground = NO;
    scroll.documentView = _wtList;
    NSClipView *clip = scroll.contentView;
    [_wtList.topAnchor constraintEqualToAnchor:clip.topAnchor].active = YES;
    [_wtList.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor].active = YES;
    [_wtList.widthAnchor constraintEqualToAnchor:clip.widthAnchor].active = YES;

    NSTextField *hint = [self hint:@"Values show while the machine is stopped. Show in Memory shows the watch in the Memory "
                                   @"tab, in its own space; Break on Change stops when the machine writes a different "
                                   @"value there (PROG, SCRATCH, VRAM and IO)."];
    NSStackView *v = vstack(@[row, scroll, hint]);
    fill(v, row);
    fill(v, scroll);
    [scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    return v;
}

/* Cart & I/O: the controllers and the console panel drawn above the text of
 * the cartridge (a Videocart's type, CRC and bank latch; the FujiNet
 * cartridge's live and staged images, APPBANK, status page), the ports 0, 1, 4
 * and 5 raw, complemented and decoded, the 2102 latch and the memory map on
 * the left; the port accesses, the cartridge's events and the mailbox right. */
- (NSView *)buildCart
{
    NSScrollView *scroll, *mailScroll, *logScroll, *ioScroll;
    _cartText = monoView(&scroll, NO);
    _mailbox = monoView(&mailScroll, NO);
    _cartLog = monoView(&logScroll, NO);
    _ioLog = monoView(&ioScroll, NO);

    _ioDiag = [[ChfIoDiagramView alloc] initWithFrame:NSZeroRect];
    fixSize(_ioDiag);
    NSTextField *padHint = [self hint:@"Filled: held, as the console latched it at the frame (active low on the port; "
                                      @"the pad is read while port 0 bit 6 is clear)."];
    NSStackView *left = vstack(@[_ioDiag, padHint, scroll]);
    left.distribution = NSStackViewDistributionFill;   /* the text takes the rest of the height */
    fill(left, scroll);

    NSTextField *ioTitle = [self label:@"Port accesses (newest last)"];
    ioTitle.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    NSStackView *ioHead = hstack(@[ioTitle, [self button:@"Clear" action:@selector(ioClear:)]]);
    NSTextField *logTitle = [self label:@"Cartridge events (register writes, bytes sent, page selects, swaps)"];
    logTitle.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    NSTextField *mailTitle = [self label:@"The mailbox ($F800-$FFFF, as the console reads it)"];
    mailTitle.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    NSStackView *right = vstack(@[ioHead, ioScroll, logTitle, logScroll, mailTitle, mailScroll]);
    right.distribution = NSStackViewDistributionFill;
    fill(right, ioScroll);
    fill(right, logScroll);
    fill(right, mailScroll);
    [logScroll.heightAnchor constraintEqualToAnchor:ioScroll.heightAnchor].active = YES;
    [mailScroll.heightAnchor constraintEqualToAnchor:ioScroll.heightAnchor].active = YES;

    NSStackView *h = hstack(@[left, right]);
    h.alignment = NSLayoutAttributeTop;
    h.spacing = 12;
    [left.widthAnchor constraintGreaterThanOrEqualToConstant:480].active = YES;
    [right.widthAnchor constraintEqualToConstant:420].active = YES;
    [left.heightAnchor constraintEqualToAnchor:h.heightAnchor].active = YES;
    [right.heightAnchor constraintEqualToAnchor:h.heightAnchor].active = YES;
    return h;
}

/* Sound: the beeper's mode, a level meter, the forced-on interval, and the
 * timeline of mode changes. */
- (NSView *)buildSound
{
    NSTextField *title = [self label:@"Beeper (port 5 bits 7-6)"];
    title.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    _soundMode = [self label:@""];
    _soundMode.font = [NSFont monospacedSystemFontOfSize:13 weight:NSFontWeightMedium];
    _soundLevel = [[NSProgressIndicator alloc] init];
    _soundLevel.indeterminate = NO;
    _soundLevel.minValue = 0;
    _soundLevel.maxValue = SOUND_LEVEL_MAX;
    [_soundLevel.widthAnchor constraintEqualToConstant:360].active = YES;
    _soundInfo = [self label:@""];
    _soundInfo.font = monoFont();
    NSStackView *head = hstack(@[[self label:@"Level"], _soundLevel, _soundInfo]);
    NSTextField *logTitle = [self label:@"Sound-mode timeline (every change of port 5 bits 7-6, newest last)"];
    logTitle.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    NSScrollView *scroll;
    _soundLog = monoView(&scroll, NO);
    NSStackView *v = vstack(@[title, _soundMode, head, logTitle, scroll]);
    v.distribution = NSStackViewDistributionFill;
    fill(v, scroll);
    return v;
}

/* Trace (idx 0) and History (idx 1): two views of the engine's one ring. */
- (NSView *)buildHist:(int)idx
{
    _hRec[idx] = [self button:@"Record" action:@selector(histRec:)];
    [_hRec[idx] setButtonType:NSButtonTypePushOnPushOff];
    _hRec[idx].toolTip = @"Record every instruction the CPU runs (the machine runs a little slower)";
    _hFilter[idx] = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_hFilter[idx] addItemsWithTitles:@[@"All instructions", @"Branches only", @"In range"]];
    _hFilter[idx].target = self;
    _hFilter[idx].action = @selector(histChanged:);
    [_hFilter[idx] setRefusesFirstResponder:YES];
    _hRange[idx] = [self field:@"range: lo-hi or a label" width:180 action:@selector(histChanged:)];
    _hNote[idx] = [self hint:@""];
    NSStackView *bar = hstack(@[_hRec[idx], _hFilter[idx], _hRange[idx],
                                [self button:@"Clear" action:@selector(histClear:)], _hNote[idx]]);
    NSScrollView *scroll;
    _hText[idx] = monoView(&scroll, NO);
    NSStackView *v = vstack(@[bar, scroll]);
    fill(v, bar);
    fill(v, scroll);
    return v;
}

/* Symbols: a filter over the table (built-ins and loaded), a click on a row
 * goes to its address in the Disassembly. */
- (NSView *)buildSymbols
{
    _symFilter = [self field:@"Filter by name (Enter to apply)" width:260 action:@selector(symFilterChanged:)];
    _symCount = [self hint:@""];
    NSStackView *bar = hstack(@[[self label:@"Filter"], _symFilter, _symCount]);

    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = YES;
    _symList = [[SymbolTextView alloc] initWithFrame:NSMakeRect(0, 0, 600, 400)];
    _symList.editable = NO;
    _symList.richText = NO;
    _symList.selectable = NO;
    _symList.font = monoFont();
    _symList.horizontallyResizable = YES;
    _symList.textContainer.widthTracksTextView = NO;
    _symList.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
    _symList.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    __weak ChfDebuggerWindow *weakSelf = self;
    _symList.onClickLine = ^(int line) {
        ChfDebuggerWindow *strongSelf = weakSelf;
        if (!strongSelf || line < 0 || line >= (int)strongSelf->_symAddrs.count) return;
        [strongSelf activateSymbol:line];
    };
    scroll.documentView = _symList;

    NSTextField *hint = [self hint:@"Click a symbol to show its address in the Disassembly (a port goes to the Memory "
                                   @"tab's IO space). Load Symbols adds a file's labels; Clear Symbols drops the loaded "
                                   @"ones (the built-ins stay); Break at: sets an execute breakpoint."];
    NSStackView *v = vstack(@[bar, scroll, hint]);
    fill(v, scroll);
    return v;
}

/* The packets left (a note and Clear over them), the cartridge's mailbox
 * right. */
- (NSView *)buildFujinet
{
    _fujiNote = [self label:@""];
    _fujiNote.textColor = NSColor.secondaryLabelColor;
    _fujiNote.lineBreakMode = NSLineBreakByTruncatingTail;
    [_fujiNote setContentHuggingPriority:NSLayoutPriorityDefaultLow
                          forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_fujiNote setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                        forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *row = hstack(@[_fujiNote, [self button:@"Clear" action:@selector(fujiClear:)]]);

    NSScrollView *fujiScroll, *stateScroll;
    _fujiText = monoView(&fujiScroll, NO);
    _fujiState = monoView(&stateScroll, NO);
    NSTextField *title = [self label:@"FujiBus packets (SLIP-decoded)"];
    title.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    NSStackView *left = vstack(@[title, row, fujiScroll]);
    left.distribution = NSStackViewDistributionFill;   /* the log takes the column's height */
    fill(left, row);
    fill(left, fujiScroll);
    NSTextField *stateTitle = [self label:@"The cartridge's mailbox"];
    stateTitle.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    NSStackView *right = vstack(@[stateTitle, stateScroll]);
    right.distribution = NSStackViewDistributionFill;
    fill(right, stateScroll);

    NSStackView *h = hstack(@[left, right]);
    h.alignment = NSLayoutAttributeTop;
    h.spacing = 12;
    [left.widthAnchor constraintGreaterThanOrEqualToConstant:520].active = YES;
    [right.widthAnchor constraintEqualToConstant:320].active = YES;
    [left.heightAnchor constraintEqualToAnchor:h.heightAnchor].active = YES;
    [right.heightAnchor constraintEqualToAnchor:h.heightAnchor].active = YES;
    return h;
}

- (void)buildWindow
{
    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 1180, 840)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"Channel F Debugger";
    _window.releasedWhenClosed = NO;
    _window.delegate = self;

    /* Toolbar rows. The keys are handled by the key monitor, not as the
     * buttons' key equivalents, so they work wherever the focus is. */
    _runBtn = [self button:@"Stop (F5)" action:@selector(toggleRun:)];
    NSButton *step = [self button:@"Step (F7)" action:@selector(step:)];
    NSButton *over = [self button:@"Step Over (F8)" action:@selector(stepOver:)];
    NSButton *outBtn = [self button:@"Step Out (⇧F8)" action:@selector(stepOut:)];
    NSButton *scan = [self button:@"Scanline" action:@selector(scanline:)];
    NSButton *frame = [self button:@"Frame" action:@selector(frame:)];
    NSButton *toCursor = [self button:@"Run to Cursor" action:@selector(runToCursor:)];
    toCursor.toolTip = @"Run until the Disassembly line you picked is reached";
    NSButton *clearBps = [self button:@"Clear Breakpoints" action:@selector(clearBreaks:)];
    NSButton *popBtn = [self button:@"Pop Out" action:@selector(popOutCurrent:)];
    popBtn.toolTip = @"Move the tab that shows into a window of its own; Dock puts it back";
    NSButton *helpBtn = ChfHelpButton(nil);
    [helpBtn setRefusesFirstResponder:YES];
    NSStackView *toolbar = hstack(@[_runBtn, step, over, outBtn, frame, scan, toCursor, clearBps, popBtn, helpBtn]);

    _breakAt = [self field:@"symbol or hex address" width:170 action:@selector(breakAt:)];
    NSButton *breakBtn = [self button:@"Break" action:@selector(breakAt:)];
    breakBtn.toolTip = @"Set an execute breakpoint at the symbol or address";
    NSPopUpButton *symbols = [self pullDown:@"Symbols…"
                                      items:@[@"From a File…", @"Beside the Cartridge"]
                                     action:@selector(loadSymbols:)];
    NSButton *clearSymbols = [self button:@"Clear Symbols" action:@selector(clearSymbols:)];
    NSMutableArray *saveTitles = [NSMutableArray array];
    for (int i = 0; i < NUM_FILE_KINDS; i++) [saveTitles addObject:kFileTitles[i]];
    NSPopUpButton *save = [self pullDown:@"Save…" items:saveTitles action:@selector(saveFile:)];
    NSStackView *toolbar2 = hstack(@[[self label:@"Break at:"], _breakAt, breakBtn, symbols, clearSymbols, save]);

    /* the stop reason gets a row of its own, so it is never cut short */
    _status = [self label:@"Running"];
    _status.textColor = NSColor.secondaryLabelColor;
    _status.lineBreakMode = NSLineBreakByTruncatingTail;
    [_status setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                      forOrientation:NSLayoutConstraintOrientationHorizontal];

    _tabs = [[NSTabView alloc] init];
    NSArray *pages = @[ [self buildConsole], [self buildDisasm], [self buildMemory], [self buildBreaks],
                        [self buildWatches], [self buildCart], [self buildHist:0], [self buildHist:1],
                        [self buildVideo], [self buildSound], [self buildSymbols], [self buildFujinet] ];
    for (int i = 0; i < NUM_TABS; i++) {
        NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:kHelpTabs[i]];
        item.label = kTabTitles[i];
        NSView *content = pages[(NSUInteger)i];
        NSView *holder = [[NSView alloc] init];
        [holder addSubview:content];
        content.translatesAutoresizingMaskIntoConstraints = NO;
        [content.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:6].active = YES;
        [content.trailingAnchor constraintEqualToAnchor:holder.trailingAnchor constant:-6].active = YES;
        [content.topAnchor constraintEqualToAnchor:holder.topAnchor constant:6].active = YES;
        [content.bottomAnchor constraintEqualToAnchor:holder.bottomAnchor constant:-6].active = YES;
        item.view = holder;
        _items[i] = item;
        [_tabs addTabViewItem:item];
    }
    const int first = tabFromText(getenv("CHF_DEBUGGER_TAB"));
    if (first >= 0 && first < (int)_tabs.numberOfTabViewItems) [_tabs selectTabViewItemAtIndex:first];
    _tabs.delegate = self;
    [self updateHelpContext];

    /* the registers sit under every tab */
    NSView *strip = [self buildRegStrip];
    NSStackView *root = vstack(@[toolbar, toolbar2, _status, _tabs, strip]);
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [toolbar.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [toolbar2.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [_status.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [_tabs.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [strip.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    _window.contentView = root;
    [_window center];
}

/* ---- actions ------------------------------------------------------------------ */

/* A line for the status label (and the console's log, when asked). */
- (void)message:(NSString *)text console:(BOOL)console
{
    _status.stringValue = text;
    if (console) [self appendConsole:[text stringByAppendingString:@"\n"]];
}

- (void)toggleRun:(id)sender
{
    (void)sender;
    const unsigned gen = chfdebug_generation(_dbg);
    if (chfdebug_is_stopped(_dbg)) chfdebug_resume(_dbg); else chfdebug_stop(_dbg);
    [self settleFrom:gen];
    [self refreshNow];
}

/* An action that moves the machine: the stop settles (or the poll catches
 * it, for a long run), then everything shown is read back. */
- (void)moved:(unsigned)gen
{
    [self settleFrom:gen];
    [self refreshNow];
}

- (void)step:(id)sender { (void)sender; const unsigned gen = chfdebug_generation(_dbg); chfdebug_step(_dbg); [self moved:gen]; }
- (void)stepOver:(id)sender { (void)sender; const unsigned gen = chfdebug_generation(_dbg); chfdebug_step_over(_dbg); [self moved:gen]; }
- (void)stepOut:(id)sender { (void)sender; const unsigned gen = chfdebug_generation(_dbg); chfdebug_step_out(_dbg); [self moved:gen]; }
- (void)scanline:(id)sender { (void)sender; const unsigned gen = chfdebug_generation(_dbg); chfdebug_scanline(_dbg, 1); [self moved:gen]; }
- (void)frame:(id)sender { (void)sender; const unsigned gen = chfdebug_generation(_dbg); chfdebug_frame(_dbg, 1); [self moved:gen]; }

/* Runs to the Disassembly line that was picked. */
- (void)runToCursor:(id)sender
{
    (void)sender;
    if (_selAddr < 0) {
        [self message:@"Pick a line in the Disassembly first (click it)" console:NO];
        return;
    }
    const unsigned gen = chfdebug_generation(_dbg);
    chfdebug_run_to(_dbg, (uint16_t)_selAddr);
    [self moved:gen];
}

- (void)ioClear:(id)sender
{
    (void)sender;
    chfdebug_io_events_clear(_dbg);
    [self refreshCart];
}

- (void)fujiClear:(id)sender
{
    (void)sender;
    chfdebug_fujinet_trace_clear(_dbg);
    [self refreshFujinet:YES];
}

- (void)appendConsole:(NSString *)text
{
    const BOOL follow = atEnd(_conOut);     /* a reader scrolled back keeps their place */
    [_conOut.textStorage appendAttributedString:[[NSAttributedString alloc] initWithString:text attributes:monoAttrs()]];
    if (follow) [_conOut scrollRangeToVisible:NSMakeRange(_conOut.string.length, 0)];
}

- (void)runCommand:(id)sender
{
    (void)sender;
    static char out[65536];
    NSString *cmd = [_conIn.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (cmd.length == 0) return;
    if (![_history.lastObject isEqualToString:cmd]) [_history addObject:cmd];
    _historyPos = (NSInteger)_history.count;
    [self appendConsole:[NSString stringWithFormat:@"> %@\n", cmd]];
    out[0] = '\0';
    const unsigned gen = chfdebug_generation(_dbg);
    chfdebug_command(_dbg, cmd.UTF8String, out, (int)sizeof out);
    NSString *text = cstr(out);
    if (text.length) [self appendConsole:[text hasSuffix:@"\n"] ? text : [text stringByAppendingString:@"\n"]];
    _conIn.stringValue = @"";
    [self settleFrom:gen];
    [self refreshNow];
}

/* In the console, Tab completes instead of moving the focus, and Up/Down
 * walk the history. */
- (BOOL)control:(NSControl *)control textView:(NSTextView *)textView doCommandBySelector:(SEL)sel
{
    (void)textView;
    if (control != _conIn) return NO;
    if (sel == @selector(moveUp:) || sel == @selector(moveDown:)) {
        const NSInteger count = (NSInteger)_history.count;
        if (count == 0) return YES;
        _historyPos += sel == @selector(moveUp:) ? -1 : 1;
        if (_historyPos < 0) _historyPos = 0;
        if (_historyPos > count) _historyPos = count;
        _conIn.stringValue = _historyPos < count ? _history[(NSUInteger)_historyPos] : @"";
        [_conIn.currentEditor setSelectedRange:NSMakeRange(_conIn.stringValue.length, 0)];
        return YES;
    }
    if (sel != @selector(insertTab:)) return NO;
    NSString *text = _conIn.stringValue;
    const NSRange space = [text rangeOfString:@" " options:NSBackwardsSearch];
    NSString *word = space.location == NSNotFound ? text : [text substringFromIndex:space.location + 1];
    char comps[4096];
    comps[0] = '\0';
    const int count = chfdebug_completions(_dbg, word.UTF8String, comps, (int)sizeof comps);
    NSString *list = cstr(comps);
    if (count == 1) {
        NSString *completion = [list componentsSeparatedByString:@"\n"][0];
        NSString *head = space.location == NSNotFound ? @"" : [text substringToIndex:space.location + 1];
        _conIn.stringValue = [NSString stringWithFormat:@"%@%@ ", head, completion];
        [_conIn.currentEditor setSelectedRange:NSMakeRange(_conIn.stringValue.length, 0)];
    } else if (count > 1) {
        if (![list hasSuffix:@"\n"]) list = [list stringByAppendingString:@"\n"];
        [self appendConsole:list];
    }
    return YES;
}

/* "From a File…" asks; "Beside the Cartridge" takes <cart>.map/.lst next to
 * the opened cartridge. */
- (void)loadSymbols:(NSPopUpButton *)sender
{
    char msg[512];
    msg[0] = '\0';
    if (sender.selectedItem.tag == 1) {
        chfdebug_load_symbols(_dbg, NULL, msg, (int)sizeof msg);
    } else {
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.title = @"Load Symbols (Macroassembler AS .map or .lst, or an \"AAAA name\" list)";
        panel.allowedContentTypes = typesForExtensions(@[@"map", @"lst", @"txt"]);
        const char *cart = chfsession_cart_path(_session);
        if (cart && cart[0])
            [panel setDirectoryURL:[NSURL fileURLWithPath:[[NSString stringWithUTF8String:cart]
                                                              stringByDeletingLastPathComponent] isDirectory:YES]];
        if ([panel runModal] != NSModalResponseOK) return;
        chfdebug_load_symbols(_dbg, [[panel URL] fileSystemRepresentation], msg, (int)sizeof msg);
    }
    [self message:cstr(msg) console:YES];
    [self refreshNow];
}

- (void)clearSymbols:(id)sender
{
    (void)sender;
    chfdebug_clear_symbols(_dbg);
    [self message:@"Loaded symbols cleared" console:YES];
    [self refreshNow];
}

- (void)saveFile:(NSPopUpButton *)sender
{
    const NSInteger kind = sender.selectedItem.tag;
    if (kind < 0 || kind >= NUM_FILE_KINDS) return;
    NSSavePanel *panel = [NSSavePanel savePanel];
    panel.title = [NSString stringWithFormat:@"Save %@", kFileTitles[kind]];
    [panel setNameFieldStringValue:kFileNames[kind]];
    if ([panel runModal] != NSModalResponseOK) return;
    char msg[512];
    msg[0] = '\0';
    chfdebug_save(_dbg, kFileKinds[kind], [[panel URL] fileSystemRepresentation], msg, (int)sizeof msg);
    [self message:cstr(msg) console:YES];
}

- (void)applyRegister:(NSTextField *)sender
{
    long value;
    NSString *why = nil;
    [_window makeFirstResponder:nil];       /* leave the field, so the refresh may rewrite it */
    if (!chfdebug_is_stopped(_dbg)) {
        [self message:@"Stop the machine (F5) to edit registers" console:NO];
    } else if (sender.tag < 0 || sender.tag >= NUM_REGS) {
        /* not one of ours */
    } else if (kRegs[sender.tag].reg == CHF_REG_ISAR) {
        if (parseIsar(sender.stringValue, &value)) chfdebug_cpu_set(_dbg, CHF_REG_ISAR, (int)value);
        else [self message:@"ISAR: octal, upper.lower (5.3)" console:NO];
    } else if ([self evaluate:sender.stringValue into:&value why:&why]) {
        chfdebug_cpu_set(_dbg, kRegs[sender.tag].reg, (int)value);
    } else {
        [self message:[NSString stringWithFormat:@"Register: %@", why] console:NO];
    }
    [self refreshCpu];
    if ([self tabVisible:TAB_DISASM]) [self refreshDisasm];
    if ([self tabVisible:TAB_MEMORY]) [self refreshMem];
}

/* A bit of W clicked (stopped only). */
- (void)flagClicked:(NSButton *)sender
{
    if (!chfdebug_is_stopped(_dbg)) {
        [self message:@"Stop the machine (F5) to change a flag" console:NO];
    } else if (sender.tag >= 0 && sender.tag < 5) {
        chfdebug_cpu_set(_dbg, kFlags[sender.tag].reg, sender.state == NSControlStateValueOn ? 1 : 0);
    }
    [self refreshCpu];
}

/* A scratchpad cell picked in the grid: its value goes in the edit box. */
- (void)pickCell:(int)reg
{
    _cellIdx = reg;
    _cellLabel.stringValue = [NSString stringWithFormat:@"r%d (octal %o%o)", reg, reg >> 3, reg & 7];
    _cellVal.stringValue = [NSString stringWithFormat:@"$%02X", _lastRegs[reg & 63]];
    [_window makeFirstResponder:_cellVal];
}

- (void)setCell:(id)sender
{
    (void)sender;
    long value;
    NSString *why = nil;
    if (_cellIdx < 0) {
        [self message:@"Click a scratchpad cell first" console:NO];
        return;
    }
    if (!chfdebug_is_stopped(_dbg)) {
        [self message:@"Stop the machine (F5) to change a register" console:NO];
        return;
    }
    if (![self evaluate:_cellVal.stringValue into:&value why:&why] || value < 0 || value > 0xFF) {
        [self message:@"Needs a byte value, such as $2A" console:NO];
        return;
    }
    chfdebug_write(_dbg, CHF_SPACE_SCRATCH, (uint32_t)_cellIdx, (uint8_t)value);
    [_window makeFirstResponder:nil];
    [self refreshCpu];
    [self refreshMem];
}

/* The space the Memory tab shows. */
- (int)memSpaceIndex
{
    const NSInteger index = _memSpace.indexOfSelectedItem;
    return index >= 0 && index < NUM_SPACES ? (int)index : CHF_SPACE_PROG;
}

/* Brings a range into the dump of a space (a couple of rows down from the
 * top) and highlights it; the scratchpad's grid outlines it. */
- (void)showSpace:(int)space lo:(uint32_t)lo hi:(uint32_t)hi
{
    if (space < 0 || space >= NUM_SPACES) return;
    if ((int)_memSpace.indexOfSelectedItem != space) {
        [_memSpace selectItemAtIndex:space];
        [self applySpaceChange];
    }
    _memBase[space] = (lo & ~0xFu) >= 32 ? (lo & ~0xFu) - 2 * 16 : 0;
    _memHl = YES;
    _memHlSpace = space;
    _memHlLo = lo;
    _memHlHi = hi;
    if (space == CHF_SPACE_SCRATCH && lo < 64) [_scratch setSelected:(int)lo];
    [self refreshMem];
}

/* A new Memory space: the dump or the scratchpad's grid, and the Watches' own
 * space follows. */
- (void)applySpaceChange
{
    const int space = [self memSpaceIndex];
    const BOOL scratch = space == CHF_SPACE_SCRATCH;
    _memScroll.hidden = scratch;
    _scratchPanel.hidden = !scratch;
    [_wtSpace selectItemAtIndex:space];
}

- (void)memSpaceChanged:(id)sender
{
    (void)sender;
    [self applySpaceChange];
    [self refreshMem];
}

- (void)gotoMem:(id)sender
{
    (void)sender;
    const int space = [self memSpaceIndex];
    uint32_t lo, hi;
    if (!chfsym_parse_range(_dbg, space, _memGoto.stringValue.UTF8String, &lo, &hi)) {
        [self message:[NSString stringWithFormat:@"Go to %@: not an address, label or range in %s", _memGoto.stringValue,
                                                 chfmem_space_name(space)]
              console:NO];
        return;
    }
    [self showSpace:space lo:lo hi:hi];
}

- (void)memPrev:(id)sender
{
    (void)sender;
    const int space = [self memSpaceIndex];
    _memBase[space] = _memBase[space] >= MEM_BYTES ? _memBase[space] - MEM_BYTES : 0;
    [self refreshMem];
}

- (void)memNext:(id)sender
{
    (void)sender;
    _memBase[[self memSpaceIndex]] += MEM_BYTES;
    [self refreshMem];
}

/* "address value [value...]": the values land at consecutive addresses of the
 * space shown. */
- (void)writeMem:(id)sender
{
    (void)sender;
    const int space = [self memSpaceIndex];
    NSArray<NSString *> *words = [_memWrite.stringValue componentsSeparatedByCharactersInSet:
        [NSCharacterSet whitespaceCharacterSet]];
    NSMutableArray<NSString *> *tokens = [NSMutableArray array];
    for (NSString *word in words)
        if (word.length) [tokens addObject:word];
    uint32_t first = 0, last = 0;
    if (tokens.count < 2 || !chfsym_parse_range(_dbg, space, tokens[0].UTF8String, &first, &last)) {
        [self message:[NSString stringWithFormat:@"Poke: address value [value...] in %s", chfmem_space_name(space)]
              console:NO];
        return;
    }
    const int size = chfdebug_space_size(_dbg, space);
    for (NSUInteger i = 1; i < tokens.count; i++) {
        long value;
        NSString *why = nil;
        if (![self evaluate:tokens[i] into:&value why:&why] || value < 0 || value > 0xFF ||
            (long)first + (long)i - 1 >= size) {
            [self message:[NSString stringWithFormat:@"Not a byte (or past the space): %@", tokens[i]] console:NO];
            return;
        }
        chfdebug_write(_dbg, space, first + (uint32_t)i - 1, (uint8_t)value);
    }
    _memWrite.stringValue = @"";
    if (space == CHF_SPACE_SCRATCH || first < _memBase[space] || first >= _memBase[space] + MEM_BYTES)
        [self showSpace:space lo:first hi:first];
    else
        [self refreshMem];
    [self refreshCpu];
}

- (void)followToggled:(id)sender
{
    (void)sender;
    _scrolledAway = NO;
    [self refreshDisasm];
}

- (void)jumpTo:(id)sender
{
    (void)sender;
    NSString *why = nil;
    const int addr = [self resolveAddr:_jump.stringValue why:&why];
    if (addr < 0) {
        [self message:[NSString stringWithFormat:@"Jump to %@: %@", _jump.stringValue, why] console:NO];
        return;
    }
    [self showDisassemblyAt:(uint16_t)addr];
}

/* The Disassembly from an address on (off the PC, until the next stop). */
- (void)showDisassemblyAt:(uint16_t)addr
{
    _followPc.state = NSControlStateValueOff;
    _scrolledAway = YES;
    _disasmAddr = addr;
    [self showTab:TAB_DISASM];
    [self refreshDisasm];
}

/* The toolbar's "Break at:": a symbol or hex address gets an execute
 * breakpoint (one already there is left as it is). */
- (void)breakAt:(id)sender
{
    (void)sender;
    NSString *why = nil;
    const int addr = [self resolveAddr:_breakAt.stringValue why:&why];
    if (addr < 0) {
        [self message:[NSString stringWithFormat:@"Break at %@: %@", _breakAt.stringValue, why] console:NO];
        return;
    }
    if (chfdebug_breakpoint_check(_dbg, (uint16_t)addr)) {
        [self message:[NSString stringWithFormat:@"A breakpoint is already set at $%04X", addr] console:NO];
        return;
    }
    chfdebug_breakpoint_toggle(_dbg, (uint16_t)addr);
    _breakAt.stringValue = @"";
    [self message:[NSString stringWithFormat:@"Breakpoint at $%04X", addr] console:NO];
    [self refreshNow];
}

- (void)symFilterChanged:(id)sender
{
    (void)sender;
    [self refreshSymbols];
}

/* A click on a symbol: the Disassembly at its address, or, for one of the
 * console's ports, the IO space at that port. */
- (void)activateSymbol:(int)line
{
    if (line < 0 || line >= (int)_symAddrs.count) return;
    const uint32_t address = (uint32_t)_symAddrs[(NSUInteger)line].intValue;
    if (_symBanks[(NSUInteger)line].intValue == CHFDEBUG_SYM_PORT) {
        [self showTab:TAB_MEMORY];
        [self showSpace:CHF_SPACE_IO lo:address hi:address];
    } else {
        [self showDisassemblyAt:(uint16_t)address];
    }
}

/* The space and range a breakpoint kind takes in its Start (and End) fields. */
static int breakpointSpace(int type)
{
    if (type & (CHFDEBUG_BP_IN | CHFDEBUG_BP_OUT)) return CHF_SPACE_IO;
    if (type == CHFDEBUG_BP_VRAM) return CHF_SPACE_VRAM;
    if (type == CHFDEBUG_BP_SCRATCH) return CHF_SPACE_SCRATCH;
    return CHF_SPACE_PROG;
}

- (void)addBreakpoint:(id)sender
{
    (void)sender;
    static const int types[8] = { CHFDEBUG_BP_EXEC, CHFDEBUG_BP_READ, CHFDEBUG_BP_WRITE, CHFDEBUG_BP_ACCESS,
                                  CHFDEBUG_BP_IN, CHFDEBUG_BP_OUT, CHFDEBUG_BP_VRAM, CHFDEBUG_BP_SCRATCH };
    const NSInteger sel = _bpType.indexOfSelectedItem;
    const int type = types[sel >= 0 && sel < 8 ? sel : 0];
    const int space = breakpointSpace(type);
    NSString *what = space == CHF_SPACE_IO ? @"port" : (space == CHF_SPACE_VRAM ? @"cell"
                     : (space == CHF_SPACE_SCRATCH ? @"register" : @"address"));
    NSString *startText = [_bpStart.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    NSString *endText = [_bpEnd.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (startText.length == 0) {
        _bpErr.stringValue = [NSString stringWithFormat:@"Give a %@", what];
        return;
    }
    uint32_t start = 0, end = 0, ignored = 0;
    BOOL ok;
    if (endText.length) {
        ok = chfsym_parse_range(_dbg, space, startText.UTF8String, &start, &ignored) &&
             chfsym_parse_range(_dbg, space, endText.UTF8String, &end, &ignored);
    } else {
        /* one field: a number, a symbol or a range lo-hi */
        ok = chfsym_parse_range(_dbg, space, startText.UTF8String, &start, &end);
    }
    if (!ok || end < start || end > 0xFFFF) {
        _bpErr.stringValue = [NSString stringWithFormat:@"Bad %@ range: %@%@%@ (in %s space)", what, startText,
                              endText.length ? @" - " : @"", endText, chfmem_space_name(space)];
        return;
    }
    const int bpId = chfdebug_breakpoint_add(_dbg, type, space, (uint16_t)start, (uint16_t)end,
                                             _bpCond.stringValue.UTF8String);
    if (bpId < 0) {
        _bpErr.stringValue = @"The breakpoint was refused: check the condition and the range";
        return;
    }
    _bpErr.stringValue = @"";
    _bpStart.stringValue = @"";
    _bpEnd.stringValue = @"";
    _bpCond.stringValue = @"";
    [self refreshNow];
}

- (void)bpEnabled:(NSButton *)sender
{
    chfdebug_breakpoint_enable(_dbg, (int)sender.tag, sender.state == NSControlStateValueOn);
    [self refreshNow];
}

- (void)bpRemove:(NSButton *)sender
{
    chfdebug_breakpoint_remove(_dbg, (int)sender.tag);
    [self refreshNow];
}

- (void)clearBreaks:(id)sender
{
    (void)sender;
    chfdebug_breakpoint_clear(_dbg);
    [self refreshNow];
}

- (void)clearHits:(id)sender
{
    (void)sender;
    chfdebug_breakpoint_clear_hits(_dbg);
    _bpSig = nil;       /* the hit counts redraw */
    [self refreshNow];
}

/* ---- watches, search, trace, pop out ---------------------------------------------- */

- (void)addWatch:(id)sender
{
    (void)sender;
    NSString *text = [_wtEntry.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    NSString *rest = nil;
    const int space = splitSpace(text, (int)_wtSpace.indexOfSelectedItem, &rest);
    if (text.length == 0 || chfwatch_add(space >= 0 ? space : CHF_SPACE_PROG, rest.UTF8String) < 0) {
        [self message:@"Type an address, label, expression or lo-hi range to watch" console:NO];
        return;
    }
    _wtEntry.stringValue = @"";
    [self refreshWatches];
    if ([self tabVisible:TAB_MEMORY]) [self refreshMem];
}

/* A breakpoint over lo..hi of a space that fires when the value changes. */
- (void)breakOnChangeSpace:(int)space lo:(uint32_t)lo hi:(uint32_t)hi
{
    int type;
    switch (space) {
    case CHF_SPACE_PROG:    type = CHFDEBUG_BP_WRITE; break;
    case CHF_SPACE_SCRATCH: type = CHFDEBUG_BP_SCRATCH; break;
    case CHF_SPACE_VRAM:    type = CHFDEBUG_BP_VRAM; break;
    case CHF_SPACE_IO:      type = CHFDEBUG_BP_OUT; break;
    default:
        [self message:@"The cartridge's memory has no breakpoint to break on" console:NO];
        return;
    }
    if (hi > 0xFFFF || chfdebug_breakpoint_add(_dbg, type, space, (uint16_t)lo, (uint16_t)hi, "value != old") < 0) {
        [self message:@"The breakpoint was refused" console:NO];
        return;
    }
    [self message:@"Breaking when it changes" console:NO];
    [self refreshNow];
}

/* Selects a tab, or raises the window it was popped out into. */
- (void)showTab:(int)tab
{
    if (_popped[tab]) {
        [_popped[tab] makeKeyAndOrderFront:nil];
        return;
    }
    [_tabs selectTabViewItem:_items[tab]];
}

- (void)wtMemory:(NSButton *)sender
{
    uint32_t lo, hi;
    if (!chfwatch_range(_dbg, (int)sender.tag, &lo, &hi)) {
        [self message:@"That watch does not resolve" console:NO];
        return;
    }
    [self showTab:TAB_MEMORY];
    [self showSpace:chfwatch_space((int)sender.tag) lo:lo hi:hi];
}

- (void)wtBreak:(NSButton *)sender
{
    uint32_t lo, hi;
    if (!chfwatch_range(_dbg, (int)sender.tag, &lo, &hi)) {
        [self message:@"That watch does not resolve" console:NO];
        return;
    }
    [self breakOnChangeSpace:chfwatch_space((int)sender.tag) lo:lo hi:hi];
}

- (void)wtRemove:(NSButton *)sender
{
    chfwatch_remove((int)sender.tag);
    [self refreshWatches];
    if ([self tabVisible:TAB_MEMORY]) [self refreshMem];
}

- (void)searchBackward:(BOOL)backward
{
    uint8_t pattern[CHFSEARCH_MAX], mask = 0xFF;
    int length = 0, flags = 0;
    const int space = [self memSpaceIndex];
    if (!chfsearch_parse((int)_srchKind.indexOfSelectedItem, _srchEntry.stringValue.UTF8String, pattern, &length,
                         &mask, &flags) || length <= 0) {
        _srchNote.stringValue = @"Type hex bytes (F8 09) or some text.";
        return;
    }
    if (!chfdebug_is_stopped(_dbg)) {
        _srchNote.stringValue = @"Stop the machine first.";
        return;
    }
    /* from the highlighted hit, else from the top of the dump (the match is
     * strictly after or before `from`, wrapping once) */
    uint32_t from = _memBase[space] ? _memBase[space] - 1 : 0;
    if (_memHl && _memHlSpace == space) from = _memHlLo;
    uint32_t found = 0;
    const int result = chfdebug_search_next(_dbg, space, pattern, length, mask, flags, from, backward ? 1 : 0, &found);
    if (result < 0) {
        _srchNote.stringValue = @"Stop the machine first.";
    } else if (result == 0) {
        _srchNote.stringValue = [NSString stringWithFormat:@"Not found in %s.", chfmem_space_name(space)];
    } else {
        _srchNote.stringValue = [NSString stringWithFormat:@"Found at $%0*X", memDigits(space), found];
        [self showSpace:space lo:found hi:found + (uint32_t)length - 1];
    }
}

- (void)searchNext:(id)sender { (void)sender; [self searchBackward:NO]; }
- (void)searchPrev:(id)sender { (void)sender; [self searchBackward:YES]; }

/* Both Record buttons follow the engine's one recorder. */
- (void)syncRecord:(BOOL)on
{
    for (int k = 0; k < 2; k++) {
        const NSInteger want = on ? NSControlStateValueOn : NSControlStateValueOff;
        if (_hRec[k].state != want) {
            _hRec[k].state = want;
            setButtonAccent(_hRec[k], on);
        }
    }
}

- (void)histRec:(NSButton *)sender
{
    chfdebug_trace_enable(_dbg, sender.state == NSControlStateValueOn);
    [self syncRecord:chfdebug_trace_enabled(_dbg) != 0];
    [self refreshTab];
}

- (void)histChanged:(id)sender
{
    (void)sender;
    [self refreshTab];
}

- (void)histClear:(id)sender
{
    (void)sender;
    chfdebug_trace_clear(_dbg);
    [self refreshTab];
}

- (void)popOutCurrent:(id)sender
{
    (void)sender;
    const NSInteger tab = [self currentTab];
    if (tab >= 0) [self popOutTab:(int)tab];
}

/* Moves a tab into a window of its own. One tab always stays. */
- (void)popOutTab:(int)tab
{
    if (tab < 0 || tab >= NUM_TABS || _popped[tab] || _tabs.numberOfTabViewItems <= 1) return;
    NSTabViewItem *item = _items[tab];
    NSView *view = item.view;
    [_tabs removeTabViewItem:item];

    NSWindow *win = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 820, 580)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    win.title = [@"Channel F Debugger - " stringByAppendingString:kTabTitles[tab]];
    win.releasedWhenClosed = NO;
    win.delegate = self;
    NSButton *dockBtn = [self button:@"Dock" action:@selector(dockButton:)];
    dockBtn.tag = tab;
    dockBtn.toolTip = @"Put this tab back in the debugger";
    NSView *root = [[NSView alloc] init];
    dockBtn.translatesAutoresizingMaskIntoConstraints = NO;
    view.translatesAutoresizingMaskIntoConstraints = NO;
    [root addSubview:dockBtn];
    [root addSubview:view];
    [dockBtn.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:8].active = YES;
    [dockBtn.topAnchor constraintEqualToAnchor:root.topAnchor constant:8].active = YES;
    [view.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:8].active = YES;
    [view.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-8].active = YES;
    [view.topAnchor constraintEqualToAnchor:dockBtn.bottomAnchor constant:6].active = YES;
    [view.bottomAnchor constraintEqualToAnchor:root.bottomAnchor constant:-8].active = YES;
    win.contentView = root;
    ChfSetHelpContext(win, [@"debugger/" stringByAppendingString:kHelpTabs[tab]]);
    _popped[tab] = win;
    [win center];
    [win makeKeyAndOrderFront:nil];
    [self updateHelpContext];
    [self refreshTab];
}

- (void)dockButton:(id)sender
{
    [self dockTab:(int)((NSButton *)sender).tag];
}

/* Puts a popped-out tab back where it was in the order. */
- (void)dockTab:(int)tab
{
    if (tab < 0 || tab >= NUM_TABS) return;
    NSWindow *win = _popped[tab];
    if (!win) return;
    NSTabViewItem *item = _items[tab];
    NSView *view = item.view;
    int position = 0;
    _popped[tab] = nil;
    win.delegate = nil;
    [view removeFromSuperview];
    view.translatesAutoresizingMaskIntoConstraints = YES;     /* the tab view sizes it again */
    item.view = view;
    for (int t = 0; t < tab; t++)
        if (!_popped[t]) position++;
    [_tabs insertTabViewItem:item atIndex:position];
    [_tabs selectTabViewItem:item];
    [win orderOut:nil];
    [self updateHelpContext];
    [self refreshTab];
}

/* ---- refreshers ------------------------------------------------------------------ */

/* Everything the window shows, read back now: the status and the visible
 * tab. The poll compares against what was seen here. */
- (void)refreshNow
{
    _seenGen = chfdebug_generation(_dbg);
    _wasStopped = chfdebug_is_stopped(_dbg) != 0;
    _liveTicks = 0;
    [self refreshStatus];
    [self refreshTab];
}

/* The registers, and each tab on show (the notebook's page and any popped-out
 * window): the others refresh when they are selected. */
- (void)refreshTab
{
    [self refreshCpu];
    if (![self tabVisible:TAB_VIDEO]) _videoShown = NO;
    for (int t = 0; t < NUM_TABS; t++)
        if ([self tabVisible:t]) [self refreshTabAt:t];
}

- (void)refreshTabAt:(int)tab
{
    switch (tab) {
    case TAB_DISASM: [self refreshDisasm]; break;
    case TAB_MEMORY: [self refreshMem]; break;
    case TAB_BREAKS: [self refreshBps]; break;
    case TAB_WATCHES: [self refreshWatches]; break;
    case TAB_CART: [self refreshCart]; break;
    case TAB_TRACE: [self refreshHist:0]; break;
    case TAB_HIST: [self refreshHist:1]; break;
    case TAB_VIDEO: [self refreshVideo]; break;
    case TAB_SOUND: [self refreshSound]; break;
    case TAB_SYMBOLS: [self refreshSymbols]; break;
    case TAB_FUJINET: [self refreshFujinet:NO]; break;
    default: break;
    }
}

- (void)refreshStatus
{
    char reason[160];
    int addr = -1;
    const BOOL stopped = chfdebug_is_stopped(_dbg) != 0;
    reason[0] = '\0';
    chfdebug_stop_reason(_dbg, reason, (int)sizeof reason, &addr);
    NSString *why = cstr(reason);
    _status.stringValue = stopped ? [NSString stringWithFormat:@"Stopped%@%@", why.length ? @": " : @"", why] : @"Running";
    _runBtn.title = stopped ? @"Run (F5)" : @"Stop (F5)";
    setButtonAccent(_runBtn, stopped);    /* the accent while stopped */
}

- (void)refreshCpu
{
    chfdebug_cpu c;
    chfdebug_cpu_get(_dbg, &c);
    const unsigned vals[NUM_REGS] = { c.a, c.w, c.isar, c.pc0, c.pc1, c.dc0, c.dc1 };
    for (int i = 0; i < NUM_REGS; i++) {
        if (_reg[i].currentEditor) continue;        /* never over a field being edited */
        switch (kRegs[i].digits) {
        case 4:  _reg[i].stringValue = [NSString stringWithFormat:@"%04X", vals[i] & 0xffff]; break;
        case 2:  _reg[i].stringValue = [NSString stringWithFormat:@"%02X", vals[i] & 0xff]; break;
        default: _reg[i].stringValue = [NSString stringWithFormat:@"%o.%o", (vals[i] >> 3) & 7, vals[i] & 7]; break;
        }
    }
    const int flags[5] = { c.sign, c.carry, c.zero, c.overflow, c.icb };
    for (int i = 0; i < 5; i++) _flag[i].state = flags[i] ? NSControlStateValueOn : NSControlStateValueOff;
    _kdepth.stringValue = c.k_depth < 0 ? [NSString stringWithFormat:@"K depth -  (r59 $%02X)", c.r[59]]
                                        : [NSString stringWithFormat:@"K depth %d  (r59 $%02X)", c.k_depth, c.r[59]];
    NSString *bios = c.bios == CHF_ROM_SL31253 || c.bios == CHF_ROM_SL90025
        ? cstr(chfsession_rom_item(c.bios) ? chfsession_rom_item(c.bios)->file : "")
        : @"the app's stand-in";
    _beam.stringValue = [NSString stringWithFormat:
        @"PC $%04X   %s, BIOS %@   line %d   cycle %d   frame %u   cycles %llu   instructions %llu   illegal %u%@",
        c.pc & 0xffff, chf_machine_name(c.machine), bios, c.vpos, c.hpos, c.frame, (unsigned long long)c.cycles,
        (unsigned long long)c.instructions, c.illegal_count,
        c.illegal_pc >= 0 ? [NSString stringWithFormat:@" (last at $%04X)", c.illegal_pc & 0xffff] : @""];
    memcpy(_lastRegs, c.r, sizeof _lastRegs);
}

/* The scratchpad's grid, the picked register's value and the BIOS's K stack. */
- (void)refreshScratch
{
    chfdebug_cpu c;
    chfdebug_cpu_get(_dbg, &c);
    memcpy(_lastRegs, c.r, sizeof _lastRegs);
    [_scratch setRegs:c.r isar:c.isar];
    if (_cellIdx >= 0 && !_cellVal.currentEditor)
        _cellVal.stringValue = [NSString stringWithFormat:@"$%02X", c.r[_cellIdx & 63]];

    BOOL watched[64] = { NO };
    for (int i = 0; i < chfwatch_count(); i++) {
        uint32_t lo, hi;
        if (chfwatch_space(i) == CHF_SPACE_SCRATCH && chfwatch_range(_dbg, i, &lo, &hi))
            for (uint32_t r = lo; r <= hi && r < 64; r++) watched[r] = YES;
    }
    [_scratch setWatched:watched];
    if (_memHl && _memHlSpace == CHF_SPACE_SCRATCH) [_scratch setHighlightLo:(int)_memHlLo hi:(int)_memHlHi];
    else [_scratch setHighlightLo:-1 hi:-1];

    /* the BIOS's K stack: r59 points past the saved returns in r40-r58 */
    NSMutableString *text = [NSMutableString string];
    if (c.k_depth < 0) {
        [text appendFormat:@"r59 = $%02X is not a K-stack pointer (it must be even, 40-58)\n", c.r[59]];
    } else {
        [text appendFormat:@"depth %d   (r59 = $%02X)\n", c.k_depth, c.r[59]];
        for (int i = c.k_depth - 1; i >= 0 && i < 10; i--) {
            char label[64];
            label[0] = '\0';
            chfdebug_address_label(_dbg, c.k_stack[i], label, (int)sizeof label);
            [text appendFormat:@" [%d] $%04X  %@\n", i, c.k_stack[i], cstr(label)];
        }
        if (c.k_depth == 0) [text appendString:@" (empty)\n"];
    }
    setText(_kstack, text, NO);
}

/* The Memory tab: the scratchpad's grid, or a hex dump of the space chosen a
 * page at a time. A PROG row ends with the name of the region it lies in (one
 * chfdebug_memory_map per refresh), a VRAM row with its row and column. The
 * highlighted range is the accent fill, the watched ranges the selection
 * colour. */
- (void)refreshMem
{
    static chfdebug_region regions[MAX_REGIONS];
    const int space = [self memSpaceIndex];
    if (space == CHF_SPACE_SCRATCH) {
        [self refreshScratch];
        return;
    }
    const uint32_t size = (uint32_t)chfdebug_space_size(_dbg, space);
    uint32_t base = _memBase[space];
    if (size > MEM_BYTES && base > size - MEM_BYTES) base = size - MEM_BYTES;
    if (size <= MEM_BYTES) base = 0;
    base &= ~0xFu;
    _memBase[space] = base;
    const int digits = memDigits(space);

    uint8_t data[MEM_BYTES];
    memset(data, 0, sizeof data);
    const int got = chfdebug_read(_dbg, space, base, data, MEM_BYTES);
    int regionCount = 0;
    if (space == CHF_SPACE_PROG) {
        regionCount = chfdebug_memory_map(_dbg, regions, MAX_REGIONS);
        if (regionCount < 0) regionCount = 0;
        if (regionCount > MAX_REGIONS) regionCount = MAX_REGIONS;
    }

    uint32_t watchLo[CHFWATCH_MAX], watchHi[CHFWATCH_MAX];
    int watchCount = 0;
    for (int i = 0; i < chfwatch_count() && watchCount < CHFWATCH_MAX; i++) {
        uint32_t lo, hi;
        if (chfwatch_space(i) == space && chfwatch_range(_dbg, i, &lo, &hi)) {
            watchLo[watchCount] = lo;
            watchHi[watchCount] = hi;
            watchCount++;
        }
    }

    NSMutableString *text = [NSMutableString stringWithFormat:@"%*s  0  1  2  3  4  5  6  7   8  9  A  B  C  D  E  F\n",
                                                             digits, ""];
    NSMutableArray<NSValue *> *marks = [NSMutableArray array];          /* the highlighted bytes' cells */
    NSMutableArray<NSValue *> *watchMarks = [NSMutableArray array];     /* the watched ones */
    const BOOL hl = _memHl && _memHlSpace == space;
    for (int row = 0; row < MEM_ROWS; row++) {
        const uint32_t rowBase = base + (uint32_t)row * 16;
        if (rowBase >= size) break;
        [text appendFormat:@"%0*X  ", digits, rowBase];
        for (int pass = 0; pass < 2; pass++) {
            for (int col = 0; col < 16; col++) {
                const uint32_t addr = rowBase + (uint32_t)col;
                if (pass == 0 && col == 8) [text appendString:@" "];
                NSMutableArray<NSValue *> *target = nil;
                if (hl && addr >= _memHlLo && addr <= _memHlHi) {
                    target = marks;
                } else {
                    for (int w = 0; w < watchCount; w++)
                        if (addr >= watchLo[w] && addr <= watchHi[w]) { target = watchMarks; break; }
                }
                const uint8_t byte = data[row * 16 + col];
                if (target) [target addObject:[NSValue valueWithRange:NSMakeRange(text.length, pass == 0 ? 2 : 1)]];
                if (pass == 0) [text appendFormat:@"%02X ", byte];
                else [text appendFormat:@"%c", (byte >= 0x20 && byte < 0x7F) ? (char)byte : '.'];
            }
            if (pass == 0) [text appendString:@" "];
        }
        if (space == CHF_SPACE_PROG) {
            char region[64];
            region[0] = '\0';
            chfmem_region_name(regions, regionCount, (uint16_t)rowBase, region, (int)sizeof region);
            [text appendFormat:@"  %s\n", region];
        } else if (space == CHF_SPACE_VRAM) {
            [text appendFormat:@"  row %u col %u\n", rowBase / CHFDEBUG_VRAM_COLS, rowBase % CHFDEBUG_VRAM_COLS];
        } else {
            [text appendString:@"\n"];
        }
    }
    if (got <= 0) [text appendString:@"\n(the machine is not running)\n"];
    NSMutableAttributedString *styled = [[NSMutableAttributedString alloc] initWithString:text attributes:monoAttrs()];
    for (NSValue *value in watchMarks)
        [styled addAttribute:NSBackgroundColorAttributeName value:NSColor.selectedTextBackgroundColor
                       range:value.rangeValue];
    for (NSValue *value in marks) [styled addAttributes:accentAttrs() range:value.rangeValue];
    setStyledText(_mem, styled);
}

- (void)refreshDisasm
{
    static chfdebug_line lines[DISASM_WINDOW];
    int pcLine = -1;
    if (_followPc.state == NSControlStateValueOn) {
        chfdebug_cpu c;
        chfdebug_cpu_get(_dbg, &c);
        /* the PC a third of the way down */
        _disasmAddr = (uint16_t)chfdebug_row_address(_dbg, (uint16_t)c.pc, -(DISASM_WINDOW / 3));
    }
    int n = chfdebug_disassemble(_dbg, _disasmAddr, lines, DISASM_WINDOW, &pcLine);
    if (n < 0) n = 0;
    if (n > DISASM_WINDOW) n = DISASM_WINDOW;
    _lineCount = n;

    /* "●> 0038  F5          LABEL            PUSH AF      ; note": the
     * ranges are the line, its label and its comment, for the colours */
    NSMutableString *text = [NSMutableString string];
    NSRange ranges[DISASM_WINDOW], labels[DISASM_WINDOW], notes[DISASM_WINDOW];
    for (int i = 0; i < n; i++) {
        _lineAddr[i] = lines[i].address;
        /* a red dot for a breakpoint, a grey ring for a disabled one */
        NSString *gutter = lines[i].has_breakpoint ? @"●" : (lines[i].bp_disabled ? @"○" : @" ");
        NSString *label = cstr(lines[i].label), *comment = cstr(lines[i].comment);
        NSMutableString *line = [NSMutableString stringWithFormat:@"%@%c %04X  ", gutter,
                                 lines[i].is_pc ? '>' : ' ', lines[i].address];
        [line appendString:padTo(cstr(lines[i].bytes), 11)];
        [line appendString:@"  "];
        labels[i] = NSMakeRange(text.length + line.length, label.length);
        [line appendString:padTo(label, 16)];
        [line appendString:@" "];
        [line appendString:padTo(cstr(lines[i].disasm), 24)];
        notes[i] = NSMakeRange(NSNotFound, 0);
        if (comment.length) {
            notes[i] = NSMakeRange(text.length + line.length, comment.length + 3);
            [line appendString:@" ; "];
            [line appendString:comment];
        }
        ranges[i] = NSMakeRange(text.length, line.length);
        [text appendString:line];
        [text appendString:@"\n"];
    }
    if (n == 0) [text appendString:@"(no disassembly: the debugger is not attached)\n"];

    NSMutableAttributedString *styled = [[NSMutableAttributedString alloc] initWithString:text attributes:monoAttrs()];
    for (int i = 0; i < n; i++) {
        /* labels and comments grey */
        if (labels[i].length)
            [styled addAttribute:NSForegroundColorAttributeName value:NSColor.systemGrayColor range:labels[i]];
        if (notes[i].location != NSNotFound)
            [styled addAttribute:NSForegroundColorAttributeName value:NSColor.systemGrayColor range:notes[i]];
        /* the gutter: a breakpoint red, a disabled one grey */
        if (lines[i].has_breakpoint || lines[i].bp_disabled)
            [styled addAttribute:NSForegroundColorAttributeName
                           value:lines[i].has_breakpoint ? breakpointRed() : NSColor.systemGrayColor
                           range:NSMakeRange(ranges[i].location, 1)];
    }
    /* the line picked for Run to Cursor */
    for (int i = 0; i < n; i++)
        if (_selAddr >= 0 && lines[i].address == _selAddr && !lines[i].is_pc)
            [styled addAttribute:NSBackgroundColorAttributeName value:NSColor.selectedTextBackgroundColor
                           range:ranges[i]];
    /* the PC's line in the accent colour with dark ink, last so its
     * colours win */
    for (int i = 0; i < n; i++)
        if (lines[i].is_pc) [styled addAttributes:accentAttrs() range:ranges[i]];
    [_disasm.textStorage setAttributedString:styled];
    NSScrollView *scroll = _disasm.enclosingScrollView;
    [scroll.contentView scrollToPoint:NSMakePoint(scroll.contentView.bounds.origin.x, 0)];
    [scroll reflectScrolledClipView:scroll.contentView];
}

/* The Video tab: the controller says what changed. Forced when the tab
 * has just come on show. */
- (void)refreshVideo
{
    [_video refreshForce:!_videoShown];
    _videoShown = YES;
}

- (void)refreshBps
{
    static chfdebug_breakpoint bps[MAX_BPS];
    int n = chfdebug_breakpoint_list(_dbg, bps, MAX_BPS);
    if (n < 0) n = 0;
    if (n > MAX_BPS) n = MAX_BPS;

    /* the first line: why it stopped, or that it runs */
    {
        char reason[160];
        int where = -1;
        reason[0] = '\0';
        chfdebug_stop_reason(_dbg, reason, (int)sizeof reason, &where);
        NSString *why = cstr(reason);
        _bpStop.stringValue = chfdebug_is_stopped(_dbg)
            ? [NSString stringWithFormat:@"Stopped: %@", why.length ? why : @"stopped"]
            : @"Running.";
    }

    /* the rows are rebuilt only when something in them changed (a refresh
     * must not eat a click) */
    NSMutableString *signature = [NSMutableString string];
    NSMutableArray<NSString *> *descriptions = [NSMutableArray array];
    for (int i = 0; i < n; i++) {
        char desc[256];
        desc[0] = '\0';
        chfbp_describe(_dbg, &bps[i], desc, (int)sizeof desc);
        [descriptions addObject:cstr(desc)];
        [signature appendFormat:@"%d %d %u %@\n", bps[i].id, bps[i].enabled, bps[i].hits, descriptions[(NSUInteger)i]];
    }
    if ([signature isEqualToString:_bpSig]) return;
    _bpSig = signature;

    for (NSView *v in [_bpList.arrangedSubviews copy]) [_bpList removeView:v];
    if (n == 0) [_bpList addArrangedSubview:[self label:@"No breakpoints"]];
    for (int i = 0; i < n; i++) {
        NSButton *box = [NSButton checkboxWithTitle:[NSString stringWithFormat:@"#%d", bps[i].id]
                                             target:self action:@selector(bpEnabled:)];
        box.state = bps[i].enabled ? NSControlStateValueOn : NSControlStateValueOff;
        box.tag = bps[i].id;
        box.font = monoFont();
        [box.widthAnchor constraintEqualToConstant:70].active = YES;
        NSButton *remove = [self button:@"Remove" action:@selector(bpRemove:)];
        remove.tag = bps[i].id;
        [remove.widthAnchor constraintEqualToConstant:70].active = YES;
        NSStackView *row = hstack(@[box, tableCell(descriptions[(NSUInteger)i], 0, NO),
                                    tableCell([NSString stringWithFormat:@"%u", bps[i].hits], 60, NO), remove]);
        row.distribution = NSStackViewDistributionFill;
        [_bpList addArrangedSubview:row];
        [row.widthAnchor constraintEqualToAnchor:_bpList.widthAnchor].active = YES;
    }
}

- (void)refreshWatches
{
    const int n = chfwatch_count();
    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    NSMutableString *signature = [NSMutableString string];
    for (int i = 0; i < n; i++) {
        char buf[256];
        buf[0] = '\0';
        chfwatch_format(_dbg, i, buf, (int)sizeof buf);
        [lines addObject:cstr(buf)];
        [signature appendFormat:@"%@\n", lines[(NSUInteger)i]];
    }
    [signature appendFormat:@"%u", chfwatch_generation()];
    if ([signature isEqualToString:_wtSig]) return;      /* a refresh must not eat a click */
    _wtSig = signature;

    for (NSView *v in [_wtList.arrangedSubviews copy]) [_wtList removeView:v];
    if (n == 0) [_wtList addArrangedSubview:[self label:@"No watches"]];
    for (int i = 0; i < n; i++) {
        NSButton *mem = [self button:@"Show in Memory" action:@selector(wtMemory:)];
        mem.tag = i;
        NSButton *brk = [self button:@"Break on Change" action:@selector(wtBreak:)];
        brk.tag = i;
        NSButton *remove = [self button:@"Remove" action:@selector(wtRemove:)];
        remove.tag = i;
        NSStackView *row = hstack(@[tableCell(lines[(NSUInteger)i], 0, NO), mem, brk, remove]);
        row.distribution = NSStackViewDistributionFill;
        [_wtList addArrangedSubview:row];
        [row.widthAnchor constraintEqualToAnchor:_wtList.widthAnchor].active = YES;
    }
}

static NSString *soundModeName(int mode)
{
    switch (mode) {
    case 1:  return @"1 kHz";
    case 2:  return @"500 Hz";
    case 3:  return @"120 Hz";
    default: return @"off";
    }
}

static NSString *bootStateName(int state)
{
    switch (state) {
    case 0:    return @"idle";
    case 1:    return @"transferring";
    case 2:    return @"ready";
    case 0x80: return @"failed";
    default:   return @"?";
    }
}

static NSString *bootErrName(int error)
{
    switch (error) {
    case 0: return @"none";
    case 1: return @"too big";
    case 2: return @"truncated";
    case 3: return @"no mapper";
    case 4: return @"store busy";
    default: return @"?";
    }
}

static NSString *linkErrName(int error)
{
    switch (error) {
    case 0: return @"ok";
    case 1: return @"no link";
    case 2: return @"timeout";
    case 3: return @"bad frame";
    case 4: return @"too big";
    default: return @"?";
    }
}

/* A hex dump, 16 bytes a line from `base`. */
static NSString *hexDump(const uint8_t *data, int count, unsigned base)
{
    NSMutableString *text = [NSMutableString string];
    for (int i = 0; i < count; i += 16) {
        [text appendFormat:@"$%04X: ", base + (unsigned)i];
        for (int k = 0; k < 16 && i + k < count; k++) [text appendFormat:@"%02X ", data[i + k]];
        [text appendString:@"\n"];
    }
    return text;
}

/* Cart & I/O: the cartridge, then the console's ports, the 2102's latch and
 * the memory map; the port accesses, the cartridge's events and the mailbox
 * beside them. */
- (void)refreshCart
{
    static chfdebug_region regions[MAX_REGIONS];
    static chfdebug_cart_event cartEvents[CARTEV_MAX];
    static chfdebug_io_event ioEvents[IOEV_MAX];
    chfdebug_cart c;
    chfdebug_io io;
    chfdebug_video v;
    char info[256];
    memset(&c, 0, sizeof c);
    memset(&io, 0, sizeof io);
    memset(&v, 0, sizeof v);
    info[0] = '\0';
    chfdebug_cart_get(_dbg, &c);
    chfdebug_cart_info(_dbg, info, (int)sizeof info);
    chfdebug_io_get(_dbg, &io);
    chfdebug_video_get(_dbg, &v);
    [_ioDiag setIo:&io];

    NSMutableString *s = [NSMutableString stringWithFormat:@"%@\n\n", cstr(info)];
    if (!c.present) {
        [s appendString:@"(no cartridge in the slot)\n"];
    } else {
        [s appendFormat:@"Type        %@\n", cstr(c.type_name)];
        const BOOL fuji = c.type_name && !strcmp(c.type_name, "FujiNet");
        if (fuji) {
            [s appendFormat:@"Link        %s%s\n", c.link_up ? "up" : "down", c.link_off ? " (switched off)" : ""];
            [s appendFormat:@"Mailbox     %s   %s%s\n", c.mailbox ? "open (the app claims it)" : "closed",
                c.app_live ? "an app is live" : "CONFIG is live", c.booted_game ? "   a game has booted" : ""];
            [s appendFormat:@"Live image  %u bytes   CRC-32 %08X\n", c.live_size, c.live_crc];
            if (c.staged_size)
                [s appendFormat:@"Staged      %s%s   %u bytes   CRC-32 %08X%s\n", c.staged_app ? "a FujiNet app" : "a game",
                    c.staged_mailbox ? " (claims the mailbox)" : "", c.staged_size, c.staged_crc,
                    c.armed ? "   armed" : ""];
            else
                [s appendString:@"Staged      nothing\n"];
            [s appendFormat:@"Pages       %d APPBANK page%s, %@ at $4800\n", c.npages, c.npages == 1 ? "" : "s",
                c.page >= 0 ? [NSString stringWithFormat:@"page %d", c.page] : @"none"];
            [s appendFormat:@"Swaps       %d   queue %u events waiting\n", c.swaps, c.queue_depth];
            [s appendFormat:@"Boot        state %d (%@)   %d%%   error %d (%@)\n", c.boot_state,
                bootStateName(c.boot_state), c.boot_pct, c.boot_err, bootErrName(c.boot_err)];
            const uint8_t *p = c.status_page;
            [s appendString:@"\nStatus page $FC00-$FC0F\n"];
            [s appendFormat:@"  $FC00 ACKSEQ      $%02X\n", p[0]];
            [s appendFormat:@"  $FC01 STATUS      $%02X  (link up %d, busy %d)\n", p[1], p[1] & 1, (p[1] >> 1) & 1];
            [s appendFormat:@"  $FC02 ERR         %d (%@)\n", p[2], linkErrName(p[2])];
            [s appendFormat:@"  $FC03 REPLY_CMD   $%02X%s\n", p[3], p[3] == 0x06 ? "  ACK" : p[3] == 0x15 ? "  NAK" : ""];
            [s appendFormat:@"  $FC04 RXLEN       %u\n", p[4] | (p[5] << 8)];
            [s appendFormat:@"  $FC06 BOOT_STATE  %d (%@)\n", p[6], bootStateName(p[6])];
            [s appendFormat:@"  $FC07 BOOT_PCT    %d%%\n", p[7]];
            [s appendFormat:@"  $FC08 BOOT_ERR    %d (%@)\n", p[8], bootErrName(p[8])];
            [s appendFormat:@"  $FC09 MAGIC       %c%c   PROTO_VER %d\n", p[9] >= 0x20 && p[9] < 0x7f ? p[9] : '.',
                p[10] >= 0x20 && p[10] < 0x7f ? p[10] : '.', p[11]];
            [s appendFormat:@"  $FC0E TICK        %u  (a free-running 60 Hz count)\n", c.tick];
        } else {
            [s appendFormat:@"Software list  %@\n", c.softlist_name ? cstr(c.softlist_name) : @"(not in MAME's list)"];
            if (c.description && c.description[0]) [s appendFormat:@"              %@\n", cstr(c.description)];
            [s appendFormat:@"ROM          %u bytes   CRC-32 %08X\n", c.rom_size, c.rom_crc];
            if (c.ram_size) [s appendFormat:@"RAM          %u %s\n", c.ram_size, c.ram_size == 1024 ? "bits (a 2102)" : "bytes"];
            if (c.type_name && !strncmp(c.type_name, "Multigame", 9))
                [s appendFormat:@"Bank latch   %d   half %d\n", c.bank, c.half];
        }
    }

    /* the console's ports: raw, complemented and decoded */
    [s appendString:@"\nPorts (the CPU's output latches)\n"];
    [s appendFormat:@"  port 0  $%02X  ~$%02X  strobe %s, controllers %s, panel latch $%X\n", v.port0, (uint8_t)~v.port0,
        v.strobe ? "up" : "down", v.controllers_enabled ? "read" : "off", v.panel_latch & 0xf];
    [s appendFormat:@"  port 1  $%02X  ~$%02X  colour value %d\n", v.port1, (uint8_t)~v.port1, v.value];
    [s appendFormat:@"  port 4  $%02X  ~$%02X  column %d\n", v.port4, (uint8_t)~v.port4, v.col];
    [s appendFormat:@"  port 5  $%02X  ~$%02X  row %d, sound %@\n", v.port5, (uint8_t)~v.port5, v.row,
        soundModeName(v.sound_mode)];
    [s appendFormat:@"The CPU reads  port 0 $%02X   port 1 $%02X   port 4 $%02X   port 5 $%02X   (active low)\n",
        v.in0, v.in1, v.in4, v.in5];
    [s appendFormat:@"Player 1 (right, port 1)  $%02X     Player 2 (left, port 4)  $%02X   (a bit per contact: 0 right, "
                    @"1 left, 2 back, 3 forward, 4 CCW, 5 CW, 6 pull, 7 push)\n", io.controller[0], io.controller[1]];
    [s appendFormat:@"Console panel  $%X  (TIME %d, MODE %d, HOLD %d, START %d)   RESET %s\n", io.panel & 0xf,
        io.panel & 1, (io.panel >> 1) & 1, (io.panel >> 2) & 1, (io.panel >> 3) & 1, io.reset_held ? "held" : "up"];
    if (io.has_2102)
        [s appendFormat:@"2102 RAM  ports $%02X/$%02X   address $%03X   bit %d\n", io.ram_port, io.ram_port + 1,
            io.ram_addr & 0x3ff, io.ram_bit];

    /* the memory map, once */
    [s appendString:@"\nMemory map\n"];
    int regionCount = chfdebug_memory_map(_dbg, regions, MAX_REGIONS);
    if (regionCount < 0) regionCount = 0;
    if (regionCount > MAX_REGIONS) regionCount = MAX_REGIONS;
    if (regionCount == 0) [s appendString:@"  (not available)\n"];
    for (int i = 0; i < regionCount; i++)
        [s appendFormat:@"  %04X-%04X  %c%c  %@\n", regions[i].low, regions[i].high, regions[i].readable ? 'r' : '-',
                        regions[i].writable ? 'w' : '-', cstr(regions[i].name)];
    setText(_cartText, s, NO);

    setText(_mailbox, c.present ? hexDump(c.mailbox_page, (int)sizeof c.mailbox_page, 0xF800) : @"", NO);

    int n = chfdebug_cart_events(_dbg, cartEvents, CARTEV_MAX);
    if (n < 0) n = 0;
    NSMutableString *log = [NSMutableString stringWithString:@"frame     PC  event\n"];
    static const char *const kinds[5] = { "?", "REG", "TX", "BANK", "SWAP" };
    for (int i = 0; i < n; i++)
        [log appendFormat:@"%5u  $%04X  %s reg $%02X = $%02X\n", cartEvents[i].frame, cartEvents[i].pc,
            kinds[cartEvents[i].kind < 5 ? cartEvents[i].kind : 0], cartEvents[i].reg, cartEvents[i].value];
    if (n == 0) [log appendString:@"(none yet)\n"];
    setText(_cartLog, log, atEnd(_cartLog));

    n = chfdebug_io_events(_dbg, ioEvents, IOEV_MAX);
    if (n < 0) n = 0;
    NSMutableString *ioLog = [NSMutableString stringWithString:@"frame       cycle  PC     access\n"];
    for (int i = 0; i < n; i++)
        [ioLog appendFormat:@"%5u %11llu  $%04X  %s port $%02X = $%02X\n", ioEvents[i].frame,
            (unsigned long long)ioEvents[i].cycle, ioEvents[i].pc, ioEvents[i].is_write ? "OUT" : "IN ",
            ioEvents[i].port, ioEvents[i].value];
    if (n == 0) [ioLog appendString:@"(none yet)\n"];
    setText(_ioLog, ioLog, atEnd(_ioLog));
}

/* Sound: the beeper's mode and level, the forced-on interval, and the
 * timeline of mode changes. */
- (void)refreshSound
{
    static chfdebug_sound_event events[SNDEV_MAX];
    chfdebug_io io;
    memset(&io, 0, sizeof io);
    chfdebug_io_get(_dbg, &io);
    _soundMode.stringValue = [NSString stringWithFormat:@"Mode  %@   (%d)", soundModeName(io.sound_mode), io.sound_mode];
    _soundLevel.doubleValue = io.sound_level;
    _soundInfo.stringValue = [NSString stringWithFormat:@"%d / %d   forced on (the ~2 ms after a change): %s   %u mode "
                                                        @"changes   %llu samples made",
                              io.sound_level, SOUND_LEVEL_MAX, io.sound_forced ? "yes" : "no", io.sound_changes,
                              (unsigned long long)io.audio_samples];
    int n = chfdebug_sound_events(_dbg, events, SNDEV_MAX);
    if (n < 0) n = 0;
    NSMutableString *log = [NSMutableString stringWithString:@"frame     PC  mode\n"];
    for (int i = 0; i < n; i++)
        [log appendFormat:@"%5u  $%04X  %@ -> %@\n", events[i].frame, events[i].pc, soundModeName(events[i].old_mode),
                          soundModeName(events[i].mode)];
    if (n == 0) [log appendString:@"(none yet)\n"];
    setText(_soundLog, log, atEnd(_soundLog));
}

/* The symbol table, filtered: "AAAA  name  [bank N]", ordered by address.
 * The table's own order is unstable, so it is asked again each time. */
- (void)refreshSymbols
{
    const int total = chfdebug_symbols_count(_dbg);
    NSString *filter = [_symFilter.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    int *indices = total > 0 ? malloc((size_t)total * sizeof *indices) : NULL;
    int shown = 0;
    if (indices) shown = chfdebug_symbols_filter(_dbg, filter.UTF8String, indices, total);
    if (shown < 0) shown = 0;
    NSMutableString *text = [NSMutableString string];
    [_symAddrs removeAllObjects];
    [_symBanks removeAllObjects];
    for (int i = 0; i < shown; i++) {
        uint16_t address = 0;
        char name[96];
        int bank = -1;
        name[0] = '\0';
        if (!chfdebug_symbols_get(_dbg, indices[i], &address, name, (int)sizeof name, &bank)) continue;
        if (bank == CHFDEBUG_SYM_PORT) [text appendFormat:@"%02X    %@  port", address, padTo(cstr(name), 32)];
        else [text appendFormat:@"%04X  %@", address, padTo(cstr(name), 32)];
        if (bank >= 0) [text appendFormat:@"  bank %d", bank];
        [text appendString:@"\n"];
        [_symAddrs addObject:@(address)];
        [_symBanks addObject:@(bank)];
    }
    free(indices);
    if (_symAddrs.count == 0) [text appendString:@"(no symbols)\n"];
    NSScrollView *scroll = _symList.enclosingScrollView;
    const NSPoint at = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
    if (![_symList.string isEqualToString:text]) {
        _symList.string = text;
        if (scroll) {
            [scroll.contentView scrollToPoint:at];
            [scroll reflectScrolledClipView:scroll.contentView];
        }
    }
    _symCount.stringValue = [NSString stringWithFormat:@"%lu of %d symbols%@", (unsigned long)_symAddrs.count, total,
                                                       filter.length ? @" (filtered)" : @""];
}

/* Trace (idx 0) and History (idx 1): the newest HIST_SHOWN lines of the
 * engine's ring that pass the filter, oldest first. */
- (void)refreshHist:(int)idx
{
    static chfdebug_trace ring[HIST_RING];
    static int hit[HIST_SHOWN];
    const BOOL on = chfdebug_trace_enabled(_dbg) != 0;
    [self syncRecord:on];

    uint16_t lo = 0, hi = 0xFFFF;
    uint32_t rangeLo = 0, rangeHi = 0;
    int filter = CHFDEBUG_TRACE_ALL;
    BOOL badRange = NO;
    switch (_hFilter[idx].indexOfSelectedItem) {
    case 1: filter = CHFDEBUG_TRACE_BRANCHES; break;
    case 2:
        if (chfsym_parse_range(_dbg, CHF_SPACE_PROG, _hRange[idx].stringValue.UTF8String, &rangeLo, &rangeHi)) {
            lo = (uint16_t)rangeLo;
            hi = (uint16_t)rangeHi;
            filter = CHFDEBUG_TRACE_RANGE;
        } else {
            badRange = YES;
        }
        break;
    default: break;
    }
    const int total = chfdebug_trace_count(_dbg);
    /* newest first: unfiltered, only the lines shown are needed */
    int n = chfdebug_trace_read(_dbg, ring, filter == CHFDEBUG_TRACE_ALL ? HIST_SHOWN : HIST_RING);
    if (n < 0) n = 0;
    int shown = 0;
    for (int i = 0; i < n && shown < HIST_SHOWN; i++)
        if (chfdebug_trace_match(&ring[i], filter, lo, hi)) hit[shown++] = i;

    NSMutableString *t = [NSMutableString string];
    for (int k = shown - 1; k >= 0; k--) {
        char line[256];
        line[0] = '\0';
        chfdebug_trace_format(&ring[hit[k]], line, (int)sizeof line);
        [t appendString:cstr(line)];
        [t appendString:@"\n"];
    }
    NSString *note;
    if (!on && total == 0)
        note = @"recording off";
    else
        note = [NSString stringWithFormat:@"%@, %d instructions%@", on ? @"recording" : @"recording off", total,
                shown < total ? [NSString stringWithFormat:@" (showing %d)", shown] : @""];
    if (badRange) note = [note stringByAppendingString:@" (range not understood: showing all)"];
    _hNote[idx].stringValue = note;
    setText(_hText[idx], t, YES);
}

/* The packet log is rebuilt only when it changed: a new packet, or a
 * repeat (which moves the newest serial on). */
- (void)refreshFujinet:(BOOL)force
{
    static chfdebug_fuji_packet p[FUJI_PACKETS_MAX];
    static char state[2048];
    char status[128];
    int n = chfdebug_fujinet_trace(_dbg, p, FUJI_PACKETS_MAX);
    if (n < 0) n = 0;

    status[0] = '\0';
    if (chfsession_fujinet_external(_session)) {
        chfsession_cart_status(_session, status, (int)sizeof status);
        _fujiNote.stringValue = [NSString stringWithFormat:@"External FujiNet at %s. Cartridge: %@.",
                                 chfsession_boip_endpoint(_session), cstr(status)];
    } else if (!chfsession_fujinet_running(_session)) {
        _fujiNote.stringValue = @"FujiNet is off (Settings ▸ FujiNet).";
    } else {
        chfsession_cart_status(_session, status, (int)sizeof status);
        _fujiNote.stringValue = [NSString stringWithFormat:@"FujiNet is on. Cartridge: %@.", cstr(status)];
    }
    state[0] = '\0';
    chfdebug_fujinet_state(_dbg, state, (int)sizeof state);
    setText(_fujiState, cstr(state), NO);

    if (!force && n == _fujiCount && (n == 0 || p[n - 1].serial == _fujiLast)) return;
    const BOOL follow = force || _fujiCount < 0 || atEnd(_fujiText);
    _fujiCount = n;
    _fujiLast = n > 0 ? p[n - 1].serial : 0;
    NSMutableString *s = [NSMutableString string];
    for (int i = 0; i < n; i++) {
        if (i) [s appendString:@"\n"];
        [s appendString:cstr(p[i].text)];
        if (p[i].repeat > 1) [s appendFormat:@" ×%u", p[i].repeat];
        [s appendString:@"\n     "];
        for (int k = 0; k < p[i].nbytes && k < (int)sizeof p[i].data; k++)
            [s appendFormat:@"%02X ", p[i].data[k]];
        if (p[i].nbytes > (int)sizeof p[i].data) [s appendString:@"…"];
    }
    if (n == 0) [s appendString:@"(no FujiBus packets since the trace was cleared)"];
    setText(_fujiText, s, follow);
}

@end
