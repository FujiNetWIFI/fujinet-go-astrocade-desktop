/*
 * AstroKeyForward -- see AstroKeyForward.h. NOT RUN-VERIFIED on macOS here.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#import "AstroKeyForward.h"

#include "bindings.h"

int AstroKeysymFromEvent(NSEvent *event)
{
    switch (event.keyCode) {
    case 126: return ASTROSESSION_KEYSYM_UP;
    case 125: return ASTROSESSION_KEYSYM_DOWN;
    case 123: return ASTROSESSION_KEYSYM_LEFT;
    case 124: return ASTROSESSION_KEYSYM_RIGHT;
    case 36:  case 76: return ASTROSESSION_KEYSYM_RETURN;   /* Return / KP-Enter */
    case 51:  return ASTROSESSION_KEYSYM_BACKSPACE;
    case 53:  return ASTROSESSION_KEYSYM_ESCAPE;
    case 49:  return ASTROSESSION_KEYSYM_SPACE;
    default: break;
    }
    /* printable characters pass through as ASCII */
    NSString *chars = event.charactersIgnoringModifiers;
    if (chars.length == 1) {
        unichar c = [chars characterAtIndex:0];
        if (c >= 0x20 && c < 0x7f)
            return (int)c;
    }
    return ASTROSESSION_KEYSYM_NONE;
}

void AstroForwardKeyEvent(astrosession *session, NSEvent *event, int down)
{
    int keysym = AstroKeysymFromEvent(event);
    astro_mapping m = bindings_resolve_keysym(keysym);
    if (m.kind == ASTRO_MAP_SYSACT) {
        if (down)
            astrosession_sysaction_fire(session, (astrosession_sysaction)m.value);
        return;
    }
    if (m.kind == ASTRO_MAP_KEY)
        astrosession_keypad_set(session, (astrosession_key)m.value, down);
}
