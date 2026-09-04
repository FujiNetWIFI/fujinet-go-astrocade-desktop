/*
 * KeyForward -- translate a Qt key event to an ASTROSESSION_KEYSYM_* value
 * (the session's own numbering, not raw Qt::Key). See astro_keymap.c.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ASTRO_KDE_KEYFORWARD_H
#define ASTRO_KDE_KEYFORWARD_H

#include <QKeyEvent>

extern "C" {
#include "astrosession.h"
}

inline int astro_keysym_from_qt(const QKeyEvent *e)
{
    switch (e->key()) {
    case Qt::Key_Up:        return ASTROSESSION_KEYSYM_UP;
    case Qt::Key_Down:      return ASTROSESSION_KEYSYM_DOWN;
    case Qt::Key_Left:      return ASTROSESSION_KEYSYM_LEFT;
    case Qt::Key_Right:     return ASTROSESSION_KEYSYM_RIGHT;
    case Qt::Key_Return:
    case Qt::Key_Enter:     return ASTROSESSION_KEYSYM_RETURN;
    case Qt::Key_Backspace: return ASTROSESSION_KEYSYM_BACKSPACE;
    case Qt::Key_Escape:    return ASTROSESSION_KEYSYM_ESCAPE;
    case Qt::Key_Space:     return ASTROSESSION_KEYSYM_SPACE;
    default: break;
    }
    const QString t = e->text();
    if (t.size() == 1) {
        const ushort u = t[0].unicode();
        if (u >= 0x20 && u < 0x7f)
            return static_cast<int>(u);
    }
    return ASTROSESSION_KEYSYM_NONE;
}

#endif
