/*
 * debugger_internal -- the engine's side of the console (prompt.c) and of the
 * emulator host (astro/host.c). Not installed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ASTRO_DEBUGGER_INTERNAL_H
#define ASTRO_DEBUGGER_INTERNAL_H

#include "astrodebug.h"
#include "astrovid.h"
#include "symbols.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the console ---- */
/* Step n instructions (stopped only). */
void astrodebug_step_count(astrodebug *d, long n);
/* Wait up to ms for the machine to be stopped with a generation other than
 * gen (read before issuing the command). 1 if it stopped. */
int astrodebug_wait_stop(astrodebug *d, unsigned gen, int ms);
/* A command ran: bump the generation so windows refresh. */
void astrodebug_touch(astrodebug *d);
/* Register names the evaluator knows, for completion. */
extern const char *const astrodebug_reg_names[];
/* Label names starting with word (case-insensitive), one per line into dst,
 * at most maxn. Returns the count. */
int astrodebug_label_completions(astrodebug *d, const char *word, char *dst,
                                 int dstsz, int maxn);
/* Screen RAM (the video chip's 4K): offsets wrap at $1000. */
int astrodebug_vram_read(astrodebug *d, uint16_t addr, uint8_t *dst, int n);
int astrodebug_vram_write(astrodebug *d, uint16_t addr, const uint8_t *src, int n);
/* The video chip snapshot, under the engine lock. */
void astrodebug_video_snapshot(astrodebug *d, astrovid_snapshot *out);

/* The cartridge file the session opened (NULL or "" for none): where
 * astrodebug_load_symbols(NULL) looks for <cartridge>.sym/.map/.noi. */
void astrodebug_set_cart_path(astrodebug *d, const char *path);

/* ---- the host (core/astro/host.c) ---- */
struct astro_machine;
/* The machine was just initialized (astro_machine_init wipes the hooks), and
 * its thread has not started: record it, and if a window holds the engine
 * attached, install the hooks and stop it at its first instruction. */
void astrodebug_core_started(struct astro_machine *m);
/* The machine thread is about to be joined: release it if it is parked, make
 * the hooks return at once, and stop touching the machine. */
void astrodebug_core_stopping(void);

#ifdef __cplusplus
}
#endif

#endif /* ASTRO_DEBUGGER_INTERNAL_H */
