/*
 * AstroKeyForward -- translate an NSEvent key to the session's keysym and
 * dispatch it through the bound key table. Shared by the display and keypad.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#import <Cocoa/Cocoa.h>

#include "astrosession.h"

/* Forward a key event to the machine (or a Map-mode remap). */
void AstroForwardKeyEvent(astrosession *session, NSEvent *event, int down);

/* Resolve an NSEvent to an ASTROSESSION_KEYSYM_* value. */
int AstroKeysymFromEvent(NSEvent *event);
