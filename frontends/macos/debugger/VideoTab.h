/*
 * The debugger's Video tab: everything the 2637 UVI knows.
 *
 * Left, the picture the chip draws -- the last frame, the registers
 * re-rendered with the layers and overlays you choose, or the frame as far as
 * the beam has got -- with the object or character cell under the pointer
 * named and outlined. Right, the chip in sections: the four objects with their
 * positions and bitmaps, the 16 x 13 (or 26) character screen with its colour
 * bits, every register $1800-$1AFF decoded (editable while the machine is
 * stopped), the control, collision, sound and colour registers, and the glyph
 * (UDC), object and palette sheets.
 *
 * Refreshed by the debugger window: live while the machine runs (at most ten
 * times a second) and on every stop, only while this tab shows.
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "chfdebug.h"

@interface ChfVideoTab : NSObject <NSTabViewDelegate>
- (instancetype)initWithDebugger:(chfdebug *)dbg;
/* The tab's content, to put in the debugger's tab view. */
@property (nonatomic, readonly) NSView *view;
/* Pulls the chip's state and redraws what shows. */
- (void)refresh;
@end
