/*
 * astro_keymap.c -- the default keyboard -> keypad mapping, a pure function
 * shared by every frontend (a frontend translates its toolkit key event to
 * an ASTROSESSION_KEYSYM_* symbol, then looks the keypad key up here). Its
 * own private numbering, NOT raw GDK/Qt/VK values -- passing a toolkit's
 * native keyval through would work for ASCII letters by coincidence and
 * silently break arrows/modifiers, the trap every sibling port documents.
 *
 * The Astrocade's keypad is a calculator, so the digits and operators map to
 * their obvious keyboard equivalents; the two arrow keys map to the console's
 * up/down keys, and the letters C / E map to the C and CE keys.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "astrosession.h"

int astrosession_key_from_keysym(int keysym)
{
    switch (keysym) {
    /* digits */
    case '0': return ASTROSESSION_KEY_0;
    case '1': return ASTROSESSION_KEY_1;
    case '2': return ASTROSESSION_KEY_2;
    case '3': return ASTROSESSION_KEY_3;
    case '4': return ASTROSESSION_KEY_4;
    case '5': return ASTROSESSION_KEY_5;
    case '6': return ASTROSESSION_KEY_6;
    case '7': return ASTROSESSION_KEY_7;
    case '8': return ASTROSESSION_KEY_8;
    case '9': return ASTROSESSION_KEY_9;

    /* operators (the gold column) */
    case '/': return ASTROSESSION_KEY_DIV;
    case '*': return ASTROSESSION_KEY_MUL;
    case '-': return ASTROSESSION_KEY_MINUS;
    case '+': return ASTROSESSION_KEY_PLUS;
    case '=': case ASTROSESSION_KEYSYM_RETURN: return ASTROSESSION_KEY_EQ;
    case '%': return ASTROSESSION_KEY_PCT;
    case '.': return ASTROSESSION_KEY_DOT;

    /* named keys */
    case 'c': case 'C': return ASTROSESSION_KEY_C;
    case ASTROSESSION_KEYSYM_UP:   return ASTROSESSION_KEY_UP;
    case ASTROSESSION_KEYSYM_DOWN: return ASTROSESSION_KEY_DOWN;

    /* MR / MS / CH memory keys, on their initials */
    case 'r': case 'R': return ASTROSESSION_KEY_MR;
    case 's': case 'S': return ASTROSESSION_KEY_MS;
    case 'h': case 'H': return ASTROSESSION_KEY_CH;

    /* CE on Escape or 'e' */
    case 'e': case 'E': case ASTROSESSION_KEYSYM_ESCAPE:
        return ASTROSESSION_KEY_CE;

    default: return -1;
    }
}
