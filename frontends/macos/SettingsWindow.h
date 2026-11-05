/*
 * The Settings window: Machine, Display, Audio, Input and FujiNet pages,
 * the same settings (keys and defaults) as the other frontends' Preferences.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "chfsession.h"

/* What the window needs the app to do for it. */
@protocol ChfSettingsDelegate <NSObject>
/* Live display changes (the app's menu and view follow). */
- (void)settingsSetAspect:(int)aspect;
- (void)settingsSetSmooth:(BOOL)smooth;
- (void)settingsOpenWebUI;
/* The window closed. needsRestart: a machine or host option changed, so the
 * console is power-cycled (chfsession_restart) and a toast says so. */
- (void)settingsClosed:(BOOL)needsRestart;
@end

@interface ChfSettingsWindow : NSObject <NSWindowDelegate>
- (instancetype)initWithSession:(chfsession *)session delegate:(id<ChfSettingsDelegate>)delegate;
- (void)show;
/* The machine note and the Web UI line, from the session (after a restart). */
- (void)refresh;
/* The View menu changed the picture: the popup and the box follow. */
- (void)syncDisplay;
@end
