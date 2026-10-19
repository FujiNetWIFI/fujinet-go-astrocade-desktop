/*
 * The Win32 debugger window -- see dbg_window.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ASTRO_WIN_DBG_WINDOW_H
#define ASTRO_WIN_DBG_WINDOW_H

#include <windows.h>

#include "astrosession.h"

void astro_debugger_show(HWND parent, astrosession *session);

#endif
