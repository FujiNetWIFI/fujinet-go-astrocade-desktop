/*
 * The debugger window -- see dbg_window.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ASTRO_GNOME_DBG_WINDOW_H
#define ASTRO_GNOME_DBG_WINDOW_H

#include <gtk/gtk.h>

#include "astrosession.h"

void astro_debugger_show(GtkWindow *parent, astrosession *session);

#endif /* ASTRO_GNOME_DBG_WINDOW_H */
