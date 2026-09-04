/*
 * AstroPrefs -- the Preferences dialog. See prefs.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ASTRO_GNOME_PREFS_H
#define ASTRO_GNOME_PREFS_H

#include <adwaita.h>

#include "astrosession.h"
#include "window.h"

void astro_prefs_show(AstroWindow *window, astrosession *session);

#endif /* ASTRO_GNOME_PREFS_H */
