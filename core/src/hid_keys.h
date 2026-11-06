/*
 * hid_keys -- private interface to core/src/hid_keys.c, for bindings.c's
 * name tables. The native-code -> HID-usage translators the frontends call
 * are public instead, declared in astrosession.h beside the keysym bands they
 * feed (ASTROSESSION_KEYSYM_HID_BASE and friends).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ASTRO_HID_KEYS_H
#define ASTRO_HID_KEYS_H

#include <stdint.h>

#include "astrosession.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Human-readable name for a USB HID Keyboard usage ID -- "F16", "Keypad A",
 * "International 3" -- the names  use. NULL for a usage
 * with no name, which is the caller's cue to synthesize one; callers must
 * handle that rather than print NULL. */
const char *astro_hid_usage_name(uint32_t usage);

/* USB HID keyboard usage IDs from each platform's native key code; 0 when
 * unknown. The public astrosession_keysym_from_* wrappers chain these
 * through astrosession_keysym_from_hid. */
uint32_t astrosession_hid_from_win_scancode(unsigned scancode, int extended);
uint32_t astrosession_hid_from_evdev(unsigned code);
uint32_t astrosession_hid_from_macos_keycode(unsigned keycode);
uint32_t astrosession_keysym_from_hid(uint32_t usage);

#ifdef __cplusplus
}
#endif

#endif /* ASTRO_HID_KEYS_H */
