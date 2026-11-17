/*
 * The Controllers window -- the console's 24-key keypad (champagne plate, gold
 * operator column, or the Bally BASIC overlay card laid over it) and all four
 * pistol-grip hand controllers, drawn as a player sees them. Held controls
 * light up in the accent whoever holds them, the pointer works every control
 * (drag the ribbed knob to turn it, scroll nudges it) and Map mode rebinds any
 * of them to a key or a gamepad input.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "astrosession.h"

/* Toggles visibility: shows the singleton window (creating it on first
 * call), or hides it if already showing. `parent` is the main window; it is
 * only used the first time, to set the transient-for relationship, and keys
 * typed in this window go through it. */
void astro_controllers_toggle(GtkWindow *parent, astrosession *session);
/* Shows it (or raises it). */
void astro_controllers_show(GtkWindow *parent, astrosession *session);
gboolean astro_controllers_visible(void);

/* Map mode's share of a key, for the main window's key path as well (Map can
 * be armed with either window focused): while Map is armed this takes the key
 * and returns TRUE. F9, F11, F12 and Ctrl chords are never Map's. */
gboolean astro_controllers_map_key(guint keyval, guint code, GdkModifierType mods,
                                   gboolean down);
