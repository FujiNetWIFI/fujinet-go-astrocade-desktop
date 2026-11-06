/*
 * gamepad_sdl -- SDL3 gamepads driving the Astrocade's hand controllers and
 * the remappable keypad/system bindings. Private to the session; the frontend
 * only needs the Map-mode capture calls, re-declared in astrosession.h.
 *
 * A background poll thread reads every connected gamepad (assigned to players
 * 0-3 by connection order), the D-pad / left stick always steering that
 * player's stick; every button and axis resolves through the bindings table
 * to a keypad key, a system action, or one of the player's stick, trigger,
 * knob keys or the knob itself (an axis). Each pad is its own input source
 * (input.c), so it never cancels the keyboard's or the panel's holds. A bundled community
 * mapping DB (gamecontrollerdb_embedded.h) is loaded on start to cover
 * controllers SDL's own bundled mappings don't recognize.
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

#ifdef __cplusplus
}
#endif

#endif /* ASTRO_GAMEPAD_SDL_H */
