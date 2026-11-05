/*
 * KeyForward -- see the header.
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "KeyForward.h"

#import <Carbon/Carbon.h>   /* kVK_* virtual key codes */

#include "chfsession.h"

uint32_t ChfKeysymFromEvent(NSEvent *event)
{
    const uint32_t ks = chfsession_keysym_from_macos_keycode([event keyCode]);
    if (ks) return ks;

    switch ([event keyCode]) {
    case kVK_UpArrow:    return CHF_KEYSYM_UP;
    case kVK_DownArrow:  return CHF_KEYSYM_DOWN;
    case kVK_LeftArrow:  return CHF_KEYSYM_LEFT;
    case kVK_RightArrow: return CHF_KEYSYM_RIGHT;
    case kVK_Tab:        return CHF_KEYSYM_TAB;
    case kVK_Escape:     return CHF_KEYSYM_ESCAPE;
    case kVK_Return:     return CHF_KEYSYM_RETURN;
    case kVK_Space:      return CHF_KEYSYM_SPACE;
    case kVK_Delete:     return CHF_KEYSYM_BACKSPACE;
    default: break;
    }

    /* Anything the table does not know: the character typed. */
    NSString *chars = [event charactersIgnoringModifiers];
    if ([chars length] == 1) {
        const unichar c = [chars characterAtIndex:0];
        if (c >= 0x20 && c <= 0x7e) {
            if (c >= 'A' && c <= 'Z') return (uint32_t)(c - 'A' + 'a');
            return (uint32_t)c;
        }
        if (c >= NSF1FunctionKey && c <= NSF12FunctionKey)
            return CHF_KEYSYM_F1 + (uint32_t)(c - NSF1FunctionKey);
    }
    return 0;
}

/* The device-dependent bits of an event's modifier flags, one per side (IOKit's
 * NX_DEVICE*KEYMASK): the pair's own flag stays set while either side is held,
 * so a release of one with the other down would read as a press. */
#define CHF_DEV_LCTL   0x00000001UL
#define CHF_DEV_LSHIFT 0x00000002UL
#define CHF_DEV_RSHIFT 0x00000004UL
#define CHF_DEV_LALT   0x00000020UL
#define CHF_DEV_RALT   0x00000040UL
#define CHF_DEV_RCTL   0x00002000UL
#define CHF_DEV_ANY    (CHF_DEV_LCTL | CHF_DEV_LSHIFT | CHF_DEV_RSHIFT | CHF_DEV_LALT | CHF_DEV_RALT | CHF_DEV_RCTL)

/* Whether a modifier key is down: its group flag is set and, when the event
 * carries the per-side bits (a synthetic one may not), its own bit is. */
static int modifierDown(NSUInteger flags, NSUInteger group, NSUInteger side)
{
    if (!(flags & group)) return 0;
    if (!(flags & CHF_DEV_ANY)) return 1;
    return (flags & side) != 0;
}

/* Modifier keys arrive as flagsChanged, not keyDown/keyUp. The key code
 * says which key; its flag says whether it went down. */
uint32_t ChfKeysymFromFlagsChange(NSEvent *event, int *down)
{
    const NSUInteger f = (NSUInteger)[event modifierFlags];
    switch ([event keyCode]) {
    case kVK_Shift:        *down = modifierDown(f, NSEventModifierFlagShift, CHF_DEV_LSHIFT);   return CHF_KEYSYM_LSHIFT;
    case kVK_RightShift:   *down = modifierDown(f, NSEventModifierFlagShift, CHF_DEV_RSHIFT);   return CHF_KEYSYM_RSHIFT;
    case kVK_Control:      *down = modifierDown(f, NSEventModifierFlagControl, CHF_DEV_LCTL);   return CHF_KEYSYM_LCTRL;
    case kVK_RightControl: *down = modifierDown(f, NSEventModifierFlagControl, CHF_DEV_RCTL);   return CHF_KEYSYM_RCTRL;
    /* Option is where a Mac keyboard puts the key a PC calls Alt; a binding
     * can sit on it like on Shift and Control. */
    case kVK_Option:       *down = modifierDown(f, NSEventModifierFlagOption, CHF_DEV_LALT);   return CHF_KEYSYM_LALT;
    case kVK_RightOption:  *down = modifierDown(f, NSEventModifierFlagOption, CHF_DEV_RALT);   return CHF_KEYSYM_RALT;
    default: *down = 0; return 0;
    }
}

static NSColor *colorFromRgb(uint32_t rgb)
{
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xff) / 255.0
                               green:((rgb >> 8) & 0xff) / 255.0
                                blue:(rgb & 0xff) / 255.0
                               alpha:1.0];
}

NSColor *ChfAccentColor(void)
{
    return colorFromRgb(CHFSESSION_ACCENT_RGB);
}

NSColor *ChfAccentTextColor(void)
{
    static NSColor *color;
    if (!color) {
        /* Resolved per appearance at draw time, so it follows a switch
         * between light and dark while the app runs. */
        color = [NSColor colorWithName:@"ChfAccentText" dynamicProvider:^NSColor *(NSAppearance *a) {
            NSAppearanceName best = [a bestMatchFromAppearancesWithNames:@[NSAppearanceNameAqua,
                                                                           NSAppearanceNameDarkAqua]];
            return colorFromRgb([best isEqualToString:NSAppearanceNameDarkAqua]
                                    ? CHFSESSION_ACCENT_RGB : CHFSESSION_ACCENT_DARK_RGB);
        }];
    }
    return color;
}
