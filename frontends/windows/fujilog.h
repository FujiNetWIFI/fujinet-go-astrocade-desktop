/*
 * The FujiNet console log window -- see fujilog.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ASTRO_WIN_FUJILOG_H
#define ASTRO_WIN_FUJILOG_H

#include <windows.h>

#include "astrosession.h"

void astro_fujilog_show(HWND parent, astrosession *session);

#endif
