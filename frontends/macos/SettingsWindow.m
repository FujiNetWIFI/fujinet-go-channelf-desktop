/*
 * ChfSettingsWindow -- see the header.
 *
 * Same keys and defaults as the other frontends' Preferences, so a machine
 * configured in one comes up the same in another. The right stick, the
 * picture and the volume apply live; the machine, the BIOS and the host
 * options power-cycle the console (chfsession_restart) when the window
 * closes, which the app does when told the window closed with changes. The
 * keys and gamepad buttons are re-mapped in the Controllers window (Map).
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "SettingsWindow.h"

#import "help/HelpWindow.h"

#include <string.h>

#define WINDOW_W 640.0
#define WINDOW_H 540.0
#define NUM_PAGES 5

/* The Help topic of each page, in tab order. */
static NSString *const kPageContexts[NUM_PAGES] = {
    @"prefs/Machine", @"prefs/Display", @"prefs/Input", @"prefs/FujiNet", @"prefs/ROMs"
};

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

    NSPopUpButton *_machinePopup, *_biosPopup;
    NSTextField *_machineNote, *_biosNote;
    NSPopUpButton *_aspectPopup;
    NSButton *_smoothBox;
    NSPopUpButton *_padList, *_padPort;
    NSTextField *_webUiLabel, *_externalField;
    NSMutableArray<NSTextField *> *_romStates;
    NSTextField *_romPath;
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
 * left of a plain view, preceded by any extra views stacked above it, and the
 * page's Help button at the bottom left. */
- (NSView *)pageWithRows:(NSArray<NSArray<NSView *> *> *)rows extras:(NSArray<NSView *> *)extras
                    help:(NSString *)context
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

    NSButton *help = ChfHelpButton(context);
    help.translatesAutoresizingMaskIntoConstraints = NO;
    [holder addSubview:help];
    [help.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:20].active = YES;
    [help.bottomAnchor constraintEqualToAnchor:holder.bottomAnchor constant:-16].active = YES;
    return holder;
}

/* ---- the machine ---------------------------------------------------------------------- */

- (int)currentMachine
{
    const int m = chfsession_get_int(_session, "machine", CHF_MACHINE_CHANNELF);
    return m >= 0 && m < CHF_MACHINE_COUNT ? m : CHF_MACHINE_CHANNELF;
}

- (int)currentBios
{
    const int b = chfsession_get_int(_session, "bios", CHF_BIOS_AUTO);
    return b >= 0 && b < CHF_BIOS_CHOICE_COUNT ? b : CHF_BIOS_AUTO;
}

- (void)machineChanged:(NSPopUpButton *)sender
{
    const NSInteger sel = sender.indexOfSelectedItem;
    if (sel < 0 || sel >= CHF_MACHINE_COUNT) return;
    chfsession_set_int(_session, "machine", (int)sel);
    _dirty = YES;
    [self refresh];
}

- (void)biosChanged:(NSPopUpButton *)sender
{
    const NSInteger sel = sender.indexOfSelectedItem;
    if (sel < 0 || sel >= CHF_BIOS_CHOICE_COUNT) return;
    chfsession_set_int(_session, "bios", (int)sel);
    _dirty = YES;
    [self refresh];
}

/* What the machine is, and which BIOS the choice would run. */
- (void)refreshMachineStatus
{
    const int m = [self currentMachine];
    _machineNote.stringValue = chf_machine_is_pal(m)
        ? @"PAL: the F3850 at 2 MHz, 312 lines, 50 frames a second."
        : @"NTSC: the F3850 at 1.79 MHz, 264 lines, 60.5 frames a second.";
    _biosNote.stringValue = chfsession_have_bios(_session, m)
        ? @"A real BIOS is imported and runs at the next power cycle."
        : @"No real BIOS for this choice: the app's stand-in runs (CONFIG and FujiNet "
           @"apps only). Import the BIOS on the ROMs page for the Videocarts.";
}

- (NSView *)machinePage
{
    _machinePopup = [[NSPopUpButton alloc] init];
    for (int i = 0; i < CHF_MACHINE_COUNT; i++)
        [[_machinePopup menu] addItemWithTitle:nsstr(chf_machine_name(i)) action:nil keyEquivalent:@""];
    [_machinePopup selectItemAtIndex:[self currentMachine]];
    _machinePopup.target = self;
    _machinePopup.action = @selector(machineChanged:);

    _biosPopup = [[NSPopUpButton alloc] init];
    for (int i = 0; i < CHF_BIOS_CHOICE_COUNT; i++)
        [[_biosPopup menu] addItemWithTitle:nsstr(chf_bios_choice_name(i)) action:nil keyEquivalent:@""];
    [_biosPopup selectItemAtIndex:[self currentBios]];
    _biosPopup.target = self;
    _biosPopup.action = @selector(biosChanged:);

    _machineNote = [self note:@""];
    _biosNote = [self note:@""];
    [self refreshMachineStatus];

    NSArray<NSArray<NSView *> *> *rows = @[
        @[ [self heading:@"Machine"], [self note:@"applies with a power cycle when this window closes"] ],
        @[ [self label:@"Machine"], _machinePopup ],
        @[ [self label:@""], _machineNote ],
        @[ [self label:@"BIOS"], _biosPopup ],
        @[ [self label:@""], _biosNote ],
    ];
    return [self pageWithRows:rows extras:@[] help:kPageContexts[0]];
}

/* ---- display and audio ---------------------------------------------------------------------- */

- (void)aspectChanged:(NSPopUpButton *)sender
{
    [_delegate settingsSetAspect:(int)sender.indexOfSelectedItem];
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

- (NSView *)displayPage
{
    _aspectPopup = [[NSPopUpButton alloc] init];
    [_aspectPopup addItemsWithTitles:@[@"Television (4:3)", @"Square pixels"]];
    _aspectPopup.target = self;
    _aspectPopup.action = @selector(aspectChanged:);
    _smoothBox = [self checkBoxForKey:"smooth" title:@"Smooth scaling" fallback:0];
    [self syncDisplay];

    NSSlider *volume = [NSSlider sliderWithValue:chfsession_get_int(_session, "volume", 100)
                                        minValue:0 maxValue:100 target:self action:@selector(volumeChanged:)];
    [volume.widthAnchor constraintEqualToConstant:220].active = YES;

    NSArray<NSArray<NSView *> *> *rows = @[
        @[ [self heading:@"Display"], [self note:@"applied immediately"] ],
        @[ [self label:@"Aspect"], _aspectPopup ],
        @[ [self label:@""], _smoothBox ],
        @[ [self heading:@"Audio"], [self note:@"the volume applies immediately; audio on/off power-cycles"] ],
        @[ [self label:@""], [self checkBoxForKey:"enable_audio" title:@"Enable audio" fallback:1] ],
        @[ [self label:@"Volume"], volume ],
    ];
    return [self pageWithRows:rows extras:@[] help:kPageContexts[1]];
}

/* ---- input ------------------------------------------------------------------------------------ */

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
    const BOOL have = sel >= 0 && sel < chfsession_gamepad_count(_session);
    _padPort.enabled = have;
    if (have) [_padPort selectItemAtIndex:chfsession_gamepad_assignment(_session, (int)sel) + 1];
}

- (void)padPortChanged:(id)sender
{
    (void)sender;
    const NSInteger sel = _padList.indexOfSelectedItem;
    if (sel >= 0 && sel < chfsession_gamepad_count(_session))
        chfsession_gamepad_assign(_session, (int)sel, (int)_padPort.indexOfSelectedItem - 1);
    [self refreshPadList];
}

- (void)resetControls:(id)sender
{
    (void)sender;
    chfsession_bindings_reset(_session);
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

    NSButton *reset = [NSButton buttonWithTitle:@"Reset Controls" target:self action:@selector(resetControls:)];

    NSArray<NSArray<NSView *> *> *rows = @[
        @[ [self heading:@"Input"], [self note:@"applied immediately; gamepads on/off power-cycles"] ],
        @[ [self label:@""], [self checkBoxForKey:"enable_gamepad" title:@"Enable gamepads" fallback:1] ],
        @[ [self label:@""],
           [self checkBoxForKey:"right_stick" title:@"Right stick works the knob" fallback:1] ],
        @[ [self label:@"Gamepads"], padRow ],
        @[ [self label:@"Controls"], reset ],
        @[ [self label:@""], [self note:@"Keys and gamepad buttons are re-mapped in the Controllers window "
                                         @"(View ▸ Controllers, F9): click Map, choose a control, then "
                                         @"press the key or the gamepad button. Reset Controls puts the "
                                         @"defaults back."] ],
    ];
    return [self pageWithRows:rows extras:@[] help:kPageContexts[2]];
}

/* ---- FujiNet -------------------------------------------------------------------------------- */

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
        @[ [self label:@"Address"], _externalField ],
        @[ [self label:@"Web UI"], webUiRow ],
    ];
    return [self pageWithRows:rows extras:@[] help:kPageContexts[3]];
}

/* ---- the ROMs ------------------------------------------------------------------------------- */

- (void)importClicked:(NSButton *)sender
{
    [_delegate settingsImportRoms:sender.tag != 0];
}

- (void)refreshRoms
{
    for (NSUInteger i = 0; i < [_romStates count]; i++)
        _romStates[i].stringValue = chfsession_rom_item_available(_session, (int)i) ? @"Imported" : @"Not imported";
    _romPath.stringValue = [NSString stringWithFormat:@"Kept in %@", nsstr(chfsession_roms_path(_session))];
}

- (NSView *)romsPage
{
    NSTextField *intro = [self note:@"The console's BIOS is not part of the app. CONFIG and the FujiNet apps run "
                                    @"on the app's own stand-in; the Videocarts need the real one. Import your "
                                    @"own dumps from files, a zip or a folder; they are recognised by their "
                                    @"checksums, whatever they are called. A new BIOS takes effect at the next "
                                    @"power cycle."];
    intro.preferredMaxLayoutWidth = 560;
    NSButton *files = [NSButton buttonWithTitle:@"Import…" target:self action:@selector(importClicked:)];
    files.tag = 0;
    NSButton *folder = [NSButton buttonWithTitle:@"Import a Folder…" target:self action:@selector(importClicked:)];
    folder.tag = 1;
    NSStackView *buttons = [NSStackView stackViewWithViews:@[files, folder]];
    buttons.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    buttons.spacing = 8;
    _romPath = [self label:@""];
    _romPath.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    _romPath.textColor = NSColor.secondaryLabelColor;
    _romPath.selectable = YES;

    NSMutableArray<NSArray<NSView *> *> *rows = [NSMutableArray array];
    _romStates = [NSMutableArray array];
    NSString *group = nil;
    for (int i = 0; i < chfsession_rom_item_count(); i++) {
        const chf_rom_item *item = chfsession_rom_item(i);
        if (!item) {
            [_romStates addObject:[self label:@""]];
            continue;
        }
        if (!group || ![group isEqualToString:nsstr(item->group)]) {
            group = nsstr(item->group);
            [rows addObject:@[ [self heading:group], [self label:@""] ]];
        }
        NSTextField *state = [self label:@""];
        state.textColor = NSColor.secondaryLabelColor;
        [_romStates addObject:state];
        [rows addObject:@[ [self label:nsstr(item->name)], state ]];
    }
    return [self pageWithRows:rows extras:@[intro, buttons, _romPath] help:kPageContexts[4]];
}

/* ---- the window --------------------------------------------------------------------------------- */

- (void)refresh
{
    if (!_window) return;
    [self refreshMachineStatus];
    [self refreshWebUi];
    [self refreshRoms];
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

/* A checkbox. The picture and the right stick apply at once; every other
 * option takes a power cycle, when the window closes. */
- (void)settingChanged:(id)sender
{
    NSButton *box = sender;
    const char *key = [box.identifier UTF8String];
    const int value = box.state == NSControlStateValueOn ? 1 : 0;

    if (!strcmp(key, "right_stick")) {
        chfsession_set_right_stick(_session, value);
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

/* The Help button and Command+? open the page being shown. */
- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    const NSInteger i = [tabView indexOfTabViewItem:item];
    if (i >= 0 && i < NUM_PAGES) ChfSetHelpContext(_window, kPageContexts[i]);
}

- (void)show
{
    if (!_window) {
        NSTabView *tabs = [[NSTabView alloc] initWithFrame:NSMakeRect(0, 0, WINDOW_W, WINDOW_H)];
        NSArray *pages = @[ @[@"Machine", [self machinePage]],
                            @[@"Display & Audio", [self displayPage]],
                            @[@"Input", [self inputPage]], @[@"FujiNet", [self fujinetPage]],
                            @[@"ROMs", [self romsPage]] ];
        for (NSArray *page in pages) {
            NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:page[0]];
            item.label = page[0];
            item.view = page[1];
            [tabs addTabViewItem:item];
        }
        tabs.delegate = self;
        _window = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(0, 0, WINDOW_W, WINDOW_H)
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
                        backing:NSBackingStoreBuffered
                          defer:NO];
        _window.title = @"Settings";
        _window.releasedWhenClosed = NO;
        _window.delegate = self;
        _window.contentView = tabs;
        ChfSetHelpContext(_window, kPageContexts[0]);
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
