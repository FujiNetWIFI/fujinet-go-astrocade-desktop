/*
 * gamepad_sdl.c -- see gamepad_sdl.h.
 *
 * One poll thread reads every open gamepad ~125 times a second. Gamepads are
 * assigned to players 0-3 by the order SDL enumerates them. Buttons resolve
 * through the bindings table (a keypad key is set directly; a system action is
 * POSTED to the session -- firing one joins threads, which the poll thread
 * must never do). While Map-mode capture is armed a button press is recorded
 * instead of injected.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "bindings.h"
#include "gamecontrollerdb_embedded.h"
#include "gamepad_sdl.h"

#define MAX_PADS 4
#define AXIS_MAX 32767.0f
#define STICK_DEADZONE 0.35f

/* the SDL buttons this app names, for Map-mode labels */
static const struct { int sdl; const char *name; } k_named[] = {
    { SDL_GAMEPAD_BUTTON_SOUTH, "A" },
    { SDL_GAMEPAD_BUTTON_EAST, "B" },
    { SDL_GAMEPAD_BUTTON_WEST, "X" },
    { SDL_GAMEPAD_BUTTON_NORTH, "Y" },
    { SDL_GAMEPAD_BUTTON_BACK, "Back" },
    { SDL_GAMEPAD_BUTTON_START, "Start" },
    { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, "L1" },
    { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "R1" },
    { SDL_GAMEPAD_BUTTON_DPAD_UP, "D-Up" },
    { SDL_GAMEPAD_BUTTON_DPAD_DOWN, "D-Down" },
    { SDL_GAMEPAD_BUTTON_DPAD_LEFT, "D-Left" },
    { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, "D-Right" },
};

static pthread_t s_thread;
static volatile int s_running;
static astrosession *s_session;

/* Map-mode capture (guarded by the poll thread; simple flags suffice). */
static volatile int s_capture_armed;
static volatile int s_capture_have;
static volatile int s_capture_button;

/* per-pad edge state, so a held button fires its binding once */
typedef struct {
    SDL_Gamepad *gp;
    SDL_JoystickID id;
    uint32_t btn_prev;   /* bitmask of buttons that were down last poll */
} pad_slot;

const char *astro_gamepad_button_name(int sdl_button)
{
    for (unsigned i = 0; i < sizeof k_named / sizeof k_named[0]; i++)
        if (k_named[i].sdl == sdl_button)
            return k_named[i].name;
    return NULL;
}

static void apply_pad(pad_slot *slot, int player)
{
    SDL_Gamepad *gp = slot->gp;

    /* ---- handle: D-pad OR left stick, plus the South button as trigger ---- */
    int up = SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_UP);
    int down = SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    int left = SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    int right = SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);

    float lx = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX) / AXIS_MAX;
    float ly = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY) / AXIS_MAX;
    if (ly < -STICK_DEADZONE) up = 1;
    if (ly > STICK_DEADZONE) down = 1;
    if (lx < -STICK_DEADZONE) left = 1;
    if (lx > STICK_DEADZONE) right = 1;

    uint8_t mask = 0;
    if (up) mask |= ASTROSESSION_HANDLE_UP;
    if (down) mask |= ASTROSESSION_HANDLE_DOWN;
    if (left) mask |= ASTROSESSION_HANDLE_LEFT;
    if (right) mask |= ASTROSESSION_HANDLE_RIGHT;
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_SOUTH))
        mask |= ASTROSESSION_HANDLE_TRIGGER;
    astrosession_handle_set(s_session, player, mask);

    /* ---- knob (paddle): right stick X mapped to 0-255 absolute ---- */
    float rx = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTX) / AXIS_MAX;
    int knob = (int)((rx + 1.0f) * 127.5f);
    if (knob < 0) knob = 0; else if (knob > 255) knob = 255;
    astrosession_knob_set(s_session, player, (uint8_t)knob);

    /* ---- buttons -> bindings ----
     * A keypad key follows the button's level (held down for as long as the
     * button is); a system action fires once on the press edge (posted, not
     * fired, since this is the poll thread). Map-mode capture takes the first
     * fresh press instead of injecting it. */
    uint32_t now = 0;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && b < 32; b++)
        if (SDL_GetGamepadButton(gp, (SDL_GamepadButton)b))
            now |= (1u << b);
    const uint32_t pressed = now & ~slot->btn_prev;   /* rising edges */
    slot->btn_prev = now;

    for (int b = 0; b < 32; b++) {
        const int is_down = (now >> b) & 1;
        const int is_edge = (pressed >> b) & 1;

        if (is_edge && s_capture_armed) {
            s_capture_button = b;
            s_capture_have = 1;
            s_capture_armed = 0;
            continue;                     /* recorded for Map mode, not injected */
        }
        astro_mapping m = bindings_resolve_button(b);
        if (m.kind == ASTRO_MAP_KEY)
            astrosession_keypad_set(s_session, (astrosession_key)m.value, is_down);
        else if (m.kind == ASTRO_MAP_SYSACT && is_edge)
            astrosession_sysaction_post(s_session, (astrosession_sysaction)m.value);
    }
}

/* A joystick SDL doesn't classify as a gamepad (SDL_IsGamepad() false) is
 * invisible to SDL_GetGamepads() and so silently drives nothing -- the
 * bundled community mapping DB (astro_gamecontrollerdb_text, loaded in
 * astro_gamepad_start()) covers most of these, but if one still slips
 * through, log its name/GUID once so it's diagnosable instead of doing
 * nothing with no explanation. */
#define MAX_LOGGED_UNRECOGNIZED 8
static void log_unrecognized_joysticks(SDL_JoystickID *seen, int *seen_count)
{
    int jcount = 0;
    SDL_JoystickID *jids = SDL_GetJoysticks(&jcount);
    for (int i = 0; i < jcount; i++) {
        SDL_JoystickID id = jids[i];
        if (SDL_IsGamepad(id))
            continue;
        int already = 0;
        for (int j = 0; j < *seen_count; j++)
            if (seen[j] == id) { already = 1; break; }
        if (already)
            continue;
        if (*seen_count < MAX_LOGGED_UNRECOGNIZED)
            seen[(*seen_count)++] = id;

        const char *name = SDL_GetJoystickNameForID(id);
        char guid_str[64];
        SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid_str, sizeof guid_str);
        fprintf(stderr,
                "astro_gamepad: joystick '%s' (GUID %s) is connected but not "
                "recognized as a gamepad; no mapping available for it\n",
                name ? name : "?", guid_str);
    }
    SDL_free(jids);
}

static void *poll_thread(void *arg)
{
    (void)arg;
    pad_slot slots[MAX_PADS];
    memset(slots, 0, sizeof slots);
    SDL_JoystickID logged_unrecognized[MAX_LOGGED_UNRECOGNIZED];
    int logged_unrecognized_count = 0;

    while (s_running) {
        SDL_UpdateGamepads();
        log_unrecognized_joysticks(logged_unrecognized, &logged_unrecognized_count);

        /* refresh the open set: assign the first MAX_PADS gamepads to slots */
        int count = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&count);
        for (int i = 0; i < MAX_PADS; i++) {
            SDL_JoystickID want = (i < count) ? ids[i] : 0;
            if (slots[i].id != want) {
                if (slots[i].gp) { SDL_CloseGamepad(slots[i].gp); slots[i].gp = NULL; }
                slots[i].id = want;
                slots[i].btn_prev = 0;
                if (want)
                    slots[i].gp = SDL_OpenGamepad(want);
            }
            if (slots[i].gp)
                apply_pad(&slots[i], i);
        }
        SDL_free(ids);

        SDL_Delay(8);   /* ~125 Hz */
    }

    for (int i = 0; i < MAX_PADS; i++)
        if (slots[i].gp)
            SDL_CloseGamepad(slots[i].gp);
    return NULL;
}

int astro_gamepad_start(astrosession *s)
{
    if (s_running)
        return 0;
    s_session = s;
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        return -1;

    /* layer the community mapping DB over SDL's own bundled one -- best
     * effort, a controller SDL already knows about is unaffected either way */
    SDL_IOStream *db = SDL_IOFromConstMem(astro_gamecontrollerdb_text,
                                          astro_gamecontrollerdb_text_size);
    if (!db || SDL_AddGamepadMappingsFromIO(db, true) < 0)
        fprintf(stderr, "astro_gamepad: failed to load the bundled controller "
                        "DB: %s\n", SDL_GetError());

    s_running = 1;
    if (pthread_create(&s_thread, NULL, poll_thread, NULL) != 0) {
        s_running = 0;
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        return -1;
    }
    return 0;
}

void astro_gamepad_stop(void)
{
    if (!s_running)
        return;
    s_running = 0;
    pthread_join(s_thread, NULL);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    s_session = NULL;
}

/* ---- Map-mode capture (public via astrosession.h) ---- */

void astrosession_gamepad_capture_begin(astrosession *s)
{
    (void)s;
    s_capture_have = 0;
    s_capture_armed = 1;
}

int astrosession_gamepad_capture_poll(astrosession *s, int *button)
{
    (void)s;
    if (!s_capture_have)
        return 0;
    s_capture_have = 0;
    if (button)
        *button = s_capture_button;
    return 1;
}

void astrosession_gamepad_capture_cancel(astrosession *s)
{
    (void)s;
    s_capture_armed = 0;
    s_capture_have = 0;
}
