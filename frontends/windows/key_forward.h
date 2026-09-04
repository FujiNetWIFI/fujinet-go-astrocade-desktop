/*
 * key_forward.h -- translate a Win32 virtual-key to an ASTROSESSION_KEYSYM_*.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ASTRO_WIN_KEY_FORWARD_H
#define ASTRO_WIN_KEY_FORWARD_H

#include <windows.h>

#include "astrosession.h"

/* wparam is the VK code; for printable keys we map the common ones directly
 * (a full layout-aware translation would use ToUnicode, but the keypad only
 * needs digits/operators/letters). */
static int astro_keysym_from_vk(WPARAM vk)
{
    switch (vk) {
    case VK_UP:     return ASTROSESSION_KEYSYM_UP;
    case VK_DOWN:   return ASTROSESSION_KEYSYM_DOWN;
    case VK_LEFT:   return ASTROSESSION_KEYSYM_LEFT;
    case VK_RIGHT:  return ASTROSESSION_KEYSYM_RIGHT;
    case VK_RETURN: return ASTROSESSION_KEYSYM_RETURN;
    case VK_BACK:   return ASTROSESSION_KEYSYM_BACKSPACE;
    case VK_ESCAPE: return ASTROSESSION_KEYSYM_ESCAPE;
    case VK_SPACE:  return ASTROSESSION_KEYSYM_SPACE;
    case VK_OEM_2:  return '/';
    case VK_MULTIPLY: case VK_OEM_PLUS: /* handled below for shifted */ break;
    case VK_ADD:      return '+';
    case VK_SUBTRACT: case VK_OEM_MINUS: return '-';
    case VK_DIVIDE:   return '/';
    case VK_DECIMAL: case VK_OEM_PERIOD: return '.';
    default: break;
    }
    if (vk >= '0' && vk <= '9') return (int)vk;                 /* digits */
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return '0' + (int)(vk - VK_NUMPAD0);
    if (vk >= 'A' && vk <= 'Z') return (int)(vk - 'A' + 'a');   /* letters */
    return ASTROSESSION_KEYSYM_NONE;
}

#endif
