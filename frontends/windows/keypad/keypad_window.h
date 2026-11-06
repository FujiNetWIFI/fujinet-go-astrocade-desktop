/*
 * The Win32 Controllers window -- see keypad_window.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ASTRO_WIN_KEYPAD_WINDOW_H
#define ASTRO_WIN_KEYPAD_WINDOW_H

#include <windows.h>

#include "astrosession.h"

/* Show/hide the singleton window (F9 / View menu). */
void astro_controllers_toggle(HWND owner, astrosession *session);
BOOL astro_controllers_visible(void);

/* The one key path every window shares: Map capture first (press only), then
 * the bindings; `lparam` is the WM_KEYDOWN/WM_KEYUP lParam (its scan code and
 * extended-key flag name the physical key). TRUE when the key was consumed. */
BOOL astro_controllers_key(astrosession *session, LPARAM lparam, BOOL down);

#endif
