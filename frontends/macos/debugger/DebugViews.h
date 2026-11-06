/*
 * Small views and builders the debugger's tabs share: a picture view with
 * nearest-neighbour scaling and a pointer, a text view that reports clicks on
 * its lines, and the stack and label helpers the tabs are laid out with.
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include <stdint.h>

/* An image of XRGB pixels (copied), +1 retained; NULL when the size is empty. */
CGImageRef ChfNewImage(const uint32_t *px, int w, int h);

/* A monospaced, read-only text view in its own scroll view; wrap: wraps at the
 * view's width instead of scrolling sideways. */
NSTextView *ChfMonoView(NSScrollView **scrollOut, BOOL wrap);
/* Replace a text view's text, keeping the reader's place. */
void ChfSetTextKeepingPlace(NSTextView *view, NSString *text);
/* Whether a view's reader is at its end (a log then follows new lines). */
BOOL ChfTextAtEnd(NSTextView *view);
/* A log's text where its reader left it, or, following, at its end. */
void ChfSetLogText(NSTextView *view, NSString *text, BOOL follow);
/* C text without its control characters (but for newline and tab). */
NSString *ChfStripControl(const char *s);

NSStackView *ChfHStack(NSArray<NSView *> *views);
NSStackView *ChfVStack(NSArray<NSView *> *views);
/* The view as wide as the stack. */
void ChfFill(NSStackView *stack, NSView *view);
/* A drawn view at its own intrinsic size. */
void ChfFixSize(NSView *view);

NSTextField *ChfLabel(NSString *text);
/* Secondary-coloured small text. */
NSTextField *ChfNote(NSString *text);
/* Bold section title. */
NSTextField *ChfTitle(NSString *text);
NSTextField *ChfField(NSString *placeholder, CGFloat width, id target, SEL action);
NSButton *ChfButton(NSString *title, id target, SEL action);

/* A picture at the largest whole multiple that fits (nearest neighbour, so
 * every pixel of these small pictures is the same size), letterboxed. The
 * pointer is reported in the picture's own pixels. */
@interface ChfPictureView : NSView
/* Takes ownership of the +1 image. */
- (void)setImage:(CGImageRef)image;
/* An outline in picture pixels (x, y, w, h from the top left); empty: none. */
@property (nonatomic) NSRect highlight;
/* The picture pixel under the pointer, or -1, -1 when it is outside. */
@property (nonatomic, copy) void (^onHover)(int x, int y);
/* A press on a picture pixel (clicks: 1, 2 for a double click); not called
 * for a press outside the picture. */
@property (nonatomic, copy) void (^onClick)(int x, int y, int clicks);
@end

/* A text view whose lines can be clicked (once, or twice), and whose wheel can
 * be taken over. */
@interface ChfLineTextView : NSTextView
@property (nonatomic, copy) void (^onClickLine)(int line);
@property (nonatomic, copy) void (^onDoubleClickLine)(int line);
@property (nonatomic, copy) void (^onScroll)(int lines);
@end
