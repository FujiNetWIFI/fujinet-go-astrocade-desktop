/*
 * The Win32 Preferences window -- see prefs.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ASTRO_WIN_PREFS_H
#define ASTRO_WIN_PREFS_H

#include <windows.h>

#include "astrosession.h"

/* Called when a BIOS is imported from the ROMs page (the main window shows
 * its picture again), and when the window closes: dirty when a machine
 * setting changed (restart), else the machine starts if it now can. */
typedef struct {
    void (*imported)(void);
    void (*closed)(int dirty);
} astro_prefs_hooks;

/* Opens (or raises) Preferences; roms_page opens it on the ROMs tab. */
void astro_prefs_show(HWND parent, astrosession *session, const astro_prefs_hooks *hooks,
                      int roms_page);

#endif
