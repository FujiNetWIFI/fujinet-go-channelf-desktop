/*
 * The Settings window: Machine, Display & Audio, Input, FujiNet and ROMs
 * pages, the same settings (keys and defaults) as the other frontends'
 * Preferences.
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
/* The ROMs page's Import… (folder NO) and Import a Folder… (folder YES). */
- (void)settingsImportRoms:(BOOL)folder;
/* The window closed. needsRestart: a machine or host option changed, so the
 * console is power-cycled (chfsession_restart) and a toast says so. */
- (void)settingsClosed:(BOOL)needsRestart;
@end

@interface ChfSettingsWindow : NSObject <NSWindowDelegate, NSTabViewDelegate>
- (instancetype)initWithSession:(chfsession *)session delegate:(id<ChfSettingsDelegate>)delegate;
- (void)show;
/* The machine and BIOS notes, the Web UI line and the ROM states, from the
 * session (after a restart or an import). */
- (void)refresh;
/* The View menu changed the picture: the popup and the box follow. */
- (void)syncDisplay;
@end
