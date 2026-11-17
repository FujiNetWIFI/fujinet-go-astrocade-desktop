/*
 * gamepad_layer_test -- the gamepad layer end to end with SDL3 virtual
 * joysticks, no hardware, no human and no machine: four pads taking the four
 * players in connection order, explicit assignment to any player, hot-unplug
 * releasing exactly what that pad held while the others play on and keep
 * their players, the default mapping (D-pad and left stick steer, South pulls
 * the trigger, the right stick's X turns the knob), a trigger axis as a bound
 * button, map-mode capture of buttons and of the knob's axis, a raw HID
 * adapter (button 0 stands in for South, a hat that steers and is bindable),
 * the sources not stepping on each other, and the assignment: written to the
 * settings ("pad_assign") when made, restored when a new session finds the
 * pad again, found through a stale GUID by the name, kept through garbage,
 * removed by Automatic, with the toasts' text and generation.
 *
 * What a pad holds is read back through astrosession_target_held, which
 * reports what the machine is driven with. Every wait polls with a five second
 * ceiling, so a slow runner is merely slow. SKIPs (77) where SDL has no
 * virtual joysticks or real pads are attached.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "astrosession.h"
#include "bindings.h"
#include "gamepad_sdl.h"
#include "input.h"
#include "test_tmpdir.h"

#define NAME_FMT "astrotest pad %c"
#define RAW_NAME "astrotest raw adapter"

#define ACT_BIT(act) (1u << (act))
#define PAD_BTN(name) SDL_GAMEPAD_BUTTON_##name

static int failures;

static void check(int pass, const char *what)
{
    printf("%s: %s\n", pass ? "ok" : "FAIL", what);
    if (!pass) failures++;
}

static void sleep_ms(int msec)
{
    struct timespec stamp = { msec / 1000, (long)(msec % 1000) * 1000000L };
    nanosleep(&stamp, NULL);
}

/* The player's stick, trigger and knob keys held, a bit per astro_action. */
static unsigned held_acts(astrosession *sess, int player)
{
    unsigned mask = 0;
    for (int act = 0; act <= ASTRO_ACT_KNOB_INC; act++)
        if (astrosession_target_held(sess, ASTRO_TARGET_PLAYER(player, act)))
            mask |= 1u << act;
    return mask;
}

/* The keypad keys held, a bit per astrosession_key. */
static unsigned held_keys(astrosession *sess)
{
    unsigned mask = 0;
    for (int key = 0; key < ASTROSESSION_KEY_COUNT; key++)
        if (astrosession_target_held(sess, ASTRO_TARGET_KEYPAD + key))
            mask |= 1u << key;
    return mask;
}

static void expect(astrosession *sess, int player, unsigned want, const char *what)
{
    unsigned held = 0;
    for (int tries = 0; tries < 250; tries++) {
        held = held_acts(sess, player);
        if (held == want) {
            printf("ok: %s\n", what);
            return;
        }
        sleep_ms(20);
    }
    printf("FAIL: %s (held %02X want %02X)\n", what, held, want);
    failures++;
}

static void expect_keys(astrosession *sess, unsigned want, const char *what)
{
    unsigned held = 0;
    for (int tries = 0; tries < 250; tries++) {
        held = held_keys(sess);
        if (held == want) {
            printf("ok: %s\n", what);
            return;
        }
        sleep_ms(20);
    }
    printf("FAIL: %s (keys %06X want %06X)\n", what, held, want);
    failures++;
}

static void expect_knob(astrosession *sess, int player, int lo, int hi, const char *what)
{
    int value = -1;
    for (int tries = 0; tries < 250; tries++) {
        value = astrosession_knob_get(sess, player);
        if (value >= lo && value <= hi) {
            printf("ok: %s\n", what);
            return;
        }
        sleep_ms(20);
    }
    printf("FAIL: %s (knob %d, want %d..%d)\n", what, value, lo, hi);
    failures++;
}

/* A distinct vendor/product per pad: virtual joysticks share one GUID
 * otherwise, and the assignment table would not tell them apart. */
static SDL_JoystickID attach_pad(const char *name, int raw, int product)
{
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    if (raw) {
        desc.type = SDL_JOYSTICK_TYPE_UNKNOWN;
        desc.naxes = 4;
        desc.nbuttons = 8;
        desc.nhats = 1;
    } else {
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    }
    desc.vendor_id = 0x1209;
    desc.product_id = (Uint16)product;
    desc.name = name;
    return SDL_AttachVirtualJoystick(&desc);
}

static int pad_slot_now(astrosession *sess, const char *name)
{
    for (int idx = 0; idx < astrosession_gamepad_count(sess); idx++) {
        char got[64];
        astrosession_gamepad_name(sess, idx, got, sizeof got);
        if (strcmp(got, name) == 0)
            return idx;
    }
    return -1;
}

static int wait_for_pad(astrosession *sess, const char *name)
{
    int idx = pad_slot_now(sess, name);
    for (int tries = 0; tries < 250 && idx < 0; tries++) {
        sleep_ms(20);
        idx = pad_slot_now(sess, name);
    }
    return idx;
}

static void wait_gone(astrosession *sess, const char *name)
{
    for (int tries = 0; tries < 250 && pad_slot_now(sess, name) >= 0; tries++)
        sleep_ms(20);
}

static int wait_count(astrosession *sess, int want)
{
    for (int tries = 0; tries < 250 && astrosession_gamepad_count(sess) != want; tries++)
        sleep_ms(20);
    return astrosession_gamepad_count(sess) == want;
}

static void pad_name(char *dst, size_t size, int num)
{
    snprintf(dst, size, NAME_FMT, 'A' + num);
}

static int player_of(astrosession *sess, const char *name)
{
    const int idx = wait_for_pad(sess, name);
    return idx < 0 ? -2 : astrosession_gamepad_effective_port(sess, idx);
}

static int wait_sysaction(astrosession *sess, unsigned want)
{
    for (int tries = 0; tries < 250; tries++) {
        const unsigned got = astrosession_sysaction_take(sess);
        if (got)
            return got == want;
        sleep_ms(20);
    }
    return 0;
}

static astrosession *session_up(const astrosession_paths *paths)
{
    astrosession *sess = astrosession_new(paths);
    if (!sess)
        return NULL;
    if (astro_gamepad_start(sess) != 0) {
        printf("SKIP: the gamepad layer would not start: %s\n", astrosession_last_error(sess));
        astrosession_free(sess);
        return NULL;
    }
    return sess;
}

static int has_entry(astrosession *sess, const char *guid, const char *name, int player)
{
    char want[200];
    snprintf(want, sizeof want, "%s|%s=%d", guid, name, player);
    return strstr(astrosession_get_str(sess, "pad_assign", ""), want) != NULL;
}

/* ---- one pad: persistence across sessions ---------------------------------- */

static void persistence(const astrosession_paths *paths)
{
    char name[64], guid[64], text[200], tag[32];
    SDL_JoystickID vid;
    astrosession *sess;
    int idx, want, auto_player;
    unsigned gen_before;

    {
        /* the earlier run assigned these pads: start from no settings */
        char path[600];
        snprintf(path, sizeof path, "%s/settings.ini", paths->config_dir);
        remove(path);
    }
    pad_name(name, sizeof name, 0);
    vid = attach_pad(name, 0, 0x1100);
    if (!vid) {
        check(0, "persistence: the virtual pad attaches");
        return;
    }
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(vid), guid, sizeof guid);

    sess = session_up(paths);
    if (!sess) { failures++; SDL_DetachVirtualJoystick(vid); return; }
    idx = wait_for_pad(sess, name);
    check(idx >= 0, "persistence: the pad is picked up");
    if (idx < 0) { astrosession_free(sess); SDL_DetachVirtualJoystick(vid); return; }
    check(astrosession_gamepad_assignment(sess, idx) == -1, "a pad nobody assigned is automatic");
    check(strstr(astrosession_get_str(sess, "pad_assign", ""), guid) == NULL, "and has no entry");
    auto_player = astrosession_gamepad_effective_port(sess, idx);
    want = auto_player == 3 ? 2 : 3;

    astrosession_gamepad_last_event(sess, text, sizeof text);
    check(strstr(text, "Connected: ") == text && strstr(text, name) != NULL, "the toast says the pad connected");
    snprintf(tag, sizeof tag, "(player %d)", auto_player + 1);
    check(auto_player < 0 || strstr(text, tag) != NULL, "and which player it plays");

    astrosession_gamepad_assign(sess, idx, want);
    check(astrosession_gamepad_assignment(sess, idx) == want &&
          astrosession_gamepad_effective_port(sess, idx) == want, "assigning plays that player");
    check(has_entry(sess, guid, name, want), "and writes <guid>|<name>=<player> to pad_assign");
    {
        char path[600], line[16384];
        int found = 0;
        FILE *file;
        snprintf(path, sizeof path, "%s/settings.ini", paths->config_dir);
        file = fopen(path, "r");
        if (file) {
            while (fgets(line, sizeof line, file))
                if (!strncmp(line, "pad_assign=", 11) && strstr(line, guid))
                    found = 1;
            fclose(file);
        }
        check(found, "the settings file on disk holds it");
    }

    /* restart: a new session on the same settings */
    astro_gamepad_stop();
    astrosession_free(sess);
    sess = session_up(paths);
    if (!sess) { failures++; SDL_DetachVirtualJoystick(vid); return; }
    idx = wait_for_pad(sess, name);
    check(idx >= 0 && astrosession_gamepad_assignment(sess, idx) == want &&
          astrosession_gamepad_effective_port(sess, idx) == want,
          "restart: the assignment is restored on device add");

    /* a stale GUID (another USB port): the name finds the entry */
    snprintf(text, sizeof text, "ffffffffffffffffffffffffffffffff|%s=%d", name, want);
    astrosession_set_str(sess, "pad_assign", text);
    astro_gamepad_stop();
    astrosession_free(sess);
    sess = session_up(paths);
    if (!sess) { failures++; SDL_DetachVirtualJoystick(vid); return; }
    idx = wait_for_pad(sess, name);
    check(idx >= 0 && astrosession_gamepad_assignment(sess, idx) == want,
          "a different GUID with the same name gets the assignment too");

    /* Automatic removes the entry */
    astrosession_gamepad_assign(sess, idx, want);
    check(has_entry(sess, guid, name, want), "assigned under the pad's own GUID again");
    astrosession_gamepad_assign(sess, idx, -1);
    check(astrosession_gamepad_assignment(sess, idx) == -1 &&
          strstr(astrosession_get_str(sess, "pad_assign", ""), name) == NULL,
          "Automatic: no explicit player, and the entry is gone");

    /* garbage, and a player outside 0..3, are ignored */
    astrosession_set_str(sess, "pad_assign", ";;|=;zz|x=9;%%%;00000000000000000000000000000000|astrotest pad A=4");
    astro_gamepad_stop();
    astrosession_free(sess);
    sess = session_up(paths);
    if (!sess) { failures++; SDL_DetachVirtualJoystick(vid); return; }
    idx = wait_for_pad(sess, name);
    check(idx >= 0 && astrosession_gamepad_assignment(sess, idx) == -1,
          "a malformed or out-of-range pad_assign leaves the pad automatic");

    /* a hot replug within a run restores it too, and is announced */
    astrosession_gamepad_assign(sess, idx, want);
    gen_before = astrosession_gamepad_generation(sess);
    SDL_DetachVirtualJoystick(vid);
    wait_gone(sess, name);
    check(pad_slot_now(sess, name) < 0 && astrosession_gamepad_generation(sess) != gen_before,
          "unplugged: the pad is gone and the generation moved");
    astrosession_gamepad_last_event(sess, text, sizeof text);
    check(!strcmp(text, "Disconnected: astrotest pad A"), "the toast says the pad disconnected");
    vid = attach_pad(name, 0, 0x1100);
    idx = vid ? wait_for_pad(sess, name) : -1;
    check(idx >= 0 && astrosession_gamepad_assignment(sess, idx) == want, "plugged back in: the player comes back");
    astrosession_gamepad_last_event(sess, text, sizeof text);
    snprintf(tag, sizeof tag, "(player %d)", want + 1);
    check(strstr(text, tag) != NULL, "and the toast names that player");
    astrosession_gamepad_assign(sess, idx, -1);

    /* the gamepad thread stopping and starting again (a cartridge load) finds
     * the same pad: no new toast, the generation where it was, a held button
     * asserted again in the new machine */
    if (vid && idx >= 0) {
        SDL_Joystick *joy = SDL_OpenJoystick(vid);
        const int player = astrosession_gamepad_effective_port(sess, idx);
        char before[200];

        gen_before = astrosession_gamepad_generation(sess);
        astrosession_gamepad_last_event(sess, before, sizeof before);
        check(joy != NULL && player >= 0, "restart: the pad is open and plays a player");
        if (joy && player >= 0) {
            SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, 30000);
            expect(sess, player, ACT_BIT(ASTRO_ACT_RIGHT), "restart: the pad thread is polling");
            SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, 0);
            expect(sess, player, 0, "restart: the stick lets go");
            SDL_SetJoystickVirtualButton(joy, PAD_BTN(SOUTH), true);
            expect(sess, player, ACT_BIT(ASTRO_ACT_TRIGGER), "restart: South is held");
            astro_gamepad_stop();
            check(astrosession_gamepad_generation(sess) == gen_before, "stopped: the generation is where it was");
            astro_input_clear_all();               /* the new machine holds nothing */
            astro_gamepad_start(sess);
            check(wait_for_pad(sess, name) >= 0, "restarted: the pad is found again");
            expect(sess, player, ACT_BIT(ASTRO_ACT_TRIGGER), "restarted: the held button is asserted again");
            astrosession_gamepad_last_event(sess, text, sizeof text);
            check(astrosession_gamepad_generation(sess) == gen_before && !strcmp(text, before),
                  "restarted: no new generation and no new toast");
            SDL_SetJoystickVirtualButton(joy, PAD_BTN(SOUTH), false);
            expect(sess, player, 0, "restarted: and the button still lets go");
        }
        if (joy)
            SDL_CloseJoystick(joy);
    }
    astro_gamepad_stop();
    astrosession_free(sess);
    if (vid)
        SDL_DetachVirtualJoystick(vid);
}

/* ---- a plain HID adapter ----------------------------------------------------- */

static void raw_adapter(astrosession *sess)
{
    SDL_JoystickID vid = attach_pad(RAW_NAME, 1, 0x1200);
    SDL_Joystick *joy;
    int idx, player;
    char text[128];

    if (!vid) {
        printf("SKIP: no raw virtual joystick here: %s\n", SDL_GetError());
        return;
    }
    joy = SDL_OpenJoystick(vid);
    idx = joy ? wait_for_pad(sess, RAW_NAME) : -1;
    check(idx >= 0, "a raw adapter is opened as a joystick, not missed");
    player = idx >= 0 ? astrosession_gamepad_effective_port(sess, idx) : -1;
    if (player < 0) {
        check(0, "the raw adapter plays a player");
        if (joy) SDL_CloseJoystick(joy);
        SDL_DetachVirtualJoystick(vid);
        return;
    }
    check(!SDL_IsGamepad(vid), "(and SDL has no gamepad mapping for it)");
    astrosession_gamepad_last_event(sess, text, sizeof text);
    check(strstr(text, "Connected: " RAW_NAME) == text, "the toast names it");
    sleep_ms(300);

    /* button 0 stands in for South until it is bound: the trigger */
    SDL_SetJoystickVirtualButton(joy, 0, true);
    expect(sess, player, ACT_BIT(ASTRO_ACT_TRIGGER), "an unbound raw button 0 pulls the trigger");
    SDL_SetJoystickVirtualButton(joy, 0, false);
    expect(sess, player, 0, "released");
    SDL_SetJoystickVirtualButton(joy, 6, true);
    sleep_ms(150);
    expect(sess, player, 0, "an unbound raw button above the first four does nothing");
    SDL_SetJoystickVirtualButton(joy, 6, false);
    sleep_ms(150);       /* (SDL folds a release and a press between two polls into nothing) */
    bindings_set_pad(sess, ASTROSESSION_KEY_9, ASTRO_PAD_BTN_RAW_BASE + 6, NULL);
    SDL_SetJoystickVirtualButton(joy, 6, true);
    expect_keys(sess, 1u << ASTROSESSION_KEY_9, "raw button 6, bound to keypad 9, presses it");
    SDL_SetJoystickVirtualButton(joy, 6, false);
    expect_keys(sess, 0, "released");

    /* hat 0 steers; its positions are also bindable buttons in the band 192.. */
    SDL_SetJoystickVirtualHat(joy, 0, SDL_HAT_UP);
    expect(sess, player, ACT_BIT(ASTRO_ACT_UP), "hat 0 up steers up");
    SDL_SetJoystickVirtualHat(joy, 0, SDL_HAT_RIGHTDOWN);
    expect(sess, player, ACT_BIT(ASTRO_ACT_RIGHT) | ACT_BIT(ASTRO_ACT_DOWN), "and a diagonal is two directions");
    SDL_SetJoystickVirtualHat(joy, 0, SDL_HAT_CENTERED);
    expect(sess, player, 0, "centred: nothing");
    bindings_set_pad(sess, ASTRO_TARGET_PLAYER(player, ASTRO_ACT_KNOB_INC), ASTRO_PAD_HAT_BASE + 2, NULL);
    SDL_SetJoystickVirtualHat(joy, 0, SDL_HAT_RIGHT);
    expect(sess, player, ACT_BIT(ASTRO_ACT_RIGHT) | ACT_BIT(ASTRO_ACT_KNOB_INC),
           "hat right steers right and, bound in the hat band, turns the knob");
    SDL_SetJoystickVirtualHat(joy, 0, SDL_HAT_CENTERED);
    expect(sess, player, 0, "centred: both let go");

    /* axes 0 and 1 are its stick */
    SDL_SetJoystickVirtualAxis(joy, 0, -30000);
    SDL_SetJoystickVirtualAxis(joy, 1, -30000);
    expect(sess, player, ACT_BIT(ASTRO_ACT_LEFT) | ACT_BIT(ASTRO_ACT_UP), "axes 0 and 1 are the stick");
    SDL_SetJoystickVirtualAxis(joy, 0, 0);
    SDL_SetJoystickVirtualAxis(joy, 1, 0);
    expect(sess, player, 0, "centred");

    /* axis 2 turns the knob (the first sample only records, then it follows) */
    SDL_SetJoystickVirtualAxis(joy, 2, 0);
    sleep_ms(200);
    SDL_SetJoystickVirtualAxis(joy, 2, 32767);
    expect_knob(sess, player, 250, 255, "axis 2 sweeps the knob right");
    SDL_SetJoystickVirtualAxis(joy, 2, -32768);
    expect_knob(sess, player, 0, 5, "and left");
    SDL_SetJoystickVirtualAxis(joy, 2, 0);
    expect_knob(sess, player, 125, 130, "and centres");

    /* map mode takes a raw button and a hat position by number */
    {
        int btn = -1, got = 0;
        astrosession_gamepad_capture_begin(sess);
        SDL_SetJoystickVirtualButton(joy, 5, true);
        for (int tries = 0; tries < 250 && !got; tries++) {
            got = astrosession_gamepad_capture_poll(sess, &btn);
            if (!got) sleep_ms(20);
        }
        check(got && btn == ASTRO_PAD_BTN_RAW_BASE + 5, "map mode captures raw button 5 as 69");
        got = 0;
        SDL_SetJoystickVirtualButton(joy, 5, false);
        astrosession_gamepad_capture_begin(sess);
        SDL_SetJoystickVirtualHat(joy, 0, SDL_HAT_LEFT);
        for (int tries = 0, done = 0; tries < 250 && !done; tries++) {
            done = got = astrosession_gamepad_capture_poll(sess, &btn);
            if (!done) sleep_ms(20);
        }
        check(got && btn == ASTRO_PAD_HAT_BASE + 6, "and hat 0 left as 198");
        SDL_SetJoystickVirtualHat(joy, 0, SDL_HAT_CENTERED);
    }
    expect(sess, player, 0, "capturing pressed nothing");

    /* unplugged while held */
    SDL_SetJoystickVirtualButton(joy, 6, true);
    SDL_SetJoystickVirtualHat(joy, 0, SDL_HAT_UP);
    expect(sess, player, ACT_BIT(ASTRO_ACT_UP), "held before the unplug");
    SDL_CloseJoystick(joy);
    SDL_DetachVirtualJoystick(vid);
    wait_gone(sess, RAW_NAME);
    expect(sess, player, 0, "the raw adapter's holds are released on unplug");
    expect_keys(sess, 0, "and so is its keypad key");
    bindings_reset_defaults(sess);
}

/* ---- four pads --------------------------------------------------------------- */

int main(void)
{
    char cfg[512], data[512], tmpl[512];
    astrosession_paths paths;
    astrosession *sess;
    SDL_JoystickID vid[4];
    SDL_Joystick *joy[4];
    char name[4][64];
    int player[4], seen = 0, tries;
    char *dir;

    /* held open for the whole run: a session stopping must not take the
     * virtual pads with it (the persistence tests restart sessions) */
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        printf("SKIP: SDL gamepad subsystem: %s\n", SDL_GetError());
        return 77;
    }
    test_tmpl(tmpl, sizeof tmpl, "astro_gm_test");
    dir = mkdtemp(tmpl);
    if (!dir) return 1;
    snprintf(cfg, sizeof cfg, "%s/config", dir);
    snprintf(data, sizeof data, "%s/data", dir);
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;

    sess = session_up(&paths);
    if (!sess)
        return 77;
    sleep_ms(300);
    if (astrosession_gamepad_count(sess) != 0) {
        printf("SKIP: real pads are attached (%d)\n", astrosession_gamepad_count(sess));
        astro_gamepad_stop();
        astrosession_free(sess);
        return 77;
    }

    /* four pads, attached one after another */
    for (int num = 0; num < 4; num++) {
        pad_name(name[num], sizeof name[num], num);
        vid[num] = attach_pad(name[num], 0, 0x1100 + num);
        if (!vid[num]) {
            printf("SKIP: no virtual joysticks here: %s\n", SDL_GetError());
            astro_gamepad_stop();
            astrosession_free(sess);
            return 77;
        }
        joy[num] = SDL_OpenJoystick(vid[num]);
        check(joy[num] && wait_for_pad(sess, name[num]) >= 0, "a virtual pad is picked up");
    }
    check(wait_count(sess, 4), "all four are connected");
    check(astrosession_gamepad_generation(sess) >= 4, "each connecting bumped the generation");

    /* connection order: the four pads take the four players, each its own */
    for (int num = 0; num < 4; num++) {
        player[num] = player_of(sess, name[num]);
        seen |= player[num] >= 0 && player[num] < 4 ? 1 << player[num] : 0;
    }
    check(seen == 0xf, "four pads in connection order take four different players");
    check(player[0] == 0 && player[1] == 1 && player[2] == 2 && player[3] == 3,
          "player 1 to 4 in the order they were plugged in");
    sleep_ms(300);   /* the pad thread takes its baseline of the buttons a poll after opening */

    /* every pad plays its own player: South pulls its trigger and no other */
    for (int num = 0; num < 4; num++) {
        SDL_SetJoystickVirtualButton(joy[num], PAD_BTN(SOUTH), true);
        expect(sess, player[num], ACT_BIT(ASTRO_ACT_TRIGGER), "South pulls its own player's trigger");
        for (int other = 0; other < 4; other++)
            if (other != num)
                check(held_acts(sess, player[other]) == 0, "and nobody else's");
        SDL_SetJoystickVirtualButton(joy[num], PAD_BTN(SOUTH), false);
        expect(sess, player[num], 0, "released");
    }

    /* the D-pad steers by default, and so does the left stick: separate sources */
    {
        const int p = player[1];
        SDL_SetJoystickVirtualButton(joy[1], PAD_BTN(DPAD_UP), true);
        expect(sess, p, ACT_BIT(ASTRO_ACT_UP), "the D-pad up steers up");
        SDL_SetJoystickVirtualButton(joy[1], PAD_BTN(DPAD_LEFT), true);
        expect(sess, p, ACT_BIT(ASTRO_ACT_UP) | ACT_BIT(ASTRO_ACT_LEFT), "a diagonal");
        SDL_SetJoystickVirtualButton(joy[1], PAD_BTN(DPAD_DOWN), true);
        expect(sess, p, ACT_BIT(ASTRO_ACT_LEFT), "up and down together cancel");
        SDL_SetJoystickVirtualButton(joy[1], PAD_BTN(DPAD_UP), false);
        expect(sess, p, ACT_BIT(ASTRO_ACT_LEFT) | ACT_BIT(ASTRO_ACT_DOWN), "and down is back once up lets go");
        SDL_SetJoystickVirtualButton(joy[1], PAD_BTN(DPAD_DOWN), false);
        SDL_SetJoystickVirtualButton(joy[1], PAD_BTN(DPAD_LEFT), false);
        expect(sess, p, 0, "released");
        SDL_SetJoystickVirtualAxis(joy[1], SDL_GAMEPAD_AXIS_LEFTX, 20000);
        SDL_SetJoystickVirtualAxis(joy[1], SDL_GAMEPAD_AXIS_LEFTY, -32768);
        expect(sess, p, ACT_BIT(ASTRO_ACT_RIGHT) | ACT_BIT(ASTRO_ACT_UP), "the stick right and up is a diagonal");
        SDL_SetJoystickVirtualButton(joy[1], PAD_BTN(DPAD_LEFT), true);
        expect(sess, p, ACT_BIT(ASTRO_ACT_UP), "the D-pad is its own source: left cancels the stick's right");
        SDL_SetJoystickVirtualButton(joy[1], PAD_BTN(DPAD_LEFT), false);
        SDL_SetJoystickVirtualAxis(joy[1], SDL_GAMEPAD_AXIS_LEFTX, 0);
        SDL_SetJoystickVirtualAxis(joy[1], SDL_GAMEPAD_AXIS_LEFTY, 0);
        expect(sess, p, 0, "the stick centred: nothing held");
        SDL_SetJoystickVirtualAxis(joy[1], SDL_GAMEPAD_AXIS_LEFTX, 12000);   /* 0.37: under half */
        sleep_ms(150);
        expect(sess, p, 0, "a third of the way is not enough");
        SDL_SetJoystickVirtualAxis(joy[1], SDL_GAMEPAD_AXIS_LEFTX, 17000);   /* 0.52 */
        expect(sess, p, ACT_BIT(ASTRO_ACT_RIGHT), "past half it is");
        SDL_SetJoystickVirtualAxis(joy[1], SDL_GAMEPAD_AXIS_LEFTX, 13000);   /* 0.40 */
        sleep_ms(150);
        expect(sess, p, ACT_BIT(ASTRO_ACT_RIGHT), "and it holds until it falls below 0.35");
        SDL_SetJoystickVirtualAxis(joy[1], SDL_GAMEPAD_AXIS_LEFTX, 9000);
        expect(sess, p, 0, "then lets go");
        SDL_SetJoystickVirtualAxis(joy[1], SDL_GAMEPAD_AXIS_LEFTX, 0);
    }

    /* the right stick's X is the knob: the first sample only records */
    {
        const int p = player[2];
        SDL_SetJoystickVirtualAxis(joy[2], SDL_GAMEPAD_AXIS_RIGHTX, 0);
        sleep_ms(200);
        astrosession_knob_set(sess, p, 40);
        SDL_SetJoystickVirtualAxis(joy[2], SDL_GAMEPAD_AXIS_RIGHTX, 32767);
        expect_knob(sess, p, 250, 255, "the right stick's X sweeps player 3's knob to the right");
        check(astrosession_knob_get(sess, player[0]) == ASTROSESSION_KNOB_CENTRE,
              "and no other player's knob moves");
        SDL_SetJoystickVirtualAxis(joy[2], SDL_GAMEPAD_AXIS_RIGHTX, -32768);
        expect_knob(sess, p, 0, 5, "and left");
        SDL_SetJoystickVirtualAxis(joy[2], SDL_GAMEPAD_AXIS_RIGHTX, 0);
        expect_knob(sess, p, 125, 130, "and centres");
        astrosession_knob_set(sess, p, 200);                  /* the dial takes it back */
        sleep_ms(150);
        check(astrosession_knob_get(sess, p) == 200, "a still axis does not fight the dial");
    }

    /* the sources do not step on each other: a key, the mouse and a pad on one action */
    {
        const int p = player[0];
        SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(SOUTH), true);
        expect(sess, p, ACT_BIT(ASTRO_ACT_TRIGGER), "South pulls the trigger");
        astrosession_press(sess, ASTRO_TARGET_PLAYER(p, ASTRO_ACT_TRIGGER), 1);
        astrosession_input_key(sess, ASTROSESSION_KEYSYM_SPACE, 1, 0);   /* player 1's trigger key */
        SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(SOUTH), false);
        sleep_ms(150);
        check((held_acts(sess, p) & ACT_BIT(ASTRO_ACT_TRIGGER)) != 0,
              "the pad letting go leaves the mouse's trigger held");
        astrosession_release_all(sess);
        check((held_acts(sess, p) & ACT_BIT(ASTRO_ACT_TRIGGER)) != 0, "release_all lets go of the keyboard's alone");
        astrosession_pointer_release(sess);
        expect(sess, p, 0, "and then the mouse's");
    }

    /* a trigger axis is a bindable button (past half travel: must not hang the pad lock) */
    bindings_set_pad(sess, ASTROSESSION_KEY_7, ASTRO_PAD_BTN_LEFT_TRIGGER, NULL);
    SDL_SetJoystickVirtualAxis(joy[3], SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    expect_keys(sess, 1u << ASTROSESSION_KEY_7, "the left trigger presses its binding");
    check(astrosession_gamepad_effective_port(sess, 3) == player[3], "the pad lock is still free with a trigger held");
    SDL_SetJoystickVirtualAxis(joy[3], SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0);
    expect_keys(sess, 0, "the left trigger released");
    bindings_reset_defaults(sess);

    /* a pad button on a console action posts it, once, on the press */
    bindings_set_pad(sess, ASTRO_TARGET_SYSACT + ASTROSESSION_SYSACT_RESET_GAME, ASTRO_PAD_BTN_START, NULL);
    (void)astrosession_sysaction_take(sess);
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(START), true);
    check(wait_sysaction(sess, 1u << ASTROSESSION_SYSACT_RESET_GAME), "Start, bound to Reset Game, posts it");
    sleep_ms(150);
    check(astrosession_sysaction_take(sess) == 0, "once: holding the button posts nothing more");
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(START), false);
    sleep_ms(150);
    check(astrosession_sysaction_take(sess) == 0 && held_keys(sess) == 0, "and the release posts nothing");
    bindings_reset_defaults(sess);

    /* map mode: a button is captured and swallowed; the knob's axis is captured by axis mode */
    {
        int btn = -1, got = 0;
        astrosession_gamepad_capture_begin(sess);
        SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(SOUTH), true);
        for (tries = 0; tries < 250 && !got; tries++) {
            got = astrosession_gamepad_capture_poll(sess, &btn);
            if (!got) sleep_ms(20);
        }
        check(got && btn == ASTRO_PAD_BTN_SOUTH, "map mode captures South");
        check(held_acts(sess, player[0]) == 0, "and South did not fire while it was captured");
        SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(SOUTH), false);
        sleep_ms(150);

        astrosession_gamepad_capture_begin_axis(sess);
        SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(EAST), true);       /* a button is not an axis */
        sleep_ms(150);
        check(!astrosession_gamepad_capture_poll(sess, &btn), "axis capture ignores a button");
        SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(EAST), false);
        SDL_SetJoystickVirtualAxis(joy[0], SDL_GAMEPAD_AXIS_RIGHTY, 30000);
        for (got = 0, tries = 0; tries < 250 && !got; tries++) {
            got = astrosession_gamepad_capture_poll(sess, &btn);
            if (!got) sleep_ms(20);
        }
        check(got && btn == ASTRO_PAD_AXIS_BASE + 3, "axis capture records the right stick's Y");
        SDL_SetJoystickVirtualAxis(joy[0], SDL_GAMEPAD_AXIS_RIGHTY, 0);
        astrosession_gamepad_capture_begin(sess);
        astrosession_gamepad_capture_cancel(sess);
        SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(EAST), true);
        sleep_ms(150);
        SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(EAST), false);
        sleep_ms(100);
        check(!astrosession_gamepad_capture_poll(sess, &btn), "a cancelled capture records nothing");
    }

    /* explicit assignment. The Nth unassigned pad plays player N, so putting
     * pad A on player 4 closes the others up in order */
    astrosession_gamepad_assign(sess, wait_for_pad(sess, name[0]), 3);
    check(player_of(sess, name[0]) == 3 && player_of(sess, name[1]) == 0 &&
          player_of(sess, name[2]) == 1 && player_of(sess, name[3]) == 2,
          "pad A on player 4: B, C and D close up as players 1, 2 and 3");
    check(astrosession_gamepad_assignment(sess, wait_for_pad(sess, name[0])) == 3 &&
          astrosession_gamepad_assignment(sess, wait_for_pad(sess, name[1])) == -1,
          "A is explicit, B is still automatic");
    /* every pad explicit, in reverse: each plays its own player whatever its place in line */
    astrosession_gamepad_assign(sess, wait_for_pad(sess, name[1]), 2);
    astrosession_gamepad_assign(sess, wait_for_pad(sess, name[2]), 1);
    astrosession_gamepad_assign(sess, wait_for_pad(sess, name[3]), 0);
    check(player_of(sess, name[0]) == 3 && player_of(sess, name[1]) == 2 &&
          player_of(sess, name[2]) == 1 && player_of(sess, name[3]) == 0,
          "all four assigned in reverse: pad A plays 4, B 3, C 2, D 1");
    /* the pad thread takes a pad's move to another player a poll later, and
     * lets go of what it held: press only once it has seen it */
    sleep_ms(200);
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(SOUTH), true);
    expect(sess, 3, ACT_BIT(ASTRO_ACT_TRIGGER), "pad A pulls player 4's trigger");
    check(held_acts(sess, 0) == 0 && held_acts(sess, 1) == 0 && held_acts(sess, 2) == 0, "and nobody else's");
    /* taking the assignment away while it is held lets go: A is automatic again,
     * and with every player claimed it plays nobody */
    astrosession_gamepad_assign(sess, wait_for_pad(sess, name[0]), -1);
    expect(sess, 3, 0, "making a pad automatic lets go of what it held on its old player");
    check(player_of(sess, name[0]) == -1, "and with all four players claimed it plays nobody");
    SDL_SetJoystickVirtualButton(joy[0], PAD_BTN(SOUTH), false);
    astrosession_gamepad_assign(sess, wait_for_pad(sess, name[0]), 3);
    sleep_ms(200);

    /* hot-unplug while held: pad C (player 2) lets go of everything; the others
     * play on and keep their players, a hold on another pad undisturbed */
    SDL_SetJoystickVirtualButton(joy[2], PAD_BTN(DPAD_RIGHT), true);
    SDL_SetJoystickVirtualButton(joy[2], PAD_BTN(SOUTH), true);
    SDL_SetJoystickVirtualAxis(joy[2], SDL_GAMEPAD_AXIS_LEFTY, -30000);
    SDL_SetJoystickVirtualButton(joy[3], PAD_BTN(SOUTH), true);
    expect(sess, 1, ACT_BIT(ASTRO_ACT_RIGHT) | ACT_BIT(ASTRO_ACT_TRIGGER) | ACT_BIT(ASTRO_ACT_UP),
           "held on player 2 before the unplug");
    expect(sess, 0, ACT_BIT(ASTRO_ACT_TRIGGER), "and pad D's trigger on player 1");
    SDL_CloseJoystick(joy[2]);
    SDL_DetachVirtualJoystick(vid[2]);
    wait_gone(sess, name[2]);
    check(pad_slot_now(sess, name[2]) < 0 && astrosession_gamepad_count(sess) == 3, "the unplugged pad is gone");
    expect(sess, 1, 0, "its directions, trigger and stick are released");
    check(player_of(sess, name[0]) == 3 && player_of(sess, name[1]) == 2 && player_of(sess, name[3]) == 0,
          "the other pads keep their players: no reshuffle");
    check((held_acts(sess, 0) & ACT_BIT(ASTRO_ACT_TRIGGER)) != 0, "pad D's held trigger is not disturbed");
    SDL_SetJoystickVirtualButton(joy[3], PAD_BTN(SOUTH), false);
    expect(sess, 0, 0, "and still lets go");

    for (int num = 0; num < 4; num++) {
        if (num == 2) continue;
        SDL_CloseJoystick(joy[num]);
        SDL_DetachVirtualJoystick(vid[num]);
    }
    wait_count(sess, 0);
    bindings_reset_defaults(sess);

    raw_adapter(sess);

    astro_gamepad_stop();
    astrosession_free(sess);

    persistence(&paths);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
