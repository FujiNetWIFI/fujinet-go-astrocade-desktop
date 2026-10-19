/*
 * fujitrace -- FujiNet's bytes, both ways: SLIP frames into FujiBus packets,
 * kept in a ring with consecutive repeats folded into x N.
 *
 * Pure: no machine and no lock (debugger.c holds its own around these), so a
 * test can drive it with synthetic bytes.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ASTRO_FUJITRACE_H
#define ASTRO_FUJITRACE_H

#include <stddef.h>
#include <stdint.h>

#include "astrodebug.h"

#define FUJITRACE_MAX 512

typedef struct {
    astrodebug_fuji_packet p;
    uint64_t hash;              /* the whole decoded packet, for repeats */
} fujitrace_entry;

typedef struct {
    fujitrace_entry ring[FUJITRACE_MAX];
    int head, count;            /* oldest at head */
    /* The first half of a repeating exchange, held back until its partner
     * shows (or not); listed last meanwhile. */
    fujitrace_entry pending;
    int has_pending;
    uint64_t serial;
    /* the frame being decoded, by to_host: its first bytes, its length and
     * a running hash of all of it */
    uint8_t acc[2][64];
    int acc_len[2];
    uint64_t acc_hash[2];
    int esc[2];
} fujitrace;

/* dir 0: FujiNet -> Astrocade, as the cart receives it.
 * dir 1: Astrocade -> FujiNet, the bytes the cart sends.
 * Any split of the stream is fine. */
void fujitrace_feed(fujitrace *t, int dir, const uint8_t *buf, size_t len);
/* The newest `max` packets, oldest first. */
int  fujitrace_copy(const fujitrace *t, astrodebug_fuji_packet *out, int max);
void fujitrace_clear(fujitrace *t);

#endif /* ASTRO_FUJITRACE_H */
