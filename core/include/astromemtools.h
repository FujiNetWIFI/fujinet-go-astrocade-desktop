/*
 * Watches, range parsing, memory-search queries and breakpoint descriptions,
 * kept in the core so the four debugger windows only lay them out.
 *
 * A watch is text the user typed (an address, a label, an expression, or a
 * range lo-hi / lo..hi) shown with its live value. It is stored as text and
 * resolved every time it is shown, so loading symbols later makes a label
 * start resolving.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ASTRO_MEMTOOLS_H
#define ASTRO_MEMTOOLS_H

#include <stdint.h>

#include "astrodebug.h"

#ifdef __cplusplus
extern "C" {
#endif

/* "lo..hi", "lo-hi" (plain numbers or labels on both sides), or one
 * expression (an address, label, label+3, [ptr]). A bare token of 1-4 hex
 * digits ("C000") is hex unless it names a label; "$C000" and expressions
 * work as in the console. Returns 1 and fills lo and hi (inclusive,
 * lo <= hi), 0 if the text means nothing. */
int astrosym_parse_range(astrodebug *d, const char *text, uint16_t *lo,
                         uint16_t *hi);

/* ---- watches ---- */
#define ASTROWATCH_MAX 64
/* The process-wide list, like the symbol tables: it outlives the window. */
int  astrowatch_count(void);
/* Returns the new index, or -1 if empty / full. A watch that does not resolve
 * yet is kept (it shows "?"). */
int  astrowatch_add(const char *text);
void astrowatch_remove(int index);
void astrowatch_clear(void);
const char *astrowatch_text(int index);
unsigned astrowatch_generation(void);
/* "score  $4F10  $42 (66)". While the machine runs the value is "-"; a range
 * shows its first 8 bytes. Returns the length. */
int  astrowatch_format(astrodebug *d, int index, char *dst, int dstsz);
/* The range a watch covers (for "show in Memory"). 1 if it resolves. */
int  astrowatch_range(astrodebug *d, int index, uint16_t *lo, uint16_t *hi);

/* ---- memory search ---- */
enum { ASTROSEARCH_HEX = 0, ASTROSEARCH_TEXT };
#define ASTROSEARCH_MAX 32
/* Hex: byte pairs, optional "$" / "0x" / spaces. Text: one byte per
 * character, case-insensitive. Fills pat, n, mask (0xFF) and flags for
 * astrodebug_search. Returns 1, or 0 for empty / malformed text. */
int astrosearch_parse(int kind, const char *text, uint8_t *pat, int *n,
                      uint8_t *mask, int *flags);

/* ---- breakpoints ---- */
/* "Write  $4100 (score)  if value != old" */
int astrobp_describe(astrodebug *d, const astrodebug_breakpoint *b, char *dst,
                     int dstsz);

#ifdef __cplusplus
}
#endif

#endif /* ASTRO_MEMTOOLS_H */
