/*
 * astrovid -- a consistent snapshot of the Astrocade data chip (video) and
 * the sound section, and the types the Video tab controller (astrovidtab.h)
 * draws from.
 *
 * EVERY programmer-visible register of the custom chip is exposed here, per
 * the Nutting manual's Output/Input Ports tables (pp. 106-107): the eight
 * color registers, the resolution/mode bit, the horizontal color boundary
 * and background, the vertical blank line, the three interrupt registers, the
 * magic function-generator control and its internal state, the expand colors,
 * the intercept/feedback inputs, and the full sound register file plus its
 * oscillator/vibrato/noise state. The snapshot is copied in one call so a
 * describe/render works from a frozen, self-consistent view.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ASTROVID_H
#define ASTROVID_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* ---- data chip: OUT registers ---- */
    uint8_t colors[8];          /* 00-07: COL0R-3R (0-3), COL0L-3L (4-7) */
    uint8_t video_mode;         /* 08: 0 = low-res (160x102), 1 = high (320x204) */
    uint8_t colorsplit;         /* 09: horizontal color boundary (4-px groups) */
    uint8_t bgdata;             /* 09 bits 7-6, replicated */
    uint8_t vblank;             /* 0A: last active astrocade line (VERBL) */
    uint8_t interrupt_vector;   /* 0D: INFBK */
    uint8_t interrupt_enabl;    /* 0E: INMOD */
    uint8_t interrupt_scanline; /* 0F: INLIN */
    uint8_t expand_color[2];    /* 19: XPAND */

    /* ---- data chip: IN registers ---- */
    uint8_t intercept;          /* 08: funcgen intercept feedback */
    uint8_t vertical_feedback;  /* 0E: lightpen Y */
    uint8_t horizontal_feedback;/* 0F: lightpen X */

    /* ---- magic function generator internal state ---- */
    uint8_t funcgen_control;    /* 0C: MAGIC (shift/rotate/expand/OR/XOR/flop) */
    uint8_t expand_count;
    uint8_t rotate_count;
    uint8_t rotate_data[4];
    uint8_t shift_prev_data;

    /* ---- sound chip ---- */
    uint8_t snd_reg[8];         /* 10-17 register file */
    uint8_t master_count;
    uint16_t vibrato_clock;
    uint16_t noise_state;
    uint8_t a_count, a_state, b_count, b_state, c_count, c_state;

    /* ---- timing + memory ---- */
    int scanline;               /* current screen line 0..261 */
    uint8_t vram[0x1000];       /* the 4K screen RAM */

    uint32_t palette[512];      /* the built palette (for the strip) */
} astrovid_snapshot;

/* The video registers a screen line was drawn with. The machine records one
 * per line as it renders it, so a game that changes colours, the boundary or
 * VERBL mid-frame (from the INLIN interrupt, or timed to the beam) can be
 * seen the way the TV showed it. */
typedef struct {
    uint8_t colors[8];          /* COL0R-3R, COL0L-3L */
    uint8_t colorsplit, bgdata, vblank, mode;
} astrovid_line;

/* A finished frame as the machine left it: the registers at the frame's end,
 * each line's own registers, and the picture. Taken once per frame by the
 * machine thread (see astro_host_capture_*), so a reader sees one consistent
 * frame while the game runs. */
typedef struct {
    astrovid_snapshot snap;
    astrovid_line lines[240];   /* ASTROVID_SCREEN_H */
    uint32_t fb[352 * 240];     /* XRGB8888, ASTROVID_SCREEN_W x _H */
    uint64_t serial;            /* 0: nothing captured yet */
    uint64_t frame;
} astrovid_capture;

/* Copy the whole chip state in one shot. Intended to be called while the
 * debugger holds the machine paused; a live call is racy but harmless (all
 * bytes). */
void astrovid_snapshot_get(astrovid_snapshot *snap);

/* Decoded register/state text (monospace), NUL-terminated. */
void astrovid_format_state(const astrovid_snapshot *snap, char *dst, int len);

#define ASTROVID_SCREEN_W 352
#define ASTROVID_SCREEN_H 240

#ifdef __cplusplus
}
#endif

#endif /* ASTROVID_H */
