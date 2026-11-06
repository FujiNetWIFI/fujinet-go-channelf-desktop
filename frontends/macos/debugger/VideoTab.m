/*
 * ChfVideoTab -- see the header.
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "VideoTab.h"

#import "DebugViews.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* chfdebug.h: size the view buffer by CHFDEBUG_VIEW_MAX_PIXELS * scale * scale. */
#define SCALE_MAX 4
#define PX_CAP ((size_t)CHFDEBUG_VIEW_MAX_PIXELS * SCALE_MAX * SCALE_MAX)

/* A VRAM cell in a view's pixels at scale 1: 2 wide, 4 tall. */
#define CELL_W 2
#define CELL_H 4

#define WRITES_MAX 400          /* the VRAM write log's lines */
#define STRIP_ROW_H 8.0
#define STRIP_W 150.0
#define TABLE_W 400.0
#define TABLE_ROW_H 58.0

#define SECTION_PALETTES 0
#define SECTION_PORTS    1
#define SECTION_WRITES   2

/* The overlay checkboxes, with the bits they set. */
static const struct { NSString *__unsafe_unretained title; unsigned bit; } kOverlays[8] = {
    { @"Visible", CHFDEBUG_F_VISIBLE },       { @"Grid", CHFDEBUG_F_GRID },
    { @"Palette columns", CHFDEBUG_F_PALCOLS }, { @"Beam", CHFDEBUG_F_BEAM },
    { @"Cursor", CHFDEBUG_F_CURSOR },         { @"Heat", CHFDEBUG_F_HEAT },
    { @"Dirty", CHFDEBUG_F_DIRTY },           { @"Palette rows", CHFDEBUG_F_PALROWS },
};

static NSColor *colorFromXrgb(uint32_t rgb)
{
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xff) / 255.0
                               green:((rgb >> 8) & 0xff) / 255.0
                                blue:(rgb & 0xff) / 255.0
                               alpha:1.0];
}

static NSString *nsstr(const char *s)
{
    NSString *r = s ? [NSString stringWithUTF8String:s] : nil;
    return r ? r : @"";
}

/* A whole decimal number, or `def` when the text is empty or not one. */
static int parseInt(NSString *text, int def)
{
    NSString *t = [text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    if (t.length == 0) return def;
    char *end;
    const long v = strtol(t.UTF8String, &end, 10);
    return *end ? def : (int)v;
}

/* ---- the 64-row palette strip ------------------------------------------------------- */

/* Every VRAM row: its number, the palette its columns 125 and 126 pick, and
 * that palette's background. The rows the TV shows (4-61) are bright; the
 * others are dimmed. */
@interface ChfRowStripView : NSView
- (void)setVideo:(const chfdebug_video *)v;
@end

@implementation ChfRowStripView {
    uint8_t _palette[CHFDEBUG_VRAM_ROWS];
    uint32_t _background[CHFDEBUG_VRAM_ROWS];
}
- (BOOL)isFlipped { return YES; }
- (NSSize)intrinsicContentSize
{
    return NSMakeSize(STRIP_W, STRIP_ROW_H * CHFDEBUG_VRAM_ROWS + 4);
}

- (void)setVideo:(const chfdebug_video *)v
{
    if (!memcmp(_palette, v->row_palette, sizeof _palette) &&
        !memcmp(_background, v->row_background, sizeof _background))
        return;
    memcpy(_palette, v->row_palette, sizeof _palette);
    memcpy(_background, v->row_background, sizeof _background);
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    NSDictionary *num = @{ NSFontAttributeName: [NSFont monospacedSystemFontOfSize:7 weight:NSFontWeightRegular],
                           NSForegroundColorAttributeName: NSColor.secondaryLabelColor };
    NSDictionary *pal = @{ NSFontAttributeName: [NSFont monospacedSystemFontOfSize:7 weight:NSFontWeightRegular],
                           NSForegroundColorAttributeName: NSColor.labelColor };
    for (int r = 0; r < CHFDEBUG_VRAM_ROWS; r++) {
        const CGFloat y = 2 + r * STRIP_ROW_H;
        const BOOL vis = r >= CHFDEBUG_VIS_ROW0 && r < CHFDEBUG_VIS_ROW0 + CHFDEBUG_VIS_ROWS;
        [[NSString stringWithFormat:@"%2d", r] drawAtPoint:NSMakePoint(0, y - 1) withAttributes:num];
        [colorFromXrgb(_background[r]) setFill];
        NSRectFill(NSMakeRect(18, y, 60, STRIP_ROW_H - 1));
        if (!vis) {
            [[NSColor colorWithWhite:0.5 alpha:0.55] setFill];
            NSRectFill(NSMakeRect(18, y, 60, STRIP_ROW_H - 1));
        }
        [[NSString stringWithFormat:@"p%d%@", _palette[r], vis ? @"" : @" hidden"]
            drawAtPoint:NSMakePoint(82, y - 1) withAttributes:pal];
    }
}
@end

/* ---- the 4 x 4 palette table -------------------------------------------------------------- */

/* Each palette: its name, how many rows use it, and its four values' pens
 * with their colour names and RGB. */
@interface ChfPaletteTableView : NSView
- (void)setVideo:(const chfdebug_video *)v;
@end

@implementation ChfPaletteTableView {
    int _rows[4];
}
- (BOOL)isFlipped { return YES; }
- (NSSize)intrinsicContentSize { return NSMakeSize(TABLE_W, TABLE_ROW_H * 4 + 4); }

- (void)setVideo:(const chfdebug_video *)v
{
    if (!memcmp(_rows, v->palette_rows, sizeof _rows)) return;
    memcpy(_rows, v->palette_rows, sizeof _rows);
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    NSDictionary *head = @{ NSFontAttributeName: [NSFont boldSystemFontOfSize:11],
                            NSForegroundColorAttributeName: NSColor.labelColor };
    NSDictionary *small = @{ NSFontAttributeName: [NSFont monospacedSystemFontOfSize:9 weight:NSFontWeightRegular],
                             NSForegroundColorAttributeName: NSColor.secondaryLabelColor };
    for (int p = 0; p < 4; p++) {
        const CGFloat y = 2 + p * TABLE_ROW_H;
        [[NSString stringWithFormat:@"Palette %d: %@   %d rows", p, nsstr(chfdebug_palette_name(p)), _rows[p]]
            drawAtPoint:NSMakePoint(0, y) withAttributes:head];
        for (int v = 0; v < 4; v++) {
            const int pen = chfdebug_palette_pen(p, v);
            const uint32_t rgb = chfdebug_colour_rgb(pen) & 0xffffff;
            const CGFloat x = v * (TABLE_W / 4);
            NSRect sw = NSMakeRect(x, y + 16, 28, 22);
            [colorFromXrgb(rgb) setFill];
            NSRectFill(sw);
            [NSColor.separatorColor setStroke];
            NSFrameRect(sw);
            [[NSString stringWithFormat:@"%d = %@", v, nsstr(chfdebug_colour_name(pen))]
                drawAtPoint:NSMakePoint(x + 32, y + 15) withAttributes:small];
            [[NSString stringWithFormat:@"#%06X", rgb] drawAtPoint:NSMakePoint(x + 32, y + 27) withAttributes:small];
        }
    }
}
@end

/* ---- the tab ----------------------------------------------------------------------------- */

@implementation ChfVideoTab {
    chfdebug *_dbg;
    uint32_t *_px;                      /* PX_CAP pixels: the view's scratch */

    NSPopUpButton *_viewSel, *_scaleSel;
    NSButton *_overlay[8];
    ChfPictureView *_pic;
    NSTextField *_hover, *_pinned;
    int _hx, _hy;                       /* the pixel under the pointer, or -1 */
    int _pinRow, _pinCol;               /* the pinned cell, or -1 */
    NSTabView *_sections;

    ChfRowStripView *_strip;
    ChfPaletteTableView *_table;
    NSTextView *_ports;
    NSTextField *_rowFrom, *_rowTo, *_colFrom, *_colTo;
    NSButton *_changesOnly, *_thisFrame, *_palCols;
    NSTextField *_counts;
    NSTextView *_writes;
}

- (instancetype)initWithDebugger:(chfdebug *)dbg
{
    self = [super init];
    if (!self) return nil;
    _dbg = dbg;
    _px = calloc(PX_CAP, sizeof *_px);
    _hx = _hy = -1;
    _pinRow = _pinCol = -1;
    _view = [self buildView];
    return self;
}

- (void)dealloc
{
    free(_px);
}

/* ---- construction ------------------------------------------------------------------------ */

- (NSView *)buildLeft
{
    _viewSel = [[NSPopUpButton alloc] init];
    [_viewSel addItemsWithTitles:@[@"Frame (what the display shows)", @"Live (the VRAM now)",
                                   @"Beam (what the CRT shows)", @"VRAM (all 128 x 64 cells)",
                                   @"Values (raw, false colour)", @"Palettes"]];
    _viewSel.target = self;
    _viewSel.action = @selector(optionChanged:);
    _scaleSel = [[NSPopUpButton alloc] init];
    [_scaleSel addItemsWithTitles:@[@"1x", @"2x", @"3x", @"4x"]];
    [_scaleSel selectItemAtIndex:1];
    _scaleSel.target = self;
    _scaleSel.action = @selector(optionChanged:);
    NSStackView *viewRow = ChfHStack(@[ChfLabel(@"View"), _viewSel, ChfLabel(@"Scale"), _scaleSel]);

    NSMutableArray<NSView *> *boxes = [NSMutableArray array];
    for (int i = 0; i < 8; i++) {
        _overlay[i] = [NSButton checkboxWithTitle:kOverlays[i].title target:self action:@selector(optionChanged:)];
        _overlay[i].controlSize = NSControlSizeSmall;
        _overlay[i].state = kOverlays[i].bit == CHFDEBUG_F_VISIBLE ? NSControlStateValueOn : NSControlStateValueOff;
        [boxes addObject:_overlay[i]];
    }
    NSStackView *ovRow1 = ChfHStack([boxes subarrayWithRange:NSMakeRange(0, 4)]);
    NSStackView *ovRow2 = ChfHStack([boxes subarrayWithRange:NSMakeRange(4, 4)]);
    ovRow1.spacing = 10;
    ovRow2.spacing = 10;

    _pic = [[ChfPictureView alloc] initWithFrame:NSMakeRect(0, 0, 512, 512)];
    [_pic.widthAnchor constraintGreaterThanOrEqualToConstant:300].active = YES;
    [_pic.heightAnchor constraintGreaterThanOrEqualToConstant:300].active = YES;
    [_pic setContentHuggingPriority:(NSLayoutPriority)1 forOrientation:NSLayoutConstraintOrientationVertical];
    __weak ChfVideoTab *weakSelf = self;
    _pic.onHover = ^(int x, int y) { [weakSelf hoverX:x y:y]; };
    _pic.onClick = ^(int x, int y, int clicks) { [weakSelf clickX:x y:y clicks:clicks]; };

    _hover = ChfNote(@"Point at the picture to read a cell; click pins it, double-click breaks on writes to it");
    _hover.lineBreakMode = NSLineBreakByTruncatingTail;
    _hover.maximumNumberOfLines = 3;
    _pinned = ChfLabel(@"");
    _pinned.maximumNumberOfLines = 3;
    _pinned.lineBreakMode = NSLineBreakByTruncatingTail;
    [_pinned setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                      forOrientation:NSLayoutConstraintOrientationHorizontal];

    NSStackView *left = ChfVStack(@[viewRow, ovRow1, ovRow2, _pic, _hover, _pinned]);
    left.distribution = NSStackViewDistributionFill;
    ChfFill(left, _pic);
    ChfFill(left, _hover);
    ChfFill(left, _pinned);
    return left;
}

- (NSView *)buildPalettes
{
    _strip = [[ChfRowStripView alloc] initWithFrame:NSMakeRect(0, 0, STRIP_W, 520)];
    ChfFixSize(_strip);
    _table = [[ChfPaletteTableView alloc] initWithFrame:NSMakeRect(0, 0, TABLE_W, 240)];
    ChfFixSize(_table);
    NSTextField *note = ChfNote(@"Each row picks its palette from the values in its columns 125 and 126: "
                                @"palette = (v126 & 2) | (v125 >> 1).");
    note.lineBreakMode = NSLineBreakByWordWrapping;
    note.maximumNumberOfLines = 3;
    note.preferredMaxLayoutWidth = TABLE_W;
    NSStackView *right = ChfVStack(@[ChfTitle(@"The four palettes"), _table, note]);
    NSStackView *left = ChfVStack(@[ChfTitle(@"The 64 rows"), _strip]);
    NSStackView *h = ChfHStack(@[left, right]);
    h.alignment = NSLayoutAttributeTop;
    h.spacing = 14;
    return h;
}

- (NSView *)buildPorts
{
    NSScrollView *scroll;
    _ports = ChfMonoView(&scroll, NO);
    return scroll;
}

- (NSView *)buildWrites
{
    _rowFrom = ChfField(@"0", 36, self, @selector(optionChanged:));
    _rowTo = ChfField(@"63", 36, self, @selector(optionChanged:));
    _colFrom = ChfField(@"0", 36, self, @selector(optionChanged:));
    _colTo = ChfField(@"127", 40, self, @selector(optionChanged:));
    _changesOnly = [NSButton checkboxWithTitle:@"Changes only" target:self action:@selector(optionChanged:)];
    _thisFrame = [NSButton checkboxWithTitle:@"This frame" target:self action:@selector(optionChanged:)];
    _palCols = [NSButton checkboxWithTitle:@"Palette columns" target:self action:@selector(optionChanged:)];
    for (NSButton *b in @[_changesOnly, _thisFrame, _palCols]) b.controlSize = NSControlSizeSmall;
    NSStackView *range = ChfHStack(@[ChfLabel(@"Rows"), _rowFrom, ChfLabel(@"to"), _rowTo,
                                     ChfLabel(@"columns"), _colFrom, ChfLabel(@"to"), _colTo]);
    NSStackView *filters = ChfHStack(@[_changesOnly, _thisFrame, _palCols,
                                       ChfButton(@"Clear log", self, @selector(clearWrites:))]);
    filters.spacing = 10;
    _counts = ChfNote(@"");
    _counts.lineBreakMode = NSLineBreakByTruncatingTail;
    NSScrollView *scroll;
    _writes = ChfMonoView(&scroll, NO);
    NSStackView *v = ChfVStack(@[range, filters, _counts, scroll]);
    v.distribution = NSStackViewDistributionFill;
    ChfFill(v, scroll);
    ChfFill(v, _counts);
    return v;
}

- (NSView *)buildView
{
    NSView *left = [self buildLeft];

    _sections = [[NSTabView alloc] init];
    NSArray *pages = @[ @[@"Palettes", [self buildPalettes]], @[@"Ports", [self buildPorts]],
                        @[@"VRAM writes", [self buildWrites]] ];
    for (NSArray *page in pages) {
        NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:page[0]];
        item.label = page[0];
        NSView *content = page[1];
        NSView *holder = [[NSView alloc] init];
        [holder addSubview:content];
        content.translatesAutoresizingMaskIntoConstraints = NO;
        [content.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:4].active = YES;
        [content.trailingAnchor constraintEqualToAnchor:holder.trailingAnchor constant:-4].active = YES;
        [content.topAnchor constraintEqualToAnchor:holder.topAnchor constant:4].active = YES;
        [content.bottomAnchor constraintEqualToAnchor:holder.bottomAnchor constant:-4].active = YES;
        item.view = holder;
        [_sections addTabViewItem:item];
    }
    _sections.delegate = self;

    NSStackView *h = ChfHStack(@[left, _sections]);
    h.alignment = NSLayoutAttributeTop;
    h.spacing = 12;
    [left.widthAnchor constraintGreaterThanOrEqualToConstant:320].active = YES;
    [_sections.widthAnchor constraintEqualToConstant:600].active = YES;
    [left.heightAnchor constraintEqualToAnchor:h.heightAnchor].active = YES;
    [_sections.heightAnchor constraintEqualToAnchor:h.heightAnchor].active = YES;
    return h;
}

/* ---- the picture ------------------------------------------------------------------------- */

- (int)viewKind { return (int)_viewSel.indexOfSelectedItem; }
- (int)scale { return (int)_scaleSel.indexOfSelectedItem + 1; }

- (unsigned)flags
{
    unsigned f = 0;
    for (int i = 0; i < 8; i++)
        if (_overlay[i].state == NSControlStateValueOn) f |= kOverlays[i].bit;
    return f;
}

- (void)optionChanged:(id)sender
{
    (void)sender;
    [self refresh];
}

- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    (void)item;
    [self refresh];
}

/* The picture-pixel rectangle of a cell in the shown view, or empty when the
 * view does not show it. */
- (NSRect)rectOfRow:(int)row col:(int)col
{
    const int view = [self viewKind], s = [self scale];
    int x, y;
    if (view == CHFDEBUG_VIEW_VRAM || view == CHFDEBUG_VIEW_VALUES) {
        x = col;
        y = row;
    } else if (view == CHFDEBUG_VIEW_FRAME || view == CHFDEBUG_VIEW_LIVE || view == CHFDEBUG_VIEW_BEAM) {
        if (col < CHFDEBUG_VIS_COL0 || col >= CHFDEBUG_VIS_COL0 + CHFDEBUG_VIS_COLS ||
            row < CHFDEBUG_VIS_ROW0 || row >= CHFDEBUG_VIS_ROW0 + CHFDEBUG_VIS_ROWS)
            return NSZeroRect;
        x = col - CHFDEBUG_VIS_COL0;
        y = row - CHFDEBUG_VIS_ROW0;
    } else {
        return NSZeroRect;
    }
    return NSMakeRect(x * CELL_W * s, y * CELL_H * s, CELL_W * s, CELL_H * s);
}

/* The cell under a picture pixel, or NO. */
- (BOOL)cellAtX:(int)x y:(int)y row:(int *)row col:(int *)col
{
    if (x < 0 || y < 0) return NO;
    return chfdebug_video_hit(_dbg, [self viewKind], x, y, [self scale], row, col) >= 0;
}

/* Three lines about a VRAM cell: where it is and whether the TV shows it, its
 * value, palette and colour, and who wrote it last. */
static NSString *cellText(chfdebug *dbg, int row, int col)
{
    chfdebug_vram_cell c;
    if (chfdebug_vram_cell_get(dbg, row, col, &c) < 0) return [NSString stringWithFormat:@"row %d col %d", row, col];
    NSMutableString *s = [NSMutableString stringWithFormat:@"Row %d, column %d: ", c.row, c.col];
    if (c.visible) [s appendString:@"visible on the TV"];
    else [s appendFormat:@"hidden (%@)", c.hidden_why ? nsstr(c.hidden_why) : @"outside the picture"];
    [s appendFormat:@"\nvalue %d   palette %d (%@)   pen %d, %@  #%06X", c.value, c.palette,
        nsstr(chfdebug_palette_name(c.palette)), c.pen, nsstr(chfdebug_colour_name(c.pen)), c.rgb & 0xffffff];
    if (c.last_pc >= 0)
        [s appendFormat:@"\n%u writes; the last by PC $%04X in frame %u, line %d, cycle %llu (it was %d)",
            c.writes, c.last_pc & 0xffff, c.last_frame, c.last_line, (unsigned long long)c.last_cycle, c.last_old];
    else
        [s appendFormat:@"\n%u writes; never written since the debugger attached", c.writes];
    return s;
}

- (void)hoverX:(int)x y:(int)y
{
    _hx = x;
    _hy = y;
    [self updateCells];
}

- (void)clickX:(int)x y:(int)y clicks:(int)clicks
{
    int row, col;
    if (![self cellAtX:x y:y row:&row col:&col]) {
        _pinRow = _pinCol = -1;
    } else {
        _pinRow = row;
        _pinCol = col;
        if (clicks >= 2) {
            /* break on writes to the cell */
            const uint16_t cell = (uint16_t)(row * CHFDEBUG_VRAM_COLS + col);
            const int id = chfdebug_breakpoint_add(_dbg, CHFDEBUG_BP_VRAM, CHF_SPACE_VRAM, cell, cell, "");
            _pinned.stringValue = id >= 0
                ? [NSString stringWithFormat:@"Breakpoint #%d: stop on writes to row %d, column %d.", id, row, col]
                : @"Could not set the breakpoint.";
            return;
        }
    }
    [self updateCells];
}

/* The hover and pinned readouts, and the outline round the cell. */
- (void)updateCells
{
    int row, col;
    NSRect box = NSZeroRect;
    if ([self cellAtX:_hx y:_hy row:&row col:&col]) {
        _hover.stringValue = cellText(_dbg, row, col);
        box = [self rectOfRow:row col:col];
    } else {
        _hover.stringValue = @"Point at the picture to read a cell; click pins it, double-click breaks on writes to it";
    }
    if (_pinRow >= 0) {
        _pinned.stringValue = [@"Pinned: " stringByAppendingString:cellText(_dbg, _pinRow, _pinCol)];
        if (NSIsEmptyRect(box)) box = [self rectOfRow:_pinRow col:_pinCol];
    } else {
        _pinned.stringValue = @"";
    }
    _pic.highlight = box;
}

- (void)drawPicture
{
    int w = 0, h = 0;
    if (!chfdebug_video_view(_dbg, [self viewKind], [self flags], [self scale], _px, PX_CAP, &w, &h)) {
        [_pic setImage:NULL];
        return;
    }
    [_pic setImage:ChfNewImage(_px, w, h)];
}

/* ---- the sections ------------------------------------------------------------------------- */

static NSString *hexBits(unsigned v)
{
    return [NSString stringWithFormat:@"$%02X %%%c%c%c%c%c%c%c%c", v & 0xff,
            (v & 0x80) ? '1' : '0', (v & 0x40) ? '1' : '0', (v & 0x20) ? '1' : '0', (v & 0x10) ? '1' : '0',
            (v & 0x08) ? '1' : '0', (v & 0x04) ? '1' : '0', (v & 0x02) ? '1' : '0', (v & 0x01) ? '1' : '0'];
}

- (void)refreshPorts:(const chfdebug_video *)v
{
    static const char *const kModes[4] = { "off", "1 kHz", "500 Hz", "120 Hz" };
    NSMutableString *s = [NSMutableString string];
    [s appendString:@"The CPU's output latches (raw), their complements, and what they mean\n\n"];
    [s appendFormat:@"port 0  %@   complement %@\n", hexBits(v->port0), hexBits((unsigned)~v->port0)];
    [s appendFormat:@"        write strobe %s   hand controllers %s   panel latch $%X\n",
        v->strobe ? "UP" : "down", v->controllers_enabled ? "enabled" : "off", v->panel_latch & 0xf];
    [s appendFormat:@"port 1  %@   complement %@\n", hexBits(v->port1), hexBits((unsigned)~v->port1)];
    [s appendFormat:@"        the next strobe writes colour value %d\n", v->value];
    [s appendFormat:@"port 4  %@   complement %@\n", hexBits(v->port4), hexBits((unsigned)~v->port4)];
    [s appendFormat:@"        column %d\n", v->col];
    [s appendFormat:@"port 5  %@   complement %@\n", hexBits(v->port5), hexBits((unsigned)~v->port5)];
    [s appendFormat:@"        row %d   beeper %s (mode %d)\n\n", v->row,
        kModes[v->sound_mode >= 0 && v->sound_mode < 4 ? v->sound_mode : 0], v->sound_mode];
    [s appendString:@"What the CPU would read now (the pins, then the latch)\n\n"];
    [s appendFormat:@"port 0  %@\nport 1  %@\nport 4  %@\nport 5  %@\n\n", hexBits(v->in0), hexBits(v->in1),
        hexBits(v->in4), hexBits(v->in5)];
    [s appendFormat:@"%s: line %d of %d, cycle %d of %d, frame %u\n", v->is_pal ? "PAL" : "NTSC", v->line,
        v->lines, v->cycle_in_line, v->cycles_per_line, v->frame];
    [s appendFormat:@"VRAM writes: %u this frame so far; %u in the last whole one, %u of them changed a cell\n",
        v->strobes_frame, v->strobes_last, v->changes_last];
    ChfSetTextKeepingPlace(_ports, s);
}

- (void)refreshWrites:(const chfdebug_video *)v
{
    static chfdebug_vram_write w[WRITES_MAX];
    chfdebug_vram_filter f;
    f.row0 = parseInt(_rowFrom.stringValue, 0);
    f.row1 = parseInt(_rowTo.stringValue, CHFDEBUG_VRAM_ROWS - 1);
    f.col0 = parseInt(_colFrom.stringValue, 0);
    f.col1 = parseInt(_colTo.stringValue, CHFDEBUG_VRAM_COLS - 1);
    f.changes_only = _changesOnly.state == NSControlStateValueOn;
    f.this_frame = _thisFrame.state == NSControlStateValueOn;
    f.palette_cols = _palCols.state == NSControlStateValueOn;
    const int n = chfdebug_vram_writes(_dbg, &f, w, WRITES_MAX);

    _counts.stringValue = [NSString stringWithFormat:
        @"Writes per frame: %u so far this frame, %u in the last (%u changed a cell). Showing the newest %d.",
        v->strobes_frame, v->strobes_last, v->changes_last, n];
    const BOOL follow = ChfTextAtEnd(_writes);
    NSMutableString *s = [NSMutableString stringWithString:@"frame  line      cycle  PC     cell (row,col)  old -> new\n"];
    for (int i = 0; i < n; i++)
        [s appendFormat:@"%5u %5d %10llu  $%04X  (%2d,%3d)        %d -> %d\n", w[i].frame, w[i].line,
            (unsigned long long)w[i].cycle, w[i].pc, w[i].row, w[i].col, w[i].old, w[i].value];
    if (n == 0) [s appendString:@"(no VRAM writes match; the log fills while the debugger is attached)\n"];
    ChfSetLogText(_writes, s, follow);
}

- (void)clearWrites:(id)sender
{
    (void)sender;
    chfdebug_vram_writes_clear(_dbg);
    [self refresh];
}

- (void)refresh
{
    chfdebug_video v;
    memset(&v, 0, sizeof v);
    chfdebug_video_get(_dbg, &v);
    [self drawPicture];
    [self updateCells];
    switch ([_sections indexOfTabViewItem:_sections.selectedTabViewItem]) {
    case SECTION_PALETTES:
        [_strip setVideo:&v];
        [_table setVideo:&v];
        break;
    case SECTION_PORTS:
        [self refreshPorts:&v];
        break;
    case SECTION_WRITES:
        [self refreshWrites:&v];
        break;
    default:
        break;
    }
}
@end
