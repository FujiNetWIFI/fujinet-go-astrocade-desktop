/*
 * The Controllers window (F9): the Astrocade console's 24-key keypad (with the
 * Bally BASIC overlay card when it is on), the four pistol-grip hand
 * controllers -- stick wedges, trigger and the knob's dial -- and the
 * gamepads with their player assignments, with Map mode for rebinding any
 * control. Hidden at start; a fixed-size floating window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QWidget>

extern "C" {
#include "astrosession.h"
}

namespace astroctl {
/* The drawing's metrics, shared with the offscreen test: pixels per layout
 * unit (a keypad key's width), the case's margin round the controls, and where
 * the layout's origin sits down from the top of the window. */
constexpr double kUnit = 34.0;
constexpr int kCasePad = 16;
constexpr int kTop = 26;
} // namespace astroctl

/* Shows the window, or hides it when it is showing. */
void astroControllersToggle(QWidget *parent, astrosession *session);
/* Shows (and raises) the window; leaves it showing if it already is. */
void astroControllersShow(QWidget *parent, astrosession *session);
bool astroControllersVisible();
/* The Controllers window has the keyboard focus (the main window then keeps
 * its keys down when it loses activation). */
bool astroControllersActive();
/* Preferences closed or the session restarted: redraw. */
void astroControllersRefresh();
