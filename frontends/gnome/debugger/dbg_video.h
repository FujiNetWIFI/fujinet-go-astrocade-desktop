/*
 * The debugger's Video chip tab (GTK4): a thin view over core/include/astrovidtab.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ASTRO_GNOME_DBG_VIDEO_H
#define ASTRO_GNOME_DBG_VIDEO_H

#include <gtk/gtk.h>

/* The page. It owns its controller and frees it when destroyed. */
GtkWidget *dbg_video_new(void);

/* Bring the page up to date (only what changed is redrawn). The first call
 * after the page is mapped, or force, redraws everything. */
void dbg_video_refresh(GtkWidget *page, gboolean force);

#endif
