/*
 * rawjoy_test -- a joystick SDL has NO gamepad mapping for (an Astro-daptor,
 * a generic USB adapter) is still attached and plays: button 0 is the
 * trigger, axes 0/1 steer, axis 2 turns the knob, hat 0 steers. Uses SDL3's
 * virtual joystick, so no device is needed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "astro_internal.h"
#include "astrosession.h"
#include "gamepad_sdl.h"
#include "host.h"
#include "input.h"
#include "test_tmpdir.h"

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

static unsigned handle(void) { return astro_host_machine()->handle[0]; }
static int knob(void) { return (uint8_t)~astro_host_machine()->knob[0]; }

#define WAIT(cond) do { for (int i_ = 0; i_ < 100 && !(cond); i_++) usleep(20000); } while (0)

int main(void)
{
    char tmpl[512];
    test_tmpl(tmpl, sizeof tmpl, "astro_rawjoy_test");
    char *dir = mkdtemp(tmpl);
    if (!dir) return 1;
    char cfg[512], data[512];
    snprintf(cfg, sizeof cfg, "%s/config", dir);
    snprintf(data, sizeof data, "%s/data", dir);
    astrosession_paths paths = { cfg, data };
    astrosession *s = astrosession_new(&paths);
    if (!s) return 1;
    astro_input_resync();

    if (astro_gamepad_start(s) != 0) {
        fprintf(stderr, "rawjoy_test: no SDL gamepad subsystem here, skipping\n");
        astrosession_free(s);
        return 77;
    }

    /* a virtual joystick that is NOT a gamepad: type unknown, no mapping */
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_UNKNOWN;
    desc.naxes = 4;
    desc.nbuttons = 4;
    desc.nhats = 1;
    desc.name = "Astro-daptor (test)";
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    if (!id) {
        fprintf(stderr, "rawjoy_test: virtual joystick unavailable (%s), skipping\n", SDL_GetError());
        astro_gamepad_stop();
        astrosession_free(s);
        return 77;
    }
    CHECK(!SDL_IsGamepad(id), "the test device has no gamepad mapping");
    SDL_Joystick *joy = SDL_OpenJoystick(id);
    CHECK(joy != NULL, "open the virtual joystick");

    WAIT(astrosession_gamepad_count(s) == 1);
    CHECK(astrosession_gamepad_count(s) == 1, "the unmapped joystick is attached as a pad");
    char name[64];
    astrosession_gamepad_name(s, 0, name, sizeof name);
    CHECK(strstr(name, "Astro-daptor") != NULL, "it is listed by its name");

    /* button 0: the trigger */
    SDL_SetJoystickVirtualButton(joy, 0, true);
    WAIT(handle() & ASTROSESSION_HANDLE_TRIGGER);
    CHECK(handle() & ASTROSESSION_HANDLE_TRIGGER, "button 0 pulls the trigger");
    SDL_SetJoystickVirtualButton(joy, 0, false);
    WAIT(!(handle() & ASTROSESSION_HANDLE_TRIGGER));
    CHECK(!(handle() & ASTROSESSION_HANDLE_TRIGGER), "released");

    /* axes 0/1 steer */
    SDL_SetJoystickVirtualAxis(joy, 0, 32767);
    SDL_SetJoystickVirtualAxis(joy, 1, -32768);
    WAIT((handle() & (ASTROSESSION_HANDLE_RIGHT | ASTROSESSION_HANDLE_UP)) ==
         (ASTROSESSION_HANDLE_RIGHT | ASTROSESSION_HANDLE_UP));
    CHECK((handle() & (ASTROSESSION_HANDLE_RIGHT | ASTROSESSION_HANDLE_UP)) ==
          (ASTROSESSION_HANDLE_RIGHT | ASTROSESSION_HANDLE_UP), "axes 0/1 steer up-right");
    SDL_SetJoystickVirtualAxis(joy, 0, 0);
    SDL_SetJoystickVirtualAxis(joy, 1, 0);
    WAIT(!(handle() & 0x0f));
    CHECK(!(handle() & 0x0f), "centred");

    /* hat 0 steers */
    SDL_SetJoystickVirtualHat(joy, 0, SDL_HAT_LEFT);
    WAIT(handle() & ASTROSESSION_HANDLE_LEFT);
    CHECK(handle() & ASTROSESSION_HANDLE_LEFT, "the hat steers");
    SDL_SetJoystickVirtualHat(joy, 0, SDL_HAT_CENTERED);

    /* axis 2 turns the knob (the first sample only records; then it follows) */
    SDL_SetJoystickVirtualAxis(joy, 2, 0);
    usleep(100000);
    SDL_SetJoystickVirtualAxis(joy, 2, 32767);
    WAIT(knob() > 250);
    CHECK(knob() > 250, "axis 2 sweeps the knob to the right");
    SDL_SetJoystickVirtualAxis(joy, 2, -32768);
    WAIT(knob() < 5);
    CHECK(knob() < 5, "and to the left");

    /* unplugging releases everything */
    SDL_SetJoystickVirtualButton(joy, 0, true);
    WAIT(handle() & ASTROSESSION_HANDLE_TRIGGER);
    SDL_CloseJoystick(joy);
    SDL_DetachVirtualJoystick(id);
    WAIT(astrosession_gamepad_count(s) == 0);
    CHECK(astrosession_gamepad_count(s) == 0, "detached");
    WAIT(!(handle() & ASTROSESSION_HANDLE_TRIGGER));
    CHECK(!(handle() & ASTROSESSION_HANDLE_TRIGGER), "unplugging lets go of the trigger");

    astro_gamepad_stop();
    astrosession_free(s);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd)) { /* ignore */ }
    fprintf(stderr, "rawjoy_test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
