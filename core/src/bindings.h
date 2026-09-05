/*
 * bindings -- the remappable keyboard/gamepad binding table, shared by
 * session.c (seed/reload at start), the keypad window's Map mode, and
 * gamepad_sdl.c (resolve a pad button on its polling thread). The Astrocade
 * has a single console keypad and no control discs, so a binding target is
 * one of the 24 keypad keys, one of the 2 system actions, or one of the 5
 * player-0 hand-controller actions (a keyboard fallback for when no gamepad
 * is connected) -- far simpler than the Intv port's per-side + disc model.
 *
 * Process-global storage (like the machine itself, a process singleton):
 * there is only ever one session's settings store live at a time.
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

/* The player-0 hand controller's 5 keyboard-bindable actions (the physical
 * joystick + trigger; the knob has no keyboard equivalent). Order matches
 * k_handle_bit/k_handle_label in bindings.c. */
#define ASTRO_HANDLE_ACTION_COUNT 5

/* A binding target: the 24 keypad keys, then the 2 system actions, then the
 * 5 hand-controller actions. */
typedef enum {
    ASTRO_TARGET_KEYPAD = 0,                    /* + astrosession_key (0..23) */
    ASTRO_TARGET_SYSACT = ASTROSESSION_KEY_COUNT, /* + astrosession_sysaction */
    ASTRO_TARGET_HANDLE = ASTRO_TARGET_SYSACT + ASTROSESSION_SYSACT_COUNT,
                                                 /* + index into k_handle_bit */
    ASTRO_TARGET_COUNT = ASTRO_TARGET_HANDLE + ASTRO_HANDLE_ACTION_COUNT,
} astro_target_base;

typedef enum {
    ASTRO_MAP_NONE = 0,
    ASTRO_MAP_KEY,      /* a keypad key -- value is an astrosession_key */
    ASTRO_MAP_SYSACT,   /* a system action -- value is an astrosession_sysaction */
    ASTRO_MAP_HANDLE,   /* the player-0 hand controller -- value is an
                          * ASTROSESSION_HANDLE_* bit (astrosession.h) */
} astro_map_kind;

typedef struct {
    astro_map_kind kind;
    int value;          /* astrosession_key / astrosession_sysaction /
                          * ASTROSESSION_HANDLE_* bit, depending on kind */
} astro_mapping;

/* Reload the table: every target to its default, then overridden by whatever
 * "bindings" holds in s's settings store. Called once per astrosession_new. */
void bindings_init(astrosession *s);

/* Resolve a keysym (ASTROSESSION_KEYSYM_* / ASCII) to what it drives now,
 * honouring user remaps. ASTRO_MAP_NONE when the key is bound to nothing. */
astro_mapping bindings_resolve_keysym(int keysym);

/* Resolve a gamepad button (SDL_GamepadButton index) to what it drives. */
astro_mapping bindings_resolve_button(int button);

/* ---- Map mode (the keypad window's remapping UI) ---- */

/* A human label for a target, e.g. "Keypad 7" / "Reset Game". */
const char *bindings_target_label(int target);

/* The current keysym / gamepad button bound to a target (0 / -1 if none). */
int bindings_target_keysym(int target);
int bindings_target_button(int target);

/* Rebind a target to a keysym (or a gamepad button), stealing it from any
 * other target that held it. Pass keysym 0 (button -1) to clear. Persists to
 * the settings store. If steal_from_out is non-NULL it receives the target
 * the binding was taken from, or -1. */
void bindings_set_keysym(astrosession *s, int target, int keysym,
                         int *steal_from_out);
void bindings_set_button(astrosession *s, int target, int button,
                         int *steal_from_out);

/* Restore every target to its default and persist. */
void bindings_reset_defaults(astrosession *s);

#ifdef __cplusplus
}
#endif

#endif /* ASTRO_BINDINGS_H */
