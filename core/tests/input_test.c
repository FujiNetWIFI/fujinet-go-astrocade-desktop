/*
 * input_test -- the merged input layer: sources do not cancel each other, a
 * key drives its target, the knob holds where it was left and a key ramps it,
 * a pad axis and the dial take it, and the dial geometry is self-consistent.
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
#include "astro_internal.h"
#include "host.h"
#include "input.h"
#include "test_tmpdir.h"

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

/* what the machine sees: the handle mask, the keypad bytes and the knob (the
 * core stores the complement) */
static unsigned handle(int p) { return astro_host_machine()->handle[p]; }
static int knob(int p) { return (uint8_t)~astro_host_machine()->knob[p]; }

int main(void)
{
    char tmpl[512];
    test_tmpl(tmpl, sizeof tmpl, "astro_input_test");
    char *dir = mkdtemp(tmpl);
    if (!dir) return 1;
    char cfg[512], data[512];
    snprintf(cfg, sizeof cfg, "%s/config", dir);
    snprintf(data, sizeof data, "%s/data", dir);
    astrosession_paths paths = { cfg, data };
    astrosession *s = astrosession_new(&paths);
    if (!s) return 1;
    astro_input_resync();
    CHECK(knob(0) == ASTROSESSION_KNOB_CENTRE, "the knob starts centred");

    /* a key drives the stick; releasing a DIFFERENT source leaves it held */
    CHECK(astrosession_input_key(s, ASTROSESSION_KEYSYM_UP, 1, 0) == 1, "Up consumed");
    CHECK(handle(0) == ASTROSESSION_HANDLE_UP, "Up reaches the machine");
    astrosession_press(s, ASTRO_TARGET_PLAYER(0, ASTRO_ACT_UP), 1);
    astrosession_press(s, ASTRO_TARGET_PLAYER(0, ASTRO_ACT_UP), 0);
    CHECK(handle(0) == ASTROSESSION_HANDLE_UP, "the panel releasing does not cancel the key");
    astro_input_pad_state(0, 0, 0, ASTROSESSION_HANDLE_UP | ASTROSESSION_HANDLE_TRIGGER, 0);
    astrosession_input_key(s, ASTROSESSION_KEYSYM_UP, 0, 0);
    CHECK(handle(0) == (ASTROSESSION_HANDLE_UP | ASTROSESSION_HANDLE_TRIGGER),
          "the pad still holds Up and the trigger after the key lets go");
    astro_input_pad_state(0, 0, 0, 0, 0);
    CHECK(handle(0) == 0, "everything released");
    CHECK(astrosession_target_held(s, ASTRO_TARGET_PLAYER(0, ASTRO_ACT_UP)) == 0, "not held");

    /* auto-repeat does nothing, a release clears the rebinding's press */
    astrosession_input_key(s, ASTROSESSION_KEYSYM_SPACE, 1, 0);
    astrosession_input_key(s, ASTROSESSION_KEYSYM_SPACE, 1, 0);
    CHECK(astrosession_target_held(s, ASTRO_TARGET_PLAYER(0, ASTRO_ACT_TRIGGER)), "trigger held");
    astrosession_release_all(s);
    CHECK(handle(0) == 0, "release_all drops the keyboard's holds");

    /* Ctrl is never consumed on a press (accelerators win) */
    CHECK(astrosession_input_key(s, 'r', 1, 1) == 0, "Ctrl+R passes through");
    astrosession_input_key(s, 'r', 0, 0);

    /* keypad keys */
    astrosession_input_key(s, '7', 1, 0);
    CHECK(astrosession_target_held(s, ASTROSESSION_KEY_7), "keypad 7 held");
    CHECK(astro_host_machine()->keypad[0] || astro_host_machine()->keypad[1] ||
          astro_host_machine()->keypad[2] || astro_host_machine()->keypad[3],
          "a keypad bit reached the machine");
    astrosession_input_key(s, '7', 0, 0);
    CHECK(!astrosession_target_held(s, ASTROSESSION_KEY_7), "keypad 7 released");

    /* a system action key posts, and drains */
    CHECK(astrosession_input_key(s, ASTROSESSION_KEYSYM_BACKSPACE, 1, 0) == 1, "Backspace consumed");
    CHECK(astrosession_sysaction_take(s) == (1u << ASTROSESSION_SYSACT_RESET_GAME),
          "Reset Game posted");
    astrosession_input_key(s, ASTROSESSION_KEYSYM_BACKSPACE, 0, 0);

    /* the knob holds where it was left */
    astrosession_knob_set(s, 0, 200);
    CHECK(knob(0) == 200 && astrosession_knob_get(s, 0) == 200, "dial sets the knob");
    astro_input_tick(100);
    CHECK(knob(0) == 200, "with no key it stays put");
    astrosession_input_key(s, ']', 1, 0);
    astro_input_tick(100);
    CHECK(knob(0) == 225, "the knob key ramps up");
    astro_input_tick(1000);
    CHECK(knob(0) == 255, "and stops at 255");
    CHECK(astro_input_knob_active(), "a knob key is active");
    astrosession_input_key(s, ']', 0, 0);
    CHECK(!astro_input_knob_active() && knob(0) == 255, "released: it holds at 255");
    astrosession_input_key(s, '[', 1, 0);
    astro_input_tick(400);
    CHECK(knob(0) == 155, "the other key ramps down");
    astrosession_input_key(s, '[', 0, 0);

    /* a pad axis: the first sample only records, then a move takes the knob */
    astro_input_pad_knob(0, 0, 10);
    CHECK(knob(0) == 155, "the first axis sample does not move the knob");
    astro_input_pad_knob(0, 0, 11);
    CHECK(knob(0) == 155, "noise below the threshold is ignored");
    astro_input_pad_knob(0, 0, 90);
    CHECK(knob(0) == 90, "an axis move takes the knob");
    astrosession_knob_set(s, 0, 40);
    astro_input_pad_knob(0, 0, 91);
    CHECK(knob(0) == 40, "a still axis does not fight the dial");

    /* dial geometry round trips */
    CHECK(astrosession_knob_from_point(0, 0, 50) == -1, "dead centre");
    CHECK(astrosession_knob_from_point(0, -40, 50) == 128 || astrosession_knob_from_point(0, -40, 50) == 127,
          "straight up is the middle");
    CHECK(astrosession_knob_from_point(-40, 40, 50) == 0, "lower left is 0");
    CHECK(astrosession_knob_from_point(40, 40, 50) == 255, "lower right is 255");
    CHECK(astrosession_knob_from_point(0, 40, 50) == 0 || astrosession_knob_from_point(0, 40, 50) == 255,
          "straight down clamps to an end");
    for (int v = 0; v <= 255; v += 5) {
        const double a = astrosession_knob_angle(v);
        const double rad = a * 3.14159265358979 / 180.0;
        const int back = astrosession_knob_from_point(50 * __builtin_sin(rad),
                                                      -50 * __builtin_cos(rad), 60);
        CHECK(back >= v - 1 && back <= v + 1, "angle -> point -> value round trips");
    }

    /* reset game drops everything */
    astrosession_input_key(s, ASTROSESSION_KEYSYM_LEFT, 1, 0);
    astro_input_clear_all();
    CHECK(handle(0) == 0, "clear_all releases the sources");

    astrosession_free(s);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd)) { /* ignore */ }
    fprintf(stderr, "input_test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
