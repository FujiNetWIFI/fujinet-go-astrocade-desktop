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

#define PAD_SOUTH ASTRO_PAD_BTN_SOUTH
#define PAD_RIGHTX (ASTRO_PAD_AXIS_BASE + 2)

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

    int stole = -99;

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

    CHECK(bindings_target_for_key(ASTROSESSION_KEYSYM_F9) == -1 &&
          bindings_target_for_key(ASTROSESSION_KEYSYM_F11) == -1 &&
          bindings_target_for_key(ASTROSESSION_KEYSYM_F12) == -1, "F9, F11 and F12 are unbound");

    /* pad: every player's pad has South = its own Trigger, the D-pad steers it,
     * right stick X = knob */
    for (int p = 0; p < ASTROSESSION_PLAYERS; p++) {
        CHECK(bindings_target_for_pad(p, PAD_SOUTH) ==
                  ASTRO_TARGET_PLAYER(p, ASTRO_ACT_TRIGGER), "South -> own Trigger");
        CHECK(bindings_target_for_pad(p, ASTRO_PAD_BTN_DPAD_UP) ==
                  ASTRO_TARGET_PLAYER(p, ASTRO_ACT_UP), "D-pad up -> own Up");
        CHECK(bindings_target_for_pad(p, ASTRO_PAD_BTN_DPAD_RIGHT) ==
                  ASTRO_TARGET_PLAYER(p, ASTRO_ACT_RIGHT), "D-pad right -> own Right");
        CHECK(bindings_knob_axis(p) == PAD_RIGHTX, "right stick X is the knob axis");
    }

    /* F9, F11 and F12 cannot be bound; the table is untouched */
    stole = -99;
    bindings_set_keysym(s, ASTROSESSION_KEY_C, ASTROSESSION_KEYSYM_F9, &stole);
    bindings_set_keysym(s, ASTROSESSION_KEY_C, ASTROSESSION_KEYSYM_F11, &stole);
    bindings_set_keysym(s, ASTROSESSION_KEY_C, ASTROSESSION_KEYSYM_F12, &stole);
    CHECK(bindings_target_for_key(ASTROSESSION_KEYSYM_F9) == -1 &&
          bindings_target_keysym(ASTROSESSION_KEY_C) == 'c', "F9/F11/F12 refused, the old key kept");

    /* a key remap steals */
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

    /* the triggers are buttons now; raw buttons and hat positions are bindable;
     * an axis belongs to the knob alone and a button never to it */
    bindings_set_pad(s, ASTRO_TARGET_PLAYER(0, ASTRO_ACT_TRIGGER), ASTRO_PAD_BTN_LEFT_TRIGGER, &stole);
    CHECK(bindings_target_for_pad(0, ASTRO_PAD_BTN_LEFT_TRIGGER) == ASTRO_TARGET_PLAYER(0, ASTRO_ACT_TRIGGER),
          "left trigger fires player 1's Trigger");
    bindings_set_pad(s, ASTRO_TARGET_PLAYER(2, ASTRO_ACT_UP), ASTRO_PAD_BTN_RAW_BASE + 5, &stole);
    bindings_set_pad(s, ASTRO_TARGET_PLAYER(2, ASTRO_ACT_DOWN), ASTRO_PAD_HAT_BASE + 4, &stole);
    CHECK(bindings_target_for_pad(2, ASTRO_PAD_BTN_RAW_BASE + 5) == ASTRO_TARGET_PLAYER(2, ASTRO_ACT_UP) &&
          bindings_target_for_pad(2, ASTRO_PAD_HAT_BASE + 4) == ASTRO_TARGET_PLAYER(2, ASTRO_ACT_DOWN),
          "a raw button and a hat position are bindable");
    bindings_set_pad(s, ASTRO_TARGET_PLAYER(0, ASTRO_ACT_TRIGGER), ASTRO_PAD_AXIS_BASE, &stole);
    CHECK(bindings_target_pad(ASTRO_TARGET_PLAYER(0, ASTRO_ACT_TRIGGER)) == ASTRO_PAD_BTN_LEFT_TRIGGER,
          "an axis is refused for a digital target");
    bindings_set_pad(s, ASTRO_TARGET_PLAYER(0, ASTRO_ACT_KNOB), ASTRO_PAD_BTN_EAST, &stole);
    CHECK(bindings_knob_axis(0) == PAD_RIGHTX, "a button is refused for the knob");
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

    /* a version 1 settings string (two system actions, SDL button numbers, axes
     * on any target) is migrated once, not reset */
    astrosession_set_str(s, "bindings",
        "30.p:3 28.k:122 4.p:11 34.p:132 35.p:129 33.p:131 1.k:65478 12.p:20");
    astrosession_set_int(s, "bindings_ver", 1);
    bindings_init(s);
    CHECK(bindings_target_pad(ASTRO_TARGET_PLAYER(0, ASTRO_ACT_TRIGGER)) == ASTRO_PAD_BTN_NORTH,
          "v1: player 1's trigger (target 30 -> 31) keeps button 3");
    CHECK(bindings_target_keysym(ASTRO_TARGET_PLAYER(0, ASTRO_ACT_LEFT)) == 'z',
          "v1: player 1's Left (28 -> 29) keeps its key");
    CHECK(bindings_target_pad(ASTRO_TARGET_PLAYER(1, ASTRO_ACT_UP)) == ASTRO_PAD_BTN_LEFT_TRIGGER,
          "v1: a trigger axis on a digital target becomes the trigger button");
    CHECK(bindings_target_pad(ASTRO_TARGET_PLAYER(1, ASTRO_ACT_DOWN)) == -1,
          "v1: a stick axis on a digital target is dropped");
    CHECK(bindings_knob_axis(0) == ASTRO_PAD_AXIS_BASE + 3, "v1: the knob keeps its axis");
    CHECK(bindings_target_for_key(ASTROSESSION_KEYSYM_F9) == -1 &&
          bindings_target_keysym(ASTROSESSION_KEY_UP) == 'u', "v1: F9 is dropped (the key goes back to default)");
    CHECK(bindings_target_pad(12) == ASTRO_PAD_BTN_RAW_BASE + 20,
          "v1: a button above the named ones goes to the raw band");
    CHECK(bindings_target_pad(4) == ASTRO_PAD_BTN_DPAD_UP &&
          bindings_target_pad(ASTRO_TARGET_PLAYER(0, ASTRO_ACT_UP)) == -1 &&
          bindings_target_for_pad(0, ASTRO_PAD_BTN_DPAD_UP) == 4,
          "v1: a file's binding displaces the default holder of the same button");
    CHECK(astrosession_get_int(s, "bindings_ver", 0) == 2, "v1: rewritten as version 2");
    bindings_init(s);                       /* reload what it wrote */
    CHECK(bindings_target_pad(ASTRO_TARGET_PLAYER(0, ASTRO_ACT_TRIGGER)) == ASTRO_PAD_BTN_NORTH &&
          bindings_target_pad(12) == ASTRO_PAD_BTN_RAW_BASE + 20 && bindings_knob_axis(0) == ASTRO_PAD_AXIS_BASE + 3,
          "v2: the migrated file reads back the same");
    bindings_reset_defaults(s);

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
