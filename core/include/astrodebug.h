/*
 * astrodebug -- the Z80 debugger engine.
 *
 * Built on the emulator core's own instruction-boundary hook (astro_host_
 * set_instr_hook / astro_machine.instr_hook), fired at z80_opdone with the PC
 * pointing at the next instruction. Engaged, the hook checks breakpoints and
 * the pause request; a pause blocks the emulator thread right there inside the
 * hook (pthread_cond_wait) until resumed -- the CPU's own progress is what
 * stops, there is no separate "paused" flag a run loop polls. Disengaged, the
 * hook is not installed at all, so a session that never opens the debugger
 * pays nothing.
 *
 * THREADING. Everything here is called from a UI thread except the internal
 * hook. While paused the emulator thread is parked in the hook, so the
 * machine is quiescent and reading its registers/memory from the UI thread is
 * safe; those calls are valid only while paused and return 0 otherwise.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ASTRODEBUG_H
#define ASTRODEBUG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct astrodebug astrodebug;

#define ASTRODEBUG_MAX_BREAKPOINTS 64

/* The Z80's programmer-visible state (chips z80.h field names). */
typedef struct {
    uint16_t af, bc, de, hl;      /* main register file */
    uint16_t af2, bc2, de2, hl2;  /* the shadow bank */
    uint16_t ix, iy, sp, pc, wz;
    uint8_t i, r, im;
    int iff1, iff2, halt;
} astrodebug_regs;

/* Engage or release the debugger (installs/removes the instruction hook).
 * Releasing resumes first if paused. */
void astrodebug_set_engaged(astrodebug *d, int engaged);
int  astrodebug_is_engaged(const astrodebug *d);

/* Pause at the next instruction boundary, or resume. */
void astrodebug_pause(astrodebug *d);
void astrodebug_resume(astrodebug *d);
int  astrodebug_is_paused(const astrodebug *d);

/* Run exactly one instruction, then pause. */
void astrodebug_step(astrodebug *d);
/* Step over a CALL/RST/DJNZ/block-repeat: run until the following instruction
 * (a one-shot breakpoint); otherwise == step. Only meaningful while paused. */
void astrodebug_step_over(astrodebug *d);
/* Run until control leaves the current subroutine -- breaks on the 16-bit
 * word currently at SP (the return address the CALL pushed), with an SP-depth
 * guard. Documented-approximate: a routine that manipulates its own return
 * address defeats it, and it then runs free rather than stopping wrong. Only
 * meaningful while paused. */
void astrodebug_step_out(astrodebug *d);

/* Bumped every time the machine stops; a UI polls this on its frame clock. */
uint64_t astrodebug_stop_serial(const astrodebug *d);

/* ---- inspection: valid only while paused (return 0 while running) ---- */
int astrodebug_regs_get(astrodebug *d, astrodebug_regs *out);
/* Non-destructive read (the peek path -- cart hotspots don't fire, IN 0x08
 * intercept isn't cleared). Returns bytes read. */
int astrodebug_read(astrodebug *d, uint16_t addr, uint8_t *dst, int n);
/* Real bus write (side effects included). Returns n. */
int astrodebug_write(astrodebug *d, uint16_t addr, const uint8_t *src, int n);

typedef struct {
    uint16_t addr;
    int      length;     /* instruction length in bytes (1-4) */
    char     text[64];
} astrodebug_dasm_line;

/* Disassembles up to `max` instructions from addr, walking by length.
 * Returns how many (0 while running). */
int astrodebug_disassemble(astrodebug *d, uint16_t addr,
                           astrodebug_dasm_line *out, int max);

/* ---- breakpoints (safe while running) ---- */
int  astrodebug_breakpoint_toggle(astrodebug *d, uint16_t addr);
int  astrodebug_breakpoint_is_set(astrodebug *d, uint16_t addr);
void astrodebug_breakpoint_clear_all(astrodebug *d);
int  astrodebug_breakpoint_list(astrodebug *d, uint16_t *out, int max);

/* Process-wide singleton (one machine, one debugger); never NULL. */
astrodebug *astrodebug_get(void);

#ifdef __cplusplus
}
#endif

#endif /* ASTRODEBUG_H */
