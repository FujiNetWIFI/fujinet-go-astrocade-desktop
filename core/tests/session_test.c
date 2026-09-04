/*
 * session_test -- the session layer over a temp config/data dir: settings
 * round-trip and persistence, option defaults, ROM presence, BIOS import by
 * CRC, and (if a BIOS is available) a start/stop cycle producing frames.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "astrosession.h"
#include "test_tmpdir.h"

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

int main(int argc, char **argv)
{
    char tmpl[512];
    test_tmpl(tmpl, sizeof tmpl, "astrosession_test");
    char *dir = mkdtemp(tmpl);
    if (!dir) { perror("mkdtemp"); return 1; }
    char cfg[512], data[512];
    snprintf(cfg, sizeof cfg, "%s/config", dir);
    snprintf(data, sizeof data, "%s/data", dir);

    astrosession_paths paths = { cfg, data };
    astrosession *s = astrosession_new(&paths);
    CHECK(s != NULL, "astrosession_new");
    if (!s) return 1;

    /* settings round-trip + persistence across a reopen */
    astrosession_set_int(s, "bios", ASTROSESSION_BIOS_BALLYHLC);
    astrosession_set_str(s, "greeting", "hello");
    CHECK(astrosession_get_int(s, "bios", -1) == ASTROSESSION_BIOS_BALLYHLC,
          "get_int round-trip");
    CHECK(strcmp(astrosession_get_str(s, "greeting", ""), "hello") == 0,
          "get_str round-trip");
    CHECK(astrosession_get_int(s, "missing", 42) == 42, "get_int default");
    astrosession_free(s);

    s = astrosession_new(&paths);
    CHECK(s != NULL, "reopen");
    CHECK(astrosession_get_int(s, "bios", -1) == ASTROSESSION_BIOS_BALLYHLC,
          "settings persisted across reopen");

    /* default opts read from the store */
    astrosession_start_opts opts;
    astrosession_default_opts(s, &opts);
    CHECK(opts.bios == ASTROSESSION_BIOS_BALLYHLC, "default_opts bios");
    CHECK(opts.exp == ASTROSESSION_EXP_NONE, "default_opts exp");
    CHECK(opts.cart_path == NULL, "default_opts cart NULL");

    /* label tables NULL-terminate */
    CHECK(astrosession_bios_name(0) != NULL, "bios_name 0");
    CHECK(astrosession_bios_name(3) == NULL, "bios_name past end");
    CHECK(astrosession_exp_name(0) != NULL, "exp_name 0");
    CHECK(astrosession_exp_name(7) == NULL, "exp_name past end");

    /* ROM import: a real BIOS passed as argv[1] must import; a bogus file
     * must be rejected. argv[1] is always passed by CMake even when the
     * ROM-less build's tools/roms is empty (as in CI), so an access() check
     * -- not just argc -- decides whether a real BIOS is actually there. */
    int have_bios = argc > 1 && access(argv[1], F_OK) == 0;
    if (have_bios) {
        char name[64];
        int rc = astrosession_import_rom(s, argv[1], name, sizeof name);
        CHECK(rc == 0, "import real BIOS");
        CHECK(astrosession_has_system_roms(s), "has_system_roms after import");

        /* a truncated copy must be rejected */
        char bogus[512];
        snprintf(bogus, sizeof bogus, "%s/bogus.bin", dir);
        FILE *f = fopen(bogus, "wb");
        if (f) { fputc(0x55, f); fclose(f); }
        CHECK(astrosession_import_rom(s, bogus, NULL, 0) != 0,
              "reject non-BIOS file");

        /* with a BIOS present, a start/stop cycle should run and paint.
         * argv[1] is astro.bin, so point default_opts back at it -- the
         * settings round-trip check above left "bios" set to BALLYHLC,
         * a variant this test never imports. */
        astrosession_set_int(s, "bios", ASTROSESSION_BIOS_ASTRO);
        CHECK(astrosession_start(s, NULL) == 0, astrosession_last_error(s));
        if (astrosession_is_running(s)) {
            static uint32_t fb[ASTROSESSION_FB_WIDTH * ASTROSESSION_FB_HEIGHT];
            uint64_t serial = 0;
            int painted = 0;
            for (int i = 0; i < 200 && !painted; i++) {
                usleep(10 * 1000);
                painted = astrosession_copy_frame(s, fb, &serial);
            }
            CHECK(painted, "session produced a frame");
            astrosession_reset_game(s);   /* must not crash / not block */
            astrosession_stop(s);
        }
    } else {
        fprintf(stderr, "session_test: SKIP: no BIOS at %s -- skipping "
                "start/stop\n", argc > 1 ? argv[1] : "(none)");
    }

    astrosession_free(s);

    /* best-effort cleanup */
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd)) { /* ignore */ }

    fprintf(stderr, "session_test: %s\n", failures ? "FAIL" : "PASS");
    if (failures) return 1;
    return have_bios ? 0 : 77;
}
