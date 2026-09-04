/*
 * gamepad_sdl -- SDL3 gamepads driving the Astrocade's hand controllers and
 * the remappable keypad/system bindings. Private to the session; the frontend
 * only needs the Map-mode capture calls, re-declared in astrosession.h.
 *
 * A background poll thread reads every connected gamepad (assigned to players
 * 0-3 by connection order), turning the D-pad / left stick into the handle
 * direction bits, the South button into the trigger, and the left-stick X
 * into the knob (paddle); any gamepad button also resolves through the
 * bindings table to a keypad key or system action.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ASTRO_GAMEPAD_SDL_H
#define ASTRO_GAMEPAD_SDL_H

#include <stdbool.h>
#include <stdint.h>

#include "astrosession.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start SDL's gamepad subsystem and the poll thread (a no-op if already
 * running). The session pointer is where system actions are POSTED from the
 * poll thread (never fired -- that joins threads). Returns 0 on success. */
int  astro_gamepad_start(astrosession *s);
void astro_gamepad_stop(void);

/* Translate an SDL_GamepadButton (uint8_t, kept SDL-free here) to a stable
 * name for the Map-mode status text; NULL for buttons this app doesn't name. */
const char *astro_gamepad_button_name(int sdl_button);

#ifdef __cplusplus
}
#endif

#endif /* ASTRO_GAMEPAD_SDL_H */
