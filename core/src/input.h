/*
 * input -- the merged input layer: private to the session, the Controllers
 * window's entry points (astrosession.h) and the gamepad poll thread. Every
 * input source holds targets by its own bit, one word per target, so letting
 * go of one source never releases another's: the keyboard, the window's
 * mouse, the overlay's typing, and for each gamepad its buttons and its stick
 * separately (two pads, or a pad's button and its stick, bound to the same
 * action cannot cancel each other). The machine sees the union.
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

/* Pads get a stable "owner" number, 0..ASTRO_MAX_PADS-1, while they are
 * connected, whatever their slot becomes when another is unplugged. */
#define ASTRO_MAX_PADS 8

#define ASTRO_SRC_KEYBOARD  (1u << 0)
#define ASTRO_SRC_POINTER   (1u << 1)
#define ASTRO_SRC_PAD(o)    (1u << (2 + (o)))
#define ASTRO_SRC_STICK(o)  (1u << (2 + ASTRO_MAX_PADS + (o)))
#define ASTRO_SRC_TYPING    (1u << (2 + 2 * ASTRO_MAX_PADS))

/* The pad thread's entry points. press: a pad BUTTON holds or releases a
 * target (its ASTRO_SRC_PAD source). stick: the analog stick, a hat or the
 * steering of `owner`'s pad holds one action of `player` (its ASTRO_SRC_STICK
 * source). Targets that are not held inputs (system actions) are ignored. */
void astro_input_pad_press(int owner, int target, int down);
void astro_input_pad_stick(int owner, int player, int act, int down);
/* A pad axis moved the knob: sets it when the axis changed since last time. */
void astro_input_pad_knob(int owner, int player, int value);
/* The pad is gone (or moved to another player): let go of everything it held. */
void astro_input_pad_clear(int owner);

/* Advance the keys' knob ramp by dt_ms (the pad poll thread calls it). */
void astro_input_tick(int dt_ms);
/* 1 while some knob key is held (the poll thread then wakes every 16 ms). */
int  astro_input_knob_active(void);

/* After the machine (re)starts: push every merged value to it again. */
void astro_input_resync(void);
/* Drop every source's holds and typing (RESET GAME, a stop) and tell the
 * machine. */
void astro_input_clear_all(void);

/* Typing (overlay.c): queue keypad presses, each held a few frames and
 * released a few frames apart, on a thread of its own that exists only while
 * there is something to type. `keys` are astrosession_key values. Returns how
 * many were queued (the queue is bounded). */
int  astro_input_type_keys(const uint8_t *keys, int count);
int  astro_input_typing_pending(void);
/* Stop typing, let go of the held press and drop the queue; waits until the
 * typing thread has gone. */
void astro_input_typing_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* ASTRO_INPUT_H */
