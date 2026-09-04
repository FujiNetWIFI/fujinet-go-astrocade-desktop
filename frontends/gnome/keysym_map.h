/*
 * keysym_map.h -- translate a GTK key event to an ASTROSESSION_KEYSYM_*
 * value (the session's own private numbering, NOT the raw GDK keyval -- see
 * astro_keymap.c's header for the trap that avoids). Printable ASCII passes
 * through as itself; named keys map to the ASTROSESSION_KEYSYM_* symbols.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ASTRO_GNOME_KEYSYM_MAP_H
#define ASTRO_GNOME_KEYSYM_MAP_H

#include <gtk/gtk.h>

#include "astrosession.h"

static inline int astro_keysym_from_key_event(guint keyval)
{
    switch (keyval) {
    case GDK_KEY_Up:        return ASTROSESSION_KEYSYM_UP;
    case GDK_KEY_Down:      return ASTROSESSION_KEYSYM_DOWN;
    case GDK_KEY_Left:      return ASTROSESSION_KEYSYM_LEFT;
    case GDK_KEY_Right:     return ASTROSESSION_KEYSYM_RIGHT;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:  return ASTROSESSION_KEYSYM_RETURN;
    case GDK_KEY_BackSpace: return ASTROSESSION_KEYSYM_BACKSPACE;
    case GDK_KEY_Escape:    return ASTROSESSION_KEYSYM_ESCAPE;
    case GDK_KEY_space:     return ASTROSESSION_KEYSYM_SPACE;

    /* numeric keypad -> the digits/operators they print */
    case GDK_KEY_KP_0: return '0';
    case GDK_KEY_KP_1: return '1';
    case GDK_KEY_KP_2: return '2';
    case GDK_KEY_KP_3: return '3';
    case GDK_KEY_KP_4: return '4';
    case GDK_KEY_KP_5: return '5';
    case GDK_KEY_KP_6: return '6';
    case GDK_KEY_KP_7: return '7';
    case GDK_KEY_KP_8: return '8';
    case GDK_KEY_KP_9: return '9';
    case GDK_KEY_KP_Divide:   return '/';
    case GDK_KEY_KP_Multiply: return '*';
    case GDK_KEY_KP_Subtract: return '-';
    case GDK_KEY_KP_Add:      return '+';
    case GDK_KEY_KP_Decimal:  return '.';
    default: break;
    }

    /* printable ASCII passes through as its own value */
    guint uni = gdk_keyval_to_unicode(keyval);
    if (uni >= 0x20 && uni < 0x7f)
        return (int)uni;
    return ASTROSESSION_KEYSYM_NONE;
}

#endif /* ASTRO_GNOME_KEYSYM_MAP_H */
