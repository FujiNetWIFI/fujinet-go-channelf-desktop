/*
 * The AppKit debugger window over the Fairchild F8 (F3850) / Channel F video
 * debugger engine (chfdebug.h).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "chfsession.h"

@interface ChfDebuggerWindow : NSObject <NSWindowDelegate, NSTextFieldDelegate>
/* Shows (creating on first use) the debugger for the session; attaching the
 * engine stops the machine, as the other frontends do. */
+ (void)showForSession:(chfsession *)session;
/* F12: shows it, or closes it (detaching, so the machine runs on). */
+ (void)toggleForSession:(chfsession *)session;
@end
