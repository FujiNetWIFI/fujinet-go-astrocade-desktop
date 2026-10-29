/*
 * AstroPrefs -- the Preferences dialog: a programmatic AdwPreferencesDialog
 * (no .ui file). Two machine options, both "restart to apply": the BIOS
 * variant and the RAM expansion. Changing either persists to the shared
 * settings store immediately and the dialog restarts the session once on
 * close if anything changed. A FujiNet group chooses the built-in FujiNet or
 * an external one at host:port. The ROMs page lists each BIOS as imported or
 * not, with the same import the Machine menu has.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "prefs.h"

#include <stdlib.h>
#include <string.h>

#define MAX_ROM_ROWS 8

typedef struct {
    AstroWindow *window;
    astrosession *session;
    gboolean dirty;
    gboolean syncing;            /* a refresh, not the user, is moving a combo */
    AdwComboRow *bios_row;
    GtkWidget *addr_row;
    GtkWidget *rom_rows[MAX_ROM_ROWS];
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

static void on_rom_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    PrefsState *st = ud;
    gboolean folder = GPOINTER_TO_INT(g_object_get_data(src, "astro-folder"));
    g_autoptr(GFile) f = folder
        ? gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL)
        : gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = f ? g_file_get_path(f) : NULL;
    char msg[1024];
    if (!path)
        return;
    astrosession_rom_import(st->session, path, msg, sizeof msg);
    astro_window_toast(st->window, msg);
    astro_window_show_page(st->window);
    refresh_rows(st);
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
        gtk_file_dialog_open(dlg, GTK_WINDOW(st->window), NULL, on_rom_chosen, st);
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

/* A changed setting restarts the machine; a BIOS imported here starts it. */
static void on_closed(AdwDialog *dialog, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)dialog;
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
    adw_preferences_page_set_icon_name(page, "applications-games-symbolic");

    AdwPreferencesGroup *group =
        ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, "Machine");
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
    adw_preferences_dialog_add(dialog, build_roms_page(state));
    refresh_rows(state);
    /* ASTRO_OPEN_PREFS=ROMs: open on that page (a developer affordance) */
    {
        const char *open = getenv("ASTRO_OPEN_PREFS");
        if (open && strcmp(open, "ROMs") == 0)
            adw_preferences_dialog_set_visible_page_name(dialog, "ROMs");
    }

    g_signal_connect(dialog, "closed", G_CALLBACK(on_closed), state);
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window));
}
