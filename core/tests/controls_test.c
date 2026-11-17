/*
 * controls_test -- the on-screen controllers' layout: the console keypad, four
 * pistol-grip cards (stick wedges round the knob's dial, the trigger) and the
 * console buttons are where they claim, every pressable part hits itself, the
 * eight wedges map to the right actions, and nothing pressable overlaps.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "astrosession.h"

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

static int overlap(const astro_control *a, const astro_control *b)
{
    return a->xpos < b->xpos + b->width && b->xpos < a->xpos + a->width &&
           a->ypos < b->ypos + b->height && b->ypos < a->ypos + a->height;
}

int main(void)
{
    int count = 0;
    float width = 0, height = 0;
    const astro_control *lay = astrosession_controls_layout(&count, &width, &height);
    int keys = 0, sticks = 0, wedges = 0, knobs = 0, triggers = 0, sys = 0, overlays = 0, captions = 0;

    CHECK(lay && count == 75, "the layout has 75 parts");
    CHECK(width > 10 && height > 10, "it has an extent");

    for (int i = 0; i < count; i++) {
        const astro_control *c = &lay[i];
        CHECK(c->xpos >= 0 && c->ypos >= 0 && c->xpos + c->width <= width + 0.001f &&
              c->ypos + c->height <= height + 0.001f, "every part is inside the extent");
        switch (c->kind) {
        case ASTRO_CTL_KEY:
            keys++;
            CHECK(c->player == -1 && c->target == ASTRO_TARGET_KEYPAD + (keys - 1),
                  "keys run in astrosession_key order with their own target");
            CHECK(c->label && c->label[0], "a key has a label");
            break;
        case ASTRO_CTL_STICK: sticks++; break;
        case ASTRO_CTL_DIRECTION: wedges++; break;
        case ASTRO_CTL_KNOB: knobs++; break;
        case ASTRO_CTL_TRIGGER:
            triggers++;
            CHECK(c->target == ASTRO_TARGET_PLAYER(c->player, ASTRO_ACT_TRIGGER),
                  "a trigger drives its player's trigger");
            break;
        case ASTRO_CTL_SYSBUTTON: sys++; break;
        case ASTRO_CTL_OVERLAY: overlays++; break;
        case ASTRO_CTL_CAPTION: captions++; break;
        default: CHECK(0, "a known kind"); break;
        }
    }
    CHECK(keys == 24 && sticks == 4 && wedges == 32 && knobs == 4 && triggers == 4 &&
          sys == 2 && overlays == 1 && captions == 4, "the right number of each part");

    /* every pressable part hits itself at its centre; the wedges' bounding
     * boxes and the stick disc are shadowed by the exact shapes, tested below */
    for (int i = 0; i < count; i++) {
        const astro_control *c = &lay[i];
        if (c->kind != ASTRO_CTL_KEY && c->kind != ASTRO_CTL_TRIGGER &&
            c->kind != ASTRO_CTL_SYSBUTTON && c->kind != ASTRO_CTL_KNOB)
            continue;
        CHECK(astrosession_controls_hit(c->xpos + c->width / 2, c->ypos + c->height / 2) == i,
              "a part's centre hits it");
    }

    /* nothing pressable overlaps another (wedges and sticks are round) */
    for (int i = 0; i < count; i++)
        for (int j = i + 1; j < count; j++) {
            const int ki = lay[i].kind, kj = lay[j].kind;
            const int pi = ki == ASTRO_CTL_KEY || ki == ASTRO_CTL_TRIGGER || ki == ASTRO_CTL_SYSBUTTON;
            const int pj = kj == ASTRO_CTL_KEY || kj == ASTRO_CTL_TRIGGER || kj == ASTRO_CTL_SYSBUTTON;
            if (pi && pj)
                CHECK(!overlap(&lay[i], &lay[j]), "pressable rectangles do not overlap");
        }

    /* the wedges: a point on each of the eight sides of the first card's dial
     * hits the wedge with the right actions */
    {
        const astro_control *stick = NULL;
        for (int i = 0; i < count; i++)
            if (lay[i].kind == ASTRO_CTL_STICK && lay[i].player == 0) { stick = &lay[i]; break; }
        CHECK(stick != NULL, "player 1 has a stick");
        if (stick) {
            const float cx = stick->xpos + stick->width / 2, cy = stick->ypos + stick->height / 2;
            const float rad = stick->width / 2 * 0.8f;   /* inside the ring */
            static const unsigned want[8] = {
                1u << ASTRO_ACT_RIGHT,
                (1u << ASTRO_ACT_UP) | (1u << ASTRO_ACT_RIGHT),
                1u << ASTRO_ACT_UP,
                (1u << ASTRO_ACT_UP) | (1u << ASTRO_ACT_LEFT),
                1u << ASTRO_ACT_LEFT,
                (1u << ASTRO_ACT_DOWN) | (1u << ASTRO_ACT_LEFT),
                1u << ASTRO_ACT_DOWN,
                (1u << ASTRO_ACT_DOWN) | (1u << ASTRO_ACT_RIGHT),
            };
            for (int sec = 0; sec < 8; sec++) {
                const float ang = (float)sec * 45.0f * 3.14159265f / 180.0f;
                const int hit = astrosession_controls_hit(cx + rad * cosf(ang), cy - rad * sinf(ang));
                CHECK(hit >= 0 && lay[hit].kind == ASTRO_CTL_DIRECTION && lay[hit].player == 0 &&
                      lay[hit].act_mask == want[sec], "a wedge hit drives its actions");
                if (hit >= 0 && (want[sec] & (want[sec] - 1)) == 0)
                    CHECK(lay[hit].target >= 0, "a cardinal wedge has a single target");
                if (hit >= 0 && (want[sec] & (want[sec] - 1)) != 0)
                    CHECK(lay[hit].target == -1, "a diagonal wedge has none");
                CHECK(astrosession_stick_from_vector(rad * cosf(ang), -rad * sinf(ang), stick->width / 2)
                          == want[sec], "stick_from_vector agrees");
            }
            /* the dial's centre is the knob, not a wedge */
            const int mid = astrosession_controls_hit(cx, cy);
            CHECK(mid >= 0 && lay[mid].kind == ASTRO_CTL_KNOB && lay[mid].player == 0,
                  "the middle of the ring is the knob");
            CHECK(astrosession_stick_from_vector(0.05f, 0.05f, 1.0f) == 0, "a tiny deflection is nothing");
            CHECK(astrosession_stick_from_vector(0, 0, 0) == 0, "no radius, no direction");
        }
    }

    /* four cards in a row, each with its own caption, stick and trigger */
    for (int p = 0; p < ASTROSESSION_PLAYERS; p++) {
        float x_stick = -1, x_trig = -1;
        for (int i = 0; i < count; i++) {
            if (lay[i].player != p) continue;
            if (lay[i].kind == ASTRO_CTL_STICK) x_stick = lay[i].xpos;
            if (lay[i].kind == ASTRO_CTL_TRIGGER) x_trig = lay[i].xpos;
        }
        CHECK(x_stick >= 0 && x_trig > x_stick, "a card has a stick and a trigger to its right");
        if (p > 0) {
            float x_prev = -1;
            for (int i = 0; i < count; i++)
                if (lay[i].kind == ASTRO_CTL_STICK && lay[i].player == p - 1) x_prev = lay[i].xpos;
            CHECK(x_stick > x_prev, "players run left to right");
        }
    }

    /* the console buttons carry the system actions */
    {
        int reset = 0, config = 0;
        for (int i = 0; i < count; i++)
            if (lay[i].kind == ASTRO_CTL_SYSBUTTON) {
                if (lay[i].target == ASTRO_TARGET_SYSACT + ASTROSESSION_SYSACT_RESET_GAME) reset++;
                if (lay[i].target == ASTRO_TARGET_SYSACT + ASTROSESSION_SYSACT_RESET_CONFIG) config++;
            }
        CHECK(reset == 1 && config == 1, "RESET and Reset to CONFIG are on the layout");
    }

    /* between parts, and outside, nothing is hit; captions and the overlay frame are not pressable */
    CHECK(astrosession_controls_hit(-1, -1) == -1, "outside is nothing");
    CHECK(astrosession_controls_hit(width + 1, height + 1) == -1, "far outside is nothing");
    for (int i = 0; i < count; i++)
        if (lay[i].kind == ASTRO_CTL_CAPTION)
            CHECK(astrosession_controls_hit(lay[i].xpos + 0.05f, lay[i].ypos + 0.05f) == -1,
                  "a caption is not pressable");
    for (int i = 0; i < count; i++)
        if (lay[i].kind == ASTRO_CTL_OVERLAY)
            CHECK(astrosession_controls_hit(lay[i].xpos + 0.05f, lay[i].ypos + 0.05f) == -1,
                  "the overlay frame is not pressable");

    /* the layout is stable */
    CHECK(astrosession_controls_layout(NULL, NULL, NULL) == lay, "the layout is one static table");

    fprintf(stderr, "controls_test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
