/*
 * Qt key events to the session's keysym space, and the one key path every
 * window that types on the Astrocade shares (the display and the Controllers
 * window): the bindings first, then -- with the Bally BASIC overlay on -- the
 * character typed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ASTRO_KDE_KEYFORWARD_H
#define ASTRO_KDE_KEYFORWARD_H

#include <QKeyEvent>
#include <cstdint>

extern "C" {
#include "astrosession.h"
}

/* The key's keysym, or 0 when it has none: the PHYSICAL key through its evdev
 * code (so Shift cannot turn 8 into '*', and NumLock cannot turn Keypad 8
 * into Up), else Qt's own key code. */
uint32_t astroKeysymFromQt(const QKeyEvent *event);

/* Press or release through the session's bindings. A console action a key is
 * bound to is posted: the main window's timer runs it. With the Bally BASIC
 * overlay on, a key the bindings did not take (a printable bound to a keypad
 * key, which types its character instead) is typed on the keypad with
 * astrosession_type_char, and Return types GO. True when the event was
 * consumed. */
bool astroForwardKey(astrosession *session, QKeyEvent *event, bool down);

/* The app's own keys: F9 (Controllers), F10 (menu), F11 (fullscreen) and F12
 * (Debugger). They are never typed on the Astrocade and cannot be bound. */
bool astroIsAppKey(const QKeyEvent *event);

#endif
