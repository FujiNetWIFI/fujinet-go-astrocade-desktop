/*
 * gamepad_sdl -- private interface: the pure helpers unit-tested without
 * hardware, and the start/stop the session calls. The public gamepad API is
 * in astrosession.h (and lives in gamepad_sdl.c).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ASTRO_GAMEPAD_SDL_H
#define ASTRO_GAMEPAD_SDL_H

#include <stdint.h>

#include "astrosession.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Direction bits a stick or hat resolves to. */
#define ASTRO_DIR_UP    1
#define ASTRO_DIR_DOWN  2
#define ASTRO_DIR_LEFT  4
#define ASTRO_DIR_RIGHT 8

/* An analog stick engages a direction at half deflection and lets go below
 * 0.35: wide enough that a thumb resting near the threshold cannot flutter. */
#define ASTRO_STICK_ENTER 0.5f
#define ASTRO_STICK_EXIT  0.35f

/* The direction bits as the player's stick actions, a mask over astro_action
 * (UP/DOWN/LEFT/RIGHT). Opposite directions cancel. Pure. */
unsigned astro_acts_from_dir(int dir);

/* An analog stick as an eight-way switch with hysteresis: an axis engages
 * past `enter` and releases below `leave` (both 0..1 of full deflection).
 * xval and yval are SDL's (+x right, +y down); `prev` is the previous result.
 * Pure. */
int astro_dir_from_stick(float xval, float yval, int prev, float enter, float leave);

/* Which pad plays `player` (0..3): an explicit assignment wins; among the
 * rest, connection order (the first unassigned pad plays the first player
 * nobody is assigned to, and so on). `assign[idx]` is pad idx's assignment
 * (-1 = automatic). Returns the pad index or -1. Pure. */
int astro_pad_for_player(const int *assign, int npads, int player);

/* The knob's value (0..255) an axis position gives: a gamepad trigger sweeps
 * 0..1, a stick (or any axis of a raw joystick) -1..1. Pure. */
int astro_knob_from_axis(float value, int is_trigger);

/* SDL3's SDL_GamepadButton (a uint8_t) to the public numbering, and back.
 * Explicit tables, never a cast, so an SDL reordering cannot desync them. */
int astro_pad_button_from_sdl(uint8_t button);
int astro_sdl_button_from_pad(int button);

/* Start SDL's gamepad subsystem and the poll thread (a no-op if already
 * running). System actions are POSTED to the session from the poll thread
 * (never fired: firing joins threads). Returns 0 on success. */
int  astro_gamepad_start(astrosession *s);
void astro_gamepad_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* ASTRO_GAMEPAD_SDL_H */
