/*
 * bindings.c -- see bindings.h.
 *
 * Two tables: s_defaults (computed once, never mutated) and s_table (live).
 * Diffing them makes persistence (only non-default entries written) and reset
 * trivial. Ported from the ADAM desktop's bindings.c.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bindings.h"
#include "hid_keys.h"

typedef struct {
    uint32_t keysym;   /* 0 = unbound */
    int pad;           /* pad input id, -1 = unbound */
} slot;

static slot s_defaults[ASTRO_TARGET_COUNT];
static slot s_table[ASTRO_TARGET_COUNT];
static int  s_ready;
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;

/* gamepad button ids (SDL_GamepadButton): SOUTH = 0 */
#define PAD_SOUTH 0
#define PAD_AXIS_RIGHTX (ASTRO_PAD_AXIS_BASE + 2)

/* The physical key, so "5" is the key that prints 5 whatever Shift does. The
 * operators sit where a laptop can reach them: '=' (the + key) adds, 'x'
 * multiplies. */
static const uint32_t k_key_default[ASTROSESSION_KEY_COUNT] = {
    'c', 'u', 'd', 'p',
    'r', 's', 'h', '/',
    '7', '8', '9', 'x',
    '4', '5', '6', '-',
    '1', '2', '3', '=',
    'e', '0', '.', ASTROSESSION_KEYSYM_RETURN,
};

static void compute_defaults(void)
{
    for (int t = 0; t < ASTRO_TARGET_COUNT; t++) {
        s_defaults[t].keysym = 0;
        s_defaults[t].pad = -1;
    }
    for (int k = 0; k < ASTROSESSION_KEY_COUNT; k++)
        s_defaults[k].keysym = k_key_default[k];
    s_defaults[ASTRO_TARGET_SYSACT + ASTROSESSION_SYSACT_RESET_GAME].keysym =
        ASTROSESSION_KEYSYM_BACKSPACE;

    /* player 1 plays from the keyboard; every pad plays its own player */
    s_defaults[ASTRO_TARGET_PLAYER(0, ASTRO_ACT_UP)].keysym = ASTROSESSION_KEYSYM_UP;
    s_defaults[ASTRO_TARGET_PLAYER(0, ASTRO_ACT_DOWN)].keysym = ASTROSESSION_KEYSYM_DOWN;
    s_defaults[ASTRO_TARGET_PLAYER(0, ASTRO_ACT_LEFT)].keysym = ASTROSESSION_KEYSYM_LEFT;
    s_defaults[ASTRO_TARGET_PLAYER(0, ASTRO_ACT_RIGHT)].keysym = ASTROSESSION_KEYSYM_RIGHT;
    s_defaults[ASTRO_TARGET_PLAYER(0, ASTRO_ACT_TRIGGER)].keysym = ASTROSESSION_KEYSYM_SPACE;
    s_defaults[ASTRO_TARGET_PLAYER(0, ASTRO_ACT_KNOB_DEC)].keysym = '[';
    s_defaults[ASTRO_TARGET_PLAYER(0, ASTRO_ACT_KNOB_INC)].keysym = ']';
    for (int p = 0; p < ASTROSESSION_PLAYERS; p++) {
        s_defaults[ASTRO_TARGET_PLAYER(p, ASTRO_ACT_TRIGGER)].pad = PAD_SOUTH;
        s_defaults[ASTRO_TARGET_PLAYER(p, ASTRO_ACT_KNOB)].pad = PAD_AXIS_RIGHTX;
    }
}

static const char *k_key_label[ASTROSESSION_KEY_COUNT] = {
    "C", "\xE2\x86\x91", "\xE2\x86\x93", "%",
    "MR", "MS", "CH", "\xC3\xB7",
    "7", "8", "9", "\xC3\x97",
    "4", "5", "6", "\xE2\x88\x92",
    "1", "2", "3", "+",
    "CE", "0", ".", "=",
};
static const char *k_act_label[ASTRO_ACT_PER_PLAYER] = {
    "Up", "Down", "Left", "Right", "Trigger", "Knob \xE2\x97\x80", "Knob \xE2\x96\xB6", "Knob",
};

const char *bindings_target_short_label(int target)
{
    if (target >= 0 && target < ASTROSESSION_KEY_COUNT)
        return k_key_label[target];
    if (target >= ASTRO_TARGET_SYSACT && target < ASTRO_TARGET_PLAYERS)
        return target == ASTRO_TARGET_SYSACT + ASTROSESSION_SYSACT_RESET_GAME
                   ? "Reset Game" : "Reset to CONFIG";
    if (target >= ASTRO_TARGET_PLAYERS && target < ASTRO_TARGET_COUNT)
        return k_act_label[(target - ASTRO_TARGET_PLAYERS) % ASTRO_ACT_PER_PLAYER];
    return "?";
}

const char *bindings_target_label(int target)
{
    static char buf[ASTRO_TARGET_COUNT][40];
    if (target < 0 || target >= ASTRO_TARGET_COUNT)
        return "?";
    if (!buf[target][0]) {
        if (target < ASTROSESSION_KEY_COUNT)
            snprintf(buf[target], sizeof buf[target], "Keypad %s",
                     bindings_target_short_label(target));
        else if (target < ASTRO_TARGET_PLAYERS)
            snprintf(buf[target], sizeof buf[target], "%s",
                     bindings_target_short_label(target));
        else {
            const int rel = target - ASTRO_TARGET_PLAYERS;
            snprintf(buf[target], sizeof buf[target], "Player %d: %s",
                     rel / ASTRO_ACT_PER_PLAYER + 1,
                     bindings_target_short_label(target));
        }
    }
    return buf[target];
}

/* -1: every pad; 0-3: that player's pad */
static int scope_of(int target)
{
    return target < ASTRO_TARGET_PLAYERS
               ? -1 : (target - ASTRO_TARGET_PLAYERS) / ASTRO_ACT_PER_PLAYER;
}

static void ensure_ready(void)
{
    if (s_ready)
        return;
    compute_defaults();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_ready = 1;
}

/* ---- persistence: "<target>.k:<keysym>" / ".p:<pad id>", space-separated -- */

static void persist(astrosession *s)
{
    char buf[4096];
    size_t n = 0;
    buf[0] = '\0';
    for (int t = 0; t < ASTRO_TARGET_COUNT && n + 48 < sizeof buf; t++) {
        if (s_table[t].keysym != s_defaults[t].keysym)
            n += (size_t)snprintf(buf + n, sizeof buf - n, "%s%d.k:%u",
                                  n ? " " : "", t, s_table[t].keysym);
        if (n + 48 < sizeof buf && s_table[t].pad != s_defaults[t].pad)
            n += (size_t)snprintf(buf + n, sizeof buf - n, "%s%d.p:%d",
                                  n ? " " : "", t, s_table[t].pad);
    }
    astrosession_set_str(s, "bindings", buf);
}

static void load_overrides(astrosession *s)
{
    const char *str = astrosession_get_str(s, "bindings", "");
    if (!str || !*str)
        return;
    for (const char *p = str; *p; ) {
        int target = 0, consumed = 0;
        char kind = 0;
        long val = 0;
        if (sscanf(p, "%d.%c:%ld%n", &target, &kind, &val, &consumed) == 3 &&
            target >= 0 && target < ASTRO_TARGET_COUNT) {
            if (kind == 'k') s_table[target].keysym = (uint32_t)val;
            else if (kind == 'p') s_table[target].pad = (int)val;
        }
        const char *next = consumed > 0 ? p + consumed : p + 1;
        while (*next == ' ' || *next == ';') next++;
        p = next;
    }
}

void bindings_init(astrosession *s)
{
    pthread_mutex_lock(&s_mtx);
    ensure_ready();
    memcpy(s_table, s_defaults, sizeof s_table);
    load_overrides(s);
    pthread_mutex_unlock(&s_mtx);
}

int bindings_target_for_key(uint32_t keysym)
{
    int r = -1;
    if (!keysym) return -1;
    pthread_mutex_lock(&s_mtx);
    ensure_ready();
    for (int t = 0; t < ASTRO_TARGET_COUNT; t++)
        if (s_table[t].keysym == keysym) { r = t; break; }
    pthread_mutex_unlock(&s_mtx);
    return r;
}

int bindings_target_for_pad(int player, int id)
{
    int r = -1;
    if (id < 0) return -1;
    pthread_mutex_lock(&s_mtx);
    ensure_ready();
    for (int t = 0; t < ASTRO_TARGET_COUNT; t++) {
        if (s_table[t].pad != id) continue;
        const int sc = scope_of(t);
        if (sc == -1 || sc == player) { r = t; break; }
    }
    pthread_mutex_unlock(&s_mtx);
    return r;
}

int bindings_knob_axis(int player)
{
    int r = -1;
    if (player < 0 || player >= ASTROSESSION_PLAYERS) return -1;
    pthread_mutex_lock(&s_mtx);
    ensure_ready();
    r = s_table[ASTRO_TARGET_PLAYER(player, ASTRO_ACT_KNOB)].pad;
    pthread_mutex_unlock(&s_mtx);
    return r >= ASTRO_PAD_AXIS_BASE ? r : -1;
}

uint32_t bindings_target_keysym(int target)
{
    uint32_t r = 0;
    if (target < 0 || target >= ASTRO_TARGET_COUNT) return 0;
    pthread_mutex_lock(&s_mtx);
    ensure_ready();
    r = s_table[target].keysym;
    pthread_mutex_unlock(&s_mtx);
    return r;
}

int bindings_target_pad(int target)
{
    int r = -1;
    if (target < 0 || target >= ASTRO_TARGET_COUNT) return -1;
    pthread_mutex_lock(&s_mtx);
    ensure_ready();
    r = s_table[target].pad;
    pthread_mutex_unlock(&s_mtx);
    return r;
}

void bindings_set_keysym(astrosession *s, int target, uint32_t keysym, int *stolen)
{
    if (stolen) *stolen = -1;
    if (target < 0 || target >= ASTRO_TARGET_COUNT) return;
    pthread_mutex_lock(&s_mtx);
    ensure_ready();
    if (keysym)
        for (int t = 0; t < ASTRO_TARGET_COUNT; t++)
            if (t != target && s_table[t].keysym == keysym) {
                s_table[t].keysym = 0;
                if (stolen) *stolen = t;
            }
    s_table[target].keysym = keysym;
    persist(s);
    pthread_mutex_unlock(&s_mtx);
}

void bindings_set_pad(astrosession *s, int target, int id, int *stolen)
{
    if (stolen) *stolen = -1;
    if (target < 0 || target >= ASTRO_TARGET_COUNT) return;
    pthread_mutex_lock(&s_mtx);
    ensure_ready();
    /* the KNOB target follows an axis; every other target takes a button or
     * (as a trigger-like switch) an axis */
    if (id >= 0)
        for (int t = 0; t < ASTRO_TARGET_COUNT; t++) {
            if (t == target || s_table[t].pad != id) continue;
            const int a = scope_of(t), b = scope_of(target);
            if (a == -1 || b == -1 || a == b) {
                s_table[t].pad = -1;
                if (stolen) *stolen = t;
            }
        }
    s_table[target].pad = id;
    persist(s);
    pthread_mutex_unlock(&s_mtx);
}

void bindings_clear(astrosession *s, int target)
{
    if (target < 0 || target >= ASTRO_TARGET_COUNT) return;
    pthread_mutex_lock(&s_mtx);
    ensure_ready();
    s_table[target].keysym = 0;
    s_table[target].pad = -1;
    persist(s);
    pthread_mutex_unlock(&s_mtx);
}

void bindings_reset_defaults(astrosession *s)
{
    pthread_mutex_lock(&s_mtx);
    ensure_ready();
    memcpy(s_table, s_defaults, sizeof s_table);
    persist(s);
    pthread_mutex_unlock(&s_mtx);
}

int astrosession_keysym_name(uint32_t keysym, char *dst, int dstsz)
{
    const char *name = NULL;
    if (!dst || dstsz <= 0) return 0;
    switch (keysym) {
    case 0: name = ""; break;
    case ASTROSESSION_KEYSYM_UP: name = "Up"; break;
    case ASTROSESSION_KEYSYM_DOWN: name = "Down"; break;
    case ASTROSESSION_KEYSYM_LEFT: name = "Left"; break;
    case ASTROSESSION_KEYSYM_RIGHT: name = "Right"; break;
    case ASTROSESSION_KEYSYM_ESCAPE: name = "Escape"; break;
    case ASTROSESSION_KEYSYM_RETURN: name = "Return"; break;
    case ASTROSESSION_KEYSYM_BACKSPACE: name = "Backspace"; break;
    case ASTROSESSION_KEYSYM_TAB: name = "Tab"; break;
    case ASTROSESSION_KEYSYM_SPACE: name = "Space"; break;
    case ASTROSESSION_KEYSYM_LSHIFT: name = "Left Shift"; break;
    case ASTROSESSION_KEYSYM_RSHIFT: name = "Right Shift"; break;
    case ASTROSESSION_KEYSYM_LCTRL: name = "Left Ctrl"; break;
    case ASTROSESSION_KEYSYM_RCTRL: name = "Right Ctrl"; break;
    case ASTROSESSION_KEYSYM_LALT: name = "Left Alt"; break;
    case ASTROSESSION_KEYSYM_RALT: name = "Right Alt"; break;
    case ASTROSESSION_KEYSYM_KP_ENTER: name = "Keypad Enter"; break;
    case ASTROSESSION_KEYSYM_KP_MULTIPLY: name = "Keypad *"; break;
    case ASTROSESSION_KEYSYM_KP_DIVIDE: name = "Keypad /"; break;
    case ASTROSESSION_KEYSYM_KP_PERIOD: name = "Keypad ."; break;
    case 0xffff: name = "Delete"; break;
    case 0xff63: name = "Insert"; break;
    case 0xff50: name = "Home"; break;
    case 0xff57: name = "End"; break;
    case 0xff55: name = "Page Up"; break;
    case 0xff56: name = "Page Down"; break;
    case 0xffe5: name = "Caps Lock"; break;
    case 0xffad: name = "Keypad -"; break;
    case 0xffab: name = "Keypad +"; break;
    default: break;
    }
    if (name) return snprintf(dst, (size_t)dstsz, "%s", name);
    if (keysym >= ASTROSESSION_KEYSYM_F1 && keysym <= ASTROSESSION_KEYSYM_F12)
        return snprintf(dst, (size_t)dstsz, "F%u", keysym - ASTROSESSION_KEYSYM_F1 + 1);
    if (keysym >= ASTROSESSION_KEYSYM_KP_0 && keysym <= ASTROSESSION_KEYSYM_KP_9)
        return snprintf(dst, (size_t)dstsz, "Keypad %u", keysym - ASTROSESSION_KEYSYM_KP_0);
    if (keysym >= 0x21 && keysym <= 0x7e) {
        char c = (char)keysym;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        return snprintf(dst, (size_t)dstsz, "%c", c);
    }
    if (keysym >= ASTROSESSION_KEYSYM_HID_BASE
        && keysym <= ASTROSESSION_KEYSYM_HID_BASE + ASTROSESSION_HID_USAGE_MAX) {
        const char *hid = astro_hid_usage_name(keysym - ASTROSESSION_KEYSYM_HID_BASE);
        if (hid) return snprintf(dst, (size_t)dstsz, "%s", hid);
        return snprintf(dst, (size_t)dstsz, "HID 0x%02X",
                        (unsigned)(keysym - ASTROSESSION_KEYSYM_HID_BASE));
    }
    return snprintf(dst, (size_t)dstsz, "Key 0x%X", keysym);
}
