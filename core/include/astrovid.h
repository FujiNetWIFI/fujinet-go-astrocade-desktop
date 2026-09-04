/*
 * astrovid -- a consistent snapshot of the Astrocade data chip (video) and
 * the sound section, and renderers that turn it into pixels/text for the
 * debugger's Video tab.
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

/* Copy the whole chip state in one shot. Intended to be called while the
 * debugger holds the machine paused; a live call is racy but harmless (all
 * bytes). */
void astrovid_snapshot_get(astrovid_snapshot *snap);

/* ---- renderers ---- */

/* Full screen (ASTROSESSION_FB geometry) rendered from the snapshot's screen
 * RAM + color registers, into XRGB8888 dst. w/h are written. */
void astrovid_render_screen(const astrovid_snapshot *snap, uint32_t *dst,
                            int *w, int *h);

/* Screen RAM as a raw bitmap honouring the current mode (low-res 160x102 /
 * high-res 320x204), XRGB8888. w/h are written. */
void astrovid_render_bitmap(const astrovid_snapshot *snap, uint32_t *dst,
                            int *w, int *h);

/* The 512-pen palette as a strip of cells, `cols` wide, each `cell` px square,
 * into XRGB8888 dst sized cols*cell x rows*cell. Returns rows. */
int astrovid_render_palette(const astrovid_snapshot *snap, uint32_t *dst,
                            int cols, int cell);

/* Decoded register/state text (monospace), NUL-terminated. */
void astrovid_format_state(const astrovid_snapshot *snap, char *dst, int len);

/* Max buffer sizes a caller must allocate for the renders above (RGBA words).
 * A single shared scratch must be sized to the MAX of all of them -- the
 * sibling ports' Windows debugger corrupted .bss by sizing it to only one. */
#define ASTROVID_SCREEN_W 352
#define ASTROVID_SCREEN_H 240
#define ASTROVID_BITMAP_W 320
#define ASTROVID_BITMAP_H 204
#define ASTROVID_PAL_COLS 32
#define ASTROVID_PAL_CELL 12
#define ASTROVID_MAX_PIXELS (ASTROVID_SCREEN_W * ASTROVID_SCREEN_H)

#ifdef __cplusplus
}
#endif

#endif /* ASTROVID_H */
