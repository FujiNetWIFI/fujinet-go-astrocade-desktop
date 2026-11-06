/*
 * bindings -- the remappable key / pad-input binding table, shared by the
 * session (seed at start), input.c (resolve a key), gamepad_sdl.c (resolve a
 * pad button or axis on its polling thread) and the Controllers window's Map
 * mode. A target (astrosession.h: ASTRO_TARGET_*) holds at most one keysym
 * and one pad input id; binding one steals it from whoever held it, so no two
 * targets ever fire from the same input.
 *
 * Keys are global. A pad input is scoped: a keypad key or system action is
 * reachable from every pad, a player's targets only from that player's pad,
 * so two players can both use South as their trigger.
 *
 * Process-global storage (like the machine itself, a process singleton).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ASTRO_BINDINGS_H
#define ASTRO_BINDINGS_H

#include "astrosession.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reload the table: every target to its default, then overridden by whatever
 * "bindings" holds in s's settings store. Called once per astrosession_new. */
void bindings_init(astrosession *s);

/* The target a keysym drives now, or -1. */
int bindings_target_for_key(uint32_t keysym);
/* The target pad input `id` drives for `player`'s pad, or -1. */
int bindings_target_for_pad(int player, int id);
/* The pad axis that sets `player`'s knob, or -1. */
int bindings_knob_axis(int player);

/* "Keypad 7", "Reset Game", "Player 2: Trigger". */
const char *bindings_target_label(int target);
/* Just the control: "Trigger", "7", "\xC3\xB7". */
const char *bindings_target_short_label(int target);

/* What a target has now: its keysym (0 none) and pad input id (-1 none). */
uint32_t bindings_target_keysym(int target);
int      bindings_target_pad(int target);

/* Rebind, stealing from any other target that held it (`stolen` receives the
 * target it was taken from, or -1). keysym 0 / id -1 clears. Persists. */
void bindings_set_keysym(astrosession *s, int target, uint32_t keysym, int *stolen);
void bindings_set_pad(astrosession *s, int target, int id, int *stolen);
/* Clear one target's key and pad input. */
void bindings_clear(astrosession *s, int target);
/* Every target back to its default. Persists. */
void bindings_reset_defaults(astrosession *s);

#ifdef __cplusplus
}
#endif

#endif /* ASTRO_BINDINGS_H */
