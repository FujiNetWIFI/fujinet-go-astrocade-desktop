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

/* A system action (ASTROSESSION_SYSACT_*) from a key, a pad button or the
 * Controllers window's console buttons, with the same feedback as the Machine
 * menu. UI thread. */
void astro_window_run_sysaction(AstroWindow *self, int sysact);

/* The one keyboard path: the main window's own key handler and the
 * Controllers window's both end here, so Map mode, the bindings, F9
 * (Controllers), F11, F12 and Bally BASIC typing behave the same whichever
 * has the focus. `keycode` is the GDK hardware keycode. Returns whether the
 * key was consumed. */
gboolean astro_window_key(AstroWindow *self, guint keyval, guint keycode,
                          GdkModifierType state, gboolean down);

/* Lets go of every key the keyboard holds, and stops typing (focus loss). */
void astro_window_release_keys(AstroWindow *self);

/* main.c */
const char *astro_icon_name(void);

#endif /* ASTRO_GNOME_WINDOW_H */
