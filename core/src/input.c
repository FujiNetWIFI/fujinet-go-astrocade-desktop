/*
 * input.c -- see input.h, and astrosession.h for the public half.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>

#include "host.h"
#include "bindings.h"
#include "input.h"
#include "session_internal.h"

#define KNOB_RATE 0.25f      /* units per ms: a full sweep in about a second */
#define KNOB_MIN_MOVE 3      /* a pad axis must move this far to take the knob */

typedef struct {
    uint32_t keys;                         /* keypad bits */
    uint8_t hand[ASTROSESSION_PLAYERS];    /* UP..TRIGGER bits per player */
    uint8_t knobdir[ASTROSESSION_PLAYERS]; /* bit0 dec, bit1 inc */
} src_state;

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static src_state g_src[ASTRO_SRC_COUNT];
static uint32_t g_keys_out;
static uint8_t  g_hand_out[ASTROSESSION_PLAYERS];
static float    g_knob[ASTROSESSION_PLAYERS] = { 128, 128, 128, 128 };
static int      g_knob_pushed[ASTROSESSION_PLAYERS] = { -1, -1, -1, -1 };
static int      g_pad_axis_last[ASTROSESSION_PLAYERS] = { -1, -1, -1, -1 };
/* the keysym that is holding each target (ignores auto-repeat, and a release
 * clears exactly what its press set) */
static uint32_t g_held_keysym[ASTRO_TARGET_COUNT];

static void push_knob_locked(int p)
{
    int v = (int)(g_knob[p] + 0.5f);
    if (v < 0) v = 0; else if (v > 255) v = 255;
    if (v == g_knob_pushed[p]) return;
    g_knob_pushed[p] = v;
    astro_host_knob_set(p, (uint8_t)v);
}

/* Recompute the union and tell the machine what changed. */
static void push_locked(int force)
{
    uint32_t keys = 0;
    uint8_t hand[ASTROSESSION_PLAYERS] = { 0 };
    for (int i = 0; i < ASTRO_SRC_COUNT; i++) {
        keys |= g_src[i].keys;
        for (int p = 0; p < ASTROSESSION_PLAYERS; p++)
            hand[p] |= g_src[i].hand[p];
    }
    const uint32_t diff = force ? 0xffffffffu : (keys ^ g_keys_out);
    for (int k = 0; k < ASTROSESSION_KEY_COUNT; k++)
        if ((diff >> k) & 1)
            astro_host_keypad_set(k % 4, k / 4, (keys >> k) & 1);
    g_keys_out = keys;
    for (int p = 0; p < ASTROSESSION_PLAYERS; p++)
        if (force || hand[p] != g_hand_out[p]) {
            g_hand_out[p] = hand[p];
            astro_host_handle_set(p, hand[p]);
        }
}

static int knob_dir_locked(int p)
{
    int dec = 0, inc = 0;
    for (int i = 0; i < ASTRO_SRC_COUNT; i++) {
        dec |= g_src[i].knobdir[p] & 1;
        inc |= (g_src[i].knobdir[p] >> 1) & 1;
    }
    return inc - dec;
}

/* Set or clear one target's hold on a source. */
static void hold_locked(int src, int target, int down)
{
    src_state *st = &g_src[src];
    if (target < ASTROSESSION_KEY_COUNT) {
        if (down) st->keys |= 1u << target; else st->keys &= ~(1u << target);
        return;
    }
    if (target < ASTRO_TARGET_PLAYERS || target >= ASTRO_TARGET_COUNT)
        return;
    const int rel = target - ASTRO_TARGET_PLAYERS;
    const int p = rel / ASTRO_ACT_PER_PLAYER, act = rel % ASTRO_ACT_PER_PLAYER;
    if (act <= ASTRO_ACT_TRIGGER) {
        const uint8_t bit = (uint8_t)(1u << act);   /* UP..TRIGGER = 0x01..0x10 */
        if (down) st->hand[p] |= bit; else st->hand[p] &= (uint8_t)~bit;
    } else if (act == ASTRO_ACT_KNOB_DEC || act == ASTRO_ACT_KNOB_INC) {
        const uint8_t bit = (act == ASTRO_ACT_KNOB_DEC) ? 1 : 2;
        if (down) st->knobdir[p] |= bit; else st->knobdir[p] &= (uint8_t)~bit;
    }
}

/* ---- public ---------------------------------------------------------------- */

void astrosession_press(astrosession *s, int target, int down)
{
    (void)s;
    if (target < 0 || target >= ASTRO_TARGET_COUNT) return;
    pthread_mutex_lock(&g_mtx);
    hold_locked(ASTRO_SRC_PANEL, target, down != 0);
    push_locked(0);
    pthread_mutex_unlock(&g_mtx);
}

int astrosession_target_held(astrosession *s, int target)
{
    int held = 0;
    (void)s;
    if (target < 0 || target >= ASTRO_TARGET_COUNT) return 0;
    pthread_mutex_lock(&g_mtx);
    if (target < ASTROSESSION_KEY_COUNT) {
        held = (g_keys_out >> target) & 1;
    } else if (target >= ASTRO_TARGET_PLAYERS) {
        const int rel = target - ASTRO_TARGET_PLAYERS;
        const int p = rel / ASTRO_ACT_PER_PLAYER, act = rel % ASTRO_ACT_PER_PLAYER;
        if (act <= ASTRO_ACT_TRIGGER)
            held = (g_hand_out[p] >> act) & 1;
        else if (act == ASTRO_ACT_KNOB_DEC)
            held = knob_dir_locked(p) < 0;
        else if (act == ASTRO_ACT_KNOB_INC)
            held = knob_dir_locked(p) > 0;
    }
    pthread_mutex_unlock(&g_mtx);
    return held;
}

void astrosession_release_all(astrosession *s)
{
    (void)s;
    pthread_mutex_lock(&g_mtx);
    memset(&g_src[ASTRO_SRC_KEYBOARD], 0, sizeof g_src[0]);
    memset(g_held_keysym, 0, sizeof g_held_keysym);
    push_locked(0);
    pthread_mutex_unlock(&g_mtx);
}

int astrosession_input_key(astrosession *s, uint32_t keysym, int down,
                           int ctrl_down)
{
    int consumed = 0;
    pthread_mutex_lock(&g_mtx);
    if (down) {
        const int t = ctrl_down ? -1 : bindings_target_for_key(keysym);
        if (t >= 0) {
            consumed = 1;
            if (g_held_keysym[t] != keysym) {     /* not an auto-repeat */
                g_held_keysym[t] = keysym;
                if (t >= ASTRO_TARGET_SYSACT && t < ASTRO_TARGET_PLAYERS)
                    astrosession_sysaction_post(
                        s, (astrosession_sysaction)(t - ASTRO_TARGET_SYSACT));
                else {
                    hold_locked(ASTRO_SRC_KEYBOARD, t, 1);
                    push_locked(0);
                }
            }
        }
    } else {
        /* a release clears exactly what the press set, whatever has been
         * rebound or pressed (Ctrl) since */
        for (int t = 0; t < ASTRO_TARGET_COUNT; t++)
            if (g_held_keysym[t] == keysym && keysym) {
                g_held_keysym[t] = 0;
                consumed = 1;
                if (t < ASTRO_TARGET_SYSACT || t >= ASTRO_TARGET_PLAYERS) {
                    hold_locked(ASTRO_SRC_KEYBOARD, t, 0);
                    push_locked(0);
                }
            }
        if (!consumed && bindings_target_for_key(keysym) >= 0)
            consumed = 1;
    }
    pthread_mutex_unlock(&g_mtx);
    return consumed;
}

void astrosession_sysaction_drain(astrosession *s)
{
    unsigned pending = astrosession_sysaction_take(s);
    for (int a = 0; a < ASTROSESSION_SYSACT_COUNT; a++)
        if (pending & (1u << a))
            astrosession_sysaction_fire(s, (astrosession_sysaction)a);
}

void astrosession_knob_set(astrosession *s, int player, uint8_t value)
{
    (void)s;
    if (player < 0 || player >= ASTROSESSION_PLAYERS) return;
    pthread_mutex_lock(&g_mtx);
    g_knob[player] = value;
    push_knob_locked(player);
    pthread_mutex_unlock(&g_mtx);
}

int astrosession_knob_get(astrosession *s, int player)
{
    int v;
    (void)s;
    if (player < 0 || player >= ASTROSESSION_PLAYERS) return ASTROSESSION_KNOB_CENTRE;
    pthread_mutex_lock(&g_mtx);
    v = (int)(g_knob[player] + 0.5f);
    pthread_mutex_unlock(&g_mtx);
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* The dial: 270 degrees, 0 at the lower left (-135), 255 at the lower right
 * (+135), 128 straight up. */
double astrosession_knob_angle(int value)
{
    if (value < 0) value = 0; else if (value > 255) value = 255;
    return -135.0 + 270.0 * value / 255.0;
}

int astrosession_knob_from_point(double dx, double dy, double radius)
{
    if (radius <= 0 || sqrt(dx * dx + dy * dy) < radius * 0.2)
        return -1;
    double deg = atan2(dx, -dy) * 180.0 / M_PI;      /* 0 = up, clockwise */
    if (deg > 135.0) deg = 135.0;
    if (deg < -135.0) deg = -135.0;
    int v = (int)lround((deg + 135.0) / 270.0 * 255.0);
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* ---- pad side ----------------------------------------------------------------- */

void astro_input_pad_state(int slot, uint32_t keys, int hand_player,
                           uint8_t hand, uint8_t knobdir)
{
    if (slot < 0 || slot >= ASTROSESSION_PLAYERS) return;
    pthread_mutex_lock(&g_mtx);
    src_state *st = &g_src[ASTRO_SRC_PAD + slot];
    memset(st, 0, sizeof *st);
    st->keys = keys & ((1u << ASTROSESSION_KEY_COUNT) - 1);
    if (hand_player >= 0 && hand_player < ASTROSESSION_PLAYERS) {
        st->hand[hand_player] = hand & 0x1f;
        st->knobdir[hand_player] = knobdir & 3;
    }
    push_locked(0);
    pthread_mutex_unlock(&g_mtx);
}

void astro_input_pad_knob(int slot, int player, int value)
{
    if (slot < 0 || slot >= ASTROSESSION_PLAYERS ||
        player < 0 || player >= ASTROSESSION_PLAYERS) return;
    if (value < 0) value = 0; else if (value > 255) value = 255;
    pthread_mutex_lock(&g_mtx);
    const int last = g_pad_axis_last[slot];
    if (last < 0 || abs(value - last) >= KNOB_MIN_MOVE) {
        if (last >= 0) {                  /* the first sample only records */
            g_knob[player] = (float)value;
            push_knob_locked(player);
        }
        g_pad_axis_last[slot] = value;
    }
    pthread_mutex_unlock(&g_mtx);
}

void astro_input_pad_clear(int slot)
{
    if (slot < 0 || slot >= ASTROSESSION_PLAYERS) return;
    pthread_mutex_lock(&g_mtx);
    memset(&g_src[ASTRO_SRC_PAD + slot], 0, sizeof g_src[0]);
    g_pad_axis_last[slot] = -1;
    push_locked(0);
    pthread_mutex_unlock(&g_mtx);
}

void astro_input_tick(int dt_ms)
{
    pthread_mutex_lock(&g_mtx);
    for (int p = 0; p < ASTROSESSION_PLAYERS; p++) {
        const int dir = knob_dir_locked(p);
        if (!dir) continue;
        g_knob[p] += dir * KNOB_RATE * (float)dt_ms;
        if (g_knob[p] < 0) g_knob[p] = 0; else if (g_knob[p] > 255) g_knob[p] = 255;
        push_knob_locked(p);
    }
    pthread_mutex_unlock(&g_mtx);
}

int astro_input_knob_active(void)
{
    int on = 0;
    pthread_mutex_lock(&g_mtx);
    for (int p = 0; p < ASTROSESSION_PLAYERS; p++)
        if (knob_dir_locked(p)) on = 1;
    pthread_mutex_unlock(&g_mtx);
    return on;
}

void astro_input_resync(void)
{
    pthread_mutex_lock(&g_mtx);
    for (int p = 0; p < ASTROSESSION_PLAYERS; p++)
        g_knob_pushed[p] = -1;
    for (int p = 0; p < ASTROSESSION_PLAYERS; p++)
        push_knob_locked(p);
    push_locked(1);
    pthread_mutex_unlock(&g_mtx);
}

void astro_input_clear_all(void)
{
    pthread_mutex_lock(&g_mtx);
    memset(g_src, 0, sizeof g_src);
    memset(g_held_keysym, 0, sizeof g_held_keysym);
    push_locked(1);
    pthread_mutex_unlock(&g_mtx);
}
