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

/* The Help context the main window shows now: "nobios", "prefs/<page>"
 * while Preferences is open, else "main". */
const char *astro_window_help_context(AstroWindow *self);

void       astro_window_restart_session(AstroWindow *self);
/* The picture, or the No-BIOS page when there is nothing to boot. */
void       astro_window_show_page(AstroWindow *self);
/* Starts the machine if it isn't running and a BIOS is there to boot. */
void       astro_window_start(AstroWindow *self);
/* A file chooser with a "these" filter (suffixes, any case) and All files. */
GtkFileDialog *astro_file_dialog(const char *title, const char *filter_name,
                                 const char *const *exts);

/* The key path both windows share (Map capture, then the bindings): `keycode`
 * is GTK's hardware keycode. TRUE when the key was consumed. */
gboolean astro_window_input_key(astrosession *session, guint keycode,
                                GdkModifierType state, gboolean down);

/* main.c */
const char *astro_icon_name(void);

#endif /* ASTRO_GNOME_WINDOW_H */
