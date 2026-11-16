/*
 * DisplayExtras.inc -- the picture's shape comes from the machine and the
 * "aspect" setting (chfsession_display_aspect), not from a TV on/off flag.
 * Included into the display view's @implementation (see DisplayGlue.h).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

- (void)extraInit {}

/* The picture is 204 x 232 (MAME's visible area: 102 x 58 cells, 2 pixels wide
 * and 4 lines tall), shown at the television's 4:3 or with square pixels. */
- (void)setMachine:(int)machine squarePixels:(BOOL)square
{
    int num = 4, den = 3;
    chfsession_display_aspect(machine, square ? 1 : 0, &num, &den);
    atomic_store(&s_fn_aspect_num, num > 0 ? num : 4);
    atomic_store(&s_fn_aspect_den, den > 0 ? den : 3);
    atomic_store(&_redraw, 1);
    [self setNeedsDisplay:YES];
}
