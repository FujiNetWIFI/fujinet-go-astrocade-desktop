/*
 * fujitrace.c -- see fujitrace.h. The SLIP decode is the MS-DOS desktop's
 * FujiTrace (fujinet-go-msdos-desktop core/src/debug.cpp), in C.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "fujitrace.h"
#include "fuji_names.h"

#define FNV_BASIS 1469598103934665603ULL
#define FNV_PRIME 1099511628211ULL

static const char *fuji_command_name(int id)
{
    for (size_t i = 0; i < sizeof fuji_command_names / sizeof *fuji_command_names; i++)
        if (fuji_command_names[i].id == id)
            return fuji_command_names[i].name;
    return "";
}

/* i: 0 is the oldest */
static fujitrace_entry *at(fujitrace *t, int i)
{
    return &t->ring[(t->head + i) % FUJITRACE_MAX];
}

static void push(fujitrace *t, const fujitrace_entry *e)
{
    if (t->count < FUJITRACE_MAX)
        t->count++;
    else
        t->head = (t->head + 1) % FUJITRACE_MAX;
    *at(t, t->count - 1) = *e;
}

static int same(const fujitrace_entry *a, const fujitrace_entry *b)
{
    return a->p.to_host == b->p.to_host && a->p.nbytes == b->p.nbytes && a->hash == b->hash;
}

/* Appends, or folds a repeat in: a packet the same as the last one bumps it;
 * an exchange (two packets) the same as the last two bumps both. */
static void add(fujitrace *t, fujitrace_entry *e)
{
    if (t->has_pending) {
        t->has_pending = 0;
        if (same(e, at(t, t->count - 1))) {
            at(t, t->count - 2)->p.repeat++;
            at(t, t->count - 1)->p.repeat++;
            at(t, t->count - 1)->p.serial = ++t->serial;
            return;
        }
        push(t, &t->pending);
    }
    e->p.serial = ++t->serial;
    if (t->count >= 1 && same(e, at(t, t->count - 1))) {
        at(t, t->count - 1)->p.repeat++;
        at(t, t->count - 1)->p.serial = e->p.serial;
        return;
    }
    if (t->count >= 2 && same(e, at(t, t->count - 2)) &&
        at(t, t->count - 2)->p.repeat == at(t, t->count - 1)->p.repeat) {
        t->pending = *e;
        t->has_pending = 1;
        return;
    }
    push(t, e);
}

static void frame_done(fujitrace *t, int to_host)
{
    const uint8_t *a = t->acc[to_host];
    const int n = t->acc_len[to_host];
    const char *way = to_host ? "FujiNet \xe2\x86\x92 Astrocade" : "Astrocade \xe2\x86\x92 FujiNet";
    fujitrace_entry e;

    memset(&e, 0, sizeof e);
    e.p.to_host = to_host;
    e.p.nbytes = n;
    e.p.repeat = 1;
    memcpy(e.p.data, a, n < (int)sizeof e.p.data ? (size_t)n : sizeof e.p.data);
    e.p.device = n >= 6 ? a[0] : -1;
    e.p.command = n >= 6 ? a[1] : -1;
    e.p.length = n >= 6 ? (a[2] | (a[3] << 8)) : -1;
    if (e.p.device >= 0) {
        const char *name = fuji_command_name(e.p.command);
        snprintf(e.p.text, sizeof e.p.text, "%s %02X %02X %s%s(%d bytes)", way,
                 e.p.device, e.p.command, name, *name ? " " : "", n);
    } else {
        snprintf(e.p.text, sizeof e.p.text, "%s %d bytes", way, n);
    }
    e.hash = t->acc_hash[to_host];
    add(t, &e);
}

void fujitrace_feed(fujitrace *t, int dir, const uint8_t *buf, size_t len)
{
    const int to_host = dir == 0 ? 1 : 0;

    /* a reply cut short is not glued onto the next one */
    if (!to_host) {
        t->acc_len[1] = 0;
        t->esc[1] = 0;
    }
    for (size_t i = 0; i < len; i++) {
        uint8_t b = buf[i];
        if (b == 0xC0) {
            if (t->acc_len[to_host] > 0)
                frame_done(t, to_host);
            t->acc_len[to_host] = 0;
            t->esc[to_host] = 0;
            continue;
        }
        if (t->esc[to_host]) {
            b = b == 0xDC ? 0xC0 : b == 0xDD ? 0xDB : b;
            t->esc[to_host] = 0;
        } else if (b == 0xDB) {
            t->esc[to_host] = 1;
            continue;
        }
        if (t->acc_len[to_host] == 0)
            t->acc_hash[to_host] = FNV_BASIS;
        if (t->acc_len[to_host] < (int)sizeof t->acc[0])
            t->acc[to_host][t->acc_len[to_host]] = b;
        t->acc_len[to_host]++;
        t->acc_hash[to_host] = (t->acc_hash[to_host] ^ b) * FNV_PRIME;
    }
}

int fujitrace_copy(const fujitrace *t, astrodebug_fuji_packet *out, int max)
{
    const int total = t->count + (t->has_pending ? 1 : 0);
    int i, n = 0;

    if (max <= 0)
        return 0;
    for (i = total > max ? total - max : 0; i < t->count; i++)
        out[n++] = t->ring[(t->head + i) % FUJITRACE_MAX].p;
    if (t->has_pending && n < max)
        out[n++] = t->pending.p;
    return n;
}

void fujitrace_clear(fujitrace *t)
{
    t->head = 0;
    t->count = 0;
    t->has_pending = 0;
}
