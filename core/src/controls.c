/*
 * controls -- the on-screen controllers: one layout for every toolkit, its hit
 * test and the stick's gesture maths.
 *
 * The console's 24-key keypad sits on top, centred, as a plate with a legend
 * zone above every key (the Bally BASIC overlay paints its legends there). Under
 * it, one row of four cards, Player 1 to 4, each a side view of the Bally
 * pistol grip: the ribbed knob on top is the dial (a paddle: drag it round),
 * ringed by eight direction wedges (the whole grip tilts as the stick), and the
 * trigger at the front. A row of console buttons closes the window:
 *
 *   KEY        a keypad key (the window of the overlay card); its legend zone is
 *              the KEY_LEGEND_H above it
 *   STICK      the disc the wedges ring (never pressed itself)
 *   DIRECTION  eight annular sectors round the knob, UP at the top, RIGHT at
 *              the right; a diagonal drives two actions
 *   KNOB       the dial: a disc, hit-tested by radius; the angle is the value
 *              (astrosession_knob_from_point)
 *   TRIGGER    the lever at the front
 *   SYSBUTTON  RESET GAME, Reset to CONFIG
 *   OVERLAY    the overlay card's frame round the plate (drawn, never pressed)
 *
 * Units: a keypad key's width is 1.0. The extents are the controls' bounding
 * box; a toolkit adds its own margin. Wedges are annular sectors, so their
 * rectangles are only bounding boxes: a toolkit paints a wedge as the sector
 * of its STICK disc whose direction is its act_mask, between the dial's edge
 * and the disc's edge, and uses astrosession_controls_hit (exact shapes) for
 * hit testing. No colours here.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <pthread.h>
#include <stddef.h>

#include "astrosession.h"
#include "bindings.h"

#define PI_F 3.14159265358979f

/* ---- geometry (layout units) ------------------------------------------------ */

#define KEY_W        1.0f
#define KEY_H        0.8f
#define KEY_LEGEND_H 0.5f                    /* the legend zone above a key */
#define KEY_PITCH_X  1.35f
#define KEY_PITCH_Y  (KEY_LEGEND_H + KEY_H)  /* 1.3 */
#define PLATE_PAD_X  0.45f
#define PLATE_PAD_Y  0.2f
#define PLATE_W      (2 * PLATE_PAD_X + 3 * KEY_PITCH_X + KEY_W)
#define PLATE_H      (2 * PLATE_PAD_Y + 6 * KEY_PITCH_Y)
#define CARD_W       3.6f
#define CARD_GAP     0.3f
#define CARD_H       5.3f
#define CAPTION_H    0.5f
#define DIAL_R       0.9f
#define DIAL_CX      (CARD_W / 2)
#define DIAL_CY      2.2f
#define RING_R       1.6f
#define TRIG_X       2.5f
#define TRIG_Y       4.0f
#define TRIG_W       0.9f
#define TRIG_H       1.1f
#define SYS_W        2.6f
#define SYS_H        0.8f
#define SYS_GAP      0.5f
#define GAP_Y        0.35f

#define ROW_W        (ASTROSESSION_PLAYERS * CARD_W + (ASTROSESSION_PLAYERS - 1) * CARD_GAP)

#define PER_CARD     12                      /* caption, stick, 8 wedges, knob, trigger */
#define TOTAL        (ASTROSESSION_KEY_COUNT + 1 + 2 + ASTROSESSION_PLAYERS * PER_CARD)

static astro_control g_layout[TOTAL];
/* the exact shape of the round parts, parallel to g_layout */
typedef struct {
    float centx, centy;          /* the disc's centre */
    float rinner, router;        /* the ring (wedges); a STICK or KNOB is a disc of router */
    int sector;                  /* wedges: 0..7, 0 = RIGHT, counter-clockwise on screen */
} shape;
static shape g_shape[TOTAL];
static float g_w, g_h;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

/* The eight sectors in order round the disc from RIGHT, counter-clockwise as
 * drawn (so UP, at the top, is 2). */
static const unsigned k_sector_acts[8] = {
    1u << ASTRO_ACT_RIGHT,
    (1u << ASTRO_ACT_UP) | (1u << ASTRO_ACT_RIGHT),
    1u << ASTRO_ACT_UP,
    (1u << ASTRO_ACT_UP) | (1u << ASTRO_ACT_LEFT),
    1u << ASTRO_ACT_LEFT,
    (1u << ASTRO_ACT_DOWN) | (1u << ASTRO_ACT_LEFT),
    1u << ASTRO_ACT_DOWN,
    (1u << ASTRO_ACT_DOWN) | (1u << ASTRO_ACT_RIGHT),
};
static const char *const k_sector_names[8] = {
    "Right", "Up right", "Up", "Up left", "Left", "Down left", "Down", "Down right",
};
static const char *const k_caption[ASTROSESSION_PLAYERS] = {
    "Player 1", "Player 2", "Player 3", "Player 4",
};

static int lowest_bit(unsigned mask)
{
    int bit = 0;
    while (mask && !(mask & 1u)) { mask >>= 1; bit++; }
    return bit;
}

static int g_count;

static astro_control *add(int kind, const char *label, int player, int target,
                          unsigned acts, float xpos, float ypos, float width, float height)
{
    astro_control *ctl = &g_layout[g_count];
    ctl->kind = kind;
    ctl->label = label;
    ctl->player = player;
    ctl->target = target;
    ctl->act_mask = acts;
    ctl->xpos = xpos; ctl->ypos = ypos; ctl->width = width; ctl->height = height;
    g_shape[g_count].sector = -1;
    g_count++;
    return ctl;
}

/* The bounding box of the ring sector `sector` round (centx, centy). */
static void sector_box(float centx, float centy, float rad0, float rad1, int sector,
                       float *xpos, float *ypos, float *width, float *height)
{
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    const float start = ((float)sector * 45.0f - 22.5f) * PI_F / 180.0f;
    int step, edge;

    for (step = 0; step <= 8; step++) {
        const float ang = start + (float)step * (45.0f * PI_F / 180.0f) / 8.0f;
        for (edge = 0; edge < 2; edge++) {
            const float rad = edge ? rad1 : rad0;
            const float posx = centx + rad * cosf(ang), posy = centy - rad * sinf(ang);
            if (posx < minx) minx = posx;
            if (posx > maxx) maxx = posx;
            if (posy < miny) miny = posy;
            if (posy > maxy) maxy = posy;
        }
    }
    *xpos = minx; *ypos = miny; *width = maxx - minx; *height = maxy - miny;
}

static void card(int player, float xorg, float yorg)
{
    const float centx = xorg + DIAL_CX, centy = yorg + DIAL_CY;
    int sector;

    add(ASTRO_CTL_CAPTION, k_caption[player], player, -1, 0, xorg, yorg, CARD_W, CAPTION_H);

    add(ASTRO_CTL_STICK, "Stick", player, -1, 0,
        centx - RING_R, centy - RING_R, 2 * RING_R, 2 * RING_R);
    g_shape[g_count - 1] = (shape){ centx, centy, 0.0f, RING_R, -1 };

    for (sector = 0; sector < 8; sector++) {
        const unsigned acts = k_sector_acts[sector];
        float xpos, ypos, width, height;
        sector_box(centx, centy, DIAL_R + 0.05f, RING_R, sector, &xpos, &ypos, &width, &height);
        add(ASTRO_CTL_DIRECTION, k_sector_names[sector], player,
            /* a diagonal drives two actions: no single target */
            (acts & (acts - 1)) ? -1 : ASTRO_TARGET_PLAYER(player, lowest_bit(acts)), acts,
            xpos, ypos, width, height);
        g_shape[g_count - 1] = (shape){ centx, centy, DIAL_R + 0.05f, RING_R, sector };
    }

    add(ASTRO_CTL_KNOB, "Knob", player, -1, 0,
        centx - DIAL_R, centy - DIAL_R, 2 * DIAL_R, 2 * DIAL_R);
    g_shape[g_count - 1] = (shape){ centx, centy, 0.0f, DIAL_R, -1 };

    add(ASTRO_CTL_TRIGGER, "Trigger", player, ASTRO_TARGET_PLAYER(player, ASTRO_ACT_TRIGGER),
        1u << ASTRO_ACT_TRIGGER, xorg + TRIG_X, yorg + TRIG_Y, TRIG_W, TRIG_H);
}

static void build(void)
{
    const float plate_x = (ROW_W - PLATE_W) / 2;
    const float cards_y = PLATE_H + GAP_Y;
    const float sys_y = cards_y + CARD_H + GAP_Y;
    int key, player;

    g_count = 0;
    add(ASTRO_CTL_OVERLAY, "Overlay", -1, -1, 0, plate_x, 0.0f, PLATE_W, PLATE_H);
    for (key = 0; key < ASTROSESSION_KEY_COUNT; key++)
        add(ASTRO_CTL_KEY, bindings_target_short_label(ASTRO_TARGET_KEYPAD + key), -1,
            ASTRO_TARGET_KEYPAD + key, 0,
            plate_x + PLATE_PAD_X + (float)(key % 4) * KEY_PITCH_X,
            PLATE_PAD_Y + (float)(key / 4) * KEY_PITCH_Y + KEY_LEGEND_H, KEY_W, KEY_H);
    for (player = 0; player < ASTROSESSION_PLAYERS; player++)
        card(player, (float)player * (CARD_W + CARD_GAP), cards_y);

    add(ASTRO_CTL_SYSBUTTON, "RESET", -1, ASTRO_TARGET_SYSACT + ASTROSESSION_SYSACT_RESET_GAME,
        0, (ROW_W - 2 * SYS_W - SYS_GAP) / 2, sys_y, SYS_W, SYS_H);
    add(ASTRO_CTL_SYSBUTTON, "Reset to CONFIG", -1,
        ASTRO_TARGET_SYSACT + ASTROSESSION_SYSACT_RESET_CONFIG,
        0, (ROW_W - 2 * SYS_W - SYS_GAP) / 2 + SYS_W + SYS_GAP, sys_y, SYS_W, SYS_H);

    g_w = ROW_W;
    g_h = sys_y + SYS_H + 0.2f;
}

static void ensure_built(void)
{
    pthread_once(&g_once, build);
}

const astro_control *astrosession_controls_layout(int *count, float *width, float *height)
{
    ensure_built();
    if (count) *count = g_count;
    if (width) *width = g_w;
    if (height) *height = g_h;
    return g_layout;
}

/* The sector (0 = RIGHT, counter-clockwise on screen) a vector lies in. */
static int sector_of(float delx, float dely)
{
    float deg = atan2f(-dely, delx) * 180.0f / PI_F;      /* +y is toward the player */
    int sector;

    if (deg < 0) deg += 360.0f;
    sector = (int)floorf((deg + 22.5f) / 45.0f);
    return sector & 7;
}

int astrosession_controls_hit(float xpos, float ypos)
{
    int idx;

    ensure_built();
    for (idx = g_count - 1; idx >= 0; idx--) {
        const astro_control *ctl = &g_layout[idx];
        const shape *shp = &g_shape[idx];

        if (ctl->kind == ASTRO_CTL_CAPTION || ctl->kind == ASTRO_CTL_OVERLAY)
            continue;
        if (ctl->kind == ASTRO_CTL_STICK || ctl->kind == ASTRO_CTL_DIRECTION ||
            ctl->kind == ASTRO_CTL_KNOB) {
            const float delx = xpos - shp->centx, dely = ypos - shp->centy;
            const float dist = sqrtf(delx * delx + dely * dely);

            if (dist > shp->router || dist < shp->rinner)
                continue;
            if (ctl->kind == ASTRO_CTL_DIRECTION && sector_of(delx, dely) != shp->sector)
                continue;
            return idx;
        }
        if (xpos >= ctl->xpos && xpos < ctl->xpos + ctl->width
            && ypos >= ctl->ypos && ypos < ctl->ypos + ctl->height)
            return idx;
    }
    return -1;
}

unsigned astrosession_stick_from_vector(float delx, float dely, float radius)
{
    if (radius <= 0.0f)
        return 0;
    if (sqrtf(delx * delx + dely * dely) < radius * 0.25f)
        return 0;
    return k_sector_acts[sector_of(delx, dely)];
}
