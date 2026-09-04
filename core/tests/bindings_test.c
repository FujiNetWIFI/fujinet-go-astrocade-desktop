/*
 * bindings_test -- the remappable binding table: defaults resolve, a remap
 * steals the input from whoever held it, an override persists across a
 * reopen, and reset restores defaults.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "astrosession.h"
#include "bindings.h"

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

int main(void)
{
    char tmpl[] = "/tmp/astro_bindings_test.XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) { perror("mkdtemp"); return 1; }
    char cfg[512], data[512];
    snprintf(cfg, sizeof cfg, "%s/config", dir);
    snprintf(data, sizeof data, "%s/data", dir);
    astrosession_paths paths = { cfg, data };

    astrosession *s = astrosession_new(&paths);
    if (!s) return 1;

    /* defaults: '7' resolves to keypad 7; Backspace to Reset Game */
    astro_mapping m = bindings_resolve_keysym('7');
    CHECK(m.kind == ASTRO_MAP_KEY && m.value == ASTROSESSION_KEY_7,
          "'7' -> keypad 7 by default");
    m = bindings_resolve_keysym(ASTROSESSION_KEYSYM_BACKSPACE);
    CHECK(m.kind == ASTRO_MAP_SYSACT && m.value == ASTROSESSION_SYSACT_RESET_GAME,
          "Backspace -> Reset Game by default");

    /* remap keypad 1 to the '7' key: '7' now drives keypad 1, and keypad 7
     * loses its binding (steal) */
    int stole = -99;
    bindings_set_keysym(s, ASTROSESSION_KEY_1, '7', &stole);
    CHECK(stole == ASTROSESSION_KEY_7, "remap stole '7' from keypad 7");
    m = bindings_resolve_keysym('7');
    CHECK(m.kind == ASTRO_MAP_KEY && m.value == ASTROSESSION_KEY_1,
          "'7' now drives keypad 1");
    CHECK(bindings_target_keysym(ASTROSESSION_KEY_7) == 0,
          "keypad 7 now unbound");

    astrosession_free(s);

    /* the override persists across a reopen */
    s = astrosession_new(&paths);
    if (!s) return 1;
    m = bindings_resolve_keysym('7');
    CHECK(m.kind == ASTRO_MAP_KEY && m.value == ASTROSESSION_KEY_1,
          "remap persisted across reopen");

    /* reset restores the default */
    bindings_reset_defaults(s);
    m = bindings_resolve_keysym('7');
    CHECK(m.kind == ASTRO_MAP_KEY && m.value == ASTROSESSION_KEY_7,
          "reset restored '7' -> keypad 7");

    /* labels exist for every target */
    for (int t = 0; t < ASTRO_TARGET_COUNT; t++)
        CHECK(bindings_target_label(t) != NULL, "target has a label");

    astrosession_free(s);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd)) { /* ignore */ }

    fprintf(stderr, "bindings_test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
