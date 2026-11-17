/*
 * AstroWindow: main window of the GNOME frontend. Header bar + menu over the
 * emulator display; keyboard capture routes everything except F9 (Controllers
 * window), F11 (full screen) and F12 (debugger) to the console. The
 * Controllers window sends its typed keys through astro_window_key too.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "window.h"

#include <stdlib.h>
#include <string.h>

#include "bindings.h"
#include "controllers/controllers_window.h"
#include "debugger/dbg_window.h"
#include "display.h"
#include "help/help_window.h"
#include "prefs.h"

struct _AstroWindow {
    AdwApplicationWindow parent_instance;

    astrosession *session;
    AstroDisplay *display;
    GtkStack *stack;          /* the picture, or the No-BIOS page */
    AdwToastOverlay *toasts;
    /* GTK4's key-pressed carries no repeat flag; tracked by hand so holding a
     * sysaction key (Backspace) fires Reset Game once, not ~60x/sec. Cleared
     * on focus loss. */
    guint sysact_timer_id;
    unsigned pad_generation;       /* the pad set the last toast was for */
    unsigned overlay_generation;   /* the overlay auto-on the last toast was for */
};

G_DEFINE_FINAL_TYPE(AstroWindow, astro_window, ADW_TYPE_APPLICATION_WINDOW)

void astro_window_toast(AstroWindow *self, const char *message)
{
    adw_toast_overlay_add_toast(self->toasts, adw_toast_new(message));
}

static void show_message(GtkWidget *over, const char *heading, const char *body)
{
    AdwDialog *dlg = adw_alert_dialog_new(heading, body);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dlg), "ok", "OK");
    adw_dialog_present(dlg, over);
}

/* The picture while there is a BIOS to boot, else the No-BIOS page. */
void astro_window_show_page(AstroWindow *self)
{
    gtk_stack_set_visible_child_name(self->stack,
        astrosession_has_system_roms(self->session) ? "display" : "nobios");
}

void astro_window_restart_session(AstroWindow *self)
{
    astrosession_start_opts opts;
    astrosession_stop(self->session);
    astrosession_default_opts(self->session, &opts);
    if (astrosession_start(self->session, &opts) != 0)
        astro_window_toast(self, astrosession_last_error(self->session));
    astro_window_show_page(self);
}

/* Starts the machine once a BIOS is there to boot (after an import). */
void astro_window_start(AstroWindow *self)
{
    astro_window_show_page(self);
    if (!astrosession_is_running(self->session) &&
        astrosession_has_system_roms(self->session))
        astro_window_restart_session(self);
}

/* ---- keyboard capture ---------------------------------------------------
 * Everything goes to the machine except the function keys handled in
 * astro_window_key. Both press and release are forwarded; a missed release
 * leaves that keypad key down in the machine. Bindings are resolved through
 * the bound table so a Controllers-window "Map" remap takes effect here too. */

/* System actions (RESET GAME, Reset to CONFIG, Stop in Debugger) from a key, a
 * pad button or the Controllers window, with the same feedback as the Machine
 * menu. UI thread. */
void astro_window_run_sysaction(AstroWindow *self, int sysact)
{
    switch (sysact) {
    case ASTROSESSION_SYSACT_RESET_GAME:
        astrosession_sysaction_fire(self->session, ASTROSESSION_SYSACT_RESET_GAME);
        astro_window_toast(self, "Reset Game");
        break;
    case ASTROSESSION_SYSACT_RESET_CONFIG:
        if (astrosession_reset_to_config(self->session) != 0)
            astro_window_toast(self, astrosession_last_error(self->session));
        else
            astro_window_toast(self, "Reset to CONFIG");
        break;
    case ASTROSESSION_SYSACT_DEBUG_STOP:
        /* open the debugger first so the stop has somewhere to show */
        astro_debugger_show(GTK_WINDOW(self), self->session);
        astrosession_sysaction_fire(self->session, ASTROSESSION_SYSACT_DEBUG_STOP);
        break;
    default:
        break;
    }
}

/* Everything posted since the last look (a bound key or a gamepad button posts
 * it; the gamepad thread cannot call into the toolkit). */
static void drain_sysactions(AstroWindow *self)
{
    unsigned pending = astrosession_sysaction_take(self->session);
    int act;
    for (act = 0; act < ASTROSESSION_SYSACT_COUNT; act++)
        if (pending & (1u << act))
            astro_window_run_sysaction(self, act);
}

/* The keycodes whose press was typed (Bally BASIC overlay) and not yet
 * released: GTK4's key-pressed carries no repeat flag, and a held key must
 * type once. */
static guint8 g_typed_down[256 / 8];

/* With the Bally BASIC overlay on, a printable key that no binding took types
 * its character: the keypad sequence for it is queued in the core. The
 * character is the one the key produced (Shift applies). */
static gboolean type_key(AstroWindow *self, guint keyval, guint keycode, GdkModifierType state)
{
    guint32 chr;

    if (!astrosession_overlay_get(self->session) ||
        (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_META_MASK)))
        return FALSE;
    switch (keyval) {
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
        chr = '\r';
        break;
    case GDK_KEY_BackSpace:
        chr = 8;
        break;
    default:
        chr = gdk_keyval_to_unicode(keyval);
        break;
    }
    if (chr == 0 || (chr < 0x20 && chr != '\r' && chr != 8))
        return FALSE;
    if (keycode < 256) {
        if (g_typed_down[keycode / 8] & (1u << (keycode % 8)))
            return TRUE;                /* a repeat of a key already typed */
        if (astrosession_type_char(self->session, chr))
            g_typed_down[keycode / 8] |= (guint8)(1u << (keycode % 8));
        else
            return FALSE;
        return TRUE;
    }
    return astrosession_type_char(self->session, chr) != 0;
}

/* The one key path: the main window and the Controllers window both end here,
 * so Map mode, the bindings, F9 (Controllers), F11, F12 and BASIC typing
 * behave the same whichever has the focus. The keysym names the PHYSICAL key
 * (evdev = GTK keycode - 8), so NumLock and Shift change nothing. Returns
 * whether the key was consumed. */
gboolean astro_window_key(AstroWindow *self, guint keyval, guint keycode,
                          GdkModifierType state, gboolean down)
{
    const uint32_t keysym = astrosession_keysym_from_evdev(keycode >= 8 ? keycode - 8 : 0);
    const int ctrl = (state & GDK_CONTROL_MASK) != 0;

    if (!down) {
        /* A release always reaches the session: it clears exactly what the
         * press asserted, even if Map mode was armed in between. Map only
         * keeps the release of the key it took out of the machine's way. */
        const gboolean released = astrosession_input_key(self->session, keysym, 0, ctrl) != 0;
        if (keycode < 256)
            g_typed_down[keycode / 8] &= (guint8)~(1u << (keycode % 8));
        if (astro_controllers_map_key(keyval, keycode, state, FALSE))
            return TRUE;
        return released;
    }

    /* Map capture, then the bindings (Ctrl chords are the menu's) */
    if (astro_controllers_map_key(keyval, keycode, state, TRUE))
        return TRUE;
    if (astrosession_input_key(self->session, keysym, 1, ctrl)) {
        drain_sysactions(self);
        return TRUE;
    }

    /* The window's own keys, deliberately not bindable: they are how you
     * reach the panels that do the binding. */
    switch (keyval) {
    case GDK_KEY_F9:
        astro_controllers_toggle(GTK_WINDOW(self), self->session);
        return TRUE;
    case GDK_KEY_F11:
        if (gtk_window_is_fullscreen(GTK_WINDOW(self)))
            gtk_window_unfullscreen(GTK_WINDOW(self));
        else
            gtk_window_fullscreen(GTK_WINDOW(self));
        return TRUE;
    case GDK_KEY_F12:
        astro_debugger_toggle(GTK_WINDOW(self), self->session);
        return TRUE;
    case GDK_KEY_r:
    case GDK_KEY_R:
        if (state & GDK_CONTROL_MASK) {
            gtk_widget_activate_action(GTK_WIDGET(self), "win.reset-config", NULL);
            return TRUE;
        }
        break;
    default:
        break;
    }
    return type_key(self, keyval, keycode, state);
}

/* Lets go of every key the keyboard holds, and stops typing (the mouse's and
 * the pads' holds are their own sources). */
void astro_window_release_keys(AstroWindow *self)
{
    astrosession_release_all(self->session);
    memset(g_typed_down, 0, sizeof g_typed_down);
}

/* The Help context the main window shows now: "nobios" on the No-BIOS page,
 * "prefs/<page>" while Preferences is open, else "main". */
const char *astro_window_help_context(AstroWindow *self)
{
    AdwDialog *d = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(self));
    if (d && ADW_IS_PREFERENCES_DIALOG(d)) {
        static char ctx[64];
        const char *page = adw_preferences_dialog_get_visible_page_name(ADW_PREFERENCES_DIALOG(d));
        g_snprintf(ctx, sizeof ctx, "prefs/%s", page ? page : "Machine");
        return ctx;
    }
    if (g_strcmp0(gtk_stack_get_visible_child_name(self->stack), "nobios") == 0)
        return "nobios";
    return "main";
}

/* app.help: every window's Ctrl+Shift+H, for what it shows. Not a cast of
 * the active window: it can be the debugger, the controllers or the log. */
static void action_help(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    (void)a; (void)p;
    astro_help_for(gtk_application_get_active_window(GTK_APPLICATION(user_data)));
}

static const GActionEntry app_actions[] = {
    { "help", action_help, NULL, NULL, NULL, { 0 } },
};

static gboolean on_key_pressed(GtkEventControllerKey *controller, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer user_data)
{
    AstroWindow *self = ASTRO_WINDOW(user_data);
    (void)controller;

    /* F1 is Help while Preferences or another dialog is open */
    if (keyval == GDK_KEY_F1 && adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(self)) &&
        !(state & GDK_MODIFIER_MASK & ~GDK_LOCK_MASK)) {
        astro_help_for(GTK_WINDOW(self));
        return TRUE;
    }
    return astro_window_key(self, keyval, keycode, state, TRUE);
}

static void on_key_released(GtkEventControllerKey *controller, guint keyval,
                            guint keycode, GdkModifierType state,
                            gpointer user_data)
{
    (void)controller;
    astro_window_key(ASTRO_WINDOW(user_data), keyval, keycode, state, FALSE);
}

static void on_focus_leave(GtkEventControllerFocus *controller,
                           gpointer user_data)
{
    (void)controller;
    /* a held key never gets its release event once focus is gone */
    astro_window_release_keys(ASTRO_WINDOW(user_data));
}

/* ---- actions ------------------------------------------------------------ */

static void action_controllers(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    AstroWindow *self = ASTRO_WINDOW(user_data);
    (void)a; (void)p;
    astro_controllers_toggle(GTK_WINDOW(self), self->session);
}

static void action_reset_config(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    AstroWindow *self = ASTRO_WINDOW(user_data);
    (void)a; (void)p;
    if (astrosession_reset_to_config(self->session) != 0)
        astro_window_toast(self, astrosession_last_error(self->session));
    else
        astro_window_toast(self, "Reset to CONFIG");
}

static void action_reset_game(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    AstroWindow *self = ASTRO_WINDOW(user_data);
    (void)a; (void)p;
    astrosession_reset_game(self->session);
    astro_window_toast(self, "Reset Game");
}

/* ---- the BIOS ---------------------------------------------------------- */

static const char *const ROM_EXTS[] = { "bin", "rom", "zip", NULL };

GtkFileDialog *astro_file_dialog(const char *title, const char *filter_name,
                                 const char *const *exts)
{
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *these = gtk_file_filter_new();
    GtkFileFilter *all = gtk_file_filter_new();

    gtk_file_filter_set_name(these, filter_name);
    for (int i = 0; exts && exts[i]; i++)
        gtk_file_filter_add_suffix(these, exts[i]);   /* any case */
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, these);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_title(dlg, title);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    g_object_unref(these);
    g_object_unref(all);
    g_object_unref(filters);
    return dlg;
}

/* Each chosen file (or the folder) through the import, then one message
 * saying what came of it all, and the machine starts if it now can. */
static void on_bios_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    AstroWindow *self = ASTRO_WINDOW(user_data);
    const gboolean folder = GPOINTER_TO_INT(g_object_get_data(src, "astro-folder"));
    g_autoptr(GListModel) files = NULL;
    g_autoptr(GFile) dir = NULL;
    GString *log = g_string_new(NULL);
    int imported = 0;

    if (folder)
        dir = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL);
    else
        files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(src), res, NULL);
    for (guint i = 0; ; i++) {
        g_autoptr(GFile) f = NULL;
        if (folder) {
            if (i > 0 || !dir)
                break;
            f = g_object_ref(dir);
        } else {
            if (!files || i >= g_list_model_get_n_items(files))
                break;
            f = g_list_model_get_item(files, i);
        }
        g_autofree char *path = g_file_get_path(f);
        char msg[1024];
        if (!path)
            continue;
        if (astrosession_rom_import(self->session, path, msg, sizeof msg) == 0)
            imported++;
        g_string_append_printf(log, "%s%s", log->len ? "\n" : "", msg);
    }
    if (log->len)
        show_message(GTK_WIDGET(self), imported ? "BIOS Imported" : "Nothing Imported", log->str);
    g_string_free(log, TRUE);
    astro_window_start(self);
}

static void import_bios(AstroWindow *self, gboolean folder)
{
    GtkFileDialog *dlg = astro_file_dialog(folder ? "Import BIOS from a Folder" : "Import BIOS",
                                           "BIOS images and ROM sets", ROM_EXTS);
    g_object_set_data(G_OBJECT(dlg), "astro-folder", GINT_TO_POINTER(folder));
    if (folder)
        gtk_file_dialog_select_folder(dlg, GTK_WINDOW(self), NULL, on_bios_chosen, self);
    else
        gtk_file_dialog_open_multiple(dlg, GTK_WINDOW(self), NULL, on_bios_chosen, self);
    g_object_unref(dlg);
}

static void action_import_bios(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    (void)a; (void)p;
    import_bios(ASTRO_WINDOW(user_data), FALSE);
}

static void action_import_bios_folder(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    (void)a; (void)p;
    import_bios(ASTRO_WINDOW(user_data), TRUE);
}

static void on_nobios_import(GtkButton *b, gpointer user_data)
{
    import_bios(ASTRO_WINDOW(user_data),
                GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "astro-folder")));
}

/* In place of the picture until a BIOS is imported. */
static GtkWidget *nobios_page(AstroWindow *self)
{
    GtkWidget *page = adw_status_page_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *files = gtk_button_new_with_label("Import BIOS\xe2\x80\xa6");
    GtkWidget *folder = gtk_button_new_with_label("Import a Folder\xe2\x80\xa6");

    adw_status_page_set_icon_name(ADW_STATUS_PAGE(page), astro_icon_name());
    adw_status_page_set_title(ADW_STATUS_PAGE(page), "No Astrocade BIOS");
    adw_status_page_set_description(ADW_STATUS_PAGE(page),
        "Bally's BIOS ROM is not part of the app. Import the one you have "
        "\xe2\x80\x94 astro.bin, ballyhlc.bin or bioswhit.bin, a zip such as "
        "MAME's astrocde set, or a folder \xe2\x80\x94 and the Astrocade starts.");
    gtk_widget_add_css_class(files, "pill");
    gtk_widget_add_css_class(files, "suggested-action");
    gtk_widget_add_css_class(folder, "pill");
    g_object_set_data(G_OBJECT(folder), "astro-folder", GINT_TO_POINTER(1));
    g_signal_connect(files, "clicked", G_CALLBACK(on_nobios_import), self);
    g_signal_connect(folder, "clicked", G_CALLBACK(on_nobios_import), self);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), files);
    gtk_box_append(GTK_BOX(box), folder);
    gtk_box_append(GTK_BOX(box), astro_help_button("nobios"));
    adw_status_page_set_child(ADW_STATUS_PAGE(page), box);
    return page;
}

static void on_cart_chosen(GObject *source, GAsyncResult *res, gpointer user_data)
{
    AstroWindow *self = ASTRO_WINDOW(user_data);
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    GFile *file = gtk_file_dialog_open_finish(dialog, res, NULL);
    if (!file)
        return;
    char *path = g_file_get_path(file);
    if (path && astrosession_load_cart(self->session, path) != 0)
        astro_window_toast(self, astrosession_last_error(self->session));
    g_free(path);
    g_object_unref(file);
}

static void action_open_cart(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    AstroWindow *self = ASTRO_WINDOW(user_data);
    (void)a; (void)p;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Open Cartridge");
    gtk_file_dialog_open(dialog, GTK_WINDOW(self), NULL, on_cart_chosen, self);
    g_object_unref(dialog);
}

static void action_fujinet_config(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    AstroWindow *self = ASTRO_WINDOW(user_data);
    GtkUriLauncher *launcher;
    (void)a; (void)p;
    if (astrosession_fujinet_external(self->session)) {
        astro_window_toast(self, "FujiNet is external: use its own web UI");
        return;
    }
    /* Always the system browser -- the config pages' OAuth/JS need a real
     * browser, and there is no embedded webview. */
    launcher = gtk_uri_launcher_new(astrosession_fujinet_webui_url(self->session));
    gtk_uri_launcher_launch(launcher, GTK_WINDOW(self), NULL, NULL, NULL);
    g_object_unref(launcher);
}

/* The FujiNet console log: a monospace read-only view refreshed on a 1 s
 * timer from the runtime's own recent-log ring. */
static gboolean log_refresh(gpointer text_view)
{
    if (!GTK_IS_TEXT_VIEW(text_view))
        return G_SOURCE_REMOVE;
    AstroWindow *self = g_object_get_data(G_OBJECT(text_view), "astro-window");
    char buf[16384];
    astrosession_fujinet_copy_log(self->session, buf, sizeof buf);
    GtkTextBuffer *tbuf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
    GtkTextIter si, ei;
    gtk_text_buffer_get_bounds(tbuf, &si, &ei);
    char *old = gtk_text_buffer_get_text(tbuf, &si, &ei, FALSE);
    gboolean same = strcmp(old, buf) == 0;
    g_free(old);
    if (!same)   /* unchanged log: leave the buffer (and any selection) be */
        gtk_text_buffer_set_text(tbuf, buf, -1);
    return G_SOURCE_CONTINUE;
}

static gboolean log_on_key(GtkEventControllerKey *c, guint keyval, guint code,
                           GdkModifierType st, gpointer ud)
{
    (void)c; (void)code; (void)st;
    if (keyval != GDK_KEY_F1)
        return FALSE;
    astro_help_for(ud);
    return TRUE;
}

static void action_fujinet_log(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    AstroWindow *self = ASTRO_WINDOW(user_data);
    (void)a; (void)p;
    if (astrosession_fujinet_external(self->session)) {
        astro_window_toast(self, "FujiNet is external: see its own console");
        return;
    }

    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "FujiNet Console Log");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 700, 500);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(self));

    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    g_object_set_data(G_OBJECT(view), "astro-window", self);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_window_set_child(GTK_WINDOW(dialog), scroll);
    {
        GtkWidget *header = adw_header_bar_new();
        GtkEventController *keys = gtk_event_controller_key_new();
        adw_header_bar_pack_end(ADW_HEADER_BAR(header), astro_help_button("fujilog"));
        gtk_window_set_titlebar(GTK_WINDOW(dialog), header);
        astro_help_set_context(GTK_WINDOW(dialog), "fujilog");
        g_signal_connect(keys, "key-pressed", G_CALLBACK(log_on_key), dialog);
        gtk_widget_add_controller(dialog, keys);
    }

    log_refresh(view);
    guint id = g_timeout_add(1000, log_refresh, view);
    /* stop the timer when the window (and its view) go away */
    g_signal_connect_swapped(dialog, "destroy",
                             G_CALLBACK(g_source_remove), GUINT_TO_POINTER(id));
    gtk_window_present(GTK_WINDOW(dialog));
}

static void action_debugger(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    AstroWindow *self = ASTRO_WINDOW(user_data);
    (void)a; (void)p;
    astro_debugger_toggle(GTK_WINDOW(self), self->session);
}

static void action_preferences(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    AstroWindow *self = ASTRO_WINDOW(user_data);
    (void)a; (void)p;
    astro_prefs_show(self, self->session);
}

static void action_about(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    AstroWindow *self = ASTRO_WINDOW(user_data);
    (void)a; (void)p;
    adw_show_about_dialog(
        GTK_WIDGET(self),
        "application-name", "FujiNet Go Astrocade",
        "application-icon", astro_icon_name(),
        "developer-name", "Thomas Cherryhomes",
        "version", ASTRO_VERSION_STRING,
        "license-type", GTK_LICENSE_GPL_3_0,
        "comments", "Self-contained Bally Astrocade with built-in FujiNet",
        "website", "https://fujinet.online/",
        NULL);
}

static const GActionEntry win_actions[] = {
    {.name = "controllers", .activate = action_controllers},
    {.name = "reset-game", .activate = action_reset_game},
    {.name = "reset-config", .activate = action_reset_config},
    {.name = "import-bios", .activate = action_import_bios},
    {.name = "import-bios-folder", .activate = action_import_bios_folder},
    {.name = "open-cart", .activate = action_open_cart},
    {.name = "fujinet-config", .activate = action_fujinet_config},
    {.name = "fujinet-log", .activate = action_fujinet_log},
    {.name = "debugger", .activate = action_debugger},
    {.name = "preferences", .activate = action_preferences},
    {.name = "about", .activate = action_about},
};

static void astro_window_dispose(GObject *object)
{
    AstroWindow *self = ASTRO_WINDOW(object);
    if (self->sysact_timer_id) {
        g_source_remove(self->sysact_timer_id);
        self->sysact_timer_id = 0;
    }
    G_OBJECT_CLASS(astro_window_parent_class)->dispose(object);
}

static void astro_window_class_init(AstroWindowClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = astro_window_dispose;
}

static void astro_window_init(AstroWindow *self)
{
    (void)self;
}

static GMenu *build_menu(void)
{
    GMenu *menu = g_menu_new();
    GMenu *machine = g_menu_new();
    GMenu *view = g_menu_new();
    GMenu *fujinet = g_menu_new();
    GMenu *tail = g_menu_new();

    g_menu_append(machine, "Open Cartridge…", "win.open-cart");
    g_menu_append(machine, "Import BIOS…", "win.import-bios");
    g_menu_append(machine, "Import BIOS from a Folder…", "win.import-bios-folder");
    g_menu_append_section(menu, "Machine", G_MENU_MODEL(machine));

    g_menu_append(view, "Controllers (F9)", "win.controllers");
    g_menu_append(view, "Debugger (F12)", "win.debugger");
    g_menu_append_section(menu, "View", G_MENU_MODEL(view));

    g_menu_append(fujinet, "Reset Game (Backspace)", "win.reset-game");
    g_menu_append(fujinet, "Reset to CONFIG (Ctrl+R)", "win.reset-config");
    g_menu_append(fujinet, "FujiNet Configuration…", "win.fujinet-config");
    g_menu_append(fujinet, "FujiNet Console Log…", "win.fujinet-log");
    g_menu_append_section(menu, "FujiNet", G_MENU_MODEL(fujinet));

    g_menu_append(tail, "Preferences…", "win.preferences");
    g_menu_append(tail, "FujiNet Go Astrocade Help", "app.help");
    g_menu_append(tail, "About FujiNet Go Astrocade", "win.about");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(tail));

    g_object_unref(machine);
    g_object_unref(view);
    g_object_unref(fujinet);
    g_object_unref(tail);
    return menu;
}

/* Every 50 ms: the console actions the gamepad thread posted (it cannot call
 * into the toolkit), and a toast when a pad comes or goes or a Bally BASIC
 * cartridge turns the overlay on. */
static gboolean sysact_drain_tick(gpointer user_data)
{
    AstroWindow *self = ASTRO_WINDOW(user_data);
    unsigned gen;

    drain_sysactions(self);

    gen = astrosession_gamepad_generation(self->session);
    if (gen != self->pad_generation) {
        char text[160];
        self->pad_generation = gen;
        if (astrosession_gamepad_last_event(self->session, text, sizeof text) > 0)
            astro_window_toast(self, text);
    }
    gen = astrosession_overlay_auto_generation(self->session);
    if (gen != self->overlay_generation) {
        self->overlay_generation = gen;
        astro_window_toast(self, "Bally BASIC overlay on");
    }
    return G_SOURCE_CONTINUE;
}

GtkWidget *astro_window_new(AdwApplication *app, astrosession *session)
{
    AstroWindow *self = g_object_new(ASTRO_TYPE_WINDOW,
                                     "application", app,
                                     "title", "FujiNet Go Astrocade",
                                     "default-width", 800,
                                     "default-height", 650,
                                     NULL);
    AdwToolbarView *view;
    AdwHeaderBar *header;
    GtkMenuButton *menu_button;
    GMenu *menu;
    GtkEventController *keys;

    self->session = session;

    g_action_map_add_action_entries(G_ACTION_MAP(self), win_actions,
                                    G_N_ELEMENTS(win_actions), self);
    if (!g_action_map_lookup_action(G_ACTION_MAP(app), "help"))
        g_action_map_add_action_entries(G_ACTION_MAP(app), app_actions,
                                        G_N_ELEMENTS(app_actions), app);
    {
        static const char *const help_accels[] = { "<Control><Shift>h", NULL };
        gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.help", help_accels);
    }

    header = ADW_HEADER_BAR(adw_header_bar_new());
    menu = build_menu();
    menu_button = GTK_MENU_BUTTON(gtk_menu_button_new());
    gtk_menu_button_set_icon_name(menu_button, "open-menu-symbolic");
    gtk_menu_button_set_menu_model(menu_button, G_MENU_MODEL(menu));
    g_object_unref(menu);
    adw_header_bar_pack_end(header, GTK_WIDGET(menu_button));

    self->display = ASTRO_DISPLAY(astro_display_new(session));
    self->stack = GTK_STACK(gtk_stack_new());
    gtk_stack_add_named(self->stack, GTK_WIDGET(self->display), "display");
    gtk_stack_add_named(self->stack, nobios_page(self), "nobios");
    self->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(self->toasts, GTK_WIDGET(self->stack));

    view = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    adw_toolbar_view_add_top_bar(view, GTK_WIDGET(header));
    adw_toolbar_view_set_content(view, GTK_WIDGET(self->toasts));
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(self),
                                       GTK_WIDGET(view));

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), self);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), self);
    gtk_widget_add_controller(GTK_WIDGET(self), keys);

    {
        GtkEventController *focus = gtk_event_controller_focus_new();
        g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), self);
        gtk_widget_add_controller(GTK_WIDGET(self), focus);
    }

    /* The current pad set and overlay are the baseline: no toast for pads
     * already plugged in at launch. */
    self->pad_generation = astrosession_gamepad_generation(session);
    self->overlay_generation = astrosession_overlay_auto_generation(session);
    self->sysact_timer_id = g_timeout_add(50, sysact_drain_tick, self);

    gtk_widget_grab_focus(GTK_WIDGET(self->display));

    /* ASTRO_OPEN_KEYPAD is the older name of ASTRO_OPEN_CONTROLLERS. */
    if (getenv("ASTRO_OPEN_CONTROLLERS") || getenv("ASTRO_OPEN_KEYPAD"))
        astro_controllers_toggle(GTK_WINDOW(self), session);
    if (getenv("ASTRO_OPEN_DEBUGGER"))
        astro_debugger_show(GTK_WINDOW(self), session);
    if (getenv("ASTRO_OPEN_PREFS"))
        astro_prefs_show(self, session);

    astro_window_show_page(self);
    return GTK_WIDGET(self);
}
