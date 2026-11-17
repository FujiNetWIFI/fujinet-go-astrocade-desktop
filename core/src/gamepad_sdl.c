/*
 * gamepad_sdl -- SDL3 gamepads (and unmapped joysticks) as Astrocade hand
 * controllers, on their own polling thread.
 *
 * A polling thread rather than SDL's event queue on the UI thread: this
 * app's video belongs to GTK/Qt/AppKit/Win32, not SDL, and only the gamepad
 * subsystem is initialised here -- never SDL video. Devices are added and
 * removed as they appear on the system: SDL posts the JOYSTICK_ and
 * GAMEPAD_ add/remove pairs and both are taken, with idempotent open/close,
 * so neither kind of device can be missed. A device SDL has no gamepad
 * mapping for (a plain HID adapter, the Astro-daptor) is opened as a raw
 * joystick instead: its buttons and hat positions are bindable in the raw and
 * hat bands, and while a raw button has no binding its first four act as
 * South/East/West/North, so button 0 is still the trigger. A bundled community
 * mapping DB (gamecontrollerdb_embedded.h) is loaded on start to cover
 * controllers SDL's own bundled mappings don't recognize.
 *
 * What a pad plays is one hand controller: the first unassigned pad plays
 * player 1, the next player 2, and so on, unless pads are assigned to players
 * explicitly. Every button goes through the bindings table: by default the
 * D-pad steers, South pulls the trigger and the right stick's X turns the
 * knob; any button may be bound to a keypad key, a direction, the trigger, a
 * knob key or a console action instead. The left analog stick is always the
 * stick: an eight-way switch with hysteresis (in at half deflection, out at
 * 0.35), and so is a raw joystick's hat 0. The ways of holding an input -- a
 * pad button, the analog stick, the keyboard -- are separate sources in
 * input.c, so letting go of one never releases another. Ported from the
 * Channel F and ADAM siblings' gamepad_sdl.c.
 *
 * An explicit assignment is remembered (settings key "pad_assign", see
 * pad_assign.h): a pad that is unplugged and plugged back in, or is there on
 * the next run, gets it back -- matched by its SDL GUID, else by its name. Two
 * identical pads share one entry; the first to connect gets it. Every
 * connect/disconnect is reported (astrosession_gamepad_last_event) for the
 * frontend's toast.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "bindings.h"
#include "gamecontrollerdb_embedded.h"
#include "gamepad_sdl.h"
#include "input.h"
#include "pad_assign.h"
#include "session_internal.h"

#define MAX_PADS ASTRO_MAX_PADS
#define MAX_HATS 4
#define TRIGGER_ON 0.5f
#define AXIS_CAPTURE 0.6f

static const char *const k_axis_name[ASTRO_PAD_AXIS_COUNT] = {
    "Left Stick X", "Left Stick Y", "Right Stick X", "Right Stick Y",
    "Left Trigger", "Right Trigger",
};
static const SDL_GamepadAxis k_axis_sdl[ASTRO_PAD_AXIS_COUNT] = {
    SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX,
    SDL_GAMEPAD_AXIS_RIGHTY, SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
    SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
};

typedef struct {
    SDL_JoystickID devid;
    int owner;                /* the input.c source number for this pad: stable
                               * while it is connected, whatever its slot */
    SDL_Gamepad *handle;      /* mapped gamepad, or NULL */
    SDL_Joystick *joy;        /* raw joystick when not a gamepad */
    int assign;               /* explicit player, -1 = automatic */
    int by_name;              /* assign was restored from a name-only match
                               * (the entry's GUID differs): an exact-GUID
                               * pad may take it over */
    int player;               /* the player it drove on the last poll, or -1 */
    int stick_dir;            /* left stick's four-way state (hysteresis) */
    uint8_t sent[ASTRO_ACT_PER_PLAYER]; /* actions the analog stick / hat hold */
    /* What each button's press asserted (target + 1, 0 = nothing), so its
     * release lets go of exactly that even if the binding changed meanwhile,
     * and a pad that goes away drops what it held. */
    int16_t btn_target[256];
    SDL_GUID guid;
    char guidstr[ASTRO_PAD_GUID_MAX];   /* guid as hex, for the pad_assign table */
    char name[ASTRO_PAD_NAME_MAX];
    int trigger_down[2];
    uint32_t axis_over;       /* axes past the capture threshold last poll */
    Uint8 last_hat[MAX_HATS];
} pad_slot;

typedef struct {
    struct astrosession *session;
    pthread_t thread;
    pthread_mutex_t lock;
    int running;
    atomic_int stop_requested;
    pad_slot pads[MAX_PADS];
    int npads;
    unsigned generation;
    int quiet_scan;           /* thread only: the first scan of a restarted
                               * session re-finds pads without announcing them */
    /* Map-mode capture */
    int cap_active, cap_axis, cap_have, cap_result;
    /* the remembered explicit assignments (the "pad_assign" setting), for
     * pads that are away as well as those here; guarded by lock */
    astro_pad_table table;
    char event[128];          /* the last hot-plug event, for a toast */
} gamepad_state;

/* One state for the process (the machine is a process singleton). The lock
 * is static so the public getters are safe whether or not the thread runs;
 * the generation and the last event carry across a restart of the thread, so
 * a frontend's baseline never goes stale, and the session remembers that the
 * thread has run (pad_restarted) so a restart's first scan finds the same pads
 * again without announcing them. */
static gamepad_state g = { .lock = PTHREAD_MUTEX_INITIALIZER };

/* ---- pure helpers ----------------------------------------------------------- */

unsigned astro_acts_from_dir(int dir)
{
    unsigned acts = 0;

    if ((dir & ASTRO_DIR_UP) && !(dir & ASTRO_DIR_DOWN)) acts |= 1u << ASTRO_ACT_UP;
    if ((dir & ASTRO_DIR_DOWN) && !(dir & ASTRO_DIR_UP)) acts |= 1u << ASTRO_ACT_DOWN;
    if ((dir & ASTRO_DIR_LEFT) && !(dir & ASTRO_DIR_RIGHT)) acts |= 1u << ASTRO_ACT_LEFT;
    if ((dir & ASTRO_DIR_RIGHT) && !(dir & ASTRO_DIR_LEFT)) acts |= 1u << ASTRO_ACT_RIGHT;
    return acts;
}

int astro_dir_from_stick(float xval, float yval, int prev, float enter, float leave)
{
    int dir = 0;
    /* each axis independently, so diagonals are two bits and a stick that
     * is exactly horizontal never picks up a vertical bit from noise */
    if (xval <= -enter || (xval <= -leave && (prev & ASTRO_DIR_LEFT))) dir |= ASTRO_DIR_LEFT;
    if (xval >=  enter || (xval >=  leave && (prev & ASTRO_DIR_RIGHT))) dir |= ASTRO_DIR_RIGHT;
    if (yval <= -enter || (yval <= -leave && (prev & ASTRO_DIR_UP))) dir |= ASTRO_DIR_UP;
    if (yval >=  enter || (yval >=  leave && (prev & ASTRO_DIR_DOWN))) dir |= ASTRO_DIR_DOWN;
    return dir;
}

int astro_pad_for_player(const int *assign, int npads, int player)
{
    int idx, next = 0;
    for (idx = 0; idx < npads; idx++)
        if (assign[idx] == player) return idx;
    for (idx = 0; idx < npads; idx++) {
        if (assign[idx] >= 0) continue;   /* explicitly elsewhere */
        if (next == player) return idx;
        next++;
    }
    return -1;
}

int astro_knob_from_axis(float value, int is_trigger)
{
    const float unit = is_trigger ? value : (value + 1.0f) * 0.5f;
    int knob = (int)(unit * 255.0f + 0.5f);
    return knob < 0 ? 0 : knob > 255 ? 255 : knob;
}

int astro_pad_button_from_sdl(uint8_t button)
{
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ASTRO_PAD_BTN_SOUTH;
    case SDL_GAMEPAD_BUTTON_EAST: return ASTRO_PAD_BTN_EAST;
    case SDL_GAMEPAD_BUTTON_WEST: return ASTRO_PAD_BTN_WEST;
    case SDL_GAMEPAD_BUTTON_NORTH: return ASTRO_PAD_BTN_NORTH;
    case SDL_GAMEPAD_BUTTON_BACK: return ASTRO_PAD_BTN_BACK;
    case SDL_GAMEPAD_BUTTON_GUIDE: return ASTRO_PAD_BTN_GUIDE;
    case SDL_GAMEPAD_BUTTON_START: return ASTRO_PAD_BTN_START;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return ASTRO_PAD_BTN_LEFT_STICK;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return ASTRO_PAD_BTN_RIGHT_STICK;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ASTRO_PAD_BTN_LEFT_SHOULDER;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ASTRO_PAD_BTN_RIGHT_SHOULDER;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ASTRO_PAD_BTN_DPAD_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ASTRO_PAD_BTN_DPAD_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ASTRO_PAD_BTN_DPAD_LEFT;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ASTRO_PAD_BTN_DPAD_RIGHT;
    default: return ASTRO_PAD_BTN_NONE;
    }
}

int astro_sdl_button_from_pad(int button)
{
    switch (button) {
    case ASTRO_PAD_BTN_SOUTH: return SDL_GAMEPAD_BUTTON_SOUTH;
    case ASTRO_PAD_BTN_EAST: return SDL_GAMEPAD_BUTTON_EAST;
    case ASTRO_PAD_BTN_WEST: return SDL_GAMEPAD_BUTTON_WEST;
    case ASTRO_PAD_BTN_NORTH: return SDL_GAMEPAD_BUTTON_NORTH;
    case ASTRO_PAD_BTN_BACK: return SDL_GAMEPAD_BUTTON_BACK;
    case ASTRO_PAD_BTN_GUIDE: return SDL_GAMEPAD_BUTTON_GUIDE;
    case ASTRO_PAD_BTN_START: return SDL_GAMEPAD_BUTTON_START;
    case ASTRO_PAD_BTN_LEFT_STICK: return SDL_GAMEPAD_BUTTON_LEFT_STICK;
    case ASTRO_PAD_BTN_RIGHT_STICK: return SDL_GAMEPAD_BUTTON_RIGHT_STICK;
    case ASTRO_PAD_BTN_LEFT_SHOULDER: return SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    case ASTRO_PAD_BTN_RIGHT_SHOULDER: return SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    case ASTRO_PAD_BTN_DPAD_UP: return SDL_GAMEPAD_BUTTON_DPAD_UP;
    case ASTRO_PAD_BTN_DPAD_DOWN: return SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    case ASTRO_PAD_BTN_DPAD_LEFT: return SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    case ASTRO_PAD_BTN_DPAD_RIGHT: return SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    default: return -1;
    }
}

/* ---- names ------------------------------------------------------------------ */

static const char *const k_button_name[ASTRO_PAD_BTN_COUNT] = {
    "A", "B", "X", "Y", "Back", "Guide", "Start", "Left Stick", "Right Stick",
    "Left Shoulder", "Right Shoulder", "D-pad Up", "D-pad Down", "D-pad Left",
    "D-pad Right", "Left Trigger", "Right Trigger",
};

int astrosession_pad_input_name(int id, char *dst, int dstsz)
{
    static const char *const dirs[ASTRO_PAD_HAT_DIRS] = {
        "Up", "Up-Right", "Right", "Down-Right", "Down", "Down-Left", "Left", "Up-Left",
    };
    if (!dst || dstsz <= 0) return 0;
    if (id < 0)
        return snprintf(dst, (size_t)dstsz, "%s", "");
    if (ASTRO_PAD_BTN_IS_NAMED(id))
        return snprintf(dst, (size_t)dstsz, "%s", k_button_name[id]);
    if (ASTRO_PAD_IS_AXIS(id))
        return snprintf(dst, (size_t)dstsz, "%s", k_axis_name[id - ASTRO_PAD_AXIS_BASE]);
    if (ASTRO_PAD_BTN_IS_RAW(id))
        return snprintf(dst, (size_t)dstsz, "Button %d", id - ASTRO_PAD_BTN_RAW_BASE);
    if (ASTRO_PAD_BTN_IS_HAT(id))
        return snprintf(dst, (size_t)dstsz, "Hat %d %s",
                        (id - ASTRO_PAD_HAT_BASE) / ASTRO_PAD_HAT_DIRS,
                        dirs[(id - ASTRO_PAD_HAT_BASE) % ASTRO_PAD_HAT_DIRS]);
    return snprintf(dst, (size_t)dstsz, "Button %d", id);
}

/* ---- slots ------------------------------------------------------------------ */

static int slot_for_id(SDL_JoystickID devid)
{
    int idx;
    for (idx = 0; idx < g.npads; idx++)
        if (g.pads[idx].devid == devid) return idx;
    return -1;
}

static const char *slot_name(const pad_slot *slot)
{
    const char *name = slot->handle ? SDL_GetGamepadName(slot->handle)
                                    : SDL_GetJoystickName(slot->joy);
    return name ? name : "Gamepad";
}

/* Lets go of everything the pad holds, whichever way it was held. The caller
 * may hold g.lock: input.c's lock is a leaf below it. */
static void slot_release_all(pad_slot *slot)
{
    memset(slot->sent, 0, sizeof slot->sent);
    memset(slot->btn_target, 0, sizeof slot->btn_target);
    astro_input_pad_clear(slot->owner);
}

static void slot_close(pad_slot *slot)
{
    slot_release_all(slot);
    if (slot->handle) SDL_CloseGamepad(slot->handle);
    if (slot->joy) SDL_CloseJoystick(slot->joy);
    slot->handle = NULL;
    slot->joy = NULL;
}

static int effective_player_locked(int idx);

/* Gives the pad in slot idx its remembered player. An entry with the pad's own
 * GUID is the pad's: it takes it from a pad that got it only through the name
 * fallback. A pad known only by name takes the entry if no other pad carries
 * it (two identical pads, or one pad known under two GUIDs): then this one
 * stays automatic. Locked by the caller; only reads g.table, never writes. */
static void restore_assignment(int idx)
{
    pad_slot *slot = &g.pads[idx];
    int ent = astro_pad_table_find(&g.table, slot->guidstr, slot->name), kix;
    int exact;

    if (ent < 0 || g.table.entry[ent].port >= ASTROSESSION_PLAYERS) return;
    exact = !strcmp(g.table.entry[ent].guid, slot->guidstr);
    for (kix = 0; kix < g.npads; kix++) {
        pad_slot *other = &g.pads[kix];
        if (kix == idx || other->assign < 0 ||
            astro_pad_table_find(&g.table, other->guidstr, other->name) != ent)
            continue;
        if (!exact || !other->by_name) return;
        other->assign = -1;          /* the entry's owner is here: it wins */
        other->by_name = 0;
    }
    slot->assign = g.table.entry[ent].port;
    slot->by_name = !exact;
}

static void set_event_locked(const char *text)
{
    snprintf(g.event, sizeof g.event, "%s", text ? text : "");
}

/* Idempotent, and the single place the gamepad/joystick decision is made:
 * both SDL_EVENT_JOYSTICK_ADDED and SDL_EVENT_GAMEPAD_ADDED land here (SDL
 * posts both for a mapped device) and the slot_for_id guard makes the second
 * one a no-op. */
static void open_pad(SDL_JoystickID devid)
{
    pthread_mutex_lock(&g.lock);
    if (g.npads < MAX_PADS && slot_for_id(devid) < 0) {
        /* Not an else-if on SDL_IsGamepad: a device SDL calls a gamepad can
         * still fail to open as one. Falling through to the raw joystick
         * keeps it usable -- every button is still bindable, just unnamed. */
        SDL_Gamepad *pad = SDL_IsGamepad(devid) ? SDL_OpenGamepad(devid) : NULL;
        SDL_Joystick *joy = pad ? NULL : SDL_OpenJoystick(devid);
        if (pad || joy) {
            pad_slot *slot = &g.pads[g.npads++];
            int hat, used = 0, kix;
            for (kix = 0; kix < g.npads - 1; kix++)
                used |= 1 << g.pads[kix].owner;
            memset(slot, 0, sizeof *slot);
            for (kix = 0; kix < MAX_PADS && (used & (1 << kix)); kix++) {}
            slot->owner = kix;
            slot->devid = devid;
            slot->handle = pad;
            slot->joy = joy;
            slot->assign = -1;
            slot->player = -1;
            for (hat = 0; hat < MAX_HATS; hat++) slot->last_hat[hat] = SDL_HAT_CENTERED;
            slot->guid = SDL_GetJoystickGUIDForID(devid);
            snprintf(slot->name, sizeof slot->name, "%s", slot_name(slot));
            SDL_GUIDToString(slot->guid, slot->guidstr, sizeof slot->guidstr);
            restore_assignment(g.npads - 1);
            fprintf(stderr, "astro_gamepad: %s connected (%s%s)\n", slot->name,
                    pad ? "gamepad" : "raw joystick",
                    pad ? "" : "; button 0 = trigger, axes 0/1 = stick, axis 2 = knob");
            if (!g.quiet_scan) {
                char msg[128];
                int player = effective_player_locked(g.npads - 1);
                if (player >= 0)
                    snprintf(msg, sizeof msg, "Connected: %s (player %d)", slot->name, player + 1);
                else
                    snprintf(msg, sizeof msg, "Connected: %s", slot->name);
                g.generation++;
                set_event_locked(msg);
            }
        }
    }
    pthread_mutex_unlock(&g.lock);
}

static void close_pad(SDL_JoystickID devid)
{
    int idx, kix;
    pthread_mutex_lock(&g.lock);
    idx = slot_for_id(devid);
    if (idx >= 0) {
        pad_slot *slot = &g.pads[idx];
        char msg[128];
        fprintf(stderr, "astro_gamepad: %s disconnected\n", slot->name);
        snprintf(msg, sizeof msg, "Disconnected: %s", slot->name);
        slot_close(slot);
        set_event_locked(msg);
        for (kix = idx; kix < g.npads - 1; kix++) g.pads[kix] = g.pads[kix + 1];
        g.npads--;
        g.generation++;
    }
    pthread_mutex_unlock(&g.lock);
}

/* ---- buttons ---------------------------------------------------------------- */

/* The player a slot plays right now. Locked by the caller. */
static int effective_player_locked(int idx)
{
    int assign[MAX_PADS], kix, player;
    for (kix = 0; kix < MAX_PADS; kix++) assign[kix] = -1;
    for (kix = 0; kix < g.npads; kix++) assign[kix] = g.pads[kix].assign;
    for (player = 0; player < ASTROSESSION_PLAYERS; player++)
        if (astro_pad_for_player(assign, g.npads, player) == idx) return player;
    return -1;
}

static void dispatch_button(SDL_JoystickID which, int btn, int down)
{
    int idx, player, owner = 0, target = -1;
    if (btn == ASTRO_PAD_BTN_NONE) return;

    pthread_mutex_lock(&g.lock);
    if (g.cap_active) {
        /* Map mode: every button event is swallowed while armed, not just
         * the captured one, so the press that maps a button does not also
         * fire whatever it used to do. (An axis capture records no button.) */
        if (down && !g.cap_have && !g.cap_axis) { g.cap_have = 1; g.cap_result = btn; }
        pthread_mutex_unlock(&g.lock);
        return;
    }
    idx = slot_for_id(which);
    player = idx >= 0 ? effective_player_locked(idx) : -1;
    if (idx >= 0 && player >= 0 && btn >= 0 && btn < 256) {
        pad_slot *slot = &g.pads[idx];
        owner = slot->owner;
        if (down) {
            target = bindings_target_for_pad(player, btn);
            /* an unmapped joystick's first four buttons stand in for A B X Y
             * until the user binds them */
            if (target < 0 && ASTRO_PAD_BTN_IS_RAW(btn) && btn - ASTRO_PAD_BTN_RAW_BASE < 4)
                target = bindings_target_for_pad(player, btn - ASTRO_PAD_BTN_RAW_BASE);
            if (target >= 0 && !(target >= ASTRO_TARGET_SYSACT && target < ASTRO_TARGET_PLAYERS))
                slot->btn_target[btn] = (int16_t)(target + 1);
        } else if (slot->btn_target[btn]) {
            /* what the press asserted, whatever the binding says now */
            target = slot->btn_target[btn] - 1;
            slot->btn_target[btn] = 0;
        }
    }
    pthread_mutex_unlock(&g.lock);
    if (target < 0) return;

    /* a console action is posted, on the press only, for the UI thread */
    if (target >= ASTRO_TARGET_SYSACT && target < ASTRO_TARGET_PLAYERS) {
        if (down)
            astrosession_sysaction_post(g.session,
                (astrosession_sysaction)(target - ASTRO_TARGET_SYSACT));
        return;
    }
    astro_input_pad_press(owner, target, down);
}

static int hat_dir_from_mask(Uint8 mask)
{
    switch (mask) {
    case SDL_HAT_UP: return 0;
    case SDL_HAT_RIGHTUP: return 1;
    case SDL_HAT_RIGHT: return 2;
    case SDL_HAT_RIGHTDOWN: return 3;
    case SDL_HAT_DOWN: return 4;
    case SDL_HAT_LEFTDOWN: return 5;
    case SDL_HAT_LEFT: return 6;
    case SDL_HAT_LEFTUP: return 7;
    default: return -1;
    }
}

static void handle_hat_motion(SDL_JoystickID which, int hat, Uint8 mask)
{
    int idx, prev_dir, dir;
    if (hat < 0 || hat >= MAX_HATS) return;
    pthread_mutex_lock(&g.lock);
    idx = slot_for_id(which);
    if (idx < 0 || g.pads[idx].handle) { pthread_mutex_unlock(&g.lock); return; }
    prev_dir = hat_dir_from_mask(g.pads[idx].last_hat[hat]);
    g.pads[idx].last_hat[hat] = mask;
    pthread_mutex_unlock(&g.lock);
    dir = hat_dir_from_mask(mask);
    /* hat positions are bindable buttons in their own band; hat 0's four
     * cardinal points also steer, in poll_one */
    if (prev_dir >= 0)
        dispatch_button(which, ASTRO_PAD_HAT_BASE + hat * ASTRO_PAD_HAT_DIRS + prev_dir, 0);
    if (dir >= 0)
        dispatch_button(which, ASTRO_PAD_HAT_BASE + hat * ASTRO_PAD_HAT_DIRS + dir, 1);
}

/* After a session restart the new machine holds nothing, but a pad button
 * may still be physically held: assert it again, as its press would have. */
static void reassert_held(void)
{
    int slot_idx;
    for (slot_idx = 0; slot_idx < MAX_PADS; slot_idx++) {
        int held[64 + 64 + MAX_HATS * ASTRO_PAD_HAT_DIRS];
        int nheld = 0, btn, hat, count;
        SDL_JoystickID devid;
        pad_slot *slot;

        pthread_mutex_lock(&g.lock);
        if (slot_idx >= g.npads) { pthread_mutex_unlock(&g.lock); break; }
        slot = &g.pads[slot_idx];
        devid = slot->devid;
        /* the player it plays from now on, so the first poll sees no move
         * (a move releases whatever the pad holds) */
        slot->player = effective_player_locked(slot_idx);
        if (slot->handle) {
            for (btn = 0; btn < SDL_GAMEPAD_BUTTON_COUNT; btn++)
                if (SDL_GetGamepadButton(slot->handle, (SDL_GamepadButton)btn))
                    held[nheld++] = astro_pad_button_from_sdl((uint8_t)btn);
        } else if (slot->joy) {
            count = SDL_GetNumJoystickButtons(slot->joy);
            for (btn = 0; btn < count && btn < 64; btn++)
                if (SDL_GetJoystickButton(slot->joy, btn))
                    held[nheld++] = ASTRO_PAD_BTN_RAW_BASE + btn;
            count = SDL_GetNumJoystickHats(slot->joy);
            for (hat = 0; hat < count && hat < MAX_HATS; hat++) {
                Uint8 mask = SDL_GetJoystickHat(slot->joy, hat);
                int dir = hat_dir_from_mask(mask);
                slot->last_hat[hat] = mask;
                if (dir >= 0)
                    held[nheld++] = ASTRO_PAD_HAT_BASE + hat * ASTRO_PAD_HAT_DIRS + dir;
            }
        }
        pthread_mutex_unlock(&g.lock);
        for (btn = 0; btn < nheld; btn++)
            dispatch_button(devid, held[btn], 1);
    }
}

/* ---- the stick and the knob --------------------------------------------------- */

static float axis_norm(Sint16 val)
{
    return val < 0 ? (float)val / 32768.0f : (float)val / 32767.0f;
}

/* Pad axis ax (0..5) as -1..1 (a trigger 0..1); a raw joystick without that
 * axis reads 0. */
static float slot_axis(const pad_slot *slot, int ax)
{
    if (slot->handle)
        return axis_norm(SDL_GetGamepadAxis(slot->handle, k_axis_sdl[ax]));
    return ax < SDL_GetNumJoystickAxes(slot->joy)
               ? axis_norm(SDL_GetJoystickAxis(slot->joy, ax)) : 0.0f;
}

/* The raw joystick's hat 0 as directions (a mapped gamepad's D-pad is a
 * button, bound like any other). */
static int slot_hat_dir(const pad_slot *slot)
{
    int dir = 0;
    if (!slot->handle && slot->joy && SDL_GetNumJoystickHats(slot->joy) > 0) {
        Uint8 mask = SDL_GetJoystickHat(slot->joy, 0);
        if (mask & SDL_HAT_UP) dir |= ASTRO_DIR_UP;
        if (mask & SDL_HAT_DOWN) dir |= ASTRO_DIR_DOWN;
        if (mask & SDL_HAT_LEFT) dir |= ASTRO_DIR_LEFT;
        if (mask & SDL_HAT_RIGHT) dir |= ASTRO_DIR_RIGHT;
    }
    return dir;
}

/* A trigger crossing its threshold. Dispatched by poll_pads once it has let
 * go of the lock: dispatch_button takes it too, and it is not recursive. */
typedef struct {
    SDL_JoystickID devid;
    int button;
    int down;
} trigger_edge;

#define MAX_EDGES (MAX_PADS * 2)

static void poll_one(pad_slot *slot, int player, trigger_edge *edges, int *nedges)
{
    uint8_t want[ASTRO_ACT_PER_PLAYER];
    unsigned acts;
    int dir, act, ax;

    memset(want, 0, sizeof want);
    if (player != slot->player) {
        /* moved to another player (or none): release what it held there */
        slot_release_all(slot);
        slot->player = player;
        slot->stick_dir = 0;
        slot->axis_over = 0;
        if (player < 0) return;
    }

    /* the left stick (and a raw joystick's hat 0) is the player's stick */
    dir = slot_hat_dir(slot);
    slot->stick_dir = astro_dir_from_stick(slot_axis(slot, 0), slot_axis(slot, 1),
                                           slot->stick_dir, ASTRO_STICK_ENTER,
                                           ASTRO_STICK_EXIT);
    dir |= slot->stick_dir;
    acts = astro_acts_from_dir(dir);
    for (act = 0; act < ASTRO_ACT_PER_PLAYER; act++)
        want[act] = (uint8_t)((acts >> act) & 1u);
    for (act = 0; act < ASTRO_ACT_PER_PLAYER; act++) {
        if (want[act] != slot->sent[act]) {
            slot->sent[act] = want[act];
            astro_input_pad_stick(slot->owner, player, act, want[act]);
        }
    }

    /* Map-mode axis capture: the first axis past 60% */
    if (g.cap_active && g.cap_axis) {
        uint32_t over = 0;
        for (ax = 0; ax < ASTRO_PAD_AXIS_COUNT; ax++)
            if (fabsf(slot_axis(slot, ax)) > AXIS_CAPTURE) over |= 1u << ax;
        for (ax = 0; ax < ASTRO_PAD_AXIS_COUNT && !g.cap_have; ax++)
            if (((over >> ax) & 1) && !((slot->axis_over >> ax) & 1)) {
                g.cap_have = 1;
                g.cap_result = ASTRO_PAD_AXIS_BASE + ax;
            }
        slot->axis_over = over;
    }

    /* the knob: an absolute axis (a trigger sweeps 0..1, a stick, or any axis
     * of a raw joystick, -1..1) */
    {
        const int kax = bindings_knob_axis(player);
        if (kax >= 0 && !g.cap_active) {
            ax = kax - ASTRO_PAD_AXIS_BASE;
            astro_input_pad_knob(slot->owner, player,
                astro_knob_from_axis(slot_axis(slot, ax), ax >= 4 && slot->handle != NULL));
        }
    }

    /* the triggers, as buttons */
    if (slot->handle) {
        int trig;
        for (trig = 0; trig < 2; trig++) {
            float level = slot_axis(slot, 4 + trig);
            int down = level >= TRIGGER_ON;
            if (down != slot->trigger_down[trig]) {
                slot->trigger_down[trig] = down;
                if (*nedges < MAX_EDGES) {
                    edges[*nedges].devid = slot->devid;
                    edges[*nedges].button = trig ? ASTRO_PAD_BTN_RIGHT_TRIGGER
                                                 : ASTRO_PAD_BTN_LEFT_TRIGGER;
                    edges[*nedges].down = down;
                    (*nedges)++;
                }
            }
        }
    }
}

static void poll_pads(void)
{
    int idx, nedges = 0;
    trigger_edge edges[MAX_EDGES];
    pthread_mutex_lock(&g.lock);
    for (idx = 0; idx < g.npads; idx++)
        poll_one(&g.pads[idx], effective_player_locked(idx), edges, &nedges);
    pthread_mutex_unlock(&g.lock);
    for (idx = 0; idx < nedges; idx++)
        dispatch_button(edges[idx].devid, edges[idx].button, edges[idx].down);
}

/* ---- the thread ------------------------------------------------------------- */

static void *thread_main(void *arg)
{
    int count = 0, idx;
    Uint64 last_tick = 0;
    (void)arg;
    /* Every joystick, not just the ones SDL has a gamepad mapping for:
     * SDL_GetGamepads would silently omit a plain HID adapter entirely. */
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    if (ids) {
        for (idx = 0; idx < count; idx++) open_pad(ids[idx]);
        SDL_free(ids);
    }
    if (g.quiet_scan) {
        g.quiet_scan = 0;
        reassert_held();
    }

    while (!atomic_load(&g.stop_requested)) {
        SDL_Event evt;
        int npads;
        Uint64 tnow;

        pthread_mutex_lock(&g.lock);
        npads = g.npads;
        pthread_mutex_unlock(&g.lock);
        while (SDL_PollEvent(&evt)) {
            switch (evt.type) {
            case SDL_EVENT_JOYSTICK_ADDED: open_pad(evt.jdevice.which); break;
            case SDL_EVENT_JOYSTICK_REMOVED: close_pad(evt.jdevice.which); break;
            case SDL_EVENT_GAMEPAD_ADDED: open_pad(evt.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_REMOVED: close_pad(evt.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                dispatch_button(evt.gbutton.which,
                                astro_pad_button_from_sdl(evt.gbutton.button),
                                evt.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
                break;
            case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
            case SDL_EVENT_JOYSTICK_BUTTON_UP: {
                /* a mapped gamepad also emits these for the same press it
                 * reported as a GAMEPAD_BUTTON event; the slot's kind decides */
                int gamepad, slot_idx;
                pthread_mutex_lock(&g.lock);
                slot_idx = slot_for_id(evt.jbutton.which);
                gamepad = slot_idx >= 0 && g.pads[slot_idx].handle != NULL;
                pthread_mutex_unlock(&g.lock);
                if (gamepad || evt.jbutton.button >= 64) break;
                dispatch_button(evt.jbutton.which,
                                ASTRO_PAD_BTN_RAW_BASE + evt.jbutton.button,
                                evt.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN);
                break;
            }
            case SDL_EVENT_JOYSTICK_HAT_MOTION:
                handle_hat_motion(evt.jhat.which, evt.jhat.hat, evt.jhat.value);
                break;
            default:
                break;
            }
        }
        poll_pads();
        /* ~60 Hz while a pad is attached or a knob key ramps. With none there
         * are only hotplug events to notice, which can wait a tenth of a
         * second. (Not SDL_WaitEventTimeout: SDL polls inside it at about
         * 1 kHz.) */
        SDL_Delay(npads > 0 || astro_input_knob_active() ? 16 : 100);
        tnow = SDL_GetTicks();
        if (last_tick) {
            int dt = (int)(tnow - last_tick);
            astro_input_tick(dt > 100 ? 100 : dt);
        }
        last_tick = tnow;
    }

    pthread_mutex_lock(&g.lock);
    for (idx = 0; idx < g.npads; idx++) slot_close(&g.pads[idx]);
    g.npads = 0;
    pthread_mutex_unlock(&g.lock);
    return NULL;
}

int astro_gamepad_start(astrosession *sess)
{
    pthread_attr_t attr;
    SDL_IOStream *db;
    if (g.running) return 0;
    /* The app owns its signals; SDL must not intercept SIGINT/SIGTERM. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    /* the poll thread is not the focused window: keep reading pads when the
     * app is in the background (the debugger or another window has focus) */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    /* Bluetooth pads over hidapi and the joystick subsystem together. */
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK)) {
        session_set_error(sess, "SDL gamepad init failed: %s", SDL_GetError());
        return -1;
    }

    /* layer the community mapping DB over SDL's own bundled one -- best
     * effort, a controller SDL already knows about is unaffected either way */
    db = SDL_IOFromConstMem(astro_gamecontrollerdb_text, astro_gamecontrollerdb_text_size);
    if (!db || SDL_AddGamepadMappingsFromIO(db, true) < 0)
        fprintf(stderr, "astro_gamepad: failed to load the bundled controller "
                        "DB: %s\n", SDL_GetError());

    pthread_mutex_lock(&g.lock);
    g.session = sess;
    atomic_store(&g.stop_requested, 0);
    g.npads = 0;
    g.cap_active = g.cap_have = g.cap_axis = 0;
    /* a restarted thread's first scan re-finds the same pads: no toast and
     * no generation change */
    g.quiet_scan = sess->pad_restarted;
    /* the remembered assignments, once: garbage or no key is an empty table */
    astro_pad_table_decode(&g.table, astrosession_get_str(sess, "pad_assign", ""),
                           ASTROSESSION_PLAYERS);
    pthread_mutex_unlock(&g.lock);

    /* An explicit stack: the platform default is 512 KB on macOS, and SDL's
     * HID enumeration runs on this thread. */
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4u << 20);
    if (pthread_create(&g.thread, &attr, thread_main, NULL) != 0) {
        pthread_attr_destroy(&attr);
        session_set_error(sess, "gamepad thread could not start");
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK);
        return -1;
    }
    pthread_attr_destroy(&attr);
    g.running = 1;
    return 0;
}

void astro_gamepad_stop(void)
{
    if (!g.running) return;
    atomic_store(&g.stop_requested, 1);
    pthread_join(g.thread, NULL);
    g.running = 0;
    if (g.session)
        g.session->pad_restarted = 1;
    g.session = NULL;
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK);
}

/* ---- public API ------------------------------------------------------------- */

int astrosession_gamepad_count(astrosession *sess)
{
    int num;
    (void)sess;
    pthread_mutex_lock(&g.lock);
    num = g.npads;
    pthread_mutex_unlock(&g.lock);
    return num;
}

int astrosession_gamepad_name(astrosession *sess, int idx, char *dst, int dstsz)
{
    int num = 0;
    (void)sess;
    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    pthread_mutex_lock(&g.lock);
    if (idx >= 0 && idx < g.npads)
        num = snprintf(dst, (size_t)dstsz, "%s", g.pads[idx].name);
    pthread_mutex_unlock(&g.lock);
    return num;
}

void astrosession_gamepad_assign(astrosession *sess, int idx, int player)
{
    char text[ASTRO_PAD_ASSIGN_TEXT_MAX];
    int changed = 0;
    pthread_mutex_lock(&g.lock);
    if (idx >= 0 && idx < g.npads) {
        pad_slot *slot = &g.pads[idx];
        if (player >= 0 && player < ASTROSESSION_PLAYERS) {
            slot->assign = player;
            slot->by_name = 0;
            changed = astro_pad_table_set(&g.table, slot->guidstr, slot->name, player) > 0;
        } else {
            const char *kguid[MAX_PADS], *kname[MAX_PADS];
            int kix, nkeep = 0;
            slot->assign = -1;
            slot->by_name = 0;
            for (kix = 0; kix < g.npads; kix++)
                if (kix != idx && g.pads[kix].assign >= 0) {
                    kguid[nkeep] = g.pads[kix].guidstr;
                    kname[nkeep++] = g.pads[kix].name;
                }
            changed = astro_pad_table_remove(&g.table, slot->guidstr, slot->name,
                                             nkeep, kguid, kname);
        }
        if (changed && astro_pad_table_encode(&g.table, text, sizeof text) < 0)
            changed = 0;
    }
    pthread_mutex_unlock(&g.lock);
    /* the settings file is written here, on the caller's thread, with the
     * pad lock let go: the pad thread must never wait on a file write */
    if (changed)
        astrosession_set_str(sess, "pad_assign", text);
}

int astrosession_gamepad_assignment(astrosession *sess, int idx)
{
    int result = -1;
    (void)sess;
    pthread_mutex_lock(&g.lock);
    if (idx >= 0 && idx < g.npads) result = g.pads[idx].assign;
    pthread_mutex_unlock(&g.lock);
    return result;
}

int astrosession_gamepad_effective_port(astrosession *sess, int idx)
{
    int result = -1;
    (void)sess;
    pthread_mutex_lock(&g.lock);
    if (idx >= 0 && idx < g.npads) result = effective_player_locked(idx);
    pthread_mutex_unlock(&g.lock);
    return result;
}

unsigned astrosession_gamepad_generation(astrosession *sess)
{
    unsigned result;
    (void)sess;
    pthread_mutex_lock(&g.lock);
    result = g.generation;
    pthread_mutex_unlock(&g.lock);
    return result;
}

int astrosession_gamepad_last_event(astrosession *sess, char *dst, int dstsz)
{
    int num;
    (void)sess;
    if (!dst || dstsz <= 0) return 0;
    pthread_mutex_lock(&g.lock);
    num = snprintf(dst, (size_t)dstsz, "%s", g.event);
    pthread_mutex_unlock(&g.lock);
    return num;
}

static void capture_begin(int axis)
{
    pthread_mutex_lock(&g.lock);
    g.cap_active = 1; g.cap_axis = axis; g.cap_have = 0; g.cap_result = ASTRO_PAD_BTN_NONE;
    pthread_mutex_unlock(&g.lock);
}

void astrosession_gamepad_capture_begin(astrosession *sess)
{
    (void)sess;
    capture_begin(0);
}

void astrosession_gamepad_capture_begin_axis(astrosession *sess)
{
    (void)sess;
    capture_begin(1);
}

void astrosession_gamepad_capture_cancel(astrosession *sess)
{
    (void)sess;
    pthread_mutex_lock(&g.lock);
    g.cap_active = 0; g.cap_have = 0; g.cap_axis = 0;
    pthread_mutex_unlock(&g.lock);
}

int astrosession_gamepad_capture_poll(astrosession *sess, int *id)
{
    int got = 0;
    (void)sess;
    pthread_mutex_lock(&g.lock);
    if (g.cap_active && g.cap_have) {
        if (id) *id = g.cap_result;
        g.cap_active = 0; g.cap_have = 0; g.cap_axis = 0;
        got = 1;
    }
    pthread_mutex_unlock(&g.lock);
    return got;
}
