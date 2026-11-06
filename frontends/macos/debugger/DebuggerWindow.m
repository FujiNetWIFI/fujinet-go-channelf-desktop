/*
 * Debugger window (AppKit) over this app's Fairchild F8 (F3850) / Channel F
 * debugger engine, via core/include/chfdebug.h. Mirrors the GTK, Qt and
 * Win32 debuggers tab for tab: Prompt, CPU & RAM, Disassembly, Video (the
 * Channel F's own: VideoTab.m), Sound & I/O, Breakpoints, Cart, FujiNet.
 *
 * Opening the window attaches the engine, which stops the machine; closing
 * it detaches, and the machine runs on. The window refreshes only while it
 * can be seen, and only the tab that shows: a stopped machine on a change, a
 * running one a couple of times a second (the Video tab ten). Every tab has
 * a Help button (the toolbar's, which follows the tab).
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DebuggerWindow.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import "DebugViews.h"
#import "VideoTab.h"
#import "../KeyForward.h"
#import "../help/HelpWindow.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "chfdebug.h"

#define DISASM_WINDOW 48
#define MAX_BPS 64
#define MEM_BYTES 256        /* the memory view */
#define TRACE_MAX 512        /* FujiNet packets shown */
#define IOEV_MAX 300         /* port accesses shown */
#define SNDEV_MAX 120        /* beeper mode changes shown */
#define CARTEV_MAX 200       /* cartridge events shown */
#define NUM_REGS 13          /* PC PC0 PC1 DC0 DC1 dbus A W ISAR J H K Q */

/* The tabs, in order. */
#define TAB_PROMPT      0
#define TAB_CPU         1
#define TAB_DISASM      2
#define TAB_VIDEO       3
#define TAB_IO          4
#define TAB_BREAKPOINTS 5
#define TAB_CART        6
#define TAB_FUJINET     7
#define NUM_TABS        8

static ChfDebuggerWindow *g_debugger;

/* The Help topic of each tab. */
static NSString *const kTabContexts[NUM_TABS] = {
    @"debugger/prompt", @"debugger/cpu", @"debugger/disassembly", @"debugger/video",
    @"debugger/io", @"debugger/breakpoints", @"debugger/cart", @"debugger/fujinet"
};

/* The open panels' filters: a type per extension (a dynamic one for an
 * extension nothing has registered, which still matches by extension). */
static NSArray<UTType *> *typesForExtensions(NSArray<NSString *> *exts)
{
    NSMutableArray<UTType *> *types = [NSMutableArray array];
    for (NSString *ext in exts) {
        UTType *t = [UTType typeWithFilenameExtension:ext];
        if (t) [types addObject:t];
    }
    return types;
}

static NSString *nsstr(const char *s)
{
    NSString *r = s ? [NSString stringWithUTF8String:s] : nil;
    return r ? r : @"";
}

/* F12 closes the debugger from inside it, as it opens it from the
 * machine's window. Taken as a key equivalent, so it works wherever the
 * focus is in the window. */
@interface DebuggerPanelWindow : NSWindow
@end

@implementation DebuggerPanelWindow
static BOOL isF12(NSEvent *event)
{
    NSString *chars = [event charactersIgnoringModifiers];
    return [event type] == NSEventTypeKeyDown && [chars length] == 1 &&
           [chars characterAtIndex:0] == NSF12FunctionKey;
}

- (BOOL)performKeyEquivalent:(NSEvent *)event
{
    if (isF12(event)) {
        [self performClose:nil];
        return YES;
    }
    return [super performKeyEquivalent:event];
}

/* and when nothing in the window took it as a key equivalent */
- (void)keyDown:(NSEvent *)event
{
    if (isF12(event)) {
        [self performClose:nil];
        return;
    }
    [super keyDown:event];
}
@end

/* ---- the hand controllers and the panel, as the console sees them ------------------- */

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
    NSDictionary *a = @{ NSFontAttributeName: [NSFont systemFontOfSize:9],
                         NSParagraphStyleAttributeName: para,
                         NSForegroundColorAttributeName: lit ? NSColor.blackColor : NSColor.labelColor };
    const NSSize sz = [text sizeWithAttributes:a];
    [text drawInRect:NSMakeRect(r.origin.x + 1, NSMidY(r) - sz.height / 2, r.size.width - 2, sz.height)
      withAttributes:a];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    static const char *const names[8] = { "Right", "Left", "Back", "Fwd", "CCW", "CW", "Pull", "Push" };
    static const char *const buttons[4] = { "1 TIME", "2 MODE", "3 HOLD", "4 START" };
    NSDictionary *head = @{ NSFontAttributeName: [NSFont systemFontOfSize:9],
                            NSForegroundColorAttributeName: NSColor.secondaryLabelColor };
    const CGFloat top = 16;
    CGFloat x = 4;
    /* Player 2 (the left controller, CPU port 4) at the left, Player 1 (the
     * right one, port 1) at the right, as they sit in front of the player */
    for (int k = 0; k < 2; k++) {
        const int p = k == 0 ? 1 : 0;
        [(p == 0 ? @"Player 1: right controller (port 1)" : @"Player 2: left controller (port 4)")
            drawAtPoint:NSMakePoint(x, 2) withAttributes:head];
        for (int a = 0; a < 8; a++)
            [self cell:NSMakeRect(x + (a % 4) * IO_CELL_W, top + (a / 4) * IO_CELL_H, IO_CELL_W, IO_CELL_H)
                  text:nsstr(names[a]) lit:((_pads[p] >> a) & 1u) != 0];
        x += 4 * IO_CELL_W + IO_GAP;
        if (k == 0) {
            /* the panel between the two controllers */
            [@"Console panel" drawAtPoint:NSMakePoint(x, 2) withAttributes:head];
            for (int b = 0; b < 4; b++)
                [self cell:NSMakeRect(x + (b % 2) * 52, top + (b / 2) * IO_CELL_H, 52, IO_CELL_H)
                      text:nsstr(buttons[b]) lit:((_panel >> b) & 1u) != 0];
            [self cell:NSMakeRect(x + 104, top, 52, IO_CELL_H) text:@"RESET" lit:_reset != 0];
            x += 3 * 52 + IO_GAP;
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
 * band (r40-r59) is shaded. A click picks a cell. */
@interface ChfScratchView : NSView
@property (nonatomic, copy) void (^onSelect)(int reg);
@property (nonatomic) int selected;
- (void)setRegs:(const uint8_t *)r isar:(int)isar;
@end

@implementation ChfScratchView {
    uint8_t _cur[64], _prev[64];
    BOOL _have;
    int _isar;
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
    return self;
}

- (void)setRegs:(const uint8_t *)r isar:(int)isar
{
    BOOL dirty = NO;
    if (!_have) {
        memcpy(_cur, r, 64);
        memcpy(_prev, r, 64);
        _have = YES;
        dirty = YES;
    } else if (memcmp(_cur, r, 64) != 0) {
        /* what changed is shown until the next change */
        memcpy(_prev, _cur, 64);
        memcpy(_cur, r, 64);
        dirty = YES;
    }
    if (isar != _isar) {
        _isar = isar;
        dirty = YES;
    }
    if (dirty) [self setNeedsDisplay:YES];
}

static NSString *scratchName(int i)
{
    switch (i) {
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

- (void)mouseDown:(NSEvent *)e
{
    const NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    const int c = (int)floor((p.x - SC_HDR_W) / SC_CELL_W), r = (int)floor((p.y - SC_HDR_H) / SC_CELL_H);
    if (c < 0 || c > 7 || r < 0 || r > 7) return;
    self.selected = r * 8 + c;
    if (self.onSelect) self.onSelect(r * 8 + c);
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    NSDictionary *head = @{ NSFontAttributeName: [NSFont monospacedSystemFontOfSize:9 weight:NSFontWeightRegular],
                            NSForegroundColorAttributeName: NSColor.secondaryLabelColor };
    NSDictionary *nm = @{ NSFontAttributeName: [NSFont systemFontOfSize:8],
                          NSForegroundColorAttributeName: NSColor.secondaryLabelColor };
    NSDictionary *val = @{ NSFontAttributeName: [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightMedium],
                           NSForegroundColorAttributeName: NSColor.labelColor };
    NSDictionary *nmOn = @{ NSFontAttributeName: [NSFont systemFontOfSize:8],
                            NSForegroundColorAttributeName: NSColor.blackColor };
    NSDictionary *valOn = @{ NSFontAttributeName: [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightMedium],
                             NSForegroundColorAttributeName: NSColor.blackColor };
    for (int i = 0; i < 8; i++) {
        [[NSString stringWithFormat:@"%d", i] drawAtPoint:NSMakePoint(SC_HDR_W + i * SC_CELL_W + SC_CELL_W / 2 - 3, 0)
                                           withAttributes:head];
        [[NSString stringWithFormat:@"%d", i] drawAtPoint:NSMakePoint(4, SC_HDR_H + i * SC_CELL_H + SC_CELL_H / 2 - 6)
                                           withAttributes:head];
    }
    const int isarCell = _isar & 63;
    for (int idx = 0; idx < 64; idx++) {
        const NSRect r = NSMakeRect(SC_HDR_W + (idx & 7) * SC_CELL_W, SC_HDR_H + (idx >> 3) * SC_CELL_H,
                                    SC_CELL_W, SC_CELL_H);
        const BOOL changed = _have && _cur[idx] != _prev[idx];
        const BOOL kband = idx >= 40 && idx <= 59;
        const NSRect in = NSInsetRect(r, 1, 1);
        if (changed) [ChfAccentColor() setFill];
        else if (kband) [[NSColor colorWithWhite:0.5 alpha:0.22] setFill];
        else [NSColor.controlBackgroundColor setFill];
        NSRectFill(in);
        [NSColor.separatorColor setStroke];
        NSFrameRect(in);
        [[NSString stringWithFormat:@"r%d%@", idx, scratchName(idx)]
            drawAtPoint:NSMakePoint(r.origin.x + 4, r.origin.y + 3) withAttributes:changed ? nmOn : nm];
        [[NSString stringWithFormat:@"%02X", _cur[idx]]
            drawAtPoint:NSMakePoint(r.origin.x + 4, r.origin.y + 14) withAttributes:changed ? valOn : val];
    }
    /* ISAR's cell, and the one picked for editing */
    const NSRect ir = NSMakeRect(SC_HDR_W + (isarCell & 7) * SC_CELL_W, SC_HDR_H + (isarCell >> 3) * SC_CELL_H,
                                 SC_CELL_W, SC_CELL_H);
    NSBezierPath *ring = [NSBezierPath bezierPathWithRect:NSInsetRect(ir, 1.5, 1.5)];
    ring.lineWidth = 2;
    [NSColor.labelColor setStroke];
    [ring stroke];
    if (_selected >= 0 && _selected < 64) {
        const NSRect sr = NSMakeRect(SC_HDR_W + (_selected & 7) * SC_CELL_W,
                                     SC_HDR_H + (_selected >> 3) * SC_CELL_H, SC_CELL_W, SC_CELL_H);
        NSBezierPath *sel = [NSBezierPath bezierPathWithRect:NSInsetRect(sr, 3.5, 3.5)];
        sel.lineWidth = 1;
        const CGFloat dash[2] = { 3, 2 };
        [sel setLineDash:dash count:2 phase:0];
        [NSColor.controlAccentColor setStroke];
        [sel stroke];
    }
}
@end

/* ---- the window --------------------------------------------------------------------- */

@interface ChfDebuggerWindow () <NSTabViewDelegate>
- (instancetype)initWithSession:(chfsession *)session;
- (void)refreshAll;
@end

@implementation ChfDebuggerWindow {
    chfsession *_session;
    chfdebug *_dbg;
    NSWindow *_window;
    NSTimer *_tick;
    unsigned _seenGen;
    BOOL _wasStopped;
    int _slowTicks;
    NSTabView *_tabs;

    NSButton *_runBtn;
    NSTextField *_status;

    NSTextView *_promptOut;
    NSTextField *_promptIn;
    NSMutableArray<NSString *> *_history;
    NSInteger _historyPos;

    NSTextField *_reg[NUM_REGS];
    NSButton *_flag[5];
    NSTextField *_cpuState;
    ChfScratchView *_scratch;
    NSTextField *_cellLabel, *_cellVal;
    int _cellIdx;
    uint8_t _lastRegs[64];
    NSTextView *_kstack, *_mem;
    NSPopUpButton *_memSpace, *_ramSpace;
    NSTextField *_memAddr, *_ramAddr, *_ramVal;

    NSButton *_followPc;
    NSTextField *_jump;
    ChfLineTextView *_disasm;
    uint16_t _disasmAddr;
    uint16_t _lineAddr[DISASM_WINDOW];
    int _lineCount;
    int _selAddr;                 /* the Disassembly line picked for Run to Cursor, or -1 */

    ChfVideoTab *_video;

    NSTextView *_sound, *_soundLog, *_ioLog;
    ChfIoDiagramView *_ioDiag;

    NSPopUpButton *_bpType;
    NSTextField *_bpRange, *_bpCond;
    NSStackView *_bpList;

    NSTextView *_cart, *_mailbox, *_cartLog;

    NSTextField *_fujiNote;
    NSTextView *_fujiText, *_fujiState;
    uint64_t _fujiLast;
    int _fujiCount;
}

/* The registers, in the order the CPU tab indexes them: each with its
 * chfdebug_reg (-1: shown only) and the hex digits it shows (0: ISAR, octal). */
static const struct { NSString *__unsafe_unretained name; int reg; int digits; } kRegs[NUM_REGS] = {
    { @"PC", CHF_REG_PC, 4 },   { @"PC0", CHF_REG_PC0, 4 }, { @"PC1", CHF_REG_PC1, 4 },
    { @"DC0", CHF_REG_DC0, 4 }, { @"DC1", CHF_REG_DC1, 4 }, { @"dbus", -1, 2 },
    { @"A", CHF_REG_A, 2 },     { @"W", CHF_REG_W, 2 },     { @"ISAR", CHF_REG_ISAR, 0 },
    { @"J", CHF_REG_J, 2 },     { @"H", CHF_REG_H, 4 },     { @"K", CHF_REG_K, 4 },
    { @"Q", CHF_REG_Q, 4 },
};
/* Where each row of the register grid puts them (-1: blank). */
static const int kRegGrid[5][3] = {
    { 0, 1, 2 }, { 3, 4, 5 }, { 6, 7, 8 }, { 9, 10, 11 }, { 12, -1, -1 },
};

/* W's bits as checkboxes (S is set when the result was positive), and the
 * register each sets. */
static const struct { NSString *__unsafe_unretained name; int reg; } kFlags[5] = {
    { @"S (positive)", CHF_REG_SIGN }, { @"C", CHF_REG_CARRY }, { @"Z", CHF_REG_ZERO },
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

/* CHF_DEBUGGER_TAB takes an index or one of these. */
static const char *const kTabNames[NUM_TABS] = {
    "prompt", "cpu", "disassembly", "video", "io", "breakpoints", "cart", "fujinet"
};

/* ---- helpers ------------------------------------------------------------------------- */

static NSString *trimmed(NSString *text)
{
    return [text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
}

/* $hex, 0xhex, hexh, #dec, or bare hex: the debugger's address, port and
 * value fields. */
static BOOL parseHex(NSString *text, long *out)
{
    NSString *t = trimmed(text);
    int base = 16;
    if ([t hasPrefix:@"$"]) t = [t substringFromIndex:1];
    else if ([t.lowercaseString hasPrefix:@"0x"]) t = [t substringFromIndex:2];
    else if ([t hasPrefix:@"#"]) { t = [t substringFromIndex:1]; base = 10; }
    else if (t.length > 1 && [t.lowercaseString hasSuffix:@"h"]) t = [t substringToIndex:t.length - 1];
    if (t.length == 0) return NO;
    char *end;
    const long v = strtol(t.UTF8String, &end, base);
    if (*end) return NO;
    *out = v;
    return YES;
}

/* A whole decimal number (or $hex / 0xhex): scratchpad registers, VRAM rows
 * and columns. */
static BOOL parseDec(NSString *text, long *out)
{
    NSString *t = trimmed(text);
    int base = 10;
    if ([t hasPrefix:@"$"]) { t = [t substringFromIndex:1]; base = 16; }
    else if ([t.lowercaseString hasPrefix:@"0x"]) { t = [t substringFromIndex:2]; base = 16; }
    if (t.length == 0) return NO;
    char *end;
    const long v = strtol(t.UTF8String, &end, base);
    if (*end) return NO;
    *out = v;
    return YES;
}

/* ISAR as the CPU writes it in octal: "5.3" (upper.lower) or "53". */
static BOOL parseIsar(NSString *text, long *out)
{
    NSString *t = trimmed(text);
    const NSRange dot = [t rangeOfString:@"."];
    if (dot.location != NSNotFound) {
        NSString *u = [t substringToIndex:dot.location], *l = [t substringFromIndex:dot.location + 1];
        char *e1, *e2;
        if (u.length == 0 || l.length == 0) return NO;
        const long hi = strtol(u.UTF8String, &e1, 8), lo = strtol(l.UTF8String, &e2, 8);
        if (*e1 || *e2) return NO;
        *out = ((hi & 7) << 3) | (lo & 7);
        return YES;
    }
    if (t.length == 0) return NO;
    char *end;
    const long v = strtol(t.UTF8String, &end, 8);
    if (*end) return NO;
    *out = v & 63;
    return YES;
}

/* A label, or a hex number. -1 when neither. */
- (int)resolveAddr:(NSString *)text
{
    long v;
    NSString *t = trimmed(text);
    if (t.length == 0) return -1;
    int addr = chfdebug_label_address(_dbg, t.UTF8String);
    if (addr < 0 && parseHex(t, &v) && v >= 0 && v <= 0xffff) addr = (int)v;
    return addr;
}

/* A hex dump: a header of column numbers, then 16 bytes a line from `base`. */
static NSString *hexDump(const uint8_t *data, int n, int base, NSString *lineFormat)
{
    NSMutableString *s = [NSMutableString string];
    const NSUInteger pad = [[NSString stringWithFormat:lineFormat, base] length];
    [s appendString:[@"" stringByPaddingToLength:pad withString:@" " startingAtIndex:0]];
    for (int c = 0; c < 16; c++) [s appendFormat:@" %X ", c];
    [s appendString:@"\n"];
    for (int row = 0; row < n; row += 16) {
        [s appendFormat:lineFormat, base + row];
        for (int c = 0; c < 16 && row + c < n; c++) [s appendFormat:@"%02X ", data[row + c]];
        [s appendString:@"\n"];
    }
    return s;
}

/* A button lit in the accent, its title dark on the yellow; or plain. */
static void setButtonLit(NSButton *b, NSString *title, BOOL lit)
{
    b.bezelColor = lit ? ChfAccentColor() : nil;
    b.attributedTitle = [[NSAttributedString alloc] initWithString:title attributes:@{
        NSFontAttributeName: [NSFont systemFontOfSize:NSFont.systemFontSize],
        NSForegroundColorAttributeName: lit ? NSColor.blackColor : NSColor.controlTextColor }];
}

/* The hex-dump line format of a space: the width of its addresses. */
static NSString *spaceFormat(int space)
{
    switch (space) {
    case CHF_SPACE_SCRATCH:
    case CHF_SPACE_IO:    return @"$%02X: ";
    case CHF_SPACE_CART:  return @"$%05X: ";
    default:              return @"$%04X: ";
    }
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

/* ---- lifetime ----------------------------------------------------------------------- */

+ (void)showForSession:(chfsession *)session
{
    if (!g_debugger) g_debugger = [[ChfDebuggerWindow alloc] initWithSession:session];
    [g_debugger->_window makeKeyAndOrderFront:nil];
    chfdebug_attach(g_debugger->_dbg);
    [g_debugger updateTick];
    [g_debugger refreshAll];
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

- (instancetype)initWithSession:(chfsession *)session
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _dbg = chfsession_debugger(session);
    _fujiCount = -1;
    _cellIdx = -1;
    _selAddr = -1;
    _history = [NSMutableArray array];
    [self buildWindow];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(occlusionChanged:)
                                                 name:NSWindowDidChangeOcclusionStateNotification
                                               object:_window];
    return self;
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [_tick invalidate];
}

- (void)windowWillClose:(NSNotification *)note
{
    if (note.object != _window) return;
    chfdebug_detach(_dbg);
    [_tick invalidate];
    _tick = nil;
}

/* The tick runs only while the window can be seen. */
- (void)updateTick
{
    const BOOL want = [_window isVisible] && ([_window occlusionState] & NSWindowOcclusionStateVisible);
    if (want && !_tick) {
        __weak ChfDebuggerWindow *weakSelf = self;
        _tick = [NSTimer scheduledTimerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *t) {
            (void)t;
            [weakSelf onTick];
        }];
        _tick.tolerance = 0.02;
    } else if (!want && _tick) {
        [_tick invalidate];
        _tick = nil;
    }
}

- (void)occlusionChanged:(NSNotification *)note
{
    (void)note;
    const BOOL had = _tick != nil;
    [self updateTick];
    if (!had && _tick) [self refreshAll];     /* it was hidden: catch up */
}

- (NSInteger)selectedTab
{
    return [_tabs indexOfTabViewItem:_tabs.selectedTabViewItem];
}

- (void)onTick
{
    if (![_window isVisible]) return;
    const NSInteger tab = [self selectedTab];
    const unsigned gen = chfdebug_generation(_dbg);
    const BOOL stopped = chfdebug_is_stopped(_dbg) != 0;
    const BOOL slow = ++_slowTicks >= 5;     /* twice a second */
    if (slow) _slowTicks = 0;

    if (gen != _seenGen || stopped != _wasStopped) {
        _seenGen = gen;
        _wasStopped = stopped;
        [self refreshAll];
    } else if (tab == TAB_VIDEO && !stopped) {
        /* running: the video live, ten times a second */
        [_video refresh];
    } else if (slow && (!stopped || tab == TAB_FUJINET)) {
        if (!stopped) [self refreshStatus];
        /* a running machine has no disassembly worth redrawing */
        if (!(tab == TAB_DISASM && !stopped)) [self refreshTab:tab];
    }
}

/* The Help button and Command+? open the tab being shown. */
- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    const NSInteger i = [tabView indexOfTabViewItem:item];
    if (i >= 0 && i < NUM_TABS) ChfSetHelpContext(_window, kTabContexts[i]);
    [self refreshTab:i];
}

/* ---- construction -------------------------------------------------------------------- */

- (NSView *)buildPrompt
{
    NSScrollView *outScroll;
    _promptOut = ChfMonoView(&outScroll, YES);
    _promptOut.string = @"Debugger prompt. Type 'help' for every command.\n";
    _promptIn = ChfField(@"command (help, step, break $0802, bpv 12,40, print a, mem vram 0 64, ports, kstack, ...) — Tab completes, ↑↓ history",
                        0, self, @selector(runPrompt:));
    _promptIn.delegate = self;

    NSStackView *v = ChfVStack(@[outScroll, _promptIn]);
    ChfFill(v, outScroll);
    ChfFill(v, _promptIn);
    return v;
}

/* PC, PC0, PC1, DC0, DC1, the data bus, A, W, ISAR (octal), J, H, K and Q as
 * fields, W's bits as boxes (editable while stopped), the 64-byte scratchpad
 * as an octal grid, the K stack, and a memory view over the five spaces with
 * a write box. */
- (NSView *)buildCpu
{
    for (int i = 0; i < NUM_REGS; i++) {
        const int digits = kRegs[i].digits;
        _reg[i] = ChfField(@"", digits == 4 ? 48 : (digits == 2 ? 36 : 44), self, @selector(applyRegister:));
        _reg[i].tag = i;
        if (kRegs[i].reg < 0) _reg[i].editable = NO;
    }
    NSMutableArray<NSArray<NSView *> *> *rows = [NSMutableArray array];
    for (int row = 0; row < 5; row++) {
        NSMutableArray<NSView *> *cells = [NSMutableArray array];
        for (int col = 0; col < 3; col++) {
            const int idx = kRegGrid[row][col];
            [cells addObject:idx < 0 ? ChfLabel(@"") : ChfLabel(kRegs[idx].name)];
            [cells addObject:idx < 0 ? ChfLabel(@"") : _reg[idx]];
        }
        [rows addObject:cells];
    }
    NSGridView *regs = [NSGridView gridViewWithViews:rows];
    regs.rowSpacing = 4;
    regs.columnSpacing = 6;
    for (int col = 0; col < 6; col += 2) [regs columnAtIndex:col].xPlacement = NSGridCellPlacementTrailing;
    [regs columnAtIndex:2].leadingPadding = 16;
    [regs columnAtIndex:4].leadingPadding = 16;

    NSMutableArray<NSView *> *flags = [NSMutableArray arrayWithObject:ChfLabel(@"W")];
    for (int i = 0; i < 5; i++) {
        _flag[i] = [NSButton checkboxWithTitle:kFlags[i].name target:self action:@selector(flagClicked:)];
        _flag[i].tag = i;
        [flags addObject:_flag[i]];
    }
    _cpuState = ChfNote(@"");
    _cpuState.maximumNumberOfLines = 3;
    NSTextField *hint = ChfNote(@"Return applies an edit while the machine is stopped. ISAR is octal (5.3 = upper.lower). H, K and Q are r10:r11, r12:r13, r14:r15");

    _scratch = [[ChfScratchView alloc] initWithFrame:NSZeroRect];
    ChfFixSize(_scratch);
    __weak ChfDebuggerWindow *weakSelf = self;
    _scratch.onSelect = ^(int reg) { [weakSelf pickCell:reg]; };
    _cellLabel = ChfLabel(@"Click a cell to edit a register");
    _cellVal = ChfField(@"$00", 44, self, @selector(setCell:));
    NSStackView *cellRow = ChfHStack(@[_cellLabel, _cellVal, ChfButton(@"Set", self, @selector(setCell:))]);

    NSScrollView *kScroll, *memScroll;
    _kstack = ChfMonoView(&kScroll, NO);
    [kScroll.heightAnchor constraintEqualToConstant:118].active = YES;

    _mem = ChfMonoView(&memScroll, NO);
    _memSpace = [[NSPopUpButton alloc] init];
    [_memSpace addItemsWithTitles:@[@"PROG (the CPU's view)", @"SCRATCH (r0-r63)", @"VRAM (row * 128 + col)",
                                    @"CART (the cartridge's memory)", @"IO (the ports)"]];
    _memSpace.target = self;
    _memSpace.action = @selector(memViewChanged:);
    _memAddr = ChfField(@"$0000", 70, self, @selector(memViewChanged:));
    _memAddr.stringValue = @"$0000";
    NSStackView *memRow = ChfHStack(@[ChfLabel(@"Show"), _memSpace, ChfLabel(@"from"), _memAddr,
                                      ChfButton(@"Go", self, @selector(memViewChanged:))]);

    _ramSpace = [[NSPopUpButton alloc] init];
    [_ramSpace addItemsWithTitles:@[@"PROG", @"SCRATCH", @"VRAM", @"CART", @"IO"]];
    _ramAddr = ChfField(@"$8000", 70, self, @selector(writeRam:));
    _ramVal = ChfField(@"$00", 60, self, @selector(writeRam:));
    NSStackView *edit = ChfHStack(@[ChfLabel(@"Write"), _ramSpace, _ramAddr, ChfLabel(@"="), _ramVal,
                                    ChfButton(@"Write", self, @selector(writeRam:))]);
    NSTextField *ramHint = ChfNote(@"Only the FujiNet cartridge's RAM and a Videocart's RAM are writable in PROG; "
                                   @"the BIOS, ROM and the mailbox's hotspots are not. A VRAM edit shows in the next frame. "
                                   @"Hex: $0802 or 0802; #12 is decimal");

    NSStackView *left = ChfVStack(@[regs, ChfHStack(flags), _cpuState, hint,
                                    ChfTitle(@"Scratchpad (ISAR's octal grid; its cell outlined, changes in yellow, r40-r59 the K stack)"),
                                    _scratch, cellRow]);
    NSStackView *right = ChfVStack(@[ChfTitle(@"K stack"), kScroll, ChfTitle(@"Memory"), memRow, memScroll, edit, ramHint]);
    right.distribution = NSStackViewDistributionFill;
    ChfFill(right, kScroll);
    ChfFill(right, memScroll);
    NSStackView *h = ChfHStack(@[left, right]);
    h.alignment = NSLayoutAttributeTop;
    h.spacing = 16;
    [right.widthAnchor constraintGreaterThanOrEqualToConstant:420].active = YES;
    [right.heightAnchor constraintEqualToAnchor:h.heightAnchor].active = YES;
    return h;
}

- (NSView *)buildDisasm
{
    _followPc = [NSButton checkboxWithTitle:@"Follow PC" target:self action:@selector(refreshDisasm)];
    _followPc.state = NSControlStateValueOn;
    _jump = ChfField(@"address or label", 150, self, @selector(jumpTo:));
    NSTextField *hint = ChfNote(@"Click a line to pick it (Run to Cursor); double-click it for an execute breakpoint (*); scroll to browse");
    NSStackView *row = ChfHStack(@[_followPc, ChfLabel(@"Jump to"), _jump, hint]);

    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = NO;
    scroll.hasHorizontalScroller = YES;
    _disasm = [[ChfLineTextView alloc] initWithFrame:NSMakeRect(0, 0, 600, 600)];
    _disasm.editable = NO;
    _disasm.richText = NO;
    _disasm.selectable = NO;
    _disasm.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    _disasm.horizontallyResizable = YES;
    _disasm.textContainer.widthTracksTextView = NO;
    _disasm.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
    _disasm.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    __weak ChfDebuggerWindow *weakSelf = self;
    _disasm.onClickLine = ^(int line) {
        ChfDebuggerWindow *s = weakSelf;
        if (!s || line < 0 || line >= s->_lineCount) return;
        s->_selAddr = s->_lineAddr[line];
        [s refreshDisasm];
    };
    _disasm.onDoubleClickLine = ^(int line) {
        ChfDebuggerWindow *s = weakSelf;
        if (!s || line < 0 || line >= s->_lineCount) return;
        s->_selAddr = s->_lineAddr[line];
        chfdebug_breakpoint_toggle(s->_dbg, s->_lineAddr[line]);
        [s refreshDisasm];
        [s refreshBps];
    };
    _disasm.onScroll = ^(int lines) {
        ChfDebuggerWindow *s = weakSelf;
        if (!s || lines == 0) return;
        s->_followPc.state = NSControlStateValueOff;
        s->_disasmAddr = (uint16_t)chfdebug_row_address(s->_dbg, s->_disasmAddr, lines);
        [s refreshDisasm];
    };
    scroll.documentView = _disasm;

    NSStackView *v = ChfVStack(@[row, scroll]);
    ChfFill(v, scroll);
    return v;
}

/* The controllers and the panel over the sound's and the ports' state, and
 * the two logs. */
- (NSView *)buildIo
{
    NSScrollView *scroll, *soundScroll, *ioScroll;
    _sound = ChfMonoView(&scroll, NO);
    _soundLog = ChfMonoView(&soundScroll, NO);
    _ioLog = ChfMonoView(&ioScroll, NO);
    _ioDiag = [[ChfIoDiagramView alloc] initWithFrame:NSZeroRect];
    ChfFixSize(_ioDiag);
    NSTextField *hint = ChfNote(@"Filled: held, as the console latched it at the frame (active low on the port; the pad is read while port 0 bit 6 is clear).");
    [scroll.heightAnchor constraintEqualToConstant:176].active = YES;      /* the logs below take the rest */

    NSStackView *soundCol = ChfVStack(@[ChfTitle(@"Beeper mode changes"), soundScroll]);
    soundCol.distribution = NSStackViewDistributionFill;
    ChfFill(soundCol, soundScroll);
    NSStackView *ioHead = ChfHStack(@[ChfTitle(@"Port accesses (newest last)"),
                                      ChfButton(@"Clear", self, @selector(ioClear:))]);
    NSStackView *ioCol = ChfVStack(@[ioHead, ioScroll]);
    ioCol.distribution = NSStackViewDistributionFill;
    ChfFill(ioCol, ioScroll);
    NSStackView *logs = ChfHStack(@[soundCol, ioCol]);
    logs.alignment = NSLayoutAttributeTop;
    logs.spacing = 12;
    [soundCol.widthAnchor constraintEqualToConstant:300].active = YES;
    [soundCol.heightAnchor constraintEqualToAnchor:logs.heightAnchor].active = YES;
    [ioCol.heightAnchor constraintEqualToAnchor:logs.heightAnchor].active = YES;

    NSStackView *v = ChfVStack(@[ChfTitle(@"Controllers, console panel and RESET"), _ioDiag, hint, scroll, logs]);
    v.distribution = NSStackViewDistributionFill;
    ChfFill(v, scroll);
    ChfFill(v, logs);
    return v;
}

- (NSView *)buildBreaks
{
    _bpType = [[NSPopUpButton alloc] init];
    [_bpType addItemsWithTitles:@[@"Execute", @"Data read", @"Data write", @"Port read", @"Port write",
                                  @"VRAM write", @"Scratchpad write"]];
    _bpRange = ChfField(@"$0802, $0800-$08FF or a label; a port $20; row,col or row,col-row,col; a register 9 or 40-59",
                        360, self, @selector(addBreakpoint:));
    _bpCond = ChfField(@"condition (optional): a == $10 && vpos > 100", 0, self, @selector(addBreakpoint:));
    NSStackView *row = ChfHStack(@[_bpType, _bpRange, _bpCond, ChfButton(@"Add", self, @selector(addBreakpoint:)),
                                  ChfButton(@"Clear all", self, @selector(clearBreaks:))]);

    /* The rows scroll: a session can collect more breakpoints than the tab
     * is tall. The stack is the scroll view's document, pinned to the top
     * and to the clip view's width so rows keep the tab's width. */
    _bpList = ChfVStack(@[]);
    _bpList.translatesAutoresizingMaskIntoConstraints = NO;
    NSScrollView *bpScroll = [[NSScrollView alloc] init];
    bpScroll.hasVerticalScroller = YES;
    bpScroll.drawsBackground = NO;
    bpScroll.documentView = _bpList;
    NSClipView *clip = bpScroll.contentView;
    [_bpList.topAnchor constraintEqualToAnchor:clip.topAnchor].active = YES;
    [_bpList.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor].active = YES;
    [_bpList.widthAnchor constraintEqualToAnchor:clip.widthAnchor].active = YES;
    NSTextField *hint = ChfNote(@"Addresses and ports are hex ($ optional; #12 is decimal); registers 0-63, rows and columns are decimal. "
                                @"Untick to disable. Conditions are C expressions over the registers, flags, [addr], value, addr, row, col, port, vpos, hpos, frame");
    NSStackView *v = ChfVStack(@[row, hint, bpScroll]);
    ChfFill(v, row);
    ChfFill(v, bpScroll);
    [bpScroll setContentHuggingPriority:NSLayoutPriorityDefaultLow
                         forOrientation:NSLayoutConstraintOrientationVertical];
    return v;
}

/* The cartridge's state as text left, its events under it; the mailbox
 * ($F800-$FFFF) as the console reads it, right. */
- (NSView *)buildCart
{
    NSScrollView *scroll, *mailScroll, *logScroll;
    _cart = ChfMonoView(&scroll, NO);
    _mailbox = ChfMonoView(&mailScroll, NO);
    _cartLog = ChfMonoView(&logScroll, NO);
    [logScroll.heightAnchor constraintEqualToConstant:150].active = YES;
    NSStackView *left = ChfVStack(@[scroll, ChfTitle(@"Cartridge events (register writes, bytes sent, page selects, swaps)"), logScroll]);
    left.distribution = NSStackViewDistributionFill;
    ChfFill(left, scroll);
    ChfFill(left, logScroll);
    NSStackView *right = ChfVStack(@[ChfTitle(@"The mailbox ($F800-$FFFF, as the console reads it)"), mailScroll]);
    right.distribution = NSStackViewDistributionFill;
    ChfFill(right, mailScroll);
    NSStackView *h = ChfHStack(@[left, right]);
    h.alignment = NSLayoutAttributeTop;
    h.spacing = 12;
    [left.widthAnchor constraintGreaterThanOrEqualToConstant:420].active = YES;
    [right.widthAnchor constraintEqualToConstant:480].active = YES;
    [left.heightAnchor constraintEqualToAnchor:h.heightAnchor].active = YES;
    [right.heightAnchor constraintEqualToAnchor:h.heightAnchor].active = YES;
    return h;
}

/* The packets left (a note and Clear over them), the cartridge's link and
 * mailbox right. */
- (NSView *)buildFujinet
{
    _fujiNote = ChfNote(@"");
    _fujiNote.lineBreakMode = NSLineBreakByTruncatingTail;
    [_fujiNote setContentHuggingPriority:NSLayoutPriorityDefaultLow
                          forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_fujiNote setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                        forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *row = ChfHStack(@[_fujiNote, ChfButton(@"Clear", self, @selector(fujiClear:))]);

    NSScrollView *fujiScroll, *stateScroll;
    _fujiText = ChfMonoView(&fujiScroll, NO);
    _fujiState = ChfMonoView(&stateScroll, NO);
    NSStackView *left = ChfVStack(@[ChfTitle(@"FujiBus packets"), row, fujiScroll]);
    left.distribution = NSStackViewDistributionFill;   /* the log takes the column's height */
    ChfFill(left, row);
    ChfFill(left, fujiScroll);
    NSStackView *right = ChfVStack(@[ChfTitle(@"The cartridge's link and mailbox"), stateScroll]);
    right.distribution = NSStackViewDistributionFill;
    ChfFill(right, stateScroll);

    NSStackView *h = ChfHStack(@[left, right]);
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
    _window = [[DebuggerPanelWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 1180, 800)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"Debugger";
    _window.releasedWhenClosed = NO;
    _window.delegate = self;

    /* Toolbar row. F5/F7/F8/Shift+F8 work as the buttons' key equivalents
     * wherever the focus is inside the window. */
    _runBtn = ChfButton(@"Stop (F5)", self, @selector(toggleRun:));
    NSButton *step = ChfButton(@"Step (F7)", self, @selector(step:));
    NSButton *over = ChfButton(@"Step Over (F8)", self, @selector(stepOver:));
    NSButton *outBtn = ChfButton(@"Step Out (⇧F8)", self, @selector(stepOut:));
    NSButton *frame = ChfButton(@"Frame", self, @selector(frame:));
    NSButton *scan = ChfButton(@"Scanline", self, @selector(scanline:));
    NSButton *toCursor = ChfButton(@"Run to Cursor", self, @selector(runToCursor:));
    _runBtn.keyEquivalent = [NSString stringWithFormat:@"%C", (unichar)NSF5FunctionKey];
    _runBtn.keyEquivalentModifierMask = 0;
    step.keyEquivalent = [NSString stringWithFormat:@"%C", (unichar)NSF7FunctionKey];
    step.keyEquivalentModifierMask = 0;
    over.keyEquivalent = [NSString stringWithFormat:@"%C", (unichar)NSF8FunctionKey];
    over.keyEquivalentModifierMask = 0;
    outBtn.keyEquivalent = [NSString stringWithFormat:@"%C", (unichar)NSF8FunctionKey];
    outBtn.keyEquivalentModifierMask = NSEventModifierFlagShift;
    NSPopUpButton *symbols = [self pullDown:@"Symbols…"
                                      items:@[@"From a File…", @"Beside the Cartridge"]
                                     action:@selector(loadSymbols:)];
    NSMutableArray *saveTitles = [NSMutableArray array];
    for (int i = 0; i < NUM_FILE_KINDS; i++) [saveTitles addObject:kFileTitles[i]];
    NSPopUpButton *save = [self pullDown:@"Save…" items:saveTitles action:@selector(saveFile:)];
    _status = ChfNote(@"Running");
    _status.alignment = NSTextAlignmentRight;
    _status.lineBreakMode = NSLineBreakByTruncatingTail;
    NSButton *help = ChfHelpButton(nil);
    [help setRefusesFirstResponder:YES];
    NSStackView *toolbar = ChfHStack(@[_runBtn, step, over, outBtn, frame, scan, toCursor, symbols, save, _status, help]);
    [_status setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_status setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                      forOrientation:NSLayoutConstraintOrientationHorizontal];

    _video = [[ChfVideoTab alloc] initWithDebugger:_dbg];
    _tabs = [[NSTabView alloc] init];
    NSArray *pages = @[ @[@"Prompt", [self buildPrompt]], @[@"CPU & RAM", [self buildCpu]],
                        @[@"Disassembly", [self buildDisasm]], @[@"Video", _video.view],
                        @[@"Sound & I/O", [self buildIo]],
                        @[@"Breakpoints", [self buildBreaks]], @[@"Cart", [self buildCart]],
                        @[@"FujiNet", [self buildFujinet]] ];
    for (NSArray *page in pages) {
        NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:page[0]];
        item.label = page[0];
        NSView *content = page[1];
        NSView *holder = [[NSView alloc] init];
        [holder addSubview:content];
        content.translatesAutoresizingMaskIntoConstraints = NO;
        [content.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:6].active = YES;
        [content.trailingAnchor constraintEqualToAnchor:holder.trailingAnchor constant:-6].active = YES;
        [content.topAnchor constraintEqualToAnchor:holder.topAnchor constant:6].active = YES;
        [content.bottomAnchor constraintEqualToAnchor:holder.bottomAnchor constant:-6].active = YES;
        item.view = holder;
        [_tabs addTabViewItem:item];
    }
    const char *tab = getenv("CHF_DEBUGGER_TAB");
    if (tab && *tab) {
        int t = -1;
        for (int i = 0; i < NUM_TABS; i++)
            if (!strcasecmp(tab, kTabNames[i])) t = i;
        if (t < 0 && !strcasecmp(tab, "sound")) t = TAB_IO;
        if (t < 0 && tab[0] >= '0' && tab[0] <= '9') t = atoi(tab);
        if (t >= 0 && t < (int)_tabs.numberOfTabViewItems) [_tabs selectTabViewItemAtIndex:t];
    }
    _tabs.delegate = self;
    {
        const NSInteger i = [self selectedTab];
        ChfSetHelpContext(_window, kTabContexts[i >= 0 && i < NUM_TABS ? i : 0]);
    }

    NSStackView *root = ChfVStack(@[toolbar, _tabs]);
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [toolbar.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [_tabs.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    _window.contentView = root;
    [_window center];
}

/* ---- actions --------------------------------------------------------------------------- */

- (void)fujiClear:(id)sender
{
    (void)sender;
    chfdebug_fujinet_trace_clear(_dbg);
    [self refreshFujinet:YES];
}

- (void)ioClear:(id)sender
{
    (void)sender;
    chfdebug_io_events_clear(_dbg);
    [self refreshIo];
}

- (void)toggleRun:(id)sender
{
    (void)sender;
    if (chfdebug_is_stopped(_dbg)) chfdebug_resume(_dbg); else chfdebug_stop(_dbg);
    [self refreshAll];
}
- (void)step:(id)sender { (void)sender; chfdebug_step(_dbg); [self refreshAll]; }
- (void)stepOver:(id)sender { (void)sender; chfdebug_step_over(_dbg); [self refreshAll]; }
- (void)stepOut:(id)sender { (void)sender; chfdebug_step_out(_dbg); [self refreshAll]; }
- (void)scanline:(id)sender { (void)sender; chfdebug_scanline(_dbg, 1); [self refreshAll]; }
- (void)frame:(id)sender { (void)sender; chfdebug_frame(_dbg, 1); [self refreshAll]; }

/* On to the Disassembly line that was picked. */
- (void)runToCursor:(id)sender
{
    (void)sender;
    if (_selAddr < 0) {
        _status.stringValue = @"Pick a line in the Disassembly first (click it)";
        return;
    }
    chfdebug_run_to(_dbg, (uint16_t)_selAddr);
    [self refreshAll];
}

- (void)appendPrompt:(NSString *)text
{
    [_promptOut.textStorage appendAttributedString:
        [[NSAttributedString alloc] initWithString:text attributes:@{ NSFontAttributeName: _promptOut.font ?: [NSFont userFixedPitchFontOfSize:11] }]];
    [_promptOut scrollRangeToVisible:NSMakeRange(_promptOut.string.length, 0)];
}

- (void)runPrompt:(id)sender
{
    (void)sender;
    static char out[65536];
    NSString *cmd = trimmed(_promptIn.stringValue);
    if (cmd.length == 0) return;
    if (![_history.lastObject isEqualToString:cmd]) [_history addObject:cmd];
    _historyPos = (NSInteger)_history.count;
    [self appendPrompt:[NSString stringWithFormat:@"> %@\n", cmd]];
    chfdebug_command(_dbg, cmd.UTF8String, out, sizeof out);
    [self appendPrompt:[ChfStripControl(out) stringByAppendingString:@"\n"]];
    _promptIn.stringValue = @"";
    [self refreshAll];
}

/* In the prompt, Tab completes instead of moving the focus, and Up/Down
 * walk the history. */
- (BOOL)control:(NSControl *)control textView:(NSTextView *)textView doCommandBySelector:(SEL)sel
{
    (void)textView;
    if (control != _promptIn) return NO;
    if (sel == @selector(moveUp:) || sel == @selector(moveDown:)) {
        const NSInteger n = (NSInteger)_history.count;
        if (n == 0) return YES;
        _historyPos += sel == @selector(moveUp:) ? -1 : 1;
        if (_historyPos < 0) _historyPos = 0;
        if (_historyPos > n) _historyPos = n;
        _promptIn.stringValue = _historyPos < n ? _history[(NSUInteger)_historyPos] : @"";
        [_promptIn.currentEditor setSelectedRange:NSMakeRange(_promptIn.stringValue.length, 0)];
        return YES;
    }
    if (sel != @selector(insertTab:)) return NO;
    NSString *text = _promptIn.stringValue;
    const NSRange sp = [text rangeOfString:@" " options:NSBackwardsSearch];
    NSString *word = sp.location == NSNotFound ? text : [text substringFromIndex:sp.location + 1];
    char comps[4096];
    const int n = chfdebug_completions(_dbg, word.UTF8String, comps, sizeof comps);
    if (n == 1) {
        NSString *c = [[NSString stringWithUTF8String:comps] componentsSeparatedByString:@"\n"][0];
        NSString *head = sp.location == NSNotFound ? @"" : [text substringToIndex:sp.location + 1];
        _promptIn.stringValue = [NSString stringWithFormat:@"%@%@ ", head, c];
        [_promptIn.currentEditor setSelectedRange:NSMakeRange(_promptIn.stringValue.length, 0)];
    } else if (n > 1) {
        NSString *list = [NSString stringWithUTF8String:comps] ?: @"";
        if (![list hasSuffix:@"\n"]) list = [list stringByAppendingString:@"\n"];
        [self appendPrompt:list];
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
        chfdebug_load_symbols(_dbg, NULL, msg, sizeof msg);
    } else {
        NSOpenPanel *p = [NSOpenPanel openPanel];
        p.title = @"Load Symbols (an AS .map or .lst, or an address list)";
        p.message = p.title;
        p.allowedContentTypes = typesForExtensions(@[@"map", @"lst", @"sym", @"txt"]);
        const char *cart = chfsession_cart_path(_session);
        if (cart && cart[0])
            [p setDirectoryURL:[NSURL fileURLWithPath:[nsstr(cart) stringByDeletingLastPathComponent] isDirectory:YES]];
        if ([p runModal] != NSModalResponseOK) return;
        chfdebug_load_symbols(_dbg, [[p URL] fileSystemRepresentation], msg, sizeof msg);
    }
    [self appendPrompt:[ChfStripControl(msg) stringByAppendingString:@"\n"]];
    _status.stringValue = ChfStripControl(msg);
    [self refreshAll];
}

- (void)saveFile:(NSPopUpButton *)sender
{
    const NSInteger kind = sender.selectedItem.tag;
    if (kind < 0 || kind >= NUM_FILE_KINDS) return;
    NSSavePanel *p = [NSSavePanel savePanel];
    p.title = [NSString stringWithFormat:@"Save %@", kFileTitles[kind]];
    [p setNameFieldStringValue:kFileNames[kind]];
    if ([p runModal] != NSModalResponseOK) return;
    char msg[512];
    msg[0] = '\0';
    chfdebug_save(_dbg, kFileKinds[kind], [[p URL] fileSystemRepresentation], msg, sizeof msg);
    [self appendPrompt:[ChfStripControl(msg) stringByAppendingString:@"\n"]];
    _status.stringValue = ChfStripControl(msg);
}

/* A register changes only while the machine is stopped: running, the next
 * instruction would overwrite it before anyone saw the edit land. */
- (void)applyRegister:(NSTextField *)sender
{
    long v;
    if (!chfdebug_is_stopped(_dbg)) {
        [self refreshCpu];
        _status.stringValue = @"Stop the machine (F5) to change a register";
        return;
    }
    const NSInteger tag = sender.tag;
    if (tag >= 0 && tag < NUM_REGS && kRegs[tag].reg >= 0) {
        const BOOL ok = kRegs[tag].reg == CHF_REG_ISAR ? parseIsar(sender.stringValue, &v)
                                                       : parseHex(sender.stringValue, &v);
        if (ok) chfdebug_cpu_set(_dbg, kRegs[tag].reg, (int)v);
    }
    [self refreshAll];
}

- (void)flagClicked:(NSButton *)sender
{
    if (!chfdebug_is_stopped(_dbg)) {
        [self refreshCpu];
        _status.stringValue = @"Stop the machine (F5) to change a flag";
        return;
    }
    if (sender.tag >= 0 && sender.tag < 5)
        chfdebug_cpu_set(_dbg, kFlags[sender.tag].reg, sender.state == NSControlStateValueOn ? 1 : 0);
    [self refreshAll];
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
    long v;
    if (_cellIdx < 0) {
        _status.stringValue = @"Click a scratchpad cell first";
        return;
    }
    if (!chfdebug_is_stopped(_dbg)) {
        _status.stringValue = @"Stop the machine (F5) to change a register";
        return;
    }
    if (!parseHex(_cellVal.stringValue, &v)) {
        _status.stringValue = @"Needs a value, such as $2A";
        return;
    }
    chfdebug_write(_dbg, CHF_SPACE_SCRATCH, (uint32_t)_cellIdx, (uint8_t)v);
    [self refreshAll];
}

- (void)memViewChanged:(id)sender
{
    (void)sender;
    [self refreshMem];
}

- (void)writeRam:(id)sender
{
    (void)sender;
    long a = 0, v = 0;
    const int space = (int)_ramSpace.indexOfSelectedItem;     /* CHF_SPACE_PROG ... _IO in order */
    const int size = chfdebug_space_size(_dbg, space);
    if (!parseHex(_ramAddr.stringValue, &a) || !parseHex(_ramVal.stringValue, &v) || a < 0 || a >= size) {
        _status.stringValue = [NSString stringWithFormat:@"Needs an address below $%X and a value", size];
        return;
    }
    chfdebug_write(_dbg, space, (uint32_t)a, (uint8_t)v);
    [self refreshAll];
}

- (void)jumpTo:(id)sender
{
    (void)sender;
    const int addr = [self resolveAddr:_jump.stringValue];
    if (addr < 0) return;
    _followPc.state = NSControlStateValueOff;
    _disasmAddr = (uint16_t)(addr & 0xffff);
    [self refreshDisasm];
}

/* "9", "9-12" or "r9": registers by decimal number. */
static BOOL parseRange(NSString *text, BOOL (*parse)(NSString *, long *), long *lo, long *hi)
{
    NSArray<NSString *> *ends = [text componentsSeparatedByString:@"-"];
    if (ends.count < 1 || ends.count > 2) return NO;
    if (!parse(ends[0], lo)) return NO;
    *hi = *lo;
    if (ends.count == 2 && !parse(ends[1], hi)) return NO;
    return *hi >= *lo;
}

- (void)addBreakpoint:(id)sender
{
    (void)sender;
    static const int types[7] = {
        CHFDEBUG_BP_EXEC, CHFDEBUG_BP_READ, CHFDEBUG_BP_WRITE, CHFDEBUG_BP_IN, CHFDEBUG_BP_OUT,
        CHFDEBUG_BP_VRAM, CHFDEBUG_BP_SCRATCH
    };
    static const int spaces[7] = {
        CHF_SPACE_PROG, CHF_SPACE_PROG, CHF_SPACE_PROG, CHF_SPACE_IO, CHF_SPACE_IO, CHF_SPACE_VRAM, CHF_SPACE_SCRATCH
    };
    const NSInteger sel = _bpType.indexOfSelectedItem;
    const int k = sel >= 0 && sel < 7 ? (int)sel : 0;
    NSString *range = trimmed(_bpRange.stringValue);
    if (range.length == 0) return;
    long start = -1, end = -1;
    BOOL ok = NO;
    if (k <= 2) {
        /* addresses, or labels */
        NSArray<NSString *> *ends = [range componentsSeparatedByString:@"-"];
        const int s = [self resolveAddr:ends[0]];
        const int e = ends.count > 1 ? [self resolveAddr:ends[1]] : s;
        ok = ends.count <= 2 && s >= 0 && e >= s;
        start = s;
        end = e;
    } else if (k <= 4) {
        ok = parseRange(range, parseHex, &start, &end) && end <= 0xff;
    } else if (k == 5) {
        /* row,col or row,col-row,col */
        NSArray<NSString *> *ends = [range componentsSeparatedByString:@"-"];
        long cell[2] = { -1, -1 };
        ok = ends.count >= 1 && ends.count <= 2;
        for (NSUInteger i = 0; ok && i < ends.count; i++) {
            NSArray<NSString *> *rc = [ends[i] componentsSeparatedByString:@","];
            long r, c;
            if (rc.count != 2 || !parseDec(rc[0], &r) || !parseDec(rc[1], &c) || r < 0 ||
                r >= CHFDEBUG_VRAM_ROWS || c < 0 || c >= CHFDEBUG_VRAM_COLS) {
                ok = NO;
                break;
            }
            cell[i] = r * CHFDEBUG_VRAM_COLS + c;
        }
        start = cell[0];
        end = cell[1] >= 0 ? cell[1] : cell[0];
        ok = ok && end >= start;
    } else {
        NSString *r = [range hasPrefix:@"r"] ? [range substringFromIndex:1] : range;
        ok = parseRange(r, parseDec, &start, &end) && end <= 63;
    }
    if (!ok || start < 0) {
        [self appendPrompt:[NSString stringWithFormat:@"breakpoint: bad range %@\n", range]];
        _status.stringValue = [NSString stringWithFormat:@"Bad range: %@", range];
        return;
    }
    const int bpId = chfdebug_breakpoint_add(_dbg, types[k], spaces[k], (uint16_t)start, (uint16_t)end,
                                            _bpCond.stringValue.UTF8String);
    if (bpId < 0) {
        _status.stringValue = @"The condition does not parse, or the range is not breakable";
        [self appendPrompt:[NSString stringWithFormat:@"breakpoint: the condition does not parse: %@\n",
                            _bpCond.stringValue]];
        return;
    }
    _bpRange.stringValue = @"";
    _bpCond.stringValue = @"";
    [self refreshAll];
}

- (void)bpEnabled:(NSButton *)sender
{
    chfdebug_breakpoint_enable(_dbg, (int)sender.tag, sender.state == NSControlStateValueOn);
    [self refreshAll];
}

- (void)bpRemove:(NSButton *)sender
{
    chfdebug_breakpoint_remove(_dbg, (int)sender.tag);
    [self refreshAll];
}

- (void)clearBreaks:(id)sender
{
    (void)sender;
    chfdebug_breakpoint_clear(_dbg);
    [self refreshAll];
}

/* ---- refreshers ------------------------------------------------------------------------ */

- (void)refreshStatus
{
    char reason[160];
    int addr;
    const BOOL stopped = chfdebug_is_stopped(_dbg) != 0;
    reason[0] = '\0';
    chfdebug_stop_reason(_dbg, reason, sizeof reason, &addr);
    _status.stringValue = stopped ? [NSString stringWithFormat:@"Stopped%s%s", reason[0] ? ": " : "", reason] : @"Running";
    setButtonLit(_runBtn, stopped ? @"Run (F5)" : @"Stop (F5)", stopped);
    /* stopped is called out in the accent (its darker shade on a light window) */
    _status.textColor = stopped ? ChfAccentTextColor() : NSColor.secondaryLabelColor;
}

static void setField(NSTextField *f, NSString *text)
{
    if (f.currentEditor) return;       /* not while it is being typed into */
    if (![f.stringValue isEqualToString:text]) f.stringValue = text;
}

- (void)refreshCpu
{
    chfdebug_cpu c;
    chfdebug_cpu_get(_dbg, &c);
    const unsigned vals[NUM_REGS] = {
        (unsigned)c.pc & 0xffff, c.pc0, c.pc1, c.dc0, c.dc1, c.dbus, c.a, c.w, c.isar,
        c.j_h_k_q[0], c.j_h_k_q[1], c.j_h_k_q[2], c.j_h_k_q[3]
    };
    for (int i = 0; i < NUM_REGS; i++) {
        switch (kRegs[i].digits) {
        case 4:  setField(_reg[i], [NSString stringWithFormat:@"%04X", vals[i] & 0xffff]); break;
        case 2:  setField(_reg[i], [NSString stringWithFormat:@"%02X", vals[i] & 0xff]); break;
        default: setField(_reg[i], [NSString stringWithFormat:@"%o.%o", (vals[i] >> 3) & 7, vals[i] & 7]); break;
        }
    }
    const int flags[5] = { c.sign, c.carry, c.zero, c.overflow, c.icb };
    for (int i = 0; i < 5; i++) _flag[i].state = flags[i] ? NSControlStateValueOn : NSControlStateValueOff;

    NSString *bios = c.bios == CHF_ROM_SL31253 || c.bios == CHF_ROM_SL90025
        ? nsstr(chfsession_rom_item(c.bios) ? chfsession_rom_item(c.bios)->file : "")
        : @"the app's stand-in";
    _cpuState.stringValue = [NSString stringWithFormat:
        @"%s, BIOS: %@\ncycles %llu   instructions %llu   frame %u   beam line %d, cycle %d\nillegal opcodes %u%@",
        chf_machine_name(c.machine), bios, (unsigned long long)c.cycles, (unsigned long long)c.instructions,
        c.frame, c.vpos, c.hpos, c.illegal_count,
        c.illegal_pc >= 0 ? [NSString stringWithFormat:@" (last at $%04X)", c.illegal_pc & 0xffff] : @""];

    /* the scratchpad grid, and the register picked in it */
    memcpy(_lastRegs, c.r, sizeof _lastRegs);
    [_scratch setRegs:c.r isar:c.isar];
    if (_cellIdx >= 0 && !_cellVal.currentEditor)
        _cellVal.stringValue = [NSString stringWithFormat:@"$%02X", c.r[_cellIdx & 63]];

    /* the BIOS's K stack: r59 points past the saved returns in r40-r58 */
    NSMutableString *st = [NSMutableString string];
    if (c.k_depth < 0) {
        [st appendFormat:@"r59 = $%02X is not a K-stack pointer (it must be even, 40-58)\n", c.r[59]];
    } else {
        [st appendFormat:@"depth %d   (r59 = $%02X)\n", c.k_depth, c.r[59]];
        for (int i = c.k_depth - 1; i >= 0 && i < 10; i--) {
            char label[64];
            label[0] = '\0';
            chfdebug_address_label(_dbg, c.k_stack[i], label, sizeof label);
            [st appendFormat:@" [%d] $%04X  %s\n", i, c.k_stack[i], label];
        }
        if (c.k_depth == 0) [st appendString:@" (empty)\n"];
    }
    ChfSetTextKeepingPlace(_kstack, st);
    [self refreshMem];
}

/* The memory view: MEM_BYTES from the address in the field, in the space chosen. */
- (void)refreshMem
{
    long a = 0;
    const int space = (int)_memSpace.indexOfSelectedItem;
    if (!parseHex(_memAddr.stringValue, &a) || a < 0) a = 0;
    uint8_t data[MEM_BYTES];
    memset(data, 0, sizeof data);
    const int n = chfdebug_read(_dbg, space, (uint32_t)a, data, MEM_BYTES);
    ChfSetTextKeepingPlace(_mem, n > 0 ? hexDump(data, n, (int)a, spaceFormat(space))
                                       : @"(the machine is not running)\n");
}

- (void)refreshDisasm
{
    static chfdebug_line lines[DISASM_WINDOW];
    static NSUInteger starts[DISASM_WINDOW], lens[DISASM_WINDOW];
    int pcLine = -1;
    if (_followPc.state == NSControlStateValueOn) {
        chfdebug_cpu c;
        chfdebug_cpu_get(_dbg, &c);
        /* the PC a third of the way down */
        _disasmAddr = (uint16_t)(chfdebug_row_address(_dbg, (uint16_t)c.pc, -(DISASM_WINDOW / 3)) & 0xffff);
    }
    const int n = chfdebug_disassemble(_dbg, _disasmAddr, lines, DISASM_WINDOW, &pcLine);
    _lineCount = n;
    NSMutableString *text = [NSMutableString string];
    for (int i = 0; i < n; i++) {
        _lineAddr[i] = lines[i].address;
        NSString *line = [NSString stringWithFormat:@"%c%c %04X  %-8s  %-14s %-22s%s%s\n",
                          lines[i].has_breakpoint ? '*' : ' ', lines[i].is_pc ? '>' : ' ',
                          lines[i].address, lines[i].bytes, lines[i].label, lines[i].disasm,
                          lines[i].comment[0] ? " ; " : "", lines[i].comment];
        starts[i] = text.length;
        lens[i] = line.length - 1;
        [text appendString:line];
    }
    if (n == 0) [text appendString:@"(no disassembly: the debugger is not attached)\n"];
    _disasm.string = text;
    for (int i = 0; i < n; i++) {
        if (lines[i].is_pc) {
            /* the current line in the accent, its text dark on the yellow */
            [_disasm.textStorage addAttributes:@{ NSBackgroundColorAttributeName: ChfAccentColor(),
                                                  NSForegroundColorAttributeName: NSColor.blackColor }
                                         range:NSMakeRange(starts[i], lens[i])];
        } else if (_selAddr >= 0 && lines[i].address == _selAddr) {
            [_disasm.textStorage addAttributes:@{ NSBackgroundColorAttributeName: NSColor.selectedTextBackgroundColor }
                                         range:NSMakeRange(starts[i], lens[i])];
        }
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

- (void)refreshIo
{
    chfdebug_io io;
    chfdebug_video v;
    chfdebug_cart cart;
    static chfdebug_sound_event se[SNDEV_MAX];
    static chfdebug_io_event ie[IOEV_MAX];
    chfdebug_io_get(_dbg, &io);
    chfdebug_video_get(_dbg, &v);
    chfdebug_cart_get(_dbg, &cart);
    [_ioDiag setIo:&io];

    NSMutableString *s = [NSMutableString string];
    [s appendFormat:@"%s (%s)   cartridge: %@\n\n", chf_machine_name(io.machine), io.is_pal ? "PAL" : "NTSC",
        cart.present ? nsstr(cart.type_name) : @"none"];
    [s appendFormat:@"Player 1 (right, port 1)  $%02X     Player 2 (left, port 4)  $%02X   (a bit per contact: 0 right, 1 left, 2 back, 3 forward, 4 CCW, 5 CW, 6 pull, 7 push)\n",
        io.controller[0], io.controller[1]];
    [s appendFormat:@"Console panel  $%X  (TIME %d, MODE %d, HOLD %d, START %d)   RESET %s\n\n", io.panel & 0xf, io.panel & 1,
        (io.panel >> 1) & 1, (io.panel >> 2) & 1, (io.panel >> 3) & 1, io.reset_held ? "held" : "up"];
    [s appendFormat:@"The CPU reads  port 0 $%02X   port 1 $%02X   port 4 $%02X   port 5 $%02X   (active low; controllers %s)\n\n",
        v.in0, v.in1, v.in4, v.in5, v.controllers_enabled ? "enabled" : "off"];
    [s appendFormat:@"Beeper  %@   level %d / 32767   forced on %s   %u mode changes   %llu samples made\n",
        soundModeName(io.sound_mode), io.sound_level, io.sound_forced ? "yes" : "no", io.sound_changes,
        (unsigned long long)io.audio_samples];
    if (io.has_2102)
        [s appendFormat:@"\n2102 RAM  ports $%02X/$%02X   address $%03X   bit %d\n", io.ram_port, io.ram_port + 1,
            io.ram_addr & 0x3ff, io.ram_bit];
    ChfSetTextKeepingPlace(_sound, s);

    int n = chfdebug_sound_events(_dbg, se, SNDEV_MAX);
    NSMutableString *sl = [NSMutableString stringWithString:@"frame     PC  mode\n"];
    for (int i = 0; i < n; i++)
        [sl appendFormat:@"%5u  $%04X  %@ -> %@\n", se[i].frame, se[i].pc, soundModeName(se[i].old_mode),
            soundModeName(se[i].mode)];
    if (n == 0) [sl appendString:@"(none yet)\n"];
    ChfSetLogText(_soundLog, sl, ChfTextAtEnd(_soundLog));

    n = chfdebug_io_events(_dbg, ie, IOEV_MAX);
    NSMutableString *il = [NSMutableString stringWithString:@"frame       cycle  PC     access\n"];
    for (int i = 0; i < n; i++)
        [il appendFormat:@"%5u %11llu  $%04X  %s port $%02X = $%02X\n", ie[i].frame, (unsigned long long)ie[i].cycle,
            ie[i].pc, ie[i].is_write ? "OUT" : "IN ", ie[i].port, ie[i].value];
    if (n == 0) [il appendString:@"(none yet)\n"];
    ChfSetLogText(_ioLog, il, ChfTextAtEnd(_ioLog));
}

- (void)refreshBps
{
    static chfdebug_breakpoint bps[MAX_BPS];
    const int n = chfdebug_breakpoint_list(_dbg, bps, MAX_BPS);
    for (NSView *v in [_bpList.arrangedSubviews copy]) [_bpList removeView:v];
    if (n == 0) [_bpList addArrangedSubview:ChfLabel(@"No breakpoints")];
    for (int i = 0; i < n; i++) {
        NSMutableString *what = [NSMutableString stringWithFormat:@"#%-3d ", bps[i].id];
        const unsigned a = bps[i].start, b = bps[i].end;
        switch (bps[i].type) {
        case CHFDEBUG_BP_EXEC:  [what appendFormat:@"exec         $%04X", a]; break;
        case CHFDEBUG_BP_READ:  [what appendFormat:@"data read    $%04X", a]; break;
        case CHFDEBUG_BP_WRITE: [what appendFormat:@"data write   $%04X", a]; break;
        case CHFDEBUG_BP_IN:    [what appendFormat:@"port read    $%02X", a]; break;
        case CHFDEBUG_BP_OUT:   [what appendFormat:@"port write   $%02X", a]; break;
        case CHFDEBUG_BP_VRAM:  [what appendFormat:@"VRAM write   row %u col %u", a / CHFDEBUG_VRAM_COLS, a % CHFDEBUG_VRAM_COLS]; break;
        case CHFDEBUG_BP_SCRATCH: [what appendFormat:@"scratchpad   r%u", a]; break;
        default:                [what appendFormat:@"type %d   $%04X", bps[i].type, a]; break;
        }
        if (b != a) {
            if (bps[i].type == CHFDEBUG_BP_VRAM)
                [what appendFormat:@" - row %u col %u", b / CHFDEBUG_VRAM_COLS, b % CHFDEBUG_VRAM_COLS];
            else if (bps[i].type == CHFDEBUG_BP_SCRATCH)
                [what appendFormat:@" - r%u", b];
            else
                [what appendFormat:@" - $%02X", b];
        }
        if (bps[i].type == CHFDEBUG_BP_EXEC || bps[i].type == CHFDEBUG_BP_READ || bps[i].type == CHFDEBUG_BP_WRITE) {
            char label[64];
            if (chfdebug_address_label(_dbg, bps[i].start, label, sizeof label) > 0)
                [what appendFormat:@"  %s", label];
        }
        if (bps[i].condition[0]) [what appendFormat:@"  if %s", bps[i].condition];
        [what appendFormat:@"   hits %u", bps[i].hits];
        NSButton *box = [NSButton checkboxWithTitle:what target:self action:@selector(bpEnabled:)];
        box.state = bps[i].enabled ? NSControlStateValueOn : NSControlStateValueOff;
        box.tag = bps[i].id;
        box.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
        NSButton *remove = ChfButton(@"Remove", self, @selector(bpRemove:));
        remove.tag = bps[i].id;
        [_bpList addArrangedSubview:ChfHStack(@[box, remove])];
    }
}

static NSString *bootStateName(int s)
{
    switch (s) {
    case 0:    return @"idle";
    case 1:    return @"transferring";
    case 2:    return @"ready";
    case 0x80: return @"failed";
    default:   return @"?";
    }
}

static NSString *bootErrName(int e)
{
    switch (e) {
    case 0: return @"none";
    case 1: return @"too big";
    case 2: return @"truncated";
    case 3: return @"no mapper";
    case 4: return @"store busy";
    default: return @"?";
    }
}

static NSString *linkErrName(int e)
{
    switch (e) {
    case 0: return @"ok";
    case 1: return @"no link";
    case 2: return @"timeout";
    case 3: return @"bad frame";
    case 4: return @"too big";
    default: return @"?";
    }
}

- (void)refreshCart
{
    static chfdebug_cart_event ev[CARTEV_MAX];
    chfdebug_cart c;
    char info[256];
    info[0] = '\0';
    chfdebug_cart_get(_dbg, &c);
    chfdebug_cart_info(_dbg, info, sizeof info);

    NSMutableString *s = [NSMutableString stringWithFormat:@"%@\n\n", ChfStripControl(info)];
    if (!c.present) {
        [s appendString:@"(no cartridge in the slot)\n"];
        ChfSetTextKeepingPlace(_cart, s);
        ChfSetTextKeepingPlace(_mailbox, @"");
        ChfSetTextKeepingPlace(_cartLog, @"");
        return;
    }
    [s appendFormat:@"Type        %@\n", nsstr(c.type_name)];
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
        [s appendFormat:@"Boot        state %d (%@)   %d%%   error %d (%@)\n", c.boot_state, bootStateName(c.boot_state),
            c.boot_pct, c.boot_err, bootErrName(c.boot_err)];
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
        [s appendFormat:@"Software list  %@\n", c.softlist_name ? nsstr(c.softlist_name) : @"(not in MAME's list)"];
        if (c.description && c.description[0]) [s appendFormat:@"              %@\n", nsstr(c.description)];
        [s appendFormat:@"ROM          %u bytes   CRC-32 %08X\n", c.rom_size, c.rom_crc];
        if (c.ram_size) [s appendFormat:@"RAM          %u %s\n", c.ram_size, c.ram_size == 1024 ? "bits (a 2102)" : "bytes"];
        if (c.type_name && !strncmp(c.type_name, "Multigame", 9))
            [s appendFormat:@"Bank         %d   half %d\n", c.bank, c.half];
    }
    ChfSetTextKeepingPlace(_cart, s);
    ChfSetTextKeepingPlace(_mailbox, hexDump(c.mailbox_page, (int)sizeof c.mailbox_page, 0xF800, @"$%04X: "));

    const int n = chfdebug_cart_events(_dbg, ev, CARTEV_MAX);
    NSMutableString *log = [NSMutableString stringWithString:@"frame     PC  event\n"];
    static const char *const kinds[5] = { "?", "REG", "TX", "BANK", "SWAP" };
    for (int i = 0; i < n; i++)
        [log appendFormat:@"%5u  $%04X  %s reg $%02X = $%02X\n", ev[i].frame, ev[i].pc,
            kinds[ev[i].kind < 5 ? ev[i].kind : 0], ev[i].reg, ev[i].value];
    if (n == 0) [log appendString:@"(none yet)\n"];
    ChfSetLogText(_cartLog, log, ChfTextAtEnd(_cartLog));
}

/* The packet log is rebuilt only when it changed: a new packet, or a
 * repeat (which moves the newest serial on). */
- (void)refreshFujinet:(BOOL)force
{
    static chfdebug_fuji_packet p[TRACE_MAX];
    static char state[2048];
    char status[128];
    int n = chfdebug_fujinet_trace(_dbg, p, TRACE_MAX);
    if (n < 0) n = 0;

    status[0] = '\0';
    if (chfsession_fujinet_external(_session)) {
        chfsession_cart_status(_session, status, (int)sizeof status);
        _fujiNote.stringValue = [NSString stringWithFormat:@"External FujiNet at %s. Cartridge: %@.",
                                 chfsession_boip_endpoint(_session), ChfStripControl(status)];
    } else if (!chfsession_fujinet_running(_session)) {
        _fujiNote.stringValue = @"FujiNet is off (Settings ▸ FujiNet).";
    } else {
        chfsession_cart_status(_session, status, (int)sizeof status);
        _fujiNote.stringValue = [NSString stringWithFormat:@"FujiNet is on. Cartridge: %@.", ChfStripControl(status)];
    }
    state[0] = '\0';
    chfdebug_fujinet_state(_dbg, state, (int)sizeof state);
    ChfSetTextKeepingPlace(_fujiState, ChfStripControl(state));

    if (!force && n == _fujiCount && (n == 0 || p[n - 1].serial == _fujiLast)) return;
    _fujiCount = n;
    _fujiLast = n > 0 ? p[n - 1].serial : 0;
    const BOOL follow = force || ChfTextAtEnd(_fujiText);
    NSMutableString *s = [NSMutableString string];
    for (int i = 0; i < n; i++) {
        if (i) [s appendString:@"\n"];
        [s appendString:[NSString stringWithUTF8String:p[i].text] ?: @""];
        if (p[i].repeat > 1) [s appendFormat:@" ×%u", p[i].repeat];
        [s appendString:@"\n     "];
        for (int k = 0; k < p[i].nbytes && k < (int)sizeof p[i].data; k++)
            [s appendFormat:@"%02X ", p[i].data[k]];
        if (p[i].nbytes > (int)sizeof p[i].data) [s appendString:@"…"];
    }
    if (n == 0) [s appendString:@"(no FujiBus packets since the trace was cleared)"];
    ChfSetLogText(_fujiText, s, follow);
}

/* Only the tab that shows is brought up to date. */
- (void)refreshTab:(NSInteger)tab
{
    switch (tab) {
    case TAB_CPU:         [self refreshCpu]; break;
    case TAB_DISASM:      [self refreshDisasm]; break;
    case TAB_VIDEO:       [_video refresh]; break;
    case TAB_IO:          [self refreshIo]; break;
    case TAB_BREAKPOINTS: [self refreshBps]; break;
    case TAB_CART:        [self refreshCart]; break;
    case TAB_FUJINET:     [self refreshFujinet:NO]; break;
    default: break;
    }
}

- (void)refreshAll
{
    [self refreshStatus];
    [self refreshTab:[self selectedTab]];
}
@end
