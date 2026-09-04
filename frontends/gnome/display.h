/*
 * AstroDisplay -- the emulator display widget. See display.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ASTRO_GNOME_DISPLAY_H
#define ASTRO_GNOME_DISPLAY_H

#include <gtk/gtk.h>

#include "astrosession.h"

G_DECLARE_FINAL_TYPE(AstroDisplay, astro_display, ASTRO, DISPLAY, GtkWidget)
#define ASTRO_TYPE_DISPLAY (astro_display_get_type())

GtkWidget *astro_display_new(astrosession *session);

#endif /* ASTRO_GNOME_DISPLAY_H */
