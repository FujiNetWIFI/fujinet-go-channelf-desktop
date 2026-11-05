/*
 * ChfSettingsWindow -- see the header.
 *
 * Same keys and defaults as the other frontends' Preferences, so a machine
 * configured in one comes up the same in another. The analog stick, the
 * picture and the volume apply live; the machine and the
 * host options power-cycle the console (chfsession_restart) when the window
 * closes, which the app does when told the window closed with changes.
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "SettingsWindow.h"

#include <string.h>

#define WINDOW_W 640.0
#define WINDOW_H 540.0

static NSString *nsstr(const char *s)
{
    NSString *r = s ? [NSString stringWithUTF8String:s] : nil;
    return r ? r : @"";
}

@implementation ChfSettingsWindow {
    chfsession *_session;
    __weak id<ChfSettingsDelegate> _delegate;
    NSWindow *_window;
    BOOL _dirty;                 /* an option changed that a power cycle applies */

    NSPopUpButton *_machinePopup;
    NSTextField *_machineNote;
    NSPopUpButton *_aspectPopup;
    NSButton *_smoothBox;
    NSPopUpButton *_padList, *_padPort;
    NSTextField *_webUiLabel, *_externalField;
    NSTimer *_padTimer;
    unsigned _padGeneration;
}

- (instancetype)initWithSession:(chfsession *)session delegate:(id<ChfSettingsDelegate>)delegate
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _delegate = delegate;
    return self;
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [_padTimer invalidate];
}

/* ---- small builders ----------------------------------------------------------------- */

- (NSTextField *)label:(NSString *)text
{
    return [NSTextField labelWithString:text];
}

- (NSTextField *)heading:(NSString *)text
{
    NSTextField *l = [NSTextField labelWithString:text];
    l.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
    return l;
}

- (NSTextField *)note:(NSString *)text
{
    NSTextField *n = [NSTextField wrappingLabelWithString:text];
    n.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    n.textColor = NSColor.secondaryLabelColor;
    n.preferredMaxLayoutWidth = 400;
    return n;
}

- (NSButton *)checkBoxForKey:(const char *)key title:(NSString *)title fallback:(int)def
{
    NSButton *box = [NSButton checkboxWithTitle:title target:self action:@selector(settingChanged:)];
    box.state = chfsession_get_int(_session, key, def) ? NSControlStateValueOn : NSControlStateValueOff;
    box.identifier = nsstr(key);
    return box;
}

/* A page: a two-column grid (labels trailing, controls leading) at the top
 * left of a plain view, followed by any extra views stacked under it. */
- (NSView *)pageWithRows:(NSArray<NSArray<NSView *> *> *)rows extras:(NSArray<NSView *> *)extras
{
    NSGridView *grid = [NSGridView gridViewWithViews:rows];
    grid.rowSpacing = 10;
    grid.columnSpacing = 12;
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;

    NSMutableArray<NSView *> *views = [NSMutableArray array];
    for (NSView *v in extras) [views addObject:v];
    [views addObject:grid];
    NSStackView *stack = [NSStackView stackViewWithViews:views];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 12;
    stack.translatesAutoresizingMaskIntoConstraints = NO;

    NSView *holder = [[NSView alloc] init];
    [holder addSubview:stack];
    [stack.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:20].active = YES;
    [stack.topAnchor constraintEqualToAnchor:holder.topAnchor constant:20].active = YES;
    return holder;
}

/* ---- the machine ---------------------------------------------------------------------- */

- (int)currentMachine
{
    const int m = chfsession_get_int(_session, "machine", CHF_MACHINE_ARCADIA);
    return m >= 0 && m < CHF_MACHINE_COUNT ? m : CHF_MACHINE_ARCADIA;
}

- (void)machineChanged:(NSPopUpButton *)sender
{
    const NSInteger sel = sender.indexOfSelectedItem;
    if (sel < 0 || sel >= CHF_MACHINE_COUNT) return;
    chfsession_set_int(_session, "machine", (int)sel);
    _dirty = YES;
    [self refresh];
}

/* The PAL clone is the app's model of the Hanimex/Leisure-Vision class; MAME
 * itself runs every clone at NTSC, so it is not checked against MAME. */
- (void)refreshMachineStatus
{
    _machineNote.stringValue = chf_machine_is_pal([self currentMachine])
        ? @"PAL clone: 4.433 MHz / 5, 50 Hz, 312 lines. Not verified against MAME."
        : @"NTSC: 3.58 MHz / 4, 60 Hz. The console has no BIOS to import.";
}

- (NSView *)machinePage
{
    _machinePopup = [[NSPopUpButton alloc] init];
    for (int i = 0; i < CHF_MACHINE_COUNT; i++)
        [[_machinePopup menu] addItemWithTitle:nsstr(chf_machine_name(i)) action:nil keyEquivalent:@""];
    [_machinePopup selectItemAtIndex:[self currentMachine]];
    _machinePopup.target = self;
    _machinePopup.action = @selector(machineChanged:);

    _machineNote = [self note:@""];
    [self refreshMachineStatus];

    NSArray<NSArray<NSView *> *> *rows = @[
        @[ [self heading:@"Machine"], [self note:@"applies with a power cycle when this window closes"] ],
        @[ [self label:@"Console"], _machinePopup ],
        @[ [self label:@""], _machineNote ],
    ];
    return [self pageWithRows:rows extras:@[]];
}

/* ---- display, audio, input, FujiNet --------------------------------------------------------- */

- (void)aspectChanged:(NSPopUpButton *)sender
{
    [_delegate settingsSetAspect:(int)sender.indexOfSelectedItem];
}

- (NSView *)displayPage
{
    _aspectPopup = [[NSPopUpButton alloc] init];
    [_aspectPopup addItemsWithTitles:@[@"TV (4:3)", @"Square pixels"]];
    _aspectPopup.target = self;
    _aspectPopup.action = @selector(aspectChanged:);
    _smoothBox = [self checkBoxForKey:"smooth" title:@"Smooth scaling" fallback:0];
    [self syncDisplay];
    NSArray<NSArray<NSView *> *> *rows = @[
        @[ [self heading:@"Display"], [self note:@"applied immediately"] ],
        @[ [self label:@"Picture"], _aspectPopup ],
        @[ [self label:@""], _smoothBox ],
    ];
    return [self pageWithRows:rows extras:@[]];
}

- (void)syncDisplay
{
    [_aspectPopup selectItemAtIndex:(chfsession_get_int(_session, "aspect", 0) ? 1 : 0)];
    _smoothBox.state = chfsession_get_int(_session, "smooth", 0) ? NSControlStateValueOn : NSControlStateValueOff;
}

- (void)volumeChanged:(NSSlider *)sender
{
    chfsession_set_volume(_session, (int)sender.integerValue);
}

- (NSView *)audioPage
{
    NSSlider *volume = [NSSlider sliderWithValue:chfsession_get_int(_session, "volume", 100)
                                        minValue:0 maxValue:100 target:self action:@selector(volumeChanged:)];
    [volume.widthAnchor constraintEqualToConstant:220].active = YES;
    NSArray<NSArray<NSView *> *> *rows = @[
        @[ [self heading:@"Audio"], [self note:@"the volume applies immediately; audio on/off power-cycles"] ],
        @[ [self label:@""], [self checkBoxForKey:"enable_audio" title:@"Audio" fallback:1] ],
        @[ [self label:@"Volume"], volume ],
    ];
    return [self pageWithRows:rows extras:@[]];
}

- (void)refreshPadList
{
    const NSInteger sel = _padList.indexOfSelectedItem;
    [_padList removeAllItems];
    const int n = chfsession_gamepad_count(_session);
    if (n == 0) [[_padList menu] addItemWithTitle:@"(no gamepads connected)" action:nil keyEquivalent:@""];
    for (int i = 0; i < n; i++) {
        char name[128];
        name[0] = '\0';
        const int eff = chfsession_gamepad_effective_port(_session, i);
        chfsession_gamepad_name(_session, i, name, (int)sizeof name);
        /* through the menu: a button's own addItemWithTitle: drops an earlier
         * item of the same title, and two pads can share a name */
        [[_padList menu] addItemWithTitle:(eff >= 0
            ? [NSString stringWithFormat:@"%d: %s  [player %d]", i + 1, name, eff + 1]
            : [NSString stringWithFormat:@"%d: %s  [unused]", i + 1, name])
                                   action:nil keyEquivalent:@""];
    }
    if (sel >= 0 && sel < n) [_padList selectItemAtIndex:sel];
    [self padSelected:nil];
}

- (void)padSelected:(id)sender
{
    (void)sender;
    const NSInteger sel = _padList.indexOfSelectedItem;
    if (sel >= 0 && sel < chfsession_gamepad_count(_session))
        [_padPort selectItemAtIndex:chfsession_gamepad_assignment(_session, (int)sel) + 1];
}

- (void)padPortChanged:(id)sender
{
    (void)sender;
    const NSInteger sel = _padList.indexOfSelectedItem;
    if (sel >= 0 && sel < chfsession_gamepad_count(_session))
        chfsession_gamepad_assign(_session, (int)sel, (int)_padPort.indexOfSelectedItem - 1);
    [self refreshPadList];
}

- (NSView *)inputPage
{
    _padList = [[NSPopUpButton alloc] init];
    _padList.target = self;
    _padList.action = @selector(padSelected:);
    _padPort = [[NSPopUpButton alloc] init];
    [_padPort addItemsWithTitles:@[@"Automatic", @"Player 1", @"Player 2"]];
    _padPort.target = self;
    _padPort.action = @selector(padPortChanged:);
    NSStackView *padRow = [NSStackView stackViewWithViews:@[_padList, _padPort]];
    padRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;

    NSArray<NSArray<NSView *> *> *rows = @[
        @[ [self heading:@"Input"], [self note:@"applied immediately; gamepads on/off power-cycles"] ],
        @[ [self label:@""],
           [self checkBoxForKey:"analog_joystick" title:@"Analog sticks steer the disc" fallback:1] ],
        @[ [self label:@""], [self checkBoxForKey:"enable_gamepad" title:@"Gamepads" fallback:1] ],
        @[ [self label:@"Gamepads"], padRow ],
        @[ [self label:@""], [self note:@"Keys and gamepad buttons are re-mapped in the Controllers window (Map)."] ],
    ];
    return [self pageWithRows:rows extras:@[]];
}

/* The external-FujiNet address field: valid text (or none, meaning
 * 127.0.0.1:11526) is saved; anything else is shown in red and not saved. */
- (void)externalAddrChanged:(id)sender
{
    NSTextField *field = sender;
    NSString *text = [field.stringValue
        stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    char host[64];
    int port;
    if (chfsession_parse_hostport(text.UTF8String, host, (int)sizeof host, &port) != 0) {
        field.textColor = NSColor.systemRedColor;
        return;
    }
    field.textColor = NSColor.controlTextColor;
    chfsession_set_str(_session, "fujinet_external_addr", text.UTF8String);
    _dirty = YES;
}

- (void)openWebUI:(id)sender
{
    (void)sender;
    [_delegate settingsOpenWebUI];
}

- (void)refreshWebUi
{
    if (!_webUiLabel) return;
    const char *url = chfsession_fujinet_webui_url(_session);
    _webUiLabel.stringValue = chfsession_fujinet_external(_session)
        ? @"(FujiNet is external)"
        : (chfsession_fujinet_running(_session) && url && url[0])
            ? nsstr(url) : @"(FujiNet is not running)";
}

- (NSView *)fujinetPage
{
    _externalField = [NSTextField textFieldWithString:nsstr(chfsession_get_str(_session, "fujinet_external_addr", ""))];
    _externalField.placeholderString = @"127.0.0.1:11526";
    [_externalField.widthAnchor constraintEqualToConstant:240].active = YES;
    _externalField.enabled = chfsession_get_int(_session, "fujinet_external", 0) != 0;
    _externalField.target = self;
    _externalField.action = @selector(externalAddrChanged:);
    [_externalField.cell setSendsActionOnEndEditing:YES];

    _webUiLabel = [self label:@""];
    _webUiLabel.selectable = YES;
    NSButton *open = [NSButton buttonWithTitle:@"Open" target:self action:@selector(openWebUI:)];
    NSStackView *webUiRow = [NSStackView stackViewWithViews:@[_webUiLabel, open]];
    webUiRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;

    NSArray<NSArray<NSView *> *> *rows = @[
        @[ [self heading:@"FujiNet"], [self note:@"applied by power-cycling when this window closes"] ],
        @[ [self label:@""], [self checkBoxForKey:"enable_fujinet" title:@"Enable FujiNet" fallback:1] ],
        @[ [self label:@""], [self checkBoxForKey:"fujinet_external" title:@"Use external FujiNet" fallback:0] ],
        @[ [self label:@"Address:port"], _externalField ],
        @[ [self label:@"Web UI"], webUiRow ],
    ];
    return [self pageWithRows:rows extras:@[]];
}

/* A checkbox. The picture and the analog stick apply at once; every other
 * option takes a power cycle, when the window closes. */
- (void)settingChanged:(id)sender
{
    NSButton *box = sender;
    const char *key = [box.identifier UTF8String];
    const int value = box.state == NSControlStateValueOn ? 1 : 0;

    if (!strcmp(key, "analog_joystick")) {
        chfsession_set_analog(_session, value);
        chfsession_set_int(_session, key, value);
        return;
    }
    if (!strcmp(key, "smooth")) {
        [_delegate settingsSetSmooth:value != 0];
        return;
    }
    chfsession_set_int(_session, key, value);
    _dirty = YES;
    if (!strcmp(key, "fujinet_external")) _externalField.enabled = value != 0;
}

/* ---- the window --------------------------------------------------------------------------------- */

- (void)refresh
{
    if (!_window) return;
    [self refreshMachineStatus];
    [self refreshWebUi];
}

/* Hot-plugged gamepads, twice a second, while the window can be seen. */
- (void)updatePadTimer
{
    const BOOL want = _window && [_window isVisible] && ![_window isMiniaturized] &&
                      ([_window occlusionState] & NSWindowOcclusionStateVisible);
    if (want && !_padTimer) {
        _padGeneration = chfsession_gamepad_generation(_session);
        [self refreshPadList];
        __weak ChfSettingsWindow *weakSelf = self;
        _padTimer = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t) {
            (void)t;
            ChfSettingsWindow *s = weakSelf;
            if (!s) return;
            const unsigned gen = chfsession_gamepad_generation(s->_session);
            if (gen != s->_padGeneration) {
                s->_padGeneration = gen;
                [s refreshPadList];
            }
        }];
        _padTimer.tolerance = 0.25;
    } else if (!want && _padTimer) {
        [_padTimer invalidate];
        _padTimer = nil;
    }
}

- (void)visibilityChanged:(NSNotification *)note
{
    (void)note;
    [self updatePadTimer];
}

- (void)show
{
    if (!_window) {
        NSTabView *tabs = [[NSTabView alloc] initWithFrame:NSMakeRect(0, 0, WINDOW_W, WINDOW_H)];
        NSArray *pages = @[ @[@"Machine", [self machinePage]],
                            @[@"Display", [self displayPage]], @[@"Audio", [self audioPage]],
                            @[@"Input", [self inputPage]], @[@"FujiNet", [self fujinetPage]] ];
        for (NSArray *page in pages) {
            NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:page[0]];
            item.label = page[0];
            item.view = page[1];
            [tabs addTabViewItem:item];
        }
        _window = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(0, 0, WINDOW_W, WINDOW_H)
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
                        backing:NSBackingStoreBuffered
                          defer:NO];
        _window.title = @"Settings";
        _window.releasedWhenClosed = NO;
        _window.delegate = self;
        _window.contentView = tabs;
        [_window center];
        for (NSString *name in @[NSWindowDidChangeOcclusionStateNotification,
                                 NSWindowDidMiniaturizeNotification, NSWindowDidDeminiaturizeNotification])
            [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(visibilityChanged:)
                                                         name:name object:_window];
    }
    [self refresh];
    [self refreshPadList];
    [_window makeKeyAndOrderFront:nil];
    [self updatePadTimer];
}

- (void)windowWillClose:(NSNotification *)note
{
    if (note.object != _window) return;
    [_window makeFirstResponder:nil];   /* commit a field edit */
    [_padTimer invalidate];
    _padTimer = nil;
    const BOOL dirty = _dirty;
    _dirty = NO;
    [_delegate settingsClosed:dirty];
}
@end
