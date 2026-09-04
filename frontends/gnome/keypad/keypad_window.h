/*
 * The keypad window -- see keypad_window.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ASTRO_GNOME_KEYPAD_WINDOW_H
#define ASTRO_GNOME_KEYPAD_WINDOW_H

#include <gtk/gtk.h>

#include "astrosession.h"

/* Show/hide the singleton keypad window (F9 / View menu). */
void astro_keypad_window_toggle(GtkWindow *parent, astrosession *session);

/* Forward a keyboard event to the keypad window's Map mode, if it is armed;
 * returns TRUE if the event was consumed by Map mode. Called by the main
 * window so a remap keystroke typed there is captured too. */
gboolean astro_keypad_map_forward_key(int keysym);

#endif /* ASTRO_GNOME_KEYPAD_WINDOW_H */
