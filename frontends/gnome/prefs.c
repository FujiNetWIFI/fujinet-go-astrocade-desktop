/*
 * AstroPrefs -- the Preferences dialog: a programmatic AdwPreferencesDialog
 * (no .ui file). Two machine options, both "restart to apply": the BIOS
 * variant and the RAM expansion. Changing either persists to the shared
 * settings store immediately and the dialog restarts the session once on
 * close if anything changed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "prefs.h"

typedef struct {
    AstroWindow *window;
    astrosession *session;
    gboolean dirty;
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

OPTION_TABLE(astrosession_bios_name, bios)
OPTION_TABLE(astrosession_exp_name, exp)

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

    if (astrosession_get_int(b->state->session, b->key, b->def) == sel)
        return;
    astrosession_set_int(b->state->session, b->key, sel);
    b->state->dirty = TRUE;
}

static AdwComboRow *add_combo(AdwPreferencesGroup *group, const char *title,
                              const char *subtitle, const char *const *items,
                              PrefsState *state, const char *key, int def)
{
    GtkStringList *model = gtk_string_list_new(items);
    AdwComboRow *row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    adw_combo_row_set_model(row, G_LIST_MODEL(model));
    adw_combo_row_set_selected(row, astrosession_get_int(state->session, key, def));
    g_signal_connect_data(row, "notify::selected", G_CALLBACK(combo_changed),
                          binding_new_(state, key, def), binding_free, 0);
    adw_preferences_group_add(group, GTK_WIDGET(row));
    return row;
}

static void on_closed(AdwDialog *dialog, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)dialog;
    if (state->dirty)
        astro_window_restart_session(state->window);
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

    add_combo(group, "System ROM", "Which on-board BIOS to boot",
              bios_table(), state, "bios", ASTROSESSION_BIOS_ASTRO);
    add_combo(group, "RAM Expansion", "Extra RAM cartridge, if any",
              exp_table(), state, "exp", ASTROSESSION_EXP_NONE);

    adw_preferences_page_add(page, group);
    adw_preferences_dialog_add(dialog, page);

    g_signal_connect(dialog, "closed", G_CALLBACK(on_closed), state);
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window));
}
