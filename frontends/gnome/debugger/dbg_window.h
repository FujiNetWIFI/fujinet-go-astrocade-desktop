/*
 * The debugger window -- see dbg_window.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ASTRO_GNOME_DBG_WINDOW_H
#define ASTRO_GNOME_DBG_WINDOW_H

#include <adwaita.h>

#include "astrosession.h"

G_BEGIN_DECLS

/* Shows (creating on first use) the debugger window for the session;
 * showing attaches the engine, which stops the machine. */
void astro_debugger_show(GtkWindow *parent, astrosession *session);

/* Shows it if hidden or absent, hides it (detaching, so the machine runs)
 * if showing: the main window's F12 and menu item. */
void astro_debugger_toggle(GtkWindow *parent, astrosession *session);

G_END_DECLS

#endif /* ASTRO_GNOME_DBG_WINDOW_H */
