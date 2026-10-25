/*
 * Importing the BIOS -- see import.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ASTRO_WIN_IMPORT_H
#define ASTRO_WIN_IMPORT_H

#include <windows.h>

#include "astrosession.h"

/* Asks for BIOS files (several, or one when single) or a folder, imports
 * each, and says in one message box what came of it. Returns how many paths
 * imported something, or -1 when the chooser was cancelled. */
int astro_import_bios(HWND owner, astrosession *session, int folder, int single);

#endif
