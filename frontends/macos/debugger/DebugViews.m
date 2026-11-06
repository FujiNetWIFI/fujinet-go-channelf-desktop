/*
 * The debugger's shared views and builders -- see the header.
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DebugViews.h"

#include <float.h>
#include <math.h>

/* ---- helpers -------------------------------------------------------------------- */

CGImageRef ChfNewImage(const uint32_t *px, int w, int h)
{
    if (!px || w <= 0 || h <= 0) return NULL;
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CFDataRef data = CFDataCreate(NULL, (const UInt8 *)px, (CFIndex)w * h * 4);
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    /* The core's pixels are 0x00RRGGBB in host order: a little-endian 32-bit
     * image with the alpha byte skipped reads them as they are. */
    CGImageRef img = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4, space,
                                   (CGBitmapInfo)kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little,
                                   provider, NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CFRelease(data);
    CGColorSpaceRelease(space);
    return img;
}

NSTextView *ChfMonoView(NSScrollView **scrollOut, BOOL wrap)
{
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = !wrap;
    NSTextView *view = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
    view.editable = NO;
    view.richText = NO;
    view.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
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

void ChfSetTextKeepingPlace(NSTextView *view, NSString *text)
{
    if ([view.string isEqualToString:text]) return;
    NSScrollView *scroll = view.enclosingScrollView;
    const NSPoint at = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
    view.string = text;
    if (scroll) [scroll.contentView scrollToPoint:at];
}

BOOL ChfTextAtEnd(NSTextView *view)
{
    NSScrollView *scroll = view.enclosingScrollView;
    if (!scroll) return YES;
    return NSMaxY(scroll.contentView.documentVisibleRect) >= NSHeight(view.frame) - 4;
}

void ChfSetLogText(NSTextView *view, NSString *text, BOOL follow)
{
    if (![view.string isEqualToString:text]) {
        if (follow) view.string = text;
        else ChfSetTextKeepingPlace(view, text);
    }
    if (follow) [view scrollRangeToVisible:NSMakeRange(view.string.length, 0)];
}

NSString *ChfStripControl(const char *s)
{
    NSMutableString *out = [NSMutableString string];
    char buf[2] = { 0, 0 };
    for (; *s; s++) {
        if ((unsigned char)*s >= 0x20 || *s == '\n' || *s == '\t') {
            buf[0] = *s;
            [out appendString:[NSString stringWithUTF8String:buf] ?: @""];
        }
    }
    return out;
}

NSStackView *ChfHStack(NSArray<NSView *> *views)
{
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    s.spacing = 6;
    return s;
}

NSStackView *ChfVStack(NSArray<NSView *> *views)
{
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationVertical;
    s.alignment = NSLayoutAttributeLeading;
    s.spacing = 6;
    return s;
}

void ChfFill(NSStackView *stack, NSView *view)
{
    [view.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
}

void ChfFixSize(NSView *view)
{
    const NSSize sz = view.intrinsicContentSize;
    [view.widthAnchor constraintEqualToConstant:sz.width].active = YES;
    [view.heightAnchor constraintEqualToConstant:sz.height].active = YES;
}

NSTextField *ChfLabel(NSString *text)
{
    return [NSTextField labelWithString:text];
}

/* A long hint must not push its window wider: it is cut short instead. */
static void yieldWidth(NSTextField *f)
{
    f.lineBreakMode = NSLineBreakByTruncatingTail;
    [f setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                forOrientation:NSLayoutConstraintOrientationHorizontal];
}

NSTextField *ChfNote(NSString *text)
{
    NSTextField *n = [NSTextField labelWithString:text];
    n.textColor = NSColor.secondaryLabelColor;
    yieldWidth(n);
    return n;
}

NSTextField *ChfTitle(NSString *text)
{
    NSTextField *t = [NSTextField labelWithString:text];
    t.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    yieldWidth(t);
    return t;
}

NSTextField *ChfField(NSString *placeholder, CGFloat width, id target, SEL action)
{
    NSTextField *f = [[NSTextField alloc] init];
    f.placeholderString = placeholder;
    f.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    f.target = target;
    f.action = action;
    if (width > 0) [f.widthAnchor constraintEqualToConstant:width].active = YES;
    return f;
}

NSButton *ChfButton(NSString *title, id target, SEL action)
{
    NSButton *b = [NSButton buttonWithTitle:title target:target action:action];
    [b setRefusesFirstResponder:YES];
    return b;
}

/* ---- the picture ------------------------------------------------------------------ */

@implementation ChfPictureView {
    CGImageRef _image;
    NSRect _dest;                 /* where the picture was drawn, in view coordinates */
    NSTrackingArea *_tracking;
    int _hx, _hy;                 /* the last pointer position reported */
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (!self) return nil;
    _hx = -1;
    _hy = -1;
    return self;
}

- (void)setImage:(CGImageRef)image
{
    if (_image) CGImageRelease(_image);
    _image = image;
    [self setNeedsDisplay:YES];
}

- (void)setHighlight:(NSRect)highlight
{
    if (NSEqualRects(highlight, _highlight)) return;
    _highlight = highlight;
    [self setNeedsDisplay:YES];
}

- (void)dealloc
{
    if (_image) CGImageRelease(_image);
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    CGContextRef dc = [[NSGraphicsContext currentContext] CGContext];
    const NSRect b = [self bounds];
    CGContextSetRGBFillColor(dc, 0.1, 0.1, 0.1, 1);
    CGContextFillRect(dc, b);
    _dest = NSZeroRect;
    if (!_image) return;
    const double iw = (double)CGImageGetWidth(_image), ih = (double)CGImageGetHeight(_image);
    const double w = b.size.width, h = b.size.height;
    double sw, sh;
    const double scale = floor(fmin(w / iw, h / ih));
    if (scale >= 1) {
        sw = iw * scale;
        sh = ih * scale;
    } else {
        const double want = iw / ih;
        if (w / h > want) { sh = h; sw = sh * want; } else { sw = w; sh = sw / want; }
    }
    _dest = NSMakeRect(floor((w - sw) / 2), floor((h - sh) / 2), sw, sh);
    CGContextSetInterpolationQuality(dc, kCGInterpolationNone);
    CGContextDrawImage(dc, NSRectToCGRect(_dest), _image);

    if (!NSIsEmptyRect(_highlight)) {
        const double sx = _dest.size.width / iw, sy = _dest.size.height / ih;
        const NSRect r = NSMakeRect(_dest.origin.x + _highlight.origin.x * sx,
                                    NSMaxY(_dest) - (_highlight.origin.y + _highlight.size.height) * sy,
                                    _highlight.size.width * sx, _highlight.size.height * sy);
        NSBezierPath *outer = [NSBezierPath bezierPathWithRect:NSInsetRect(r, -1.5, -1.5)];
        outer.lineWidth = 3;
        [[NSColor blackColor] setStroke];
        [outer stroke];
        NSBezierPath *inner = [NSBezierPath bezierPathWithRect:NSInsetRect(r, -1.5, -1.5)];
        inner.lineWidth = 1;
        [[NSColor whiteColor] setStroke];
        [inner stroke];
    }
}

- (void)updateTrackingAreas
{
    [super updateTrackingAreas];
    if (_tracking) [self removeTrackingArea:_tracking];
    _tracking = [[NSTrackingArea alloc] initWithRect:[self bounds]
        options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
          owner:self userInfo:nil];
    [self addTrackingArea:_tracking];
}

- (void)report:(int)x y:(int)y
{
    if (x == _hx && y == _hy) return;
    _hx = x;
    _hy = y;
    if (self.onHover) self.onHover(x, y);
}

/* The picture pixel under an event, or NO when it is outside the picture. */
- (BOOL)pixelAt:(NSEvent *)e x:(int *)px y:(int *)py
{
    if (!_image || _dest.size.width <= 0) return NO;
    const NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    if (!NSPointInRect(p, _dest)) return NO;
    const double iw = (double)CGImageGetWidth(_image), ih = (double)CGImageGetHeight(_image);
    int x = (int)floor((p.x - _dest.origin.x) * iw / _dest.size.width);
    int y = (int)floor((NSMaxY(_dest) - p.y) * ih / _dest.size.height);
    /* the picture's top edge is the rectangle's far side: keep it inside */
    if (x > (int)iw - 1) x = (int)iw - 1;
    if (y > (int)ih - 1) y = (int)ih - 1;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    *px = x;
    *py = y;
    return YES;
}

- (void)mouseMoved:(NSEvent *)e
{
    int x, y;
    if ([self pixelAt:e x:&x y:&y]) [self report:x y:y];
    else [self report:-1 y:-1];
}

- (void)mouseDown:(NSEvent *)e
{
    int x, y;
    if (self.onClick && [self pixelAt:e x:&x y:&y]) self.onClick(x, y, (int)[e clickCount]);
}

- (void)mouseExited:(NSEvent *)e
{
    (void)e;
    [self report:-1 y:-1];
}
@end

/* ---- a text view whose lines can be clicked ----------------------------------------- */

@implementation ChfLineTextView {
    CGFloat _wheel;                   /* the part of a line not yet scrolled */
}
- (void)mouseDown:(NSEvent *)event
{
    if (!self.onClickLine && !self.onDoubleClickLine) {
        [super mouseDown:event];
        return;
    }
    const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSUInteger idx = [self characterIndexForInsertionAtPoint:p];
    NSString *text = self.string;
    if (idx > text.length) idx = text.length;
    int line = 0;
    for (NSUInteger i = 0; i < idx && i < text.length; i++)
        if ([text characterAtIndex:i] == '\n') line++;
    if ([event clickCount] >= 2 && self.onDoubleClickLine) self.onDoubleClickLine(line);
    else if (self.onClickLine) self.onClickLine(line);
}

- (void)scrollWheel:(NSEvent *)event
{
    if (!self.onScroll) {
        [super scrollWheel:event];
        return;
    }
    /* a wheel notch reports lines already; a trackpad, points */
    CGFloat dy = -event.scrollingDeltaY;
    if (event.hasPreciseScrollingDeltas) dy /= 14.0;
    _wheel += dy;
    const int lines = (int)_wheel;
    if (lines == 0) return;
    _wheel -= lines;
    self.onScroll(lines);
}
@end
