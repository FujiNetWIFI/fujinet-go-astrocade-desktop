/*
 * The app's accent colour (ASTROSESSION_ACCENT_RGB, the icon's gold). Buttons
 * in the accent state -- Map while armed -- carry the dynamic property
 * "accent" and wear it with the accent's ink; main.cpp's style sheet draws
 * them. Held parts in the Controllers window use it too.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QColor>
#include <QPushButton>
#include <QStyle>

extern "C" {
#include "astrosession.h"
}

/* the dark ink that reads on the gold */
#define ASTRO_CTL_ACCENT_INK_RGB 0x241A00u

inline QColor astroCtlAccent()
{
    return QColor((ASTROSESSION_ACCENT_RGB >> 16) & 0xff,
                  (ASTROSESSION_ACCENT_RGB >> 8) & 0xff,
                  ASTROSESSION_ACCENT_RGB & 0xff);
}

/* The accent's edge (a held part's outline shade) and the ink on it. */
inline QColor astroCtlAccentDark() { return astroCtlAccent().darker(140); }
inline QColor astroCtlAccentInk()
{
    return QColor((ASTRO_CTL_ACCENT_INK_RGB >> 16) & 0xff,
                  (ASTRO_CTL_ACCENT_INK_RGB >> 8) & 0xff,
                  ASTRO_CTL_ACCENT_INK_RGB & 0xff);
}

/* Puts a button in or out of the accent state and repolishes it, so the style
 * sheet's [accent="true"] rule takes effect at once. */
inline void astroCtlSetAccent(QPushButton *b, bool on)
{
    if (b->property("accent").toBool() == on) return;
    b->setProperty("accent", on);
    b->style()->unpolish(b);
    b->style()->polish(b);
}
