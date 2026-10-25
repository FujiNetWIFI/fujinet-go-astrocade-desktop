/*
 * The emulator picture -- see display.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ASTRO_WIN_DISPLAY_H
#define ASTRO_WIN_DISPLAY_H

#include <windows.h>

#include "astrosession.h"

/* Creates the picture as a child of parent (size it with MoveWindow) and
 * starts its render thread. One per process. */
HWND astro_display_create(HWND parent, astrosession *session);
/* Stops the render thread; call before the parent is destroyed. */
void astro_display_destroy(void);
/* "Direct3D 11", "Direct3D 11 (WARP)" or "GDI": what is drawing. */
const char *astro_display_renderer(void);

#endif
