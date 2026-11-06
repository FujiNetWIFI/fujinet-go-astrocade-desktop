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

#include <math.h>

#include "bindings.h"
#include "input.h"
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
    { SDL_GAMEPAD_BUTTON_GUIDE, "Guide" },
    { SDL_GAMEPAD_BUTTON_START, "Start" },
    { SDL_GAMEPAD_BUTTON_LEFT_STICK, "L3" },
    { SDL_GAMEPAD_BUTTON_RIGHT_STICK, "R3" },
    { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, "L1" },
    { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "R1" },
    { SDL_GAMEPAD_BUTTON_DPAD_UP, "D-Up" },
    { SDL_GAMEPAD_BUTTON_DPAD_DOWN, "D-Down" },
    { SDL_GAMEPAD_BUTTON_DPAD_LEFT, "D-Left" },
    { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, "D-Right" },
};
static const char *const k_axis_name[ASTRO_PAD_AXIS_COUNT] = {
    "Left Stick X", "Left Stick Y", "Right Stick X", "Right Stick Y",
    "Left Trigger", "Right Trigger",
};
static const SDL_GamepadAxis k_axis_sdl[ASTRO_PAD_AXIS_COUNT] = {
    SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX,
    SDL_GAMEPAD_AXIS_RIGHTY, SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
    SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
};

static pthread_t s_thread;
static volatile int s_running;
static astrosession *s_session;

/* Map-mode capture (guarded by the poll thread; simple flags suffice). */
static volatile int s_capture_armed;
static volatile int s_capture_have;
static volatile int s_capture_id;

/* the connected pads, for the Controllers window's list */
static pthread_mutex_t s_info_mtx = PTHREAD_MUTEX_INITIALIZER;
static char s_pad_name[MAX_PADS][64];
static int  s_pad_count;

/* per-pad edge state, so a held button fires its binding once */
typedef struct {
    SDL_Gamepad *gp;      /* a device SDL maps as a gamepad ... */
    SDL_Joystick *joy;    /* ... or any other joystick, read raw */
    SDL_JoystickID id;
    uint32_t btn_prev;   /* bitmask of buttons that were down last poll */
    uint32_t axis_prev;  /* axes that were past the capture threshold */
} pad_slot;

int astrosession_pad_input_name(int id, char *dst, int dstsz)
{
    const char *name = NULL;
    if (!dst || dstsz <= 0) return 0;
    if (id < 0) name = "";
    else if (id >= ASTRO_PAD_AXIS_BASE && id < ASTRO_PAD_AXIS_BASE + ASTRO_PAD_AXIS_COUNT)
        name = k_axis_name[id - ASTRO_PAD_AXIS_BASE];
    else
        for (unsigned i = 0; i < sizeof k_named / sizeof k_named[0]; i++)
            if (k_named[i].sdl == id) { name = k_named[i].name; break; }
    if (name) return snprintf(dst, (size_t)dstsz, "%s", name);
    return snprintf(dst, (size_t)dstsz, "Button %d", id);
}

int astrosession_gamepad_count(astrosession *s)
{
    (void)s;
    pthread_mutex_lock(&s_info_mtx);
    const int n = s_pad_count;
    pthread_mutex_unlock(&s_info_mtx);
    return n;
}

int astrosession_gamepad_name(astrosession *s, int idx, char *dst, int dstsz)
{
    (void)s;
    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    pthread_mutex_lock(&s_info_mtx);
    if (idx >= 0 && idx < s_pad_count)
        snprintf(dst, (size_t)dstsz, "%s", s_pad_name[idx]);
    pthread_mutex_unlock(&s_info_mtx);
    return (int)strlen(dst);
}

/* What one pad input does, accumulated for the pad's source. */
typedef struct {
    uint32_t keys;
    uint8_t hand, knobdir;
} acc;

static void apply_target(acc *a, int target, int down, int edge)
{
    if (target < 0 || !down) {
        /* a system action fires on the press edge only */
        return;
    }
    if (target >= ASTRO_TARGET_SYSACT && target < ASTRO_TARGET_PLAYERS) {
        if (edge)
            astrosession_sysaction_post(
                s_session, (astrosession_sysaction)(target - ASTRO_TARGET_SYSACT));
    } else if (target < ASTROSESSION_KEY_COUNT) {
        a->keys |= 1u << target;
    } else if (target < ASTRO_TARGET_COUNT) {
        const int act = (target - ASTRO_TARGET_PLAYERS) % ASTRO_ACT_PER_PLAYER;
        if (act <= ASTRO_ACT_TRIGGER) a->hand |= (uint8_t)(1u << act);
        else if (act == ASTRO_ACT_KNOB_DEC) a->knobdir |= 1;
        else if (act == ASTRO_ACT_KNOB_INC) a->knobdir |= 2;
    }
}

/* One poll's worth of a device, whichever API it came from. A raw joystick
 * (one SDL has no gamepad mapping for -- an Astro-daptor, a generic USB
 * adapter) is mapped like a gamepad: its buttons 0-31 are the pad buttons
 * (button 0 = A = the default trigger), hat 0 the D-pad, axes 0-5 the six
 * pad axes (0/1 steer, 2 is the default knob). */
typedef struct {
    uint32_t buttons;           /* bit n = pad button n down */
    float axis[ASTRO_PAD_AXIS_COUNT];
} pad_input;

static float clampf(float v) { return v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v; }

static void read_gamepad(SDL_Gamepad *gp, pad_input *in)
{
    memset(in, 0, sizeof *in);
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && b < 32; b++)
        if (SDL_GetGamepadButton(gp, (SDL_GamepadButton)b))
            in->buttons |= 1u << b;
    for (int a = 0; a < ASTRO_PAD_AXIS_COUNT; a++)
        in->axis[a] = clampf(SDL_GetGamepadAxis(gp, k_axis_sdl[a]) / AXIS_MAX);
}

static void read_joystick(SDL_Joystick *j, pad_input *in)
{
    const int nb = SDL_GetNumJoystickButtons(j), na = SDL_GetNumJoystickAxes(j);
    memset(in, 0, sizeof *in);
    for (int b = 0; b < nb && b < 32; b++)
        if (SDL_GetJoystickButton(j, b))
            in->buttons |= 1u << b;
    if (SDL_GetNumJoystickHats(j) > 0) {
        const Uint8 h = SDL_GetJoystickHat(j, 0);
        if (h & SDL_HAT_UP)    in->buttons |= 1u << SDL_GAMEPAD_BUTTON_DPAD_UP;
        if (h & SDL_HAT_DOWN)  in->buttons |= 1u << SDL_GAMEPAD_BUTTON_DPAD_DOWN;
        if (h & SDL_HAT_LEFT)  in->buttons |= 1u << SDL_GAMEPAD_BUTTON_DPAD_LEFT;
        if (h & SDL_HAT_RIGHT) in->buttons |= 1u << SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    }
    for (int a = 0; a < na && a < ASTRO_PAD_AXIS_COUNT; a++)
        in->axis[a] = clampf(SDL_GetJoystickAxis(j, a) / AXIS_MAX);
}

static void apply_pad(pad_slot *slot, int player)
{
    pad_input in;
    acc a = { 0, 0, 0 };

    if (slot->gp) read_gamepad(slot->gp, &in);
    else          read_joystick(slot->joy, &in);

    /* ---- steering: the D-pad and the left stick always move the stick ---- */
    int up = (in.buttons >> SDL_GAMEPAD_BUTTON_DPAD_UP) & 1;
    int down = (in.buttons >> SDL_GAMEPAD_BUTTON_DPAD_DOWN) & 1;
    int left = (in.buttons >> SDL_GAMEPAD_BUTTON_DPAD_LEFT) & 1;
    int right = (in.buttons >> SDL_GAMEPAD_BUTTON_DPAD_RIGHT) & 1;
    const float lx = in.axis[0], ly = in.axis[1];
    if (ly < -STICK_DEADZONE) up = 1;
    if (ly > STICK_DEADZONE) down = 1;
    if (lx < -STICK_DEADZONE) left = 1;
    if (lx > STICK_DEADZONE) right = 1;
    if (up) a.hand |= ASTROSESSION_HANDLE_UP;
    if (down) a.hand |= ASTROSESSION_HANDLE_DOWN;
    if (left) a.hand |= ASTROSESSION_HANDLE_LEFT;
    if (right) a.hand |= ASTROSESSION_HANDLE_RIGHT;

    /* ---- buttons -> bindings ---- */
    const uint32_t now = in.buttons;
    const uint32_t pressed = now & ~slot->btn_prev;   /* rising edges */
    slot->btn_prev = now;
    for (int b = 0; b < 32; b++) {
        const int is_down = (now >> b) & 1, is_edge = (pressed >> b) & 1;
        if (is_edge && s_capture_armed) {
            s_capture_id = b;
            s_capture_have = 1;
            s_capture_armed = 0;
            continue;                     /* recorded for Map mode, not injected */
        }
        apply_target(&a, bindings_target_for_pad(player, b), is_down, is_edge);
    }

    /* ---- axes: a digital target fires at +50%; the knob follows one ---- */
    uint32_t over = 0;
    for (int ax = 0; ax < ASTRO_PAD_AXIS_COUNT; ax++) {
        const float v = in.axis[ax];
        const int id = ASTRO_PAD_AXIS_BASE + ax;
        if (fabsf(v) > 0.6f) over |= 1u << ax;
        if (s_capture_armed && ((over >> ax) & 1) && !((slot->axis_prev >> ax) & 1)) {
            s_capture_id = id;
            s_capture_have = 1;
            s_capture_armed = 0;
            continue;
        }
        const int t = bindings_target_for_pad(player, id);
        if (t >= ASTRO_TARGET_PLAYERS &&
            (t - ASTRO_TARGET_PLAYERS) % ASTRO_ACT_PER_PLAYER == ASTRO_ACT_KNOB)
            continue;                     /* the knob axis, handled below */
        const int is_down = v > 0.5f;
        apply_target(&a, t, is_down, is_down && !((slot->axis_prev >> ax) & 1));
    }
    slot->axis_prev = over;

    astro_input_pad_state(player, a.keys, player, a.hand, a.knobdir);

    /* ---- the knob: an absolute axis (a gamepad trigger sweeps 0..1; a
     * stick, or any axis of a raw joystick, -1..1) ---- */
    const int kax = bindings_knob_axis(player);
    if (kax >= 0) {
        const int ax = kax - ASTRO_PAD_AXIS_BASE;
        const float v = in.axis[ax];
        const float u = (ax >= 4 && slot->gp) ? v : (v + 1.0f) * 0.5f;
        astro_input_pad_knob(player, player, (int)(u * 255.0f + 0.5f));
    }
}

/* A joystick SDL doesn't classify as a gamepad (SDL_IsGamepad() false) is
 * invisible to SDL_GetGamepads(); the poll thread opens those as raw joysticks
 * (see read_joystick). The
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
                "astro_gamepad: joystick '%s' (GUID %s) has no gamepad mapping; "
                "using it as a raw joystick (button 0 = trigger, axes 0/1 = "
                "stick, axis 2 = knob)\n",
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

    int npads = 0;
    Uint64 last_tick = 0;
    while (s_running) {
        SDL_UpdateGamepads();
        log_unrecognized_joysticks(logged_unrecognized, &logged_unrecognized_count);

        /* refresh the open set: assign the first MAX_PADS gamepads to slots */
        int count = 0;
        SDL_JoystickID *ids = SDL_GetJoysticks(&count);   /* gamepads and raw joysticks */
        for (int i = 0; i < MAX_PADS; i++) {
            SDL_JoystickID want = (i < count) ? ids[i] : 0;
            if (slots[i].id != want) {
                if (slots[i].gp) { SDL_CloseGamepad(slots[i].gp); slots[i].gp = NULL; }
                if (slots[i].joy) { SDL_CloseJoystick(slots[i].joy); slots[i].joy = NULL; }
                astro_input_pad_clear(i);
                slots[i].id = want;
                slots[i].btn_prev = 0;
                slots[i].axis_prev = 0;
                if (want) {
                    if (SDL_IsGamepad(want))
                        slots[i].gp = SDL_OpenGamepad(want);
                    if (!slots[i].gp)
                        slots[i].joy = SDL_OpenJoystick(want);
                }
            }
            if (slots[i].gp || slots[i].joy)
                apply_pad(&slots[i], i);
        }
        SDL_free(ids);
        npads = count;
        pthread_mutex_lock(&s_info_mtx);
        s_pad_count = 0;
        for (int i = 0; i < MAX_PADS; i++)
            if (slots[i].gp || slots[i].joy) {
                const char *nm = slots[i].gp ? SDL_GetGamepadName(slots[i].gp)
                                             : SDL_GetJoystickName(slots[i].joy);
                snprintf(s_pad_name[s_pad_count], sizeof s_pad_name[0], "%s",
                         nm ? nm : "Gamepad");
                s_pad_count++;
            }
        pthread_mutex_unlock(&s_info_mtx);

        /* ~60 Hz while a pad is attached; with none there is only hotplug to
         * notice, which can wait a tenth of a second. (SDL_WaitEventTimeout
         * is no help: SDL polls inside it at about 1 kHz.) */
        const int fast = npads > 0 || astro_input_knob_active();
        SDL_Delay(fast ? 16 : 100);
        const Uint64 tnow = SDL_GetTicks();
        if (last_tick) {
            int dt = (int)(tnow - last_tick);
            astro_input_tick(dt > 100 ? 100 : dt);
        }
        last_tick = tnow;
    }

    for (int i = 0; i < MAX_PADS; i++) {
        if (slots[i].gp)
            SDL_CloseGamepad(slots[i].gp);
        if (slots[i].joy)
            SDL_CloseJoystick(slots[i].joy);
        astro_input_pad_clear(i);
    }
    pthread_mutex_lock(&s_info_mtx);
    s_pad_count = 0;
    pthread_mutex_unlock(&s_info_mtx);
    return NULL;
}

int astro_gamepad_start(astrosession *s)
{
    if (s_running)
        return 0;
    s_session = s;
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    /* the poll thread is not the focused window: keep reading pads when the
     * app is in the background (the debugger or another window has focus) */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK))
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
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK);
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
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK);
    s_session = NULL;
}

/* ---- Map-mode capture (public via astrosession.h) ---- */

void astrosession_gamepad_capture_begin(astrosession *s)
{
    (void)s;
    s_capture_have = 0;
    s_capture_armed = 1;
}

int astrosession_gamepad_capture_poll(astrosession *s, int *id)
{
    (void)s;
    if (!s_capture_have)
        return 0;
    s_capture_have = 0;
    if (id)
        *id = s_capture_id;
    return 1;
}

void astrosession_gamepad_capture_cancel(astrosession *s)
{
    (void)s;
    s_capture_armed = 0;
    s_capture_have = 0;
}
