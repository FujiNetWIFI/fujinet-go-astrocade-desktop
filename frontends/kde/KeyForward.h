/*
 * Qt key events to the session's keysym space, and the one key path every
 * window that types on the Astrocade shares (the display and the Controllers
 * window).
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

class KeypadWindow;

/* The key's keysym, or 0 when it has none: the PHYSICAL key through its evdev
 * code (so Shift cannot turn 8 into '*', and NumLock cannot turn Keypad 8
 * into Up), else Qt's own key code. */
uint32_t astroKeysymFromQt(const QKeyEvent *event);

/* Press or release: Map capture first (press only), then the bindings. True
 * when the event was consumed. `keypad` may be null. */
bool astroForwardKey(astrosession *session, KeypadWindow *keypad,
                     QKeyEvent *event, bool down);

#endif
