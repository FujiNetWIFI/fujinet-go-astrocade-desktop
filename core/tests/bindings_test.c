/*
 * bindings_test -- the remappable binding table: defaults resolve, a remap
 * steals the input from whoever held it, an override persists across a
 * reopen, pad inputs are scoped per player, and reset restores defaults.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "astrosession.h"
#include "bindings.h"
#include "test_tmpdir.h"

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

#define PAD_SOUTH 0
#define PAD_RIGHTX (ASTRO_PAD_AXIS_BASE + 2)
#define PAD_LTRIG (ASTRO_PAD_AXIS_BASE + 4)

int main(void)
{
    char tmpl[512];
    test_tmpl(tmpl, sizeof tmpl, "astro_bindings_test");
    char *dir = mkdtemp(tmpl);
    if (!dir) { perror("mkdtemp"); return 1; }
    char cfg[512], data[512];
    snprintf(cfg, sizeof cfg, "%s/config", dir);
    snprintf(data, sizeof data, "%s/data", dir);
    astrosession_paths paths = { cfg, data };

    astrosession *s = astrosession_new(&paths);
    if (!s) return 1;

    /* defaults */
    CHECK(bindings_target_for_key('7') == ASTROSESSION_KEY_7, "'7' -> keypad 7");
    CHECK(bindings_target_for_key('=') == ASTROSESSION_KEY_PLUS, "'=' -> keypad +");
    CHECK(bindings_target_for_key(ASTROSESSION_KEYSYM_BACKSPACE) ==
              ASTRO_TARGET_SYSACT + ASTROSESSION_SYSACT_RESET_GAME,
          "Backspace -> Reset Game");
    CHECK(bindings_target_for_key(ASTROSESSION_KEYSYM_UP) ==
              ASTRO_TARGET_PLAYER(0, ASTRO_ACT_UP), "Up -> player 1 Up");
    CHECK(bindings_target_for_key(ASTROSESSION_KEYSYM_SPACE) ==
              ASTRO_TARGET_PLAYER(0, ASTRO_ACT_TRIGGER), "Space -> player 1 Trigger");
    CHECK(bindings_target_for_key('[') == ASTRO_TARGET_PLAYER(0, ASTRO_ACT_KNOB_DEC),
          "'[' turns the knob down");
    CHECK(bindings_target_for_key('u') == ASTROSESSION_KEY_UP, "'u' -> keypad Up");

    /* pad: every player's pad has South = its own Trigger, right stick X = knob */
    for (int p = 0; p < ASTROSESSION_PLAYERS; p++) {
        CHECK(bindings_target_for_pad(p, PAD_SOUTH) ==
                  ASTRO_TARGET_PLAYER(p, ASTRO_ACT_TRIGGER), "South -> own Trigger");
        CHECK(bindings_knob_axis(p) == PAD_RIGHTX, "right stick X is the knob axis");
    }

    /* a key remap steals */
    int stole = -99;
    bindings_set_keysym(s, ASTROSESSION_KEY_1, '7', &stole);
    CHECK(stole == ASTROSESSION_KEY_7, "remap stole '7' from keypad 7");
    CHECK(bindings_target_for_key('7') == ASTROSESSION_KEY_1, "'7' drives keypad 1");
    CHECK(bindings_target_keysym(ASTROSESSION_KEY_7) == 0, "keypad 7 unbound");

    /* a pad input is scoped: player 2 binding South to Up leaves player 1's
     * Trigger alone; a keypad key (every pad) steals from all */
    bindings_set_pad(s, ASTRO_TARGET_PLAYER(1, ASTRO_ACT_UP), PAD_SOUTH, &stole);
    CHECK(stole == ASTRO_TARGET_PLAYER(1, ASTRO_ACT_TRIGGER),
          "same player's Trigger lost South");
    CHECK(bindings_target_for_pad(0, PAD_SOUTH) == ASTRO_TARGET_PLAYER(0, ASTRO_ACT_TRIGGER),
          "player 1's South is untouched");
    CHECK(bindings_target_for_pad(1, PAD_SOUTH) == ASTRO_TARGET_PLAYER(1, ASTRO_ACT_UP),
          "player 2's South is now Up");
    bindings_set_pad(s, ASTROSESSION_KEY_5, PAD_SOUTH, &stole);
    CHECK(stole >= 0, "a keypad key steals South from a player");
    CHECK(bindings_target_for_pad(0, PAD_SOUTH) == ASTROSESSION_KEY_5,
          "every pad's South is now keypad 5");

    /* a trigger axis can stand in for a button; the knob axis can move */
    bindings_set_pad(s, ASTRO_TARGET_PLAYER(0, ASTRO_ACT_TRIGGER), PAD_LTRIG, &stole);
    CHECK(bindings_target_for_pad(0, PAD_LTRIG) == ASTRO_TARGET_PLAYER(0, ASTRO_ACT_TRIGGER),
          "left trigger fires player 1's Trigger");
    bindings_set_pad(s, ASTRO_TARGET_PLAYER(0, ASTRO_ACT_KNOB), ASTRO_PAD_AXIS_BASE + 0, &stole);
    CHECK(bindings_knob_axis(0) == ASTRO_PAD_AXIS_BASE, "knob rebound to left stick X");
    CHECK(bindings_knob_axis(1) == PAD_RIGHTX, "player 2's knob axis unchanged");

    /* clear */
    bindings_clear(s, ASTRO_TARGET_PLAYER(0, ASTRO_ACT_TRIGGER));
    CHECK(bindings_target_keysym(ASTRO_TARGET_PLAYER(0, ASTRO_ACT_TRIGGER)) == 0 &&
          bindings_target_pad(ASTRO_TARGET_PLAYER(0, ASTRO_ACT_TRIGGER)) == -1,
          "clear unbinds key and pad");

    astrosession_free(s);

    /* the overrides persist across a reopen */
    s = astrosession_new(&paths);
    if (!s) return 1;
    CHECK(bindings_target_for_key('7') == ASTROSESSION_KEY_1, "key remap persisted");
    CHECK(bindings_target_for_pad(0, PAD_SOUTH) == ASTROSESSION_KEY_5, "pad remap persisted");
    CHECK(bindings_knob_axis(0) == ASTRO_PAD_AXIS_BASE, "axis remap persisted");
    CHECK(bindings_target_keysym(ASTRO_TARGET_PLAYER(0, ASTRO_ACT_TRIGGER)) == 0,
          "clear persisted");

    /* reset restores defaults */
    bindings_reset_defaults(s);
    CHECK(bindings_target_for_key('7') == ASTROSESSION_KEY_7, "reset: '7' -> keypad 7");
    CHECK(bindings_target_for_pad(0, PAD_SOUTH) == ASTRO_TARGET_PLAYER(0, ASTRO_ACT_TRIGGER),
          "reset: South -> Trigger");
    CHECK(strcmp(astrosession_get_str(s, "bindings", "x"), "") == 0, "reset empties the store");

    /* names and labels */
    for (int t = 0; t < ASTRO_TARGET_COUNT; t++) {
        CHECK(bindings_target_label(t) != NULL && bindings_target_label(t)[0], "target has a label");
        CHECK(bindings_target_short_label(t)[0], "target has a short label");
    }
    char nm[64];
    astrosession_keysym_name(ASTROSESSION_KEYSYM_KP_5, nm, sizeof nm);
    CHECK(strcmp(nm, "Keypad 5") == 0, "keypad 5 named");
    astrosession_keysym_name('a', nm, sizeof nm);
    CHECK(strcmp(nm, "A") == 0, "letters are upper-case names");

    /* native key codes land on the physical key */
    CHECK(astrosession_keysym_from_evdev(2) == '1', "evdev KEY_1 -> '1'");
    CHECK(astrosession_keysym_from_evdev(79) == ASTROSESSION_KEYSYM_KP_1, "evdev KP1");
    CHECK(astrosession_keysym_from_win_scancode(0x02, 0) == '1', "win scan 02 -> '1'");
    CHECK(astrosession_keysym_from_win_scancode(0x1C, 1) == ASTROSESSION_KEYSYM_KP_ENTER,
          "win E0 1C -> Keypad Enter");
    CHECK(astrosession_keysym_from_macos_keycode(0x12) == '1', "mac keycode 0x12 -> '1'");

    astrosession_free(s);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd)) { /* ignore */ }

    fprintf(stderr, "bindings_test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
