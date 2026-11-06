/*
 * AstroKeyForward -- translate an NSEvent key to the session's keysym (the
 * PHYSICAL key, by its macOS virtual key code) and dispatch it: Map capture
 * first, then the bound key table. Shared by the display and the Controllers
 * window.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#import <Cocoa/Cocoa.h>

#include "astrosession.h"

/* Forward a key press or release. YES when the key was consumed. A key typed
 * with Command held is never consumed (a menu equivalent; no key-up arrives). */
BOOL AstroForwardKeyEvent(astrosession *session, NSEvent *event, int down);

/* Shift, Control, Option and Caps Lock arrive as flagsChanged, not keyDown. */
void AstroForwardModifierEvent(astrosession *session, NSEvent *event);

/* The keysym for an NSEvent's virtual key code, 0 when unknown. */
uint32_t AstroKeysymFromEvent(NSEvent *event);
