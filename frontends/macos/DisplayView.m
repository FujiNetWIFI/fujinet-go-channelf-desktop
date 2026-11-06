/*
 * ChfDisplayView -- the framebuffer, as a Core Animation layer fed from a
 * CVDisplayLink.
 *
 * CVDisplayLink is this platform's frame clock -- the equivalent of
 * GdkFrameClock and DwmFlush -- and feeding it to the session is what lets
 * the emulator phase-lock to the panel instead of beating against it.
 *
 * The energy rules this view follows, as the other frontends do:
 *
 *   - It is LAYER-BACKED and has no drawRect:. The picture is a CGImage set
 *     as the contents of a sublayer, which the compositor scales (nearest
 *     neighbour, or linear for Smooth) on the GPU: nothing is rasterised on
 *     the CPU per frame and AppKit allocates no backing store for the view.
 *   - The display link runs ONLY while the window can be seen: on screen, not
 *     occluded, not miniaturized, the app not hidden. A window nobody can
 *     see costs no ticks at all; the machine itself keeps running (netplay and
 *     audio go on) and paces on the wall clock until the link returns.
 *   - A tick redraws only when the session published a new frame (the serial
 *     chfsession_copy_frame keeps): a static CONFIG screen does no image, no
 *     dispatch and no commit, whatever the link's rate.
 *   - Pixels go into a small POOL of reused buffers. Each frame's CGImage is
 *     backed by a CGDataProvider over one pool buffer, and the provider's
 *     release callback returns the buffer to the pool when Core Animation has
 *     let go of the image -- so a buffer is never rewritten while the
 *     compositor may still read it, and no frame allocates pixel memory.
 *
 * The link's callback runs on its OWN high-priority thread, not the main one.
 * It hands over the tick, pulls the frame into a pool buffer and wraps it in
 * an image; the main thread only swaps that image in as the layer's contents.
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DisplayView.h"

#import <CoreVideo/CoreVideo.h>
#import <QuartzCore/QuartzCore.h>

/* CoreVideo hands out host time in mach_absolute_time units, which are NOT
 * nanoseconds on every machine -- the timebase ratio converts them. */
#include <mach/mach_time.h>

#include <stdlib.h>
#include <string.h>

/* The picture is 204 x 232 (MAME's visible area: 102 x 58 cells, 2 pixels wide
 * and 4 lines tall), shown at the television's 4:3 or with square pixels;
 * chfsession_display_aspect says which shape for the machine. */
#define TV_ASPECT (4.0 / 3.0)

/* Buffers in flight: one being filled, one waiting for the main thread, one
 * or two held by the compositor. */
#define POOL_SIZE 4

#define FRAME_BYTES ((size_t)CHFSESSION_FB_WIDTH * CHFSESSION_FB_HEIGHT * 4)

/* One frame's pixels. `busy` (guarded by @synchronized on the class) is set
 * while the buffer is filling, queued, or referenced by an image. */
@interface ChfFrameBuffer : NSObject
@property (nonatomic, readonly) uint32_t *pixels;
@property (nonatomic) BOOL busy;
@end

@implementation ChfFrameBuffer
- (instancetype)init
{
    self = [super init];
    if (!self) return nil;
    _pixels = calloc(1, FRAME_BYTES);
    return self;
}

- (void)dealloc
{
    free(_pixels);
}
@end

/* The image's data provider lets go: the buffer is free again. The provider
 * owned one retain of the buffer object (see newImageFor:), taken back here,
 * so the pixels outlive both the pool and the view while an image is alive. */
static void releaseFrame(void *info, const void *data, size_t size)
{
    (void)data;
    (void)size;
    ChfFrameBuffer *buf = (__bridge_transfer ChfFrameBuffer *)info;
    @synchronized ([ChfFrameBuffer class]) {
        buf.busy = NO;
    }
}

@implementation ChfDisplayView {
    chfsession *_session;
    CVDisplayLinkRef _link;          /* NULL until first wanted; Start/Stop after */
    BOOL _linkRunning;               /* main thread */
    NSArray<ChfFrameBuffer *> *_pool;
    uint64_t _serial;                /* the link thread's: the last frame copied */
    CGColorSpaceRef _cs;
    CALayer *_picture;
    double _aspect;                  /* width / height of the picture's box */
    BOOL _smooth;
    NSLock *_lock;                   /* guards _pending and _posted */
    CGImageRef _pending;             /* the newest image the main thread has not shown */
    BOOL _posted;                    /* a main-thread block is queued to show it */
}

static CVReturn displayCallback(CVDisplayLinkRef link, const CVTimeStamp *now,
                                const CVTimeStamp *out, CVOptionFlags flagsIn,
                                CVOptionFlags *flagsOut, void *ctx)
{
    (void)link; (void)out; (void)flagsIn; (void)flagsOut;
    ChfDisplayView *self = (__bridge ChfDisplayView *)ctx;
    [self tick:now->hostTime];
    return kCVReturnSuccess;
}

- (instancetype)initWithSession:(chfsession *)session
{
    /* 2x the 232 lines at the TV's 4:3 is 619 wide: a comfortable first
     * window on any Mac is a little more */
    self = [super initWithFrame:NSMakeRect(0, 0, 699, 524)];
    if (!self) return nil;
    _session = session;
    _aspect = TV_ASPECT;
    _smooth = NO;
    _lock = [[NSLock alloc] init];
    _cs = CGColorSpaceCreateDeviceRGB();

    NSMutableArray<ChfFrameBuffer *> *pool = [NSMutableArray array];
    for (int i = 0; i < POOL_SIZE; i++) [pool addObject:[[ChfFrameBuffer alloc] init]];
    _pool = pool;

    [self setWantsLayer:YES];
    _picture = [CALayer layer];
    _picture.contentsGravity = kCAGravityResize;
    _picture.magnificationFilter = kCAFilterNearest;
    _picture.minificationFilter = kCAFilterLinear;
    _picture.opaque = YES;
    [[self layer] addSublayer:_picture];
    [self positionPicture];
    return self;
}

/* The root layer: black, the letterbox. */
- (CALayer *)makeBackingLayer
{
    CALayer *root = [CALayer layer];
    root.backgroundColor = [[NSColor blackColor] CGColor];
    return root;
}

- (BOOL)isOpaque { return YES; }

- (void)dealloc
{
    [self stop];
    if (_cs) CGColorSpaceRelease(_cs);
}

/* ---- the picture's place ---------------------------------------------------- */

/* Letterboxed at the chosen shape. */
- (void)positionPicture
{
    if (!_picture) return;
    const NSRect b = [self bounds];
    const double want = _aspect;
    const double w = b.size.width, h = b.size.height;
    if (w <= 0 || h <= 0) return;
    double sw, sh;
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _picture.frame = CGRectMake(floor((w - sw) / 2), floor((h - sh) / 2), sw, sh);
    [CATransaction commit];
}

- (void)setFrameSize:(NSSize)size
{
    [super setFrameSize:size];
    [self positionPicture];
}

/* AppKit does not promise setFrame: goes through setFrameSize:. */
- (void)setFrame:(NSRect)frame
{
    [super setFrame:frame];
    [self positionPicture];
}

- (void)setMachine:(int)machine squarePixels:(BOOL)square
{
    int num = 4, den = 3;
    chfsession_display_aspect(machine, square ? 1 : 0, &num, &den);
    _aspect = den > 0 && num > 0 ? (double)num / (double)den : TV_ASPECT;
    [self positionPicture];
}

- (void)setSmooth:(BOOL)smooth
{
    _smooth = smooth;
    _picture.magnificationFilter = smooth ? kCAFilterLinear : kCAFilterNearest;
}

/* ---- the frame clock: only while the window can be seen --------------------- */

- (BOOL)shouldRun
{
    NSWindow *w = [self window];
    if (!w || ![w isVisible] || [w isMiniaturized]) return NO;
    if (!([w occlusionState] & NSWindowOcclusionStateVisible)) return NO;
    if ([NSApp isHidden]) return NO;
    return YES;
}

/* The panel the window is on: the link follows it, so its ticks come at that
 * panel's refresh. */
- (void)retargetLink
{
    NSScreen *screen = [[self window] screen];
    NSNumber *num = [[screen deviceDescription] objectForKey:@"NSScreenNumber"];
    if (num && _link) CVDisplayLinkSetCurrentCGDisplay(_link, (CGDirectDisplayID)[num unsignedIntValue]);
}

- (void)startLink
{
    if (!_link) {
        if (CVDisplayLinkCreateWithActiveCGDisplays(&_link) != kCVReturnSuccess) {
            _link = NULL;
            return;
        }
        CVDisplayLinkSetOutputCallback(_link, displayCallback, (__bridge void *)self);
    }
    [self retargetLink];
    if (!_linkRunning) {
        CVDisplayLinkStart(_link);
        _linkRunning = YES;
    }
}

- (void)stopLink
{
    if (_link && _linkRunning) CVDisplayLinkStop(_link);
    _linkRunning = NO;
}

- (void)updateVisibility
{
    if ([self shouldRun]) [self startLink];
    else [self stopLink];
}

- (void)visibilityChanged:(NSNotification *)note
{
    /* a closing window is still "visible" while the notice is delivered */
    if ([[note name] isEqualToString:NSWindowWillCloseNotification]) {
        [self stopLink];
        return;
    }
    [self updateVisibility];
}

- (void)viewDidMoveToWindow
{
    [super viewDidMoveToWindow];
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    [nc removeObserver:self];
    NSWindow *w = [self window];
    if (w) {
        NSArray<NSString *> *names = @[
            NSWindowDidChangeOcclusionStateNotification,
            NSWindowDidMiniaturizeNotification,
            NSWindowDidDeminiaturizeNotification,
            NSWindowDidChangeScreenNotification,
            NSWindowWillCloseNotification,
        ];
        for (NSString *name in names)
            [nc addObserver:self selector:@selector(visibilityChanged:) name:name object:w];
        /* Cmd+H hides every window without minimising them */
        [nc addObserver:self selector:@selector(visibilityChanged:)
                   name:NSApplicationDidHideNotification object:nil];
        [nc addObserver:self selector:@selector(visibilityChanged:)
                   name:NSApplicationDidUnhideNotification object:nil];
    }
    [self updateVisibility];
}

/* Final teardown: the link, the observers, any frame still waiting. */
- (void)stop
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [self stopLink];
    if (_link) {
        CVDisplayLinkRelease(_link);
        _link = NULL;
    }
    [_lock lock];
    CGImageRef old = _pending;
    _pending = NULL;
    [_lock unlock];
    if (old) CGImageRelease(old);
}

/* ---- the link's thread -------------------------------------------------------- */

/* A free buffer, marked busy, or nil when the compositor still holds them all
 * (the tick is then skipped: the frame is copied on the next one). */
- (ChfFrameBuffer *)takeBuffer
{
    @synchronized ([ChfFrameBuffer class]) {
        for (ChfFrameBuffer *b in _pool) {
            if (!b.busy) {
                b.busy = YES;
                return b;
            }
        }
    }
    return nil;
}

/* The image over a buffer's pixels (+1). The provider keeps the buffer alive
 * and frees it for reuse when the image's last owner lets go. */
- (CGImageRef)newImageFor:(ChfFrameBuffer *)buf
{
    void *info = (void *)CFBridgingRetain(buf);
    CGDataProviderRef dp = CGDataProviderCreateWithData(info, [buf pixels], FRAME_BYTES, releaseFrame);
    if (!dp) {
        CFRelease(info);     /* the caller frees the buffer's slot */
        return NULL;
    }
    /* The session's pixels are 0x00RRGGBB in host order: a little-endian
     * 32-bit image with the alpha byte skipped reads them as they are. */
    CGImageRef img = CGImageCreate(CHFSESSION_FB_WIDTH, CHFSESSION_FB_HEIGHT, 8, 32,
                                   (size_t)CHFSESSION_FB_WIDTH * 4, _cs,
                                   (CGBitmapInfo)kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little,
                                   dp, NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(dp);
    return img;
}

- (void)tick:(uint64_t)hostTime
{
    @autoreleasepool {
        static double toNs = 0.0;
        if (toNs == 0.0) {
            mach_timebase_info_data_t tb;
            mach_timebase_info(&tb);
            toNs = (double)tb.numer / (double)tb.denom;
        }
        chfsession_notify_vsync(_session, (int64_t)((double)hostTime * toNs));

        /* Only a new frame is worth a buffer, an image and a trip to the main
         * thread: copy_frame says nothing changed by returning 0. */
        ChfFrameBuffer *buf = [self takeBuffer];
        if (!buf) return;
        if (!chfsession_copy_frame(_session, [buf pixels], &_serial)) {
            @synchronized ([ChfFrameBuffer class]) {
                buf.busy = NO;
            }
            return;
        }
        CGImageRef img = [self newImageFor:buf];
        if (!img) {
            @synchronized ([ChfFrameBuffer class]) {
                buf.busy = NO;
            }
            return;
        }

        /* The newest frame wins: one still waiting for the main thread is
         * dropped, and only one block is ever queued. */
        [_lock lock];
        CGImageRef old = _pending;
        _pending = img;
        const BOOL post = !_posted;
        _posted = YES;
        [_lock unlock];
        if (old) CGImageRelease(old);
        if (post) dispatch_async(dispatch_get_main_queue(), ^{ [self present]; });
    }
}

/* ---- the main thread ------------------------------------------------------------ */

- (void)present
{
    [_lock lock];
    CGImageRef img = _pending;
    _pending = NULL;
    _posted = NO;
    [_lock unlock];
    if (!img) return;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _picture.contents = (__bridge id)img;
    [CATransaction commit];
    CGImageRelease(img);
}
@end
