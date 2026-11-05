/*
 * Debugger window (AppKit) over this app's Signetics 2650 / 2637 UVI debugger
 * engine, via core/include/chfdebug.h. Mirrors the GTK, Qt and Win32 debuggers
 * tab for tab: Prompt, CPU & RAM, Disassembly, Video (the UVI: VideoTab.m),
 * Sound & I/O, Breakpoints, Cart, FujiNet.
 *
 * Opening the window attaches the engine, which stops the machine; closing
 * it detaches, and the machine runs on. The window refreshes only while it
 * can be seen, and only the tab that shows: a stopped machine on a change, a
 * running one a couple of times a second (the Video tab ten).
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

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "chfdebug.h"

/* chfdebug_cart.type of the FujiNet cartridge (1 standard, 2 Golf, 3 FujiNet) */
#define CART_TYPE_FUJINET 3

#define DISASM_WINDOW 48
#define MAX_BPS 64
#define NUM_SCALARS 12       /* R0 R1-R3 R1'-R3' PC IAR PSU PSL SP */
#define MEM_BYTES 256        /* the memory view */
#define TRACE_MAX 512        /* FujiNet packets shown */

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

/* ---- the keypads, the discs and the panel, as the console sees them ---------------- */

#define IO_KEY_W  34.0
#define IO_KEY_H  22.0
#define IO_PAD_GAP 18.0
#define IO_DISC_R 34.0

/* Both 12-key keypads (a cell per key, filled while held), the panel's three
 * buttons and RESET, and each hand controller's disc (a dot where it is, the
 * analog deflection when there is one) with its fire button. */
@interface ChfIoDiagramView : NSView
- (void)setIo:(const chfdebug_io *)io;
@end

@implementation ChfIoDiagramView {
    uint64_t _keys;
    uint8_t _sticks[2];
    int8_t _disc[2][2];
    int _reset;
}
- (BOOL)isFlipped { return YES; }
- (NSSize)intrinsicContentSize
{
    return NSMakeSize(560, 122);
}

- (void)setIo:(const chfdebug_io *)io
{
    if (io->keys == _keys && !memcmp(io->sticks, _sticks, sizeof _sticks) &&
        !memcmp(io->disc, _disc, sizeof _disc) && io->reset_held == _reset)
        return;
    _keys = io->keys;
    memcpy(_sticks, io->sticks, sizeof _sticks);
    memcpy(_disc, io->disc, sizeof _disc);
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
    NSDictionary *a = @{ NSFontAttributeName: [NSFont systemFontOfSize:8],
                         NSParagraphStyleAttributeName: para,
                         NSForegroundColorAttributeName: lit ? NSColor.blackColor : NSColor.labelColor };
    const NSSize sz = [text sizeWithAttributes:a];
    [text drawInRect:NSMakeRect(r.origin.x + 1, NSMidY(r) - sz.height / 2, r.size.width - 2, sz.height)
      withAttributes:a];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    NSDictionary *head = @{ NSFontAttributeName: [NSFont systemFontOfSize:9],
                            NSForegroundColorAttributeName: NSColor.secondaryLabelColor };
    const CGFloat top = 16;
    CGFloat x = 4;
    for (int p = 0; p < 2; p++) {
        [[NSString stringWithFormat:@"Player %d", p + 1] drawAtPoint:NSMakePoint(x, 2) withAttributes:head];
        for (int k = 0; k < 12; k++) {
            const int key = CHF_KEY_PAD_BASE(p) + k;
            const char *label = chfsession_key_label(0, key);
            [self cell:NSMakeRect(x + (k % 3) * IO_KEY_W, top + (k / 3) * IO_KEY_H, IO_KEY_W, IO_KEY_H)
                  text:(label ? [NSString stringWithUTF8String:label] : @"")
                   lit:((_keys >> key) & 1u) != 0];
        }
        /* the disc: a ring with a dot at the pot position */
        const CGFloat dx = x + 3 * IO_KEY_W + IO_PAD_GAP + IO_DISC_R, dy = top + IO_DISC_R + 4;
        NSBezierPath *ring = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(dx - IO_DISC_R, dy - IO_DISC_R,
                                                                             2 * IO_DISC_R, 2 * IO_DISC_R)];
        [NSColor.separatorColor setStroke];
        [ring stroke];
        const unsigned s = _sticks[p];
        double kx = _disc[p][0] / 127.0, ky = _disc[p][1] / 127.0;
        if (_disc[p][0] == 0 && _disc[p][1] == 0) {
            kx = ((s >> 1) & 1u) - ((s >> 3) & 1u);
            ky = ((s >> 2) & 1u) - (s & 1u);
            if (kx != 0 && ky != 0) { kx *= 0.7071; ky *= 0.7071; }
        }
        const NSRect dot = NSMakeRect(dx + kx * (IO_DISC_R - 7) - 6, dy + ky * (IO_DISC_R - 7) - 6, 12, 12);
        NSBezierPath *d = [NSBezierPath bezierPathWithOvalInRect:dot];
        [((kx != 0 || ky != 0) ? ChfAccentColor() : NSColor.tertiaryLabelColor) setFill];
        [d fill];
        [self cell:NSMakeRect(dx - IO_DISC_R, dy + IO_DISC_R + 4, 2 * IO_DISC_R, IO_KEY_H - 4) text:@"FIRE"
               lit:(s & 16u) != 0];
        x += 3 * IO_KEY_W + IO_PAD_GAP + 2 * IO_DISC_R + 40;
        if (p == 0) {
            /* the panel between the two controllers */
            static const char *const names[3] = { "START", "OPTION", "SELECT" };
            for (int b = 0; b < 3; b++)
                [self cell:NSMakeRect(x, top + b * IO_KEY_H, 56, IO_KEY_H)
                      text:[NSString stringWithUTF8String:names[b]] lit:((_keys >> (CHF_KEY_START + b)) & 1u) != 0];
            [self cell:NSMakeRect(x, top + 3 * IO_KEY_H, 56, IO_KEY_H) text:@"RESET" lit:_reset != 0];
            x += 56 + 30;
        }
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

    NSTextField *_reg[NUM_SCALARS];
    NSButton *_flag[8];
    NSButton *_stat[3];
    NSTextField *_cpuState, *_beam;
    NSTextView *_stack, *_mem;
    NSPopUpButton *_memSpace, *_ramSpace;
    NSTextField *_memAddr, *_ramAddr, *_ramVal;

    NSButton *_followPc;
    NSTextField *_jump;
    ChfLineTextView *_disasm;
    uint16_t _disasmAddr;
    uint16_t _lineAddr[DISASM_WINDOW];
    int _lineCount;

    ChfVideoTab *_video;

    NSTextView *_sound;
    ChfIoDiagramView *_ioDiag;

    NSPopUpButton *_bpType;
    NSTextField *_bpRange, *_bpCond;
    NSStackView *_bpList;

    NSTextView *_cart, *_mailbox;

    NSTextField *_fujiNote;
    NSTextView *_fujiText, *_fujiState;
    uint64_t _fujiLast;
    int _fujiCount;
}

/* The scalar registers, in the order the CPU tab indexes them: each with its
 * chfdebug_reg and the hex digits it shows. */
static const struct { NSString *__unsafe_unretained name; int reg; int digits; } kRegs[NUM_SCALARS] = {
    { @"R0", CHF_REG_R0, 2 },   { @"R1", CHF_REG_R1, 2 },   { @"R2", CHF_REG_R2, 2 },
    { @"R3", CHF_REG_R3, 2 },   { @"R1'", CHF_REG_R1B, 2 }, { @"R2'", CHF_REG_R2B, 2 },
    { @"R3'", CHF_REG_R3B, 2 }, { @"PC", CHF_REG_PC, 4 },   { @"IAR", CHF_REG_IAR, 4 },
    { @"PSU", CHF_REG_PSU, 2 }, { @"PSL", CHF_REG_PSL, 2 }, { @"SP", CHF_REG_SP, 1 },
};
/* Where each of the three register columns puts the scalars above (-1: blank). */
static const int kRegColumns[3][5] = {
    { 0, 1, 2, 3, -1 }, { -1, 4, 5, 6, -1 }, { 7, 8, 9, 10, 11 },
};

/* The status bits as checkboxes, and the register each sets. */
static const struct { NSString *__unsafe_unretained name; int reg; } kFlags[8] = {
    { @"F", CHF_REG_F }, { @"II", CHF_REG_II }, { @"IDC", CHF_REG_IDC }, { @"RS (bank 1)", CHF_REG_RS },
    { @"WC", CHF_REG_WC }, { @"OVF", CHF_REG_OVF }, { @"COM", CHF_REG_COM }, { @"C", CHF_REG_C },
};

#define NUM_FILE_KINDS 7
static const char *const kFileKinds[NUM_FILE_KINDS] = { "dis", "mailbox", "uvi", "font", "cart", "screen", "regs" };
static NSString *const kFileTitles[NUM_FILE_KINDS] = {
    @"Disassembly…", @"Mailbox page…", @"UVI registers…", @"Glyph table…",
    @"Cartridge image…", @"Screen (PPM)…", @"Registers…"
};
static NSString *const kFileNames[NUM_FILE_KINDS] = {
    @"disassembly.asm", @"mailbox.bin", @"uvi.bin", @"font.bin", @"cart.bin", @"screen.ppm", @"registers.txt"
};

/* CHF_DEBUGGER_TAB takes an index or one of these. */
static const char *const kTabNames[NUM_TABS] = {
    "prompt", "cpu", "disassembly", "video", "io", "breakpoints", "cart", "fujinet"
};

/* ---- helpers ------------------------------------------------------------------------- */

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
    long v = strtol(t.UTF8String, &end, base);
    if (*end) return NO;
    *out = v;
    return YES;
}

/* A label, or a number. -1 when neither. */
- (int)resolveAddr:(NSString *)text
{
    long v;
    NSString *t = [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (t.length == 0) return -1;
    int addr = chfdebug_label_address(_dbg, t.UTF8String);
    if (addr < 0 && parseNum(t, &v) && v >= 0 && v <= 0x7fff) addr = (int)v;
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

/* A button lit in the accent, its title dark on the gold; or plain. */
static void setButtonLit(NSButton *b, NSString *title, BOOL lit)
{
    b.bezelColor = lit ? ChfAccentColor() : nil;
    b.attributedTitle = [[NSAttributedString alloc] initWithString:title attributes:@{
        NSFontAttributeName: [NSFont systemFontOfSize:NSFont.systemFontSize],
        NSForegroundColorAttributeName: lit ? NSColor.blackColor : NSColor.controlTextColor }];
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
        /* running: the UVI live, ten times a second */
        [_video refresh];
    } else if (slow && (!stopped || tab == TAB_FUJINET)) {
        if (!stopped) [self refreshStatus];
        /* a running machine has no disassembly worth redrawing */
        if (!(tab == TAB_DISASM && !stopped)) [self refreshTab:tab];
    }
}

- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    [self refreshTab:[_tabs indexOfTabViewItem:item]];
}

/* ---- construction -------------------------------------------------------------------- */

- (NSView *)buildPrompt
{
    NSScrollView *outScroll;
    _promptOut = ChfMonoView(&outScroll, YES);
    _promptOut.string = @"Debugger prompt. Type 'help' for every command.\n";
    _promptIn = ChfField(@"command (help, step, break $0412, bpw $18F0, print r0, mem prog $400 32, uvi, ...) — Tab completes, ↑↓ history",
                        0, self, @selector(runPrompt:));
    _promptIn.delegate = self;

    NSStackView *v = ChfVStack(@[outScroll, _promptIn]);
    ChfFill(v, outScroll);
    ChfFill(v, _promptIn);
    return v;
}

/* R0-R3 (the bank in use), R1'-R3' (bank 1), PC, IAR, PSU, PSL and SP as
 * fields, the status bits as boxes (editable while stopped), the return
 * address stack, and a memory view with a write box. */
- (NSView *)buildCpu
{
    for (int i = 0; i < NUM_SCALARS; i++) {
        const int digits = kRegs[i].digits;
        _reg[i] = ChfField(@"", digits == 4 ? 44 : (digits == 2 ? 36 : 28), self, @selector(applyRegister:));
        _reg[i].tag = i;
    }
    NSMutableArray<NSArray<NSView *> *> *rows = [NSMutableArray array];
    for (int row = 0; row < 5; row++) {
        NSMutableArray<NSView *> *cells = [NSMutableArray array];
        for (int col = 0; col < 3; col++) {
            const int idx = kRegColumns[col][row];
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

    NSMutableArray<NSView *> *flags = [NSMutableArray arrayWithObject:ChfLabel(@"PSU / PSL")];
    for (int i = 0; i < 8; i++) {
        _flag[i] = [NSButton checkboxWithTitle:kFlags[i].name target:self action:@selector(flagClicked:)];
        _flag[i].tag = i;
        [flags addObject:_flag[i]];
    }
    static NSString *const statTitles[3] = { @"S (SENSE: vblank)", @"halted", @"IRQ line" };
    NSMutableArray<NSView *> *stats = [NSMutableArray arrayWithObject:ChfLabel(@"Inputs")];
    for (int i = 0; i < 3; i++) {
        _stat[i] = [NSButton checkboxWithTitle:statTitles[i] target:nil action:NULL];
        _stat[i].enabled = NO;   /* the machine's own state: shown, not set here */
        [stats addObject:_stat[i]];
    }
    _cpuState = ChfLabel(@"");
    _cpuState.maximumNumberOfLines = 3;
    _beam = ChfNote(@"");
    _beam.maximumNumberOfLines = 3;
    NSTextField *hint = ChfNote(@"Enter applies an edit while the machine is stopped. R1'-R3' are register bank 1 (RS); R0 is the same in both banks");

    NSScrollView *stackScroll, *memScroll;
    _stack = ChfMonoView(&stackScroll, NO);
    [stackScroll.heightAnchor constraintEqualToConstant:130].active = YES;

    _mem = ChfMonoView(&memScroll, NO);
    _memSpace = [[NSPopUpButton alloc] init];
    [_memSpace addItemsWithTitles:@[@"Program (CPU view)", @"UVI registers", @"Cartridge image"]];
    _memSpace.target = self;
    _memSpace.action = @selector(memViewChanged:);
    _memAddr = ChfField(@"$0000", 70, self, @selector(memViewChanged:));
    _memAddr.stringValue = @"$0000";
    NSStackView *memRow = ChfHStack(@[ChfLabel(@"Show"), _memSpace, ChfLabel(@"from"), _memAddr,
                                      ChfButton(@"Go", self, @selector(memViewChanged:))]);

    _ramSpace = [[NSPopUpButton alloc] init];
    [_ramSpace addItemsWithTitles:@[@"Program (cartridge buffer / UVI)", @"UVI registers", @"Cartridge image"]];
    _ramAddr = ChfField(@"$1800", 70, self, @selector(writeRam:));
    _ramVal = ChfField(@"$00", 60, self, @selector(writeRam:));
    NSStackView *edit = ChfHStack(@[ChfLabel(@"Write"), _ramSpace, _ramAddr, ChfLabel(@"="), _ramVal,
                                    ChfButton(@"Write", self, @selector(writeRam:))]);
    NSTextField *ramHint = ChfNote(@"The console has no RAM of its own: a program address in the UVI window goes to the chip, one the cartridge serves into its own buffer; the rest is ROM");

    NSStackView *left = ChfVStack(@[regs, ChfHStack(flags), ChfHStack(stats), hint, _cpuState, _beam,
                                    ChfTitle(@"Return address stack (8 deep; the top is at SP)"), stackScroll]);
    NSStackView *right = ChfVStack(@[ChfTitle(@"Memory"), memRow, memScroll, edit, ramHint]);
    ChfFill(right, memScroll);
    NSStackView *h = ChfHStack(@[left, right]);
    h.alignment = NSLayoutAttributeTop;
    h.spacing = 16;
    [right.widthAnchor constraintGreaterThanOrEqualToConstant:420].active = YES;
    return h;
}

- (NSView *)buildDisasm
{
    _followPc = [NSButton checkboxWithTitle:@"Follow PC" target:self action:@selector(refreshDisasm)];
    _followPc.state = NSControlStateValueOn;
    _jump = ChfField(@"address or label", 150, self, @selector(jumpTo:));
    NSTextField *hint = ChfNote(@"Click a line to toggle its breakpoint (*); scroll to browse");
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

/* The keypads, discs and panel live over the ports' decoded text. */
- (NSView *)buildIo
{
    NSScrollView *scroll;
    _sound = ChfMonoView(&scroll, NO);
    _ioDiag = [[ChfIoDiagramView alloc] initWithFrame:NSZeroRect];
    ChfFixSize(_ioDiag);
    NSTextField *hint = ChfNote(@"Filled: held, as the console sees it (the discs show the pot position; FIRE is the keypad 2 wire).");
    NSStackView *v = ChfVStack(@[ChfTitle(@"Keypads, discs, panel and RESET"), _ioDiag, hint, scroll]);
    v.distribution = NSStackViewDistributionFill;   /* the text takes the rest of the height */
    ChfFill(v, scroll);
    return v;
}

- (NSView *)buildBreaks
{
    _bpType = [[NSPopUpButton alloc] init];
    [_bpType addItemsWithTitles:@[@"Execute", @"Read", @"Write", @"Read/Write"]];
    _bpRange = ChfField(@"$0412, $0400-$04FF, $18F0 (the UVI is at $1800-$1AFF), or a label", 330,
                        self, @selector(addBreakpoint:));
    _bpCond = ChfField(@"condition (optional): r0 == $10 && vpos > 100", 0, self, @selector(addBreakpoint:));
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
    NSTextField *hint = ChfNote(@"Untick to disable; conditions are C expressions over registers, flags, [addr], value, addr, vpos, hpos; a write breakpoint only has the UVI to watch");
    NSStackView *v = ChfVStack(@[row, hint, bpScroll]);
    ChfFill(v, row);
    ChfFill(v, bpScroll);
    [bpScroll setContentHuggingPriority:NSLayoutPriorityDefaultLow
                         forOrientation:NSLayoutConstraintOrientationVertical];
    return v;
}

/* The cartridge's state as text left; the mailbox page ($2B00-$2FFF) as the
 * console reads it, right. */
- (NSView *)buildCart
{
    NSScrollView *scroll, *mailScroll;
    _cart = ChfMonoView(&scroll, NO);
    _mailbox = ChfMonoView(&mailScroll, NO);
    NSStackView *right = ChfVStack(@[ChfTitle(@"The mailbox page ($2B00-$2FFF)"), mailScroll]);
    right.distribution = NSStackViewDistributionFill;
    ChfFill(right, mailScroll);
    NSStackView *h = ChfHStack(@[scroll, right]);
    h.alignment = NSLayoutAttributeTop;
    h.spacing = 12;
    [scroll.widthAnchor constraintGreaterThanOrEqualToConstant:420].active = YES;
    [right.widthAnchor constraintEqualToConstant:420].active = YES;
    [scroll.heightAnchor constraintEqualToAnchor:h.heightAnchor].active = YES;
    [right.heightAnchor constraintEqualToAnchor:h.heightAnchor].active = YES;
    return h;
}

/* The packets left (a note and Clear over them), the cartridge's mailbox
 * right. */
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
    NSStackView *left = ChfVStack(@[ChfTitle(@"FujiBus packets (SLIP-decoded)"), row, fujiScroll]);
    left.distribution = NSStackViewDistributionFill;   /* the log takes the column's height */
    ChfFill(left, row);
    ChfFill(left, fujiScroll);
    NSStackView *right = ChfVStack(@[ChfTitle(@"The cartridge's mailbox"), stateScroll]);
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
    NSButton *scan = ChfButton(@"Scanline+1", self, @selector(scanline:));
    NSButton *frame = ChfButton(@"Frame+1", self, @selector(frame:));
    _runBtn.keyEquivalent = [NSString stringWithFormat:@"%C", (unichar)NSF5FunctionKey];
    _runBtn.keyEquivalentModifierMask = 0;
    step.keyEquivalent = [NSString stringWithFormat:@"%C", (unichar)NSF7FunctionKey];
    step.keyEquivalentModifierMask = 0;
    over.keyEquivalent = [NSString stringWithFormat:@"%C", (unichar)NSF8FunctionKey];
    over.keyEquivalentModifierMask = 0;
    outBtn.keyEquivalent = [NSString stringWithFormat:@"%C", (unichar)NSF8FunctionKey];
    outBtn.keyEquivalentModifierMask = NSEventModifierFlagShift;
    NSPopUpButton *symbols = [self pullDown:@"Load Symbols…"
                                      items:@[@"From a File…", @"Beside the Cartridge"]
                                     action:@selector(loadSymbols:)];
    NSMutableArray *saveTitles = [NSMutableArray array];
    for (int i = 0; i < NUM_FILE_KINDS; i++) [saveTitles addObject:kFileTitles[i]];
    NSPopUpButton *save = [self pullDown:@"Save…" items:saveTitles action:@selector(saveFile:)];
    _status = ChfNote(@"Running");
    _status.alignment = NSTextAlignmentRight;
    _status.lineBreakMode = NSLineBreakByTruncatingTail;
    NSStackView *toolbar = ChfHStack(@[_runBtn, step, over, outBtn, scan, frame, symbols, save, _status]);
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
        if (t < 0 && !strcasecmp(tab, "uvi")) t = TAB_VIDEO;
        if (t < 0 && !strcasecmp(tab, "sound")) t = TAB_IO;
        if (t < 0 && tab[0] >= '0' && tab[0] <= '9') t = atoi(tab);
        if (t >= 0 && t < (int)_tabs.numberOfTabViewItems) [_tabs selectTabViewItemAtIndex:t];
    }
    _tabs.delegate = self;

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
    NSString *cmd = [_promptIn.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
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

/* "From a File…" asks; "Beside the Cartridge" takes <cart>.map/.lst/.sym
 * next to the opened cartridge. */
- (void)loadSymbols:(NSPopUpButton *)sender
{
    char msg[512];
    msg[0] = '\0';
    if (sender.selectedItem.tag == 1) {
        chfdebug_load_symbols(_dbg, NULL, msg, sizeof msg);
    } else {
        NSOpenPanel *p = [NSOpenPanel openPanel];
        p.title = @"Load Symbols (AS .map or .lst, WLA-DX .sym, or an address list)";
        p.allowedContentTypes = typesForExtensions(@[@"map", @"lst", @"sym", @"txt"]);
        const char *cart = chfsession_cart_path(_session);
        if (cart && cart[0])
            [p setDirectoryURL:[NSURL fileURLWithPath:[[NSString stringWithUTF8String:cart]
                                                          stringByDeletingLastPathComponent] isDirectory:YES]];
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
    if (parseNum(sender.stringValue, &v) && tag >= 0 && tag < NUM_SCALARS)
        chfdebug_cpu_set(_dbg, kRegs[tag].reg, (int)v);
    [self refreshAll];
}

- (void)flagClicked:(NSButton *)sender
{
    if (!chfdebug_is_stopped(_dbg)) {
        [self refreshCpu];
        _status.stringValue = @"Stop the machine (F5) to change a flag";
        return;
    }
    if (sender.tag >= 0 && sender.tag < 8)
        chfdebug_cpu_set(_dbg, kFlags[sender.tag].reg, sender.state == NSControlStateValueOn ? 1 : 0);
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
    const int space = (int)_ramSpace.indexOfSelectedItem;     /* CHF_SPACE_PROG, _UVI, _CART in order */
    const int size = chfdebug_space_size(_dbg, space);
    if (!parseNum(_ramAddr.stringValue, &a) || !parseNum(_ramVal.stringValue, &v) || a < 0 || a >= size) {
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
    _disasmAddr = (uint16_t)(addr & 0x7fff);
    [self refreshDisasm];
}

- (void)addBreakpoint:(id)sender
{
    (void)sender;
    static const int types[4] = {
        CHFDEBUG_BP_EXEC, CHFDEBUG_BP_READ, CHFDEBUG_BP_WRITE, CHFDEBUG_BP_READ | CHFDEBUG_BP_WRITE
    };
    const NSInteger sel = _bpType.indexOfSelectedItem;
    const int type = types[sel >= 0 && sel < 4 ? sel : 0];
    NSString *range = [_bpRange.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (range.length == 0) return;
    NSArray<NSString *> *ends = [range componentsSeparatedByString:@"-"];
    const int start = [self resolveAddr:ends[0]];
    const int end = ends.count > 1 ? [self resolveAddr:ends[1]] : start;
    /* program space is 15 bits */
    if (start < 0 || end < 0 || end < start || end > 0x7fff) {
        [self appendPrompt:[NSString stringWithFormat:@"breakpoint: bad address %@ (up to $7FFF)\n", range]];
        _status.stringValue = [NSString stringWithFormat:@"Bad address: %@", range];
        return;
    }
    const int bpId = chfdebug_breakpoint_add(_dbg, type, CHF_SPACE_PROG, (uint16_t)start, (uint16_t)end,
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
    const int bank = c.rs ? 1 : 0;      /* R1-R3 are the bank the instructions name */
    const int vals[NUM_SCALARS] = { c.r0, c.r[bank][1], c.r[bank][2], c.r[bank][3], c.r[1][1], c.r[1][2], c.r[1][3],
                                    c.pc, c.iar, c.psu, c.psl, c.sp };
    for (int i = 0; i < NUM_SCALARS; i++) {
        switch (kRegs[i].digits) {
        case 4:  setField(_reg[i], [NSString stringWithFormat:@"%04X", vals[i] & 0x7fff]); break;
        case 2:  setField(_reg[i], [NSString stringWithFormat:@"%02X", vals[i] & 0xff]); break;
        default: setField(_reg[i], [NSString stringWithFormat:@"%X", vals[i] & 0xf]); break;
        }
    }
    const int flags[8] = { c.flag, c.ii, c.idc, c.rs, c.wc, c.ovf, c.com, c.c };
    for (int i = 0; i < 8; i++) _flag[i].state = flags[i] ? NSControlStateValueOn : NSControlStateValueOff;
    const int stats[3] = { c.sense, c.halted, c.irq_line };
    for (int i = 0; i < 3; i++) _stat[i].state = stats[i] ? NSControlStateValueOn : NSControlStateValueOff;

    static const char *const ccs[4] = { "zero", "positive", "negative", "?" };
    _cpuState.stringValue = [NSString stringWithFormat:
        @"PC $%04X (page $%04X, IAR $%04X)   PSU $%02X: S %d F %d II %d SP %d   PSL $%02X: CC %s IDC %d RS %d WC %d OVF %d COM %d C %d%@",
        c.pc & 0x7fff, c.page, c.iar, c.psu & 0xff, c.sense, c.flag, c.ii, c.sp, c.psl & 0xff,
        ccs[c.cc >= 0 && c.cc < 3 ? c.cc : 3], c.idc, c.rs, c.wc, c.ovf, c.com, c.c,
        c.halted ? @"   HALTED" : @""];
    _beam.stringValue = [NSString stringWithFormat:
        @"beam line %d   pixel %d   frame %u   cycles %llu   instructions %llu   illegal opcodes %u%@",
        c.vpos, c.hpos, c.frame, (unsigned long long)c.cycles, (unsigned long long)c.instructions,
        c.illegal_count, c.illegal_pc >= 0 ? [NSString stringWithFormat:@" (last at $%04X)", c.illegal_pc] : @""];

    NSMutableString *st = [NSMutableString string];
    for (int i = 7; i >= 0; i--)
        [st appendFormat:@"%s [%d] $%04X\n", i == c.sp ? ">" : " ", i, c.ras[i] & 0x7fff];
    ChfSetTextKeepingPlace(_stack, st);
    [self refreshMem];
}

/* The memory view: MEM_BYTES from the address in the field, in the space chosen. */
- (void)refreshMem
{
    long a = 0;
    const int space = (int)_memSpace.indexOfSelectedItem;
    if (!parseNum(_memAddr.stringValue, &a) || a < 0) a = 0;
    uint8_t data[MEM_BYTES];
    memset(data, 0, sizeof data);
    const int n = chfdebug_read(_dbg, space, (uint32_t)a, data, MEM_BYTES);
    ChfSetTextKeepingPlace(_mem, n > 0 ? hexDump(data, n, (int)a, space == CHF_SPACE_CART ? @"$%05X: " : @"$%04X: ")
                                       : @"(the machine is not running)\n");
}

- (void)refreshDisasm
{
    static chfdebug_line lines[DISASM_WINDOW];
    int pcLine = -1;
    if (_followPc.state == NSControlStateValueOn) {
        chfdebug_cpu c;
        chfdebug_cpu_get(_dbg, &c);
        /* the PC a third of the way down */
        _disasmAddr = (uint16_t)(chfdebug_row_address(_dbg, (uint16_t)c.pc, -(DISASM_WINDOW / 3)) & 0x7fff);
    }
    const int n = chfdebug_disassemble(_dbg, _disasmAddr, lines, DISASM_WINDOW, &pcLine);
    _lineCount = n;
    NSMutableString *text = [NSMutableString string];
    NSUInteger pcStart = 0, pcLen = 0;
    BOOL havePc = NO;
    for (int i = 0; i < n; i++) {
        _lineAddr[i] = lines[i].address;
        NSString *line = [NSString stringWithFormat:@"%c%c %04X  %-8s  %-14s %-22s%s%s\n",
                          lines[i].has_breakpoint ? '*' : ' ', lines[i].is_pc ? '>' : ' ',
                          lines[i].address, lines[i].bytes, lines[i].label, lines[i].disasm,
                          lines[i].comment[0] ? " ; " : "", lines[i].comment];
        if (lines[i].is_pc) { havePc = YES; pcStart = text.length; pcLen = line.length - 1; }
        [text appendString:line];
    }
    if (n == 0) [text appendString:@"(no disassembly: the debugger is not attached)\n"];
    _disasm.string = text;
    if (havePc) {
        /* the current line in the accent, its text dark on the gold */
        [_disasm.textStorage addAttributes:@{ NSBackgroundColorAttributeName: ChfAccentColor(),
                                              NSForegroundColorAttributeName: NSColor.blackColor }
                                     range:NSMakeRange(pcStart, pcLen)];
    }
}

- (void)refreshIo
{
    chfdebug_io io;
    chfdebug_io_get(_dbg, &io);
    [_ioDiag setIo:&io];
    chfdebug_uvi uvi;
    chfdebug_uvi_get(_dbg, &uvi);

    NSMutableString *s = [NSMutableString string];
    [s appendFormat:@"%s%s\n\n", chf_machine_name(io.machine), io.is_pal ? " (PAL clone, not verified against MAME)" : ""];
    [s appendFormat:@"Cartridge   type %d   SENSE %d   FLAG %d   RESET %s\n", io.cart_type, io.sense, io.flag,
        io.reset_held ? "held" : "up"];
    [s appendFormat:@"Discs       1: $%02X  2: $%02X (bit 0 up, 1 right, 2 down, 3 left, 4 fire)\n",
        io.sticks[0], io.sticks[1]];
    [s appendFormat:@"            analog 1: x %d y %d   analog 2: x %d y %d (0 = digital)\n",
        io.disc[0][0], io.disc[0][1], io.disc[1][0], io.disc[1][1]];
    [s appendFormat:@"Pots        disc 1 x $%02X y $%02X   disc 2 x $%02X y $%02X\n",
        io.pot[0][0], io.pot[0][1], io.pot[1][0], io.pot[1][1]];
    [s appendString:@"Keypads     $1900-$1907:"];
    for (int i = 0; i < 8; i++) [s appendFormat:@" %02X", io.pad[i]];
    [s appendFormat:@"\nPanel       $1908 = $%02X (START %d, OPTION %d, SELECT %d)\n\n", io.panel, io.panel & 1,
        (io.panel >> 1) & 1, (io.panel >> 2) & 1];

    [s appendFormat:@"UVI sound   pitch divider %d   volume %d/7   tone %s   noise %s\n", uvi.sound_pitch,
        uvi.sound_volume, uvi.sound_tone ? "on" : "off", uvi.sound_noise ? "on" : "off"];
    [s appendFormat:@"Audio       %llu samples made\n", (unsigned long long)io.audio_samples];
    ChfSetTextKeepingPlace(_sound, s);
}

- (void)refreshBps
{
    static chfdebug_breakpoint bps[MAX_BPS];
    const int n = chfdebug_breakpoint_list(_dbg, bps, MAX_BPS);
    for (NSView *v in [_bpList.arrangedSubviews copy]) [_bpList removeView:v];
    if (n == 0) [_bpList addArrangedSubview:ChfLabel(@"No breakpoints")];
    for (int i = 0; i < n; i++) {
        static const struct { int bit; const char *name; } kinds[3] = {
            { CHFDEBUG_BP_EXEC, "exec" }, { CHFDEBUG_BP_READ, "read" }, { CHFDEBUG_BP_WRITE, "write" },
        };
        NSMutableString *what = [NSMutableString stringWithFormat:@"#%-3d ", bps[i].id];
        BOOL first = YES;
        for (int k = 0; k < 3; k++)
            if (bps[i].type & kinds[k].bit) {
                [what appendFormat:@"%s%s", first ? "" : "/", kinds[k].name];
                first = NO;
            }
        [what appendFormat:@"  $%04X", bps[i].start];
        if (bps[i].end != bps[i].start) [what appendFormat:@"-$%04X", bps[i].end];
        char label[64];
        if (chfdebug_address_label(_dbg, bps[i].start, label, sizeof label) > 0)
            [what appendFormat:@"  %s", label];
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

- (void)refreshCart
{
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
        return;
    }
    [s appendFormat:@"Type        %s\n", c.type_name ? c.type_name : "?"];
    if (c.type == CART_TYPE_FUJINET) {
        [s appendFormat:@"Link        %s%s\n", c.link_up ? "up" : "down", c.link_off ? " (switched off)" : ""];
        [s appendFormat:@"Mailbox     %s   %s%s\n", c.mailbox ? "open (the app claims it)" : "closed",
            c.app_live ? "an app is live" : "CONFIG is live", c.booted_game ? "   a game has booted" : ""];
        [s appendFormat:@"Live image  %u bytes   CRC-32 %08X   %d bank%s, showing %d\n", c.live_size, c.live_crc,
            c.nbanks, c.nbanks == 1 ? "" : "s", c.fbank];
        if (c.staged_size)
            [s appendFormat:@"Staged      %s%s   %u bytes   CRC-32 %08X%s\n", c.staged_app ? "a FujiNet app" : "a game",
                c.staged_mailbox ? " (claims the mailbox)" : "", c.staged_size, c.staged_crc,
                c.armed ? "   armed" : ""];
        else
            [s appendString:@"Staged      nothing\n"];
        [s appendFormat:@"Swaps       %d\n", c.swaps];
        [s appendFormat:@"Boot        state %d   %d%%   error %d\n", c.boot_state, c.boot_pct, c.boot_err];
        [s appendFormat:@"Mailbox     ACKSEQ $%02X   STATUS $%02X   last error %d   reply cmd $%02X   RXLEN %d\n",
            c.ackseq, c.status, c.last_error, c.reply_cmd, c.rxlen];
        [s appendFormat:@"Queue       %u events waiting\n", c.queue_depth];
    } else {
        [s appendFormat:@"Image       %u bytes\n", c.rom_size];
    }
    ChfSetTextKeepingPlace(_cart, s);
    ChfSetTextKeepingPlace(_mailbox, hexDump(c.mailbox_page, (int)sizeof c.mailbox_page, 0x2B00, @"$%04X: "));
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
