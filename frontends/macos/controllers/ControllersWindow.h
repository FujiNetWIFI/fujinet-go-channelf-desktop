/*
 * The Controllers panel (F9 / Cmd+J): the Channel F's console buttons and its
 * two hand controllers drawn from the session's layout (chfsession_controls_*),
 * the gamepads with their assignments, and Map mode for rebinding. Hidden at
 * start; a fixed-size floating NSPanel.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "chfsession.h"

/* The keys the app itself owns (F9, F11, F12): called with the keysym of one
 * typed with the panel key; returns YES when it took it. */
typedef BOOL (^ChfAppKeyHandler)(uint32_t keysym);

@interface ChfControllersWindow : NSObject
- (instancetype)initWithSession:(chfsession *)session appKeys:(ChfAppKeyHandler)appKeys;
/* Show, or hide when showing. */
- (void)toggle;
- (void)hide;
- (BOOL)isVisible;
/* The machine changed (Settings): the panel is rebuilt for it. */
- (void)refresh;
@end
