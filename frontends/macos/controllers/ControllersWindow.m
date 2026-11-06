/*
 * ChfControllersWindow -- the AppKit Controllers panel: the Channel F's
 * console panel (RESET and the four numbered buttons) and its two hand
 * controllers, drawn from the session's layout (chfsession_controls_layout)
 * with NSBezierPath, so every frontend shows the same console; the gamepads
 * and the player each drives; and Map mode.
 *
 * Each hand controller is the one knob with four axes, shown twice: from
 * above (a ring of eight direction wedges round the knob, which shifts toward
 * the held direction, turns when twisted, shrinks when pushed and grows when
 * pulled, with a twist arrow either side) and from the side (the handle, with
 * PULL above and PUSH below, and the knob's height). One custom-drawn view
 * (ChfConsoleView) paints the console and takes the mouse; the layout, the
 * exact hit shapes and the stick maths are the core's.
 *
 * Gestures: drag on the stick = eight ways (nothing near the middle); a press
 * on the cap pushes, a right-press or Control-press pulls; the wheel and the
 * trackpad's scroll twist (a notch each, coalesced); a press on a wedge, a
 * twist or PULL/PUSH arrow, or a console button holds it. The pointer is its
 * own input source (chfsession_pointer_set), so the panel lets go of
 * everything the mouse holds when it is hidden, rebuilt or loses key status.
 *
 * What the console sees held -- host keyboard, gamepads and the mouse -- is
 * painted in the accent, so the panel doubles as an input tester for a new
 * gamepad. Keys typed with the panel key go to the console as in the main
 * window; no view here takes the keyboard, so the panel itself is the first
 * responder.
 *
 * FIXED SIZE: an NSPanel without a resize mask, sized from its contents. Only
 * a rebuild resizes it; the top edge stays put. Hidden at start, ordered out
 * rather than closed, so its position survives. The 30 Hz poll runs only
 * while the panel can be seen.
 *
 * MAP MODE: Map arms a two-step rebind -- click a control on the picture or
 * choose one from the list (all CHF_TARGET_COUNT targets, the app's own
 * actions among them), then press a key or a gamepad button; the next one is
 * bound and whatever held it is told so in the status line. Esc cancels the
 * pick; typed keys are not typed while Map is armed. Rebinding steals the key
 * from its old target.
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "ControllersWindow.h"

#import "../KeyForward.h"
#import "../help/HelpWindow.h"

#include <math.h>
#include <string.h>

#define UNIT        36.0    /* points per layout unit (a console button's width) */
#define MARGIN      14.0
#define WOOD_H      10.0    /* the walnut strip along the top of the case */
#define HEADER_H    36.0    /* the strip and the nameplate above the panel */
#define MAX_CTL     64      /* room for the core's 35 controls */
#define TITLE_H     18.0
#define ROW_H       26.0
#define EFF_W      120.0
#define POPUP_W    120.0
#define BTN_H       28.0
#define MAX_PAD_ROWS 3
#define SLOW_TICKS   8      /* the 30 Hz tick's 250 ms part */
#define NOTCH_PT    12.0    /* trackpad points that make one twist notch */
#define MAX_NOTCHES  4      /* per scroll event */

static NSString *nsstr(const char *s)
{
    NSString *r = s ? [NSString stringWithUTF8String:s] : nil;
    return r ? r : @"";
}

static NSString *keyName(uint32_t keysym)
{
    char name[48];
    name[0] = '\0';
    if (keysym) chfsession_keysym_name(keysym, name, (int)sizeof name);
    return nsstr(name);
}

static NSColor *rgb(uint32_t v)
{
    return [NSColor colorWithSRGBRed:((v >> 16) & 0xff) / 255.0
                               green:((v >> 8) & 0xff) / 255.0
                                blue:(v & 0xff) / 255.0
                               alpha:1.0];
}

/* Lines of text centred in r, the font shrunk (to 6 pt) until they fit. */
static void drawFitted(NSString *text, NSRect r, CGFloat size, BOOL bold, NSColor *ink)
{
    NSArray<NSString *> *lines = [text componentsSeparatedByString:@"\n"];
    const CGFloat nlines = (CGFloat)[lines count];
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = NSTextAlignmentCenter;
    para.lineBreakMode = NSLineBreakByClipping;
    NSDictionary *attrs = nil;
    CGFloat lineH = 0;
    for (;;) {
        NSFont *font = bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];
        attrs = @{ NSFontAttributeName: font,
                   NSParagraphStyleAttributeName: para,
                   NSForegroundColorAttributeName: ink };
        CGFloat w = 0;
        lineH = 0;
        for (NSString *l in lines) {
            const NSSize s = [l sizeWithAttributes:attrs];
            if (s.width > w) w = s.width;
            if (s.height > lineH) lineH = s.height;
        }
        if ((w <= r.size.width && lineH * nlines <= r.size.height + 2) || size <= 6) break;
        size -= 0.5;
    }
    CGFloat y = r.origin.y + floor((r.size.height - lineH * nlines) / 2);
    for (NSString *l in lines) {
        [l drawInRect:NSMakeRect(r.origin.x, y, r.size.width, lineH) withAttributes:attrs];
        y += lineH;
    }
}

/* The sector of a ring between radii r0 and r1, centred on centreDeg and
 * halfDeg either side of it. Angles are mathematical (counter-clockwise as
 * seen, 0 = right); the view is flipped, so y is subtracted. Straight
 * segments, so nothing depends on how AppKit turns arcs in a flipped view. */
static NSBezierPath *ringSector(NSPoint c, CGFloat r0, CGFloat r1, double centreDeg, double halfDeg)
{
    NSBezierPath *p = [NSBezierPath bezierPath];
    const int steps = 8;
    for (int i = 0; i <= steps; i++) {
        const double a = (centreDeg - halfDeg + 2.0 * halfDeg * i / steps) * M_PI / 180.0;
        const NSPoint pt = NSMakePoint(c.x + r1 * cos(a), c.y - r1 * sin(a));
        if (i == 0) [p moveToPoint:pt]; else [p lineToPoint:pt];
    }
    for (int i = steps; i >= 0; i--) {
        const double a = (centreDeg - halfDeg + 2.0 * halfDeg * i / steps) * M_PI / 180.0;
        [p lineToPoint:NSMakePoint(c.x + r0 * cos(a), c.y - r0 * sin(a))];
    }
    [p closePath];
    return p;
}

/* The direction a wedge points, in degrees, from its contacts. */
static double wedgeAngle(unsigned acts)
{
    const int x = (int)((acts >> CHF_ACT_RIGHT) & 1u) - (int)((acts >> CHF_ACT_LEFT) & 1u);
    const int y = (int)((acts >> CHF_ACT_FORWARD) & 1u) - (int)((acts >> CHF_ACT_BACK) & 1u);
    return atan2((double)y, (double)x) * 180.0 / M_PI;
}

/* An open circular arrow about c: from fromDeg to toDeg (increasing is
 * counter-clockwise as seen), with its head at the end. */
static void strokeArcArrow(NSPoint c, CGFloat radius, double fromDeg, double toDeg, NSColor *ink)
{
    const double dir = toDeg >= fromDeg ? 1.0 : -1.0;
    NSBezierPath *arc = [NSBezierPath bezierPath];
    BOOL first = YES;
    for (double a = fromDeg; dir > 0 ? a <= toDeg : a >= toDeg; a += dir * 10.0) {
        const double t = a * M_PI / 180.0;
        const NSPoint pt = NSMakePoint(c.x + radius * cos(t), c.y - radius * sin(t));
        if (first) [arc moveToPoint:pt]; else [arc lineToPoint:pt];
        first = NO;
    }
    arc.lineWidth = 2.5;
    arc.lineCapStyle = NSLineCapStyleRound;
    [ink setStroke];
    [arc stroke];

    /* the head: along the tangent at the end */
    const double e = toDeg * M_PI / 180.0;
    const NSPoint end = NSMakePoint(c.x + radius * cos(e), c.y - radius * sin(e));
    const NSPoint tng = NSMakePoint(-dir * sin(e), -dir * cos(e));
    const NSPoint nrm = NSMakePoint(-tng.y, tng.x);
    NSBezierPath *head = [NSBezierPath bezierPath];
    [head moveToPoint:NSMakePoint(end.x + tng.x * 7, end.y + tng.y * 7)];
    [head lineToPoint:NSMakePoint(end.x + nrm.x * 5, end.y + nrm.y * 5)];
    [head lineToPoint:NSMakePoint(end.x - nrm.x * 5, end.y - nrm.y * 5)];
    [head closePath];
    [ink setFill];
    [head fill];
}

/* ---- views --------------------------------------------------------------------- */

/* Laid out from the top, like the other frontends' panels. */
@interface ChfFlippedView : NSView
@end

@implementation ChfFlippedView
- (BOOL)isFlipped { return YES; }
@end

/* Keys typed with the panel key go to its owner. */
@class ChfControllersWindow;
@interface ChfControllersPanel : NSPanel
@property (nonatomic, weak) ChfControllersWindow *owner;
@end

@interface ChfControllersWindow () <NSWindowDelegate>
- (void)mapPick:(int)target;
- (NSString *)legendForTarget:(int)target;
- (void)panelKeyDown:(NSEvent *)e;
- (void)panelKeyUp:(NSEvent *)e;
- (void)panelFlagsChanged:(NSEvent *)e;
@end

/* What a hand controller's knob looks like now, eased toward where the held
 * contacts put it: the tilt (-1..1, +x right, +y away from the player), the
 * twist (-1 counter-clockwise .. 1 clockwise) and the axis (-1 pushed down ..
 * 1 pulled up). */
typedef struct { double tx, ty, twist, axis; } ChfKnob;

static double ease(double cur, double target)
{
    const double d = target - cur;
    return fabs(d) < 0.01 ? target : cur + d * 0.5;
}

/* The console: the charcoal case with its walnut strip, the panel's buttons
 * and the two hand controllers. Everything on it is drawn here and hit-tested
 * here (by the core). */
@interface ChfConsoleView : ChfFlippedView
@property (nonatomic) chfsession *session;
@property (nonatomic, weak) ChfControllersWindow *owner;
@property (nonatomic, copy) NSString *title;
@property (nonatomic) BOOL mapping;          /* Map armed: bindings shown, nothing lit */
@property (nonatomic) int mapTarget;         /* the control waiting for a key, or -1 */
@property (nonatomic) BOOL showKeys;         /* the bound host key as a small legend */
@property (nonatomic, readonly) NSSize contentSize;
- (void)configureForMachine:(int)machine;
/* Reads what the console sees held and moves the knobs toward it; redraws
 * when anything changed. */
- (void)poll;
/* Lets go of everything the pointer holds. */
- (void)letGo;
@end

@implementation ChfConsoleView {
    int _machine;
    const chf_control *_ctl;
    int _n;
    float _lw, _lh;
    NSSize _size;
    BOOL _lit[MAX_CTL];
    unsigned _held[2];
    ChfKnob _knob[2];

    /* the mouse */
    int _downIdx;                /* the control pressed, or -1 */
    int _downPort;               /* the controller whose pointer mask is set, or -1 */
    int _downTarget;             /* the console button held, or -1 */
    unsigned _downMask;
    unsigned _axisBit;           /* a cap press: push, or pull */
    BOOL _stickDrag, _fromCap, _moved;
    NSPoint _downPt;
    CGFloat _wheel;              /* scroll not yet turned into notches */
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (!self) return nil;
    _downIdx = -1;
    _downPort = -1;
    _downTarget = -1;
    _mapTarget = -1;
    return self;
}

- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { (void)e; return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }
/* a Control-click is a pull, not a context menu */
- (NSMenu *)menuForEvent:(NSEvent *)e { (void)e; return nil; }

- (NSSize)contentSize { return _size; }

- (void)configureForMachine:(int)machine
{
    _machine = machine;
    _ctl = chfsession_controls_layout(machine, &_n, &_lw, &_lh);
    if (!_ctl) _n = 0;
    if (_n > MAX_CTL) _n = MAX_CTL;
    _size = NSMakeSize(ceil(_lw * UNIT) + 2 * MARGIN, HEADER_H + ceil(_lh * UNIT) + MARGIN);
    memset(_lit, 0, sizeof _lit);
    [self setNeedsDisplay:YES];
}

/* ---- geometry ------------------------------------------------------------------ */

- (NSRect)rectOf:(const chf_control *)c
{
    return NSMakeRect(MARGIN + c->x * UNIT, HEADER_H + c->y * UNIT, c->w * UNIT, c->h * UNIT);
}

/* The STICK of a controller: its centre and its radius, in view points. */
- (BOOL)stickOfPort:(int)port centre:(NSPoint *)centre radius:(CGFloat *)radius
{
    for (int i = 0; i < _n; i++) {
        const chf_control *c = &_ctl[i];
        if (c->kind != CHF_CTL_STICK || c->port != port) continue;
        const NSRect r = [self rectOf:c];
        *centre = NSMakePoint(NSMidX(r), NSMidY(r));
        *radius = r.size.width / 2;
        return YES;
    }
    return NO;
}

/* The controller whose block a point is in, or -1 (the captions give the
 * blocks: a column from the caption down). */
- (int)controllerAt:(NSPoint)p
{
    for (int i = 0; i < _n; i++) {
        const chf_control *c = &_ctl[i];
        if (c->kind != CHF_CTL_CAPTION || c->port < 0) continue;
        const NSRect r = [self rectOf:c];
        if (p.x >= r.origin.x && p.x < NSMaxX(r) && p.y >= r.origin.y &&
            p.y < HEADER_H + _lh * UNIT)
            return c->port;
    }
    return -1;
}

/* ---- the held state --------------------------------------------------------------- */

- (void)poll
{
    BOOL dirty = NO;
    if (self.mapping) {
        _held[0] = _held[1] = 0;
    } else {
        _held[0] = chfsession_buttons_held(_session, 0);
        _held[1] = chfsession_buttons_held(_session, 1);
    }
    /* the stick's contacts, with opposite ones cancelling */
    unsigned stick[2];
    for (int p = 0; p < 2; p++) {
        unsigned s = _held[p] & ((1u << CHF_ACT_RIGHT) | (1u << CHF_ACT_LEFT) |
                                 (1u << CHF_ACT_BACK) | (1u << CHF_ACT_FORWARD));
        if ((s & (1u << CHF_ACT_RIGHT)) && (s & (1u << CHF_ACT_LEFT)))
            s &= ~((1u << CHF_ACT_RIGHT) | (1u << CHF_ACT_LEFT));
        if ((s & (1u << CHF_ACT_BACK)) && (s & (1u << CHF_ACT_FORWARD)))
            s &= ~((1u << CHF_ACT_BACK) | (1u << CHF_ACT_FORWARD));
        stick[p] = s;
    }

    for (int i = 0; i < _n; i++) {
        const chf_control *c = &_ctl[i];
        BOOL on = NO;
        switch (c->kind) {
        case CHF_CTL_BUTTON:
            on = !self.mapping && chfsession_key_held(_session, c->target - CHF_TARGET_KEY(0)) != 0;
            break;
        case CHF_CTL_RESET:
            on = !self.mapping && chfsession_switch_held(_session, c->target - CHF_TARGET_SWITCH(0)) != 0;
            break;
        case CHF_CTL_DIRECTION:
            /* the wedge the stick is in: a diagonal lights only its own */
            on = c->port >= 0 && c->port < 2 && c->act_mask != 0 && stick[c->port] == c->act_mask;
            break;
        case CHF_CTL_TWIST:
        case CHF_CTL_AXIS:
            on = c->port >= 0 && c->port < 2 && (_held[c->port] & c->act_mask) != 0;
            break;
        default:
            break;
        }
        if (on != _lit[i]) {
            _lit[i] = on;
            dirty = YES;
        }
    }

    for (int p = 0; p < 2; p++) {
        double tx = (double)((stick[p] >> CHF_ACT_RIGHT) & 1u) - (double)((stick[p] >> CHF_ACT_LEFT) & 1u);
        double ty = (double)((stick[p] >> CHF_ACT_FORWARD) & 1u) - (double)((stick[p] >> CHF_ACT_BACK) & 1u);
        if (tx != 0 && ty != 0) { tx *= 0.7071; ty *= 0.7071; }
        const double twist = (double)((_held[p] >> CHF_ACT_TWIST_CW) & 1u) -
                             (double)((_held[p] >> CHF_ACT_TWIST_CCW) & 1u);
        const double axis = (double)((_held[p] >> CHF_ACT_PULL) & 1u) -
                            (double)((_held[p] >> CHF_ACT_PUSH) & 1u);
        ChfKnob k = _knob[p];
        k.tx = ease(k.tx, tx);
        k.ty = ease(k.ty, ty);
        k.twist = ease(k.twist, twist);
        k.axis = ease(k.axis, axis);
        if (k.tx != _knob[p].tx || k.ty != _knob[p].ty || k.twist != _knob[p].twist ||
            k.axis != _knob[p].axis) {
            _knob[p] = k;
            dirty = YES;
        }
    }
    if (dirty) [self setNeedsDisplay:YES];
}

/* ---- drawing -------------------------------------------------------------------------- */

- (NSString *)legendFor:(const chf_control *)c
{
    if (!(self.mapping || self.showKeys) || c->target < 0) return nil;
    return [self.owner legendForTarget:c->target];
}

- (BOOL)selected:(const chf_control *)c
{
    return self.mapping && c->target >= 0 && c->target == self.mapTarget;
}

/* A console button: the cap with its number, the bound key under the number
 * in Map mode, and its legend below the cap. RESET carries its name below in
 * the accent. */
- (void)drawButton:(const chf_control *)c lit:(BOOL)lit
{
    NSColor *accent = rgb(CHFSESSION_ACCENT_RGB);
    const NSRect cap = [self rectOf:c];
    const NSRect r = NSInsetRect(cap, 0.5, 0.5);
    const BOOL on = lit || [self selected:c];
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:r xRadius:5 yRadius:5];
    [(on ? accent : rgb(0x4a4a4f)) setFill];
    [path fill];
    [(on ? [NSColor whiteColor] : rgb(0x1f1f22)) setStroke];
    path.lineWidth = on ? 2 : 1;
    [path stroke];

    NSColor *ink = on ? rgb(0x1a1408) : [NSColor whiteColor];
    NSString *key = [self legendFor:c];
    const NSRect inner = NSInsetRect(cap, 3, 2);
    const CGFloat top = key.length ? floor(inner.size.height * 0.58) : inner.size.height;
    if (c->kind == CHF_CTL_BUTTON)
        drawFitted(nsstr(c->label), NSMakeRect(inner.origin.x, inner.origin.y, inner.size.width, top),
                   15, YES, ink);
    if (key.length)
        drawFitted(key, NSMakeRect(inner.origin.x, inner.origin.y + (c->kind == CHF_CTL_BUTTON ? top : 0),
                                   inner.size.width,
                                   inner.size.height - (c->kind == CHF_CTL_BUTTON ? top : 0)),
                   8, NO, on ? rgb(0x1a1408) : rgb(0xd8d8dc));

    const NSRect below = NSMakeRect(cap.origin.x - 10, NSMaxY(cap) + 3, cap.size.width + 20, 16);
    if (c->kind == CHF_CTL_RESET)
        drawFitted(nsstr(c->label), below, 11, YES, accent);
    else if (c->legend)
        drawFitted(nsstr(c->legend), below, 10, NO, rgb(0xcfcfd2));
}

/* One of the eight wedges round the knob. */
- (void)drawWedge:(const chf_control *)c centre:(NSPoint)ctr radius:(CGFloat)R lit:(BOOL)lit
{
    NSColor *accent = rgb(CHFSESSION_ACCENT_RGB);
    const BOOL on = lit || [self selected:c];
    const double ang = wedgeAngle(c->act_mask);
    NSBezierPath *w = ringSector(ctr, R * 0.5 + 1.5, R - 1.5, ang, 20.5);
    [(on ? accent : rgb(0x3a3a40)) setFill];
    [w fill];
    [(on ? [NSColor whiteColor] : rgb(0x1c1c1f)) setStroke];
    w.lineWidth = on ? 2 : 1;
    [w stroke];
    NSString *key = [self legendFor:c];
    if (key.length) {
        const double a = ang * M_PI / 180.0;
        const CGFloat rm = R * 0.75;
        drawFitted(key, NSMakeRect(ctr.x + rm * cos(a) - 16, ctr.y - rm * sin(a) - 6, 32, 12),
                   8, NO, on ? rgb(0x1a1408) : rgb(0xd8d8dc));
    }
}

/* A twist arrow: a circular arrow, counter-clockwise or clockwise. */
- (void)drawTwist:(const chf_control *)c lit:(BOOL)lit
{
    NSColor *accent = rgb(CHFSESSION_ACCENT_RGB);
    const NSRect r = NSInsetRect([self rectOf:c], 1, 1);
    const BOOL on = lit || [self selected:c];
    NSBezierPath *bg = [NSBezierPath bezierPathWithRoundedRect:r xRadius:8 yRadius:8];
    [(on ? accent : rgb(0x3a3a40)) setFill];
    [bg fill];
    [(on ? [NSColor whiteColor] : rgb(0x1c1c1f)) setStroke];
    bg.lineWidth = on ? 2 : 1;
    [bg stroke];
    const BOOL ccw = c->act_mask == (1u << CHF_ACT_TWIST_CCW);
    NSString *key = [self legendFor:c];
    const CGFloat keyH = key.length ? 12 : 0;
    const NSPoint mid = NSMakePoint(NSMidX(r), NSMidY(r) - keyH / 2);
    const CGFloat rad = MIN(r.size.width * 0.5 - 6, 12.0);
    NSColor *ink = on ? rgb(0x1a1408) : rgb(0xe8e8ec);
    if (ccw) strokeArcArrow(mid, rad, 50, 320, ink);
    else     strokeArcArrow(mid, rad, 130, -140, ink);
    if (key.length)
        drawFitted(key, NSMakeRect(r.origin.x, NSMaxY(r) - 14, r.size.width, 12), 8, NO, ink);
}

/* PULL above the handle, PUSH below it: a triangle and the word. */
- (void)drawAxis:(const chf_control *)c lit:(BOOL)lit
{
    NSColor *accent = rgb(CHFSESSION_ACCENT_RGB);
    const NSRect r = NSInsetRect([self rectOf:c], 1, 1);
    const BOOL on = lit || [self selected:c];
    NSBezierPath *bg = [NSBezierPath bezierPathWithRoundedRect:r xRadius:8 yRadius:8];
    [(on ? accent : rgb(0x3a3a40)) setFill];
    [bg fill];
    [(on ? [NSColor whiteColor] : rgb(0x1c1c1f)) setStroke];
    bg.lineWidth = on ? 2 : 1;
    [bg stroke];
    NSColor *ink = on ? rgb(0x1a1408) : rgb(0xe8e8ec);
    const BOOL pull = c->act_mask == (1u << CHF_ACT_PULL);
    const CGFloat mx = NSMidX(r);
    NSString *key = [self legendFor:c];
    const CGFloat keyH = key.length ? 10 : 0;
    NSBezierPath *tri = [NSBezierPath bezierPath];
    if (pull) {
        const CGFloat y0 = r.origin.y + 4;
        [tri moveToPoint:NSMakePoint(mx, y0)];
        [tri lineToPoint:NSMakePoint(mx - 8, y0 + 11)];
        [tri lineToPoint:NSMakePoint(mx + 8, y0 + 11)];
        drawFitted(nsstr(c->label), NSMakeRect(r.origin.x, y0 + 13, r.size.width, 13), 10, YES, ink);
        if (key.length)
            drawFitted(key, NSMakeRect(r.origin.x, y0 + 26, r.size.width, keyH), 8, NO, ink);
    } else {
        const CGFloat y1 = NSMaxY(r) - 4;
        [tri moveToPoint:NSMakePoint(mx, y1)];
        [tri lineToPoint:NSMakePoint(mx - 8, y1 - 11)];
        [tri lineToPoint:NSMakePoint(mx + 8, y1 - 11)];
        drawFitted(nsstr(c->label), NSMakeRect(r.origin.x, y1 - 24, r.size.width, 13), 10, YES, ink);
        if (key.length)
            drawFitted(key, NSMakeRect(r.origin.x, y1 - 24 - keyH, r.size.width, keyH), 8, NO, ink);
    }
    [tri closePath];
    [ink setFill];
    [tri fill];
}

/* The handle from the side: the housing, the shaft and the knob's cap, which
 * rises with a pull and sinks with a push. */
- (void)drawSide:(const chf_control *)c
{
    NSColor *accent = rgb(CHFSESSION_ACCENT_RGB);
    const NSRect r = [self rectOf:c];
    const int port = c->port >= 0 && c->port < 2 ? c->port : 0;
    const double axis = _knob[port].axis;
    const CGFloat mx = NSMidX(r);

    NSBezierPath *shaft = [NSBezierPath bezierPathWithRoundedRect:
        NSMakeRect(mx - 5, r.origin.y + 3, 10, r.size.height - 6) xRadius:3 yRadius:3];
    [rgb(0x55555a) setFill];
    [shaft fill];
    NSBezierPath *housing = [NSBezierPath bezierPathWithRoundedRect:
        NSMakeRect(mx - 20, NSMaxY(r) - 9, 40, 9) xRadius:3 yRadius:3];
    [rgb(0x1f1f22) setFill];
    [housing fill];

    const CGFloat topY = r.origin.y + 2;
    const CGFloat pushY = NSMaxY(r) - 22;
    const CGFloat neutral = topY + (pushY - topY) * 0.45;
    const CGFloat ky = axis > 0 ? neutral - (CGFloat)axis * (neutral - topY)
                                : neutral - (CGFloat)axis * (pushY - neutral);
    NSBezierPath *cap = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(mx - 20, ky, 40, 12)
                                                        xRadius:5 yRadius:5];
    [rgb(0x3b3b40) setFill];
    [cap fill];
    [(fabs(axis) > 0.5 ? accent : rgb(0x8a8a90)) setStroke];
    cap.lineWidth = fabs(axis) > 0.5 ? 2.5 : 1.5;
    [cap stroke];
}

/* The disc: its dish, with the ring the wedges sit on; the knob goes over
 * the wedges (drawKnob). */
- (void)drawDish:(const chf_control *)c
{
    const NSRect r = [self rectOf:c];
    NSBezierPath *dish = [NSBezierPath bezierPathWithOvalInRect:r];
    [rgb(0x1c1c1f) setFill];
    [dish fill];
    [rgb(0x3e3e44) setStroke];
    dish.lineWidth = 1;
    [dish stroke];
}

/* The knob from above: shifted toward the held direction, turned by the
 * twist (a mark on its rim), smaller pushed and bigger pulled. */
- (void)drawKnob:(const chf_control *)c
{
    NSColor *accent = rgb(CHFSESSION_ACCENT_RGB);
    const NSRect r = [self rectOf:c];
    const int port = c->port >= 0 && c->port < 2 ? c->port : 0;
    const ChfKnob k = _knob[port];
    const CGFloat ctrX = NSMidX(r), ctrY = NSMidY(r);
    const CGFloat r0 = r.size.width / 4;
    const CGFloat kr = r0 * 0.9 * (CGFloat)(1.0 + 0.18 * k.axis);
    const NSPoint kc = NSMakePoint(ctrX + (CGFloat)k.tx * r0 * 0.5, ctrY - (CGFloat)k.ty * r0 * 0.5);

    NSBezierPath *body = [NSBezierPath bezierPathWithOvalInRect:
        NSMakeRect(kc.x - kr, kc.y - kr, 2 * kr, 2 * kr)];
    const BOOL active = fabs(k.axis) > 0.5;
    [rgb(0x3b3b40) setFill];
    [body fill];
    [(active ? accent : rgb(0x8a8a90)) setStroke];
    body.lineWidth = active ? 2.5 : 1.5;
    [body stroke];
    NSBezierPath *grip = [NSBezierPath bezierPathWithOvalInRect:
        NSMakeRect(kc.x - kr * 0.55, kc.y - kr * 0.55, kr * 1.1, kr * 1.1)];
    [rgb(0x55555a) setStroke];
    grip.lineWidth = 1;
    [grip stroke];

    /* the mark on the rim: straight up at rest, turned up to 25 degrees */
    const double a = (90.0 - 25.0 * k.twist) * M_PI / 180.0;
    NSBezierPath *mark = [NSBezierPath bezierPath];
    [mark moveToPoint:NSMakePoint(kc.x + kr * 0.4 * cos(a), kc.y - kr * 0.4 * sin(a))];
    [mark lineToPoint:NSMakePoint(kc.x + kr * 0.88 * cos(a), kc.y - kr * 0.88 * sin(a))];
    mark.lineWidth = 3;
    mark.lineCapStyle = NSLineCapStyleRound;
    [(fabs(k.twist) > 0.5 ? accent : rgb(0xe8e8ec)) setStroke];
    [mark stroke];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    const NSRect b = [self bounds];
    NSColor *accent = rgb(CHFSESSION_ACCENT_RGB);

    /* the charcoal case with its walnut strip and nameplate */
    [rgb(0x302F2F) setFill];
    NSRectFill(b);
    [rgb(0x47332A) setFill];
    NSRectFill(NSMakeRect(0, 0, b.size.width, WOOD_H));
    [rgb(0x36271F) setFill];
    for (CGFloat y = 3; y < WOOD_H; y += 4) NSRectFill(NSMakeRect(0, y, b.size.width, 1));
    [rgb(0x1f1f22) setFill];
    NSRectFill(NSMakeRect(0, WOOD_H, b.size.width, 1));
    NSDictionary *nameplate = @{ NSFontAttributeName: [NSFont boldSystemFontOfSize:12],
                                 NSForegroundColorAttributeName: rgb(0xcfcfd2) };
    [self.title drawAtPoint:NSMakePoint(MARGIN, WOOD_H + 5) withAttributes:nameplate];

    /* the plate under the console's buttons and legends */
    CGFloat px0 = 1e9, px1 = -1e9, py0 = 1e9, py1 = -1e9;
    for (int i = 0; i < _n; i++) {
        const chf_control *c = &_ctl[i];
        if (c->port >= 0 || (c->kind != CHF_CTL_BUTTON && c->kind != CHF_CTL_RESET)) continue;
        const NSRect r = [self rectOf:c];
        px0 = MIN(px0, r.origin.x - 10);
        px1 = MAX(px1, NSMaxX(r) + 10);
        py0 = MIN(py0, r.origin.y - 6);
        py1 = MAX(py1, NSMaxY(r) + 22);
    }
    if (px1 > px0) {
        NSBezierPath *plate = [NSBezierPath bezierPathWithRoundedRect:
            NSMakeRect(px0, py0, px1 - px0, py1 - py0) xRadius:8 yRadius:8];
        [rgb(0x161618) setFill];
        [plate fill];
    }

    /* each hand controller's body and its name */
    for (int i = 0; i < _n; i++) {
        const chf_control *c = &_ctl[i];
        if (c->kind != CHF_CTL_CAPTION || c->port < 0) continue;
        const NSRect cr = [self rectOf:c];
        const NSRect body = NSMakeRect(cr.origin.x - 4, cr.origin.y - 5, cr.size.width + 8,
                                       HEADER_H + _lh * UNIT + 5 - cr.origin.y + 6);
        NSBezierPath *base = [NSBezierPath bezierPathWithRoundedRect:body xRadius:10 yRadius:10];
        [rgb(0x232326) setFill];
        [base fill];
        [rgb(0x3e3e44) setStroke];
        base.lineWidth = 1;
        [base stroke];
        [nsstr(c->label) drawAtPoint:NSMakePoint(cr.origin.x + 6, cr.origin.y + 2)
                      withAttributes:@{ NSFontAttributeName: [NSFont boldSystemFontOfSize:11],
                                        NSForegroundColorAttributeName: rgb(0xa8a8ad) }];
    }

    /* the parts, the knobs last (they ride over the wedges) */
    for (int i = 0; i < _n; i++) {
        const chf_control *c = &_ctl[i];
        switch (c->kind) {
        case CHF_CTL_BUTTON:
        case CHF_CTL_RESET:
            [self drawButton:c lit:_lit[i]];
            break;
        case CHF_CTL_STICK:
            [self drawDish:c];
            break;
        case CHF_CTL_DIRECTION: {
            NSPoint ctr;
            CGFloat R;
            if ([self stickOfPort:c->port centre:&ctr radius:&R])
                [self drawWedge:c centre:ctr radius:R lit:_lit[i]];
            break;
        }
        case CHF_CTL_TWIST:
            [self drawTwist:c lit:_lit[i]];
            break;
        case CHF_CTL_AXIS:
            [self drawAxis:c lit:_lit[i]];
            break;
        case CHF_CTL_SIDE:
            [self drawSide:c];
            break;
        default:
            break;
        }
    }
    for (int i = 0; i < _n; i++)
        if (_ctl[i].kind == CHF_CTL_STICK) [self drawKnob:&_ctl[i]];

    /* Map mode: the picked control is called out */
    if (self.mapping && self.mapTarget >= 0) {
        for (int i = 0; i < _n; i++) {
            const chf_control *c = &_ctl[i];
            if (c->target != self.mapTarget || c->kind == CHF_CTL_DIRECTION) continue;
            NSBezierPath *ring = [NSBezierPath bezierPathWithRect:NSInsetRect([self rectOf:c], -2, -2)];
            ring.lineWidth = 2;
            [accent setStroke];
            [ring stroke];
        }
    }
}

/* ---- the mouse ---------------------------------------------------------------------- */

- (void)setPointerMask:(unsigned)mask
{
    if (_downPort < 0) return;
    _downMask = mask;
    chfsession_pointer_set(_session, _downPort, mask);
}

- (void)letGo
{
    for (int p = 0; p < 2; p++) chfsession_pointer_set(_session, p, 0);
    if (_downTarget >= 0) chfsession_press(_session, _downTarget, 0);
    _downIdx = -1;
    _downPort = -1;
    _downTarget = -1;
    _downMask = 0;
    _stickDrag = NO;
    _fromCap = NO;
    _moved = NO;
    _wheel = 0;
}

- (void)pressAt:(NSPoint)p pull:(BOOL)pull
{
    if (_downIdx >= 0) return;
    const int idx = chfsession_controls_hit(_machine, (float)((p.x - MARGIN) / UNIT),
                                            (float)((p.y - HEADER_H) / UNIT));
    if (idx < 0 || idx >= _n) return;
    const chf_control *c = &_ctl[idx];

    if (self.mapping) {
        /* Map: a part with a target of its own is picked; a diagonal drives
         * two contacts and the cap none */
        switch (c->kind) {
        case CHF_CTL_BUTTON: case CHF_CTL_RESET: case CHF_CTL_DIRECTION:
        case CHF_CTL_TWIST: case CHF_CTL_AXIS:
            if (c->target >= 0) [self.owner mapPick:c->target];
            break;
        default: break;
        }
        return;
    }

    switch (c->kind) {
    case CHF_CTL_BUTTON:
    case CHF_CTL_RESET:
        _downIdx = idx;
        _downTarget = c->target;
        chfsession_press(_session, c->target, 1);
        break;
    case CHF_CTL_TWIST:
    case CHF_CTL_AXIS:
        _downIdx = idx;
        _downPort = c->port;
        [self setPointerMask:c->act_mask];
        break;
    case CHF_CTL_DIRECTION:
        _downIdx = idx;
        _downPort = c->port;
        _stickDrag = YES;
        _fromCap = NO;
        [self setPointerMask:c->act_mask];
        break;
    case CHF_CTL_STICK:
        /* the cap: pushed, or pulled with a right or Control press; dragging
         * out of it tilts instead */
        _downIdx = idx;
        _downPort = c->port;
        _stickDrag = YES;
        _fromCap = YES;
        _moved = NO;
        _downPt = p;
        _axisBit = 1u << (pull ? CHF_ACT_PULL : CHF_ACT_PUSH);
        [self setPointerMask:_axisBit];
        break;
    default:
        break;
    }
    [self poll];
}

- (void)mouseDown:(NSEvent *)e
{
    const NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    [self pressAt:p pull:([e modifierFlags] & NSEventModifierFlagControl) != 0];
}

- (void)rightMouseDown:(NSEvent *)e
{
    const NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    [self pressAt:p pull:YES];
}

- (void)dragTo:(NSPoint)p
{
    if (_downIdx < 0) return;
    if (_stickDrag) {
        NSPoint ctr;
        CGFloat R;
        if (![self stickOfPort:_downPort centre:&ctr radius:&R]) return;
        unsigned m = chfsession_stick_from_vector((float)(p.x - ctr.x), (float)(p.y - ctr.y), (float)R);
        if (_fromCap) {
            if (!_moved && hypot(p.x - _downPt.x, p.y - _downPt.y) < 4.0) m = _axisBit;
            else _moved = YES;
            if (m == 0) m = _axisBit;
        }
        if (m != _downMask) {
            [self setPointerMask:m];
            [self poll];
        }
        return;
    }
    /* a button, a twist or an axis arrow: dragging off lets go */
    if (!NSPointInRect(p, [self rectOf:&_ctl[_downIdx]])) {
        [self letGo];
        [self poll];
    }
}

- (void)mouseDragged:(NSEvent *)e
{
    [self dragTo:[self convertPoint:[e locationInWindow] fromView:nil]];
}

- (void)rightMouseDragged:(NSEvent *)e
{
    [self dragTo:[self convertPoint:[e locationInWindow] fromView:nil]];
}

- (void)endPress:(NSEvent *)e
{
    (void)e;
    if (_downIdx < 0) return;
    [self letGo];
    [self poll];
}

- (void)mouseUp:(NSEvent *)e { [self endPress:e]; }
- (void)rightMouseUp:(NSEvent *)e { [self endPress:e]; }

/* The wheel and the trackpad twist the controller under the pointer: scrolling
 * down (or right) turns it clockwise, up (or left) counter-clockwise. A wheel
 * click is one notch; a trackpad's points are collected into notches. */
- (void)scrollWheel:(NSEvent *)e
{
    if (self.mapping) return;
    const NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    const int port = [self controllerAt:p];
    if (port < 0) {
        [super scrollWheel:e];
        return;
    }
    /* a fling's momentum is not more turns */
    if ([e momentumPhase] != NSEventPhaseNone) return;
    if ([e phase] == NSEventPhaseBegan) _wheel = 0;
    CGFloat amount = (CGFloat)(-[e scrollingDeltaY] + [e scrollingDeltaX]);
    if (![e hasPreciseScrollingDeltas]) amount *= NOTCH_PT;
    if ((_wheel > 0 && amount < 0) || (_wheel < 0 && amount > 0)) _wheel = 0;
    _wheel += amount;
    int pulses = 0;
    while (fabs(_wheel) >= NOTCH_PT && pulses < MAX_NOTCHES) {
        const int dir = _wheel > 0 ? 1 : -1;
        chfsession_twist_pulse(_session, port, dir);
        _wheel -= dir * NOTCH_PT;
        pulses++;
    }
    if (pulses == MAX_NOTCHES) _wheel = 0;
}
@end

@implementation ChfControllersPanel
- (void)keyDown:(NSEvent *)e { [self.owner panelKeyDown:e]; }
- (void)keyUp:(NSEvent *)e { [self.owner panelKeyUp:e]; }
- (void)flagsChanged:(NSEvent *)e { [self.owner panelFlagsChanged:e]; }
/* Escape is the console's Reset to CONFIG (or cancels a Map pick), not "close
 * the panel". */
- (void)cancelOperation:(id)sender { (void)sender; }
@end

/* ---- the panel ------------------------------------------------------------------- */

@implementation ChfControllersWindow {
    chfsession *_session;
    ChfAppKeyHandler _appKeys;
    ChfControllersPanel *_panel;
    ChfFlippedView *_content;
    ChfConsoleView *_console;
    NSTextField *_padsTitle;
    NSTextField *_noPads;
    NSTextField *_padName[MAX_PAD_ROWS];
    NSTextField *_padEff[MAX_PAD_ROWS];     /* each pad's "drives player N" */
    NSPopUpButton *_padAssign[MAX_PAD_ROWS];
    int _padRows;
    unsigned _padGeneration;
    NSButton *_mapButton;
    NSButton *_defaultsButton;
    NSButton *_showKeysBox;
    NSTextField *_pickLabel;
    NSPopUpButton *_pickPopup;              /* every target, for Map */
    NSButton *_helpButton;
    NSTextField *_hint;
    NSTimer *_timer;
    int _slow;
    int _builtMachine;
    uint32_t _swallowKey;                   /* the key Map took: its repeats and release too */
    /* -2 idle, -1 armed and waiting for a control, >= 0 waiting for a key or
     * gamepad button to bind to that target */
    int _mapState;
    BOOL _placed;
}

- (instancetype)initWithSession:(chfsession *)session appKeys:(ChfAppKeyHandler)appKeys
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _appKeys = [appKeys copy];
    _mapState = -2;
    _builtMachine = -1;
    _padGeneration = (unsigned)-1;

    _panel = [[ChfControllersPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, 700, 560)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskUtilityWindow)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [_panel setTitle:@"Controllers"];
    /* Floats above the machine's window, a full-screen one too. */
    [_panel setLevel:NSFloatingWindowLevel];
    [_panel setCollectionBehavior:NSWindowCollectionBehaviorFullScreenAuxiliary];
    [_panel setReleasedWhenClosed:NO];
    [_panel setBecomesKeyOnlyIfNeeded:NO];
    _panel.owner = self;
    [_panel setDelegate:self];
    ChfSetHelpContext(_panel, @"controllers");

    _content = [[ChfFlippedView alloc] initWithFrame:NSMakeRect(0, 0, 700, 560)];
    [_content setAutoresizesSubviews:NO];
    [_panel setContentView:_content];

    _console = [[ChfConsoleView alloc] initWithFrame:NSZeroRect];
    _console.owner = self;
    _console.session = _session;
    _console.mapTarget = -1;
    _console.showKeys = chfsession_get_int(_session, "controllers_show_bindings", 0) != 0;
    [_content addSubview:_console];

    /* Gamepads: one row per pad, its name, the player it drives and the one it
     * is set to; rows reserved, so pads coming and going keep the size. */
    _padsTitle = [NSTextField labelWithString:@"Gamepads"];
    [_padsTitle setFont:[NSFont boldSystemFontOfSize:12]];
    [_content addSubview:_padsTitle];
    _noPads = [NSTextField labelWithString:@"No gamepads connected"];
    [_noPads setTextColor:[NSColor secondaryLabelColor]];
    [_content addSubview:_noPads];
    for (int i = 0; i < MAX_PAD_ROWS; i++) {
        _padName[i] = [NSTextField labelWithString:@""];
        [_padName[i] setLineBreakMode:NSLineBreakByTruncatingTail];
        [_content addSubview:_padName[i]];
        _padEff[i] = [NSTextField labelWithString:@""];
        [_padEff[i] setTextColor:[NSColor secondaryLabelColor]];
        [_padEff[i] setAlignment:NSTextAlignmentRight];
        [_content addSubview:_padEff[i]];
        /* -1 = automatic (connection order), 0 and 1 = that player */
        _padAssign[i] = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        [_padAssign[i] addItemsWithTitles:@[@"Automatic", @"Player 1", @"Player 2"]];
        _padAssign[i].tag = i;
        _padAssign[i].target = self;
        _padAssign[i].action = @selector(padAssignChanged:);
        [_padAssign[i] setRefusesFirstResponder:YES];
        [_content addSubview:_padAssign[i]];
    }

    _mapButton = [NSButton buttonWithTitle:@"Map" target:self action:@selector(toggleMap:)];
    [_mapButton setRefusesFirstResponder:YES];
    [_content addSubview:_mapButton];
    _defaultsButton = [NSButton buttonWithTitle:@"Reset Controls" target:self action:@selector(restoreDefaults:)];
    [_defaultsButton setRefusesFirstResponder:YES];
    [_content addSubview:_defaultsButton];
    _showKeysBox = [NSButton checkboxWithTitle:@"Show bindings" target:self action:@selector(toggleShowKeys:)];
    [_showKeysBox setState:_console.showKeys ? NSControlStateValueOn : NSControlStateValueOff];
    [_showKeysBox setRefusesFirstResponder:YES];
    [_content addSubview:_showKeysBox];
    _pickLabel = [NSTextField labelWithString:@"Control"];
    [_content addSubview:_pickLabel];
    _pickPopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (int t = 0; t < CHF_TARGET_COUNT; t++)
        [[_pickPopup menu] addItemWithTitle:nsstr(chf_target_name(t)) action:nil keyEquivalent:@""];
    _pickPopup.target = self;
    _pickPopup.action = @selector(pickChanged:);
    [_pickPopup setRefusesFirstResponder:YES];
    [_content addSubview:_pickPopup];
    _helpButton = ChfHelpButton(@"controllers");
    [_helpButton setRefusesFirstResponder:YES];
    [_content addSubview:_helpButton];
    _hint = [NSTextField wrappingLabelWithString:@""];
    [_hint setFont:[NSFont systemFontOfSize:[NSFont smallSystemFontSize]]];
    [_hint setTextColor:[NSColor secondaryLabelColor]];
    [_hint setMaximumNumberOfLines:2];
    [[_hint cell] setTruncatesLastVisibleLine:YES];
    [_content addSubview:_hint];

    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(occlusionChanged:)
                                                 name:NSWindowDidChangeOcclusionStateNotification
                                               object:_panel];

    [self setMapState:-2];
    [self rebuild];
    return self;
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [_timer invalidate];
    [_panel setDelegate:nil];
}

/* ---- the console ------------------------------------------------------------------- */

- (int)machineNow
{
    const int m = chfsession_get_int(_session, "machine", CHF_MACHINE_CHANNELF);
    return m >= 0 && m < CHF_MACHINE_COUNT ? m : CHF_MACHINE_CHANNELF;
}

/* A new content size with the top-left corner where it was. */
- (void)fitPanel:(NSSize)size
{
    const NSRect frame = [_panel frame];
    NSRect content = [_panel contentRectForFrameRect:frame];
    if (NSEqualSizes(content.size, size)) return;
    content.size = size;
    NSRect next = [_panel frameRectForContentRect:content];
    next.origin.x = frame.origin.x;
    next.origin.y = NSMaxY(frame) - next.size.height;
    [_panel setFrame:next display:YES];
}

- (void)relayout
{
    const NSSize cs = _console.contentSize;
    const CGFloat w = cs.width;
    const CGFloat inner = w - 2 * MARGIN;
    const CGFloat padsY = cs.height + MARGIN;
    const CGFloat rowsY = padsY + TITLE_H;
    const CGFloat mapY = rowsY + MAX_PAD_ROWS * ROW_H + 8;
    const CGFloat hintY = mapY + BTN_H + 8;
    const CGFloat total = hintY + 34 + MARGIN;

    [self fitPanel:NSMakeSize(w, total)];
    [_console setFrame:NSMakeRect(0, 0, cs.width, cs.height)];
    [_padsTitle setFrame:NSMakeRect(MARGIN, padsY, 200, 16)];
    [_noPads setFrame:NSMakeRect(MARGIN, rowsY + 4, inner, 18)];
    for (int i = 0; i < MAX_PAD_ROWS; i++) {
        const CGFloat y = rowsY + i * ROW_H;
        [_padName[i] setFrame:NSMakeRect(MARGIN, y + 4, inner - POPUP_W - EFF_W - 16, 18)];
        [_padEff[i] setFrame:NSMakeRect(MARGIN + inner - POPUP_W - EFF_W - 8, y + 4, EFF_W, 18)];
        [_padAssign[i] setFrame:NSMakeRect(MARGIN + inner - POPUP_W, y, POPUP_W, 25)];
    }
    [_mapButton setFrame:NSMakeRect(MARGIN, mapY, 64, BTN_H)];
    [_defaultsButton setFrame:NSMakeRect(MARGIN + 70, mapY, 112, BTN_H)];
    [_showKeysBox setFrame:NSMakeRect(MARGIN + 192, mapY + 3, 120, 22)];
    [_pickLabel setFrame:NSMakeRect(MARGIN + 324, mapY + 5, 50, 18)];
    [_pickPopup setFrame:NSMakeRect(MARGIN + 376, mapY, 190, 25)];
    [_helpButton setFrameOrigin:NSMakePoint(w - MARGIN - _helpButton.frame.size.width, mapY + 2)];
    [_hint setFrame:NSMakeRect(MARGIN, hintY, inner, 34)];
    [_console setNeedsDisplay:YES];
}

- (void)rebuild
{
    [self letGo];
    _builtMachine = [self machineNow];
    [_console configureForMachine:_builtMachine];
    _console.title = nsstr(chf_machine_name(_builtMachine));
    [self relayout];
    [self refreshLabels];
}

/* Lets go of what the mouse holds in this panel. */
- (void)letGo
{
    [_console letGo];
}

/* ---- Map mode --------------------------------------------------------------------- */

- (NSString *)legendForTarget:(int)target
{
    return keyName(chfsession_binding_get(_session, target).keysym);
}

/* What is bound to a target, for the status line. */
- (NSString *)bindingText:(int)target
{
    const chf_binding b = chfsession_binding_get(_session, target);
    NSString *key = keyName(b.keysym);
    NSString *text = key.length ? key : @"no key";
    if (b.button != CHF_PAD_BTN_NONE)
        text = [NSString stringWithFormat:@"%@, pad %@", text, nsstr(chf_pad_button_name(b.button))];
    return text;
}

/* In Map mode nothing is lit and every control shows what is bound to it. */
- (void)refreshLabels
{
    const BOOL mapping = _mapState != -2;
    _console.mapping = mapping;
    _console.mapTarget = _mapState >= 0 ? _mapState : -1;
    [_console setNeedsDisplay:YES];
    [_console poll];
}

- (void)setMapState:(int)state
{
    _mapState = state;
    if (state >= 0) {
        [self letGo];
        chfsession_gamepad_capture_begin(_session);
        _hint.stringValue = [NSString stringWithFormat:@"Press a key or a gamepad button for %@ (now: %@). Esc cancels.",
                             nsstr(chf_target_name(state)), [self bindingText:state]];
    } else {
        chfsession_gamepad_capture_cancel(_session);
        _hint.stringValue = state == -1
            ? @"Click a control, or choose one from the list, to remap it."
            : @"Drag the knob (eight ways), click it to push, right-click or Control-click to pull, scroll to "
              @"twist. Click and hold the buttons and arrows.";
    }
    [_mapButton setTitle:(state == -2 ? @"Map" : @"Done")];
    _mapButton.bezelColor = state == -2 ? nil : rgb(CHFSESSION_ACCENT_RGB);
    _pickPopup.enabled = state != -2;
    if (state >= 0 && state < [_pickPopup numberOfItems]) [_pickPopup selectItemAtIndex:state];
    [self refreshLabels];
}

/* A control clicked on the picture in Map mode. */
- (void)mapPick:(int)target
{
    if (_mapState != -2) [self setMapState:target];
}

- (void)pickChanged:(NSPopUpButton *)sender
{
    if (_mapState == -2) return;
    const NSInteger i = sender.indexOfSelectedItem;
    if (i >= 0 && i < CHF_TARGET_COUNT) [self setMapState:(int)i];
}

- (void)showBound:(NSString *)name target:(int)target stolen:(const char *)stolen
{
    NSString *what = nsstr(chf_target_name(target));
    _hint.stringValue = stolen[0]
        ? [NSString stringWithFormat:@"%@ is now %@ (it was %@)", what, name, nsstr(stolen)]
        : [NSString stringWithFormat:@"%@ is now %@", what, name];
}

- (void)bindKey:(uint32_t)keysym swallow:(BOOL)swallow
{
    char stolen[128];
    stolen[0] = '\0';
    const int target = _mapState;
    chfsession_binding_set_key(_session, target, keysym, stolen, (int)sizeof stolen);
    if (swallow) _swallowKey = keysym;
    [self setMapState:-1];   /* stay armed: remapping several in a row is normal */
    [self showBound:keyName(keysym) target:target stolen:stolen];
}

- (void)pollCapture
{
    int button = CHF_PAD_BTN_NONE;
    char stolen[128];
    if (_mapState < 0 || !chfsession_gamepad_capture_poll(_session, &button)) return;
    stolen[0] = '\0';
    const int target = _mapState;
    chfsession_binding_set_button(_session, target, button, stolen, (int)sizeof stolen);
    [self setMapState:-1];
    [self showBound:nsstr(chf_pad_button_name(button)) target:target stolen:stolen];
}

- (void)toggleMap:(id)sender
{
    (void)sender;
    [self setMapState:(_mapState == -2 ? -1 : -2)];
}

- (void)restoreDefaults:(id)sender
{
    (void)sender;
    chfsession_bindings_reset(_session);
    [self refreshLabels];
    _hint.stringValue = @"Default bindings restored";
}

- (void)toggleShowKeys:(id)sender
{
    (void)sender;
    const BOOL on = [_showKeysBox state] == NSControlStateValueOn;
    chfsession_set_int(_session, "controllers_show_bindings", on ? 1 : 0);
    _console.showKeys = on;
    [_console setNeedsDisplay:YES];
}

/* ---- gamepads ----------------------------------------------------------------------- */

- (void)refreshPadPorts
{
    for (int i = 0; i < _padRows; i++) {
        const int eff = chfsession_gamepad_effective_port(_session, i);
        NSString *text = eff >= 0 ? [NSString stringWithFormat:@"drives player %d", eff + 1] : @"drives nothing";
        if (![_padEff[i].stringValue isEqualToString:text]) _padEff[i].stringValue = text;
    }
}

- (void)refreshPads
{
    const int n = chfsession_gamepad_count(_session);
    _padGeneration = chfsession_gamepad_generation(_session);
    _padRows = n < MAX_PAD_ROWS ? (n > 0 ? n : 0) : MAX_PAD_ROWS;
    _noPads.hidden = n > 0;
    for (int i = 0; i < MAX_PAD_ROWS; i++) {
        const BOOL shown = i < _padRows;
        _padName[i].hidden = !shown;
        _padEff[i].hidden = !shown;
        _padAssign[i].hidden = !shown;
        if (!shown) continue;
        char name[128];
        name[0] = '\0';
        chfsession_gamepad_name(_session, i, name, (int)sizeof name);
        _padName[i].stringValue = nsstr(name);
        _padName[i].toolTip = nsstr(name);
        const int assign = chfsession_gamepad_assignment(_session, i);
        [_padAssign[i] selectItemAtIndex:(assign < 0 || assign > 1 ? 0 : assign + 1)];
    }
    [self refreshPadPorts];
}

- (void)padAssignChanged:(NSPopUpButton *)sender
{
    const int sel = (int)sender.indexOfSelectedItem;
    chfsession_gamepad_assign(_session, (int)sender.tag, sel <= 0 ? -1 : sel - 1);
    [self refreshPadPorts];
}

/* ---- the poll ----------------------------------------------------------------------- */

/* 30 Hz while the panel can be seen: what is held and the gamepad capture;
 * every 250 ms the hot-plugged pads, auto-assignment and a machine change in
 * Settings. */
- (void)tick
{
    if (_mapState >= 0) [self pollCapture];
    if (++_slow >= SLOW_TICKS) {
        _slow = 0;
        if (chfsession_gamepad_generation(_session) != _padGeneration) [self refreshPads];
        else [self refreshPadPorts];
        if ([self machineNow] != _builtMachine) [self rebuild];
    }
    [_console poll];
}

/* The timer runs only while the panel is on screen and not occluded or
 * hidden with its app. */
- (void)updateTimer
{
    const BOOL want = [_panel isVisible] && ([_panel occlusionState] & NSWindowOcclusionStateVisible);
    if (want && !_timer) {
        __weak ChfControllersWindow *weakSelf = self;
        _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30 repeats:YES block:^(NSTimer *t) {
            (void)t;
            [weakSelf tick];
        }];
        _timer.tolerance = 0.005;
    } else if (!want && _timer) {
        [_timer invalidate];
        _timer = nil;
    }
}

- (void)occlusionChanged:(NSNotification *)note
{
    (void)note;
    [self updateTimer];
}

/* ---- the keyboard ------------------------------------------------------------------- */

- (void)panelKeyDown:(NSEvent *)e
{
    if ([e modifierFlags] & NSEventModifierFlagCommand) return;
    const uint32_t ks = ChfKeysymFromEvent(e);
    if (!ks) return;
    if (_swallowKey && ks == _swallowKey) return;
    if ([e isARepeat]) return;
    if (ks == CHF_KEYSYM_F9 || ks == CHF_KEYSYM_F11 || ks == CHF_KEYSYM_F12) {
        if (_appKeys) _appKeys(ks);
        return;
    }
    if (_mapState >= 0) {
        /* the next key is the binding; Esc backs out of the pick */
        if (ks == CHF_KEYSYM_ESCAPE) [self setMapState:-1];
        else [self bindKey:ks swallow:YES];
        return;
    }
    if (_mapState == -1) return;    /* armed: typing does nothing */
    /* a key bound to a system action goes to the app's queue, as a gamepad's does */
    const int sa = chfsession_key_sysaction(_session, ks);
    if (sa >= 0) {
        chfsession_sysaction_post(_session, sa);
        return;
    }
    chfsession_key(_session, ks, 1);
}

- (void)panelKeyUp:(NSEvent *)e
{
    const uint32_t ks = ChfKeysymFromEvent(e);
    if (!ks) return;
    if (_swallowKey && ks == _swallowKey) {
        _swallowKey = 0;
        return;
    }
    chfsession_key(_session, ks, 0);
}

/* Shift, Control and Option bind too (Command is the menus'); otherwise
 * they are the console's. A release always goes through. */
- (void)panelFlagsChanged:(NSEvent *)e
{
    int down = 0;
    const uint32_t ks = ChfKeysymFromFlagsChange(e, &down);
    if (!ks) return;
    if (_mapState >= 0 && down) {
        [self bindKey:ks swallow:NO];
        return;
    }
    if (_mapState != -2 && down) return;
    chfsession_key(_session, ks, down);
}

/* ---- the window --------------------------------------------------------------------- */

/* Hidden: Map disarmed, nothing held, no polling. */
- (void)quiesce
{
    [self setMapState:-2];
    [self letGo];
    _swallowKey = 0;
    [_timer invalidate];
    _timer = nil;
}

- (void)show
{
    if ([self machineNow] != _builtMachine) [self rebuild];
    [self refreshPads];
    [self setMapState:-2];
    if (!_placed) {
        [_panel center];
        _placed = YES;
    }
    [_panel makeFirstResponder:nil];    /* the panel itself takes the keys */
    [_panel makeKeyAndOrderFront:nil];
    [self updateTimer];
}

- (void)hide
{
    [_panel orderOut:nil];
    [self quiesce];
}

- (void)toggle
{
    if ([self isVisible]) [self hide];
    else [self show];
}

- (BOOL)isVisible
{
    return [_panel isVisible];
}

- (void)refresh
{
    if ([self isVisible]) {
        [self rebuild];
        [self refreshPads];
    } else {
        _builtMachine = -1;     /* rebuilt when next shown */
    }
}

/* Only what this panel's mouse holds: the main window releases its own. */
- (void)windowDidResignKey:(NSNotification *)note
{
    (void)note;
    [self letGo];
    _swallowKey = 0;    /* its release goes elsewhere */
}

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    [self quiesce];
}
@end
