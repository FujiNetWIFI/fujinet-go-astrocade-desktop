/*
 * input.c -- see input.h, and astrosession.h for the public half.
 *
 * Process-global, like the machine it drives (a process singleton): one
 * mutex, one table of "who holds each target". After every change the union
 * is recomputed and what changed is handed to the machine.
 *
 * A direction pair held both ways (UP and DOWN, LEFT and RIGHT) holds
 * neither: a stick's switches cannot close both, and astrosession_target_held
 * reports exactly what the machine sees.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "host.h"
#include "bindings.h"
#include "input.h"
#include "session_internal.h"

#define KNOB_RATE 0.25f      /* units per ms: a full sweep in about a second */
#define KNOB_MIN_MOVE 3      /* a pad axis must move this far to take the knob */

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
/* who holds each target (ASTRO_SRC_* bits); the system actions are not held */
static uint32_t g_bits[ASTRO_TARGET_COUNT];
static uint32_t g_keys_out;
static uint8_t  g_hand_out[ASTROSESSION_PLAYERS];
static float    g_knob[ASTROSESSION_PLAYERS] = { 128, 128, 128, 128 };
static int      g_knob_pushed[ASTROSESSION_PLAYERS] = { -1, -1, -1, -1 };
static int      g_pad_axis_last[ASTRO_MAX_PADS] = { -1, -1, -1, -1, -1, -1, -1, -1 };
/* the keysym that is holding each target (ignores auto-repeat, and a release
 * clears exactly what its press set) */
static uint32_t g_held_keysym[ASTRO_TARGET_COUNT];
/* Backspace was taken as an ERASE tap and its release is still to come (UI thread) */
static int g_backspace_typed;

static int is_held_target(int target)
{
    return target >= 0 && target < ASTRO_TARGET_COUNT &&
           !(target >= ASTRO_TARGET_SYSACT && target < ASTRO_TARGET_PLAYERS);
}

static void push_knob_locked(int p)
{
    int v = (int)(g_knob[p] + 0.5f);
    if (v < 0) v = 0; else if (v > 255) v = 255;
    if (v == g_knob_pushed[p]) return;
    g_knob_pushed[p] = v;
    astro_host_knob_set(p, (uint8_t)v);
}

static uint8_t hand_locked(int p)
{
    uint8_t hand = 0;
    for (int act = 0; act <= ASTRO_ACT_TRIGGER; act++)
        if (g_bits[ASTRO_TARGET_PLAYER(p, act)])
            hand |= (uint8_t)(1u << act);        /* UP..TRIGGER = 0x01..0x10 */
    if ((hand & ASTROSESSION_HANDLE_UP) && (hand & ASTROSESSION_HANDLE_DOWN))
        hand &= (uint8_t)~(ASTROSESSION_HANDLE_UP | ASTROSESSION_HANDLE_DOWN);
    if ((hand & ASTROSESSION_HANDLE_LEFT) && (hand & ASTROSESSION_HANDLE_RIGHT))
        hand &= (uint8_t)~(ASTROSESSION_HANDLE_LEFT | ASTROSESSION_HANDLE_RIGHT);
    return hand;
}

/* Recompute the union and tell the machine what changed. */
static void push_locked(int force)
{
    uint32_t keys = 0;
    for (int k = 0; k < ASTROSESSION_KEY_COUNT; k++)
        if (g_bits[ASTRO_TARGET_KEYPAD + k])
            keys |= 1u << k;
    const uint32_t diff = force ? 0xffffffffu : (keys ^ g_keys_out);
    for (int k = 0; k < ASTROSESSION_KEY_COUNT; k++)
        if ((diff >> k) & 1)
            astro_host_keypad_set(k % 4, k / 4, (keys >> k) & 1);
    g_keys_out = keys;
    for (int p = 0; p < ASTROSESSION_PLAYERS; p++) {
        const uint8_t hand = hand_locked(p);
        if (force || hand != g_hand_out[p]) {
            g_hand_out[p] = hand;
            astro_host_handle_set(p, hand);
        }
    }
}

static int knob_dir_locked(int p)
{
    const int dec = g_bits[ASTRO_TARGET_PLAYER(p, ASTRO_ACT_KNOB_DEC)] != 0;
    const int inc = g_bits[ASTRO_TARGET_PLAYER(p, ASTRO_ACT_KNOB_INC)] != 0;
    return inc - dec;
}

static void source_locked(int target, uint32_t src, int down)
{
    if (!is_held_target(target)) return;
    if (down) g_bits[target] |= src; else g_bits[target] &= ~src;
}

static void clear_sources_locked(uint32_t mask)
{
    for (int t = 0; t < ASTRO_TARGET_COUNT; t++)
        g_bits[t] &= ~mask;
}

/* ---- public: the window's mouse ----------------------------------------------- */

void astrosession_press(astrosession *s, int target, int down)
{
    (void)s;
    if (!is_held_target(target)) return;
    pthread_mutex_lock(&g_mtx);
    source_locked(target, ASTRO_SRC_POINTER, down != 0);
    push_locked(0);
    pthread_mutex_unlock(&g_mtx);
}

void astrosession_pointer_set_player(astrosession *s, int player, unsigned act_mask)
{
    (void)s;
    if (player < 0 || player >= ASTROSESSION_PLAYERS) return;
    pthread_mutex_lock(&g_mtx);
    for (int act = 0; act <= ASTRO_ACT_KNOB_INC; act++)
        source_locked(ASTRO_TARGET_PLAYER(player, act), ASTRO_SRC_POINTER,
                      (act_mask >> act) & 1u);
    push_locked(0);
    pthread_mutex_unlock(&g_mtx);
}

void astrosession_pointer_release(astrosession *s)
{
    (void)s;
    pthread_mutex_lock(&g_mtx);
    clear_sources_locked(ASTRO_SRC_POINTER);
    push_locked(0);
    pthread_mutex_unlock(&g_mtx);
}

int astrosession_target_held(astrosession *s, int target)
{
    int held = 0;
    (void)s;
    if (!is_held_target(target)) return 0;
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
    astro_input_typing_stop();
    pthread_mutex_lock(&g_mtx);
    clear_sources_locked(ASTRO_SRC_KEYBOARD);
    memset(g_held_keysym, 0, sizeof g_held_keysym);
    g_backspace_typed = 0;
    push_locked(0);
    pthread_mutex_unlock(&g_mtx);
}

/* ---- public: the keyboard ----------------------------------------------------- */

/* With the overlay on, a printable host key that is bound to a keypad key
 * types its character instead (astrosession_type_char), so BASIC can be typed
 * on the host keyboard; a key bound to the stick, the trigger, a knob key or a
 * console action keeps its binding. */
static int types_instead(astrosession *s, uint32_t keysym, int target)
{
    return target >= 0 && target < ASTROSESSION_KEY_COUNT &&
           keysym >= 0x21 && keysym <= 0x7e && astrosession_overlay_get(s);
}

/* Backspace is Reset Game by default, but with the overlay on it is the
 * card's ERASE: a typo in a BASIC line must not wipe the program. One ERASE
 * per press (an auto-repeat is ignored, as for every typed key). With Ctrl held
 * it is left alone, as every Ctrl chord is. */
static int backspace_erases(astrosession *s, uint32_t keysym, int down, int ctrl_down)
{
    uint8_t keys[2];
    if (keysym != ASTROSESSION_KEYSYM_BACKSPACE)
        return 0;
    if (!down) {
        const int was = g_backspace_typed;
        g_backspace_typed = 0;
        return was;                              /* the release of a press we took */
    }
    if (ctrl_down || !astrosession_overlay_get(s))
        return 0;
    if (!g_backspace_typed &&
        astrosession_overlay_sequence(8, keys, 2) == 1 &&
        astro_input_type_keys(keys, 1) == 1)
        g_backspace_typed = 1;
    return g_backspace_typed;                    /* also swallows the repeats */
}

int astrosession_input_key(astrosession *s, uint32_t keysym, int down,
                           int ctrl_down)
{
    int consumed = 0;
    if (backspace_erases(s, keysym, down, ctrl_down))
        return 1;
    if (down) {
        const int t = ctrl_down ? -1 : bindings_target_for_key(keysym);
        if (t >= 0 && !types_instead(s, keysym, t)) {
            consumed = 1;
            pthread_mutex_lock(&g_mtx);
            if (g_held_keysym[t] != keysym) {     /* not an auto-repeat */
                g_held_keysym[t] = keysym;
                if (t >= ASTRO_TARGET_SYSACT && t < ASTRO_TARGET_PLAYERS)
                    astrosession_sysaction_post(
                        s, (astrosession_sysaction)(t - ASTRO_TARGET_SYSACT));
                else {
                    source_locked(t, ASTRO_SRC_KEYBOARD, 1);
                    push_locked(0);
                }
            }
            pthread_mutex_unlock(&g_mtx);
        }
    } else {
        /* a release clears exactly what the press set, whatever has been
         * rebound or pressed (Ctrl) since */
        pthread_mutex_lock(&g_mtx);
        for (int t = 0; t < ASTRO_TARGET_COUNT; t++)
            if (g_held_keysym[t] == keysym && keysym) {
                g_held_keysym[t] = 0;
                consumed = 1;
                if (is_held_target(t)) {
                    source_locked(t, ASTRO_SRC_KEYBOARD, 0);
                    push_locked(0);
                }
            }
        pthread_mutex_unlock(&g_mtx);
        if (!consumed) {
            const int t = bindings_target_for_key(keysym);
            if (t >= 0 && !types_instead(s, keysym, t))
                consumed = 1;
        }
    }
    return consumed;
}

void astrosession_sysaction_drain(astrosession *s)
{
    unsigned pending = astrosession_sysaction_take(s);
    for (int a = 0; a < ASTROSESSION_SYSACT_COUNT; a++)
        if (pending & (1u << a))
            astrosession_sysaction_fire(s, (astrosession_sysaction)a);
}

/* ---- public: the knob ---------------------------------------------------------- */

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

void astro_input_pad_press(int owner, int target, int down)
{
    if (owner < 0 || owner >= ASTRO_MAX_PADS || !is_held_target(target)) return;
    pthread_mutex_lock(&g_mtx);
    source_locked(target, ASTRO_SRC_PAD(owner), down != 0);
    push_locked(0);
    pthread_mutex_unlock(&g_mtx);
}

void astro_input_pad_stick(int owner, int player, int act, int down)
{
    if (owner < 0 || owner >= ASTRO_MAX_PADS || player < 0 ||
        player >= ASTROSESSION_PLAYERS || act < 0 || act >= ASTRO_ACT_PER_PLAYER)
        return;
    pthread_mutex_lock(&g_mtx);
    source_locked(ASTRO_TARGET_PLAYER(player, act), ASTRO_SRC_STICK(owner), down != 0);
    push_locked(0);
    pthread_mutex_unlock(&g_mtx);
}

void astro_input_pad_knob(int owner, int player, int value)
{
    if (owner < 0 || owner >= ASTRO_MAX_PADS ||
        player < 0 || player >= ASTROSESSION_PLAYERS) return;
    if (value < 0) value = 0; else if (value > 255) value = 255;
    pthread_mutex_lock(&g_mtx);
    const int last = g_pad_axis_last[owner];
    if (last < 0 || abs(value - last) >= KNOB_MIN_MOVE) {
        if (last >= 0) {                  /* the first sample only records */
            g_knob[player] = (float)value;
            push_knob_locked(player);
        }
        g_pad_axis_last[owner] = value;
    }
    pthread_mutex_unlock(&g_mtx);
}

void astro_input_pad_clear(int owner)
{
    if (owner < 0 || owner >= ASTRO_MAX_PADS) return;
    pthread_mutex_lock(&g_mtx);
    clear_sources_locked(ASTRO_SRC_PAD(owner) | ASTRO_SRC_STICK(owner));
    g_pad_axis_last[owner] = -1;
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

/* ---- typing: the overlay's presses, paced ---------------------------------------
 * The BIOS scans the keypad once a frame (interpreted, so a press shorter than
 * a few frames can be missed). Each queued key is held TYPE_HOLD ticks and the
 * keypad rests TYPE_GAP ticks before the next, one tick a frame at 60 Hz. The
 * ticks come from a thread of its own, started with the first queued key and
 * gone when the queue is empty: typing must work with no pad and no frontend
 * clock. */
#define TYPE_QUEUE 512
#define TYPE_HOLD  4
#define TYPE_GAP   4
#define TYPE_GAP_LONG 12       /* after ERASE and GO: BASIC redraws / runs the line */
#define TYPE_LONG  0x80        /* a queue entry's flag: rest TYPE_GAP_LONG after it */
#define TYPE_TICK_NS 16666667L

static uint8_t g_type_q[TYPE_QUEUE];
static int     g_type_head, g_type_count;
static int     g_type_phase;          /* 0 resting, 1 holding */
static int     g_type_ticks;          /* ticks left in the phase */
static int     g_type_key = -1;       /* the key being held */
static int     g_type_long;           /* the key being held rests long */
static int     g_type_running;        /* the thread exists */
static int     g_type_quit;

static void type_release_locked(void)
{
    if (g_type_key >= 0)
        source_locked(ASTRO_TARGET_KEYPAD + g_type_key, ASTRO_SRC_TYPING, 0);
    g_type_key = -1;
    g_type_long = 0;
    g_type_phase = 0;
    g_type_ticks = 0;
}

/* One tick; returns 0 when there is nothing left to do. g_mtx held. */
static int type_step_locked(void)
{
    if (g_type_ticks > 0)
        g_type_ticks--;
    if (g_type_ticks > 0)
        return 1;
    if (g_type_phase == 1) {                       /* the hold is over: rest */
        const int rest = g_type_long ? TYPE_GAP_LONG : TYPE_GAP;
        type_release_locked();
        g_type_ticks = rest;
        push_locked(0);
        return 1;
    }
    if (g_type_count == 0)
        return 0;
    g_type_key = g_type_q[g_type_head] & ~TYPE_LONG;   /* the rest is over: next key */
    g_type_long = (g_type_q[g_type_head] & TYPE_LONG) != 0;
    g_type_head = (g_type_head + 1) % TYPE_QUEUE;
    g_type_count--;
    g_type_phase = 1;
    g_type_ticks = TYPE_HOLD;
    source_locked(ASTRO_TARGET_KEYPAD + g_type_key, ASTRO_SRC_TYPING, 1);
    push_locked(0);
    return 1;
}

static void *type_thread(void *arg)
{
    (void)arg;
    for (;;) {
        struct timespec ts = { 0, TYPE_TICK_NS };
        int more;
        nanosleep(&ts, NULL);
        pthread_mutex_lock(&g_mtx);
        more = !g_type_quit && type_step_locked();
        if (!more) {
            type_release_locked();
            push_locked(0);
            g_type_running = 0;
            pthread_mutex_unlock(&g_mtx);
            return NULL;
        }
        pthread_mutex_unlock(&g_mtx);
    }
}

int astro_input_type_keys(const uint8_t *keys, int count)
{
    int queued = 0;
    if (!keys || count <= 0) return 0;
    pthread_mutex_lock(&g_mtx);
    while (queued < count && g_type_count < TYPE_QUEUE) {
        if (keys[queued] < ASTROSESSION_KEY_COUNT) {
            g_type_q[(g_type_head + g_type_count) % TYPE_QUEUE] =
                (uint8_t)(keys[queued] |
                          ((count == 1 && (keys[queued] == ASTROSESSION_KEY_3 ||
                                           keys[queued] == ASTROSESSION_KEY_C)) ? TYPE_LONG : 0));
            g_type_count++;
        }
        queued++;
    }
    if (g_type_count > 0 && !g_type_running) {
        pthread_t thr;
        g_type_quit = 0;
        if (pthread_create(&thr, NULL, type_thread, NULL) == 0) {
            pthread_detach(thr);
            g_type_running = 1;
        } else {
            g_type_count = 0;                      /* cannot type: drop it */
            queued = 0;
        }
    }
    pthread_mutex_unlock(&g_mtx);
    return queued;
}

int astro_input_typing_pending(void)
{
    int n;
    pthread_mutex_lock(&g_mtx);
    n = g_type_count + (g_type_key >= 0 ? 1 : 0);
    pthread_mutex_unlock(&g_mtx);
    return n;
}

void astro_input_typing_stop(void)
{
    for (;;) {
        int running;
        pthread_mutex_lock(&g_mtx);
        g_type_quit = 1;
        g_type_count = 0;
        g_type_head = 0;
        type_release_locked();
        push_locked(0);
        running = g_type_running;
        pthread_mutex_unlock(&g_mtx);
        if (!running) break;
        struct timespec ts = { 0, 2000000L };
        nanosleep(&ts, NULL);
    }
}

int astrosession_typing_pending(astrosession *s)
{
    (void)s;
    return astro_input_typing_pending();
}

void astro_input_clear_all(void)
{
    astro_input_typing_stop();
    pthread_mutex_lock(&g_mtx);
    memset(g_bits, 0, sizeof g_bits);
    memset(g_held_keysym, 0, sizeof g_held_keysym);
    push_locked(1);
    pthread_mutex_unlock(&g_mtx);
}
