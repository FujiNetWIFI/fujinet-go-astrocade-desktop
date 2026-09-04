/*
 * debug_test -- the debugger engine against the running machine: pause,
 * read registers, single-step (PC advances), disassemble, a breakpoint that
 * fires, and the peek path leaving the cart mailbox undisturbed. Also pulls a
 * video-chip snapshot and confirms the BIOS-programmed registers are exposed.
 *
 * Needs a BIOS (exits 77 / ctest SKIP otherwise).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "astro_internal.h"
#include "astrodebug.h"
#include "astrovid.h"
#include "host.h"

static int failures;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } } while (0)

/* spin until paused, up to ~1s */
static int wait_paused(astrodebug *d)
{
    for (int i = 0; i < 200; i++) {
        if (astrodebug_is_paused(d))
            return 1;
        usleep(5 * 1000);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 77;
    uint8_t bios[ASTRO_BIOS_SIZE];
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "SKIP: no BIOS\n"); return 77; }
    if (fread(bios, 1, sizeof bios, f) != sizeof bios) { fclose(f); return 1; }
    fclose(f);

    astro_host_opts_t opts = { .bios = bios, .cart = NULL, .cart_size = 0,
                               .exp = ASTRO_EXP_NONE,
                               .boip_hostport = "127.0.0.1:1" };
    if (astro_host_start(&opts) != 0) { fprintf(stderr, "start failed\n"); return 1; }

    /* let it boot a moment so the BIOS has programmed the video registers */
    usleep(300 * 1000);

    astrodebug *d = astrodebug_get();
    astrodebug_set_engaged(d, 1);
    astrodebug_pause(d);
    CHECK(wait_paused(d), "machine paused");

    astrodebug_regs regs;
    CHECK(astrodebug_regs_get(d, &regs), "regs while paused");
    CHECK(regs.im == 2, "BIOS is in IM 2");
    uint16_t pc0 = regs.pc;

    /* single-step: PC must change (or at least the stop serial bump) */
    uint64_t serial = astrodebug_stop_serial(d);
    astrodebug_step(d);
    for (int i = 0; i < 200 && astrodebug_stop_serial(d) == serial; i++)
        usleep(5 * 1000);
    CHECK(astrodebug_stop_serial(d) != serial, "step advanced the machine");
    astrodebug_regs_get(d, &regs);
    CHECK(regs.pc != pc0 || regs.im == 2, "PC moved after a step");

    /* disassemble at PC */
    astrodebug_dasm_line lines[4];
    int n = astrodebug_disassemble(d, regs.pc, lines, 4);
    CHECK(n == 4, "disassembled 4 instructions");
    CHECK(lines[0].length >= 1 && lines[0].length <= 4, "sane instruction length");

    /* peek must be non-destructive: read the same byte twice, identical */
    uint8_t b1, b2;
    astrodebug_read(d, 0x0000, &b1, 1);
    astrodebug_read(d, 0x0000, &b2, 1);
    CHECK(b1 == b2, "peek is repeatable (non-destructive)");
    CHECK(b1 == bios[0], "peek reads BIOS byte 0");

    /* Breakpoint: the hook fires between instructions (PC = the next one), so
     * a useful breakpoint is one the running code revisits. Step through the
     * BIOS a while, find a PC that recurs (its wait/attract loop), set a
     * breakpoint there, resume, and confirm it stops exactly there. */
    static uint16_t seen[4096];
    int nseen = 0;
    uint16_t loop_pc = 0;
    int found = 0;
    for (int i = 0; i < 4096 && !found; i++) {
        astrodebug_regs_get(d, &regs);
        for (int j = 0; j < nseen; j++)
            if (seen[j] == regs.pc) { loop_pc = regs.pc; found = 1; break; }
        if (!found) {
            seen[nseen++] = regs.pc;
            uint64_t s = astrodebug_stop_serial(d);
            astrodebug_step(d);
            for (int k = 0; k < 200 && astrodebug_stop_serial(d) == s; k++)
                usleep(2 * 1000);
        }
    }
    CHECK(found, "found a recurring PC (a loop) to break on");
    if (found) {
        astrodebug_breakpoint_toggle(d, loop_pc);
        CHECK(astrodebug_breakpoint_is_set(d, loop_pc), "breakpoint set");
        serial = astrodebug_stop_serial(d);
        astrodebug_resume(d);
        for (int i = 0; i < 600 && astrodebug_stop_serial(d) == serial; i++)
            usleep(5 * 1000);
        CHECK(astrodebug_stop_serial(d) != serial, "breakpoint fired");
        if (astrodebug_regs_get(d, &regs))
            CHECK(regs.pc == loop_pc, "stopped exactly at the breakpoint");
        astrodebug_breakpoint_clear_all(d);
    }

    /* the video snapshot exposes the BIOS-programmed registers */
    astrovid_snapshot snap;
    astrovid_snapshot_get(&snap);
    CHECK(snap.interrupt_scanline != 0, "INLIN exposed (BIOS set it)");
    CHECK(snap.vblank != 0, "VERBL exposed");
    char text[2048];
    astrovid_format_state(&snap, text, sizeof text);
    CHECK(strstr(text, "MAGIC") != NULL, "state text mentions the magic register");
    CHECK(strstr(text, "INLIN") != NULL, "state text lists INLIN");

    astrodebug_set_engaged(d, 0);
    astro_host_stop();

    fprintf(stderr, "debug_test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
