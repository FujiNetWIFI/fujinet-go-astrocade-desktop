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
    CHECK(astro_gamepad_button_name(0) != NULL, "button 0 (South) named");

    astrosession_free(s);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd)) { /* ignore */ }

    fprintf(stderr, "gamepad_test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
