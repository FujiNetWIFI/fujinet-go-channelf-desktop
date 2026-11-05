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

@interface ChfAppDelegate () <ChfSettingsDelegate>
- (void)runSysaction:(int)sa;
- (void)loadMediaPaths:(NSArray<NSString *> *)paths;
- (BOOL)handleAppKey:(uint32_t)keysym;
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

    /* The 192x262 picture at the TV's 4:3, twice its 262 lines high:
     * 699x524, a comfortable first window on any Mac. */
    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 699, 524)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [_window setTitle:APP_TITLE];
    /* NSWindow's default is to release itself on close, which CHF then does
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
    [_display setTvAspect:chfsession_get_int(_session, "aspect", 0) == 0];
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

/* What the 10 Hz timer does: system actions the gamepad thread resolved (it
 * cannot touch AppKit), and gamepads coming and going. */
- (void)sysactTick
{
    int sa;
    while (chfsession_sysaction_take(_session, &sa)) [self runSysaction:sa];
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

- (void)runSysaction:(int)sa
{
    switch (sa) {
    case CHF_SYSACT_RESET_CONFIG:
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
    if (note.object == _window) chfsession_release_all(_session);
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app
{
    (void)app;
    return YES;
}

- (void)applicationWillTerminate:(NSNotification *)note
{
    (void)note;
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
    [self item:fuji title:@"Web UI" action:@selector(openWebUI:) key:@""];
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

    NSMenuItem *helpItem = [[NSMenuItem alloc] init];
    NSMenu *help = [[NSMenu alloc] initWithTitle:@"Help"];
    [self item:help title:@"About " APP_TITLE action:@selector(showAbout:) key:@""];
    [helpItem setSubmenu:help];
    [bar addItem:helpItem];

    [NSApp setMainMenu:bar];
    [NSApp setWindowsMenu:windows];
    [NSApp setHelpMenu:help];
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
                      "The emulator is MAME's Channel F code -- the Signetics 2650 and the "
                      "2637 UVI video chip (BSD-3-Clause CPU, GPL-2.0+ driver; Peter Trauner, "
                      "Juergen Buchmueller, Martin Buchholz and others) -- "
                      "transposed to C. The FujiNet firmware and the cartridge: Thomas Cherryhomes "
                      "and the FujiNet contributors.";
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

- (void)openCartAtPath:(NSString *)path
{
    if (chfsession_load_cart(_session, [path fileSystemRepresentation]) != 0)
        [self reportRefusal:[self lastError] title:@"Could not open"];
    [self updateTitle];
}

/* Open, a drop, Finder, the command line: the first cartridge is opened,
 * anything else goes on FujiNet's SD card. */
- (void)loadMediaPaths:(NSArray<NSString *> *)paths
{
    NSString *cart = nil;
    for (NSString *p in paths) {
        const char *fs = [p fileSystemRepresentation];
        if (!cart && chfsession_media_is_cartridge(fs)) {
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
    if (cart) [self openCartAtPath:cart];
}

/* allName: offer an "All files" choice beside the filter, named so (an
 * accessory: AppKit's panels have no filter menu of their own); nil: the
 * filter alone. */
- (NSString *)pickFile:(NSString *)title types:(NSArray<NSString *> *)types allFilesBeside:(NSString *)allName
{
    NSOpenPanel *p = [NSOpenPanel openPanel];
    [p setTitle:title];
    _panelTypes = typesForExtensions(types);
    p.allowedContentTypes = _panelTypes;
    if (allName) {
        NSPopUpButton *kind = [[NSPopUpButton alloc] init];
        [kind addItemsWithTitles:@[allName, @"All files"]];
        kind.target = self;
        kind.action = @selector(panelKindChanged:);
        NSStackView *acc = [NSStackView stackViewWithViews:@[[NSTextField labelWithString:@"Show"], kind]];
        acc.orientation = NSUserInterfaceLayoutOrientationHorizontal;
        acc.edgeInsets = NSEdgeInsetsMake(8, 12, 8, 12);
        [acc setFrameSize:acc.fittingSize];
        p.accessoryView = acc;
        p.accessoryViewDisclosed = YES;
        _panel = p;
    }
    const NSModalResponse r = [p runModal];
    _panel = nil;
    if (r != NSModalResponseOK) return nil;
    return [[p URL] path];
}

- (void)panelKindChanged:(NSPopUpButton *)sender
{
    /* an empty list allows every file */
    _panel.allowedContentTypes = sender.indexOfSelectedItem == 0 ? _panelTypes : @[];
}

/* .bin, .rom, .chf or a zip of one: the cartridge. */
- (NSString *)pickCartridge:(NSString *)title
{
    return [self pickFile:title types:@[@"bin", @"rom", @"chf", @"zip"]
           allFilesBeside:@"Cartridge images (.bin .rom .chf .zip)"];
}

- (void)openCart:(id)sender
{
    (void)sender;
    NSString *path = [self pickCartridge:@"Open Cartridge"];
    if (path) [self openCartAtPath:path];
}

- (void)ejectCart:(id)sender
{
    (void)sender;
    if (chfsession_eject(_session) != 0)
        [self alert:@"Could not eject" text:[self lastError]];
    [self updateTitle];
}

- (void)importToSd:(id)sender
{
    (void)sender;
    NSString *path = [self pickCartridge:@"Import Cartridge to SD"];
    if (!path) return;
    char dest[1024];
    if (chfsession_import_cart_to_sd(_session, [path fileSystemRepresentation], dest, sizeof dest) != 0) {
        [self alert:@"Import failed" text:[self lastError]];
        return;
    }
    [self showToast:[NSString stringWithFormat:@"%@ is on the SD host. Boot it from CONFIG.",
                     [nsstr(dest) lastPathComponent]]];
}

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
    [_display setTvAspect:aspect == 0];
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

/* A power cycle for the machine options, a full restart if a host option
 * changed: the session knows which. */
- (void)settingsClosed:(BOOL)needsRestart
{
    if (needsRestart) {
        if (chfsession_restart(_session) != 0)
            [self reportRefusal:[self lastError] title:@"Could not start"];
        else
            [self showToast:@"Settings applied: the console was power-cycled."];
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

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 860, 600)];
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers = NO;

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
    _logWindow.contentView = scroll;
    [_logWindow center];

    [self refreshLog:nil];
    [_logWindow makeKeyAndOrderFront:nil];
    [self updateLogTimer];
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
