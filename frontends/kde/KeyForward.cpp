/*
 * KeyForward -- see KeyForward.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KeyForward.h"
#include "keypad/KeypadWindow.h"

uint32_t astroKeysymFromQt(const QKeyEvent *event)
{
    /* On X11 and Wayland Qt reports the X keycode: the evdev code plus 8. */
    const quint32 sc = event->nativeScanCode();
    if (sc >= 8) {
        const uint32_t ks = astrosession_keysym_from_evdev(sc - 8);
        if (ks)
            return ks;
    }

    const int k = event->key();
    const bool pad = event->modifiers().testFlag(Qt::KeypadModifier);
    switch (k) {
    case Qt::Key_Up:        return ASTROSESSION_KEYSYM_UP;
    case Qt::Key_Down:      return ASTROSESSION_KEYSYM_DOWN;
    case Qt::Key_Left:      return ASTROSESSION_KEYSYM_LEFT;
    case Qt::Key_Right:     return ASTROSESSION_KEYSYM_RIGHT;
    case Qt::Key_Tab:       return ASTROSESSION_KEYSYM_TAB;
    case Qt::Key_Escape:    return ASTROSESSION_KEYSYM_ESCAPE;
    case Qt::Key_Return:    return ASTROSESSION_KEYSYM_RETURN;
    case Qt::Key_Enter:     return ASTROSESSION_KEYSYM_KP_ENTER;
    case Qt::Key_Backspace: return ASTROSESSION_KEYSYM_BACKSPACE;
    case Qt::Key_Space:     return ASTROSESSION_KEYSYM_SPACE;
    case Qt::Key_Shift:     return ASTROSESSION_KEYSYM_LSHIFT;
    case Qt::Key_Control:   return ASTROSESSION_KEYSYM_LCTRL;
    case Qt::Key_Alt:       return ASTROSESSION_KEYSYM_LALT;
    default: break;
    }
    if (k >= Qt::Key_F1 && k <= Qt::Key_F12)
        return ASTROSESSION_KEYSYM_F1 + (uint32_t)(k - Qt::Key_F1);
    if (k >= Qt::Key_0 && k <= Qt::Key_9)
        return (pad ? (uint32_t)ASTROSESSION_KEYSYM_KP_0 : (uint32_t)'0') +
               (uint32_t)(k - Qt::Key_0);
    if (k >= Qt::Key_A && k <= Qt::Key_Z)
        return (uint32_t)('a' + (k - Qt::Key_A));
    if (pad && k == Qt::Key_Asterisk) return ASTROSESSION_KEYSYM_KP_MULTIPLY;
    if (pad && k == Qt::Key_Slash)    return ASTROSESSION_KEYSYM_KP_DIVIDE;
    if (k >= 0x20 && k <= 0x7e)
        return (uint32_t)k;
    return 0;
}

bool astroForwardKey(astrosession *session, KeypadWindow *keypad,
                     QKeyEvent *event, bool down)
{
    const uint32_t ks = astroKeysymFromQt(event);
    if (down && keypad && keypad->mapForwardKey(ks))
        return true;
    const bool r = astrosession_input_key(
        session, ks, down, event->modifiers().testFlag(Qt::ControlModifier)) != 0;
    astrosession_sysaction_drain(session);
    return r;
}
