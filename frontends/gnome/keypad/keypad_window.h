/*
 * The Controllers window -- see keypad_window.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ASTRO_GNOME_KEYPAD_WINDOW_H
#define ASTRO_GNOME_KEYPAD_WINDOW_H

#include <gtk/gtk.h>

#include "astrosession.h"

/* Show/hide the singleton Controllers window (F9 / View menu). */
void astro_keypad_window_toggle(GtkWindow *parent, astrosession *session);
gboolean astro_keypad_window_is_visible(void);

/* A key event while Map mode is armed: binds it and returns TRUE. Called by
 * both windows' key paths FIRST, so a remap keystroke is captured wherever
 * the focus is. FALSE when Map mode is not waiting for a key. */
gboolean astro_keypad_map_forward_key(uint32_t keysym);

#endif /* ASTRO_GNOME_KEYPAD_WINDOW_H */
