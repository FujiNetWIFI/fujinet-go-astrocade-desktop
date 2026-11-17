/*
 * The Win32 Controllers window (F9) -- see controllers_window.c.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ASTRO_WIN_CONTROLLERS_WINDOW_H
#define ASTRO_WIN_CONTROLLERS_WINDOW_H

#include <windows.h>

#include "astrosession.h"

/* Show / hide the singleton window (F9, View > Controllers). Created on first
 * use, hidden and not destroyed after. */
void astro_controllers_toggle(HWND owner, astrosession *session);
/* Show it (and raise it) if it is not showing. */
void astro_controllers_show(HWND owner, astrosession *session);
BOOL astro_controllers_visible(void);

/* The gamepad set, the bindings or the overlay changed: repaint. */
void astro_controllers_refresh(void);

/* A key for the console, the one path the main window and this window share
 * (WM_KEYDOWN / WM_KEYUP and their SYS twins): Map mode first (F9, F11 and
 * F12 are the app's, never Map's), then the bindings, then the system actions
 * a bound key posted. TRUE when the key was consumed. A key the bindings did
 * not take may still be typed (astro_controllers_char). */
BOOL astro_controllers_key(astrosession *session, UINT msg, WPARAM wparam, LPARAM lparam);

/* Map mode alone (the window's own message path uses it too): TRUE if Map
 * took the key. */
BOOL astro_controllers_map_key(UINT msg, WPARAM wparam, LPARAM lparam);

/* A WM_CHAR: with the Bally BASIC overlay on, types the character on the
 * keypad (shift colour, then key), unless the key that produced it was a
 * bound control or Map's. */
void astro_controllers_char(astrosession *session, WPARAM wparam, LPARAM lparam);

/* Take and run the system actions posted since last time (a pad button or a
 * bound key); "Stop in Debugger" also shows the debugger. UI thread. */
void astro_controllers_run_sysactions(HWND owner, astrosession *session);

#endif
