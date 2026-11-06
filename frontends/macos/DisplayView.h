/*
 * The emulator display.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "chfsession.h"

@interface ChfDisplayView : NSView
- (instancetype)initWithSession:(chfsession *)session;
/* The shape the 204x232 picture is shown at: chfsession_display_aspect for
 * this machine (the TV's 4:3, setting "aspect" 0, the default; or square
 * pixels, 1). Call again when the machine changes. */
- (void)setMachine:(int)machine squarePixels:(BOOL)square;
- (void)setSmooth:(BOOL)smooth;
/* Starts or stops the frame clock to match whether the window can be seen
 * (on screen, not occluded, not miniaturized, the app not hidden). It follows
 * the window's own notifications; call it after ordering the window in. */
- (void)updateVisibility;
- (void)stop;
@end
