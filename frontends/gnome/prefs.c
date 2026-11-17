/*
 * AstroPrefs -- the Preferences dialog: a programmatic AdwPreferencesDialog
 * (no .ui file). Two machine options, both "restart to apply": the BIOS
 * variant and the RAM expansion. Changing either persists to the shared
 * settings store immediately and the dialog restarts the session once on
 * close if anything changed. A FujiNet group chooses the built-in FujiNet or
 * an external one at host:port. The ROMs page lists each BIOS as imported or
 * not, with the same import the Machine menu has. The Input page holds the
 * gamepad options: enable (restart to apply), which player each pad plays
 * (live), Reset Controls and Open Controllers.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "prefs.h"
#include "bindings.h"
#include "controllers/controllers_window.h"
#include "help/help_window.h"

#include <stdlib.h>
#include <string.h>

#define MAX_ROM_ROWS 8
#define MAX_PAD_ROWS 8

typedef struct {
    AstroWindow *window;
    astrosession *session;
    gboolean dirty;
    gboolean syncing;            /* a refresh, not the user, is moving a combo */
    AdwComboRow *bios_row;
    GtkWidget *addr_row;
    GtkWidget *rom_rows[MAX_ROM_ROWS];
    AdwDialog *dialog;
    GtkWidget *pad_group;               /* the Input page's gamepad rows */
    GtkWidget *pad_rows[MAX_PAD_ROWS + 1];
    int pad_row_count;
    unsigned pad_generation;            /* the pad set the rows were built for */
    guint pad_timer;
} PrefsState;

typedef struct {
    PrefsState *state;
    const char *key;
    int def;
} RowBinding;

static void binding_free(gpointer p, GClosure *closure)
{
    (void)closure;
    g_free(p);
}

/* Materialise a NULL-terminated name(idx) table into a GtkStringList's
 * backing array, so a new BIOS/expansion option in astrosession.h's own table
 * is the only place it has to be added. */
#define OPTION_TABLE(fn, storage)                                    \
    static const char *const *storage##_table(void)                  \
    {                                                                \
        static const char *names[10];                                \
        int i;                                                       \
        for (i = 0; i < (int)G_N_ELEMENTS(names) - 1; i++) {         \
            names[i] = fn(i);                                        \
            if (!names[i]) break;                                    \
        }                                                            \
        names[i] = NULL;                                             \
        return names;                                                \
    }

OPTION_TABLE(astrosession_exp_name, exp)

/* The BIOS choices, each one not imported yet saying so. */
static GtkStringList *bios_model(astrosession *session)
{
    GtkStringList *list = gtk_string_list_new(NULL);
    for (int i = 0; astrosession_bios_name(i); i++) {
        char label[160];
        g_snprintf(label, sizeof label, "%s%s", astrosession_bios_name(i),
                   astrosession_rom_item_available(session, i) ? "" : " (not imported)");
        gtk_string_list_append(list, label);
    }
    return list;
}

static gpointer binding_new_(PrefsState *state, const char *key, int def)
{
    RowBinding *b = g_new0(RowBinding, 1);
    b->state = state;
    b->key = key;
    b->def = def;
    return b;
}

static void combo_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    RowBinding *b = user_data;
    int sel = (int)adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    (void)pspec;

    if (b->state->syncing)
        return;
    if (astrosession_get_int(b->state->session, b->key, b->def) == sel)
        return;
    astrosession_set_int(b->state->session, b->key, sel);
    b->state->dirty = TRUE;
}

static AdwComboRow *add_combo(AdwPreferencesGroup *group, const char *title,
                              const char *subtitle, GtkStringList *model,
                              PrefsState *state, const char *key, int def)
{
    AdwComboRow *row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    adw_combo_row_set_model(row, G_LIST_MODEL(model));
    g_object_unref(model);
    adw_combo_row_set_selected(row, astrosession_get_int(state->session, key, def));
    g_signal_connect_data(row, "notify::selected", G_CALLBACK(combo_changed),
                          binding_new_(state, key, def), binding_free, 0);
    adw_preferences_group_add(group, GTK_WIDGET(row));
    return row;
}

#define EXTERNAL_PLACEHOLDER "127.0.0.1:11500"

/* The address field: valid text (or none, meaning 127.0.0.1:11500) is saved
 * and marks the machine dirty; anything else is flagged and not saved. */
static void addr_changed(GtkEditable *entry, gpointer user_data)
{
    PrefsState *state = user_data;
    const char *text = gtk_editable_get_text(entry);
    char host[64];
    int port;

    if (astrosession_parse_hostport(text, host, sizeof host, &port) != 0) {
        gtk_widget_add_css_class(GTK_WIDGET(entry), "error");
        return;
    }
    gtk_widget_remove_css_class(GTK_WIDGET(entry), "error");
    if (g_strcmp0(astrosession_get_str(state->session, "fujinet_external_addr", ""), text) == 0)
        return;
    astrosession_set_str(state->session, "fujinet_external_addr", text);
    state->dirty = TRUE;
}

static void external_toggled(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    PrefsState *state = user_data;
    int active = adw_switch_row_get_active(ADW_SWITCH_ROW(row));
    (void)pspec;

    gtk_widget_set_sensitive(state->addr_row, active);
    if (astrosession_get_int(state->session, "fujinet_external", 0) == active)
        return;
    astrosession_set_int(state->session, "fujinet_external", active);
    state->dirty = TRUE;
}

/* After an import: the ROMs rows' states and the BIOS choices' labels. */
static void refresh_rows(PrefsState *st)
{
    st->syncing = TRUE;
    for (int r = 0; r < astrosession_rom_item_count() && r < MAX_ROM_ROWS; r++)
        if (st->rom_rows[r])
            adw_action_row_set_subtitle(ADW_ACTION_ROW(st->rom_rows[r]),
                                        astrosession_rom_item_available(st->session, r)
                                            ? "Imported" : "Not imported");
    if (st->bios_row) {
        GtkStringList *model = bios_model(st->session);
        adw_combo_row_set_model(st->bios_row, G_LIST_MODEL(model));
        g_object_unref(model);
        adw_combo_row_set_selected(st->bios_row,
            (guint)astrosession_get_int(st->session, "bios", ASTROSESSION_BIOS_ASTRO));
    }
    st->syncing = FALSE;
}

/* ---- the ROMs page ---- */

static const char *const ROM_EXTS[] = { "bin", "rom", "zip", NULL };

/* Each chosen file (or the folder) through the import, then one message
 * saying what came of it all, as the Machine menu does. */
static void on_rom_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    PrefsState *st = ud;
    gboolean folder = GPOINTER_TO_INT(g_object_get_data(src, "astro-folder"));
    g_autoptr(GFile) dir = NULL;
    g_autoptr(GListModel) files = NULL;
    GString *log = g_string_new(NULL);

    if (folder)
        dir = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL);
    else
        files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(src), res, NULL);
    for (guint i = 0; ; i++) {
        g_autoptr(GFile) f = NULL;
        g_autofree char *path = NULL;
        char msg[1024];
        if (folder) {
            if (i > 0 || !dir)
                break;
            f = g_object_ref(dir);
        } else {
            if (!files || i >= g_list_model_get_n_items(files))
                break;
            f = g_list_model_get_item(files, i);
        }
        path = g_file_get_path(f);
        if (!path)
            continue;
        astrosession_rom_import(st->session, path, msg, sizeof msg);
        g_string_append_printf(log, "%s%s", log->len ? "\n" : "", msg);
    }
    if (log->len) {
        astro_window_toast(st->window, log->str);
        astro_window_show_page(st->window);
        refresh_rows(st);
    }
    g_string_free(log, TRUE);
}

static void on_rom_import(GtkButton *b, gpointer ud)
{
    PrefsState *st = ud;
    gboolean folder = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "astro-folder"));
    GtkFileDialog *dlg = astro_file_dialog(folder ? "Import BIOS from a Folder" : "Import BIOS",
                                           "BIOS images and ROM sets", ROM_EXTS);
    g_object_set_data(G_OBJECT(dlg), "astro-folder", GINT_TO_POINTER(folder));
    if (folder)
        gtk_file_dialog_select_folder(dlg, GTK_WINDOW(st->window), NULL, on_rom_chosen, st);
    else
        gtk_file_dialog_open_multiple(dlg, GTK_WINDOW(st->window), NULL, on_rom_chosen, st);
    g_object_unref(dlg);
}

static AdwPreferencesPage *build_roms_page(PrefsState *st)
{
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    AdwPreferencesGroup *group = NULL;
    const char *last = NULL;
    char desc[1200];

    adw_preferences_page_set_title(page, "ROMs");
    adw_preferences_page_set_name(page, "ROMs");
    adw_preferences_page_set_icon_name(page, "media-flash-symbolic");
    for (int r = 0; r < astrosession_rom_item_count() && r < MAX_ROM_ROWS; r++) {
        const astro_rom_item *item = astrosession_rom_item(r);
        GtkWidget *row;
        if (!last || strcmp(last, item->group) != 0) {
            group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
            adw_preferences_group_set_title(group, item->group);
            if (!last) {
                GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
                GtkWidget *files = gtk_button_new_with_label("Import\xe2\x80\xa6");
                GtkWidget *dir = gtk_button_new_with_label("Import a Folder\xe2\x80\xa6");
                g_snprintf(desc, sizeof desc,
                    "The console's BIOS ROM is not part of the app. Import it from a file, "
                    "a zip or a folder (MAME's astrocde set works); it is recognised by its "
                    "checksum. Kept in %s.",
                    astrosession_roms_path(st->session));
                adw_preferences_group_set_description(group, desc);
                g_object_set_data(G_OBJECT(dir), "astro-folder", GINT_TO_POINTER(1));
                g_signal_connect(files, "clicked", G_CALLBACK(on_rom_import), st);
                g_signal_connect(dir, "clicked", G_CALLBACK(on_rom_import), st);
                gtk_widget_set_valign(files, GTK_ALIGN_CENTER);
                gtk_widget_set_valign(dir, GTK_ALIGN_CENTER);
                gtk_box_append(GTK_BOX(box), files);
                gtk_box_append(GTK_BOX(box), dir);
                gtk_box_append(GTK_BOX(box), astro_help_button("prefs/ROMs"));
                adw_preferences_group_set_header_suffix(group, box);
            }
            adw_preferences_page_add(page, group);
            last = item->group;
        }
        row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), item->name);
        adw_preferences_group_add(group, row);
        st->rom_rows[r] = row;
    }
    return page;
}

/* ---- the Input page ---- */

/* A row with a title and a line under it, nothing to set. */
static GtkWidget *info_row(const char *title, const char *subtitle)
{
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    return row;
}

/* Enable gamepads: a machine option, applied by restarting on close. */
static void gamepad_switch_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    PrefsState *state = user_data;
    const int active = adw_switch_row_get_active(ADW_SWITCH_ROW(row));
    (void)pspec;
    if (astrosession_get_int(state->session, "enable_gamepad", 1) == active)
        return;
    astrosession_set_int(state->session, "enable_gamepad", active);
    state->dirty = TRUE;
}

/* A pad row's choice: 0 is Automatic (connection order), 1-4 that player. It
 * is live, not a machine option, so it does not restart the session. */
static void pad_assign_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    PrefsState *state = user_data;
    const int idx = GPOINTER_TO_INT(g_object_get_data(row, "pad-index"));
    const int sel = (int)adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    (void)pspec;
    astrosession_gamepad_assign(state->session, idx, sel - 1);
}

/* "Playing player N" / "Playing nobody" for a pad. */
static void pad_subtitle(PrefsState *state, int idx, char *dst, int dstsz)
{
    const int eff = astrosession_gamepad_effective_port(state->session, idx);
    if (eff >= 0)
        g_snprintf(dst, dstsz, "Playing player %d", eff + 1);
    else
        g_snprintf(dst, dstsz, "Playing nobody");
}

static void rebuild_pad_rows(PrefsState *state)
{
    static const char *const choices[] = { "Automatic", "Player 1", "Player 2", "Player 3",
                                           "Player 4", NULL };
    const int count = astrosession_gamepad_count(state->session);
    char name[128], sub[64];
    int idx;

    for (idx = 0; idx < state->pad_row_count; idx++)
        adw_preferences_group_remove(ADW_PREFERENCES_GROUP(state->pad_group),
                                     state->pad_rows[idx]);
    state->pad_row_count = 0;

    if (count == 0) {
        GtkWidget *row = info_row("No gamepads connected",
                                  "Plug one in: it is picked up as it appears");
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(state->pad_group), row);
        state->pad_rows[state->pad_row_count++] = row;
        return;
    }
    for (idx = 0; idx < count && idx < MAX_PAD_ROWS; idx++) {
        GtkWidget *row = adw_combo_row_new();
        GtkStringList *model = gtk_string_list_new(choices);
        const int assign = astrosession_gamepad_assignment(state->session, idx);

        astrosession_gamepad_name(state->session, idx, name, sizeof name);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name);
        pad_subtitle(state, idx, sub, sizeof sub);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub);
        adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
        adw_combo_row_set_selected(ADW_COMBO_ROW(row),
                                   assign < 0 || assign >= ASTROSESSION_PLAYERS
                                       ? 0 : (guint)assign + 1);
        g_object_set_data(G_OBJECT(row), "pad-index", GINT_TO_POINTER(idx));
        g_signal_connect(row, "notify::selected", G_CALLBACK(pad_assign_changed), state);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(state->pad_group), row);
        state->pad_rows[state->pad_row_count++] = row;
        g_object_unref(model);
    }
}

/* Once a second: the rows again when a pad came or went, else just the
 * "Playing ..." lines (assigning one pad moves an automatic one). */
static gboolean pad_tick(gpointer user_data)
{
    PrefsState *state = user_data;
    const unsigned gen = astrosession_gamepad_generation(state->session);

    if (gen != state->pad_generation) {
        state->pad_generation = gen;
        rebuild_pad_rows(state);
    } else if (astrosession_gamepad_count(state->session) > 0) {
        int idx;
        for (idx = 0; idx < state->pad_row_count; idx++) {
            AdwActionRow *row = ADW_ACTION_ROW(state->pad_rows[idx]);
            char sub[64];
            pad_subtitle(state, idx, sub, sizeof sub);
            if (g_strcmp0(adw_action_row_get_subtitle(row), sub) != 0)
                adw_action_row_set_subtitle(row, sub);
        }
    }
    return G_SOURCE_CONTINUE;
}

static void on_open_controllers(GtkButton *button, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)button;
    astro_controllers_show(GTK_WINDOW(state->window), state->session);
}

static void on_reset_controls(GtkButton *button, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)button;
    bindings_reset_defaults(state->session);
    adw_preferences_dialog_add_toast(ADW_PREFERENCES_DIALOG(state->dialog),
                                     adw_toast_new("Controls back to their defaults"));
}

static AdwPreferencesPage *build_input_page(PrefsState *state)
{
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    GtkWidget *row, *open, *reset;

    adw_preferences_page_set_title(page, "Input");
    adw_preferences_page_set_name(page, "Input");
    adw_preferences_page_set_icon_name(page, "input-gaming-symbolic");

    adw_preferences_group_set_title(group, "Controllers");
    adw_preferences_group_set_header_suffix(group, astro_help_button("prefs/Input"));
    adw_preferences_group_set_description(
        group, "Applied by restarting the session when this dialog closes");
    row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Enable gamepads");
    adw_switch_row_set_active(ADW_SWITCH_ROW(row),
                              astrosession_get_int(state->session, "enable_gamepad", 1) != 0);
    g_signal_connect(row, "notify::active", G_CALLBACK(gamepad_switch_changed), state);
    adw_preferences_group_add(group, row);
    adw_preferences_page_add(page, group);

    group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, "Gamepads");
    adw_preferences_group_set_description(
        group, "Assigned to the four hand controllers in connection order unless chosen here.");
    state->pad_group = GTK_WIDGET(group);
    rebuild_pad_rows(state);
    state->pad_generation = astrosession_gamepad_generation(state->session);
    adw_preferences_page_add(page, group);

    group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, "Controls");
    adw_preferences_group_set_description(
        group, "Keys and gamepad inputs are re-mapped in the Controllers window (F9): "
               "press Map, choose a control, then press the key or gamepad input to give it.");
    row = info_row("Controllers window", "Map any of the controls, and turn on the Bally BASIC overlay");
    open = gtk_button_new_with_label("Open Controllers");
    gtk_widget_set_valign(open, GTK_ALIGN_CENTER);
    g_signal_connect(open, "clicked", G_CALLBACK(on_open_controllers), state);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), open);
    adw_preferences_group_add(group, row);
    row = info_row("Reset Controls", "Put every key and gamepad input back to its default");
    reset = gtk_button_new_with_label("Reset Controls");
    gtk_widget_set_valign(reset, GTK_ALIGN_CENTER);
    g_signal_connect(reset, "clicked", G_CALLBACK(on_reset_controls), state);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), reset);
    adw_preferences_group_add(group, row);
    adw_preferences_page_add(page, group);
    return page;
}

/* A changed setting restarts the machine; a BIOS imported here starts it. */
static void on_closed(AdwDialog *dialog, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)dialog;
    if (state->pad_timer)
        g_source_remove(state->pad_timer);
    state->pad_timer = 0;
    if (state->dirty)
        astro_window_restart_session(state->window);
    else
        astro_window_start(state->window);
    g_free(state);
}

void astro_prefs_show(AstroWindow *window, astrosession *session)
{
    PrefsState *state = g_new0(PrefsState, 1);
    state->window = window;
    state->session = session;

    AdwPreferencesDialog *dialog =
        ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
    adw_dialog_set_title(ADW_DIALOG(dialog), "Preferences");

    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(page, "Machine");
    adw_preferences_page_set_name(page, "Machine");
    adw_preferences_page_set_icon_name(page, "applications-games-symbolic");

    AdwPreferencesGroup *group =
        ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, "Machine");
    adw_preferences_group_set_header_suffix(group, astro_help_button("prefs/Machine"));
    adw_preferences_group_set_description(
        group, "Changes take effect when the machine restarts.");

    state->bios_row = add_combo(group, "System ROM", "Which on-board BIOS to boot",
                                bios_model(session), state, "bios", ASTROSESSION_BIOS_ASTRO);
    add_combo(group, "RAM Expansion", "Extra RAM cartridge, if any",
              gtk_string_list_new(exp_table()), state, "exp", ASTROSESSION_EXP_NONE);

    adw_preferences_page_add(page, group);

    AdwPreferencesGroup *fujinet =
        ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(fujinet, "FujiNet");
    adw_preferences_group_set_description(
        fujinet, "Skip the built-in FujiNet and connect to another instance "
                 "instead: its [BOIP] listener's host and port.");
    GtkWidget *external = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(external), "Use external FujiNet");
    adw_switch_row_set_active(ADW_SWITCH_ROW(external),
                              astrosession_get_int(session, "fujinet_external", 0) != 0);
    state->addr_row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(state->addr_row),
                                  "Address:port, e.g. " EXTERNAL_PLACEHOLDER);
    gtk_editable_set_text(GTK_EDITABLE(state->addr_row),
                          astrosession_get_str(session, "fujinet_external_addr", ""));
    g_signal_connect(state->addr_row, "changed", G_CALLBACK(addr_changed), state);
    gtk_widget_set_sensitive(state->addr_row,
                             adw_switch_row_get_active(ADW_SWITCH_ROW(external)));
    g_signal_connect(external, "notify::active", G_CALLBACK(external_toggled), state);
    adw_preferences_group_add(fujinet, external);
    adw_preferences_group_add(fujinet, state->addr_row);
    adw_preferences_page_add(page, fujinet);

    adw_preferences_dialog_add(dialog, page);
    state->dialog = ADW_DIALOG(dialog);
    adw_preferences_dialog_add(dialog, build_input_page(state));
    adw_preferences_dialog_add(dialog, build_roms_page(state));
    refresh_rows(state);
    /* ASTRO_OPEN_PREFS=ROMs: open on that page (a developer affordance) */
    {
        const char *open = getenv("ASTRO_OPEN_PREFS");
        if (open && (strcmp(open, "ROMs") == 0 || strcmp(open, "Input") == 0))
            adw_preferences_dialog_set_visible_page_name(dialog, open);
    }

    g_signal_connect(dialog, "closed", G_CALLBACK(on_closed), state);
    state->pad_timer = g_timeout_add(1000, pad_tick, state);
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window));
}
