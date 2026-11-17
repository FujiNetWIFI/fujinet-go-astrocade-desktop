/*
 * DisplayGlue.h -- what DisplayView.m needs to know about THIS machine: the
 * session's functions, the frame size and the shape of the picture.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import "AstroKeyForward.h"
#include "astrosession.h"

#define FN_VIEW      DisplayView
#define FN_SESSION   astrosession
#define FN_FB_W      ASTROSESSION_FB_WIDTH
#define FN_FB_H      ASTROSESSION_FB_HEIGHT
#define FN_INITIAL_W 800
#define FN_INITIAL_H 650

/* The keyboard and start() (DisplayExtras.inc). */
#define FN_EXTRA_METHODS "DisplayExtras.inc"

/* The core paces itself on the wall clock and has no vsync lock: the display
 * link only draws. Nothing here is used for pacing. */
static inline int fn_machine_hz(FN_SESSION *s) { (void)s; return 60; }
static inline void fn_notify_vsync(FN_SESSION *s, int64_t ns) { (void)s; (void)ns; }

/* Every frame is the whole 352 x 240; nothing to measure. */
static inline int fn_pull_frame(FN_SESSION *s, uint32_t *dst, int *lines, uint64_t *serial)
{
    *lines = FN_FB_H;
    return astrosession_copy_frame(s, dst, serial);
}

/* Letterboxed at a fixed 4:3: the Astrocade's pixels are not square. */
static inline double fn_picture_aspect(FN_SESSION *s, int lines, BOOL tv)
{
    (void)s; (void)lines; (void)tv;
    return 4.0 / 3.0;
}
