/*
 * The Controllers window -- the Bally Astrocade's own controls drawn as a
 * player sees them: the console's 24-key calculator keypad (champagne plate,
 * gold operator column, dark keys) set in the black-and-walnut console, and
 * under it all four pistol-grip hand controllers, each a black grip with the
 * gold Bally badge, a ribbed orange-brown knob (the paddle) ringed by eight
 * direction wedges, and the trigger at the front. Everything is drawn from the
 * session's layout (astrosession_controls_layout), so every frontend shows the
 * same controls.
 *
 * The Bally BASIC overlay: with the check on, the keypad is the overlay card --
 * a black legend bar over every key with the green, red and blue letters, and
 * the key itself carrying its white legend and gold word. The host keyboard
 * then types BASIC through it (astrosession_type_char; see window.c).
 *
 * Mouse: a press on a key or the trigger holds it, a press on the stick's ring
 * of wedges tilts the grip toward the pointer and a drag follows, a press on
 * the knob turns it to the pointer's angle and a drag follows (the scroll wheel
 * nudges it). Every press is the raw press and release (a GtkGestureDrag, not a
 * click: a click gives up when the pointer wobbles), because the machine samples
 * its controls once per frame. One press holds one control: chords come from the
 * keyboard or a gamepad. Losing the focus, hiding and the pointer leaving a held
 * key let go of whatever the mouse holds here.
 *
 * What the machine sees held -- keyboard, gamepads and this window together
 * (astrosession_target_held) -- is polled every frame and painted in the
 * accent, so the window doubles as a gamepad tester; the knobs show the live
 * knob position.
 *
 * Keys typed with the window active go through the main window
 * (astro_window_key), so the machine, F9 and the shortcuts behave the same
 * whichever window has the focus.
 *
 * FIXED SIZE, FLOATING: the console is a drawing area of one size, the labels
 * ellipsize rather than grow, and the window is not resizable, so neither Map
 * mode nor a long gamepad name changes its size and tiling compositors float
 * it. It is hidden at startup (F9 or the menu shows it).
 *
 * SINGLETON, hidden rather than destroyed, so a remap survives closing it.
 * Its timers run only while it is on screen.
 *
 * MAP MODE: the Map button arms a two-step rebind -- click a control to pick
 * the target, then press a keyboard key OR a gamepad input (the knob takes an
 * axis). While armed the same gestures pick the target instead of pressing,
 * every control shows the input bound to it, and typed keys are not typed into
 * the console; once a target is picked the other controls are greyed.
 * Rebinding steals the input from whatever held it. Escape cancels. Clear takes
 * the picked target's key and input away, Defaults puts every binding back.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controllers_window.h"

#include "../help/help_window.h"
#include "../window.h"
#include "bindings.h"

#include <math.h>
#include <pango/pangocairo.h>
#include <string.h>

#define UNIT 36.0               /* pixels per layout unit (a keypad key's width) */
#define FRAME 8                 /* the walnut frame */
#define TOP_PAD 10              /* the black top inside it */
#define BRAND_H 20              /* the brand strip along the top */
#define TOP_GAP 8
#define MAX_PARTS 96
#define KEY_ZONE 0.5            /* the legend zone above a key, units (core layout) */
#define CARD_H 5.3              /* one hand controller's card, units (core layout) */
#define DIR_BITS ((1u << ASTRO_ACT_UP) | (1u << ASTRO_ACT_DOWN) | \
                  (1u << ASTRO_ACT_LEFT) | (1u << ASTRO_ACT_RIGHT))
#define KNOB_NUDGE 8            /* one scroll step turns the knob by this much */
#define KB_PER_PLAYER 2         /* the knob-key buttons: turn left, turn right */
#define KB_COUNT (ASTROSESSION_PLAYERS * KB_PER_PLAYER)

/* ---- singleton state ------------------------------------------------------ */

static GtkWidget *g_window;
static GtkWindow *g_parent;     /* the AstroWindow: typed keys go through it */
static astrosession *g_session;
static GtkWidget *g_console;    /* the controls' drawing area */
static GtkWidget *g_overlay_check;

static const astro_control *g_layout;  /* astrosession_controls_layout: static */
static int g_part_count;
static float g_layout_wid, g_layout_hgt;
static int g_key_part[ASTROSESSION_KEY_COUNT];  /* the layout's KEY part per key */
static int g_card_part[ASTROSESSION_PLAYERS];   /* the layout's CAPTION part per player */
static gboolean g_lit[MAX_PARTS];       /* what the machine sees held */
static gboolean g_kb_lit[KB_COUNT];
static int g_knob_val[ASTROSESSION_PLAYERS];
static gboolean g_overlay_on;           /* the overlay as last painted */
static int g_hover = -1;                /* the part under the pointer */
static int g_hover_kb = -1;             /* the knob-key button under the pointer */
static int g_pressed = -1;              /* a key or the trigger the mouse holds */
static int g_pressed_kb = -1;           /* a knob-key button the mouse holds */
static int g_pressed_sys = -1;          /* a console button the mouse holds */
static double g_ptr_x, g_ptr_y;         /* the pointer, for the scroll wheel */

/* What the mouse is dragging: a stick's wedges or a knob's dial. */
static struct {
    int kind;                   /* 0 none, 1 stick, 2 knob */
    int player;
    double cenx, ceny, rad;     /* the disc, pixels */
} g_drag;

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 waiting for a
 * key or pad input to bind to that target. */
static int g_map_state = -2;
static GtkWidget *g_map_button, *g_clear_button;
static GtkWidget *g_map_hint;
static GtkWidget *g_show_bindings;
#define MAX_PADS    8           /* pads listed (SDL opens few more) */
#define PAD_ROWS    2           /* pad rows the window reserves; more scroll */
#define PAD_ROW_H   34          /* one row's height, px */
#define PAD_GAP     4
static GtkWidget *g_pad_box;
static GtkWidget *g_pad_eff[MAX_PADS];  /* each pad's "plays player N" */
static int g_pad_row_count;
static unsigned g_pad_generation = (unsigned)-1;
static guint g_capture_timer;
static guint g_swallow_code;    /* the key Map took: its repeats and release too */
static guint g_info_timer;

static void refresh_all(void);

/* "controllers_show_bindings"; the older spellings are read when it is absent. */
static gboolean show_bindings_saved(void)
{
    static const char *const older[] = { "controllers_bindings", "controllers_key_names",
                                         "show_bindings" };
    int val = astrosession_get_int(g_session, "controllers_show_bindings", -1);
    size_t idx;
    for (idx = 0; val < 0 && idx < G_N_ELEMENTS(older); idx++)
        val = astrosession_get_int(g_session, older[idx], -1);
    return val > 0;
}

/* ---- small helpers ---------------------------------------------------------- */

static void rgb(cairo_t *cctx, guint32 col)
{
    cairo_set_source_rgb(cctx, ((col >> 16) & 0xFF) / 255.0, ((col >> 8) & 0xFF) / 255.0,
                         (col & 0xFF) / 255.0);
}

static guint32 mix(guint32 from, guint32 dest, double frac)
{
    guint32 out = 0;
    int shift;
    for (shift = 16; shift >= 0; shift -= 8) {
        double chan_from = (from >> shift) & 0xFF, chan_dest = (dest >> shift) & 0xFF;
        out |= (guint32)(chan_from + (chan_dest - chan_from) * frac + 0.5) << shift;
    }
    return out;
}

static void rrect(cairo_t *cctx, double xpos, double ypos, double wid, double hgt, double rad)
{
    if (rad > wid / 2) rad = wid / 2;
    if (rad > hgt / 2) rad = hgt / 2;
    cairo_new_sub_path(cctx);
    cairo_arc(cctx, xpos + wid - rad, ypos + rad, rad, -G_PI / 2, 0);
    cairo_arc(cctx, xpos + wid - rad, ypos + hgt - rad, rad, 0, G_PI / 2);
    cairo_arc(cctx, xpos + rad, ypos + hgt - rad, rad, G_PI / 2, G_PI);
    cairo_arc(cctx, xpos + rad, ypos + rad, rad, G_PI, 3 * G_PI / 2);
    cairo_close_path(cctx);
}

static void stop_rgb(cairo_pattern_t *pat, double offset, guint32 col)
{
    cairo_pattern_add_color_stop_rgb(pat, offset, ((col >> 16) & 0xFF) / 255.0,
                                     ((col >> 8) & 0xFF) / 255.0, (col & 0xFF) / 255.0);
}

/* Text centred in a box, in an absolute pixel size, ellipsized to the box. */
static void legend(cairo_t *cctx, GtkWidget *widget, const char *text, double pxsize,
                   gboolean bold, guint32 colour, double xpos, double ypos, double boxw,
                   double boxh)
{
    PangoLayout *lay = gtk_widget_create_pango_layout(widget, text);
    PangoFontDescription *fdesc = pango_font_description_new();
    int textw, texth;

    pango_font_description_set_family(fdesc, "Sans");
    pango_font_description_set_weight(fdesc, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_font_description_set_absolute_size(fdesc, pxsize * PANGO_SCALE);
    pango_layout_set_font_description(lay, fdesc);
    pango_layout_set_width(lay, (int)(boxw * PANGO_SCALE));
    pango_layout_set_ellipsize(lay, PANGO_ELLIPSIZE_END);
    pango_layout_set_alignment(lay, PANGO_ALIGN_CENTER);
    pango_layout_get_pixel_size(lay, &textw, &texth);
    rgb(cctx, colour);
    cairo_move_to(cctx, xpos, ypos + (boxh - texth) / 2.0);
    pango_cairo_show_layout(cctx, lay);
    /* the move_to above is still the current point, and the next arc would
     * join it with a line */
    cairo_new_path(cctx);
    pango_font_description_free(fdesc);
    g_object_unref(lay);
}

/* A host key's name as a small legend: the long ones shortened to fit. */
static void legend_name(guint32 keysym, char *dst, int dstsz)
{
    char name[32];
    astrosession_keysym_name(keysym, name, sizeof name);
    if (!strcmp(name, "Keypad Enter"))
        g_snprintf(dst, dstsz, "KP Ent");
    else if (g_str_has_prefix(name, "Keypad "))
        g_snprintf(dst, dstsz, "KP %s", name + 7);
    else if (g_str_has_prefix(name, "Left "))
        g_snprintf(dst, dstsz, "L %s", name + 5);
    else if (g_str_has_prefix(name, "Right "))
        g_snprintf(dst, dstsz, "R %s", name + 6);
    else if (!strcmp(name, "Backspace"))
        g_snprintf(dst, dstsz, "BkSp");
    else if (!strcmp(name, "Escape"))
        g_snprintf(dst, dstsz, "Esc");
    else
        g_snprintf(dst, dstsz, "%s", name);
}

static gboolean is_sysact(int target)
{
    return target >= ASTRO_TARGET_SYSACT && target < ASTRO_TARGET_PLAYERS;
}

static gboolean is_knob_target(int target)
{
    return target >= ASTRO_TARGET_PLAYERS &&
           (target - ASTRO_TARGET_PLAYERS) % ASTRO_ACT_PER_PLAYER == ASTRO_ACT_KNOB;
}

static gboolean showing_bindings(void)
{
    return g_map_state != -2 ||
           gtk_check_button_get_active(GTK_CHECK_BUTTON(g_show_bindings));
}

/* The binding legend for a target, or "" when none or not shown. The knob's is
 * the pad axis that turns it, every other target's the key. */
static void bound_text(int target, char *dst, int dstsz)
{
    dst[0] = '\0';
    if (target < 0 || !showing_bindings()) return;
    if (is_knob_target(target)) {
        const int pad = bindings_target_pad(target);
        if (pad >= 0) {
            /* "Right Stick X" does not fit a knob's face: "RS X" */
            char name[48];
            astrosession_pad_input_name(pad, name, sizeof name);
            if (g_str_has_prefix(name, "Left Stick "))
                g_snprintf(dst, dstsz, "LS %s", name + 11);
            else if (g_str_has_prefix(name, "Right Stick "))
                g_snprintf(dst, dstsz, "RS %s", name + 12);
            else if (!strcmp(name, "Left Trigger"))
                g_snprintf(dst, dstsz, "LT");
            else if (!strcmp(name, "Right Trigger"))
                g_snprintf(dst, dstsz, "RT");
            else
                g_snprintf(dst, dstsz, "%s", name);
        } else
            g_snprintf(dst, dstsz, "\xe2\x80\x94");
        return;
    }
    if (bindings_target_keysym(target))
        legend_name(bindings_target_keysym(target), dst, dstsz);
    else
        g_snprintf(dst, dstsz, "\xe2\x80\x94");
}

/* ---- geometry ---------------------------------------------------------------- */

#define ORG_X (FRAME + TOP_PAD)
#define ORG_Y (FRAME + TOP_PAD + BRAND_H + TOP_GAP)

static double pix_x(double unitx) { return ORG_X + unitx * UNIT; }
static double pix_y(double unity) { return ORG_Y + unity * UNIT; }

static int console_width(void)
{
    return (int)(2 * ORG_X + g_layout_wid * UNIT + 0.5);
}

static int console_height(void)
{
    return (int)(ORG_Y + g_layout_hgt * UNIT + FRAME + TOP_PAD + 0.5);
}

/* A disc of the layout (the stick's, or the knob's) of a player: its centre
 * and radius in pixels, or FALSE. */
static gboolean disc_of(int kind, int player, double *cenx, double *ceny, double *rad)
{
    int idx;
    for (idx = 0; idx < g_part_count; idx++)
        if (g_layout[idx].kind == kind && g_layout[idx].player == player) {
            *cenx = pix_x(g_layout[idx].xpos + g_layout[idx].width / 2);
            *ceny = pix_y(g_layout[idx].ypos + g_layout[idx].height / 2);
            *rad = g_layout[idx].width * UNIT / 2;
            return TRUE;
        }
    return FALSE;
}

/* The two small buttons on each grip that turn the knob left and right while
 * held (the KNOB_DEC and KNOB_INC targets; the core's layout has no part for
 * them). Rectangle in pixels. */
static void kb_rect(int idx, double *xpos, double *ypos, double *wid, double *hgt)
{
    const int player = idx / KB_PER_PLAYER, right = idx % KB_PER_PLAYER;
    const astro_control *card = &g_layout[g_card_part[player]];
    *xpos = pix_x(card->xpos + (right ? 1.75 : 0.75));
    *ypos = pix_y(card->ypos + 4.55);
    *wid = 0.5 * UNIT;
    *hgt = 0.5 * UNIT;
}

static int kb_target(int idx)
{
    return ASTRO_TARGET_PLAYER(idx / KB_PER_PLAYER,
                               idx % KB_PER_PLAYER ? ASTRO_ACT_KNOB_INC : ASTRO_ACT_KNOB_DEC);
}

static int kb_hit(double xpos, double ypos)
{
    int idx;
    for (idx = 0; idx < KB_COUNT; idx++) {
        double bx, by, bw, bh;
        kb_rect(idx, &bx, &by, &bw, &bh);
        if (xpos >= bx && xpos < bx + bw && ypos >= by && ypos < by + bh)
            return idx;
    }
    return -1;
}

/* What Map binds when a part is clicked: its target, the knob's axis for the
 * dial, or -1 (the stick, a diagonal wedge, the overlay). */
static int part_target(const astro_control *ctl)
{
    if (ctl->target >= 0)
        return ctl->target;
    if (ctl->kind == ASTRO_CTL_KNOB && ctl->player >= 0)
        return ASTRO_TARGET_PLAYER(ctl->player, ASTRO_ACT_KNOB);
    return -1;
}

/* ---- drawing ------------------------------------------------------------------- */

/* The Astrocade's own plastic and paper (DESIGN-astrocade.md): the walnut
 * console with its black top and gold pinstripe, the champagne keypad plate
 * with its gold column, the black glossy grips with the ribbed orange-brown
 * knobs, the ivory overlay card with its black legend bars. */
#define COL_WALNUT_TOP 0x9A6638
#define COL_WALNUT_BOT 0x70482A
#define COL_WALNUT_EDGE 0x3E2614
#define COL_WALNUT_LIGHT 0xB47E4C
#define COL_TOP 0x262524
#define COL_TOP_EDGE 0x0E0E0D
#define COL_PINSTRIPE 0xB08A3C
#define COL_BRAND_INK 0xD4B04C
#define COL_PLATE_TOP 0xE2CFAE
#define COL_PLATE_BOT 0xD4C19D
#define COL_PLATE_EDGE 0x8E7C52
#define COL_PLATE_FRAME 0xC9A64E
#define COL_GOLD_COLUMN 0xCFAE5E
#define COL_PLATE_INK 0x4A3A30
#define COL_KEY 0x342626
#define COL_KEY_HOT 0x4A3536
#define COL_KEY_SIDE 0x150E0E
#define COL_KEY_INK 0xE8DCC8
#define COL_CARD 0xFBEFDD
#define COL_CARD_EDGE 0xB8AC96
#define COL_BAR 0x1A1A1A
#define COL_WHITE 0xF2F2F2
#define COL_WORDS 0xE6BC4C
#define COL_GREEN 0x3FA55A
#define COL_RED 0xE04A42
#define COL_BLUE 0x2878B0
#define COL_BAY_TOP 0x3E3A37
#define COL_BAY_BOT 0x2B2826
#define COL_BAY_EDGE 0x121110
#define COL_BAY_INK 0xC9A64E
#define COL_SHELL_TOP 0x2A2828
#define COL_SHELL_BOT 0x111010
#define COL_SHELL_HI 0x777476
#define COL_KNURL 0x4C3D3D
#define COL_KNURL_BASE 0x252121
#define COL_BADGE 0xD4A93A
#define COL_KNOB_TOP 0xE67A2C
#define COL_KNOB_BOT 0xB35728
#define COL_KNOB_COLLAR 0x8A4318
#define COL_KNOB_RIB 0x5F2E1D
#define COL_KNOB_INK 0x3A1A06
#define COL_WEDGE 0x2E2A2A
#define COL_WEDGE_HOT 0x463F3F
#define COL_WEDGE_INK 0x8A8080
#define COL_TRIG_TOP 0xB4B4BA
#define COL_TRIG_BOT 0x84848A
#define COL_SYS_FACE 0x181716
#define COL_SYS_HOT 0x2A2826
#define COL_SYS_SIDE 0x070707
#define COL_SYS_INK 0xD4B04C
#define COL_ACCENT ASTROSESSION_ACCENT_RGB
#define COL_HELD_INK 0x2A1F00

typedef struct {
    gboolean held, selected, dim, hot;
} PartState;

static PartState part_state(int idx)
{
    const astro_control *ctl = &g_layout[idx];
    const int target = part_target(ctl);
    PartState pst;
    const gboolean armed = g_map_state != -2;
    pst.held = g_lit[idx] || (g_pressed_sys >= 0 && g_pressed_sys == idx);
    pst.selected = g_map_state >= 0 && target >= 0 && g_map_state == target;
    pst.dim = armed && (target < 0 || (g_map_state >= 0 && !pst.selected));
    pst.hot = g_hover == idx && !pst.held;
    return pst;
}

/* Everything a part draws between begin and end is dimmed to 40% in Map mode
 * when it is not the picked target. */
static void dim_begin(cairo_t *cctx, const PartState *pst)
{
    if (pst->dim) cairo_push_group(cctx);
}

static void dim_end(cairo_t *cctx, const PartState *pst)
{
    if (pst->dim) {
        cairo_pop_group_to_source(cctx);
        cairo_paint_with_alpha(cctx, 0.4);
    }
}

/* The champagne keypad plate with its gold frame and the gold operator column,
 * or -- with the overlay on -- the ivory overlay card laid over it. The keys
 * and their legends are drawn over it. Inert. */
static void draw_plate(cairo_t *cctx, const astro_control *ctl)
{
    const double xpos = pix_x(ctl->xpos), ypos = pix_y(ctl->ypos);
    const double wid = ctl->width * UNIT, hgt = ctl->height * UNIT;
    cairo_pattern_t *grad = cairo_pattern_create_linear(0, ypos, 0, ypos + hgt);
    int row;

    if (g_overlay_on) {
        stop_rgb(grad, 0, COL_CARD);
        stop_rgb(grad, 1, mix(COL_CARD, 0xE8DAC0, 0.6));
    } else {
        stop_rgb(grad, 0, COL_PLATE_TOP);
        stop_rgb(grad, 1, COL_PLATE_BOT);
    }
    rrect(cctx, xpos, ypos, wid, hgt, 8);
    cairo_set_source(cctx, grad);
    cairo_fill_preserve(cctx);
    cairo_pattern_destroy(grad);
    rgb(cctx, g_overlay_on ? COL_CARD_EDGE : COL_PLATE_EDGE);
    cairo_set_line_width(cctx, 1);
    cairo_stroke(cctx);

    if (g_overlay_on) {
        /* a black legend bar over each row of keys, the card's own rule */
        for (row = 0; row < 6; row++) {
            const astro_control *first = &g_layout[g_key_part[row * 4]];
            const astro_control *last = &g_layout[g_key_part[row * 4 + 3]];
            const double bx = pix_x(first->xpos) - 0.175 * UNIT;
            const double bw = pix_x(last->xpos + last->width) + 0.175 * UNIT - bx;
            rrect(cctx, bx, pix_y(first->ypos - KEY_ZONE), bw, KEY_ZONE * UNIT, 3);
            rgb(cctx, COL_BAR);
            cairo_fill(cctx);
        }
        return;
    }

    /* the gold pinstripe frame round the keypad, and the gold operator column
     * (the divide key down to the equals key) */
    rrect(cctx, xpos + 5, ypos + 5, wid - 10, hgt - 10, 5);
    rgb(cctx, COL_PLATE_FRAME);
    cairo_set_line_width(cctx, 1.5);
    cairo_stroke(cctx);
    {
        const astro_control *top = &g_layout[g_key_part[ASTROSESSION_KEY_DIV]];
        const astro_control *bot = &g_layout[g_key_part[ASTROSESSION_KEY_EQ]];
        const double cx0 = pix_x(top->xpos) - 0.175 * UNIT;
        const double cy0 = pix_y(top->ypos - KEY_ZONE);
        const double cy1 = pix_y(bot->ypos + bot->height) + 3;
        rrect(cctx, cx0, cy0, top->width * UNIT + 0.35 * UNIT, cy1 - cy0, 4);
        rgb(cctx, COL_GOLD_COLUMN);
        cairo_fill(cctx);
    }
}

/* One shift colour's small box over a key: a white box with the letter in the
 * colour (a blank box when the overlay prints nothing there). */
static void draw_shift_box(cairo_t *cctx, GtkWidget *widget, double xpos, double ypos,
                           double wid, double hgt, const char *text, guint32 colour)
{
    rrect(cctx, xpos, ypos, wid, hgt, 2);
    rgb(cctx, COL_WHITE);
    cairo_fill(cctx);
    if (text && text[0])
        legend(cctx, widget, text, 10, TRUE, colour, xpos, ypos, wid, hgt);
}

/* The Bally BASIC overlay's printing for one key: the three coloured boxes in
 * the black bar over it, and on the key itself the white legend and the gold
 * word. The bottom row (CE 0 . =) prints the four shifts instead. */
static void draw_overlay_key(cairo_t *cctx, GtkWidget *widget, int key, const PartState *pst)
{
    const astro_control *ctl = &g_layout[g_key_part[key]];
    const astro_overlay_key *leg = &astrosession_overlay_layout()[key];
    const double xpos = pix_x(ctl->xpos), ypos = pix_y(ctl->ypos);
    const double wid = ctl->width * UNIT, hgt = ctl->height * UNIT - 3;
    const double zone_y = ypos - KEY_ZONE * UNIT, zone_h = KEY_ZONE * UNIT;
    const int shift = astrosession_overlay_key_shift(key);
    static const guint32 shift_col[ASTRO_OVERLAY_SHIFTS] =
        { 0, COL_GREEN, COL_RED, COL_BLUE, COL_WORDS };
    const guint32 ink = pst->held || pst->selected ? COL_HELD_INK : COL_KEY_INK;

    if (shift >= 0) {
        /* a shift: its colour's dot in the bar and on the key, or WORDS in gold */
        if (shift == ASTRO_SHIFT_WORDS) {
            legend(cctx, widget, "WORDS", 9, TRUE, COL_WORDS, xpos - 4, zone_y, wid + 8, zone_h);
            legend(cctx, widget, "WORDS", 8, TRUE, pst->held || pst->selected ? COL_HELD_INK : COL_WORDS,
                   xpos, ypos, wid, hgt);
        } else {
            cairo_arc(cctx, xpos + wid / 2, zone_y + zone_h / 2, 4.5, 0, 2 * G_PI);
            rgb(cctx, shift_col[shift]);
            cairo_fill(cctx);
            cairo_arc(cctx, xpos + wid / 2, ypos + hgt / 2, 6, 0, 2 * G_PI);
            rgb(cctx, shift_col[shift]);
            cairo_fill(cctx);
        }
        return;
    }
    /* the three boxes: green, red, blue */
    {
        const double bw = 14, gap = 1.5, hgt_box = zone_h - 5;
        const double bx = pix_x(ctl->xpos + ctl->width / 2) - (3 * bw + 2 * gap) / 2;
        int col;
        for (col = 0; col < 3; col++)
            draw_shift_box(cctx, widget, bx + col * (bw + gap), zone_y + 2.5, bw, hgt_box,
                           leg->legend[ASTRO_SHIFT_GREEN + col],
                           shift_col[ASTRO_SHIFT_GREEN + col]);
    }
    /* on the key: the white legend, the gold word under it */
    {
        const char *white = leg->legend[ASTRO_SHIFT_NONE];
        const char *words = leg->legend[ASTRO_SHIFT_WORDS];
        const double white_h = words ? hgt * 0.62 : hgt;
        if (white && !strcmp(white, "HALT")) {
            /* the circled H: a hexagon with the letter, the one key held */
            const double cenx = xpos + wid / 2, ceny = ypos + white_h / 2;
            const double rad = MIN(wid * 0.26, white_h * 0.52);
            int vtx;
            for (vtx = 0; vtx < 6; vtx++) {
                const double ang = vtx * G_PI / 3 + G_PI / 6;
                if (vtx) cairo_line_to(cctx, cenx + cos(ang) * rad, ceny + sin(ang) * rad);
                else cairo_move_to(cctx, cenx + cos(ang) * rad, ceny + sin(ang) * rad);
            }
            cairo_close_path(cctx);
            rgb(cctx, ink);
            cairo_set_line_width(cctx, 1.5);
            cairo_stroke(cctx);
            legend(cctx, widget, "H", 10, TRUE, ink, xpos, ypos, wid, white_h);
        } else if (white) {
            legend(cctx, widget, white, strlen(white) > 3 ? 9 : 13, TRUE, ink, xpos, ypos, wid,
                   white_h);
        }
        if (words)
            legend(cctx, widget, words, 8, TRUE,
                   pst->held || pst->selected ? COL_HELD_INK : COL_WORDS,
                   xpos - 2, ypos + white_h - 3, wid + 4, hgt - white_h + 3);
    }
}

/* A keypad key: the dark brown rubber cap, its legend printed on the plate
 * above it (or the overlay's printing), the bound key on the cap when shown. */
static void draw_key(cairo_t *cctx, GtkWidget *widget, int idx)
{
    const astro_control *ctl = &g_layout[idx];
    const PartState pst = part_state(idx);
    const double xpos = pix_x(ctl->xpos), ypos = pix_y(ctl->ypos);
    const double wid = ctl->width * UNIT, hgt = ctl->height * UNIT - 3;
    const gboolean lit = pst.held || pst.selected;
    guint32 face = pst.hot ? COL_KEY_HOT : COL_KEY;
    char bound[40];
    const int key = ctl->target - ASTRO_TARGET_KEYPAD;

    dim_begin(cctx, &pst);
    if (g_overlay_on) {
        /* the card's window: the key shows through a white-edged hole */
        rrect(cctx, xpos - 2.5, ypos - 1.5, wid + 5, hgt + 6, 4);
        rgb(cctx, COL_WHITE);
        cairo_fill(cctx);
    } else {
        /* the console's own legend, printed on the plate above the key */
        legend(cctx, widget, ctl->label, 12, TRUE, COL_PLATE_INK, xpos - 4,
               ypos - KEY_ZONE * UNIT, wid + 8, KEY_ZONE * UNIT);
    }
    if (lit) face = COL_ACCENT;
    rrect(cctx, xpos, ypos + 3, wid, hgt, 4);
    rgb(cctx, lit ? mix(COL_ACCENT, 0x000000, 0.45) : COL_KEY_SIDE);
    cairo_fill(cctx);
    rrect(cctx, xpos, ypos, wid, hgt, 4);
    rgb(cctx, face);
    cairo_fill_preserve(cctx);
    rgb(cctx, lit ? COL_HELD_INK : mix(face, 0x000000, 0.5));
    cairo_set_line_width(cctx, lit ? 2 : 1);
    cairo_stroke(cctx);
    if (!lit) {
        /* a light edge along the cap's top */
        cairo_move_to(cctx, xpos + 4, ypos + 1.5);
        cairo_line_to(cctx, xpos + wid - 4, ypos + 1.5);
        rgb(cctx, mix(COL_KEY, 0xFFFFFF, 0.28));
        cairo_set_line_width(cctx, 1);
        cairo_stroke(cctx);
    }

    if (g_overlay_on && key >= 0 && key < ASTROSESSION_KEY_COUNT)
        draw_overlay_key(cctx, widget, key, &pst);
    bound_text(ctl->target, bound, sizeof bound);
    if (bound[0]) {
        if (g_overlay_on) {
            /* the key's printing is in the way: the binding goes in the bar */
            legend(cctx, widget, bound, 8, TRUE, COL_BAY_INK, xpos, ypos + hgt - 11, wid, 11);
        } else {
            legend(cctx, widget, bound, 9, TRUE, lit ? COL_HELD_INK : COL_KEY_INK, xpos, ypos, wid,
                   hgt);
        }
    }
    dim_end(cctx, &pst);
}

/* The screen angle of a wedge from its direction bits (y down). */
static double wedge_angle(unsigned acts)
{
    const double delx = (int)((acts >> ASTRO_ACT_RIGHT) & 1) - (int)((acts >> ASTRO_ACT_LEFT) & 1);
    const double dely = (int)((acts >> ASTRO_ACT_DOWN) & 1) - (int)((acts >> ASTRO_ACT_UP) & 1);
    return atan2(dely, delx);
}

/* One of the eight wedges: the sector of the stick disc between the knob's edge
 * and the disc's edge. */
static void draw_wedge(cairo_t *cctx, GtkWidget *widget, int idx, double cenx, double ceny,
                       double ringr, double dialr)
{
    const astro_control *ctl = &g_layout[idx];
    const PartState pst = part_state(idx);
    const double ang = wedge_angle(ctl->act_mask), span = 21.0 * G_PI / 180.0;
    const double rad0 = dialr + 3, rad1 = ringr - 1;
    const double mid = (rad0 + rad1) / 2;
    const double glyx = cenx + cos(ang) * mid, glyy = ceny + sin(ang) * mid;
    const gboolean lit = pst.held || pst.selected;
    guint32 face = lit ? COL_ACCENT : pst.hot ? COL_WEDGE_HOT : COL_WEDGE;
    guint32 ink = lit ? COL_HELD_INK : COL_WEDGE_INK;
    char bound[40];

    dim_begin(cctx, &pst);
    cairo_arc(cctx, cenx, ceny, rad1, ang - span, ang + span);
    cairo_arc_negative(cctx, cenx, ceny, rad0, ang + span, ang - span);
    cairo_close_path(cctx);
    rgb(cctx, face);
    cairo_fill_preserve(cctx);
    rgb(cctx, lit ? COL_HELD_INK : mix(face, 0x000000, 0.5));
    cairo_set_line_width(cctx, lit ? 2 : 1);
    cairo_stroke(cctx);

    bound_text(ctl->target, bound, sizeof bound);
    if (bound[0]) {
        legend(cctx, widget, bound, 8, TRUE, lit ? COL_HELD_INK : 0xC8BEBE, glyx - 16, glyy - 7,
               32, 14);
    } else {
        /* an arrow pointing the way the grip tilts */
        const double unitx = cos(ang), unity = sin(ang);
        cairo_move_to(cctx, glyx + unitx * 7, glyy + unity * 7);
        cairo_line_to(cctx, glyx - unitx * 4 - unity * 6, glyy - unity * 4 + unitx * 6);
        cairo_line_to(cctx, glyx - unitx * 4 + unity * 6, glyy - unity * 4 - unitx * 6);
        cairo_close_path(cctx);
        rgb(cctx, ink);
        cairo_fill(cctx);
    }
    dim_end(cctx, &pst);
}

/* The black glossy ring the wedges sit in, round the knob. */
static void draw_ring(cairo_t *cctx, double cenx, double ceny, double rad)
{
    cairo_pattern_t *grad = cairo_pattern_create_radial(cenx - rad * 0.3, ceny - rad * 0.4,
                                                         rad * 0.1, cenx, ceny, rad + 4);
    stop_rgb(grad, 0, COL_SHELL_TOP);
    stop_rgb(grad, 1, COL_SHELL_BOT);
    cairo_arc(cctx, cenx, ceny, rad + 3, 0, 2 * G_PI);
    cairo_set_source(cctx, grad);
    cairo_fill_preserve(cctx);
    cairo_pattern_destroy(grad);
    rgb(cctx, COL_SHELL_HI);
    cairo_set_line_width(cctx, 1);
    cairo_stroke(cctx);
}

/* The ribbed orange-brown knob: a fluted rim, a collar, the top face with the
 * player's number (or the bound axis), and a notch showing where it is turned. */
static void draw_dial(cairo_t *cctx, GtkWidget *widget, int idx, double cenx, double ceny,
                      double rad)
{
    const astro_control *ctl = &g_layout[idx];
    const PartState pst = part_state(idx);
    const int target = part_target(ctl);
    const double ang = astrosession_knob_angle(g_knob_val[ctl->player]) * G_PI / 180.0;
    cairo_pattern_t *grad;
    char bound[40];
    int rib;

    dim_begin(cctx, &pst);
    /* the collar */
    cairo_arc(cctx, cenx, ceny, rad + 3, 0, 2 * G_PI);
    rgb(cctx, COL_KNOB_COLLAR);
    cairo_fill(cctx);
    /* the fluted rim */
    cairo_arc(cctx, cenx, ceny, rad, 0, 2 * G_PI);
    rgb(cctx, COL_KNOB_BOT);
    cairo_fill(cctx);
    for (rib = 0; rib < 24; rib++) {
        const double ribang = rib * G_PI / 12;
        cairo_move_to(cctx, cenx + cos(ribang) * rad * 0.8, ceny + sin(ribang) * rad * 0.8);
        cairo_line_to(cctx, cenx + cos(ribang) * rad, ceny + sin(ribang) * rad);
    }
    rgb(cctx, COL_KNOB_RIB);
    cairo_set_line_width(cctx, 1.6);
    cairo_stroke(cctx);
    /* the top face */
    grad = cairo_pattern_create_radial(cenx - rad * 0.3, ceny - rad * 0.3, rad * 0.1, cenx, ceny,
                                       rad * 0.82);
    stop_rgb(grad, 0, COL_KNOB_TOP);
    stop_rgb(grad, 1, COL_KNOB_BOT);
    cairo_arc(cctx, cenx, ceny, rad * 0.8, 0, 2 * G_PI);
    cairo_set_source(cctx, grad);
    cairo_fill_preserve(cctx);
    cairo_pattern_destroy(grad);
    rgb(cctx, pst.selected ? COL_ACCENT : mix(COL_KNOB_BOT, 0x000000, 0.4));
    cairo_set_line_width(cctx, pst.selected ? 3 : 1);
    cairo_stroke(cctx);

    /* where it is turned to */
    cairo_move_to(cctx, cenx + sin(ang) * rad * 0.5, ceny - cos(ang) * rad * 0.5);
    cairo_line_to(cctx, cenx + sin(ang) * rad * 0.78, ceny - cos(ang) * rad * 0.78);
    rgb(cctx, COL_KNOB_INK);
    cairo_set_line_width(cctx, 3);
    cairo_set_line_cap(cctx, CAIRO_LINE_CAP_ROUND);
    cairo_stroke(cctx);
    cairo_set_line_cap(cctx, CAIRO_LINE_CAP_BUTT);

    bound_text(target, bound, sizeof bound);
    if (bound[0]) {
        legend(cctx, widget, bound, 8, TRUE, COL_KNOB_INK, cenx - rad * 0.5, ceny - 6, rad, 12);
    } else {
        char num[8];
        g_snprintf(num, sizeof num, "%d", ctl->player + 1);
        legend(cctx, widget, num, 13, TRUE, COL_KNOB_INK, cenx - 10, ceny - 8, 20, 16);
    }
    dim_end(cctx, &pst);
}

/* The trigger: a pale lever at the front of the grip. */
static void draw_trigger(cairo_t *cctx, GtkWidget *widget, int idx)
{
    const astro_control *ctl = &g_layout[idx];
    const PartState pst = part_state(idx);
    const double xpos = pix_x(ctl->xpos), ypos = pix_y(ctl->ypos);
    const double wid = ctl->width * UNIT, hgt = ctl->height * UNIT;
    const gboolean lit = pst.held || pst.selected;
    cairo_pattern_t *grad = cairo_pattern_create_linear(xpos, ypos, xpos + wid, ypos);
    char bound[40];

    dim_begin(cctx, &pst);
    if (lit) {
        stop_rgb(grad, 0, COL_ACCENT);
        stop_rgb(grad, 1, mix(COL_ACCENT, 0x000000, 0.3));
    } else {
        stop_rgb(grad, 0, pst.hot ? mix(COL_TRIG_TOP, 0xFFFFFF, 0.2) : COL_TRIG_TOP);
        stop_rgb(grad, 1, COL_TRIG_BOT);
    }
    /* the lever: curved at the front, the hook of a pistol trigger */
    cairo_move_to(cctx, xpos, ypos + 2);
    cairo_curve_to(cctx, xpos + wid * 0.4, ypos - 2, xpos + wid * 1.05, ypos + hgt * 0.1,
                   xpos + wid, ypos + hgt * 0.55);
    cairo_line_to(cctx, xpos + wid * 0.78, ypos + hgt);
    cairo_line_to(cctx, xpos + 2, ypos + hgt - 3);
    cairo_close_path(cctx);
    cairo_set_source(cctx, grad);
    cairo_fill_preserve(cctx);
    cairo_pattern_destroy(grad);
    rgb(cctx, lit ? COL_HELD_INK : 0x0C0B0B);
    cairo_set_line_width(cctx, lit ? 2 : 1.5);
    cairo_stroke(cctx);
    /* the pad's grooves */
    cairo_move_to(cctx, xpos + wid * 0.25, ypos + hgt * 0.3);
    cairo_line_to(cctx, xpos + wid * 0.62, ypos + hgt * 0.78);
    cairo_move_to(cctx, xpos + wid * 0.4, ypos + hgt * 0.22);
    cairo_line_to(cctx, xpos + wid * 0.8, ypos + hgt * 0.6);
    rgb(cctx, mix(lit ? COL_ACCENT : COL_TRIG_BOT, 0x000000, 0.4));
    cairo_set_line_width(cctx, 1);
    cairo_stroke(cctx);

    bound_text(ctl->target, bound, sizeof bound);
    if (bound[0])
        legend(cctx, widget, bound, 8, TRUE, lit ? COL_HELD_INK : 0x1A1A1A, xpos, ypos + 2, wid,
               hgt - 4);
    dim_end(cctx, &pst);
}

/* The knurled panel down the front of a grip, with the Bally badge on it. */
static void draw_knurl(cairo_t *cctx, double xpos, double ypos, double wid, double hgt)
{
    double step;
    rrect(cctx, xpos, ypos, wid, hgt, 4);
    rgb(cctx, COL_KNURL_BASE);
    cairo_fill_preserve(cctx);
    cairo_save(cctx);
    cairo_clip(cctx);
    for (step = -hgt; step < wid + hgt; step += 5) {
        cairo_move_to(cctx, xpos + step, ypos);
        cairo_line_to(cctx, xpos + step + hgt, ypos + hgt);
        cairo_move_to(cctx, xpos + step + hgt, ypos);
        cairo_line_to(cctx, xpos + step, ypos + hgt);
    }
    rgb(cctx, COL_KNURL);
    cairo_set_line_width(cctx, 0.8);
    cairo_stroke(cctx);
    cairo_restore(cctx);
}

/* One of a grip's two knob-key buttons: turns the knob while held. */
static void draw_kb(cairo_t *cctx, int idx)
{
    double xpos, ypos, wid, hgt;
    const gboolean lit = g_kb_lit[idx] || g_pressed_kb == idx;
    const int target = kb_target(idx);
    const gboolean sel = g_map_state >= 0 && g_map_state == target;
    const gboolean dim = g_map_state != -2 && !sel;
    const gboolean right = idx % KB_PER_PLAYER;
    guint32 face = lit || sel ? COL_ACCENT : g_hover_kb == idx ? 0x4A4444 : 0x353030;
    char bound[40];

    kb_rect(idx, &xpos, &ypos, &wid, &hgt);
    if (dim) cairo_push_group(cctx);
    rrect(cctx, xpos, ypos + 2, wid, hgt, 5);
    rgb(cctx, lit || sel ? mix(COL_ACCENT, 0x000000, 0.45) : 0x0A0808);
    cairo_fill(cctx);
    rrect(cctx, xpos, ypos, wid, hgt - 1, 5);
    rgb(cctx, face);
    cairo_fill_preserve(cctx);
    rgb(cctx, lit || sel ? COL_HELD_INK : 0x0A0808);
    cairo_set_line_width(cctx, lit || sel ? 2 : 1);
    cairo_stroke(cctx);
    bound_text(target, bound, sizeof bound);
    if (bound[0]) {
        /* the bound key, over the arrow */
        legend(cctx, g_console, bound, 8, TRUE, lit || sel ? COL_HELD_INK : 0xC8BEBE, xpos - 2,
               ypos, wid + 4, hgt - 1);
    } else {
        const double cenx = xpos + wid / 2, ceny = ypos + (hgt - 1) / 2;
        const double dirx = right ? 1 : -1;
        cairo_move_to(cctx, cenx + dirx * 5, ceny);
        cairo_line_to(cctx, cenx - dirx * 3, ceny - 5);
        cairo_line_to(cctx, cenx - dirx * 3, ceny + 5);
        cairo_close_path(cctx);
        rgb(cctx, lit || sel ? COL_HELD_INK : COL_WEDGE_INK);
        cairo_fill(cctx);
    }
    if (dim) {
        cairo_pop_group_to_source(cctx);
        cairo_paint_with_alpha(cctx, 0.4);
    }
}

/* One hand controller's bay and body, from its caption down: the recess in the
 * console top, the grip's black shell with its chrome edge, the knurled front
 * with the Bally badge and the knob-key buttons. The ring, knob and trigger
 * are drawn over it by their own parts. */
static void draw_card(cairo_t *cctx, GtkWidget *widget, int idx)
{
    const astro_control *cap = &g_layout[idx];
    const double ox = pix_x(cap->xpos), oy = pix_y(cap->ypos);
    cairo_pattern_t *grad = cairo_pattern_create_linear(0, oy, 0, oy + CARD_H * UNIT);
    const gboolean armed = g_map_state != -2;
    int kb;

    /* the bay */
    stop_rgb(grad, 0, COL_BAY_TOP);
    stop_rgb(grad, 1, COL_BAY_BOT);
    rrect(cctx, ox - 1, oy - 1, cap->width * UNIT + 2, CARD_H * UNIT + 2, 12);
    cairo_set_source(cctx, grad);
    cairo_fill_preserve(cctx);
    cairo_pattern_destroy(grad);
    rgb(cctx, COL_BAY_EDGE);
    cairo_set_line_width(cctx, 1);
    cairo_stroke(cctx);
    legend(cctx, widget, cap->label, 11, TRUE, COL_BAY_INK, ox, oy + 2, cap->width * UNIT,
           CARD_H * 0.1 * UNIT);

    if (armed) cairo_push_group(cctx);
    /* the grip's shell: wide at the head, narrower down the hand */
    grad = cairo_pattern_create_linear(ox + 0.5 * UNIT, 0, ox + 3.1 * UNIT, 0);
    stop_rgb(grad, 0, COL_SHELL_TOP);
    stop_rgb(grad, 0.5, COL_SHELL_BOT);
    stop_rgb(grad, 1, COL_SHELL_TOP);
    cairo_move_to(cctx, ox + 0.9 * UNIT, oy + 3.0 * UNIT);
    cairo_curve_to(cctx, ox + 0.6 * UNIT, oy + 4.0 * UNIT, ox + 0.45 * UNIT, oy + 4.8 * UNIT,
                   ox + 0.55 * UNIT, oy + 5.05 * UNIT);
    cairo_curve_to(cctx, ox + 0.6 * UNIT, oy + 5.25 * UNIT, ox + 1.3 * UNIT, oy + 5.2 * UNIT,
                   ox + 1.8 * UNIT, oy + 5.2 * UNIT);
    cairo_curve_to(cctx, ox + 2.3 * UNIT, oy + 5.2 * UNIT, ox + 2.95 * UNIT, oy + 5.25 * UNIT,
                   ox + 3.05 * UNIT, oy + 5.0 * UNIT);
    cairo_curve_to(cctx, ox + 3.1 * UNIT, oy + 4.2 * UNIT, ox + 3.0 * UNIT, oy + 3.5 * UNIT,
                   ox + 2.7 * UNIT, oy + 3.0 * UNIT);
    cairo_close_path(cctx);
    cairo_set_source(cctx, grad);
    cairo_fill_preserve(cctx);
    cairo_pattern_destroy(grad);
    rgb(cctx, COL_SHELL_HI);
    cairo_set_line_width(cctx, 1);
    cairo_stroke(cctx);
    /* the gloss line down the left edge */
    cairo_move_to(cctx, ox + 0.85 * UNIT, oy + 3.3 * UNIT);
    cairo_curve_to(cctx, ox + 0.68 * UNIT, oy + 4.0 * UNIT, ox + 0.62 * UNIT, oy + 4.6 * UNIT,
                   ox + 0.66 * UNIT, oy + 4.9 * UNIT);
    cairo_set_source_rgba(cctx, 1, 1, 1, 0.22);
    cairo_set_line_width(cctx, 1.5);
    cairo_stroke(cctx);

    draw_knurl(cctx, ox + 0.72 * UNIT, oy + 3.62 * UNIT, 1.68 * UNIT, 1.36 * UNIT);
    /* the gold Bally badge on the knurl, between the knob buttons' ends */
    {
        const double bx = ox + 1.56 * UNIT, by = oy + 4.12 * UNIT, br = 0.27 * UNIT;
        cairo_arc(cctx, bx, by, br, 0, 2 * G_PI);
        rgb(cctx, 0x0E0D0D);
        cairo_fill_preserve(cctx);
        rgb(cctx, COL_BADGE);
        cairo_set_line_width(cctx, 1.6);
        cairo_stroke(cctx);
        legend(cctx, widget, "Bally", 7, TRUE, COL_BADGE, bx - 2 * br, by - br, 4 * br, 2 * br);
    }
    if (armed) {
        cairo_pop_group_to_source(cctx);
        cairo_paint_with_alpha(cctx, 0.4);
    }

    for (kb = 0; kb < KB_PER_PLAYER; kb++)
        draw_kb(cctx, cap->player * KB_PER_PLAYER + kb);
}

/* A console button (RESET, Reset to CONFIG): black with gold lettering, like
 * the real RESET. */
static void draw_sysbutton(cairo_t *cctx, GtkWidget *widget, int idx)
{
    const astro_control *ctl = &g_layout[idx];
    const PartState pst = part_state(idx);
    const double xpos = pix_x(ctl->xpos), ypos = pix_y(ctl->ypos);
    const double wid = ctl->width * UNIT, hgt = ctl->height * UNIT - 3;
    const gboolean lit = pst.held || pst.selected;
    guint32 face = lit ? COL_ACCENT : pst.hot ? COL_SYS_HOT : COL_SYS_FACE;
    char bound[40], text[120];

    dim_begin(cctx, &pst);
    rrect(cctx, xpos, ypos + 3, wid, hgt, 7);
    rgb(cctx, lit ? mix(COL_ACCENT, 0x000000, 0.45) : COL_SYS_SIDE);
    cairo_fill(cctx);
    rrect(cctx, xpos, ypos, wid, hgt, 7);
    rgb(cctx, face);
    cairo_fill_preserve(cctx);
    rgb(cctx, lit ? COL_HELD_INK : 0x5A4A1E);
    cairo_set_line_width(cctx, 1);
    cairo_stroke(cctx);
    bound_text(ctl->target, bound, sizeof bound);
    if (bound[0] && strcmp(bound, "\xe2\x80\x94") != 0)   /* nothing bound: no suffix */
        g_snprintf(text, sizeof text, "%s  \xc2\xb7  %s", ctl->label, bound);
    else
        g_snprintf(text, sizeof text, "%s", ctl->label);
    legend(cctx, widget, text, 10.5, TRUE, lit ? COL_HELD_INK : COL_SYS_INK, xpos + 2, ypos,
           wid - 4, hgt);
    dim_end(cctx, &pst);
}

static void draw_console(GtkDrawingArea *area, cairo_t *cctx, int wid, int hgt, gpointer udata)
{
    GtkWidget *widget = GTK_WIDGET(area);
    cairo_pattern_t *grad = cairo_pattern_create_linear(0, 0, 0, hgt);
    int idx;
    (void)udata;

    /* the walnut frame */
    stop_rgb(grad, 0, COL_WALNUT_TOP);
    stop_rgb(grad, 1, COL_WALNUT_BOT);
    rrect(cctx, 0.5, 0.5, wid - 1, hgt - 1, 16);
    cairo_set_source(cctx, grad);
    cairo_fill_preserve(cctx);
    rgb(cctx, COL_WALNUT_EDGE);
    cairo_set_line_width(cctx, 1);
    cairo_stroke(cctx);
    cairo_pattern_destroy(grad);
    cairo_move_to(cctx, 18, 2.5);
    cairo_line_to(cctx, wid - 18, 2.5);
    rgb(cctx, COL_WALNUT_LIGHT);
    cairo_set_line_width(cctx, 1.5);
    cairo_stroke(cctx);

    /* the black top, with its gold pinstripe */
    rrect(cctx, FRAME, FRAME, wid - 2 * FRAME, hgt - 2 * FRAME, 10);
    rgb(cctx, COL_TOP);
    cairo_fill_preserve(cctx);
    rgb(cctx, COL_TOP_EDGE);
    cairo_set_line_width(cctx, 1);
    cairo_stroke(cctx);
    rrect(cctx, FRAME + 4.5, FRAME + 4.5, wid - 2 * FRAME - 9, hgt - 2 * FRAME - 9, 7);
    rgb(cctx, COL_PINSTRIPE);
    cairo_set_line_width(cctx, 1);
    cairo_stroke(cctx);

    /* the name, in gold, with the accent under it */
    legend(cctx, widget, "ASTROCADE", 12, TRUE, COL_BRAND_INK, wid / 2.0 - 80,
           FRAME + TOP_PAD, 160, BRAND_H - 2);
    cairo_rectangle(cctx, wid / 2.0 - 60, FRAME + TOP_PAD + BRAND_H - 1, 120, 2);
    rgb(cctx, COL_ACCENT);
    cairo_fill(cctx);

    if (!g_layout)
        return;

    /* the parts in the layout's paint order (plate, keys, then each card: its
     * body, ring, wedges, knob, trigger; the console's buttons) */
    for (idx = 0; idx < g_part_count; idx++) {
        const astro_control *ctl = &g_layout[idx];
        double cenx, ceny, ringr, dialr;
        switch (ctl->kind) {
        case ASTRO_CTL_OVERLAY:
            draw_plate(cctx, ctl);
            break;
        case ASTRO_CTL_KEY:
            draw_key(cctx, widget, idx);
            break;
        case ASTRO_CTL_CAPTION:
            draw_card(cctx, widget, idx);
            break;
        case ASTRO_CTL_STICK:
            if (disc_of(ASTRO_CTL_STICK, ctl->player, &cenx, &ceny, &ringr)) {
                const gboolean armed = g_map_state != -2;
                if (armed) cairo_push_group(cctx);
                draw_ring(cctx, cenx, ceny, ringr);
                if (armed) {
                    cairo_pop_group_to_source(cctx);
                    cairo_paint_with_alpha(cctx, 0.4);
                }
            }
            break;
        case ASTRO_CTL_DIRECTION:
            if (disc_of(ASTRO_CTL_STICK, ctl->player, &cenx, &ceny, &ringr) &&
                disc_of(ASTRO_CTL_KNOB, ctl->player, &cenx, &ceny, &dialr))
                draw_wedge(cctx, widget, idx, cenx, ceny, ringr, dialr);
            break;
        case ASTRO_CTL_KNOB:
            if (disc_of(ASTRO_CTL_KNOB, ctl->player, &cenx, &ceny, &dialr))
                draw_dial(cctx, widget, idx, cenx, ceny, dialr);
            break;
        case ASTRO_CTL_TRIGGER:
            draw_trigger(cctx, widget, idx);
            break;
        case ASTRO_CTL_SYSBUTTON:
            draw_sysbutton(cctx, widget, idx);
            break;
        default:
            break;
        }
    }
}

/* ---- pointer ---------------------------------------------------------------------- */

static int console_hit(double xpos, double ypos)
{
    return astrosession_controls_hit((float)((xpos - ORG_X) / UNIT),
                                     (float)((ypos - ORG_Y) / UNIT));
}

/* Lets go of everything the mouse holds on the controls. */
static void mouse_release(void)
{
    gboolean any = g_drag.kind || g_pressed >= 0 || g_pressed_kb >= 0 || g_pressed_sys >= 0;
    astrosession_pointer_release(g_session);
    g_drag.kind = 0;
    g_pressed = g_pressed_kb = g_pressed_sys = -1;
    if (any && g_console)
        gtk_widget_queue_draw(g_console);
}

static void stop_capture(void)
{
    if (g_capture_timer) {
        g_source_remove(g_capture_timer);
        g_capture_timer = 0;
    }
    astrosession_gamepad_capture_cancel(g_session);
}

static void say_bound(const char *what, int stolen)
{
    char msg[220];
    if (stolen >= 0)
        g_snprintf(msg, sizeof msg, "Bound %s (was %s)", what, bindings_target_label(stolen));
    else
        g_snprintf(msg, sizeof msg, "Bound %s", what);
    gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
}

static void set_map_state(int state);

static gboolean capture_tick(gpointer udata)
{
    int input;
    (void)udata;
    if (g_map_state < 0) { g_capture_timer = 0; return G_SOURCE_REMOVE; }
    if (astrosession_gamepad_capture_poll(g_session, &input)) {
        char name[48];
        int stolen = -1;
        bindings_set_pad(g_session, g_map_state, input, &stolen);
        astrosession_pad_input_name(input, name, sizeof name);
        g_capture_timer = 0;
        set_map_state(-1);     /* stay armed: remapping several in a row is normal */
        say_bound(name, stolen);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void set_map_state(int state)
{
    g_map_state = state;
    gtk_widget_set_sensitive(g_clear_button, state >= 0);
    if (state == -2) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Map");
        gtk_widget_remove_css_class(g_map_button, "astro-accent");
        gtk_label_set_text(GTK_LABEL(g_map_hint),
                           "Hold a control with the mouse, drag a knob to turn it. Map rebinds one.");
    } else if (state == -1) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Done");
        gtk_widget_add_css_class(g_map_button, "astro-accent");
        gtk_label_set_text(GTK_LABEL(g_map_hint), "Click a control to remap it");
    } else {
        char msg[200];
        mouse_release();
        if (is_knob_target(state)) {
            g_snprintf(msg, sizeof msg, "Move a gamepad stick or trigger for %s (Esc cancels)",
                       bindings_target_label(state));
            astrosession_gamepad_capture_begin_axis(g_session);
        } else {
            g_snprintf(msg, sizeof msg, "Press a key or a gamepad button for %s (Esc cancels)",
                       bindings_target_label(state));
            astrosession_gamepad_capture_begin(g_session);
        }
        gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
        if (!g_capture_timer)
            g_capture_timer = g_timeout_add(50, capture_tick, NULL);
    }
    refresh_all();
}

static void run_sysaction(int sysact)
{
    if (g_parent)
        astro_window_run_sysaction(ASTRO_WINDOW(g_parent), sysact);
    else
        astrosession_sysaction_fire(g_session, (astrosession_sysaction)sysact);
}

/* The knob under a drag: the pointer's angle from the dial's centre. */
static void knob_follow(double xpos, double ypos)
{
    const int val = astrosession_knob_from_point(xpos - g_drag.cenx, ypos - g_drag.ceny,
                                                 g_drag.rad);
    if (val >= 0)
        astrosession_knob_set(g_session, g_drag.player, (uint8_t)val);
    gtk_widget_queue_draw(g_console);
}

/* The grip under a drag: the pointer's vector from the ring's centre. */
static void stick_follow(double xpos, double ypos)
{
    const unsigned stick = astrosession_stick_from_vector((float)(xpos - g_drag.cenx),
                                                          (float)(ypos - g_drag.ceny),
                                                          (float)g_drag.rad);
    astrosession_pointer_set_player(g_session, g_drag.player, stick);
    gtk_widget_queue_draw(g_console);
}

static void drag_follow(double xpos, double ypos)
{
    if (g_drag.kind == 1)
        stick_follow(xpos, ypos);
    else if (g_drag.kind == 2)
        knob_follow(xpos, ypos);
}

static void on_console_begin(GtkGestureDrag *gest, double xpos, double ypos, gpointer udata)
{
    const guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gest));
    const int kbtn = kb_hit(xpos, ypos);
    const int idx = kbtn >= 0 ? -1 : console_hit(xpos, ypos);
    const astro_control *ctl;
    (void)udata;

    if (button != GDK_BUTTON_PRIMARY || (kbtn < 0 && idx < 0))
        return;
    if (kbtn >= 0) {
        if (g_map_state != -2) {
            set_map_state(kb_target(kbtn));
            return;
        }
        mouse_release();
        g_pressed_kb = kbtn;
        astrosession_press(g_session, kb_target(kbtn), 1);
        gtk_widget_queue_draw(g_console);
        return;
    }
    ctl = &g_layout[idx];
    if (g_map_state != -2) {
        /* armed: picks (or changes) the target; the ring's disc and the
         * diagonal wedges are no single control */
        const int target = part_target(ctl);
        if (target >= 0)
            set_map_state(target);
        return;
    }
    mouse_release();
    switch (ctl->kind) {
    case ASTRO_CTL_STICK:
    case ASTRO_CTL_DIRECTION:
        if (!disc_of(ASTRO_CTL_STICK, ctl->player, &g_drag.cenx, &g_drag.ceny, &g_drag.rad))
            return;
        g_drag.kind = 1;
        g_drag.player = ctl->player;
        drag_follow(xpos, ypos);
        break;
    case ASTRO_CTL_KNOB:
        if (!disc_of(ASTRO_CTL_KNOB, ctl->player, &g_drag.cenx, &g_drag.ceny, &g_drag.rad))
            return;
        g_drag.kind = 2;
        g_drag.player = ctl->player;
        drag_follow(xpos, ypos);
        break;
    case ASTRO_CTL_KEY:
    case ASTRO_CTL_TRIGGER:
        g_pressed = idx;
        astrosession_press(g_session, ctl->target, 1);
        break;
    case ASTRO_CTL_SYSBUTTON:
        g_pressed_sys = idx;
        break;
    default:
        return;
    }
    gtk_widget_queue_draw(g_console);
}

static void on_console_update(GtkGestureDrag *gest, double offx, double offy, gpointer udata)
{
    double startx, starty;
    (void)udata;
    if (!g_drag.kind) return;
    gtk_gesture_drag_get_start_point(gest, &startx, &starty);
    drag_follow(startx + offx, starty + offy);
}

/* A console button fires on release, like a real one: pressing and dragging
 * off must not reset the machine. */
static void on_console_end(GtkGestureDrag *gest, double offx, double offy, gpointer udata)
{
    const int sys = g_pressed_sys;
    double startx, starty;
    (void)udata;
    if (sys >= 0) {
        gtk_gesture_drag_get_start_point(gest, &startx, &starty);
        g_pressed_sys = -1;
        if (console_hit(startx + offx, starty + offy) == sys && is_sysact(g_layout[sys].target))
            run_sysaction(g_layout[sys].target - ASTRO_TARGET_SYSACT);
    }
    mouse_release();
}

static void on_console_cancel(GtkGesture *gest, GdkEventSequence *seq, gpointer udata)
{
    (void)gest; (void)seq; (void)udata;
    mouse_release();
}

/* Dragging off a held key or the trigger lets it go. */
static void on_console_motion(GtkEventControllerMotion *ctrl, double xpos, double ypos,
                              gpointer udata)
{
    const int kbtn = kb_hit(xpos, ypos);
    const int idx = kbtn >= 0 ? -1 : console_hit(xpos, ypos);
    (void)ctrl; (void)udata;
    g_ptr_x = xpos;
    g_ptr_y = ypos;
    if (g_pressed >= 0 && idx != g_pressed)
        mouse_release();
    if (g_pressed_kb >= 0 && kbtn != g_pressed_kb)
        mouse_release();
    if (idx != g_hover || kbtn != g_hover_kb) {
        g_hover = idx;
        g_hover_kb = kbtn;
        gtk_widget_queue_draw(g_console);
    }
}

static void on_console_leave(GtkEventControllerMotion *ctrl, gpointer udata)
{
    (void)ctrl; (void)udata;
    if (g_pressed >= 0 || g_pressed_kb >= 0)
        mouse_release();
    if (g_hover >= 0 || g_hover_kb >= 0) {
        g_hover = g_hover_kb = -1;
        gtk_widget_queue_draw(g_console);
    }
}

/* The wheel over a grip nudges its knob. */
static gboolean on_console_scroll(GtkEventControllerScroll *ctrl, double delx, double dely,
                                  gpointer udata)
{
    int idx, player = -1, val;
    (void)ctrl; (void)delx; (void)udata;
    if (g_map_state != -2 || g_drag.kind)
        return FALSE;
    idx = console_hit(g_ptr_x, g_ptr_y);
    if (idx >= 0)
        player = g_layout[idx].player;
    else if (kb_hit(g_ptr_x, g_ptr_y) >= 0)
        player = kb_hit(g_ptr_x, g_ptr_y) / KB_PER_PLAYER;
    if (player < 0)
        return FALSE;
    val = astrosession_knob_get(g_session, player) + (dely < 0 ? KNOB_NUDGE : -KNOB_NUDGE);
    astrosession_knob_set(g_session, player, (uint8_t)CLAMP(val, 0, 255));
    gtk_widget_queue_draw(g_console);
    return TRUE;
}

static void binding_tip(int target, char *text, int textsz)
{
    char key[32], pad[48];
    if (is_knob_target(target)) {
        const int input = bindings_target_pad(target);
        if (input >= 0) astrosession_pad_input_name(input, pad, sizeof pad);
        else g_snprintf(pad, sizeof pad, "none");
        g_snprintf(text, textsz, "%s\nGamepad axis: %s", bindings_target_label(target), pad);
        return;
    }
    astrosession_keysym_name(bindings_target_keysym(target), key, sizeof key);
    if (bindings_target_pad(target) >= 0)
        astrosession_pad_input_name(bindings_target_pad(target), pad, sizeof pad);
    else
        g_snprintf(pad, sizeof pad, "none");
    g_snprintf(text, textsz, "%s\nKey: %s    Gamepad: %s", bindings_target_label(target),
               key[0] ? key : "none", pad);
}

static gboolean on_console_tooltip(GtkWidget *widget, int xpos, int ypos, gboolean kbmode,
                                   GtkTooltip *tip, gpointer udata)
{
    const int kbtn = kbmode ? -1 : kb_hit(xpos, ypos);
    const int idx = kbmode || kbtn >= 0 ? -1 : console_hit(xpos, ypos);
    char text[300];
    (void)widget; (void)udata;
    if (kbtn >= 0) {
        char tip2[300];
        binding_tip(kb_target(kbtn), tip2, sizeof tip2);
        g_snprintf(text, sizeof text, "Turns the knob while held\n%s", tip2);
    } else if (idx >= 0) {
        const astro_control *ctl = &g_layout[idx];
        const int target = part_target(ctl);
        if (target >= 0 && ctl->kind == ASTRO_CTL_KNOB)
            g_snprintf(text, sizeof text,
                       "Drag to turn the knob, scroll to nudge it (Map: its gamepad axis)");
        else if (target >= 0)
            binding_tip(target, text, sizeof text);
        else if (ctl->kind == ASTRO_CTL_STICK)
            g_snprintf(text, sizeof text, "Press and drag to tilt the grip");
        else if (ctl->kind == ASTRO_CTL_DIRECTION)
            g_snprintf(text, sizeof text, "%s (two directions): press and hold", ctl->label);
        else
            return FALSE;
    } else {
        return FALSE;
    }
    gtk_tooltip_set_text(tip, text);
    return TRUE;
}

/* ---- refresh ---------------------------------------------------------------------- */

static void refresh_all(void)
{
    if (g_console) gtk_widget_queue_draw(g_console);
}

/* Every frame while the window is on screen: what the machine sees held,
 * painted in the accent, the knobs' positions, and the overlay. Redraws only
 * when something moved. */
static gboolean frame_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer udata)
{
    unsigned dirs[ASTROSESSION_PLAYERS];
    gboolean changed = FALSE;
    int idx, player, overlay;
    (void)clock; (void)udata;

    for (player = 0; player < ASTROSESSION_PLAYERS; player++) {
        unsigned mask = 0;
        int act;
        for (act = ASTRO_ACT_UP; act <= ASTRO_ACT_RIGHT; act++)
            if (astrosession_target_held(g_session, ASTRO_TARGET_PLAYER(player, act)))
                mask |= 1u << act;
        dirs[player] = mask;
        {
            const int val = astrosession_knob_get(g_session, player);
            if (val != g_knob_val[player]) {
                g_knob_val[player] = val;
                changed = TRUE;
            }
        }
    }
    for (idx = 0; idx < g_part_count && idx < MAX_PARTS; idx++) {
        const astro_control *ctl = &g_layout[idx];
        gboolean lit = FALSE;
        if (ctl->kind == ASTRO_CTL_DIRECTION)
            lit = ctl->act_mask && dirs[ctl->player] == ctl->act_mask;
        else if ((ctl->kind == ASTRO_CTL_KEY || ctl->kind == ASTRO_CTL_TRIGGER) && ctl->target >= 0)
            lit = astrosession_target_held(g_session, ctl->target) != 0;
        if (lit != g_lit[idx]) {
            g_lit[idx] = lit;
            changed = TRUE;
        }
    }
    for (idx = 0; idx < KB_COUNT; idx++) {
        const gboolean lit = astrosession_target_held(g_session, kb_target(idx)) != 0;
        if (lit != g_kb_lit[idx]) {
            g_kb_lit[idx] = lit;
            changed = TRUE;
        }
    }
    overlay = astrosession_overlay_get(g_session) != 0;
    if (overlay != g_overlay_on) {
        g_overlay_on = overlay;
        changed = TRUE;
    }
    if (changed)
        gtk_widget_queue_draw(widget);
    return G_SOURCE_CONTINUE;
}

/* ---- gamepads ------------------------------------------------------------- */

static void refresh_pad_ports(void)
{
    int idx;
    for (idx = 0; idx < g_pad_row_count; idx++) {
        const int eff = astrosession_gamepad_effective_port(g_session, idx);
        char text[32];
        if (eff >= 0) g_snprintf(text, sizeof text, "plays player %d", eff + 1);
        else g_snprintf(text, sizeof text, "plays nobody");
        if (strcmp(gtk_label_get_text(GTK_LABEL(g_pad_eff[idx])), text) != 0)
            gtk_label_set_text(GTK_LABEL(g_pad_eff[idx]), text);
    }
}

static void on_assign_changed(GObject *obj, GParamSpec *pspec, gpointer udata)
{
    const int idx = GPOINTER_TO_INT(udata);
    const guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
    (void)pspec;
    /* 0 = automatic (connection order), 1-4 = that player */
    astrosession_gamepad_assign(g_session, idx, sel == 0 ? -1 : (int)sel - 1);
    refresh_pad_ports();
}

/* Rebuilt whenever a pad comes or goes (the session's generation counter):
 * one row per pad, its name, the player it plays and the one it is set to. The
 * area is PAD_ROWS rows tall whatever is plugged in (more pads scroll), so the
 * window never changes size. */
static void refresh_pads(void)
{
    static const char *const choices[] = { "Automatic", "Player 1", "Player 2", "Player 3",
                                           "Player 4", NULL };
    GtkWidget *child;
    int count, idx;

    while ((child = gtk_widget_get_first_child(g_pad_box)) != NULL)
        gtk_box_remove(GTK_BOX(g_pad_box), child);
    g_pad_row_count = 0;

    count = astrosession_gamepad_count(g_session);
    if (count == 0) {
        GtkWidget *none = gtk_label_new("No gamepads connected - plug one in");
        gtk_widget_add_css_class(none, "dim-label");
        gtk_label_set_xalign(GTK_LABEL(none), 0);
        gtk_widget_set_size_request(none, -1, PAD_ROW_H);
        gtk_widget_set_tooltip_text(none, "A gamepad is picked up as it appears");
        gtk_box_append(GTK_BOX(g_pad_box), none);
        return;
    }
    for (idx = 0; idx < count && idx < MAX_PADS; idx++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *name, *drop, *eff;
        char text[128];
        const int assign = astrosession_gamepad_assignment(g_session, idx);

        astrosession_gamepad_name(g_session, idx, text, sizeof text);
        gtk_widget_set_size_request(row, -1, PAD_ROW_H);
        name = gtk_label_new(text);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(name), 1);
        gtk_widget_set_tooltip_text(name, text);
        eff = gtk_label_new("");
        gtk_widget_add_css_class(eff, "dim-label");
        drop = gtk_drop_down_new_from_strings(choices);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(drop),
                                   assign < 0 || assign >= ASTROSESSION_PLAYERS ? 0
                                                                                : (guint)assign + 1);
        gtk_widget_set_focus_on_click(drop, FALSE);
        gtk_widget_set_valign(drop, GTK_ALIGN_CENTER);
        g_signal_connect(drop, "notify::selected", G_CALLBACK(on_assign_changed),
                         GINT_TO_POINTER(idx));
        gtk_box_append(GTK_BOX(row), name);
        gtk_box_append(GTK_BOX(row), eff);
        gtk_box_append(GTK_BOX(row), drop);
        gtk_box_append(GTK_BOX(g_pad_box), row);
        g_pad_eff[g_pad_row_count++] = eff;
    }
    refresh_pad_ports();
}

/* The pad rows (hot plug, automatic assignment changes), and the overlay check
 * when a Bally BASIC cartridge turned the overlay on by itself. */
static gboolean info_tick(gpointer udata)
{
    unsigned gen;
    (void)udata;
    if (!g_window || !gtk_widget_get_visible(g_window)) return G_SOURCE_CONTINUE;

    gen = astrosession_gamepad_generation(g_session);
    if (gen != g_pad_generation) {
        g_pad_generation = gen;
        refresh_pads();
    } else {
        refresh_pad_ports();
    }
    if ((astrosession_overlay_get(g_session) != 0) !=
        (gtk_check_button_get_active(GTK_CHECK_BUTTON(g_overlay_check)) != 0))
        gtk_check_button_set_active(GTK_CHECK_BUTTON(g_overlay_check),
                                    astrosession_overlay_get(g_session) != 0);
    return G_SOURCE_CONTINUE;
}

/* ---- keyboard ------------------------------------------------------------- */

/* Map mode's share of a key, for every window's key path (the main window
 * asks too: Map can be armed with either window focused). While Map is armed
 * a press is the key to bind (once a target is picked) or is swallowed; Escape
 * steps back. The window's own keys (F9, F11, F12) are never Map's, and neither
 * is a Ctrl chord. Returns TRUE when Map took the key. A release of a key Map
 * took reaches the session as a release of nothing, which is harmless. */
gboolean astro_controllers_map_key(guint keyval, guint code, GdkModifierType mods, gboolean down)
{
    guint32 keysym;
    char name[32];
    int stolen = -1;

    if (!g_window || g_map_state == -2)
        return FALSE;
    if (keyval == GDK_KEY_F9 || keyval == GDK_KEY_F11 || keyval == GDK_KEY_F12 ||
        (mods & GDK_CONTROL_MASK))
        return FALSE;
    if (g_swallow_code && code == g_swallow_code) {
        if (!down) g_swallow_code = 0;
        return TRUE;
    }
    if (!down)
        return TRUE;
    if (keyval == GDK_KEY_Escape) {
        g_swallow_code = code;
        set_map_state(g_map_state >= 0 ? -1 : -2);
        return TRUE;
    }
    if (g_map_state < 0)
        return TRUE;              /* armed, no target yet: typing is not typed */
    if (is_knob_target(g_map_state)) {
        /* the knob follows a pad axis: a key cannot turn it */
        gtk_label_set_text(GTK_LABEL(g_map_hint),
                           "The knob follows an axis: move a gamepad stick or trigger");
        return TRUE;
    }
    keysym = code >= 8 ? astrosession_keysym_from_evdev(code - 8) : 0;
    if (!keysym) keysym = keyval;
    bindings_set_keysym(g_session, g_map_state, keysym, &stolen);
    astrosession_keysym_name(keysym, name, sizeof name);
    g_swallow_code = code;
    set_map_state(-1);            /* stay armed: remapping several in a row is normal */
    say_bound(name, stolen);
    return TRUE;
}

static gboolean on_key(guint keyval, guint code, GdkModifierType mods, gboolean down)
{
    /* an open popover has the arrows and Return */
    GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(g_window));
    if (focus && gtk_widget_get_ancestor(focus, GTK_TYPE_POPOVER))
        return FALSE;

    if (astro_controllers_map_key(keyval, code, mods, down))
        return TRUE;
    /* Otherwise exactly what the main window does with it. */
    if (!g_parent) return FALSE;
    return astro_window_key(ASTRO_WINDOW(g_parent), keyval, code, mods, down);
}

static gboolean on_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                               guint code, GdkModifierType mods, gpointer udata)
{
    (void)ctrl; (void)udata;
    return on_key(keyval, code, mods, TRUE);
}

static void on_key_released(GtkEventControllerKey *ctrl, guint keyval,
                            guint code, GdkModifierType mods, gpointer udata)
{
    (void)ctrl; (void)udata;
    on_key(keyval, code, mods, FALSE);
}

/* Keys typed here went through the main window, so a key held into a focus
 * change must be let go there too (and the mouse's, here). */
static void on_focus_leave(GtkEventControllerFocus *ctrl, gpointer udata)
{
    (void)ctrl; (void)udata;
    mouse_release();
    if (g_parent)
        astro_window_release_keys(ASTRO_WINDOW(g_parent));
    g_swallow_code = 0;     /* its release went elsewhere */
}

static void on_active_changed(GObject *obj, GParamSpec *pspec, gpointer udata)
{
    (void)pspec; (void)udata;
    if (!gtk_window_is_active(GTK_WINDOW(obj)))
        mouse_release();
}

/* ---- the window ----------------------------------------------------------- */

static void on_map_clicked(GtkButton *button, gpointer udata)
{
    (void)button; (void)udata;
    set_map_state(g_map_state == -2 ? -1 : -2);
}

static void on_defaults_clicked(GtkButton *button, gpointer udata)
{
    (void)button; (void)udata;
    bindings_reset_defaults(g_session);
    gtk_label_set_text(GTK_LABEL(g_map_hint), "Bindings back to their defaults");
    refresh_all();
}

/* Takes the picked control's key and gamepad input away. */
static void on_clear_clicked(GtkButton *button, gpointer udata)
{
    (void)button; (void)udata;
    if (g_map_state < 0) return;
    bindings_clear(g_session, g_map_state);
    gtk_label_set_text(GTK_LABEL(g_map_hint),
                       "Cleared. Press a key or a gamepad input to bind it again");
    refresh_all();
}

static void on_show_bindings(GtkCheckButton *button, gpointer udata)
{
    (void)udata;
    astrosession_set_int(g_session, "controllers_show_bindings",
                         gtk_check_button_get_active(button) ? 1 : 0);
    refresh_all();
}

static void on_overlay_toggled(GtkCheckButton *button, gpointer udata)
{
    (void)udata;
    astrosession_overlay_set(g_session, gtk_check_button_get_active(button) ? 1 : 0);
    g_overlay_on = astrosession_overlay_get(g_session) != 0;
    refresh_all();
}

static void hide_window(void)
{
    set_map_state(-2);
    mouse_release();
    g_swallow_code = 0;
    gtk_widget_set_visible(g_window, FALSE);
}

static gboolean on_close(GtkWindow *window, gpointer udata)
{
    (void)window; (void)udata;
    hide_window();
    return TRUE;
}

/* The pad line's timer runs only while the window is on screen: closing only
 * hides it, and a hidden window has nobody to update. (The per-frame tick
 * stops by itself while the console is unmapped.) */
static void on_window_map(GtkWidget *widget, gpointer udata)
{
    (void)widget; (void)udata;
    if (!g_info_timer) g_info_timer = g_timeout_add(250, info_tick, NULL);
    info_tick(NULL);
}

static void on_window_unmap(GtkWidget *widget, gpointer udata)
{
    (void)widget; (void)udata;
    if (g_info_timer) g_source_remove(g_info_timer);
    g_info_timer = 0;
}

static void on_destroy(GtkWidget *widget, gpointer udata)
{
    (void)widget; (void)udata;
    if (g_capture_timer) g_source_remove(g_capture_timer);
    if (g_info_timer) g_source_remove(g_info_timer);
    g_capture_timer = g_info_timer = 0;
    g_console = NULL;
    g_overlay_check = NULL;
    g_window = NULL;
    g_pad_box = NULL;
    g_pad_row_count = 0;
    g_map_state = -2;
    g_hover = g_hover_kb = g_pressed = g_pressed_kb = g_pressed_sys = -1;
    g_drag.kind = 0;
}

/* The Map "Done" button in the accent. */
static void install_css(void)
{
    static gboolean done;
    GtkCssProvider *css;
    if (done) return;
    done = TRUE;
    css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        "button.astro-accent { background-image: none; background-color: #e8c14a;"
        " color: #2a1f00; font-weight: bold; }");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

static void find_parts(void)
{
    int idx;
    for (idx = 0; idx < ASTROSESSION_KEY_COUNT; idx++)
        g_key_part[idx] = 0;
    for (idx = 0; idx < ASTROSESSION_PLAYERS; idx++)
        g_card_part[idx] = 0;
    for (idx = 0; idx < g_part_count; idx++) {
        const astro_control *ctl = &g_layout[idx];
        if (ctl->kind == ASTRO_CTL_KEY) {
            const int key = ctl->target - ASTRO_TARGET_KEYPAD;
            if (key >= 0 && key < ASTROSESSION_KEY_COUNT)
                g_key_part[key] = idx;
        } else if (ctl->kind == ASTRO_CTL_CAPTION && ctl->player >= 0 &&
                   ctl->player < ASTROSESSION_PLAYERS) {
            g_card_part[ctl->player] = idx;
        }
    }
}

static void build_window(GtkWindow *parent)
{
    GtkWidget *root, *maprow, *toolbar, *header, *defaults, *padscroll;
    GtkEventController *keys, *focus, *motion, *scroll;
    GtkGesture *drag;

    install_css();
    g_layout = astrosession_controls_layout(&g_part_count, &g_layout_wid, &g_layout_hgt);
    if (g_part_count > MAX_PARTS) g_part_count = MAX_PARTS;
    find_parts();
    memset(g_lit, 0, sizeof g_lit);
    memset(g_kb_lit, 0, sizeof g_kb_lit);
    {
        int player;
        for (player = 0; player < ASTROSESSION_PLAYERS; player++)
            g_knob_val[player] = astrosession_knob_get(g_session, player);
    }
    g_overlay_on = astrosession_overlay_get(g_session) != 0;
    g_hover = g_hover_kb = g_pressed = g_pressed_kb = g_pressed_sys = -1;
    g_drag.kind = 0;

    g_window = adw_window_new();
    gtk_window_set_title(GTK_WINDOW(g_window), "Controllers");
    if (parent) {
        gtk_window_set_transient_for(GTK_WINDOW(g_window), parent);
        /* Same app-id as the main window, so the shell groups and icons it. */
        gtk_window_set_application(GTK_WINDOW(g_window), gtk_window_get_application(parent));
    }
    gtk_window_set_destroy_with_parent(GTK_WINDOW(g_window), TRUE);
    /* min == max: Wayland compositors float it */
    gtk_window_set_resizable(GTK_WINDOW(g_window), FALSE);
    g_signal_connect(g_window, "close-request", G_CALLBACK(on_close), NULL);
    g_signal_connect(g_window, "destroy", G_CALLBACK(on_destroy), NULL);
    g_signal_connect(g_window, "notify::is-active", G_CALLBACK(on_active_changed), NULL);

    /* the controls */
    g_console = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(g_console), console_width());
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(g_console), console_height());
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_console), draw_console, NULL, NULL);
    gtk_widget_set_halign(g_console, GTK_ALIGN_CENTER);
    /* Not focusable: clicking a control must not steal focus. */
    gtk_widget_set_focusable(g_console, FALSE);
    gtk_widget_set_has_tooltip(g_console, TRUE);
    g_signal_connect(g_console, "query-tooltip", G_CALLBACK(on_console_tooltip), NULL);
    drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), 0);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_console_begin), NULL);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_console_update), NULL);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_console_end), NULL);
    g_signal_connect(drag, "cancel", G_CALLBACK(on_console_cancel), NULL);
    gtk_widget_add_controller(g_console, GTK_EVENT_CONTROLLER(drag));
    motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_console_motion), NULL);
    g_signal_connect(motion, "leave", G_CALLBACK(on_console_leave), NULL);
    gtk_widget_add_controller(g_console, motion);
    scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll, "scroll", G_CALLBACK(on_console_scroll), NULL);
    gtk_widget_add_controller(g_console, scroll);
    gtk_widget_add_tick_callback(g_console, frame_tick, NULL, NULL);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);
    gtk_widget_set_size_request(root, console_width(), -1);
    gtk_box_append(GTK_BOX(root), g_console);

    /* the pad rows: a fixed-height area, so the window never resizes */
    g_pad_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, PAD_GAP);
    padscroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(padscroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(padscroll), FALSE);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(padscroll),
        PAD_ROWS * PAD_ROW_H + (PAD_ROWS - 1) * PAD_GAP);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(padscroll),
        PAD_ROWS * PAD_ROW_H + (PAD_ROWS - 1) * PAD_GAP);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(padscroll), g_pad_box);
    gtk_box_append(GTK_BOX(root), padscroll);
    g_pad_generation = astrosession_gamepad_generation(g_session);
    refresh_pads();

    maprow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_map_button = gtk_button_new_with_label("Map");
    gtk_widget_set_size_request(g_map_button, 72, -1);
    gtk_widget_set_focus_on_click(g_map_button, FALSE);
    g_signal_connect(g_map_button, "clicked", G_CALLBACK(on_map_clicked), NULL);
    g_clear_button = gtk_button_new_with_label("Clear");
    gtk_widget_set_focus_on_click(g_clear_button, FALSE);
    gtk_widget_set_tooltip_text(g_clear_button, "Take the picked control's key and gamepad input away");
    g_signal_connect(g_clear_button, "clicked", G_CALLBACK(on_clear_clicked), NULL);
    defaults = gtk_button_new_with_label("Defaults");
    gtk_widget_set_focus_on_click(defaults, FALSE);
    g_signal_connect(defaults, "clicked", G_CALLBACK(on_defaults_clicked), NULL);
    g_show_bindings = gtk_check_button_new_with_label("Show bindings");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(g_show_bindings), show_bindings_saved());
    gtk_widget_set_focus_on_click(g_show_bindings, FALSE);
    g_signal_connect(g_show_bindings, "toggled", G_CALLBACK(on_show_bindings), NULL);
    g_overlay_check = gtk_check_button_new_with_label("Overlay: Bally BASIC");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(g_overlay_check),
                                astrosession_overlay_get(g_session) != 0);
    gtk_widget_set_focus_on_click(g_overlay_check, FALSE);
    gtk_widget_set_tooltip_text(g_overlay_check,
        "Show the Bally BASIC overlay card on the keypad, and type BASIC with the keyboard");
    g_signal_connect(g_overlay_check, "toggled", G_CALLBACK(on_overlay_toggled), NULL);
    gtk_box_append(GTK_BOX(maprow), g_map_button);
    gtk_box_append(GTK_BOX(maprow), g_clear_button);
    gtk_box_append(GTK_BOX(maprow), defaults);
    gtk_box_append(GTK_BOX(maprow), g_show_bindings);
    gtk_box_append(GTK_BOX(maprow), g_overlay_check);
    gtk_box_append(GTK_BOX(root), maprow);

    g_map_hint = gtk_label_new("");
    gtk_widget_add_css_class(g_map_hint, "dim-label");
    gtk_widget_set_hexpand(g_map_hint, TRUE);
    gtk_label_set_xalign(GTK_LABEL(g_map_hint), 0);
    /* a long hint wraps onto a second line rather than clip its "Esc cancels"
     * or widen the window; two lines are always reserved, so the window never
     * changes height either */
    gtk_label_set_wrap(GTK_LABEL(g_map_hint), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(g_map_hint), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_lines(GTK_LABEL(g_map_hint), 2);
    gtk_label_set_ellipsize(GTK_LABEL(g_map_hint), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(g_map_hint), 1);
    gtk_label_set_yalign(GTK_LABEL(g_map_hint), 0);
    {
        PangoLayout *twolines = gtk_widget_create_pango_layout(g_map_hint, "A\nA");
        int linesw, linesh;
        pango_layout_get_pixel_size(twolines, &linesw, &linesh);
        g_object_unref(twolines);
        gtk_widget_set_size_request(g_map_hint, -1, linesh);
    }
    gtk_box_append(GTK_BOX(root), g_map_hint);

    header = adw_header_bar_new();
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header),
                                    adw_window_title_new("Controllers", "Bally Astrocade"));
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), astro_help_button("controllers"));
    astro_help_set_context(GTK_WINDOW(g_window), "controllers");
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), root);
    adw_window_set_content(ADW_WINDOW(g_window), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), NULL);
    gtk_widget_add_controller(g_window, keys);

    focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), NULL);
    gtk_widget_add_controller(g_window, focus);

    set_map_state(-2);
    g_signal_connect(g_window, "map", G_CALLBACK(on_window_map), NULL);
    g_signal_connect(g_window, "unmap", G_CALLBACK(on_window_unmap), NULL);
}

static void ensure_window(GtkWindow *parent, astrosession *session)
{
    g_session = session;
    if (parent)
        g_parent = parent;
    if (!g_window)
        build_window(g_parent);
}

void astro_controllers_show(GtkWindow *parent, astrosession *session)
{
    ensure_window(parent, session);
    gtk_window_present(GTK_WINDOW(g_window));
}

void astro_controllers_toggle(GtkWindow *parent, astrosession *session)
{
    ensure_window(parent, session);
    if (gtk_widget_get_visible(g_window))
        hide_window();
    else
        gtk_window_present(GTK_WINDOW(g_window));
}

gboolean astro_controllers_visible(void)
{
    return g_window && gtk_widget_get_visible(g_window);
}
