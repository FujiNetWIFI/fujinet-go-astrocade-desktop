/*
 * The Controllers window: the console's 24-key calculator keypad, the two
 * system buttons, and all four hand controllers -- each a stick, a trigger
 * and a knob dial -- with Map mode. Laid out like the other family members'
 * controller panels.
 *
 * Every control is pressed with a raw GtkGestureClick (press AND release),
 * not GtkButton's "clicked": the emulator samples the controllers once per
 * frame, so a value present only for the instant of a click falls between
 * frames. A control on screen is HELD for as long as the mouse button is
 * down; the gesture's "cancel" is wired to the release path too, so dragging
 * off a control cannot leave the machine believing it is held forever.
 *
 * What the machine sees held -- keyboard, gamepads and this window together
 * (astrosession_target_held) -- is painted in the accent colour, polled every
 * 50 ms while the window is visible, so a keypress or a gamepad lights the
 * matching control and the window doubles as an input tester. The dials show
 * the live knob.
 *
 * FIXED SIZE and FLOATING: a non-resizable transient window, so a tiling
 * compositor floats it instead of stretching the buttons. SINGLETON, hidden
 * rather than destroyed, so it comes back where it was.
 *
 * FOCUS: GTK4 has no reliable way to refuse focus on a toplevel, so this
 * window forwards keys through the same path as the main window and typing
 * drives the machine whichever window is focused.
 *
 * MAP MODE: the Map toggle arms a two-step rebind -- click a control to pick
 * the target, then press a key OR a gamepad button / axis (a dial takes an
 * axis). While armed the gestures pick instead of injecting input and keys go
 * to Map, not the machine. Rebinding steals the input from whatever held it
 * (core/src/bindings.c). Leaving Map cancels the capture.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "keypad_window.h"

#include <math.h>
#include <string.h>

#include "../window.h"
#include "bindings.h"

#define EM_DASH "\xe2\x80\x94"

/* ---- singleton state ------------------------------------------------------ */

static GtkWidget *g_window;
static GtkWindow *g_parent;      /* the main window: resets go through it */
static astrosession *g_session;

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 waiting for a
 * key or pad input to bind to that target. */
static int g_map_state = -2;
static GtkWidget *g_map_button;
static GtkWidget *g_clear_button;
static GtkWidget *g_map_hint;
static GtkWidget *g_pads_label;
static guint g_tick_id;

typedef struct {
    GtkWidget *button;
    GtkWidget *label;
    int target;
    gboolean lit;
} control;

static control g_controls[ASTRO_TARGET_COUNT];
static int g_ncontrols;

typedef struct {
    GtkWidget *area;
    int player;
    int target;          /* ASTRO_TARGET_PLAYER(player, ASTRO_ACT_KNOB) */
    GtkWidget *caption;  /* the bound axis, under the dial */
} dial;
static dial g_dials[ASTROSESSION_PLAYERS];

static void refresh_labels(void);

static gboolean is_knob_target(int t)
{
    return t >= ASTRO_TARGET_PLAYERS &&
           (t - ASTRO_TARGET_PLAYERS) % ASTRO_ACT_PER_PLAYER == ASTRO_ACT_KNOB;
}

/* ---- pressing ------------------------------------------------------------- */

static void press_target(int target, int down)
{
    if (target >= ASTRO_TARGET_SYSACT && target < ASTRO_TARGET_PLAYERS) {
        /* System actions fire on release, like a real button: pressing and
         * dragging off must not reset the machine. */
        if (!down)
            astrosession_sysaction_fire(g_session,
                                        (astrosession_sysaction)(target - ASTRO_TARGET_SYSACT));
        return;
    }
    astrosession_press(g_session, target, down);
}

static void set_hint(const char *text)
{
    gtk_label_set_text(GTK_LABEL(g_map_hint), text);
}

static void set_lit(control *c, gboolean on)
{
    if (c->lit == on)
        return;
    c->lit = on;
    if (on) gtk_widget_add_css_class(c->button, "astro-lit");
    else gtk_widget_remove_css_class(c->button, "astro-lit");
}

static void set_map_state(int state)
{
    g_map_state = state;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_map_button), state != -2);
    gtk_widget_set_sensitive(g_clear_button, state >= 0);
    if (state >= 0) {
        char msg[160];
        g_snprintf(msg, sizeof msg,
                   is_knob_target(state) ? "Move a gamepad stick or trigger for %s"
                                         : "Press a key or gamepad button for %s",
                   bindings_target_label(state));
        set_hint(msg);
        astrosession_gamepad_capture_begin(g_session);
    } else {
        astrosession_gamepad_capture_cancel(g_session);
        set_hint(state == -1 ? "Click a control to remap" : "");
    }
    refresh_labels();
    for (int p = 0; p < ASTROSESSION_PLAYERS; p++)
        gtk_widget_queue_draw(g_dials[p].area);
}

static void on_pressed(GtkGestureClick *g, int n, double x, double y,
                       gpointer user_data)
{
    int target = GPOINTER_TO_INT(user_data);
    (void)g; (void)n; (void)x; (void)y;

    if (g_map_state == -1) {
        set_map_state(target);
        return;
    }
    if (g_map_state >= 0)
        return;   /* waiting for an input; ignore further clicks */
    press_target(target, 1);
}

static void on_released(GtkGestureClick *g, int n, double x, double y,
                        gpointer user_data)
{
    int target = GPOINTER_TO_INT(user_data);
    (void)g; (void)n; (void)x; (void)y;
    if (g_map_state != -2) return;
    press_target(target, 0);
}

static void on_cancelled(GtkGesture *g, GdkEventSequence *seq,
                         gpointer user_data)
{
    int target = GPOINTER_TO_INT(user_data);
    (void)g; (void)seq;
    if (g_map_state != -2) return;
    if (target >= ASTRO_TARGET_SYSACT && target < ASTRO_TARGET_PLAYERS)
        return;   /* dragged off: no reset */
    press_target(target, 0);
}

/* A fixed-size control whose face is `face`; in Map mode the label shows the
 * binding instead (ellipsized, so a long key name never resizes the window). */
static GtkWidget *control_button(const char *face, int target, int w, int h)
{
    GtkWidget *b = gtk_button_new();
    GtkWidget *label = gtk_label_new(face);
    GtkGesture *g = gtk_gesture_click_new();

    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
    gtk_button_set_child(GTK_BUTTON(b), label);
    if (target < ASTROSESSION_KEY_COUNT && target % 4 == 3)
        gtk_widget_add_css_class(b, "astro-gold");   /* the console's gold column */
    gtk_widget_set_size_request(b, w, h);
    g_signal_connect(g, "pressed", G_CALLBACK(on_pressed), GINT_TO_POINTER(target));
    g_signal_connect(g, "released", G_CALLBACK(on_released), GINT_TO_POINTER(target));
    g_signal_connect(g, "cancel", G_CALLBACK(on_cancelled), GINT_TO_POINTER(target));
    gtk_widget_add_controller(b, GTK_EVENT_CONTROLLER(g));
    /* Never focusable: clicking a control must not steal focus from the
     * display, and tabbing through every button is nobody's idea of input. */
    gtk_widget_set_focusable(b, FALSE);
    gtk_widget_set_focus_on_click(b, FALSE);
    g_object_set_data_full(G_OBJECT(b), "face", g_strdup(face), g_free);

    if (g_ncontrols < ASTRO_TARGET_COUNT) {
        g_controls[g_ncontrols].button = b;
        g_controls[g_ncontrols].label = label;
        g_controls[g_ncontrols].target = target;
        g_controls[g_ncontrols].lit = FALSE;
        g_ncontrols++;
    }
    return b;
}

/* In Map mode each control shows the key (and gamepad input) bound to it, so
 * choosing what to change does not require remembering the table; the picked
 * one is in the accent colour. */
static void refresh_labels(void)
{
    for (int i = 0; i < g_ncontrols; i++) {
        control *c = &g_controls[i];
        if (g_map_state != -2) {
            char key[48], pad[48], text[112];
            astrosession_keysym_name(bindings_target_keysym(c->target), key, sizeof key);
            astrosession_pad_input_name(bindings_target_pad(c->target), pad, sizeof pad);
            if (pad[0])
                g_snprintf(text, sizeof text, "%s\n%s", *key ? key : EM_DASH, pad);
            else
                g_snprintf(text, sizeof text, "%s", *key ? key : EM_DASH);
            gtk_label_set_text(GTK_LABEL(c->label), text);
            gtk_widget_add_css_class(c->label, "caption");
            set_lit(c, c->target == g_map_state);
        } else {
            gtk_label_set_text(GTK_LABEL(c->label),
                               g_object_get_data(G_OBJECT(c->button), "face"));
            gtk_widget_remove_css_class(c->label, "caption");
            set_lit(c, FALSE);   /* tick() relights what is held */
        }
    }
    for (int p = 0; p < ASTROSESSION_PLAYERS; p++) {
        char pad[48] = "";
        if (g_map_state != -2)
            astrosession_pad_input_name(bindings_target_pad(g_dials[p].target),
                                        pad, sizeof pad);
        gtk_label_set_text(GTK_LABEL(g_dials[p].caption),
                           g_map_state != -2 ? (pad[0] ? pad : EM_DASH) : "Knob");
    }
}

/* ---- the knob dial -------------------------------------------------------- */

static void draw_dial(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer d)
{
    dial *dl = d;
    GdkRGBA fg;
    const double cx = w / 2.0, cy = h / 2.0, r = (w < h ? w : h) / 2.0 - 4;
    const int v = astrosession_knob_get(g_session, dl->player);
    const double a = astrosession_knob_angle(v) * M_PI / 180.0;
    const gboolean picked = g_map_state == dl->target;

    gtk_widget_get_color(GTK_WIDGET(area), &fg);
    gdk_cairo_set_source_rgba(cr, &fg);
    cairo_set_line_width(cr, 2);
    cairo_arc(cr, cx, cy, r, (-225.0) * M_PI / 180.0, 45.0 * M_PI / 180.0);   /* the 270 degree sweep */
    cairo_stroke(cr);
    cairo_set_line_width(cr, 1);
    cairo_arc(cr, cx, cy, r * 0.62, 0, 2 * M_PI);
    cairo_stroke(cr);
    if (picked)
        cairo_set_source_rgb(cr, 0.71, 0.20, 0.17);
    cairo_set_line_width(cr, 3);
    cairo_move_to(cr, cx + sin(a) * r * 0.25, cy - cos(a) * r * 0.25);
    cairo_line_to(cr, cx + sin(a) * r * 0.95, cy - cos(a) * r * 0.95);
    cairo_stroke(cr);
}

static void dial_drag(GtkGestureDrag *g, double ox, double oy, dial *dl, double dx, double dy)
{
    const double w = gtk_widget_get_width(dl->area), h = gtk_widget_get_height(dl->area);
    const int v = astrosession_knob_from_point(ox + dx - w / 2.0, oy + dy - h / 2.0,
                                               (w < h ? w : h) / 2.0);
    (void)g;
    if (v >= 0)
        astrosession_knob_set(g_session, dl->player, (uint8_t)v);
    gtk_widget_queue_draw(dl->area);
}

static void on_dial_begin(GtkGestureDrag *g, double x, double y, gpointer d)
{
    dial *dl = d;
    if (g_map_state == -1) { set_map_state(dl->target); return; }
    if (g_map_state != -2) return;
    g_object_set_data(G_OBJECT(g), "ox", GINT_TO_POINTER((int)(x * 100)));
    g_object_set_data(G_OBJECT(g), "oy", GINT_TO_POINTER((int)(y * 100)));
    dial_drag(g, x, y, dl, 0, 0);
}

static void on_dial_update(GtkGestureDrag *g, double dx, double dy, gpointer d)
{
    if (g_map_state != -2) return;
    dial_drag(g, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(g), "ox")) / 100.0,
              GPOINTER_TO_INT(g_object_get_data(G_OBJECT(g), "oy")) / 100.0, d, dx, dy);
}

/* ---- polling -------------------------------------------------------------- */

static void capture_poll(void)
{
    int id;
    char msg[256], stolen[96] = "";
    int target = g_map_state, from = -1;

    if (!astrosession_gamepad_capture_poll(g_session, &id))
        return;
    if (is_knob_target(target) && id < ASTRO_PAD_AXIS_BASE) {
        astrosession_gamepad_capture_begin(g_session);   /* a button cannot turn a knob */
        set_hint("The knob follows an axis: move a stick or trigger");
        return;
    }
    bindings_set_pad(g_session, target, id, &from);
    if (from >= 0)
        g_snprintf(stolen, sizeof stolen, " (was %s)", bindings_target_label(from));
    {
        char name[48];
        astrosession_pad_input_name(id, name, sizeof name);
        g_snprintf(msg, sizeof msg, "%s = pad %s%s", bindings_target_label(target), name, stolen);
    }
    set_map_state(-1);   /* stay armed: remapping several in a row is normal */
    set_hint(msg);
}

static void update_pads_label(void)
{
    char name[64], text[256];
    int n = astrosession_gamepad_count(g_session);
    text[0] = '\0';
    for (int i = 0; i < n; i++) {
        astrosession_gamepad_name(g_session, i, name, sizeof name);
        g_snprintf(text + strlen(text), sizeof text - strlen(text), "%sPlayer %d: %s",
                   i ? "   " : "", i + 1, name);
    }
    gtk_label_set_text(GTK_LABEL(g_pads_label), n ? text : "No gamepad connected");
}

static gboolean tick(gpointer d)
{
    (void)d;
    update_pads_label();
    for (int p = 0; p < ASTROSESSION_PLAYERS; p++)
        gtk_widget_queue_draw(g_dials[p].area);
    if (g_map_state >= 0) {
        capture_poll();
        return G_SOURCE_CONTINUE;
    }
    if (g_map_state != -2)
        return G_SOURCE_CONTINUE;
    for (int i = 0; i < g_ncontrols; i++)
        set_lit(&g_controls[i], astrosession_target_held(g_session, g_controls[i].target));
    return G_SOURCE_CONTINUE;
}

static void on_map(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (!g_tick_id)
        g_tick_id = g_timeout_add(50, tick, NULL);
}

/* Hiding the window (close, F9, menu) leaves Map and stops the poll. */
static void on_unmap(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (g_tick_id) {
        g_source_remove(g_tick_id);
        g_tick_id = 0;
    }
    if (g_window && g_map_state != -2)
        set_map_state(-2);
}

static void on_destroy(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (g_tick_id) {
        g_source_remove(g_tick_id);
        g_tick_id = 0;
    }
    g_window = NULL;
    g_parent = NULL;
    g_map_state = -2;
    g_ncontrols = 0;
}

/* ---- the keypad ----------------------------------------------------------- */

#define KEY_W 62
#define KEY_H 40
#define PKEY_W 52
#define PKEY_H 34

static const char *const key_face[ASTROSESSION_KEY_COUNT] = {
    "C", "\xE2\x86\x91", "\xE2\x86\x93", "%",
    "MR", "MS", "CH", "\xC3\xB7",
    "7", "8", "9", "\xC3\x97",
    "4", "5", "6", "\xE2\x88\x92",
    "1", "2", "3", "+",
    "CE", "0", ".", "=",
};

static GtkWidget *build_keypad(void)
{
    GtkWidget *frame = gtk_frame_new(NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *grid = gtk_grid_new();
    GtkWidget *title = gtk_label_new("Console keypad");
    GtkWidget *sys = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    gtk_widget_add_css_class(title, "heading");
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
    for (int key = 0; key < ASTROSESSION_KEY_COUNT; key++)
        gtk_grid_attach(GTK_GRID(grid),
                        control_button(key_face[key], ASTRO_TARGET_KEYPAD + key, KEY_W, KEY_H),
                        key % 4, key / 4, 1, 1);
    gtk_widget_set_halign(grid, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(sys),
                   control_button("Reset Game",
                                  ASTRO_TARGET_SYSACT + ASTROSESSION_SYSACT_RESET_GAME,
                                  (4 * KEY_W + 18 - 6) / 2, 44));
    gtk_box_append(GTK_BOX(sys),
                   control_button("Reset to CONFIG",
                                  ASTRO_TARGET_SYSACT + ASTROSESSION_SYSACT_RESET_CONFIG,
                                  (4 * KEY_W + 18 - 6) / 2, 44));
    gtk_widget_set_halign(sys, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(box), title);
    gtk_box_append(GTK_BOX(box), grid);
    gtk_box_append(GTK_BOX(box), sys);
    gtk_widget_set_margin_top(box, 10);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_frame_set_child(GTK_FRAME(frame), box);
    return frame;
}

/* ---- one hand controller -------------------------------------------------- */

static GtkWidget *stick_button(const char *face, int player, int act)
{
    return control_button(face, ASTRO_TARGET_PLAYER(player, act), PKEY_W, PKEY_H);
}

static GtkWidget *build_player(int player)
{
    GtkWidget *frame = gtk_frame_new(NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *stick = gtk_grid_new();
    GtkWidget *knobrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *knobcol = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkGesture *drag = gtk_gesture_drag_new();
    dial *dl = &g_dials[player];
    char label[24];
    GtkWidget *title;

    g_snprintf(label, sizeof label, "Player %d", player + 1);
    title = gtk_label_new(label);
    gtk_widget_add_css_class(title, "heading");

    gtk_grid_set_row_spacing(GTK_GRID(stick), 4);
    gtk_grid_set_column_spacing(GTK_GRID(stick), 4);
    gtk_grid_attach(GTK_GRID(stick), stick_button("\xe2\x96\xb2", player, ASTRO_ACT_UP), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(stick), stick_button("\xe2\x97\x80", player, ASTRO_ACT_LEFT), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(stick), stick_button("\xe2\x96\xb6", player, ASTRO_ACT_RIGHT), 2, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(stick), stick_button("\xe2\x96\xbc", player, ASTRO_ACT_DOWN), 1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(stick),
                    control_button("Trigger", ASTRO_TARGET_PLAYER(player, ASTRO_ACT_TRIGGER),
                                   3 * PKEY_W + 8, 40),
                    0, 3, 3, 1);
    gtk_widget_set_halign(stick, GTK_ALIGN_CENTER);

    dl->player = player;
    dl->target = ASTRO_TARGET_PLAYER(player, ASTRO_ACT_KNOB);
    dl->area = gtk_drawing_area_new();
    gtk_widget_set_size_request(dl->area, 92, 92);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(dl->area), draw_dial, dl, NULL);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_dial_begin), dl);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_dial_update), dl);
    gtk_widget_add_controller(dl->area, GTK_EVENT_CONTROLLER(drag));
    dl->caption = gtk_label_new("Knob");
    gtk_widget_add_css_class(dl->caption, "caption");
    gtk_label_set_ellipsize(GTK_LABEL(dl->caption), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(dl->caption), 12);
    gtk_box_append(GTK_BOX(knobcol), dl->area);
    gtk_box_append(GTK_BOX(knobcol), dl->caption);
    {
        GtkWidget *dec = control_button("\xE2\x97\x80", ASTRO_TARGET_PLAYER(player, ASTRO_ACT_KNOB_DEC), 40, PKEY_H);
        GtkWidget *inc = control_button("\xE2\x96\xB6", ASTRO_TARGET_PLAYER(player, ASTRO_ACT_KNOB_INC), 40, PKEY_H);
        gtk_widget_set_valign(dec, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(inc, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(knobrow), dec);
        gtk_box_append(GTK_BOX(knobrow), knobcol);
        gtk_box_append(GTK_BOX(knobrow), inc);
    }
    gtk_widget_set_halign(knobrow, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(knobrow, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(box), title);
    gtk_box_append(GTK_BOX(box), stick);
    gtk_box_append(GTK_BOX(box), knobrow);
    gtk_widget_set_margin_top(box, 10);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_frame_set_child(GTK_FRAME(frame), box);
    return frame;
}

/* ---- keyboard ------------------------------------------------------------- */

gboolean astro_keypad_map_forward_key(uint32_t keysym)
{
    char name[48], msg[256], stolen[96] = "";
    int target = g_map_state, from = -1;

    if (g_map_state == -2)
        return FALSE;
    if (g_map_state == -1)
        return TRUE;   /* swallow keys while choosing a target */
    if (is_knob_target(target)) {
        set_hint("The knob follows a gamepad axis: move a stick or trigger");
        return TRUE;
    }
    bindings_set_keysym(g_session, target, keysym, &from);
    astrosession_keysym_name(keysym, name, sizeof name);
    if (from >= 0)
        g_snprintf(stolen, sizeof stolen, " (was %s)", bindings_target_label(from));
    g_snprintf(msg, sizeof msg, "%s = %s%s", bindings_target_label(target), name, stolen);
    set_map_state(-1);   /* stay armed */
    set_hint(msg);
    return TRUE;
}

static gboolean on_key_pressed(GtkEventControllerKey *c, guint keyval,
                               guint code, GdkModifierType st, gpointer d)
{
    (void)c; (void)d;
    /* Exactly like the main window, so playing works whichever window has
     * focus: Map capture and the bindings first, then F9. */
    if (astro_window_input_key(g_session, code, st, TRUE))
        return TRUE;
    if (keyval == GDK_KEY_F9) {
        astro_keypad_window_toggle(NULL, g_session);
        return TRUE;
    }
    return FALSE;
}

static void on_key_released(GtkEventControllerKey *c, guint keyval,
                            guint code, GdkModifierType st, gpointer d)
{
    (void)c; (void)d; (void)keyval;
    astro_window_input_key(g_session, code, st, FALSE);
}

/* Keys held into a focus change must not stay down. */
static void on_active_changed(GObject *o, GParamSpec *ps, gpointer d)
{
    (void)ps; (void)d;
    if (!gtk_window_is_active(GTK_WINDOW(o)))
        astrosession_release_all(g_session);
}

/* ---- the window ----------------------------------------------------------- */

static void on_map_toggled(GtkToggleButton *b, gpointer d)
{
    int want;
    (void)d;
    want = gtk_toggle_button_get_active(b) ? (g_map_state != -2 ? g_map_state : -1) : -2;
    if (want != g_map_state)
        set_map_state(want);
}

/* Clears both the key and the pad input of the picked control. */
static void on_clear_clicked(GtkButton *b, gpointer d)
{
    char msg[128];
    int target = g_map_state;
    (void)b; (void)d;
    if (target < 0)
        return;
    bindings_clear(g_session, target);
    set_map_state(-1);
    g_snprintf(msg, sizeof msg, "%s cleared", bindings_target_label(target));
    set_hint(msg);
}

static void on_defaults_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    bindings_reset_defaults(g_session);
    refresh_labels();
    set_hint("Bindings reset to the defaults");
}

static gboolean on_close(GtkWindow *w, gpointer d)
{
    (void)d;
    /* Hide, do not destroy: the position survives closing it. Unmap leaves
     * Map mode. */
    gtk_widget_set_visible(GTK_WIDGET(w), FALSE);
    return TRUE;
}

static void install_css(void)
{
    static gboolean done;
    GtkCssProvider *css;
    if (done) return;
    done = TRUE;
    css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        ".astro-gold { background-image: none; background-color: #e8c14a;"
        " color: #3a2c00; font-weight: bold; }"
        ".astro-gold:hover { background-color: #f0cf6a; }"
        "button.astro-lit, button.astro-gold.astro-lit { background-image: none;"
        " background-color: #b5342c; color: #fff; }"
        "button.astro-toggle:checked { background-image: none;"
        " background-color: #b5342c; color: #fff; }");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

static void build_window(GtkWindow *parent)
{
    GtkWidget *root, *top, *left, *players, *grid, *maprow, *toolbar, *header, *defaults;
    GtkEventController *keys;

    install_css();
    g_parent = parent;
    g_window = adw_window_new();
    if (parent)
        g_object_add_weak_pointer(G_OBJECT(parent), (gpointer *)&g_parent);
    gtk_window_set_title(GTK_WINDOW(g_window), "Controllers");
    gtk_window_set_icon_name(GTK_WINDOW(g_window), astro_icon_name());
    gtk_window_set_transient_for(GTK_WINDOW(g_window), parent);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(g_window), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(g_window), FALSE);
    g_signal_connect(g_window, "close-request", G_CALLBACK(on_close), NULL);
    g_signal_connect(g_window, "map", G_CALLBACK(on_map), NULL);
    g_signal_connect(g_window, "unmap", G_CALLBACK(on_unmap), NULL);
    g_signal_connect(g_window, "destroy", G_CALLBACK(on_destroy), NULL);
    g_signal_connect(g_window, "notify::is-active", G_CALLBACK(on_active_changed), NULL);

    g_ncontrols = 0;

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);

    top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_box_append(GTK_BOX(left), build_keypad());
    gtk_box_append(GTK_BOX(top), left);
    players = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    for (int p = 0; p < ASTROSESSION_PLAYERS; p++)
        gtk_grid_attach(GTK_GRID(grid), build_player(p), p % 2, p / 2, 1, 1);
    gtk_box_append(GTK_BOX(players), grid);
    gtk_box_append(GTK_BOX(top), players);
    gtk_box_append(GTK_BOX(root), top);

    g_pads_label = gtk_label_new("");
    gtk_widget_add_css_class(g_pads_label, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(g_pads_label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(g_pads_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(g_pads_label), 30);
    gtk_label_set_max_width_chars(GTK_LABEL(g_pads_label), 30);
    gtk_label_set_wrap(GTK_LABEL(g_pads_label), TRUE);
    gtk_label_set_lines(GTK_LABEL(g_pads_label), 2);
    gtk_widget_set_vexpand(g_pads_label, TRUE);
    gtk_widget_set_valign(g_pads_label, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(left), g_pads_label);

    maprow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_map_button = gtk_toggle_button_new_with_label("Map");
    gtk_widget_add_css_class(g_map_button, "astro-toggle");
    gtk_widget_set_size_request(g_map_button, 72, -1);
    gtk_widget_set_focusable(g_map_button, FALSE);
    gtk_widget_set_focus_on_click(g_map_button, FALSE);
    g_signal_connect(g_map_button, "toggled", G_CALLBACK(on_map_toggled), NULL);
    g_clear_button = gtk_button_new_with_label("Clear");
    gtk_widget_set_size_request(g_clear_button, 72, -1);
    gtk_widget_set_focusable(g_clear_button, FALSE);
    gtk_widget_set_focus_on_click(g_clear_button, FALSE);
    gtk_widget_set_sensitive(g_clear_button, FALSE);
    g_signal_connect(g_clear_button, "clicked", G_CALLBACK(on_clear_clicked), NULL);
    defaults = gtk_button_new_with_label("Defaults");
    gtk_widget_set_size_request(defaults, 84, -1);
    gtk_widget_set_focusable(defaults, FALSE);
    gtk_widget_set_focus_on_click(defaults, FALSE);
    g_signal_connect(defaults, "clicked", G_CALLBACK(on_defaults_clicked), NULL);
    g_map_hint = gtk_label_new("");
    gtk_widget_add_css_class(g_map_hint, "dim-label");
    gtk_widget_set_hexpand(g_map_hint, TRUE);
    gtk_label_set_xalign(GTK_LABEL(g_map_hint), 0);
    gtk_label_set_wrap(GTK_LABEL(g_map_hint), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(g_map_hint), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_lines(GTK_LABEL(g_map_hint), 2);
    gtk_label_set_ellipsize(GTK_LABEL(g_map_hint), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(g_map_hint), 30);
    gtk_label_set_max_width_chars(GTK_LABEL(g_map_hint), 30);
    gtk_widget_set_size_request(g_map_hint, -1, 40);   /* two lines, always */
    gtk_box_append(GTK_BOX(maprow), g_map_button);
    gtk_box_append(GTK_BOX(maprow), g_clear_button);
    gtk_box_append(GTK_BOX(maprow), defaults);
    gtk_box_append(GTK_BOX(maprow), g_map_hint);
    gtk_box_append(GTK_BOX(left), maprow);

    header = adw_header_bar_new();
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), root);
    adw_window_set_content(ADW_WINDOW(g_window), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), NULL);
    gtk_widget_add_controller(g_window, keys);

    set_map_state(-2);
}

void astro_keypad_window_toggle(GtkWindow *parent, astrosession *session)
{
    g_session = session;
    if (!g_window)
        build_window(parent);

    if (gtk_widget_get_visible(g_window))
        gtk_widget_set_visible(g_window, FALSE);   /* unmap leaves Map mode */
    else
        gtk_window_present(GTK_WINDOW(g_window));
}

gboolean astro_keypad_window_is_visible(void)
{
    return g_window && gtk_widget_get_visible(g_window);
}
