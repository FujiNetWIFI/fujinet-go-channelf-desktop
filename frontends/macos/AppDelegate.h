/*
 * The application delegate: the window, the menu bar, and the session.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "chfsession.h"

@interface ChfAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
/* mediaPath: a file named on the command line (a cartridge or
 * anything for the SD folder), or NULL. */
- (instancetype)initWithSession:(chfsession *)session mediaPath:(const char *)mediaPath;
@end
