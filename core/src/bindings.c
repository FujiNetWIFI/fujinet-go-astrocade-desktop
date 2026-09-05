/*
 * bindings.c -- see bindings.h.
 *
 * Two tables: s_defaults (computed once, never mutated) and s_table (live).
 * Diffing them makes persistence (only non-default entries written) and reset
 * trivial, the same shape as the Intv port's bindings.c. A target holds at
 * most one keyboard keysym and one gamepad button; binding one steals it from
 * whoever held it, so no two targets ever fire from the same input.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bindings.h"

typedef struct {
    int keysym;     /* ASTROSESSION_KEYSYM_* / ASCII, 0 = unbound */
    int button;     /* SDL_GamepadButton, -1 = unbound */
} slot;

static slot s_defaults[ASTRO_TARGET_COUNT];
static slot s_table[ASTRO_TARGET_COUNT];
static int  s_ready;

/* the 5 hand-controller actions, in ASTRO_TARGET_HANDLE order */
static const uint8_t k_handle_bit[ASTRO_HANDLE_ACTION_COUNT] = {
    ASTROSESSION_HANDLE_UP, ASTROSESSION_HANDLE_DOWN,
    ASTROSESSION_HANDLE_LEFT, ASTROSESSION_HANDLE_RIGHT,
    ASTROSESSION_HANDLE_TRIGGER,
};

static astro_map_kind target_kind(int target, int *value)
{
    if (target >= ASTRO_TARGET_HANDLE && target < ASTRO_TARGET_COUNT) {
        *value = k_handle_bit[target - ASTRO_TARGET_HANDLE];
        return ASTRO_MAP_HANDLE;
    }
    if (target >= ASTRO_TARGET_SYSACT && target < ASTRO_TARGET_HANDLE) {
        *value = target - ASTRO_TARGET_SYSACT;
        return ASTRO_MAP_SYSACT;
    }
    if (target >= 0 && target < ASTROSESSION_KEY_COUNT) {
        *value = target;
        return ASTRO_MAP_KEY;
    }
    *value = 0;
    return ASTRO_MAP_NONE;
}

/* The default keyboard keysym for a keypad key: the inverse of
 * astro_keymap's astrosession_key_from_keysym, picking one representative
 * keysym per key. */
static int default_keysym_for_key(int key)
{
    switch (key) {
    case ASTROSESSION_KEY_C:     return 'c';
    /* 'u'/'d', not the arrows -- the arrows drive the hand controller
     * (ASTRO_TARGET_HANDLE) by default instead; see compute_defaults(). */
    case ASTROSESSION_KEY_UP:    return 'u';
    case ASTROSESSION_KEY_DOWN:  return 'd';
    case ASTROSESSION_KEY_PCT:   return '%';
    case ASTROSESSION_KEY_MR:    return 'r';
    case ASTROSESSION_KEY_MS:    return 's';
    case ASTROSESSION_KEY_CH:    return 'h';
    case ASTROSESSION_KEY_DIV:   return '/';
    case ASTROSESSION_KEY_7:     return '7';
    case ASTROSESSION_KEY_8:     return '8';
    case ASTROSESSION_KEY_9:     return '9';
    case ASTROSESSION_KEY_MUL:   return '*';
    case ASTROSESSION_KEY_4:     return '4';
    case ASTROSESSION_KEY_5:     return '5';
    case ASTROSESSION_KEY_6:     return '6';
    case ASTROSESSION_KEY_MINUS: return '-';
    case ASTROSESSION_KEY_1:     return '1';
    case ASTROSESSION_KEY_2:     return '2';
    case ASTROSESSION_KEY_3:     return '3';
    case ASTROSESSION_KEY_PLUS:  return '+';
    case ASTROSESSION_KEY_CE:    return 'e';
    case ASTROSESSION_KEY_0:     return '0';
    case ASTROSESSION_KEY_DOT:   return '.';
    case ASTROSESSION_KEY_EQ:    return '=';
    default:                     return 0;
    }
}

static void compute_defaults(void)
{
    for (int t = 0; t < ASTRO_TARGET_COUNT; t++) {
        s_defaults[t].keysym = 0;
        s_defaults[t].button = -1;
    }
    for (int k = 0; k < ASTROSESSION_KEY_COUNT; k++)
        s_defaults[k].keysym = default_keysym_for_key(k);

    /* system actions: Reset Game on Backspace (Reset to CONFIG stays menu-
     * only by default -- it is heavyweight, so no stray key should hit it) */
    s_defaults[ASTRO_TARGET_SYSACT + ASTROSESSION_SYSACT_RESET_GAME].keysym =
        ASTROSESSION_KEYSYM_BACKSPACE;

    /* hand controller (player 0): arrows move, Space fires -- a keyboard
     * fallback for when no gamepad is connected. Space is parsed by every
     * frontend's key translator but otherwise unused. */
    s_defaults[ASTRO_TARGET_HANDLE + 0].keysym = ASTROSESSION_KEYSYM_UP;
    s_defaults[ASTRO_TARGET_HANDLE + 1].keysym = ASTROSESSION_KEYSYM_DOWN;
    s_defaults[ASTRO_TARGET_HANDLE + 2].keysym = ASTROSESSION_KEYSYM_LEFT;
    s_defaults[ASTRO_TARGET_HANDLE + 3].keysym = ASTROSESSION_KEYSYM_RIGHT;
    s_defaults[ASTRO_TARGET_HANDLE + 4].keysym = ASTROSESSION_KEYSYM_SPACE;

    /* default gamepad buttons for the digits are left unbound; the frontend's
     * gamepad handling drives the hand controllers, not the keypad. */
}

static const char *k_key_label[ASTROSESSION_KEY_COUNT] = {
    "Keypad C", "Keypad Up", "Keypad Down", "Keypad %",
    "Keypad MR", "Keypad MS", "Keypad CH", "Keypad \xC3\xB7",
    "Keypad 7", "Keypad 8", "Keypad 9", "Keypad \xC3\x97",
    "Keypad 4", "Keypad 5", "Keypad 6", "Keypad \xE2\x88\x92",
    "Keypad 1", "Keypad 2", "Keypad 3", "Keypad +",
    "Keypad CE", "Keypad 0", "Keypad .", "Keypad =",
};

static const char *k_handle_label[ASTRO_HANDLE_ACTION_COUNT] = {
    "Move Up", "Move Down", "Move Left", "Move Right", "Trigger",
};

const char *bindings_target_label(int target)
{
    if (target >= ASTRO_TARGET_HANDLE && target < ASTRO_TARGET_COUNT)
        return k_handle_label[target - ASTRO_TARGET_HANDLE];
    int v;
    astro_map_kind k = target_kind(target, &v);
    if (k == ASTRO_MAP_KEY)
        return k_key_label[v];
    if (k == ASTRO_MAP_SYSACT)
        return v == ASTROSESSION_SYSACT_RESET_GAME ? "Reset Game"
                                                   : "Reset to CONFIG";
    return "?";
}

static void ensure_ready(void)
{
    if (s_ready)
        return;
    compute_defaults();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_ready = 1;
}

/* ---- persistence: "<target>.k:<keysym>" / ".p:<button>", ;-separated ---- */

static void persist(astrosession *s)
{
    char buf[2048];
    size_t n = 0;
    buf[0] = '\0';
    for (int t = 0; t < ASTRO_TARGET_COUNT; t++) {
        if (s_table[t].keysym != s_defaults[t].keysym)
            n += (size_t)snprintf(buf + n, sizeof buf - n, "%d.k:%d;",
                                  t, s_table[t].keysym);
        if (n < sizeof buf && s_table[t].button != s_defaults[t].button)
            n += (size_t)snprintf(buf + n, sizeof buf - n, "%d.p:%d;",
                                  t, s_table[t].button);
        if (n >= sizeof buf)
            break;
    }
    astrosession_set_str(s, "bindings", buf);
}

static void load_overrides(astrosession *s)
{
    const char *str = astrosession_get_str(s, "bindings", "");
    if (!str || !*str)
        return;
    const char *p = str;
    while (*p) {
        int target = 0, val = 0;
        char kind = 0;
        /* parse "<target>.<k|p>:<val>" */
        if (sscanf(p, "%d.%c:%d", &target, &kind, &val) == 3 &&
            target >= 0 && target < ASTRO_TARGET_COUNT) {
            if (kind == 'k') s_table[target].keysym = val;
            else if (kind == 'p') s_table[target].button = val;
        }
        const char *semi = strchr(p, ';');
        if (!semi) break;
        p = semi + 1;
    }
}

void bindings_init(astrosession *s)
{
    ensure_ready();
    memcpy(s_table, s_defaults, sizeof s_table);
    load_overrides(s);
}

astro_mapping bindings_resolve_keysym(int keysym)
{
    astro_mapping m = { ASTRO_MAP_NONE, 0 };
    ensure_ready();
    if (keysym == 0)
        return m;
    for (int t = 0; t < ASTRO_TARGET_COUNT; t++)
        if (s_table[t].keysym == keysym) {
            int v;
            m.kind = target_kind(t, &v);
            m.value = v;
            return m;
        }
    return m;
}

astro_mapping bindings_resolve_button(int button)
{
    astro_mapping m = { ASTRO_MAP_NONE, 0 };
    ensure_ready();
    if (button < 0)
        return m;
    for (int t = 0; t < ASTRO_TARGET_COUNT; t++)
        if (s_table[t].button == button) {
            int v;
            m.kind = target_kind(t, &v);
            m.value = v;
            return m;
        }
    return m;
}

int bindings_target_keysym(int target)
{
    ensure_ready();
    if (target < 0 || target >= ASTRO_TARGET_COUNT) return 0;
    return s_table[target].keysym;
}

int bindings_target_button(int target)
{
    ensure_ready();
    if (target < 0 || target >= ASTRO_TARGET_COUNT) return -1;
    return s_table[target].button;
}

void bindings_set_keysym(astrosession *s, int target, int keysym,
                         int *steal_from_out)
{
    ensure_ready();
    if (steal_from_out) *steal_from_out = -1;
    if (target < 0 || target >= ASTRO_TARGET_COUNT) return;
    if (keysym != 0)
        for (int t = 0; t < ASTRO_TARGET_COUNT; t++)
            if (t != target && s_table[t].keysym == keysym) {
                s_table[t].keysym = 0;
                if (steal_from_out) *steal_from_out = t;
            }
    s_table[target].keysym = keysym;
    persist(s);
}

void bindings_set_button(astrosession *s, int target, int button,
                         int *steal_from_out)
{
    ensure_ready();
    if (steal_from_out) *steal_from_out = -1;
    if (target < 0 || target >= ASTRO_TARGET_COUNT) return;
    if (button >= 0)
        for (int t = 0; t < ASTRO_TARGET_COUNT; t++)
            if (t != target && s_table[t].button == button) {
                s_table[t].button = -1;
                if (steal_from_out) *steal_from_out = t;
            }
    s_table[target].button = button;
    persist(s);
}

void bindings_reset_defaults(astrosession *s)
{
    ensure_ready();
    memcpy(s_table, s_defaults, sizeof s_table);
    persist(s);
}
