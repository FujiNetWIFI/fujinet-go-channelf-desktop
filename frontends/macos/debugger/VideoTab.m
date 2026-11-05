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
#import "../KeyForward.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The views want at most one frame (192 x 262) at 1x, and the sheets up to 4x
 * of their own size: chfdebug.h says to size by MAX_PIXELS * scale * scale. */
#define PX_CAP ((size_t)CHFDEBUG_UVI_VIEW_MAX_PIXELS * 16)
#define OBJ_CAP (64 * 64)

#define OBJ_ROW_H     46.0
#define OBJ_HEAD_H    24.0
#define OBJ_THUMB     40.0

#define SECTION_OBJECTS  0
#define SECTION_SCREEN   1
#define SECTION_REGS     2
#define SECTION_CONTROL  3
#define SECTION_SHEETS   4

/* The layer and overlay checkboxes, with the bits they set. */
static const struct { NSString *__unsafe_unretained title; unsigned bit; } kLayers[2] = {
    { @"Characters", CHFDEBUG_UVI_L_CHARS }, { @"Objects", CHFDEBUG_UVI_L_OBJECTS },
};
static const struct { NSString *__unsafe_unretained title; unsigned bit; } kOverlays[4] = {
    { @"Object boxes + indices", CHFDEBUG_UVI_F_BOXES }, { @"Character grid", CHFDEBUG_UVI_F_GRID },
    { @"Collisions", CHFDEBUG_UVI_F_COLLIDE }, { @"Beam cross-hair", CHFDEBUG_UVI_F_BEAM },
};

static NSColor *colorFromXrgb(uint32_t rgb)
{
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xff) / 255.0
                               green:((rgb >> 8) & 0xff) / 255.0
                                blue:(rgb & 0xff) / 255.0
                               alpha:1.0];
}

/* $hex, 0xhex or bare hex. */
static BOOL parseHex(NSString *text, long *out)
{
    NSString *t = [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if ([t hasPrefix:@"$"]) t = [t substringFromIndex:1];
    else if ([t.lowercaseString hasPrefix:@"0x"]) t = [t substringFromIndex:2];
    if (t.length == 0) return NO;
    char *end;
    const long v = strtol(t.UTF8String, &end, 16);
    if (*end) return NO;
    *out = v;
    return YES;
}

/* ---- the objects list ------------------------------------------------------------- */

/* One line of the objects list: a heading, or an object with its bitmap. */
@interface ChfObjectRow : NSObject
@property (nonatomic, copy) NSString *heading;     /* set: a section heading */
@property (nonatomic, copy) NSString *line1;
@property (nonatomic, copy) NSString *line2;
@property (nonatomic, strong) NSImage *thumb;
@end

@implementation ChfObjectRow
@end

/* The rows, drawn from the top; the view's height follows its rows, so the
 * scroll view around it scrolls them. */
@interface ChfObjectsView : NSView
@property (nonatomic, copy) NSArray<ChfObjectRow *> *rows;
@end

@implementation ChfObjectsView
- (BOOL)isFlipped { return YES; }

- (void)setRows:(NSArray<ChfObjectRow *> *)rows
{
    _rows = [rows copy];
    CGFloat h = 4;
    for (ChfObjectRow *r in _rows) h += r.heading ? OBJ_HEAD_H : OBJ_ROW_H;
    [self setFrameSize:NSMakeSize(self.frame.size.width, h)];
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    NSDictionary *head = @{ NSFontAttributeName: [NSFont boldSystemFontOfSize:12],
                            NSForegroundColorAttributeName: NSColor.labelColor };
    NSDictionary *one = @{ NSFontAttributeName: [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular],
                           NSForegroundColorAttributeName: NSColor.labelColor };
    NSDictionary *two = @{ NSFontAttributeName: [NSFont monospacedSystemFontOfSize:10 weight:NSFontWeightRegular],
                           NSForegroundColorAttributeName: NSColor.secondaryLabelColor };
    CGFloat y = 4;
    for (ChfObjectRow *r in _rows) {
        if (r.heading) {
            [r.heading drawAtPoint:NSMakePoint(6, y + 4) withAttributes:head];
            y += OBJ_HEAD_H;
            continue;
        }
        const NSRect box = NSMakeRect(6, y + 3, OBJ_THUMB, OBJ_THUMB);
        [colorFromXrgb(0x101010) setFill];
        NSRectFill(box);
        if (r.thumb) {
            [[NSGraphicsContext currentContext] setImageInterpolation:NSImageInterpolationNone];
            const NSSize sz = r.thumb.size;
            const double f = fmin(1.0, fmin(OBJ_THUMB / sz.width, OBJ_THUMB / sz.height));
            const NSRect dst = NSMakeRect(box.origin.x + floor((OBJ_THUMB - sz.width * f) / 2),
                                          box.origin.y + floor((OBJ_THUMB - sz.height * f) / 2),
                                          sz.width * f, sz.height * f);
            [r.thumb drawInRect:dst fromRect:NSZeroRect operation:NSCompositingOperationSourceOver
                       fraction:1.0 respectFlipped:YES hints:nil];
        }
        [r.line1 drawAtPoint:NSMakePoint(OBJ_THUMB + 12, y + 6) withAttributes:one];
        [r.line2 drawAtPoint:NSMakePoint(OBJ_THUMB + 12, y + 24) withAttributes:two];
        y += OBJ_ROW_H;
    }
}
@end

/* ---- the tab ----------------------------------------------------------------------------- */

@implementation ChfVideoTab {
    chfdebug *_dbg;
    chfdebug_uvi _uvi;
    uint32_t *_px;                      /* PX_CAP pixels: every view's scratch */
    uint32_t *_objPx;                   /* OBJ_CAP pixels: one object's bitmap */
    uint32_t _pal[8];                   /* the UVI's 8 colours */
    BOOL _palLoaded;
    CFAbsoluteTime _regsAt;

    NSPopUpButton *_viewSel;
    NSButton *_layer[2];
    NSButton *_overlay[4];
    ChfPictureView *_pic;
    NSTextField *_hover, *_beam;
    NSTabView *_sections;

    ChfObjectsView *_objects;
    NSTextView *_screen;
    NSTextField *_screenNote;
    ChfLineTextView *_regs;
    NSTextField *_regAddr, *_regVal, *_regNote;
    NSTextView *_control;
    ChfPictureView *_fontPic, *_objPic, *_palPic;
}

- (instancetype)initWithDebugger:(chfdebug *)dbg
{
    self = [super init];
    if (!self) return nil;
    _dbg = dbg;
    _px = calloc(PX_CAP, sizeof *_px);
    _objPx = calloc(OBJ_CAP, sizeof *_objPx);
    _view = [self buildView];
    return self;
}

- (void)dealloc
{
    free(_px);
    free(_objPx);
}

/* ---- construction ------------------------------------------------------------------------ */

- (NSView *)buildLeft
{
    _viewSel = [[NSPopUpButton alloc] init];
    [_viewSel addItemsWithTitles:@[@"Frame (what the display shows)",
                                   @"Rendered from the registers",
                                   @"Frame in progress, to the beam"]];
    _viewSel.target = self;
    _viewSel.action = @selector(viewChanged:);
    NSStackView *viewRow = ChfHStack(@[ChfLabel(@"View"), _viewSel]);

    NSMutableArray<NSView *> *layerBoxes = [NSMutableArray array];
    for (int i = 0; i < 2; i++) {
        _layer[i] = [NSButton checkboxWithTitle:kLayers[i].title target:self action:@selector(optionChanged:)];
        _layer[i].state = NSControlStateValueOn;
        _layer[i].controlSize = NSControlSizeSmall;
        [layerBoxes addObject:_layer[i]];
    }
    NSMutableArray<NSView *> *overlayBoxes = [NSMutableArray array];
    for (int i = 0; i < 4; i++) {
        _overlay[i] = [NSButton checkboxWithTitle:kOverlays[i].title target:self action:@selector(optionChanged:)];
        _overlay[i].controlSize = NSControlSizeSmall;
        [overlayBoxes addObject:_overlay[i]];
    }
    NSStackView *layerRow = ChfHStack(layerBoxes);
    NSStackView *ovRow1 = ChfHStack([overlayBoxes subarrayWithRange:NSMakeRange(0, 2)]);
    NSStackView *ovRow2 = ChfHStack([overlayBoxes subarrayWithRange:NSMakeRange(2, 2)]);
    layerRow.spacing = 10;
    ovRow1.spacing = 10;
    ovRow2.spacing = 10;
    NSTextField *layersTitle = ChfNote(@"Layers (Rendered view)");
    NSTextField *overlaysTitle = ChfNote(@"Overlays");

    _pic = [[ChfPictureView alloc] initWithFrame:NSMakeRect(0, 0, 384, 524)];
    [_pic.widthAnchor constraintGreaterThanOrEqualToConstant:192].active = YES;
    [_pic.heightAnchor constraintGreaterThanOrEqualToConstant:262].active = YES;
    [_pic setContentHuggingPriority:(NSLayoutPriority)1 forOrientation:NSLayoutConstraintOrientationVertical];
    __weak ChfVideoTab *weakSelf = self;
    _pic.onHover = ^(int x, int y) { [weakSelf hoverX:x y:y]; };

    _hover = ChfNote(@"Point at the picture: what is drawn under the pointer is named here and outlined");
    _hover.lineBreakMode = NSLineBreakByTruncatingTail;
    _beam = ChfNote(@"");
    _beam.lineBreakMode = NSLineBreakByTruncatingTail;

    NSStackView *left = ChfVStack(@[viewRow, layersTitle, layerRow, overlaysTitle, ovRow1, ovRow2,
                                   _pic, _hover, _beam]);
    left.distribution = NSStackViewDistributionFill;
    ChfFill(left, _pic);
    ChfFill(left, _hover);
    ChfFill(left, _beam);
    return left;
}

- (NSView *)scrollFor:(NSView *)doc
{
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.drawsBackground = NO;
    scroll.documentView = doc;
    return scroll;
}

- (NSView *)buildObjects
{
    _objects = [[ChfObjectsView alloc] initWithFrame:NSMakeRect(0, 0, 480, 100)];
    _objects.autoresizingMask = NSViewWidthSizable;
    return [self scrollFor:_objects];
}

/* The 16 x 13 (or 26) character screen: each cell's code, drawn in the
 * colours the chip draws it in (the character's own colour bits in
 * multicolor mode). */
- (NSView *)buildScreen
{
    NSScrollView *scroll;
    _screen = ChfMonoView(&scroll, NO);
    _screenNote = ChfNote(@"");
    _screenNote.maximumNumberOfLines = 4;
    NSStackView *v = ChfVStack(@[ChfTitle(@"The character screen ($1800-$18EF, 16 columns)"), _screenNote, scroll]);
    v.distribution = NSStackViewDistributionFill;
    ChfFill(v, scroll);
    return v;
}

- (NSView *)buildRegisters
{
    _regs = [[ChfLineTextView alloc] initWithFrame:NSMakeRect(0, 0, 480, 300)];
    _regs.editable = NO;
    _regs.richText = NO;
    _regs.selectable = NO;
    _regs.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    _regs.horizontallyResizable = YES;
    _regs.textContainer.widthTracksTextView = NO;
    _regs.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
    _regs.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = YES;
    scroll.documentView = _regs;
    __weak ChfVideoTab *weakSelf = self;
    _regs.onClickLine = ^(int line) { [weakSelf pickRegister:line]; };

    _regAddr = ChfField(@"$18F0", 70, self, @selector(writeRegister:));
    _regVal = ChfField(@"$00", 60, self, @selector(writeRegister:));
    _regNote = ChfNote(@"Click a line, edit the value, Return writes it (stop the machine first: a running program may write it again)");
    _regNote.lineBreakMode = NSLineBreakByTruncatingTail;
    NSStackView *edit = ChfHStack(@[ChfLabel(@"Address"), _regAddr, ChfLabel(@"value"), _regVal,
                                   ChfButton(@"Write", self, @selector(writeRegister:))]);
    NSStackView *v = ChfVStack(@[scroll, edit, _regNote]);
    v.distribution = NSStackViewDistributionFill;
    ChfFill(v, scroll);
    return v;
}

- (NSView *)buildControl
{
    NSScrollView *scroll;
    _control = ChfMonoView(&scroll, NO);
    return scroll;
}

- (ChfPictureView *)sheetPicture:(CGFloat)height
{
    ChfPictureView *p = [[ChfPictureView alloc] initWithFrame:NSMakeRect(0, 0, 480, height)];
    [p.heightAnchor constraintEqualToConstant:height].active = YES;
    return p;
}

- (NSView *)buildSheets
{
    _fontPic = [self sheetPicture:170];
    _objPic = [self sheetPicture:110];
    _palPic = [self sheetPicture:60];
    NSStackView *v = ChfVStack(@[ChfTitle(@"Glyphs (64: $00-$37 built in, $38-$3F the UDCs at $1980-$19BF)"), _fontPic,
                                ChfTitle(@"The four objects' images"), _objPic,
                                ChfTitle(@"Palette (white, yellow, cyan, green, magenta, red, blue, black)"), _palPic]);
    ChfFill(v, _fontPic);
    ChfFill(v, _objPic);
    ChfFill(v, _palPic);
    return v;
}

- (NSView *)buildView
{
    NSView *left = [self buildLeft];

    _sections = [[NSTabView alloc] init];
    NSArray *pages = @[ @[@"Objects", [self buildObjects]], @[@"Screen", [self buildScreen]],
                        @[@"Registers", [self buildRegisters]], @[@"Control/Collisions", [self buildControl]],
                        @[@"Sheets", [self buildSheets]] ];
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
    [left.widthAnchor constraintGreaterThanOrEqualToConstant:300].active = YES;
    [_sections.widthAnchor constraintEqualToConstant:560].active = YES;
    [left.heightAnchor constraintEqualToAnchor:h.heightAnchor].active = YES;
    [_sections.heightAnchor constraintEqualToAnchor:h.heightAnchor].active = YES;
    return h;
}

/* ---- the controls --------------------------------------------------------------------------- */

- (unsigned)layers
{
    unsigned m = 0;
    for (int i = 0; i < 2; i++)
        if (_layer[i].state == NSControlStateValueOn) m |= kLayers[i].bit;
    return m;
}

- (unsigned)overlays
{
    unsigned m = 0;
    for (int i = 0; i < 4; i++)
        if (_overlay[i].state == NSControlStateValueOn) m |= kOverlays[i].bit;
    return m;
}

- (void)viewChanged:(id)sender
{
    (void)sender;
    [self refresh];
}

/* A layer or an overlay only means something in a re-render: the real frame
 * has neither, so touching a layer switches to it. */
- (void)optionChanged:(id)sender
{
    (void)sender;
    if (_viewSel.indexOfSelectedItem == CHFDEBUG_UVI_FRAME) [_viewSel selectItemAtIndex:CHFDEBUG_UVI_RENDER];
    [self refresh];
}

- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    (void)item;
    _regsAt = 0;
    [self refresh];
}

/* ---- the pointer ------------------------------------------------------------------------------ */

/* What the chip draws for one hit, and its box on the 192 x 262 picture. */
- (NSString *)describeHit:(chfdebug_hit)hit box:(NSRect *)box
{
    if (hit.kind == CHFDEBUG_UVI_HIT_OBJECT && hit.index >= 0 && hit.index < 4) {
        const chfdebug_uvi_object *o = &_uvi.obj[hit.index];
        *box = NSMakeRect(o->px, o->py, o->width, o->height);
        return [NSString stringWithFormat:@"Object %d: x=%d y=%d colour=%d (%s) UDC $%02X%s", hit.index, o->x, o->y,
                o->colour, chfdebug_uvi_colour_name(o->colour), o->udc, o->double_height ? " double height" : ""];
    }
    if (hit.kind == CHFDEBUG_UVI_HIT_CELL && hit.index >= 0) {
        const int row = hit.index / CHFDEBUG_UVI_COLS, col = hit.index % CHFDEBUG_UVI_COLS;
        chfdebug_uvi_cell cells[CHFDEBUG_UVI_COLS * CHFDEBUG_UVI_MAX_ROWS];
        const int rows = chfdebug_uvi_cells(_dbg, cells);
        *box = NSMakeRect(32 + col * 8, _uvi.first_line + row * _uvi.cell_height, 8, _uvi.cell_height);
        if (row < rows) {
            const chfdebug_uvi_cell *c = &cells[hit.index];
            return [NSString stringWithFormat:@"Cell row %d col %d: code $%02X glyph $%02X%s fg %s bg %s", row, col,
                    c->code, c->glyph, c->graphics ? " (block graphics)" : "",
                    chfdebug_uvi_colour_name(c->fg), chfdebug_uvi_colour_name(c->bg)];
        }
    }
    return nil;
}

- (void)hoverX:(int)x y:(int)y
{
    if (x < 0 || y < 0) {
        _hover.stringValue = @"Point at the picture: what is drawn under the pointer is named here and outlined";
        _pic.highlight = NSZeroRect;
        return;
    }
    chfdebug_hit hits[8];
    chfdebug_uvi_get(_dbg, &_uvi);
    const int n = chfdebug_uvi_hit_test(_dbg, x, y, hits, 8);
    NSRect box = NSZeroRect;
    NSString *what = n > 0 ? [self describeHit:hits[0] box:&box] : nil;
    if (!what) {
        _hover.stringValue = [NSString stringWithFormat:@"(%d, %d): nothing drawn here", x, y];
        _pic.highlight = NSZeroRect;
        return;
    }
    _hover.stringValue = [NSString stringWithFormat:@"(%d, %d)  %@%@", x, y, what,
                          n > 1 ? [NSString stringWithFormat:@"   (+%d more here)", n - 1] : @""];
    _pic.highlight = box;
}

/* ---- refreshing -------------------------------------------------------------------------------- */

- (NSInteger)section
{
    return [_sections indexOfTabViewItem:_sections.selectedTabViewItem];
}

- (void)refreshPicture
{
    int w = 0, h = 0;
    if (chfdebug_uvi_view(_dbg, (int)_viewSel.indexOfSelectedItem, [self layers], [self overlays], 1,
                          _px, PX_CAP, &w, &h) && w > 0 && h > 0)
        [_pic setImage:ChfNewImage(_px, w, h)];
}

- (void)refreshBeam
{
    _beam.stringValue = [NSString stringWithFormat:@"%s   beam line %d of %d (character row %d)   %s",
                         _uvi.lines == 312 ? "PAL" : "NTSC", _uvi.line, _uvi.lines, _uvi.charline,
                         _uvi.vblank ? "vertical blank (SENSE)" : "drawing"];
}

/* The 8 UVI colours, read off the palette strip. */
- (void)loadPalette
{
    int w = 0, h = 0;
    if (!chfdebug_uvi_view(_dbg, CHFDEBUG_UVI_PALETTE, 0, 0, 1, _px, PX_CAP, &w, &h) || w < 8 || h < 1) return;
    const int cell = w / 8;
    for (int c = 0; c < 8; c++)
        _pal[c] = _px[(size_t)(h / 2) * (size_t)w + (size_t)(c * cell + cell / 2)];
    _palLoaded = YES;
}

/* One object's image as an NSImage, or nil. */
- (NSImage *)thumbForObject:(int)index
{
    int w = 0, h = 0;
    if (!chfdebug_uvi_thing(_dbg, CHFDEBUG_UVI_OBJECT, index, 2, 0x00101010u, _objPx, OBJ_CAP, &w, &h) ||
        w <= 0 || h <= 0)
        return nil;
    CGImageRef img = ChfNewImage(_objPx, w, h);
    if (!img) return nil;
    NSImage *im = [[NSImage alloc] initWithCGImage:img size:NSMakeSize(w, h)];
    CGImageRelease(img);
    return im;
}

- (void)refreshObjects
{
    NSMutableArray<ChfObjectRow *> *rows = [NSMutableArray array];
    ChfObjectRow *head = [[ChfObjectRow alloc] init];
    head.heading = @"Objects (position $18F0-$18F7, colours $19F8-$19F9 and the size flags; image in UDC $38-$3B)";
    [rows addObject:head];
    for (int i = 0; i < 4; i++) {
        const chfdebug_uvi_object *o = &_uvi.obj[i];
        ChfObjectRow *r = [[ChfObjectRow alloc] init];
        r.thumb = [self thumbForObject:i];
        r.line1 = [NSString stringWithFormat:@"Object %d  x %4d  y %4d  colour %d (%s)  %s  UDC $%02X  %s", i, o->x, o->y,
                   o->colour, chfdebug_uvi_colour_name(o->colour), o->double_height ? "8x16" : "8x8 ", o->udc,
                   o->visible ? "visible" : "off screen"];
        r.line2 = [NSString stringWithFormat:@"X $%02X Y $%02X  image %02X %02X %02X %02X %02X %02X %02X %02X  hits: chars %s objects $%X",
                   o->reg_x, o->reg_y, o->shape[0], o->shape[1], o->shape[2], o->shape[3], o->shape[4], o->shape[5],
                   o->shape[6], o->shape[7], o->hits_chars ? "yes" : "no", (unsigned)o->hits_objects];
        [rows addObject:r];
    }
    _objects.rows = rows;
}

- (void)refreshScreen
{
    if (!_palLoaded) [self loadPalette];
    chfdebug_uvi_cell cells[CHFDEBUG_UVI_COLS * CHFDEBUG_UVI_MAX_ROWS];
    const int rows = chfdebug_uvi_cells(_dbg, cells);
    _screenNote.stringValue = [NSString stringWithFormat:
        @"%d rows of 16, cells %d lines tall, first line %d, fine scroll %d px. %@%@%@",
        rows, _uvi.cell_height, _uvi.first_line, _uvi.shift,
        _uvi.multicolor ? @"Multicolor: each character carries its own colour bits. " : @"One character colour for the screen. ",
        _uvi.block_graphics ? @"Block graphics from the top. " : @"",
        _uvi.two_screens ? @"Two screens ($1800 and $1A00)." : @""];

    NSFont *font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    NSMutableAttributedString *out = [[NSMutableAttributedString alloc] init];
    NSDictionary *plain = @{ NSFontAttributeName: font, NSForegroundColorAttributeName: NSColor.secondaryLabelColor };
    for (int row = 0; row < rows; row++) {
        [out appendAttributedString:[[NSAttributedString alloc]
            initWithString:[NSString stringWithFormat:@"%2d  ", row] attributes:plain]];
        for (int col = 0; col < CHFDEBUG_UVI_COLS; col++) {
            const chfdebug_uvi_cell *c = &cells[row * CHFDEBUG_UVI_COLS + col];
            NSMutableDictionary *a = [NSMutableDictionary dictionaryWithObjectsAndKeys:font, NSFontAttributeName, nil];
            a[NSForegroundColorAttributeName] = colorFromXrgb(_pal[c->fg & 7]);
            a[NSBackgroundColorAttributeName] = colorFromXrgb(_pal[c->bg & 7]);
            if (c->graphics) a[NSUnderlineStyleAttributeName] = @(NSUnderlineStyleSingle);
            [out appendAttributedString:[[NSAttributedString alloc]
                initWithString:[NSString stringWithFormat:@"%02X", c->code] attributes:a]];
            [out appendAttributedString:[[NSAttributedString alloc] initWithString:@" " attributes:plain]];
        }
        [out appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n" attributes:plain]];
    }
    if (rows == 0)
        [out appendAttributedString:[[NSAttributedString alloc]
            initWithString:@"(the machine is not running)\n" attributes:plain]];
    [_screen.textStorage setAttributedString:out];
}

- (void)refreshRegisters
{
    /* 768 decoded lines is a lot of locking for a running machine: twice a second there */
    const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    if (!chfdebug_is_stopped(_dbg) && now - _regsAt < 0.5) return;
    _regsAt = now;
    NSMutableString *s = [NSMutableString stringWithCapacity:0x300 * 64];
    for (unsigned off = 0; off < 0x300; off++) {
        char name[64], text[160];
        name[0] = text[0] = '\0';
        chfdebug_uvi_describe(_dbg, off, name, sizeof name, text, sizeof text);
        [s appendFormat:@"$%04X  %02X  %-14s %s\n", 0x1800 + off, _uvi.reg[off], name, text];
    }
    ChfSetTextKeepingPlace(_regs, s);
}

/* A click on a register line fills the edit fields from it. */
- (void)pickRegister:(int)line
{
    if (line < 0 || line >= 0x300) return;
    _regAddr.stringValue = [NSString stringWithFormat:@"$%04X", 0x1800 + line];
    _regVal.stringValue = [NSString stringWithFormat:@"$%02X", _uvi.reg[line]];
}

/* The engine takes the write running or stopped; a running program may
 * simply write the register again, so the hint says to stop first. The
 * address is the CPU's ($1800-$1AFF) or the register file's ($000-$2FF). */
- (void)writeRegister:(id)sender
{
    (void)sender;
    long off = 0, val = 0;
    if (!parseHex(_regAddr.stringValue, &off) || !parseHex(_regVal.stringValue, &val)) {
        _regNote.stringValue = @"Needs a register ($1800-$1AFF, or $000-$2FF) and a value (hex)";
        return;
    }
    if (off >= 0x1800) off -= 0x1800;
    if (off < 0 || off >= 0x300) {
        _regNote.stringValue = @"Needs a register ($1800-$1AFF, or $000-$2FF) and a value (hex)";
        return;
    }
    chfdebug_uvi_write(_dbg, (unsigned)off, (uint8_t)val);
    _regNote.stringValue = [NSString stringWithFormat:@"$%04lX = $%02lX", 0x1800 + off, val & 0xff];
    _regsAt = 0;
    [self refresh];
}

- (void)refreshControl
{
    NSMutableString *s = [NSMutableString string];
    const chfdebug_uvi *c = &_uvi;
    [s appendFormat:@"BEAM          line %d of %d, character row %d, %s\n", c->line, c->lines, c->charline,
        c->vblank ? "vertical blank" : "drawing"];
    [s appendFormat:@"SCREEN        first line %d ($18FC), fine scroll %d ($18FE bits 5-7), %d rows of %d lines\n",
        c->first_line, c->shift, c->rows, c->cell_height];
    [s appendFormat:@"  multicolor %s ($18FD bit 7)   two screens %s ($19F8 bit 6)   block graphics %s ($19F8 bit 7)\n",
        c->multicolor ? "on" : "off", c->two_screens ? "on" : "off", c->block_graphics ? "on" : "off"];
    [s appendFormat:@"  pots %s ($19F9 bit 6 %s)\n\n", c->pot_y ? "read the other axis (mux)" : "read the first axis",
        c->pot_y ? "clear" : "set"];

    [s appendFormat:@"COLOURS       screen %d (%s)  character %d (%s)  ($19F9)\n", c->screen_colour,
        chfdebug_uvi_colour_name(c->screen_colour), c->char_colour, chfdebug_uvi_colour_name(c->char_colour)];
    [s appendFormat:@"              alternate screen %d (%s)  character %d (%s)  ($19F8)\n\n", c->alt_screen_colour,
        chfdebug_uvi_colour_name(c->alt_screen_colour), c->alt_char_colour,
        chfdebug_uvi_colour_name(c->alt_char_colour)];

    [s appendFormat:@"COLLISIONS    $19FC objects x characters = $%02X, a 0 bit is a hit\n", c->coll_chars];
    for (int i = 0; i < 4; i++)
        if (!((c->coll_chars >> i) & 1u)) [s appendFormat:@"  object %d hit the character screen\n", i];
    [s appendFormat:@"              $19FD object pairs = $%02X, a 0 bit is a hit\n", c->coll_objects];
    static const char *const pairs[6] = { "0-1", "0-2", "0-3", "1-2", "1-3", "2-3" };
    for (int i = 0; i < 6; i++)
        if (!((c->coll_objects >> i) & 1u)) [s appendFormat:@"  objects %s overlap\n", pairs[i]];
    [s appendString:@"\n"];

    [s appendFormat:@"SOUND         pitch divider %d ($18FD bits 0-6)   volume %d/7 ($18FE bits 0-2)\n",
        c->sound_pitch, c->sound_volume];
    [s appendFormat:@"              tone %s ($18FE bit 3)   noise %s ($18FE bit 4)\n\n",
        c->sound_tone ? "on" : "off", c->sound_noise ? "on" : "off"];

    [s appendFormat:@"POTS          $19FF (disc 1) = $%02X   $19FE (disc 2) = $%02X\n", c->pot[0], c->pot[1]];
    [s appendString:@"KEYPADS       $1900-$1907:"];
    for (int i = 0; i < 8; i++) [s appendFormat:@" %02X", c->pad[i]];
    [s appendFormat:@"\nPANEL         $1908 = $%02X  START %d  OPTION %d  SELECT %d\n", c->panel, c->panel & 1,
        (c->panel >> 1) & 1, (c->panel >> 2) & 1];
    ChfSetTextKeepingPlace(_control, s);
}

- (void)refreshSheets
{
    int w = 0, h = 0;
    if (chfdebug_uvi_view(_dbg, CHFDEBUG_UVI_FONT, 0, 0, 3, _px, PX_CAP, &w, &h) && w > 0 && h > 0)
        [_fontPic setImage:ChfNewImage(_px, w, h)];
    if (chfdebug_uvi_view(_dbg, CHFDEBUG_UVI_OBJECTS, 0, 0, 3, _px, PX_CAP, &w, &h) && w > 0 && h > 0)
        [_objPic setImage:ChfNewImage(_px, w, h)];
    if (chfdebug_uvi_view(_dbg, CHFDEBUG_UVI_PALETTE, 0, 0, 2, _px, PX_CAP, &w, &h) && w > 0 && h > 0)
        [_palPic setImage:ChfNewImage(_px, w, h)];
    [self loadPalette];
}

- (void)refresh
{
    chfdebug_uvi_get(_dbg, &_uvi);
    [self refreshPicture];
    [self refreshBeam];
    switch ([self section]) {
    case SECTION_OBJECTS: [self refreshObjects]; break;
    case SECTION_SCREEN:  [self refreshScreen]; break;
    case SECTION_REGS:    [self refreshRegisters]; break;
    case SECTION_CONTROL: [self refreshControl]; break;
    case SECTION_SHEETS:  [self refreshSheets]; break;
    default: break;
    }
}
@end
