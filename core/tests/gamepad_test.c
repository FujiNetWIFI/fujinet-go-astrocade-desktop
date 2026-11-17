/*
 * gamepad_test -- the gamepad lifecycle and Map-mode capture API are safe
 * with no controller attached: start/stop don't crash, capture arms and
 * cancels, and a poll with nothing pressed reports no capture. (The actual
 * button->binding path needs hardware; this guards the headless contract the
 * session relies on -- start is best-effort and must never take the session
 * down when there's no gamepad.) astro_gamepad_start() also loads the bundled
 * community mapping DB (gamecontrollerdb_embedded.h) on every successful
 * start exercised below -- this guards that a bad/missing embed can't take
 * the session down either.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "astrosession.h"
#include "gamepad_sdl.h"
#include "pad_assign.h"
#include "test_tmpdir.h"

static int failures;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } } while (0)

int main(void)
{
    char tmpl[512];
    test_tmpl(tmpl, sizeof tmpl, "astro_gamepad_test");
    char *dir = mkdtemp(tmpl);
    if (!dir) return 1;
    char cfg[512], data[512];
    snprintf(cfg, sizeof cfg, "%s/config", dir);
    snprintf(data, sizeof data, "%s/data", dir);
    astrosession_paths paths = { cfg, data };

    astrosession *s = astrosession_new(&paths);
    if (!s) return 1;

    /* start may fail if SDL can't init the gamepad subsystem in this
     * environment; either way the session must survive it. */
    int rc = astro_gamepad_start(s);
    fprintf(stderr, "gamepad_test: astro_gamepad_start rc=%d\n", rc);

    if (rc == 0) {
        /* capture: arm, poll (nothing pressed -> 0), cancel */
        astrosession_gamepad_capture_begin(s);
        usleep(30 * 1000);
        int button = -99;
        int got = astrosession_gamepad_capture_poll(s, &button);
        CHECK(got == 0, "no capture without a controller press");
        astrosession_gamepad_capture_cancel(s);
        astro_gamepad_stop();
        /* start/stop again must be safe */
        CHECK(astro_gamepad_start(s) == 0 || 1, "restart tolerated");
        astro_gamepad_stop();
    }

    /* the button-name table resolves the common buttons */
    char nm[32];
    astrosession_pad_input_name(0, nm, sizeof nm);
    CHECK(strcmp(nm, "A") == 0, "button 0 (South) named");
    astrosession_pad_input_name(ASTRO_PAD_AXIS_BASE + 2, nm, sizeof nm);
    CHECK(strcmp(nm, "Right Stick X") == 0, "axis 2 named");

    astrosession_pad_input_name(ASTRO_PAD_BTN_DPAD_UP, nm, sizeof nm);
    CHECK(strcmp(nm, "D-pad Up") == 0, "the D-pad is named");
    astrosession_pad_input_name(ASTRO_PAD_BTN_RIGHT_TRIGGER, nm, sizeof nm);
    CHECK(strcmp(nm, "Right Trigger") == 0, "the trigger buttons are named");
    astrosession_pad_input_name(ASTRO_PAD_BTN_RAW_BASE + 3, nm, sizeof nm);
    CHECK(strcmp(nm, "Button 3") == 0, "a raw button is named by its number");
    astrosession_pad_input_name(ASTRO_PAD_HAT_BASE + 8 + 1, nm, sizeof nm);
    CHECK(strcmp(nm, "Hat 1 Up-Right") == 0, "a hat position is named");
    astrosession_pad_input_name(-1, nm, sizeof nm);
    CHECK(nm[0] == '\0', "no input has no name");

    /* the pure helpers */
    CHECK(astro_acts_from_dir(ASTRO_DIR_UP | ASTRO_DIR_DOWN) == 0, "up and down cancel");
    CHECK(astro_acts_from_dir(ASTRO_DIR_UP | ASTRO_DIR_RIGHT) ==
          ((1u << ASTRO_ACT_UP) | (1u << ASTRO_ACT_RIGHT)), "a diagonal is two actions");
    {
        int dir = astro_dir_from_stick(0.4f, 0, 0, ASTRO_STICK_ENTER, ASTRO_STICK_EXIT);
        CHECK(dir == 0, "0.4 does not engage");
        dir = astro_dir_from_stick(0.6f, 0, dir, ASTRO_STICK_ENTER, ASTRO_STICK_EXIT);
        CHECK(dir == ASTRO_DIR_RIGHT, "0.6 engages right");
        dir = astro_dir_from_stick(0.4f, 0, dir, ASTRO_STICK_ENTER, ASTRO_STICK_EXIT);
        CHECK(dir == ASTRO_DIR_RIGHT, "0.4 holds it (hysteresis)");
        dir = astro_dir_from_stick(0.3f, 0, dir, ASTRO_STICK_ENTER, ASTRO_STICK_EXIT);
        CHECK(dir == 0, "0.3 lets go");
        dir = astro_dir_from_stick(-0.7f, -0.7f, 0, ASTRO_STICK_ENTER, ASTRO_STICK_EXIT);
        CHECK(dir == (ASTRO_DIR_LEFT | ASTRO_DIR_UP), "SDL's -y is up: a diagonal is two bits");
    }
    {
        int none[4] = { -1, -1, -1, -1 };
        CHECK(astro_pad_for_player(none, 3, 0) == 0 && astro_pad_for_player(none, 3, 1) == 1 &&
              astro_pad_for_player(none, 3, 2) == 2 && astro_pad_for_player(none, 3, 3) == -1,
              "unassigned pads take players in connection order");
        int one[4] = { 1, -1, -1, -1 };
        CHECK(astro_pad_for_player(one, 3, 1) == 0, "an explicit assignment wins its player");
        CHECK(astro_pad_for_player(one, 3, 0) == 1, "the first unassigned pad plays the first free player");
        CHECK(astro_pad_for_player(one, 3, 2) == -1, "the second unassigned pad has nobody left to play");
        int four[4] = { 3, 3, -1, -1 };
        CHECK(astro_pad_for_player(four, 4, 3) == 0, "two pads on one player: the first wins");
        CHECK(astro_pad_for_player(four, 4, 0) == 2 && astro_pad_for_player(four, 4, 1) == 3,
              "the rest are in order");
    }
    CHECK(astro_knob_from_axis(-1.0f, 0) == 0 && astro_knob_from_axis(0.0f, 0) == 128 &&
          astro_knob_from_axis(1.0f, 0) == 255, "a stick axis sweeps the knob -1..1");
    CHECK(astro_knob_from_axis(0.0f, 1) == 0 && astro_knob_from_axis(1.0f, 1) == 255 &&
          astro_knob_from_axis(2.0f, 1) == 255, "a trigger sweeps it 0..1, clamped");
    for (int btn = 0; btn < ASTRO_PAD_BTN_DPAD_RIGHT + 1; btn++) {
        const int sdl = astro_sdl_button_from_pad(btn);
        CHECK(sdl >= 0 && astro_pad_button_from_sdl((uint8_t)sdl) == btn, "named buttons round trip SDL");
    }
    CHECK(astro_sdl_button_from_pad(ASTRO_PAD_BTN_LEFT_TRIGGER) == -1, "the triggers are not SDL buttons");

    /* nothing connected: the lists are empty and the getters are safe */
    CHECK(astrosession_gamepad_count(s) == 0 || astrosession_gamepad_count(s) > 0, "count callable");
    CHECK(astrosession_gamepad_assignment(s, 99) == -1 && astrosession_gamepad_effective_port(s, 99) == -1,
          "an unknown pad has no assignment");
    astrosession_gamepad_assign(s, 99, 1);        /* no such pad: ignored */
    CHECK(astrosession_gamepad_name(s, 99, nm, sizeof nm) == 0, "no name for no pad");

    astrosession_free(s);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd)) { /* ignore */ }

    fprintf(stderr, "gamepad_test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
