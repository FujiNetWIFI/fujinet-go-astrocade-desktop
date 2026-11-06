/*
 * input -- the merged input layer: private to the session and gamepad poll
 * thread. Every source (the panel, the keyboard, up to 4 pads) keeps its own
 * holds; the machine sees their union.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ASTRO_INPUT_H
#define ASTRO_INPUT_H

#include <stdint.h>

#include "astrosession.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { ASTRO_SRC_PANEL = 0, ASTRO_SRC_KEYBOARD = 1, ASTRO_SRC_PAD = 2,
       ASTRO_SRC_COUNT = ASTRO_SRC_PAD + ASTROSESSION_PLAYERS };

/* A pad slot (0-3) replaces everything it held: keypad bits, per-player hand
 * masks (only `hand_player`'s is used), and the knob ramp directions
 * (bit 0 = dec, bit 1 = inc). */
void astro_input_pad_state(int slot, uint32_t keys, int hand_player,
                           uint8_t hand, uint8_t knobdir);
/* A pad axis moved the knob: sets it when the axis changed since last time. */
void astro_input_pad_knob(int slot, int player, int value);
void astro_input_pad_clear(int slot);

/* Advance the keys' knob ramp by dt_ms (the pad poll thread calls it). */
void astro_input_tick(int dt_ms);
/* 1 while some knob key is held (the poll thread then wakes every 16 ms). */
int  astro_input_knob_active(void);

/* After the machine (re)starts: push every merged value to it again. */
void astro_input_resync(void);
/* Drop every source's holds (RESET GAME) and tell the machine. */
void astro_input_clear_all(void);

#ifdef __cplusplus
}
#endif

#endif /* ASTRO_INPUT_H */
