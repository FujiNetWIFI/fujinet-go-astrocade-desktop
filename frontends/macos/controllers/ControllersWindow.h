/*
 * The Controllers panel (F9 / Cmd+K / View menu): the console's 24-key keypad
 * (with the Bally BASIC overlay when it is on), the four pistol-grip hand
 * controllers with their knob dials, the two console buttons, the gamepads
 * with the player each plays, and Map mode for rebinding. Hidden at start; a
 * fixed-size floating NSPanel.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "astrosession.h"

/* The keys the app itself owns (F9, F12): called with the keysym of one typed
 * with the panel key; returns YES when it took it. */
typedef BOOL (^AstroAppKeyHandler)(uint32_t keysym);

@interface AstroControllersWindow : NSObject
- (instancetype)initWithSession:(astrosession *)session appKeys:(AstroAppKeyHandler)appKeys;
/* Runs a console action the panel's buttons fire (ASTROSESSION_SYSACT_*); the
 * app sets it so the action also shows its toast. Without one the session
 * runs it. */
@property (nonatomic, copy) void (^sysactionHandler)(int sysact);
/* Show, or hide when showing. */
- (void)toggle;
- (void)show;
- (void)hide;
- (BOOL)isVisible;
/* The settings changed (Settings): pads and bindings are read again. */
- (void)refresh;
@end

/* While Map mode waits for a key, binds it and returns YES. Called first by
 * the shared key path (AstroKeyForward), so a remap keystroke is captured
 * whichever window has the focus. F9, F11 and F12 are never taken. */
BOOL AstroControllersMapKey(uint32_t keysym);
