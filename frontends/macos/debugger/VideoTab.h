/*
 * The debugger's Video tab: everything the Channel F's video knows.
 *
 * Left, the picture at a chosen scale in one of six views -- the last frame,
 * the VRAM drawn now, the frame as far as the beam has got, the whole 128 x 64
 * VRAM, its raw values, the four palettes -- with the overlay checkboxes and
 * the cell under the pointer read out (click pins it, double-click breaks on
 * writes to it). Right, in sections: the 64-row palette strip and the 4 x 4
 * palette table, the four ports (raw, complement, decoded, and as the CPU
 * would read them now), and the VRAM write log with its filters and counts.
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
/* Pulls the video's state and redraws what shows. */
- (void)refresh;
@end
