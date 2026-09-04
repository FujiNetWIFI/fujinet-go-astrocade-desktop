/*
 * The Astrocade keypad window: the console's 24-key calculator keypad as a
 * 6x4 grid (the rightmost column -- % / divide / multiply / minus / plus /
 * equals -- is the gold column on real hardware, tinted here to match),
 * plus a System row (Reset Game / Reset to CONFIG) and a Map row for
 * remapping keys.
 *
 * Every keypad button is pressed with a raw GtkGestureClick (press AND
 * release), not GtkButton's "clicked" -- a keypad key is held for as long as
 * the finger is down, and astrosession_keypad_set models exactly that.
 *
 * MAP MODE: the Map button arms a two-step rebind -- click a keypad/system
 * button to pick the target, then press a keyboard key to bind it (through
 * core/src/bindings.c). forward_key here and in the main window both route
 * the next keystroke to astro_keypad_map_forward_key while armed.
 *
 * Singleton, hidden-not-destroyed, so a remap survives closing the window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "keypad_window.h"

#include "../keysym_map.h"
#include "bindings.h"

/* row-major, matching the astrosession_key enum order */
static const char *k_labels[ASTROSESSION_KEY_COUNT] = {
    "C", "\xE2\x86\x91", "\xE2\x86\x93", "%",          /* C  up  down  %   */
    "MR", "MS", "CH", "\xC3\xB7",                       /* MR MS CH ÷       */
    "7", "8", "9", "\xC3\x97",                          /* 7 8 9 ×          */
    "4", "5", "6", "\xE2\x88\x92",                      /* 4 5 6 −          */
    "1", "2", "3", "+",                                 /* 1 2 3 +          */
    "CE", "0", ".", "=",                                /* CE 0 . =         */
};

static GtkWindow *g_win;
static astrosession *g_session;
static GtkWidget *g_key_buttons[ASTROSESSION_KEY_COUNT];
static GtkWidget *g_sysact_buttons[ASTROSESSION_SYSACT_COUNT];
static GtkWidget *g_status_label;
static GtkWidget *g_map_btn;

typedef enum {
    MAP_IDLE = 0,
    MAP_PICK_TARGET,    /* Map armed, waiting for a button click */
    MAP_WAIT_INPUT,     /* target picked, waiting for a keystroke */
} MapState;

static MapState g_map_state;
static int g_map_target = -1;   /* an ASTRO_TARGET_* index */

static void set_highlighted(GtkWidget *w, gboolean on)
{
    if (!w) return;
    if (on) gtk_widget_add_css_class(w, "suggested-action");
    else    gtk_widget_remove_css_class(w, "suggested-action");
}

static void map_set_status(const char *text)
{
    if (g_status_label)
        gtk_label_set_text(GTK_LABEL(g_status_label), text ? text : "");
}

static GtkWidget *button_for_target(int target)
{
    if (target >= 0 && target < ASTROSESSION_KEY_COUNT)
        return g_key_buttons[target];
    if (target >= ASTRO_TARGET_SYSACT && target < ASTRO_TARGET_COUNT)
        return g_sysact_buttons[target - ASTRO_TARGET_SYSACT];
    return NULL;
}

static void map_cancel(void)
{
    if (g_map_target >= 0)
        set_highlighted(button_for_target(g_map_target), FALSE);
    g_map_target = -1;
    g_map_state = MAP_IDLE;
    if (g_map_btn)
        gtk_button_set_label(GTK_BUTTON(g_map_btn), "Map");
    map_set_status("");
}

static void map_pick_target(int target)
{
    g_map_target = target;
    g_map_state = MAP_WAIT_INPUT;
    set_highlighted(button_for_target(target), TRUE);
    char msg[128];
    g_snprintf(msg, sizeof msg, "Press a key to bind to %s (Esc cancels)",
               bindings_target_label(target));
    map_set_status(msg);
}

/* consume a keystroke as the new binding for the armed target */
gboolean astro_keypad_map_forward_key(int keysym)
{
    if (g_map_state != MAP_WAIT_INPUT || g_map_target < 0)
        return FALSE;
    if (keysym == ASTROSESSION_KEYSYM_ESCAPE) {
        map_set_status("Cancelled");
        int t = g_map_target;
        set_highlighted(button_for_target(t), FALSE);
        g_map_target = -1;
        g_map_state = MAP_IDLE;
        if (g_map_btn) gtk_button_set_label(GTK_BUTTON(g_map_btn), "Map");
        return TRUE;
    }

    int stole = -1;
    bindings_set_keysym(g_session, g_map_target, keysym, &stole);

    char status[256];
    const char *tname = bindings_target_label(g_map_target);
    if (stole >= 0)
        g_snprintf(status, sizeof status, "%s mapped (taken from %s)", tname,
                   bindings_target_label(stole));
    else
        g_snprintf(status, sizeof status, "%s mapped", tname);

    set_highlighted(button_for_target(g_map_target), FALSE);
    g_map_target = -1;
    g_map_state = MAP_IDLE;
    if (g_map_btn) gtk_button_set_label(GTK_BUTTON(g_map_btn), "Map");
    map_set_status(status);
    return TRUE;
}

/* ---- keypad buttons ---- */

static void key_pressed(GtkGestureClick *g, int n, double x, double y,
                        gpointer user_data)
{
    int key = GPOINTER_TO_INT(user_data);
    (void)g; (void)n; (void)x; (void)y;
    if (g_map_state == MAP_PICK_TARGET) {
        map_pick_target(key);
        return;
    }
    if (g_map_state == MAP_WAIT_INPUT)
        return;         /* ignore button presses while waiting for a key */
    astrosession_keypad_set(g_session, key, 1);
}

static void key_released(GtkGestureClick *g, int n, double x, double y,
                         gpointer user_data)
{
    int key = GPOINTER_TO_INT(user_data);
    (void)g; (void)n; (void)x; (void)y;
    if (g_map_state != MAP_IDLE)
        return;
    astrosession_keypad_set(g_session, key, 0);
}

static void sysact_pressed(GtkGestureClick *g, int n, double x, double y,
                           gpointer user_data)
{
    int act = GPOINTER_TO_INT(user_data);
    (void)g; (void)n; (void)x; (void)y;
    int target = ASTRO_TARGET_SYSACT + act;
    if (g_map_state == MAP_PICK_TARGET) {
        map_pick_target(target);
        return;
    }
    if (g_map_state == MAP_WAIT_INPUT)
        return;
    astrosession_sysaction_fire(g_session, act);
}

static GtkWidget *make_key_button(int key)
{
    GtkWidget *btn = gtk_button_new_with_label(k_labels[key]);
    gtk_widget_set_hexpand(btn, TRUE);
    gtk_widget_set_size_request(btn, 56, 44);
    /* the gold column is col 3 (the rightmost) */
    if (key % 4 == 3)
        gtk_widget_add_css_class(btn, "astro-gold");

    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(key_pressed),
                     GINT_TO_POINTER(key));
    g_signal_connect(click, "released", G_CALLBACK(key_released),
                     GINT_TO_POINTER(key));
    gtk_widget_add_controller(btn, GTK_EVENT_CONTROLLER(click));
    g_key_buttons[key] = btn;
    return btn;
}

static GtkWidget *make_sysact_button(int act, const char *label)
{
    GtkWidget *btn = gtk_button_new_with_label(label);
    gtk_widget_set_hexpand(btn, TRUE);
    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(sysact_pressed),
                     GINT_TO_POINTER(act));
    gtk_widget_add_controller(btn, GTK_EVENT_CONTROLLER(click));
    g_sysact_buttons[act] = btn;
    return btn;
}

/* ---- Map row ---- */

static void on_map_clicked(GtkButton *b, gpointer user_data)
{
    (void)b; (void)user_data;
    if (g_map_state == MAP_IDLE) {
        g_map_state = MAP_PICK_TARGET;
        gtk_button_set_label(GTK_BUTTON(g_map_btn), "Cancel");
        map_set_status("Click a key to remap, then press its new key");
    } else {
        map_cancel();
    }
}

static void on_reset_bindings_clicked(GtkButton *b, gpointer user_data)
{
    (void)b; (void)user_data;
    bindings_reset_defaults(g_session);
    map_set_status("Bindings reset to defaults");
}

/* ---- window keyboard: drive the keypad, or feed Map mode ---- */

static gboolean on_key_pressed(GtkEventControllerKey *c, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer user_data)
{
    (void)c; (void)keycode; (void)state; (void)user_data;
    if (keyval == GDK_KEY_F9) {
        gtk_widget_set_visible(GTK_WIDGET(g_win), FALSE);
        return TRUE;
    }
    int keysym = astro_keysym_from_key_event(keyval);
    if (astro_keypad_map_forward_key(keysym))
        return TRUE;
    astro_mapping m = bindings_resolve_keysym(keysym);
    if (m.kind == ASTRO_MAP_KEY)
        astrosession_keypad_set(g_session, m.value, 1);
    else if (m.kind == ASTRO_MAP_SYSACT)
        astrosession_sysaction_fire(g_session, m.value);
    return TRUE;
}

static void on_key_released(GtkEventControllerKey *c, guint keyval,
                            guint keycode, GdkModifierType state,
                            gpointer user_data)
{
    (void)c; (void)keycode; (void)state; (void)user_data;
    if (g_map_state != MAP_IDLE)
        return;
    astro_mapping m = bindings_resolve_keysym(astro_keysym_from_key_event(keyval));
    if (m.kind == ASTRO_MAP_KEY)
        astrosession_keypad_set(g_session, m.value, 0);
}

/* ---- construction ---- */

static void install_css(void)
{
    static gboolean done;
    if (done) return;
    done = TRUE;
    GtkCssProvider *css = gtk_css_provider_new();
    /* dark-red app accent on the gold column's pressed/hover, matching the
     * icon; the gold column is the console's own accent stripe. */
    gtk_css_provider_load_from_string(css,
        ".astro-gold { background-image: none;"
        " background-color: #e8c14a; color: #3a2c00; font-weight: bold; }"
        ".astro-gold:hover { background-color: #f0cf6a; }");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

static GtkWidget *build_content(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);

    /* 6x4 keypad grid */
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
    for (int key = 0; key < ASTROSESSION_KEY_COUNT; key++) {
        int row = key / 4, col = key % 4;
        gtk_grid_attach(GTK_GRID(grid), make_key_button(key), col, row, 1, 1);
    }
    gtk_box_append(GTK_BOX(box), grid);

    /* System row */
    GtkWidget *sys = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(sys),
        make_sysact_button(ASTROSESSION_SYSACT_RESET_GAME, "Reset Game"));
    gtk_box_append(GTK_BOX(sys),
        make_sysact_button(ASTROSESSION_SYSACT_RESET_CONFIG, "Reset to CONFIG"));
    gtk_box_append(GTK_BOX(box), sys);

    /* Map row */
    GtkWidget *maprow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    g_map_btn = gtk_button_new_with_label("Map");
    g_signal_connect(g_map_btn, "clicked", G_CALLBACK(on_map_clicked), NULL);
    GtkWidget *reset = gtk_button_new_with_label("Reset Bindings");
    g_signal_connect(reset, "clicked", G_CALLBACK(on_reset_bindings_clicked), NULL);
    g_status_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(g_status_label), 0.0f);
    gtk_widget_set_hexpand(g_status_label, TRUE);
    gtk_box_append(GTK_BOX(maprow), g_map_btn);
    gtk_box_append(GTK_BOX(maprow), reset);
    gtk_box_append(GTK_BOX(maprow), g_status_label);
    gtk_box_append(GTK_BOX(box), maprow);

    return box;
}

static void ensure_window(GtkWindow *parent)
{
    if (g_win)
        return;
    install_css();

    g_win = GTK_WINDOW(gtk_window_new());
    gtk_window_set_title(g_win, "Keypad");
    gtk_window_set_transient_for(g_win, parent);
    gtk_window_set_resizable(g_win, FALSE);
    gtk_window_set_hide_on_close(g_win, TRUE);   /* singleton: hide, not destroy */
    gtk_window_set_child(g_win, build_content());

    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), NULL);
    gtk_widget_add_controller(GTK_WIDGET(g_win), keys);
}

void astro_keypad_window_toggle(GtkWindow *parent, astrosession *session)
{
    g_session = session;
    ensure_window(parent);
    gboolean vis = gtk_widget_get_visible(GTK_WIDGET(g_win));
    gtk_widget_set_visible(GTK_WIDGET(g_win), !vis);
    if (!vis)
        gtk_window_present(g_win);
}
