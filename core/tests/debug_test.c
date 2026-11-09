/*
 * debug_test -- the debugger engine (astrodebug.h) against the running
 * machine. Modelled on the ADAM desktop's debug_boot_test:
 *
 *   attach stops the machine; step / over / out / run-to / scanline / frame;
 *   registers read and edited (PC redirect); execute breakpoints with reason
 *   strings, hit counts and conditions; read/write/IN/OUT watchpoints (and
 *   READ ignoring the instruction's own fetches); disassembly lines, labels
 *   and port names; the console; symbol files; save kinds; the memory map;
 *   detach resumes and takes the hooks out;
 *   and the two regressions that motivated the engine's rewrite:
 *     1. stopping and restarting the host while the machine is PARKED must
 *        not hang (a SIGALRM watchdog fails the test instead of the CI job);
 *     2. after a restart with the engine attached, the new machine stops at
 *        its first instruction and breakpoints still fire.
 *
 * A tiny hand-assembled program in RL64 expansion RAM is the test subject
 * (interrupts off), so every address and stop below is exact; the real BIOS
 * boots the machine around it. Needs a BIOS (exits 77 / ctest SKIP otherwise).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef _WIN32
/* no SIGALRM on Windows: the restart watchdog is POSIX-only there (a hang
 * falls to the CI job timeout) */
static unsigned alarm(unsigned s) { (void)s; return 0; }
#endif

#include "astro_internal.h"
#include "astrodebug.h"
#include "astromemtools.h"
#include "astrovidtab.h"
#include "astrovid.h"
#include "host.h"

static int failures;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", m, __LINE__); failures++; } } while (0)

static void on_alarm(int sig)
{
    static const char msg[] = "FAIL: watchdog fired -- the test hung\n";
    (void)sig;
    if (write(2, msg, sizeof msg - 1) < 0) {}
    _exit(1);
}

static astrodebug *D;

static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }

static int wait_stopped(int ms)
{
    for (int i = 0; i < ms / 2; i++) {
        if (astrodebug_is_stopped(D))
            return 1;
        sleep_ms(2);
    }
    return astrodebug_is_stopped(D);
}

/* Issue `go` (an action that releases the machine) and wait for the next
 * stop; 0 if it did not stop in time. */
#define GO(stmt) (astrodebug_is_stopped(D) ? ((stmt), go_wait()) : 0)
static int go_wait(void)
{
    sleep_ms(1);
    return wait_stopped(3000);
}

static astrodebug_cpu cpu(void)
{
    astrodebug_cpu c;
    astrodebug_cpu_get(D, &c);
    return c;
}

static char reason[160];
static int reason_pc;
static const char *why(void)
{
    astrodebug_stop_reason(D, reason, sizeof reason, &reason_pc);
    return reason;
}

static int stopped_at(int pc, const char *prefix)
{
    why();
    return astrodebug_is_stopped(D) && cpu().pc == pc && reason_pc == pc &&
           strncmp(reason, prefix, strlen(prefix)) == 0;
}

/* ---- the test program, at $8000 -------------------------------------------
 *   8000 3E 5A        LD A,$5A
 *   8002 D3 10        OUT ($10),A        sound register 0
 *   8004 32 00 41     LD ($4100),A       screen RAM
 *   8007 CD 20 80     CALL $8020
 *   800A 3A 00 41     LD A,($4100)
 *   800D DB 10        IN A,($10)         player 1 handle
 *   800F 06 03        LD B,3
 *   8011 10 FE        DJNZ $8011
 *   8013 C3 13 80     JP $8013
 *   8020 3E 11        LD A,$11
 *   8022 C9           RET
 */
static const uint8_t prog[] = {
    0x3E, 0x5A, 0xD3, 0x10, 0x32, 0x00, 0x41, 0xCD, 0x20, 0x80,
    0x3A, 0x00, 0x41, 0xDB, 0x10, 0x06, 0x03, 0x10, 0xFE,
    0xC3, 0x13, 0x80,
};
static const uint8_t sub[] = { 0x3E, 0x11, 0xC9 };

/* Stopped: put the program's entry in place (interrupts off, a stack). */
static void restart_prog(void)
{
    for (size_t i = 0; i < sizeof prog; i++)
        astrodebug_write(D, (uint16_t)(0x8000 + i), prog[i]);
    for (size_t i = 0; i < sizeof sub; i++)
        astrodebug_write(D, (uint16_t)(0x8020 + i), sub[i]);
    astrodebug_cpu_set(D, ASTRO_REG_IFF1, 0);
    astrodebug_cpu_set(D, ASTRO_REG_IFF2, 0);
    astrodebug_cpu_set(D, ASTRO_REG_SP, 0xFF00);
    astrodebug_cpu_set(D, ASTRO_REG_PC, 0x8000);
}

static int has(const char *text, const char *needle)
{
    return strstr(text, needle) != NULL;
}

static int read_file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fclose(f);
    return (int)n;
}

static int stop_cb_calls, stop_cb_kind;
static void stop_cb(void *ud, int kind, uint16_t pc)
{
    (void)ud; (void)pc;
    stop_cb_calls++;
    stop_cb_kind = kind;
}

int main(int argc, char **argv)
{
    uint8_t bios[ASTRO_BIOS_SIZE];
    char out[4096], err[128];
    astrodebug_cpu c, c2;

    if (argc < 2)
        return 77;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "SKIP: no BIOS\n"); return 77; }
    if (fread(bios, 1, sizeof bios, f) != sizeof bios) { fclose(f); return 1; }
    fclose(f);

#ifndef _WIN32
    signal(SIGALRM, on_alarm);
#endif
    alarm(120);

    char tmpl[] = "/tmp/astro_debug_test_XXXXXX";
    if (!mkdtemp(tmpl)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
    char path[256];

    astro_host_opts_t opts = { .bios = bios, .cart = NULL, .cart_size = 0,
                               .exp = ASTRO_EXP_RL64_RAM,
                               .boip_hostport = "127.0.0.1:1" };
    D = astrodebug_get();
    astrodebug_fujinet_install(D);
    astrodebug_set_stop_callback(D, stop_cb, NULL);

    /* ---- not running: the calls are harmless -------------------------------- */
    CHECK(!astrodebug_is_stopped(D), "not stopped before the session starts");
    CHECK(astrodebug_read(D, 0, (uint8_t[4]){0}, 4) == 0, "read answers 0 when off");
    astrodebug_cpu_get(D, &c);
    CHECK(c.pc == 0 && c.cycles == 0, "cpu_get is zeros when off");

    CHECK(astro_host_start(&opts) == 0, "host start");
    sleep_ms(300);                       /* let the BIOS program the chip */
    CHECK(!astrodebug_is_attached(D) && !astrodebug_is_stopped(D),
          "running free when detached");
    CHECK(astro_host_machine()->instr_hook == NULL &&
          astro_host_machine()->bus_hook == NULL, "no hooks while detached");

    /* ---- attach stops ---------------------------------------------------------- */
    unsigned gen0 = astrodebug_generation(D);
    astrodebug_attach(D);
    CHECK(wait_stopped(1000), "attach stops the machine");
    CHECK(astrodebug_generation(D) != gen0, "attach bumps the generation");
    CHECK(strcmp(why(), "stopped") == 0, "reason is \"stopped\"");
    CHECK(stop_cb_calls >= 1 && stop_cb_kind == ASTRODBG_STOP_PAUSE, "stop callback fired (pause)");
    CHECK(astro_host_machine()->instr_hook != NULL, "instruction hook installed");
    CHECK(astro_host_machine()->bus_hook == NULL, "no bus hook without watchpoints");
    c = cpu();
    CHECK(c.im == 2, "the BIOS runs IM 2");
    CHECK(reason_pc == c.pc, "stop reason address is the PC");
    uint64_t cyc = c.cycles;
    sleep_ms(50);
    c2 = cpu();
    CHECK(c2.pc == c.pc && c2.cycles == cyc, "parked: nothing advances");

    /* ---- registers -------------------------------------------------------------- */
    restart_prog();
    c = cpu();
    CHECK(c.pc == 0x8000 && c.sp == 0xFF00 && !c.iff1, "cpu_set PC/SP/IFF1");
    astrodebug_cpu_set(D, ASTRO_REG_BC, 0x1234);
    astrodebug_cpu_set(D, ASTRO_FLAG_Z, 1);
    astrodebug_cpu_set(D, ASTRO_REG_AF2, 0xABCD);
    c = cpu();
    CHECK(c.bc == 0x1234 && c.zf == 1 && c.af2 == 0xABCD, "BC, Z flag, AF' edits");
    astrodebug_cpu_set(D, ASTRO_FLAG_Z, 0);
    astrodebug_cpu_set(D, ASTRO_REG_BC, 0);

    /* ---- memory ------------------------------------------------------------------ */
    uint8_t b[4];
    CHECK(astrodebug_read(D, 0x8000, b, 4) == 4 && b[0] == 0x3E && b[1] == 0x5A, "read RAM");
    uint8_t rom0;
    astrodebug_read(D, 0x1500, &rom0, 1);
    astrodebug_write(D, 0x1500, (uint8_t)(rom0 ^ 0xFF));
    astrodebug_read(D, 0x1500, b, 1);
    CHECK(b[0] == rom0, "a write to ROM is dropped");

    /* ---- stepping ------------------------------------------------------------------ */
    unsigned g = astrodebug_generation(D);
    CHECK(GO(astrodebug_step(D)), "step stops");
    CHECK(stopped_at(0x8002, "step"), "step: LD A,n -> $8002, reason \"step\"");
    CHECK(astrodebug_generation(D) != g, "step bumps the generation");
    c = cpu();
    CHECK((c.af >> 8) == 0x5A, "the step executed LD A,$5A");
    CHECK(GO(astrodebug_step(D)) && stopped_at(0x8004, "step"), "step OUT");
    CHECK(GO(astrodebug_step(D)) && stopped_at(0x8007, "step"), "step LD (nn),A");
    {
        uint8_t v;
        astrodebug_read(D, 0x4100, &v, 1);
        CHECK(v == 0x5A, "LD ($4100),A reached screen RAM");
    }
    CHECK(GO(astrodebug_step(D)) && stopped_at(0x8020, "step"), "step into the CALL");
    CHECK(cpu().sp == 0xFEFE, "CALL pushed the return address");
    CHECK(GO(astrodebug_step_out(D)) && stopped_at(0x800A, "step out"),
          "step out returns to $800A");

    restart_prog();
    astrodebug_cpu_set(D, ASTRO_REG_PC, 0x8007);
    CHECK(GO(astrodebug_step_over(D)) && stopped_at(0x800A, "step over"),
          "step over a CALL lands after it");
    CHECK(cpu().sp == 0xFF00, "the call returned (SP balanced)");
    CHECK(GO(astrodebug_step_over(D)) && stopped_at(0x800D, "step"),
          "step over a plain instruction is a step");
    astrodebug_cpu_set(D, ASTRO_REG_PC, 0x8011);
    astrodebug_cpu_set(D, ASTRO_REG_B, 3);
    CHECK(GO(astrodebug_step_over(D)) && stopped_at(0x8013, "step over"),
          "step over DJNZ runs the loop out");
    CHECK(cpu().sp == 0xFF00, "SP intact");

    /* ---- run to ---------------------------------------------------------------------- */
    restart_prog();
    CHECK(GO(astrodebug_run_to(D, 0x800D)) && stopped_at(0x800D, "run to"), "run to $800D");
    CHECK(stop_cb_kind == ASTRODBG_STOP_RUNTO, "stop callback kind run-to");

    /* ---- execute breakpoints and hit counts ------------------------------------------- */
    restart_prog();
    astrodebug_breakpoint_clear(D);
    CHECK(astrodebug_breakpoint_toggle(D, 0x8011) == 1, "toggle sets");
    CHECK(astrodebug_breakpoint_check(D, 0x8011) == 1, "check sees it");
    CHECK(GO(astrodebug_resume(D)) && stopped_at(0x8011, "breakpoint: exec $8011"),
          "exec breakpoint, reason string");
    CHECK(stop_cb_kind == ASTRODBG_STOP_BREAKPOINT, "stop callback kind breakpoint");
    CHECK(cpu().sp == 0xFF00 && (cpu().bc >> 8) == 3, "stopped BEFORE DJNZ ran (B=3)");
    CHECK(GO(astrodebug_resume(D)) && stopped_at(0x8011, "breakpoint: exec $8011") &&
          (cpu().bc >> 8) == 2, "second hit (B=2)");
    CHECK(GO(astrodebug_resume(D)) && stopped_at(0x8011, "breakpoint: exec $8011") &&
          (cpu().bc >> 8) == 1, "third hit (B=1)");
    {
        astrodebug_breakpoint bp[8];
        int n = astrodebug_breakpoint_list(D, bp, 8);
        CHECK(n == 1 && bp[0].hits == 3 && bp[0].type == ASTRODEBUG_BP_EXEC &&
              bp[0].enabled, "hit count 3 in the list");
        astrodebug_breakpoint_enable(D, bp[0].id, 0);
    }
    CHECK(astrodebug_breakpoint_check(D, 0x8011) == 1, "a disabled breakpoint still checks");
    {
        astrodebug_line ln[2];
        astrodebug_disassemble(D, 0x8011, ln, 1, NULL);
        CHECK(ln[0].has_breakpoint && !ln[0].breakpoint_enabled, "line shows a disabled breakpoint");
    }
    CHECK(GO(astrodebug_run_to(D, 0x8013)) && stopped_at(0x8013, "run to"),
          "a disabled breakpoint does not stop");
    CHECK(astrodebug_breakpoint_toggle(D, 0x8011) == 0, "toggle removes");

    /* ---- conditions ---------------------------------------------------------------------- */
    {
        int id;
        astrodebug_breakpoint bp[8];
        astrodebug_breakpoint_clear(D);
        restart_prog();
        id = astrodebug_breakpoint_add(D, ASTRODEBUG_BP_EXEC, 0x8011, 0x8011,
                                       "b == 1", err, sizeof err);
        CHECK(id > 0, "conditional breakpoint added");
        CHECK(GO(astrodebug_resume(D)) && stopped_at(0x8011, "breakpoint: exec $8011") &&
              (cpu().bc >> 8) == 1, "condition true only on the last pass");
        astrodebug_breakpoint_list(D, bp, 8);
        CHECK(bp[0].hits == 1, "false passes are not hits");
        astrodebug_breakpoint_clear(D);

        id = astrodebug_breakpoint_add(D, ASTRODEBUG_BP_EXEC, 0x8011, 0x8011,
                                       "b == 99", err, sizeof err);
        CHECK(id > 0, "false-condition breakpoint added");
        restart_prog();
        CHECK(GO(astrodebug_run_to(D, 0x8013)) && stopped_at(0x8013, "run to"),
              "a false condition never stops");
        astrodebug_breakpoint_clear(D);

        id = astrodebug_breakpoint_add(D, ASTRODEBUG_BP_EXEC, 0x8000, 0x8000,
                                       "1 +", err, sizeof err);
        CHECK(id < 0 && err[0], "an unparsable condition is rejected with a reason");
        id = astrodebug_breakpoint_add(D, ASTRODEBUG_BP_EXEC, 0x8000, 0x8000,
                                       "nosuchname == 1", err, sizeof err);
        CHECK(id < 0, "an unknown name is rejected");
        id = astrodebug_breakpoint_add(D, ASTRODEBUG_BP_OUT, 0x100, 0x100, NULL,
                                       err, sizeof err);
        CHECK(id < 0 && has(err, "255"), "ports above 255 rejected");

        /* a condition that fails at run time stops rather than hides */
        id = astrodebug_breakpoint_add(D, ASTRODEBUG_BP_EXEC, 0x8011, 0x8011,
                                       "1 / (a - a)", err, sizeof err);
        CHECK(id > 0, "division by zero parses");
        restart_prog();
        CHECK(GO(astrodebug_resume(D)) && stopped_at(0x8011, "breakpoint: exec $8011"),
              "a condition that cannot be evaluated stops");
        astrodebug_breakpoint_clear(D);
    }

    /* ---- watchpoints ------------------------------------------------------------------------ */
    astrodebug_breakpoint_clear(D);
    CHECK(astro_host_machine()->bus_hook == NULL, "bus hook removed with the last watchpoint");
    CHECK(astrodebug_breakpoint_add(D, ASTRODEBUG_BP_WRITE, 0x4100, 0x4100, NULL, err, sizeof err) > 0,
          "write watch on screen RAM");
    CHECK(astro_host_machine()->bus_hook != NULL, "bus hook installed for a watchpoint");
    restart_prog();
    CHECK(GO(astrodebug_resume(D)) && stopped_at(0x8007, "write $4100 = $5A"),
          "write watch stops AFTER the instruction");
    CHECK(stop_cb_kind == ASTRODBG_STOP_WATCH, "stop callback kind watch");
    astrodebug_breakpoint_clear(D);

    CHECK(astrodebug_breakpoint_add(D, ASTRODEBUG_BP_OUT, 0x10, 0x10, NULL, err, sizeof err) > 0,
          "OUT watch on a data-chip/sound port");
    restart_prog();
    CHECK(GO(astrodebug_resume(D)) && stopped_at(0x8004, "out $10 <- $5A"),
          "OUT watch: port, direction, value");
    astrodebug_breakpoint_clear(D);

    CHECK(astrodebug_breakpoint_add(D, ASTRODEBUG_BP_OUT, 0x08, 0x0F, "value >= 0", err, sizeof err) > 0,
          "OUT watch over a port range with a condition");
    astrodebug_breakpoint_clear(D);

    CHECK(astrodebug_breakpoint_add(D, ASTRODEBUG_BP_IN, 0x10, 0x13, NULL, err, sizeof err) > 0, "IN watch");
    restart_prog();
    CHECK(GO(astrodebug_resume(D)) && stopped_at(0x800F, "in $10 -> $"), "IN watch stops after IN");
    astrodebug_breakpoint_clear(D);

    /* READ: a data read stops; the instruction's own fetches do not */
    CHECK(astrodebug_breakpoint_add(D, ASTRODEBUG_BP_READ, 0x8000, 0x801F, NULL, err, sizeof err) > 0,
          "read watch over the code itself");
    restart_prog();
    CHECK(GO(astrodebug_run_to(D, 0x8013)) && stopped_at(0x8013, "run to"),
          "opcode and operand fetches are not READ hits");
    astrodebug_breakpoint_clear(D);
    CHECK(astrodebug_breakpoint_add(D, ASTRODEBUG_BP_READ, 0x4100, 0x4100, NULL, err, sizeof err) > 0,
          "read watch on $4100");
    restart_prog();
    CHECK(GO(astrodebug_resume(D)) && stopped_at(0x800D, "read $4100 = $5A"),
          "a data read stops after LD A,(nn)");
    astrodebug_breakpoint_clear(D);

    /* a conditional write watch: value decides */
    CHECK(astrodebug_breakpoint_add(D, ASTRODEBUG_BP_WRITE, 0x4000, 0x4FFF, "value == $99", err, sizeof err) > 0,
          "conditional write watch");
    restart_prog();
    CHECK(GO(astrodebug_run_to(D, 0x8013)) && stopped_at(0x8013, "run to"),
          "a watch whose condition is false does not stop");
    astrodebug_breakpoint_clear(D);
    CHECK(astrodebug_breakpoint_add(D, ASTRODEBUG_BP_WRITE, 0x4000, 0x4FFF, "value == $5A && addr == $4100", err, sizeof err) > 0,
          "value and addr in conditions");
    restart_prog();
    CHECK(GO(astrodebug_resume(D)) && stopped_at(0x8007, "write $4100 = $5A"), "... and it hits");
    astrodebug_breakpoint_clear(D);

    /* `old`: the byte before a write (the hook runs after the store) */
    astrodebug_write(D, 0x4100, 0x00);
    CHECK(astrodebug_breakpoint_add(D, ASTRODEBUG_BP_WRITE, 0x4100, 0x4100, "value != old", err, sizeof err) > 0,
          "change-only write watch");
    restart_prog();
    CHECK(GO(astrodebug_resume(D)) && stopped_at(0x8007, "write $4100 = $5A"),
          "value != old stops on a change");
    astrodebug_breakpoint_clear(D);
    astrodebug_write(D, 0x4100, 0x5A);
    CHECK(astrodebug_breakpoint_add(D, ASTRODEBUG_BP_WRITE, 0x4100, 0x4100, "value != old", err, sizeof err) > 0,
          "change-only write watch, again");
    restart_prog();
    CHECK(GO(astrodebug_run_to(D, 0x8013)) && stopped_at(0x8013, "run to"),
          "writing the same value is not a change");
    astrodebug_breakpoint_clear(D);
    astrodebug_write(D, 0x4100, 0x77);
    CHECK(astrodebug_breakpoint_add(D, ASTRODEBUG_BP_WRITE, 0x4100, 0x4100, "old == $77", err, sizeof err) > 0,
          "old is seeded from memory");
    restart_prog();
    CHECK(GO(astrodebug_resume(D)) && stopped_at(0x8007, "write $4100 = $5A"), "old == $77 hits");
    astrodebug_breakpoint_clear(D);

    /* the stop serial moves on every stop */
    {
        uint64_t s0 = astrodebug_stop_serial(D);
        restart_prog();
        CHECK(GO(astrodebug_step(D)) && astrodebug_stop_serial(D) > s0, "stop serial bumps on a stop");
    }

    /* ---- memory search (stopped) -------------------------------------------------------------- */
    {
        static const uint8_t needle[] = { 0xC0, 0xDE, 0x12 };
        const uint8_t pat[] = { 0xC0, 0xDE };
        const uint8_t txt[] = { 'h', 'i' };
        uint16_t hits[8], at = 0;
        int r;
        for (int i = 0; i < 3; i++) {
            astrodebug_write(D, (uint16_t)(0x9000 + i), needle[i]);
            astrodebug_write(D, (uint16_t)(0xA000 + i), needle[i]);
        }
        astrodebug_write(D, 0xB000, 'H'); astrodebug_write(D, 0xB001, 'I');
        r = astrodebug_search(D, pat, 2, 0xFF, 0, 0x8800, 0xBFFF, hits, 8);
        CHECK(r == 2 && hits[0] == 0x9000 && hits[1] == 0xA000, "search finds both");
        r = astrodebug_search(D, pat, 2, 0xF0, 0, 0x9000, 0x9001, hits, 8);
        CHECK(r == 1, "search with a mask");
        r = astrodebug_search(D, txt, 2, 0xFF, 0, 0xB000, 0xB001, hits, 8);
        CHECK(r == 0, "case matters by default");
        r = astrodebug_search(D, txt, 2, 0xFF, ASTRODEBUG_SEARCH_NOCASE, 0xB000, 0xB001, hits, 8);
        CHECK(r == 1 && hits[0] == 0xB000, "NOCASE folds");
        CHECK(astrodebug_search_next(D, pat, 2, 0xFF, 0, 0x9000, 0, &at) == 1 && at == 0xA000,
              "search next");
        CHECK(astrodebug_search_next(D, pat, 2, 0xFF, 0, 0xA000, 0, &at) == 1 && at == 0x9000,
              "search next wraps");
        CHECK(astrodebug_search_next(D, pat, 2, 0xFF, 0, 0xA000, 1, &at) == 1 && at == 0x9000,
              "search previous");
        CHECK(astrodebug_search_next(D, pat, 2, 0xFF, 0, 0x9000, 1, &at) == 1 && at == 0xA000,
              "search previous wraps");
        CHECK(astrodebug_search(D, pat, 2, 0xFF, 0, 0x9000, 0x9000, hits, 8) == 1,
              "a match may run past `to`");
    }

    /* ---- range parsing ---------------------------------------------------------------------------- */
    {
        uint16_t lo = 0, hi = 0;
        CHECK(astrosym_parse_range(D, "C000", &lo, &hi) && lo == 0xC000 && hi == 0xC000, "bare hex");
        CHECK(astrosym_parse_range(D, "$4000-$40FF", &lo, &hi) && lo == 0x4000 && hi == 0x40FF, "lo-hi");
        CHECK(astrosym_parse_range(D, "8203..8200", &lo, &hi) && lo == 0x8200 && hi == 0x8203, "reversed, dots");
        CHECK(astrosym_parse_range(D, "$4000+3", &lo, &hi) && lo == 0x4003, "an expression");
        CHECK(!astrosym_parse_range(D, "nosuchlabel", &lo, &hi), "unknown name");
    }

    /* ---- cart info and the memory map ---------------------------------------------------------- */
    {
        astrodebug_cart ci;
        astrodebug_region rg[16];
        int n, i;
        astrodebug_cart_info(D, &ci);
        CHECK(ci.present && ci.kind[0] && strcmp(ci.kind, "none") != 0, "cart info");
        n = astrodebug_memory_map(D, rg, 16);
        CHECK(n >= 5 && rg[0].lo == 0 && rg[n - 1].hi == 0xFFFF, "memory map covers the space");
        for (i = 1; i < n; i++)
            if (rg[i].lo != (uint16_t)(rg[i - 1].hi + 1)) break;
        CHECK(i == n, "memory map is ordered and gap-free");
        CHECK(rg[2].lo == 0x2000 && has(rg[2].name, "Cartridge") && !rg[2].writable, "cart region");
    }

    /* ---- history ring -------------------------------------------------------------------------- */
    {
        static astrodebug_trace tr[128];
        char line[128];
        int n;
        CHECK(!astrodebug_trace_enabled(D) && astrodebug_trace_count(D) == 0, "recording is off by default");
        astrodebug_trace_enable(D, 1);
        restart_prog();
        CHECK(GO(astrodebug_run_to(D, 0x8013)) && stopped_at(0x8013, "run to"), "ran with tracing on");
        n = astrodebug_trace_read(D, tr, 128);
        CHECK(n >= 8 && astrodebug_trace_count(D) >= 8, "instructions recorded");
        CHECK(tr[0].pc == 0x8011 || tr[0].pc == 0x8013 || tr[0].pc == 0x8022, "newest entry first");
        astrodebug_trace_format(&tr[n - 1], line, sizeof line);
        CHECK(has(line, "8002") && has(line, "OUT ($10),A"), "format shows pc and mnemonic");
        {
            int br = 0;
            for (int i = 0; i < n; i++) br += astrodebug_trace_match(&tr[i], ASTRODEBUG_TRACE_BRANCHES, 0, 0);
            CHECK(br >= 2, "branch filter finds the CALL and RET");
        }
        astrodebug_trace_enable(D, 0);
        astrodebug_trace_clear(D);
        CHECK(astrodebug_trace_count(D) == 0, "clear");
    }

    /* ---- beam: scanline / frame ----------------------------------------------------------------- */
    restart_prog();
    astrodebug_cpu_set(D, ASTRO_REG_PC, 0x8013);
    c = cpu();
    CHECK(GO(astrodebug_scanline(D, 3)), "scanline stops");
    why();
    CHECK(strcmp(reason, "scanline") == 0, "reason \"scanline\"");
    c2 = cpu();
    CHECK(c2.line == (c.line + 3) % 262, "three lines later (wrapping at 261)");
    CHECK(c2.cycles > c.cycles, "cycles advanced");
    c = cpu();
    CHECK(GO(astrodebug_frame(D, 1)), "frame stops");
    why();
    CHECK(strcmp(reason, "frame") == 0, "reason \"frame\"");
    c2 = cpu();
    CHECK(c2.frame == c.frame + 1 && c2.line == 0, "one frame, on the boundary (line 0)");
    CHECK(GO(astrodebug_frame(D, 2)), "two frames");
    CHECK(cpu().frame == c2.frame + 2, "frame count +2");

    /* ---- disassembly ---------------------------------------------------------------------------------- */
    restart_prog();
    astrodebug_set_label(D, 0x8020, "subr");
    {
        astrodebug_line ln[12];
        int pcl = -9;
        int n = astrodebug_disassemble(D, 0x8000, ln, 12, &pcl);
        CHECK(n == 12, "12 lines");
        CHECK(pcl == 0 && ln[0].is_pc && !ln[1].is_pc, "is_pc and pc_line");
        CHECK(strcmp(ln[0].disasm, "LD A,$5A") == 0 && strcmp(ln[0].bytes, "3E 5A") == 0 &&
              ln[0].length == 2, "LD A,n line");
        CHECK(has(ln[1].disasm, "OUT") && has(ln[1].comment, "TONMO"), "OUT line: port name comment");
        CHECK(ln[3].flags & ASTRODASM_CALL && ln[3].target == 0x8020 &&
              strcmp(ln[3].comment, "subr") == 0, "CALL line: flag, target, label comment");
        CHECK(has(ln[5].disasm, "IN") && has(ln[5].comment, "SW0"), "IN line: port name");
        CHECK((ln[7].flags & (ASTRODASM_JUMP | ASTRODASM_RELATIVE | ASTRODASM_COND)) ==
              (ASTRODASM_JUMP | ASTRODASM_RELATIVE | ASTRODASM_COND), "DJNZ flags");
        n = astrodebug_disassemble(D, 0x8020, ln, 3, &pcl);
        CHECK(pcl == -1, "pc_line -1 when the PC is not among the lines");
        CHECK(strcmp(ln[0].label, "subr") == 0, "label at the line's address");
        CHECK(ln[1].flags & ASTRODASM_RET, "RET flag");
        CHECK(astrodebug_address_label(D, 0x8020, out, sizeof out) == 4 && !strcmp(out, "subr"), "address_label");
        CHECK(astrodebug_label_address(D, "subr") == 0x8020 &&
              astrodebug_label_address(D, "nosuch") == -1, "label_address");
        astrodebug_breakpoint_toggle(D, 0x8004);
        astrodebug_disassemble(D, 0x8004, ln, 1, NULL);
        CHECK(ln[0].has_breakpoint && ln[0].breakpoint_enabled, "breakpoint shown on its line");
        astrodebug_breakpoint_clear(D);
    }
    CHECK(astrodebug_row_address(D, 0x8007, 2) == 0x800D, "row_address forward");
    CHECK(astrodebug_row_address(D, 0x800D, -2) == 0x8007, "row_address backward");
    CHECK(astrodebug_row_address(D, 0x8007, 0) == 0x8007, "row_address zero");

    /* ---- the console ----------------------------------------------------------------------------------- */
    astrodebug_command(D, "help", out, sizeof out);
    CHECK(has(out, "step") && has(out, "bpin") && has(out, "scanline"), "help lists commands");
    astrodebug_command(D, "break $8011 b == 2", out, sizeof out);
    CHECK(has(out, "breakpoint #"), "break command");
    astrodebug_command(D, "bpw $4000-$40FF", out, sizeof out);
    CHECK(has(out, "breakpoint #"), "bpw command");
    astrodebug_command(D, "bpout $10", out, sizeof out);
    astrodebug_command(D, "breaks", out, sizeof out);
    CHECK(has(out, "$8011") && has(out, "if b == 2") && has(out, "$4000-$40FF") && has(out, "hits"),
          "breaks lists them");
    astrodebug_command(D, "clear", out, sizeof out);
    astrodebug_command(D, "breaks", out, sizeof out);
    CHECK(has(out, "no breakpoints"), "clear");
    astrodebug_command(D, "print 1+2*3", out, sizeof out);
    CHECK(has(out, "$0007") && has(out, "7"), "print");
    astrodebug_command(D, "print [$8000]", out, sizeof out);
    CHECK(has(out, "$003E"), "print [addr]");
    astrodebug_command(D, "print {$8000}", out, sizeof out);
    CHECK(has(out, "$5A3E"), "print {addr}");
    astrodebug_command(D, "print nosuch", out, sizeof out);
    CHECK(has(out, "error"), "print error");
    astrodebug_command(D, "regs", out, sizeof out);
    CHECK(has(out, "PC=8000") && has(out, "SP=FF00"), "regs");
    astrodebug_command(D, "set hl $BEEF", out, sizeof out);
    CHECK(has(out, "HL=BEEF"), "set");
    astrodebug_command(D, "mem $8000 4", out, sizeof out);
    CHECK(has(out, "8000") && has(out, "3E 5A D3 10"), "mem");
    astrodebug_command(D, "poke $9000 1 2 $FF", out, sizeof out);
    CHECK(has(out, "3 bytes"), "poke");
    astrodebug_command(D, "mem $9000 3", out, sizeof out);
    CHECK(has(out, "01 02 FF"), "poke took");
    astrodebug_command(D, "vpoke $10 $A5", out, sizeof out);
    astrodebug_command(D, "vram $10 1", out, sizeof out);
    CHECK(has(out, "A5"), "vpoke/vram");
    astrodebug_command(D, "disasm $8000 3", out, sizeof out);
    CHECK(has(out, ">") && has(out, "LD A,$5A") && has(out, "OUT"), "disasm");
    astrodebug_command(D, "label $8013 spin", out, sizeof out);
    astrodebug_command(D, "disasm $8013 1", out, sizeof out);
    CHECK(has(out, "spin:"), "label command");
    astrodebug_command(D, "trace on", out, sizeof out);
    astrodebug_command(D, "step 3", out, sizeof out);
    CHECK(has(out, "step") && has(out, "$8007"), "console step 3 says where it is");
    astrodebug_command(D, "trace 8", out, sizeof out);
    CHECK(has(out, "8002") && has(out, "8004") && has(out, "8007") && has(out, "OUT"), "trace lists instructions");
    astrodebug_command(D, "trace clear", out, sizeof out);
    astrodebug_command(D, "trace off", out, sizeof out);
    {
        astrodebug_trace tr[8];
        CHECK(astrodebug_trace_read(D, tr, 8) == 0 && !astrodebug_trace_enabled(D), "trace clear/off");
    }
    astrodebug_command(D, "over", out, sizeof out);
    CHECK(has(out, "step over") && has(out, "$800A"), "console over");
    astrodebug_command(D, "runto $8013", out, sizeof out);
    CHECK(has(out, "run to") && has(out, "$8013"), "console runto");
    astrodebug_command(D, "frame", out, sizeof out);
    CHECK(has(out, "frame"), "console frame");
    astrodebug_command(D, "scanline 2", out, sizeof out);
    CHECK(has(out, "scanline"), "console scanline");
    astrodebug_command(D, "io", out, sizeof out);
    CHECK(has(out, "$4000-$4FFF"), "console io");
    astrodebug_command(D, "video", out, sizeof out);
    CHECK(has(out, "MAGIC"), "console video");
    astrodebug_command(D, "fujinet", out, sizeof out);
    CHECK(out[0], "console fujinet");
    astrodebug_command(D, "bogus", out, sizeof out);
    CHECK(has(out, "unknown command"), "unknown command");
    astrodebug_command(D, "stop", out, sizeof out);
    CHECK(has(out, "$"), "stop while stopped says where");
    {
        char comp[1024];
        int n = astrodebug_completions(D, "sc", comp, sizeof comp);
        CHECK(n == 1 && has(comp, "scanline"), "command completion");
        n = astrodebug_completions(D, "save ", comp, sizeof comp);
        CHECK(n == 4, "save kind completion");
        n = astrodebug_completions(D, "print h", comp, sizeof comp);
        CHECK(has(comp, "hl"), "register completion");
        long v;
        CHECK(astrodebug_eval(D, "subr + 2", &v, err, sizeof err) == 0 && v == 0x8022, "eval with a label");
        CHECK(astrodebug_eval(D, "[", &v, err, sizeof err) == -1 && err[0], "eval error");
    }

    /* ---- symbols ----------------------------------------------------------------------------------------- */
    snprintf(path, sizeof path, "%s/t.sym", tmpl);
    f = fopen(path, "w");
    fputs("8011 LOOPTOP ; the djnz\n# comment\n8020 SUBX\n", f);
    fclose(f);
    {
        char msg[300];
        uint16_t addr = 0, off = 0;
        CHECK(astrodebug_load_symbols(D, path, msg, sizeof msg) == 2 && has(msg, "2 labels"), "load a .sym");
        CHECK(astrodebug_label_address(D, "LOOPTOP") == 0x8011, "symbol loaded");
        CHECK(astrodebug_symbol_find(D, "SUBX", &addr) == 1 && addr == 0x8020, "symbol_find");
        CHECK(astrodebug_symbol_at(D, 0x8012, &off) != NULL && off == 1, "symbol_at with offset");
        CHECK(astrodebug_load_symbols(D, NULL, msg, sizeof msg) == -1 && msg[0],
              "no cartridge file: a message, not a crash");
        CHECK(astrodebug_label_address(D, "NORMEM") == 0x4000, "built-in table");
        astrodebug_command(D, "break LOOPTOP", out, sizeof out);
        CHECK(has(out, "breakpoint #"), "labels work as addresses");
        astrodebug_command(D, "clear", out, sizeof out);
        CHECK(astrodebug_symbols_load(D, "/nonexistent/x.sym", "x") < 0, "unreadable symbol file");
    }

    /* ---- save ------------------------------------------------------------------------------------------------ */
    {
        char msg[300];
        snprintf(path, sizeof path, "%s/mem.bin", tmpl);
        CHECK(astrodebug_save(D, "mem", path, msg, sizeof msg) == 0 && read_file_size(path) == 65536, "save mem");
        snprintf(path, sizeof path, "%s/vram.bin", tmpl);
        CHECK(astrodebug_save(D, "vram", path, msg, sizeof msg) == 0 && read_file_size(path) == 4096, "save vram");
        snprintf(path, sizeof path, "%s/regs.txt", tmpl);
        CHECK(astrodebug_save(D, "regs", path, msg, sizeof msg) == 0 && read_file_size(path) > 100, "save regs");
        snprintf(path, sizeof path, "%s/dis.txt", tmpl);
        CHECK(astrodebug_save(D, "dis", path, msg, sizeof msg) == 0 && read_file_size(path) > 10000, "save dis");
        CHECK(astrodebug_save(D, "bogus", path, msg, sizeof msg) == -1 && has(msg, "Unknown"), "unknown kind");
        CHECK(astrodebug_save(D, "mem", "/nonexistent/dir/x", msg, sizeof msg) == -1, "unwritable path");
        astrodebug_command(D, "save regs /dev/null", out, sizeof out);
        CHECK(has(out, "Saved"), "console save");
    }

    /* ---- memory map and I/O -------------------------------------------------------------------------------------- */
    {
        astrodebug_memmap mm;
        astrodebug_memmap_get(D, &mm);
        CHECK(mm.exp_bytes == 0xB000 && mm.exp_base == 0x5000 && mm.cart_present &&
              mm.bios_name && mm.exp_name && has(mm.exp_name, "RL64"), "memmap");
        CHECK(astrodebug_io_state(D, out, sizeof out) > 200 && has(out, "$0000-$0FFF") &&
              has(out, "$2000-$3FFF") && has(out, "$4000-$4FFF") && has(out, "RL64"), "io_state");
    }

    /* ---- FujiNet (no listener, nothing traced) ------------------------------------------------------------------------- */
    {
        static astrodebug_fuji_packet pk[8];
        char state[1024];
        CHECK(astrodebug_fujinet_trace(D, pk, 8) == 0, "no FujiBus packets without a link");
        astrodebug_fujinet_state(D, state, sizeof state);
        CHECK(has(state, "BoIP link     not connected"), "state shows the link down");
        astrodebug_fujinet_trace_clear(D);
    }

    /* the video snapshot the debugger's Video tab uses */
    {
        astrovid_snapshot *snap = malloc(sizeof *snap);
        astrovid_snapshot_get(snap);
        CHECK(snap->interrupt_scanline != 0 || snap->vblank != 0, "video chip registers exposed");
        free(snap);
    }

    /* ---- the Video tab's controller against the live machine ------------------------------------------- */
    {
        astrovid_tab *vt = astrovid_tab_new();
        const uint32_t *pic;
        int w, h, same = 1;
        astrovid_tab_set_zoom(vt, 1);
        astrovid_tab_set_overlays(vt, 0);
        /* let a few frames finish with the tab open (it asks the machine to capture them) */
        astrodebug_resume(D);
        sleep_ms(300);
        astrodebug_stop(D);
        for (int i = 0; i < 100 && !astrodebug_is_stopped(D); i++) sleep_ms(20);
        restart_prog();
        CHECK(GO(astrodebug_frame(D, 1)), "stopped at a frame boundary");
        astrovid_tab_set_view(vt, ASTROVID_V_DISPLAY);
        CHECK(astrovid_tab_refresh(vt, 1) & ASTROVID_TAB_PICTURE, "tab refresh");
        pic = astrovid_tab_picture(vt, &w, &h);
        CHECK(pic && w == 352 && h == 240, "Display is the full frame");
        if (pic) {
            const astro_machine_t *m = astro_host_machine();
            for (int i = 0; i < 352 * 240; i++)
                if ((pic[i] & 0xffffff) != (m->fb[i] & 0xffffff)) { same = 0; break; }
        }
        CHECK(same, "Display equals the machine's framebuffer at a frame boundary");
        /* Decoded must agree with Display inside the data area when the registers hold still */
        astrovid_tab_set_view(vt, ASTROVID_V_DECODED);
        astrovid_tab_refresh(vt, 1);
        {
            const uint32_t *dec = astrovid_tab_picture(vt, &w, &h);
            const astro_machine_t *m = astro_host_machine();
            int bad = 0, checked = 0, step = 2 - (m->video_mode & 1);
            for (int y = 0; dec && y < h; y += 7)
                for (int x = 0; x < w; x += 5) {
                    const int sx = 16 + x * step, sy = 22 + y * step;
                    if (sy < 240 && sx < 352 && y * step < m->vblank) {
                        checked++;
                        if ((dec[(size_t)y * w + x] & 0xffffff) != (m->fb[(size_t)sy * 352 + sx] & 0xffffff))
                            bad++;
                    }
                }
            CHECK(checked > 50 && bad == 0, "Decoded agrees with the machine's own picture");
        }
        astrodebug_breakpoint_clear(D);
        astrovid_tab_hover(vt, 5, 5);
        CHECK(astrovid_tab_break_on_pixel(vt) >= 0, "break on a pixel's writes adds breakpoints");
        {
            astrodebug_breakpoint bl[8];
            CHECK(astrodebug_breakpoint_list(D, bl, 8) == 2 && (bl[0].type & ASTRODEBUG_BP_WRITE) &&
                  bl[0].start >= 0x4000 && bl[1].start == bl[0].start - 0x4000, "...at $4000+off and its mirror");
        }
        astrodebug_breakpoint_clear(D);
        astrovid_tab_free(vt);
    }

    /* ---- detach resumes ----------------------------------------------------------------------------------------------------- */
    astrodebug_breakpoint_add(D, ASTRODEBUG_BP_WRITE, 0x4000, 0x4000, NULL, err, sizeof err);
    {
        uint64_t f0 = astro_host_frame_count();
        astrodebug_detach(D);
        CHECK(!astrodebug_is_attached(D) && !astrodebug_is_stopped(D), "detach: running");
        CHECK(astro_host_machine()->instr_hook == NULL && astro_host_machine()->bus_hook == NULL,
              "detach takes every hook out of the core");
        sleep_ms(200);
        CHECK(astro_host_frame_count() > f0 + 3, "the machine runs on after detach");
        why();
        CHECK(strcmp(reason, "running") == 0 && reason_pc == -1, "reason while running");
    }
    astrodebug_breakpoint_clear(D);

    /* ---- stop() while running, then resume -------------------------------------------------------------------------------------- */
    astrodebug_stop(D);                 /* detached: attaches and stops */
    CHECK(wait_stopped(1000), "stop() when detached attaches and stops");
    astrodebug_resume(D);
    sleep_ms(50);
    CHECK(!astrodebug_is_stopped(D) && astrodebug_is_attached(D), "resume: running, still attached");
    astrodebug_stop(D);
    CHECK(wait_stopped(1000), "stop while running");

    /* ---- REGRESSION 1: restart while parked must not hang ------------------------------------------------------------------------- */
    CHECK(astrodebug_is_stopped(D), "parked");
    {
        uint16_t pcs[8];
        int npc = 0;
        alarm(15);
        astro_host_stop();              /* used to pthread_join a parked thread forever */
        alarm(120);
        CHECK(!astro_host_is_running(), "host stopped while parked");
        CHECK(astrodebug_is_attached(D), "the engine stays attached across the stop");
        CHECK(!astrodebug_is_stopped(D), "nothing is parked in a dead machine");
        astrodebug_cpu_get(D, &c);
        CHECK(c.pc == 0 && c.cycles == 0, "no machine: cpu is zeros");
        CHECK(astrodebug_read(D, 0, (uint8_t[2]){0}, 2) == 0, "no machine: read is 0");
        astrodebug_command(D, "regs", out, sizeof out);       /* harmless */
        astrodebug_command(D, "step", out, sizeof out);

        /* a breakpoint set while there is no machine survives */
        CHECK(astrodebug_breakpoint_add(D, ASTRODEBUG_BP_EXEC, 0x0100, 0x0100, NULL, err, sizeof err) > 0,
              "breakpoint with no machine");
        astrodebug_breakpoint_clear(D);

        /* ---- REGRESSION 2: an attached engine stops the new machine at instruction one */
        alarm(15);
        CHECK(astro_host_start(&opts) == 0, "host restart");
        CHECK(wait_stopped(2000), "restart: the attached engine stops the new machine");
        alarm(120);
        astrodebug_cpu_get(D, &c);
        CHECK(c.pc == 0 && c.cycles < 100, "... at its first instruction");
        CHECK(astro_host_machine()->instr_hook != NULL, "hooks re-installed after astro_machine_init");
        CHECK(strcmp(why(), "stopped") == 0, "reason \"stopped\"");
        /* learn the first instructions' addresses */
        pcs[npc++] = (uint16_t)cpu().pc;
        for (int i = 0; i < 6; i++) {
            CHECK(GO(astrodebug_step(D)), "step after restart");
            pcs[npc++] = (uint16_t)cpu().pc;
        }
        /* restart again: a breakpoint (set now) fires in the third machine */
        astro_host_stop();
        astrodebug_breakpoint_add(D, ASTRODEBUG_BP_EXEC, pcs[4], pcs[4], NULL, err, sizeof err);
        alarm(15);
        CHECK(astro_host_start(&opts) == 0, "second restart");
        CHECK(wait_stopped(2000) && cpu().pc == 0, "stopped at instruction one again");
        alarm(120);
        CHECK(GO(astrodebug_resume(D)) && astrodebug_is_stopped(D), "resume runs to the breakpoint");
        snprintf(out, sizeof out, "breakpoint: exec $%04X", pcs[4]);
        CHECK(stopped_at(pcs[4], out), "the breakpoint fires after a restart");

        /* a watchpoint (bus hook) also survives a restart */
        astrodebug_breakpoint_clear(D);
        astrodebug_breakpoint_add(D, ASTRODEBUG_BP_WRITE, 0x4000, 0x4FFF, NULL, err, sizeof err);
        astro_host_stop();
        alarm(15);
        CHECK(astro_host_start(&opts) == 0, "third restart");
        CHECK(wait_stopped(2000), "stopped again");
        alarm(120);
        CHECK(astro_host_machine()->bus_hook != NULL, "bus hook re-installed");
        CHECK(GO(astrodebug_resume(D)), "BIOS writes screen RAM");
        CHECK(has(why(), "write $4"), "write watch fires after a restart");
    }

    /* a restart while RUNNING (attached, resumed) is fine too */
    astrodebug_breakpoint_clear(D);
    astrodebug_resume(D);
    sleep_ms(30);
    alarm(15);
    astro_host_stop();
    CHECK(astro_host_start(&opts) == 0, "restart while running");
    alarm(120);
    CHECK(wait_stopped(2000), "... still stops at instruction one");

    /* detach with the machine stopped; then stop the host; then attach with no
     * machine: it stops the machine as soon as it exists */
    astrodebug_detach(D);
    CHECK(!astrodebug_is_stopped(D), "detach released the parked thread");
    alarm(15);
    astro_host_stop();
    astrodebug_attach(D);
    CHECK(astrodebug_is_attached(D) && !astrodebug_is_stopped(D), "attach before start: pending");
    CHECK(astro_host_start(&opts) == 0, "start");
    CHECK(wait_stopped(2000) && cpu().pc == 0, "attach-before-start stops at the first instruction");
    alarm(120);

    astrodebug_detach(D);
    astro_host_stop();
    astrodebug_set_stop_callback(D, NULL, NULL);
    astrodebug_fujinet_state(D, out, sizeof out);
    CHECK(strcmp(out, "The Astrocade is off\n") == 0, "state once stopped");

    {
        char cmd[300];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpl);
        if (system(cmd) != 0) {}
    }
    fprintf(stderr, "debug_test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
