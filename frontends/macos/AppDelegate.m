/*
 * ChfAppDelegate -- the window, the menu bar and the session's lifetime.
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "AppDelegate.h"

#import <QuartzCore/QuartzCore.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import "DisplayView.h"
#import "KeyForward.h"
#import "SettingsWindow.h"
#import "controllers/ControllersWindow.h"
#import "debugger/DebuggerWindow.h"
#import "help/HelpWindow.h"

#include <stdlib.h>
#include <string.h>

#ifndef CHF_VERSION_STRING
#define CHF_VERSION_STRING "unknown"
#endif

#define APP_TITLE @"FujiNet Go Channel F"

#define WEBSITE_URL "https://fujinet.online/"
#define ISSUES_URL  "https://github.com/FujiNetWIFI/fujinet-go-channelf-desktop/issues"

/* How long a gamepad connect/disconnect or import message stays over the
 * picture. */
#define TOAST_SECONDS 3.0

static NSString *nsstr(const char *s)
{
    NSString *r = s ? [NSString stringWithUTF8String:s] : nil;
    return r ? r : @"";
}

/* The open panels' filters: a type per extension (a dynamic one for an
 * extension nothing has registered, which still matches by extension). */
static NSArray<UTType *> *typesForExtensions(NSArray<NSString *> *exts)
{
    NSMutableArray<UTType *> *types = [NSMutableArray array];
    for (NSString *ext in exts) {
        UTType *t = [UTType typeWithFilenameExtension:ext];
        if (t) [types addObject:t];
    }
    return types;
}

@class ChfAppDelegate;

/* The content view sits between AppKit and the session: it forwards keys
 * and the drop, and leaves the display purely about pixels. */
@interface ChfContentView : NSView
@property (nonatomic) chfsession *session;
@property (nonatomic, weak) ChfAppDelegate *owner;
@end

@interface ChfAppDelegate () <ChfSettingsDelegate, NSAlertDelegate>
- (void)runSysaction:(int)sa;
- (void)loadMediaPaths:(NSArray<NSString *> *)paths;
- (BOOL)handleAppKey:(uint32_t)keysym;
- (void)importRomsFromPanel:(BOOL)folder;
@end

@implementation ChfContentView

- (BOOL)acceptsFirstResponder { return YES; }

- (void)keyDown:(NSEvent *)e
{
    if ([e isARepeat]) return;
    /* Cmd shortcuts are the menus'. One no menu claims still arrives here,
     * but AppKit sends no keyUp while Command is down, so forwarding it
     * would leave the machine holding the key. */
    if ([e modifierFlags] & NSEventModifierFlagCommand) return;
    const uint32_t ks = ChfKeysymFromEvent(e);
    if (!ks) return;
    if ([self.owner handleAppKey:ks]) return;
    chfsession_key(self.session, ks, 1);
}

- (void)keyUp:(NSEvent *)e
{
    const uint32_t ks = ChfKeysymFromEvent(e);
    if (ks) chfsession_key(self.session, ks, 0);
}

/* Modifier keys arrive as flagsChanged, not keyDown/keyUp; a binding moved
 * onto Shift, Control or Option would otherwise never press. */
- (void)flagsChanged:(NSEvent *)e
{
    int down = 0;
    const uint32_t ks = ChfKeysymFromFlagsChange(e, &down);
    if (ks) chfsession_key(self.session, ks, down);
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
    (void)sender;
    return NSDragOperationCopy;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
    NSArray *urls = [[sender draggingPasteboard] readObjectsForClasses:@[[NSURL class]]
        options:@{ NSPasteboardURLReadingFileURLsOnlyKey: @YES }];
    if ([urls count] == 0) return NO;
    NSMutableArray<NSString *> *paths = [NSMutableArray array];
    for (NSURL *u in urls) [paths addObject:[u path]];
    /* after the drag session: what the drop does can be an alert */
    dispatch_async(dispatch_get_main_queue(), ^{ [self.owner loadMediaPaths:paths]; });
    return YES;
}
@end

@implementation ChfAppDelegate {
    chfsession *_session;
    const char *_mediaPath;
    NSString *_pendingMedia;     /* a file Finder handed over before launch finished */
    BOOL _launched;
    NSWindow *_window;
    ChfDisplayView *_display;
    ChfContentView *_content;
    NSTimer *_statusTimer;
    NSTimer *_sysactTimer;

    /* the transient message over the picture */
    NSTextField *_toast;
    unsigned _toastGeneration;
    NSUInteger _toastSerial;

    NSMenuItem *_aspectItem;
    NSMenuItem *_smoothItem;
    int _machine;                /* the machine the console was started as */

    BOOL _needsBiosShown;        /* the "Needs BIOS" alert, once per occurrence */
    NSString *_refusedCart;      /* a game refused for want of the BIOS: opened again after the import */

    /* an open panel with an "All files" choice, while it runs */
    NSOpenPanel *_panel;
    NSArray<UTType *> *_panelTypes;

    ChfSettingsWindow *_settings;
    ChfControllersWindow *_controllers;

    NSWindow *_logWindow;
    NSTextView *_logView;
    NSTimer *_logTimer;
}

- (instancetype)initWithSession:(chfsession *)session mediaPath:(const char *)mediaPath
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _mediaPath = mediaPath;
    return self;
}

- (NSString *)lastError
{
    const char *e = chfsession_last_error(_session);
    return (e && e[0]) ? nsstr(e) : @"The machine is not running.";
}

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    (void)note;

    /* The 204x232 picture at the TV's 4:3: 699x524 is a comfortable first
     * window on any Mac. */
    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 699, 524)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [_window setTitle:APP_TITLE];
    /* NSWindow's default is to release itself on close, which ARC then does
     * a second time: the window is kept, and closing it quits (below) */
    [_window setReleasedWhenClosed:NO];
    [_window setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];
    [_window center];

    _content = [[ChfContentView alloc] initWithFrame:[[_window contentView] bounds]];
    _content.session = _session;
    _content.owner = self;
    [_content setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [_content registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
    /* layer-backed, so the display's layer and the labels over it composite
     * in the order they were added */
    [_content setWantsLayer:YES];

    _display = [[ChfDisplayView alloc] initWithSession:_session];
    [_display setFrame:[_content bounds]];
    [_display setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    _machine = [self machineSetting];
    [_display setMachine:_machine squarePixels:chfsession_get_int(_session, "aspect", 0) != 0];
    [_display setSmooth:chfsession_get_int(_session, "smooth", 0) != 0];
    [_content addSubview:_display];

    /* The toast: a label over the top of the picture that fades. */
    _toast = [NSTextField labelWithString:@""];
    _toast.font = [NSFont boldSystemFontOfSize:13];
    _toast.textColor = NSColor.whiteColor;
    _toast.drawsBackground = YES;
    _toast.backgroundColor = [NSColor colorWithWhite:0.0 alpha:0.7];
    _toast.alignment = NSTextAlignmentCenter;
    _toast.alphaValue = 0.0;
    [_toast setFrame:NSMakeRect(0, [_content bounds].size.height - 34, [_content bounds].size.width, 26)];
    [_toast setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
    [_content addSubview:_toast];

    [_window setContentView:_content];
    ChfSetHelpContext(_window, @"main");
    [self buildMenu];
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_content];
    [_window setDelegate:self];
    [_display updateVisibility];

    __weak ChfAppDelegate *weakSelf = self;
    _controllers = [[ChfControllersWindow alloc] initWithSession:_session
                                                        appKeys:^BOOL(uint32_t ks) {
        return [weakSelf handleAppKey:ks];
    }];

    /* A file from the command line or from Finder is routed as a drop is: it
     * waits for the machine to be up, then a cartridge boots, anything else goes to the SD folder. */
    NSString *media = _pendingMedia;
    if (!media && _mediaPath)
        media = [[NSFileManager defaultManager] stringWithFileSystemRepresentation:_mediaPath
                                                                            length:strlen(_mediaPath)];
    _pendingMedia = nil;

    chfsession_start_opts opts;
    chfsession_default_opts(_session, &opts);
    const BOOL started = chfsession_start(_session, &opts) == 0;
    _machine = opts.machine;
    [self applyDisplayShape];
    _launched = YES;
    _toastGeneration = chfsession_gamepad_generation(_session);
    if (!started) [self reportRefusal:[self lastError] title:@"Could not start"];
    if (media) [self loadMediaPaths:@[media]];

    /* The family's launch hooks, for when the app misbehaves before a menu
     * is reachable. */
    if (getenv("CHF_OPEN_CONTROLLERS")) [_controllers toggle];
    if (getenv("CHF_OPEN_DEBUGGER")) [ChfDebuggerWindow showForSession:_session];
    if (getenv("CHF_OPEN_SETTINGS")) [self showSettings:nil];

    /* The timers below run only while the machine's window can be seen: they
     * follow the same notices the display's frame clock does. */
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    for (NSString *name in @[NSWindowDidChangeOcclusionStateNotification, NSWindowDidMiniaturizeNotification,
                             NSWindowDidDeminiaturizeNotification, NSApplicationDidHideNotification,
                             NSApplicationDidUnhideNotification])
        [nc addObserver:self selector:@selector(visibilityChanged:) name:name object:nil];
    [self updateTitle];
    [self updateTimers];
}

/* The machine the settings name (what the next power cycle starts). */
- (int)machineSetting
{
    const int m = chfsession_get_int(_session, "machine", CHF_MACHINE_CHANNELF);
    return m >= 0 && m < CHF_MACHINE_COUNT ? m : CHF_MACHINE_CHANNELF;
}

/* The picture's shape: the running machine's, at the chosen aspect. */
- (void)applyDisplayShape
{
    [_display setMachine:_machine squarePixels:chfsession_get_int(_session, "aspect", 0) != 0];
}

/* What the 10 Hz timer does: system actions the gamepad thread resolved (it
 * cannot touch AppKit), the machine asking for the BIOS, and gamepads coming
 * and going. */
- (void)sysactTick
{
    int sa;
    while (chfsession_sysaction_take(_session, &sa)) [self runSysaction:sa];
    [self checkMissing];
    const unsigned gen = chfsession_gamepad_generation(_session);
    if (gen != _toastGeneration) {
        _toastGeneration = gen;
        char msg[160];
        if (chfsession_gamepad_last_event(_session, msg, sizeof msg) > 0)
            [self showToast:nsstr(msg)];
    }
}

- (BOOL)windowCanBeSeen:(NSWindow *)w
{
    return w && [w isVisible] && ![w isMiniaturized] &&
           ([w occlusionState] & NSWindowOcclusionStateVisible) && ![NSApp isHidden];
}

/* The status and system-action timers, and the console log's, run while their
 * windows can be seen; they catch up once when they start again. */
- (void)updateTimers
{
    const BOOL want = _launched && [self windowCanBeSeen:_window];
    if (want && !_sysactTimer) {
        __weak ChfAppDelegate *weakSelf = self;
        _statusTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES
            block:^(NSTimer *t) { (void)t; [weakSelf updateTitle]; }];
        _statusTimer.tolerance = 0.5;
        _sysactTimer = [NSTimer scheduledTimerWithTimeInterval:0.1 repeats:YES
            block:^(NSTimer *t) { (void)t; [weakSelf sysactTick]; }];
        _sysactTimer.tolerance = 0.05;
        [self updateTitle];
        [self sysactTick];
    } else if (!want && _sysactTimer) {
        [_statusTimer invalidate];
        [_sysactTimer invalidate];
        _statusTimer = nil;
        _sysactTimer = nil;
    }
    [self updateLogTimer];
}

- (void)visibilityChanged:(NSNotification *)note
{
    (void)note;
    [self updateTimers];
}

- (void)showToast:(NSString *)text
{
    if (!text) return;
    _toast.stringValue = text;
    _toast.alphaValue = 1.0;
    const NSUInteger serial = ++_toastSerial;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(TOAST_SECONDS * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        /* a newer message restarts the clock */
        if (serial != self->_toastSerial) return;
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
            ctx.duration = 0.5;
            self->_toast.animator.alphaValue = 0.0;
        } completionHandler:nil];
    });
}

/* A refusal (power-up or load): the session's message says why. */
- (void)reportRefusal:(NSString *)why title:(NSString *)title
{
    [self alert:title text:why];
}

/* ---- what the machine is missing ----------------------------------------------------- */

/* The BIOS: a game was refused, or a FujiNet app called a routine the
 * stand-in lacks and the console is parked. Said once, as a sheet on the
 * machine's window, until a power cycle (or the import) clears it and
 * something trips it again. */
- (void)checkMissing
{
    if (!chfsession_needs_bios(_session)) {
        if (_needsBiosShown) {
            _needsBiosShown = NO;
            ChfSetHelpContext(_window, @"main");
        }
        return;
    }
    ChfSetHelpContext(_window, @"nobios");
    if (_needsBiosShown) return;
    _needsBiosShown = YES;
    char why[256];
    why[0] = '\0';
    chfsession_missing_reason(_session, why, (int)sizeof why);
    [self askForBios:nsstr(why)];
}

/* The alert: Import BIOS… picks the files (and opens the refused cartridge
 * again, or power-cycles), Not Now leaves it. Its Help button opens the
 * nobios topic. */
- (void)askForBios:(NSString *)reason
{
    /* the BIOS switched off in Settings: importing would not change that */
    const BOOL canImport = ![reason containsString:@"switched off"];
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = @"Needs BIOS";
    a.informativeText = reason.length ? reason
        : @"This needs the console's own BIOS, which the app does not include. CONFIG and the "
           @"FujiNet apps run without it.";
    if (canImport) {
        [a addButtonWithTitle:@"Import BIOS…"];
        [a addButtonWithTitle:@"Not Now"];
    } else {
        [a addButtonWithTitle:@"OK"];
    }
    a.showsHelp = YES;
    a.delegate = self;
    [a beginSheetModalForWindow:_window completionHandler:^(NSModalResponse r) {
        if (!canImport || r != NSAlertFirstButtonReturn) return;
        dispatch_async(dispatch_get_main_queue(), ^{ [self importRomsFromPanel:NO]; });
    }];
}

- (BOOL)alertShowHelp:(NSAlert *)alert
{
    (void)alert;
    [[ChfHelpWindow shared] showAt:@"nobios"];
    return YES;
}

- (void)runSysaction:(int)sa
{
    switch (sa) {
    case CHF_SYSACT_RESET_CONFIG:
        _refusedCart = nil;
        chfsession_sysaction(_session, sa);
        break;
    case CHF_SYSACT_DEBUG_STOP:
        /* the debugger window attaches, which stops the machine */
        [ChfDebuggerWindow showForSession:_session];
        break;
    default: break;
    }
    [self updateTitle];
}

/* F9, F11 and F12 are the app's, not bindable; then a key bound to a system
 * action. YES when taken. */
- (BOOL)handleAppKey:(uint32_t)ks
{
    if (ks == CHF_KEYSYM_F9) { [_controllers toggle]; return YES; }
    if (ks == CHF_KEYSYM_F11) { [_window toggleFullScreen:nil]; return YES; }
    if (ks == CHF_KEYSYM_F12) { [ChfDebuggerWindow toggleForSession:_session]; return YES; }
    const int sa = chfsession_key_sysaction(_session, ks);
    if (sa >= 0) { [self runSysaction:sa]; return YES; }
    return NO;
}

- (void)updateTitle
{
    NSString *state;
    char st[160];
    if (!chfsession_is_running(_session)) {
        state = @"stopped";
    } else {
        chfsession_cart_status(_session, st, sizeof st);
        /* the status dot: filled while the cartridge's link to FujiNet is up */
        const int linkUp = chfsession_cart_link_up(_session);
        state = [NSString stringWithFormat:@"%@FujiNet: %s",
                 linkUp > 0 ? @"● " : (linkUp == 0 ? @"○ " : @""), st];
        const char *cart = chfsession_cart_path(_session);
        if (cart && cart[0])
            state = [NSString stringWithFormat:@"%@ — %@", [nsstr(cart) lastPathComponent], state];
        if (chfsession_is_paused(_session)) state = [state stringByAppendingString:@" — Paused"];
    }
    /* The title bar is the status bar here: an AppKit window has no natural
     * place for one, and a floating HUD over the picture would be worse. */
    NSString *title = [NSString stringWithFormat:@"%@ — %@", APP_TITLE, state];
    if (![[_window title] isEqualToString:title]) [_window setTitle:title];
}

/* Losing key status with keys held would leave the machine believing they
 * are still down. */
- (void)windowDidResignKey:(NSNotification *)note
{
    if (_session && note.object == _window) chfsession_release_all(_session);
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app
{
    (void)app;
    return YES;
}

- (void)applicationWillTerminate:(NSNotification *)note
{
    (void)note;
    /* Close the windows while the session is alive: AppKit closes them after
     * this returns, and their resign-key / will-close handlers use it. */
    _window.delegate = nil;
    for (NSWindow *w in [[NSApp windows] copy])
        if (w != _window) [w close];
    [_statusTimer invalidate];
    [_sysactTimer invalidate];
    [_logTimer invalidate];
    [_display stop];
    chfsession_stop(_session);
    chfsession_free(_session);
    _session = NULL;
}

- (BOOL)application:(NSApplication *)app openFile:(NSString *)filename
{
    (void)app;
    /* Opening a file from Finder launches the app and delivers the file
     * before applicationDidFinishLaunching:, when there is no machine yet
     * to load it into: hold it until there is. */
    if (!_launched) {
        _pendingMedia = filename;
        return YES;
    }
    [self loadMediaPaths:@[filename]];
    return YES;
}

/* ---- menu ------------------------------------------------------------------------- */

- (NSMenuItem *)item:(NSMenu *)menu title:(NSString *)title action:(SEL)sel key:(NSString *)key
{
    NSMenuItem *it = [menu addItemWithTitle:title action:sel keyEquivalent:key];
    [it setTarget:self];
    return it;
}

- (void)buildMenu
{
    NSMenu *bar = [[NSMenu alloc] init];

    NSMenuItem *appItem = [[NSMenuItem alloc] init];
    NSMenu *appMenu = [[NSMenu alloc] init];
    [self item:appMenu title:@"About " APP_TITLE action:@selector(showAbout:) key:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [self item:appMenu title:@"Settings…" action:@selector(showSettings:) key:@","];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Hide " APP_TITLE action:@selector(hide:) keyEquivalent:@"h"];
    NSMenuItem *others = [appMenu addItemWithTitle:@"Hide Others"
                                            action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
    [others setKeyEquivalentModifierMask:NSEventModifierFlagOption | NSEventModifierFlagCommand];
    [appMenu addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit " APP_TITLE action:@selector(terminate:) keyEquivalent:@"q"];
    [appItem setSubmenu:appMenu];
    [bar addItem:appItem];

    NSMenuItem *machineItem = [[NSMenuItem alloc] init];
    NSMenu *machine = [[NSMenu alloc] initWithTitle:@"Machine"];
    [self item:machine title:@"Open Cartridge…" action:@selector(openCart:) key:@"o"];
    [self item:machine title:@"Eject Cartridge" action:@selector(ejectCart:) key:@""];
    [self item:machine title:@"Import Cartridge to SD…" action:@selector(importToSd:) key:@""];
    [self item:machine title:@"Open SD Folder" action:@selector(openSdFolder:) key:@""];
    [machine addItem:[NSMenuItem separatorItem]];
    [self item:machine title:@"Import BIOS…" action:@selector(importBios:) key:@""];
    [self item:machine title:@"Import BIOS from a Folder…" action:@selector(importBiosFolder:) key:@""];
    [machine addItem:[NSMenuItem separatorItem]];
    [self item:machine title:@"Pause" action:@selector(togglePause:) key:@""];
    /* F1 and Escape are bindings (the console's RESET key, Reset to CONFIG),
     * not key equivalents: a menu equivalent would take the key before the
     * bindings table sees it and could not be remapped. The titles name
     * whatever key is bound now (see validateMenuItem:). */
    [self item:machine title:@"Reset" action:@selector(resetConsole:) key:@""];
    [self item:machine title:@"Reset to CONFIG" action:@selector(resetConfig:) key:@"r"];
    [machineItem setSubmenu:machine];
    [bar addItem:machineItem];

    NSMenuItem *viewItem = [[NSMenuItem alloc] init];
    NSMenu *view = [[NSMenu alloc] initWithTitle:@"View"];
    [self item:view title:@"Controllers (F9)" action:@selector(toggleControllers:) key:@"j"];
    [self item:view title:@"Debugger (F12)" action:@selector(toggleDebugger:) key:@"d"];
    [view addItem:[NSMenuItem separatorItem]];
    _aspectItem = [self item:view title:@"TV Aspect" action:@selector(toggleAspect:) key:@""];
    [_aspectItem setState:(chfsession_get_int(_session, "aspect", 0) == 0 ? NSControlStateValueOn : NSControlStateValueOff)];
    _smoothItem = [self item:view title:@"Smooth Scaling" action:@selector(toggleSmooth:) key:@""];
    [_smoothItem setState:(chfsession_get_int(_session, "smooth", 0) ? NSControlStateValueOn : NSControlStateValueOff)];
    /* F11 toggles it too, from the machine's window (see ChfContentView). */
    NSMenuItem *fs = [view addItemWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
    [fs setKeyEquivalentModifierMask:NSEventModifierFlagControl | NSEventModifierFlagCommand];
    [viewItem setSubmenu:view];
    [bar addItem:viewItem];

    NSMenuItem *fujiItem = [[NSMenuItem alloc] init];
    NSMenu *fuji = [[NSMenu alloc] initWithTitle:@"FujiNet"];
    [self item:fuji title:@"FujiNet Web UI" action:@selector(openWebUI:) key:@""];
    [self item:fuji title:@"Console Log" action:@selector(showFujiNetLog:) key:@""];
    [fujiItem setSubmenu:fuji];
    [bar addItem:fujiItem];

    /* Window and Help: AppKit fills these in (the window list, the Help
     * search field) once they are registered as such. */
    NSMenuItem *windowItem = [[NSMenuItem alloc] init];
    NSMenu *windows = [[NSMenu alloc] initWithTitle:@"Window"];
    [windows addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [windows addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
    [windows addItem:[NSMenuItem separatorItem]];
    [windows addItemWithTitle:@"Bring All to Front" action:@selector(arrangeInFront:) keyEquivalent:@""];
    [windowItem setSubmenu:windows];
    [bar addItem:windowItem];

    /* openHelp:, not showHelp: (AppKit's: "Help isn't available") */
    NSMenuItem *helpItem = [[NSMenuItem alloc] init];
    NSMenu *help = [[NSMenu alloc] initWithTitle:@"Help"];
    [self item:help title:APP_TITLE @" Help" action:@selector(openHelp:) key:@"?"];
    [help addItem:[NSMenuItem separatorItem]];
    [self item:help title:@"About " APP_TITLE action:@selector(showAbout:) key:@""];
    [helpItem setSubmenu:help];
    [bar addItem:helpItem];

    [NSApp setMainMenu:bar];
    [NSApp setWindowsMenu:windows];
    [NSApp setHelpMenu:help];
    /* the Help menu's search field finds the Help's sections too */
    [NSApp registerUserInterfaceItemSearchHandler:[ChfHelpWindow shared]];
}

/* ---- actions ------------------------------------------------------------------------ */

- (void)alert:(NSString *)title text:(NSString *)text
{
    NSAlert *a = [[NSAlert alloc] init];
    [a setMessageText:title];
    if (text) [a setInformativeText:text];
    [a runModal];
}

- (void)showAbout:(id)sender
{
    (void)sender;
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = NSTextAlignmentCenter;
    NSString *text = @"Fairchild Channel F with built-in FujiNet\n\n"
                      WEBSITE_URL "\n"
                      "Report issues: " ISSUES_URL "\n\n"
                      "Licensed under the GNU General Public License, version 3 or later "
                      "(GPL-3.0-or-later).\n\n"
                      "The emulator is MAME's Channel F code -- the Fairchild F3850 (F8) "
                      "processor (BSD-3-Clause) and the console driver (GPL-2.0+); Juergen "
                      "Buchmueller, Frank Palazzolo, Sean Riddle and others -- transposed to C. "
                      "The FujiNet firmware and the cartridge: Thomas Cherryhomes and the "
                      "FujiNet contributors. No BIOS ships with the app.";
    NSMutableAttributedString *credits = [[NSMutableAttributedString alloc]
        initWithString:text
            attributes:@{ NSFontAttributeName: [NSFont systemFontOfSize:NSFont.smallSystemFontSize],
                          NSForegroundColorAttributeName: NSColor.labelColor,
                          NSParagraphStyleAttributeName: para }];
    /* the two addresses as links */
    for (NSString *url in @[@WEBSITE_URL, @ISSUES_URL]) {
        const NSRange r = [text rangeOfString:url];
        if (r.location != NSNotFound)
            [credits addAttribute:NSLinkAttributeName value:[NSURL URLWithString:url] range:r];
    }
    [NSApp orderFrontStandardAboutPanelWithOptions:@{
        NSAboutPanelOptionApplicationName: APP_TITLE,
        NSAboutPanelOptionApplicationVersion: nsstr(CHF_VERSION_STRING),
        NSAboutPanelOptionVersion: @"",
        NSAboutPanelOptionCredits: credits,
    }];
}

/* A game while no BIOS is imported is refused with the reason: the session
 * says so and flags it, and the Needs BIOS alert offers the import; the
 * cartridge is opened again after it. */
- (void)openCartAtPath:(NSString *)path
{
    if (chfsession_load_cart(_session, [path fileSystemRepresentation]) != 0) {
        if (chfsession_needs_bios(_session)) {
            _refusedCart = path;
            _needsBiosShown = NO;
            [self checkMissing];
        } else {
            _refusedCart = nil;
            [self reportRefusal:[self lastError] title:@"Could not open"];
        }
    } else {
        _refusedCart = nil;
    }
    [self updateTitle];
}

/* Open, a drop, Finder, the command line: BIOS ROMs and zips of them are
 * imported (together, one report), the first cartridge is opened, anything
 * else goes on FujiNet's SD card. */
- (void)loadMediaPaths:(NSArray<NSString *> *)paths
{
    NSMutableArray<NSString *> *roms = [NSMutableArray array];
    NSString *cart = nil;
    for (NSString *p in paths) {
        const char *fs = [p fileSystemRepresentation];
        if (chfsession_media_is_rom(fs)) {
            [roms addObject:p];
        } else if (!cart && chfsession_media_is_cartridge(fs)) {
            cart = p;
        } else {
            char dest[1024];
            if (chfsession_import_media(_session, fs, dest, (int)sizeof dest) != 0) {
                [self alert:@"Import failed" text:[self lastError]];
                continue;
            }
            [self showToast:[NSString stringWithFormat:@"%@ copied to FujiNet's SD folder. Mount it from CONFIG.",
                             [nsstr(dest) lastPathComponent]]];
        }
    }
    if ([roms count]) [self importRomPaths:roms];
    if (cart) [self openCartAtPath:cart];
}

/* Runs an open panel as a sheet on the key window (the machine's window if
 * none; a window nobody can see gets the panel alone) and calls done with
 * the chosen path, after the sheet has gone. */
- (void)runPanel:(NSOpenPanel *)p then:(void (^)(NSString *path))done
{
    void (^finish)(NSModalResponse) = ^(NSModalResponse r) {
        self->_panel = nil;
        if (r != NSModalResponseOK) return;
        NSString *path = [[p URL] path];
        dispatch_async(dispatch_get_main_queue(), ^{ done(path); });
    };
    NSWindow *host = [NSApp keyWindow];
    if (!host) host = _window;
    if (host && [host isVisible] && ![host isMiniaturized])
        [p beginSheetModalForWindow:host completionHandler:finish];
    else
        finish([p runModal]);
}

/* allName: offer an "All files" choice beside the filter, named so (an
 * accessory: AppKit's panels have no filter menu of their own); nil: the
 * filter alone. helpContext: a Help button in the accessory. */
- (void)pickFile:(NSString *)title types:(NSArray<NSString *> *)types allFilesBeside:(NSString *)allName
            help:(NSString *)helpContext then:(void (^)(NSString *path))done
{
    NSOpenPanel *p = [NSOpenPanel openPanel];
    [p setTitle:title];
    [p setMessage:title];
    _panelTypes = typesForExtensions(types);
    p.allowedContentTypes = _panelTypes;
    NSMutableArray<NSView *> *acc = [NSMutableArray array];
    if (allName) {
        NSPopUpButton *kind = [[NSPopUpButton alloc] init];
        [kind addItemsWithTitles:@[allName, @"All files"]];
        kind.target = self;
        kind.action = @selector(panelKindChanged:);
        [acc addObject:[NSTextField labelWithString:@"Show"]];
        [acc addObject:kind];
    }
    if (helpContext) [acc addObject:ChfHelpButton(helpContext)];
    if ([acc count]) {
        NSStackView *view = [NSStackView stackViewWithViews:acc];
        view.orientation = NSUserInterfaceLayoutOrientationHorizontal;
        view.edgeInsets = NSEdgeInsetsMake(8, 12, 8, 12);
        [view setFrameSize:view.fittingSize];
        p.accessoryView = view;
        p.accessoryViewDisclosed = YES;
    }
    _panel = allName ? p : nil;
    [self runPanel:p then:done];
}

- (void)panelKindChanged:(NSPopUpButton *)sender
{
    /* an empty list allows every file */
    _panel.allowedContentTypes = sender.indexOfSelectedItem == 0 ? _panelTypes : @[];
}

/* .bin or .chf, or a zip of one: the cartridge. */
- (void)pickCartridge:(NSString *)title help:(NSString *)helpContext then:(void (^)(NSString *path))done
{
    [self pickFile:title types:@[@"bin", @"chf", @"zip"]
         allFilesBeside:@"Cartridge images (.bin .chf .zip)" help:helpContext then:done];
}

- (void)openCart:(id)sender
{
    (void)sender;
    [self pickCartridge:@"Open Cartridge" help:nil then:^(NSString *path) { [self openCartAtPath:path]; }];
}

- (void)ejectCart:(id)sender
{
    (void)sender;
    _refusedCart = nil;
    if (chfsession_eject(_session) != 0)
        [self alert:@"Could not eject" text:[self lastError]];
    [self updateTitle];
}

- (void)importToSd:(id)sender
{
    (void)sender;
    [self pickCartridge:@"Import Cartridge to SD" help:@"import-sd" then:^(NSString *path) {
        char dest[1024];
        if (chfsession_import_cart_to_sd(self->_session, [path fileSystemRepresentation], dest, sizeof dest) != 0) {
            [self alert:@"Import failed" text:[self lastError]];
            return;
        }
        [self showToast:[NSString stringWithFormat:@"%@ is on the SD host. Boot it from CONFIG.",
                         [nsstr(dest) lastPathComponent]]];
    }];
}

/* FujiNet's SD folder in Finder. */
- (void)openSdFolder:(id)sender
{
    (void)sender;
    const char *p = chfsession_sd_path(_session);
    if (!p || !p[0]) return;
    NSString *dir = nsstr(p);
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES
                                               attributes:nil error:nil];
    [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:dir isDirectory:YES]];
}

/* ---- the BIOS ----------------------------------------------------------------------------- */

/* Whatever was imported, the session's messages say what happened -- which ROM
 * it was, an unrecognised dump of the right size, the next power cycle. A
 * successful import takes effect at once when something was missing: the
 * cartridge that was refused is opened again, or the console power-cycles
 * (back onto the cartridge in the slot, or CONFIG). */
- (void)importRomPaths:(NSArray<NSString *> *)paths
{
    NSMutableArray<NSString *> *log = [NSMutableArray array];
    int imported = 0;
    for (NSString *p in paths) {
        char msg[2048];
        msg[0] = '\0';
        const int n = chfsession_rom_import(_session, [p fileSystemRepresentation], msg, (int)sizeof msg);
        if (n > 0) imported += n;
        if (msg[0]) [log addObject:nsstr(msg)];
    }
    [_settings refresh];
    NSString *text = [log count] ? [log componentsJoinedByString:@"\n"]
                                 : (imported > 0 ? @"Imported." : @"None of these files is a ROM this app uses.");
    [self alert:(imported > 0 ? @"BIOS Imported" : @"Nothing Imported") text:text];
    if (imported <= 0) return;

    if (_refusedCart) {
        NSString *cart = _refusedCart;
        _refusedCart = nil;
        [self openCartAtPath:cart];
    } else if (!chfsession_is_running(_session) || chfsession_missing(_session)) {
        if (chfsession_restart(_session) != 0)
            [self reportRefusal:[self lastError] title:@"Could not start"];
        else
            [self showToast:@"The console was power-cycled with the new BIOS."];
    }
    [self updateTitle];
    [self checkMissing];
}

/* Machine > Import BIOS… (files and zips, any number) and Import BIOS from a
 * Folder…: the ROMs are recognised by checksum, so the files may be called
 * anything. */
- (void)importRomsFromPanel:(BOOL)folder
{
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    [panel setMessage:folder ? @"Import BIOS from a Folder (MAME's ROM folder works)"
                             : @"Import BIOS: ROM images or zips (MAME's channelf.zip works)"];
    [panel setCanChooseDirectories:folder];
    [panel setCanChooseFiles:!folder];
    [panel setAllowsMultipleSelection:!folder];
    NSWindow *host = [NSApp keyWindow];
    if (!host) host = _window;
    void (^finish)(NSModalResponse) = ^(NSModalResponse r) {
        if (r != NSModalResponseOK) return;
        NSMutableArray<NSString *> *paths = [NSMutableArray array];
        for (NSURL *u in [panel URLs]) [paths addObject:[u path]];
        /* after the sheet: what came in is an alert of its own */
        dispatch_async(dispatch_get_main_queue(), ^{ [self importRomPaths:paths]; });
    };
    if (host && [host isVisible] && ![host isMiniaturized])
        [panel beginSheetModalForWindow:host completionHandler:finish];
    else
        finish([panel runModal]);
}

- (void)importBios:(id)sender { (void)sender; [self importRomsFromPanel:NO]; }
- (void)importBiosFolder:(id)sender { (void)sender; [self importRomsFromPanel:YES]; }

/* ---- the console ---------------------------------------------------------------------------- */

- (void)togglePause:(id)sender
{
    (void)sender;
    chfsession_set_paused(_session, !chfsession_is_paused(_session));
    [self updateTitle];
}

/* The console's RESET key: pressed and released over a few frames by the
 * session, as the console samples it once a frame. */
- (void)resetConsole:(id)sender
{
    (void)sender;
    chfsession_reset(_session);
}

- (void)resetConfig:(id)sender { (void)sender; [self runSysaction:CHF_SYSACT_RESET_CONFIG]; }

/* "Reset (F1)": the key bound to a target now, if any. */
- (NSString *)title:(NSString *)base forTarget:(int)target
{
    char key[32];
    const chf_binding b = chfsession_binding_get(_session, target);
    if (!b.keysym || chfsession_keysym_name(b.keysym, key, sizeof key) <= 0) return base;
    return [NSString stringWithFormat:@"%@ (%s)", base, key];
}

/* The console items' titles follow the bindings (the Controllers panel
 * remaps them). */
- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    const SEL a = [item action];
    if (a == @selector(resetConsole:)) {
        [item setTitle:[self title:@"Reset" forTarget:CHF_TARGET_SWITCH(CHF_SW_RESET)]];
        return chfsession_is_running(_session);
    }
    if (a == @selector(resetConfig:)) {
        [item setTitle:[self title:@"Reset to CONFIG" forTarget:CHF_TARGET_SYSACT(CHF_SYSACT_RESET_CONFIG)]];
        return chfsession_is_running(_session);
    }
    if (a == @selector(togglePause:)) {
        [item setState:(chfsession_is_paused(_session) ? NSControlStateValueOn : NSControlStateValueOff)];
        return chfsession_is_running(_session);
    }
    if (a == @selector(ejectCart:))
        return chfsession_is_running(_session);
    return YES;
}

- (void)toggleControllers:(id)sender { (void)sender; [_controllers toggle]; }
- (void)toggleDebugger:(id)sender { (void)sender; [ChfDebuggerWindow toggleForSession:_session]; }

/* aspect 0: the TV's 4:3 (the default); 1: square pixels. */
- (void)setAspect:(int)aspect
{
    chfsession_set_int(_session, "aspect", aspect);
    [_display setMachine:_machine squarePixels:aspect != 0];
    [_aspectItem setState:(aspect == 0 ? NSControlStateValueOn : NSControlStateValueOff)];
    [_settings syncDisplay];
}

- (void)setSmooth:(BOOL)on
{
    chfsession_set_int(_session, "smooth", on ? 1 : 0);
    [_display setSmooth:on];
    [_smoothItem setState:(on ? NSControlStateValueOn : NSControlStateValueOff)];
    [_settings syncDisplay];
}

- (void)toggleAspect:(id)sender
{
    (void)sender;
    [self setAspect:chfsession_get_int(_session, "aspect", 0) == 0 ? 1 : 0];
}

- (void)toggleSmooth:(id)sender
{
    (void)sender;
    [self setSmooth:chfsession_get_int(_session, "smooth", 0) == 0];
}

/* ---- settings ------------------------------------------------------------------------------- */

- (void)showSettings:(id)sender
{
    (void)sender;
    if (!_settings) _settings = [[ChfSettingsWindow alloc] initWithSession:_session delegate:self];
    [_settings show];
}

- (void)settingsSetAspect:(int)aspect { [self setAspect:aspect]; }
- (void)settingsSetSmooth:(BOOL)smooth { [self setSmooth:smooth]; }
- (void)settingsOpenWebUI { [self openWebUI:nil]; }
- (void)settingsImportRoms:(BOOL)folder { [self importRomsFromPanel:folder]; }

/* A power cycle for the machine options, a full restart if a host option
 * changed: the session knows which. */
- (void)settingsClosed:(BOOL)needsRestart
{
    if (needsRestart) {
        _refusedCart = nil;
        if (chfsession_restart(_session) != 0)
            [self reportRefusal:[self lastError] title:@"Could not start"];
        else
            [self showToast:@"Settings applied: the console was power-cycled."];
        /* the new machine's picture shape */
        _machine = [self machineSetting];
        [self applyDisplayShape];
    }
    [_controllers refresh];
    [self updateTitle];
}

/* ---- FujiNet console log ----------------------------------------------------------------------- */

- (void)refreshLog:(NSTimer *)timer
{
    (void)timer;
    static char buf[128 * 1024];
    const int n = chfsession_fujinet_copy_log(_session, buf, sizeof buf);
    NSString *text = nil;
    if (n > 0) {
        /* UTF-8, or failing that Latin-1: one stray byte must not blank it */
        text = [[NSString alloc] initWithBytes:buf length:(NSUInteger)n encoding:NSUTF8StringEncoding];
        if (!text) text = [[NSString alloc] initWithBytes:buf length:(NSUInteger)n encoding:NSISOLatin1StringEncoding];
    }
    NSString *shown = text ?: @"(no FujiNet output yet)";
    /* nothing new: leave the text, and the reader's place in it, alone */
    if ([_logView.string isEqualToString:shown]) return;
    NSScrollView *scroll = (NSScrollView *)_logView.enclosingScrollView;
    const BOOL atEnd = !scroll || (NSMaxY(scroll.contentView.documentVisibleRect) >=
                                   NSMaxY(((NSView *)scroll.documentView).frame) - 4.0);
    [_logView setString:shown];
    if (atEnd) [_logView scrollRangeToVisible:NSMakeRange(_logView.string.length, 0)];
}

/* Running while the log window can be seen; one refresh when it starts. */
- (void)updateLogTimer
{
    const BOOL want = [self windowCanBeSeen:_logWindow];
    if (want && !_logTimer) {
        _logTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 target:self
                                                   selector:@selector(refreshLog:) userInfo:nil repeats:YES];
        _logTimer.tolerance = 0.5;
        [self refreshLog:nil];
    } else if (!want && _logTimer) {
        [_logTimer invalidate];
        _logTimer = nil;
    }
}

- (void)windowWillClose:(NSNotification *)note
{
    if (note.object == _logWindow) {
        [_logTimer invalidate];
        _logTimer = nil;
    } else if (note.object == _window) {
        /* the machine's window is the app: with the debugger or Settings
         * still open, "the last window closed" would never come */
        dispatch_async(dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
    }
}

- (void)showFujiNetLog:(id)sender
{
    (void)sender;
    if (chfsession_fujinet_external(_session)) {
        [self alert:@"FujiNet is external" text:@"Its console output is in the process that is running it."];
        return;
    }
    if (_logWindow) {
        [_logWindow makeKeyAndOrderFront:nil];
        [self refreshLog:nil];
        [self updateLogTimer];
        return;
    }

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 36, 860, 564)];
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers = NO;
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

    _logView = [[NSTextView alloc] initWithFrame:scroll.bounds];
    _logView.editable = NO;
    _logView.richText = NO;
    _logView.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    _logView.autoresizingMask = NSViewWidthSizable;
    scroll.documentView = _logView;

    _logWindow = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 860, 600)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _logWindow.title = @"FujiNet Console Log";
    _logWindow.releasedWhenClosed = NO;
    _logWindow.delegate = self;
    {
        /* the log over a strip with the Help button */
        NSView *box = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 860, 600)];
        NSButton *help = ChfHelpButton(@"fujilog");
        [help setFrameOrigin:NSMakePoint(10, 6)];
        help.autoresizingMask = NSViewMaxXMargin | NSViewMaxYMargin;
        [box addSubview:scroll];
        [box addSubview:help];
        _logWindow.contentView = box;
    }
    ChfSetHelpContext(_logWindow, @"fujilog");
    [_logWindow center];

    [self refreshLog:nil];
    [_logWindow makeKeyAndOrderFront:nil];
    [self updateLogTimer];
}

/* For the window in front: Settings' page, the debugger's tab ... */
- (void)openHelp:(id)sender
{
    (void)sender;
    [[ChfHelpWindow shared] showFor:[NSApp keyWindow]];
}

- (void)openWebUI:(id)sender
{
    (void)sender;
    if (chfsession_fujinet_external(_session)) {
        [self alert:@"FujiNet is external" text:@"Use the web UI of the FujiNet instance this app dials."];
        return;
    }
    if (!chfsession_fujinet_running(_session)) {
        [self alert:@"FujiNet is not running" text:nil];
        return;
    }
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:nsstr(chfsession_fujinet_webui_url(_session))]];
}
@end
