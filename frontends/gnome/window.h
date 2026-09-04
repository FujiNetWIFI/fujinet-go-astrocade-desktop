/*
 * AstroWindow -- the main window of the GNOME frontend. See window.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ASTRO_GNOME_WINDOW_H
#define ASTRO_GNOME_WINDOW_H

#include <adwaita.h>

#include "astrosession.h"

G_DECLARE_FINAL_TYPE(AstroWindow, astro_window, ASTRO, WINDOW,
                     AdwApplicationWindow)
#define ASTRO_TYPE_WINDOW (astro_window_get_type())

GtkWidget *astro_window_new(AdwApplication *app, astrosession *session);
void       astro_window_toast(AstroWindow *self, const char *message);
void       astro_window_restart_session(AstroWindow *self);

/* main.c */
const char *astro_icon_name(void);

#endif /* ASTRO_GNOME_WINDOW_H */
