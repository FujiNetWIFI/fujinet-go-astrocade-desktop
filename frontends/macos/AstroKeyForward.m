/*
 * AstroKeyForward -- see AstroKeyForward.h. NOT RUN-VERIFIED on macOS here.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#import "AstroKeyForward.h"

#include "bindings.h"

BOOL AstroControllersMapKey(uint32_t keysym);   /* keypad/KeypadWindow.m */

uint32_t AstroKeysymFromEvent(NSEvent *event)
{
    return astrosession_keysym_from_macos_keycode(event.keyCode);
}

BOOL AstroForwardKeyEvent(astrosession *session, NSEvent *event, int down)
{
    const uint32_t ks = AstroKeysymFromEvent(event);
    BOOL r;
    if (down && AstroControllersMapKey(ks))
        return YES;
    if (down && (event.modifierFlags & NSEventModifierFlagCommand))
        return NO;
    r = astrosession_input_key(session, ks, down,
                               (event.modifierFlags & NSEventModifierFlagControl) != 0) != 0;
    astrosession_sysaction_drain(session);
    return r;
}

void AstroForwardModifierEvent(astrosession *session, NSEvent *event)
{
    NSEventModifierFlags f = event.modifierFlags;
    BOOL down;
    switch (event.keyCode) {
    case 56: case 60: down = (f & NSEventModifierFlagShift) != 0; break;
    case 59: case 62: down = (f & NSEventModifierFlagControl) != 0; break;
    case 58: case 61: down = (f & NSEventModifierFlagOption) != 0; break;
    case 57:          down = (f & NSEventModifierFlagCapsLock) != 0; break;
    default: return;
    }
    const uint32_t ks = AstroKeysymFromEvent(event);
    if (down && AstroControllersMapKey(ks))
        return;
    astrosession_input_key(session, ks, down, 0);
    astrosession_sysaction_drain(session);
}
