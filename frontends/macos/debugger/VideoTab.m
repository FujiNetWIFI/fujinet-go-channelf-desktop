/*
 * The debugger's Video tab (AppKit).
 *
 * Everything that decides what to show lives in the core (chfvidtab.h): this file is views.
 * The picture comes from the controller's cached 0xFFRRGGBB buffer and is drawn as a CGImage
 * (kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, no interpolation) over a copy of
 * that buffer; the image is rebuilt only when the controller re-rendered. A mouse move redraws
 * only the marks. Only the section showing is filled in.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#import "VideoTab.h"

#import "../KeyForward.h"

#include <stdio.h>
#include <string.h>

#include "chfvidtab.h"

#define MAX_OVERLAYS 8
#define SWATCH_ROWS 16         /* four palettes of four values */

/* A C string as an NSString, never nil (invalid UTF-8 reads as empty). */
static NSString *str(const char *text)
{
    NSString *s = text ? [NSString stringWithUTF8String:text] : nil;
    return s ? s : @"";
}

/* A 0xFFRRGGBB word as a colour. */
static NSColor *colorFromRGB(uint32_t rgb)
{
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 255) / 255.0
                               green:((rgb >> 8) & 255) / 255.0
                                blue:(rgb & 255) / 255.0
                               alpha:1.0];
}

/* The ink on the accent fill: the Videocarts yellow is bright, so the text on it is dark. */
static NSColor *accentInk(void)
{
    return colorFromRGB(CHFSESSION_ACCENT_DARK_RGB);
}

static NSFont *monoFont11(void)
{
    return [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
}

/* A selectable monospace label that wraps at `width` (0: no limit). */
static NSTextField *monoLabel(CGFloat width)
{
    NSTextField *label = [NSTextField wrappingLabelWithString:@""];
    label.font = monoFont11();
    label.selectable = YES;
    if (width > 0) label.preferredMaxLayoutWidth = width;
    return label;
}

/* ---- the picture ------------------------------------------------------------------ */

@interface ChfVideoPicture : NSView
@property (nonatomic, assign) chfvid_tab *tab;
@property (nonatomic, copy) void (^onChanged)(void);        /* the inspector changed */
@property (nonatomic, copy) void (^onBreak)(void);          /* a double-click: break on the pixel */
- (void)sync;                                               /* the controller re-rendered */
@end

@implementation ChfVideoPicture {
    CGImageRef _image;
    NSTrackingArea *_area;
    NSString *_message;
    NSSize _size;
}

- (BOOL)isFlipped { return YES; }

- (void)dealloc
{
    if (_image) CGImageRelease(_image);
}

- (NSSize)intrinsicContentSize { return _size.width > 0 ? _size : NSMakeSize(320, 240); }

- (void)sync
{
    int width = 0, height = 0;
    const uint32_t *pixels = chfvid_tab_picture(_tab, &width, &height);
    if (_image) {
        CGImageRelease(_image);
        _image = NULL;
    }
    if (pixels && width > 0 && height > 0) {
        /* a copy: the controller's buffer lives only until its next refresh */
        CFDataRef data = CFDataCreate(NULL, (const UInt8 *)pixels, (CFIndex)width * height * 4);
        CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        _image = CGImageCreate((size_t)width, (size_t)height, 8, 32, (size_t)width * 4, space,
                               kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, provider, NULL, false,
                               kCGRenderingIntentDefault);
        CGColorSpaceRelease(space);
        CGDataProviderRelease(provider);
        CFRelease(data);
        _message = nil;
        _size = NSMakeSize(width, height);
    } else {
        const char *why = chfvid_tab_why(_tab);
        _message = str(why ? why : "Nothing to show");
        _size = NSMakeSize(0, 0);
    }
    [self invalidateIntrinsicContentSize];
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    CGContextRef context = [[NSGraphicsContext currentContext] CGContext];
    [[NSColor colorWithCalibratedRed:0.07 green:0.07 blue:0.09 alpha:1] setFill];
    NSRectFill(self.bounds);
    if (!_image) {
        NSDictionary *attrs = @{ NSForegroundColorAttributeName: [NSColor colorWithCalibratedWhite:0.6 alpha:1],
                                 NSFontAttributeName: [NSFont systemFontOfSize:13] };
        NSString *message = _message ? _message : @"";
        const NSSize textSize = [message sizeWithAttributes:attrs];
        [message drawAtPoint:NSMakePoint((self.bounds.size.width - textSize.width) / 2,
                                         (self.bounds.size.height - textSize.height) / 2)
              withAttributes:attrs];
        return;
    }
    /* the view is flipped: draw the image upright */
    CGContextSaveGState(context);
    CGContextSetInterpolationQuality(context, kCGInterpolationNone);
    CGContextTranslateCTM(context, 0, _size.height);
    CGContextScaleCTM(context, 1, -1);
    CGContextDrawImage(context, CGRectMake(0, 0, _size.width, _size.height), _image);
    CGContextRestoreGState(context);
    int hover[4] = { 0 }, pin[4] = { 0 };
    const int marks = chfvid_tab_marks(_tab, hover, pin);
    if (marks & 1) {
        [[NSColor colorWithCalibratedWhite:1 alpha:0.9] setStroke];
        NSBezierPath *path = [NSBezierPath bezierPathWithRect:NSMakeRect(hover[0] + 0.5, hover[1] + 0.5,
                                                                         hover[2] - 1, hover[3] - 1)];
        path.lineWidth = 1;
        [path stroke];
    }
    if (marks & 2) {
        [[NSColor colorWithCalibratedRed:1 green:0.85 blue:0.1 alpha:1] setStroke];
        NSBezierPath *path = [NSBezierPath bezierPathWithRect:NSMakeRect(pin[0] + 1, pin[1] + 1,
                                                                         pin[2] - 2, pin[3] - 2)];
        path.lineWidth = 2;
        [path stroke];
    }
}

- (void)updateTrackingAreas
{
    [super updateTrackingAreas];
    if (_area) [self removeTrackingArea:_area];
    _area = [[NSTrackingArea alloc] initWithRect:self.bounds
                                         options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                                                 NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                                           owner:self userInfo:nil];
    [self addTrackingArea:_area];
}

- (NSPoint)local:(NSEvent *)event { return [self convertPoint:event.locationInWindow fromView:nil]; }

- (void)mouseMoved:(NSEvent *)event
{
    const NSPoint point = [self local:event];
    if (chfvid_tab_hover(_tab, (int)point.x, (int)point.y)) {
        [self setNeedsDisplay:YES];
        if (_onChanged) _onChanged();
    }
}

- (void)mouseExited:(NSEvent *)event
{
    (void)event;
    if (chfvid_tab_hover(_tab, -1, -1)) {
        [self setNeedsDisplay:YES];
        if (_onChanged) _onChanged();
    }
}

- (void)mouseDown:(NSEvent *)event
{
    const NSPoint point = [self local:event];
    if (event.clickCount >= 2) {
        chfvid_tab_hover(_tab, (int)point.x, (int)point.y);
        if (_onBreak) _onBreak();
    } else {
        chfvid_tab_pin(_tab, (int)point.x, (int)point.y);
    }
    [self setNeedsDisplay:YES];
    if (_onChanged) _onChanged();
}
@end

/* ---- a colour swatch ---------------------------------------------------------------- */

@interface ChfSwatchView : NSView
@property (nonatomic, strong) NSColor *color;
@end

@implementation ChfSwatchView
- (NSSize)intrinsicContentSize { return NSMakeSize(22, 16); }

- (void)setColor:(NSColor *)color
{
    _color = color;
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    [(_color ? _color : NSColor.blackColor) setFill];
    NSRectFill(self.bounds);
    [[NSColor colorWithCalibratedWhite:0.5 alpha:1] setStroke];
    NSBezierPath *path = [NSBezierPath bezierPathWithRect:NSInsetRect(self.bounds, 0.5, 0.5)];
    path.lineWidth = 1;
    [path stroke];
}
@end

/* ---- the tab ------------------------------------------------------------------------------ */

@interface ChfVideoTab () <NSTableViewDataSource, NSTableViewDelegate, NSTabViewDelegate>
@end

@implementation ChfVideoTab {
    chfvid_tab *_tab;
    NSView *_root;
    NSPopUpButton *_viewPopup, *_zoomPopup;
    NSButton *_breakButton, *_pixelBreak;
    NSButton *_overlayBox[MAX_OVERLAYS];
    NSButton *_filterBox[5];
    NSTextField *_status, *_note, *_pixelText, *_timingText, *_writesNote;
    NSTextField *_swatchLabel[SWATCH_ROWS];
    ChfSwatchView *_swatchView[SWATCH_ROWS];
    ChfVideoPicture *_picture;
    NSScrollView *_scroll;
    NSTabView *_sections;
    NSTableView *_regsTable, *_linesTable, *_writesTable;
    NSProgressIndicator *_beam;
    NSString *_regsSignature, *_linesSignature, *_writesSignature;
    BOOL _updating;
    int _regCount, _lineCount, _writeCount;
    int _viewportWidth, _viewportHeight;
    id _resizeObserver;
}

- (NSView *)view { return _root; }

- (instancetype)initWithDebugger:(chfdebug *)debugger
{
    self = [super init];
    if (!self) return nil;
    _tab = chfvid_tab_new(debugger);
    [self build];
    return self;
}

- (void)dealloc
{
    if (_resizeObserver) [[NSNotificationCenter defaultCenter] removeObserver:_resizeObserver];
    if (_tab) chfvid_tab_free(_tab);
    _tab = NULL;
}

- (void)build
{
    int overlayCount = 0;
    const chfvid_overlay_info *overlays = chfvid_overlays(&overlayCount);
    if (overlayCount > MAX_OVERLAYS) overlayCount = MAX_OVERLAYS;
    int zoomCount = 0;
    const int *zooms = chfvid_zooms(&zoomCount);

    _viewPopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (int i = 0; i < CHFVID_V_COUNT; i++) [_viewPopup addItemWithTitle:str(chfvid_view_name(i))];
    _viewPopup.target = self;
    _viewPopup.action = @selector(onView:);
    _viewPopup.toolTip = @"Display: the last frame the console finished.\n"
                          "Decoded: the whole VRAM, hidden cells hatched, each row in its own palette.\n"
                          "Beam: what a CRT shows, the rows the beam has passed this frame.\n"
                          "Values: the VRAM's raw 0-3 values as flat false colours.\n"
                          "Live: the VRAM as it is now, drawn like Display.";
    _zoomPopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (int i = 0; i < zoomCount; i++)
        [_zoomPopup addItemWithTitle:zooms[i] == 0 ? @"Fit" : [NSString stringWithFormat:@"%d×", zooms[i]]];
    _zoomPopup.target = self;
    _zoomPopup.action = @selector(onZoom:);
    _breakButton = [NSButton buttonWithTitle:@"Break on writes" target:self action:@selector(onBreak:)];
    _breakButton.toolTip = @"Add a VRAM write breakpoint on the inspected cell";
    NSStackView *bar = [NSStackView stackViewWithViews:@[[NSTextField labelWithString:@"View"], _viewPopup,
                                                         [NSTextField labelWithString:@"Zoom"], _zoomPopup,
                                                         _breakButton]];
    bar.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    bar.spacing = 8;

    NSMutableArray<NSView *> *row1 = [NSMutableArray array], *row2 = [NSMutableArray array];
    for (int i = 0; i < overlayCount; i++) {
        _overlayBox[i] = [NSButton checkboxWithTitle:str(overlays[i].label) target:self action:@selector(onOverlay:)];
        [(i < 4 ? row1 : row2) addObject:_overlayBox[i]];
    }
    NSStackView *overlayRow1 = [NSStackView stackViewWithViews:row1];
    NSStackView *overlayRow2 = [NSStackView stackViewWithViews:row2];
    overlayRow1.orientation = overlayRow2.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    overlayRow1.spacing = overlayRow2.spacing = 10;

    _status = monoLabel(0);
    _picture = [[ChfVideoPicture alloc] initWithFrame:NSMakeRect(0, 0, 320, 240)];
    _picture.tab = _tab;
    __weak ChfVideoTab *weak = self;
    _picture.onChanged = ^{ [weak updatePixel]; };
    _picture.onBreak = ^{ [weak breakOnPixel]; };
    _scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 640, 400)];
    _scroll.hasVerticalScroller = YES;
    _scroll.hasHorizontalScroller = YES;
    _scroll.documentView = _picture;
    _scroll.drawsBackground = NO;
    [_scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    [_scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    _note = [NSTextField labelWithString:@""];

    NSStackView *left = [NSStackView stackViewWithViews:@[bar, overlayRow1, overlayRow2, _status, _scroll, _note]];
    left.orientation = NSUserInterfaceLayoutOrientationVertical;
    left.alignment = NSLayoutAttributeLeading;
    left.spacing = 6;
    [_scroll.widthAnchor constraintEqualToAnchor:left.widthAnchor].active = YES;

    /* Fit follows the scroll view's size */
    _scroll.postsFrameChangedNotifications = YES;
    _resizeObserver = [[NSNotificationCenter defaultCenter]
        addObserverForName:NSViewFrameDidChangeNotification
                    object:_scroll
                     queue:nil
                usingBlock:^(NSNotification *note) {
        (void)note;
        [weak viewportChanged];
    }];

    /* ---- the sections, in chfvid_section_name order */
    _sections = [[NSTabView alloc] init];
    _sections.delegate = self;
    {   /* Pixel */
        _pixelText = monoLabel(370);
        _pixelBreak = [NSButton buttonWithTitle:@"Break on writes" target:self action:@selector(onBreak:)];
        NSButton *unpin = [NSButton buttonWithTitle:@"Unpin" target:self action:@selector(onUnpin:)];
        NSStackView *buttons = [NSStackView stackViewWithViews:@[_pixelBreak, unpin]];
        buttons.orientation = NSUserInterfaceLayoutOrientationHorizontal;
        buttons.spacing = 6;
        NSStackView *page = [NSStackView stackViewWithViews:@[_pixelText, buttons]];
        page.orientation = NSUserInterfaceLayoutOrientationVertical;
        page.alignment = NSLayoutAttributeLeading;
        page.spacing = 8;
        [self addSection:@"Pixel" content:page];
    }
    {   /* Registers: the four output latches, the cell and the sound */
        _regsTable = [[NSTableView alloc] init];
        [self addSection:@"Registers" content:[self scrollForTable:_regsTable
                                                           columns:@[@"name", @"raw", @"comp", @"val", @"meaning", @"last"]
                                                            titles:@[@"Register", @"Raw", @"~Raw", @"Value", @"Meaning", @"Last OUT"]
                                                            widths:@[@60, @40, @40, @120, @250, @130]]];
    }
    {   /* Colours: four palettes of four values */
        NSMutableArray<NSView *> *rows = [NSMutableArray array];
        for (int i = 0; i < SWATCH_ROWS; i++) {
            _swatchView[i] = [[ChfSwatchView alloc] initWithFrame:NSMakeRect(0, 0, 22, 16)];
            _swatchLabel[i] = monoLabel(0);
            _swatchLabel[i].selectable = NO;
            NSStackView *row = [NSStackView stackViewWithViews:@[_swatchView[i], _swatchLabel[i]]];
            row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
            row.spacing = 8;
            [rows addObject:row];
        }
        NSStackView *page = [NSStackView stackViewWithViews:rows];
        page.orientation = NSUserInterfaceLayoutOrientationVertical;
        page.alignment = NSLayoutAttributeLeading;
        page.spacing = 2;
        page.translatesAutoresizingMaskIntoConstraints = NO;
        NSScrollView *colours = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
        colours.hasVerticalScroller = YES;
        colours.drawsBackground = NO;
        colours.documentView = page;
        NSClipView *clip = colours.contentView;
        [page.topAnchor constraintEqualToAnchor:clip.topAnchor].active = YES;
        [page.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor].active = YES;
        [self addSection:@"Colours" content:colours];
    }
    {   /* Lines: the 64 VRAM rows */
        _linesTable = [[NSTableView alloc] init];
        [self addSection:@"Lines" content:[self scrollForTable:_linesTable
                                                       columns:@[@"row", @"line", @"vis", @"pal", @"bg", @"writes", @"palw", @"last"]
                                                        titles:@[@"Row", @"Line", @"Vis", @"Palette", @"Background", @"Writes", @"Pal wr", @"Last writer"]
                                                        widths:@[@36, @40, @30, @110, @70, @46, @46, @120]]];
        _linesTable.toolTip = @"Click a row to inspect the row's cell its last writer wrote";
    }
    {   /* Writes: the VRAM write log with its filters */
        static NSString *const titles[5] = { @"This frame", @"Changes only", @"Palette columns", @"Inspected cell",
                                             @"Inspected row" };
        NSMutableArray<NSView *> *boxes = [NSMutableArray array];
        for (int i = 0; i < 5; i++) {
            _filterBox[i] = [NSButton checkboxWithTitle:titles[i] target:self action:@selector(onFilter:)];
            [boxes addObject:_filterBox[i]];
        }
        NSStackView *filters1 = [NSStackView stackViewWithViews:[boxes subarrayWithRange:NSMakeRange(0, 3)]];
        NSStackView *filters2 = [NSStackView stackViewWithViews:[boxes subarrayWithRange:NSMakeRange(3, 2)]];
        filters1.orientation = filters2.orientation = NSUserInterfaceLayoutOrientationHorizontal;
        filters1.spacing = filters2.spacing = 10;
        NSButton *clear = [NSButton buttonWithTitle:@"Clear" target:self action:@selector(onClearWrites:)];
        clear.toolTip = @"Forget the write log";
        _writesNote = [NSTextField labelWithString:@""];
        _writesNote.textColor = NSColor.secondaryLabelColor;
        NSStackView *foot = [NSStackView stackViewWithViews:@[clear, _writesNote]];
        foot.orientation = NSUserInterfaceLayoutOrientationHorizontal;
        foot.spacing = 8;
        _writesTable = [[NSTableView alloc] init];
        NSScrollView *table = [self scrollForTable:_writesTable
                                           columns:@[@"write"]
                                            titles:@[@"Write (oldest first)"]
                                            widths:@[@480]];
        NSStackView *page = [NSStackView stackViewWithViews:@[filters1, filters2, foot, table]];
        page.orientation = NSUserInterfaceLayoutOrientationVertical;
        page.alignment = NSLayoutAttributeLeading;
        page.spacing = 6;
        [table.widthAnchor constraintEqualToAnchor:page.widthAnchor].active = YES;
        [self addSection:@"Writes" content:page];
    }
    {   /* Timing */
        _beam = [[NSProgressIndicator alloc] init];
        _beam.indeterminate = NO;
        _beam.minValue = 0;
        _beam.maxValue = 1000;
        _timingText = monoLabel(370);
        NSStackView *page = [NSStackView stackViewWithViews:@[_beam, _timingText]];
        page.orientation = NSUserInterfaceLayoutOrientationVertical;
        page.alignment = NSLayoutAttributeLeading;
        page.spacing = 8;
        [_beam.widthAnchor constraintEqualToAnchor:page.widthAnchor].active = YES;
        [self addSection:@"Timing" content:page];
    }
    [_sections.widthAnchor constraintEqualToConstant:560].active = YES;

    NSStackView *root = [NSStackView stackViewWithViews:@[left, _sections]];
    root.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    root.alignment = NSLayoutAttributeTop;
    root.spacing = 10;
    _root = root;
    [left setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];

    /* start from the controller's state (the CHF_DEBUGGER_VIDEO_* variables) */
    _updating = YES;
    [_viewPopup selectItemAtIndex:chfvid_tab_view(_tab)];
    for (int i = 0; i < zoomCount; i++)
        if (zooms[i] == chfvid_tab_zoom(_tab)) [_zoomPopup selectItemAtIndex:i];
    for (int i = 0; i < overlayCount; i++)
        _overlayBox[i].state = (chfvid_tab_overlays(_tab) & overlays[i].bit) ? NSControlStateValueOn
                                                                             : NSControlStateValueOff;
    const int section = chfvid_tab_env_section(_tab);
    if (section > 0 && section < (int)_sections.numberOfTabViewItems) [_sections selectTabViewItemAtIndex:section];
    _updating = NO;
}

/* Sets a table up and returns the scroll view that holds it. */
- (NSScrollView *)scrollForTable:(NSTableView *)table
                         columns:(NSArray<NSString *> *)ids
                          titles:(NSArray<NSString *> *)titles
                          widths:(NSArray<NSNumber *> *)widths
{
    for (NSUInteger i = 0; i < ids.count; i++) {
        NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:ids[i]];
        column.title = titles[i];
        column.width = widths[i].doubleValue;
        [table addTableColumn:column];
    }
    table.dataSource = self;
    table.delegate = self;
    table.usesAlternatingRowBackgroundColors = YES;
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
    scroll.hasVerticalScroller = YES;
    scroll.documentView = table;
    return scroll;
}

- (void)addSection:(NSString *)title content:(NSView *)content
{
    NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:title];
    item.label = title;
    NSView *holder = [[NSView alloc] init];
    [holder addSubview:content];
    content.translatesAutoresizingMaskIntoConstraints = NO;
    [content.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:6].active = YES;
    [content.trailingAnchor constraintEqualToAnchor:holder.trailingAnchor constant:-6].active = YES;
    [content.topAnchor constraintEqualToAnchor:holder.topAnchor constant:6].active = YES;
    /* tables, and the Writes page with its table, take the whole height */
    if ([content isKindOfClass:[NSScrollView class]] || [title isEqualToString:@"Writes"])
        [content.bottomAnchor constraintEqualToAnchor:holder.bottomAnchor constant:-6].active = YES;
    item.view = holder;
    [_sections addTabViewItem:item];
}

/* ---- actions -------------------------------------------------------------------------- */

- (void)onView:(id)sender
{
    (void)sender;
    if (_updating) return;
    chfvid_tab_set_view(_tab, (int)_viewPopup.indexOfSelectedItem);
    [self refreshView:CHFVID_TAB_PICTURE | CHFVID_TAB_TABLES];
}

- (void)onZoom:(id)sender
{
    (void)sender;
    int zoomCount = 0;
    const int *zooms = chfvid_zooms(&zoomCount);
    const NSInteger index = _zoomPopup.indexOfSelectedItem;
    if (_updating || index < 0 || index >= zoomCount) return;
    chfvid_tab_set_zoom(_tab, zooms[index]);
    [self refreshView:CHFVID_TAB_PICTURE];
}

- (void)onOverlay:(id)sender
{
    (void)sender;
    if (_updating) return;
    int overlayCount = 0;
    const chfvid_overlay_info *overlays = chfvid_overlays(&overlayCount);
    if (overlayCount > MAX_OVERLAYS) overlayCount = MAX_OVERLAYS;
    unsigned mask = 0;
    for (int i = 0; i < overlayCount; i++)
        if (_overlayBox[i].state == NSControlStateValueOn) mask |= overlays[i].bit;
    chfvid_tab_set_overlays(_tab, mask);
    [self refreshView:CHFVID_TAB_PICTURE];
}

/* The Writes page's checkboxes into the controller's filter. */
- (void)onFilter:(id)sender
{
    (void)sender;
    if (_updating) return;
    chfvid_write_filter filter;
    chfvid_tab_write_filter(_tab, &filter);
    filter.this_frame = _filterBox[0].state == NSControlStateValueOn;
    filter.changes_only = _filterBox[1].state == NSControlStateValueOn;
    filter.palette_cols = _filterBox[2].state == NSControlStateValueOn;
    filter.inspected_cell = _filterBox[3].state == NSControlStateValueOn;
    filter.inspected_row = _filterBox[4].state == NSControlStateValueOn;
    chfvid_tab_set_write_filter(_tab, &filter);
    _writesSignature = nil;
    [self refreshView:CHFVID_TAB_TABLES];
}

- (void)onClearWrites:(id)sender
{
    (void)sender;
    chfvid_tab_clear_writes(_tab);
    _writesSignature = nil;
    [self refreshView:CHFVID_TAB_TABLES];
}

/* A VRAM write breakpoint on the inspected cell, with a note of what happened. */
- (void)breakOnPixel
{
    const int ident = chfvid_tab_break_on_pixel(_tab);
    _note.stringValue = ident >= 0 ? @"Break on writes to the cell (see Breakpoints)"
                                   : @"Nothing to break on: inspect a cell first (hover, or click to pin)";
}

- (void)onBreak:(id)sender
{
    (void)sender;
    [self breakOnPixel];
}

- (void)onUnpin:(id)sender
{
    (void)sender;
    chfvid_tab_unpin(_tab);
    [_picture setNeedsDisplay:YES];
    [self updatePixel];
}

- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    (void)item;
    if (!_updating) [self refreshView:CHFVID_TAB_TABLES];
}

/* The picture's room changed: Fit needs a new picture. */
- (void)viewportChanged
{
    const NSSize viewport = _scroll.contentSize;
    if ((int)viewport.width == _viewportWidth && (int)viewport.height == _viewportHeight) return;
    if (chfvid_tab_zoom(_tab) == 0) {
        [self refreshView:CHFVID_TAB_PICTURE];
    } else {
        _viewportWidth = (int)viewport.width;
        _viewportHeight = (int)viewport.height;
        chfvid_tab_set_viewport(_tab, _viewportWidth > 2 ? _viewportWidth - 2 : 1,
                                _viewportHeight > 2 ? _viewportHeight - 2 : 1);
    }
}

/* A click on a Lines or Writes row inspects that cell. */
- (void)tableViewSelectionDidChange:(NSNotification *)note
{
    NSTableView *table = note.object;
    if (_updating || table.selectedRow < 0) return;
    const NSInteger row = table.selectedRow;
    if (table == _linesTable) {
        const chfvid_line_row *lines = NULL;
        const int count = chfvid_tab_lines(_tab, &lines);
        if (!lines || row >= count) return;
        chfvid_tab_pin_cell(_tab, lines[row].row, lines[row].last_col >= 0 ? lines[row].last_col : CHFDEBUG_VIS_COL0);
    } else if (table == _writesTable) {
        const chfdebug_vram_write *writes = NULL;
        const int count = chfvid_tab_writes(_tab, &writes);
        if (!writes || row >= count) return;
        chfvid_tab_pin_cell(_tab, writes[row].row, writes[row].col);
    } else {
        return;
    }
    [_picture setNeedsDisplay:YES];
    [self updatePixel];
}

/* ---- the sections ----------------------------------------------------------------------- */

- (void)updatePixel
{
    const chfvid_pixel *pixel = chfvid_tab_pixel(_tab);
    _pixelText.stringValue = pixel ? str(pixel->text)
                                   : @"Move over the picture to inspect a cell.\nClick to pin it; click again to unpin.\n"
                                      "Double-click to break on writes to it.";
    _pixelBreak.enabled = chfvid_tab_can_break(_tab) != 0;
    _breakButton.enabled = _pixelBreak.enabled;
    /* the swatch rows mark the inspected colour */
    if ([self sectionIndex] == CHFVID_S_COLOURS) [self fillColours];
}

- (NSInteger)sectionIndex
{
    return (NSInteger)[_sections.tabViewItems indexOfObject:_sections.selectedTabViewItem];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)table
{
    if (table == _linesTable) return _lineCount;
    if (table == _writesTable) return _writeCount;
    return _regCount;
}

- (NSView *)tableView:(NSTableView *)table viewForTableColumn:(NSTableColumn *)column row:(NSInteger)row
{
    NSString *text = @"";
    BOOL hot = NO;
    NSString *identifier = column.identifier;
    if (table == _regsTable) {
        const chfvid_reg *regs = NULL;
        const int count = chfvid_tab_regs(_tab, &regs);
        if (!regs || row < 0 || row >= count) return nil;
        if ([identifier isEqualToString:@"name"]) text = str(regs[row].name);
        else if ([identifier isEqualToString:@"raw"]) text = regs[row].port >= 0 ? [NSString stringWithFormat:@"%02X", regs[row].raw] : @"";
        else if ([identifier isEqualToString:@"comp"]) text = regs[row].port >= 0 ? [NSString stringWithFormat:@"%02X", regs[row].comp] : @"";
        else if ([identifier isEqualToString:@"val"]) text = str(regs[row].value);
        else if ([identifier isEqualToString:@"last"]) text = str(regs[row].last);
        else text = str(regs[row].meaning);
        hot = regs[row].age < CHFVID_REG_FADE;
    } else if (table == _linesTable) {
        const chfvid_line_row *lines = NULL;
        const int count = chfvid_tab_lines(_tab, &lines);
        if (!lines || row < 0 || row >= count) return nil;
        if ([identifier isEqualToString:@"row"]) text = [NSString stringWithFormat:@"%d", lines[row].row];
        else if ([identifier isEqualToString:@"line"]) text = [NSString stringWithFormat:@"%d", lines[row].scanline];
        else if ([identifier isEqualToString:@"vis"]) text = lines[row].visible ? @"yes" : @"";
        else if ([identifier isEqualToString:@"pal"])
            text = [NSString stringWithFormat:@"%d %@", lines[row].palette, str(lines[row].palette_name)];
        else if ([identifier isEqualToString:@"bg"]) text = [NSString stringWithFormat:@"#%06X", lines[row].background & 0xFFFFFF];
        else if ([identifier isEqualToString:@"writes"]) text = [NSString stringWithFormat:@"%d", lines[row].writes];
        else if ([identifier isEqualToString:@"palw"]) text = [NSString stringWithFormat:@"%d", lines[row].pal_writes];
        else
            text = lines[row].last_pc >= 0 ? [NSString stringWithFormat:@"$%04X col %d f%u", lines[row].last_pc,
                                                                        lines[row].last_col, lines[row].last_frame]
                                           : @"";
        hot = lines[row].beam != 0;
    } else {
        const chfdebug_vram_write *writes = NULL;
        const int count = chfvid_tab_writes(_tab, &writes);
        if (!writes || row < 0 || row >= count) return nil;
        char line[160];
        line[0] = '\0';
        chfvid_write_text(&writes[row], line, (int)sizeof line);
        text = str(line);
    }
    NSTextField *field = [table makeViewWithIdentifier:column.identifier owner:self];
    if (![field isKindOfClass:[NSTextField class]]) {
        field = [NSTextField labelWithString:@""];
        field.identifier = column.identifier;
        field.font = monoFont11();
    }
    field.stringValue = text;
    field.drawsBackground = hot;
    field.backgroundColor = hot ? ChfAccentColor() : NSColor.clearColor;
    field.textColor = hot ? accentInk() : NSColor.labelColor;
    return field;
}

- (void)fillRegs
{
    const chfvid_reg *regs = NULL;
    const int count = chfvid_tab_regs(_tab, &regs);
    NSMutableString *signature = [NSMutableString string];
    for (int i = 0; regs && i < count; i++)
        [signature appendFormat:@"%s%s%c", regs[i].value, regs[i].last, regs[i].age < CHFVID_REG_FADE ? '*' : '.'];
    [signature appendFormat:@"%d", count];
    if ([signature isEqualToString:_regsSignature]) return;
    _regsSignature = signature;
    _regCount = regs ? count : 0;
    _updating = YES;
    [_regsTable reloadData];
    _updating = NO;
}

- (void)fillLines
{
    const chfvid_line_row *lines = NULL;
    const int count = chfvid_tab_lines(_tab, &lines);
    NSMutableString *signature = [NSMutableString string];
    for (int i = 0; lines && i < count; i++)
        [signature appendFormat:@"%d:%d:%d:%d:%d:%X:%d;", lines[i].palette, lines[i].writes, lines[i].pal_writes,
                                lines[i].last_pc, lines[i].last_col, lines[i].background, lines[i].beam];
    [signature appendFormat:@"%d", count];
    if ([signature isEqualToString:_linesSignature]) return;
    _linesSignature = signature;
    _lineCount = lines ? count : 0;
    _updating = YES;
    [_linesTable reloadData];
    _updating = NO;
}

- (void)fillWrites
{
    const chfdebug_vram_write *writes = NULL;
    const int count = chfvid_tab_writes(_tab, &writes);
    chfvid_write_filter filter;
    chfvid_tab_write_filter(_tab, &filter);
    _updating = YES;
    _filterBox[0].state = filter.this_frame ? NSControlStateValueOn : NSControlStateValueOff;
    _filterBox[1].state = filter.changes_only ? NSControlStateValueOn : NSControlStateValueOff;
    _filterBox[2].state = filter.palette_cols ? NSControlStateValueOn : NSControlStateValueOff;
    _filterBox[3].state = filter.inspected_cell ? NSControlStateValueOn : NSControlStateValueOff;
    _filterBox[4].state = filter.inspected_row ? NSControlStateValueOn : NSControlStateValueOff;
    _updating = NO;
    NSString *signature = (writes && count > 0)
        ? [NSString stringWithFormat:@"%d:%llu:%llu", count, (unsigned long long)writes[0].cycle,
                                     (unsigned long long)writes[count - 1].cycle]
        : @"0";
    _writesNote.stringValue = [NSString stringWithFormat:@"%d write%s shown (the newest %d at most)", writes ? count : 0,
                                                         count == 1 ? "" : "s", CHFVID_WRITES_MAX];
    if ([signature isEqualToString:_writesSignature]) return;
    _writesSignature = signature;
    /* a reader at the end follows the newest write; one scrolled back keeps the place */
    NSScrollView *scroll = _writesTable.enclosingScrollView;
    const BOOL follow = !scroll || NSMaxY(scroll.contentView.documentVisibleRect) >= NSHeight(_writesTable.frame) - 4;
    _writeCount = writes ? count : 0;
    _updating = YES;
    [_writesTable reloadData];
    if (follow && _writeCount > 0) [_writesTable scrollRowToVisible:_writeCount - 1];
    _updating = NO;
}

- (void)fillColours
{
    const chfvid_swatch *swatches = NULL;
    const int count = chfvid_tab_swatches(_tab, &swatches);
    for (int i = 0; i < SWATCH_ROWS; i++) {
        if (!swatches || i >= count) {
            _swatchLabel[i].stringValue = @"";
            _swatchView[i].color = nil;
            continue;
        }
        _swatchView[i].color = colorFromRGB(swatches[i].rgb);
        _swatchLabel[i].stringValue = [NSString stringWithFormat:@"%@pal %d %@  v%d %@  #%06X  %d rows  %ld cells",
                                       swatches[i].inspected ? @"> " : @"  ", swatches[i].palette,
                                       padded(str(swatches[i].palette_name), 14), swatches[i].value,
                                       padded(str(swatches[i].name), 11), swatches[i].rgb & 0xFFFFFF,
                                       swatches[i].rows, swatches[i].cells];
    }
}

/* Pads a name to a column (never cuts a longer one). */
static NSString *padded(NSString *text, NSUInteger width)
{
    return text.length >= width ? text : [text stringByPaddingToLength:width withString:@" " startingAtIndex:0];
}

- (void)fillTiming
{
    const char *const *lines = NULL;
    const int count = chfvid_tab_timing(_tab, &lines);
    NSMutableString *text = [NSMutableString string];
    for (int i = 0; lines && i < count; i++) {
        NSString *line = str(lines[i]);
        const NSRange tab = [line rangeOfString:@"\t"];
        if (tab.location != NSNotFound)
            [text appendFormat:@"%@ %@\n", padded([line substringToIndex:tab.location], 13),
                               [line substringFromIndex:tab.location + 1]];
        else
            [text appendFormat:@"%@\n", line];
    }
    _timingText.stringValue = count ? text : @"The machine is off";
    _beam.doubleValue = chfvid_tab_beam_permille(_tab);
}

/* ---- refresh ------------------------------------------------------------------------------ */

- (void)refreshView:(unsigned)what
{
    if (what & CHFVID_TAB_PICTURE) {
        const NSSize viewport = _scroll.contentSize;
        _viewportWidth = (int)viewport.width;
        _viewportHeight = (int)viewport.height;
        if (viewport.width > 4 && viewport.height > 4)
            chfvid_tab_set_viewport(_tab, (int)viewport.width - 2, (int)viewport.height - 2);
        [_picture sync];
        [_picture setFrameSize:_picture.intrinsicContentSize];
    }
    if (what & CHFVID_TAB_TABLES) {
        _status.stringValue = str(chfvid_tab_status(_tab));
        switch ([self sectionIndex]) {
        case CHFVID_S_REGISTERS: [self fillRegs]; break;
        case CHFVID_S_COLOURS: [self fillColours]; break;
        case CHFVID_S_LINES: [self fillLines]; break;
        case CHFVID_S_WRITES: [self fillWrites]; break;
        case CHFVID_S_TIMING: [self fillTiming]; break;
        default: break;
        }
        [self updatePixelText];
    }
}

- (void)updatePixelText
{
    const chfvid_pixel *pixel = chfvid_tab_pixel(_tab);
    _pixelText.stringValue = pixel ? str(pixel->text)
                                   : @"Move over the picture to inspect a cell.\nClick to pin it; click again to unpin.\n"
                                      "Double-click to break on writes to it.";
    _pixelBreak.enabled = chfvid_tab_can_break(_tab) != 0;
    _breakButton.enabled = _pixelBreak.enabled;
}

- (void)refreshForce:(BOOL)force
{
    unsigned what = chfvid_tab_refresh(_tab, force ? 1 : 0);
    if (force) what |= CHFVID_TAB_PICTURE | CHFVID_TAB_TABLES;
    if (what & CHFVID_TAB_OFF) what |= CHFVID_TAB_PICTURE | CHFVID_TAB_TABLES;     /* the message */
    [self refreshView:what];
}

@end
