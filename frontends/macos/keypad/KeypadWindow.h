/*
 * The macOS Controllers window -- see KeypadWindow.m.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#import <Cocoa/Cocoa.h>

#include "astrosession.h"

@interface AstroControllers : NSObject
/* Show or hide the singleton window (View > Controllers, Cmd+K). */
+ (void)toggleForSession:(astrosession *)session;
+ (BOOL)isVisible;
@end

/* While Map mode waits for a key, binds it and returns YES. Called first by
 * the shared key path, so a remap keystroke is captured whichever window has
 * the focus. */
BOOL AstroControllersMapKey(uint32_t keysym);
