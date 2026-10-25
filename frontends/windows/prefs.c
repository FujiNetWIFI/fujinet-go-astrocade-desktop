/*
 * The Win32 Preferences window, the GNOME and KDE dialogs' twin: a fixed-size
 * window with two tabs.
 *
 *  Machine -- the System ROM (BIOS variant; one not imported says so) and the
 *      RAM expansion. They apply to the settings store at once and the
 *      machine restarts once when the window closes.
 *  ROMs -- what fujinet-go-msdos-desktop's ROMs page is: a note on where the
 *      BIOS comes from and where it is kept, Import... and Import a
 *      Folder..., and a list of each BIOS, imported or not.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "prefs.h"

#include <commctrl.h>
#include <stdio.h>
#include <string.h>

#include "import.h"
#include "resource.h"
#include "win_common.h"

#define PREFS_CLASS "AstroPrefs"
#define PREFS_STYLE (WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN)

enum {
    ID_TABS = 100, ID_BIOS, ID_EXP, ID_ROM_FILES, ID_ROM_FOLDER, ID_ROM_LIST, ID_CLOSE,
};
enum { PAGE_MACHINE, PAGE_ROMS };

/* the layout, in 96-dpi units */
#define CW 600              /* client width */
#define CH 380              /* client height */
#define M 12                /* margin */

static HWND g_win, g_tabs, g_close;
static HWND g_bios_label, g_bios, g_exp_label, g_exp, g_machine_note;
static HWND g_rom_note, g_rom_path, g_rom_files, g_rom_folder, g_rom_list;
static HFONT g_font;
static astrosession *g_session;
static astro_prefs_hooks g_hooks;
static int g_dirty, g_page, g_top;

static int S(int v) { return astro_scale(v, astro_window_dpi(g_win)); }

/* The BIOS choices, each one not imported yet saying so. */
static void fill_bios(void)
{
    SendMessageW(g_bios, CB_RESETCONTENT, 0, 0);
    for (int i = 0; astrosession_bios_name(i); i++) {
        char label[200];
        snprintf(label, sizeof label, "%s%s", astrosession_bios_name(i),
                 astrosession_rom_item_available(g_session, i) ? "" : " (not imported)");
        wchar_t *w = astro_wide_dup(label);
        if (w) { SendMessageW(g_bios, CB_ADDSTRING, 0, (LPARAM)w); free(w); }
    }
    SendMessageW(g_bios, CB_SETCURSEL,
                 (WPARAM)astrosession_get_int(g_session, "bios", ASTROSESSION_BIOS_ASTRO), 0);
}

static void refresh_roms(void)
{
    SendMessageW(g_rom_list, LVM_DELETEALLITEMS, 0, 0);
    for (int i = 0; i < astrosession_rom_item_count(); i++) {
        const astro_rom_item *item = astrosession_rom_item(i);
        wchar_t *name = astro_wide_dup(item->name), *group = astro_wide_dup(item->group);
        LVITEMW it;
        memset(&it, 0, sizeof it);
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = name ? name : L"";
        SendMessageW(g_rom_list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        it.iSubItem = 1;
        it.pszText = group ? group : L"";
        SendMessageW(g_rom_list, LVM_SETITEMTEXTW, (WPARAM)i, (LPARAM)&it);
        it.iSubItem = 2;
        it.pszText = astrosession_rom_item_available(g_session, i) ? L"Imported" : L"Not imported";
        SendMessageW(g_rom_list, LVM_SETITEMTEXTW, (WPARAM)i, (LPARAM)&it);
        free(name);
        free(group);
    }
    fill_bios();
}

static void show_page(int page)
{
    HWND machine[] = { g_bios_label, g_bios, g_exp_label, g_exp, g_machine_note };
    HWND roms[] = { g_rom_note, g_rom_path, g_rom_files, g_rom_folder, g_rom_list };
    g_page = page;
    for (size_t i = 0; i < sizeof machine / sizeof machine[0]; i++)
        ShowWindow(machine[i], page == PAGE_MACHINE ? SW_SHOW : SW_HIDE);
    for (size_t i = 0; i < sizeof roms / sizeof roms[0]; i++)
        ShowWindow(roms[i], page == PAGE_ROMS ? SW_SHOW : SW_HIDE);
}

static void layout(void)
{
    const int cw = S(CW), ch = S(CH), m = S(M);
    RECT page;
    int y;

    /* the tab control is the strip alone: the pages sit on the window's
     * own background below it, above the Close button */
    g_top = m + astro_place_tab_strip(g_tabs, m, m, cw - 2 * m) + S(12);
    SetRect(&page, m - S(8), g_top, cw - m + S(8), ch - m - S(40));
    y = g_top;

    {
        const int px = page.left + S(8), pw = page.right - S(8) - px;

        /* Machine */
        MoveWindow(g_bios_label, px, y + S(4), S(120), S(20), TRUE);
        MoveWindow(g_bios, px + S(124), y, pw - S(124), S(300), TRUE);
        MoveWindow(g_exp_label, px, y + S(40), S(120), S(20), TRUE);
        MoveWindow(g_exp, px + S(124), y + S(36), pw - S(124), S(300), TRUE);
        MoveWindow(g_machine_note, px, y + S(80), pw, S(20), TRUE);

        /* ROMs */
        MoveWindow(g_rom_note, px, y, pw, S(48), TRUE);
        MoveWindow(g_rom_path, px, y + S(50), pw, S(20), TRUE);
        MoveWindow(g_rom_files, px, y + S(76), S(140), S(28), TRUE);
        MoveWindow(g_rom_folder, px + S(148), y + S(76), S(160), S(28), TRUE);
        MoveWindow(g_rom_list, px, y + S(112), pw, page.bottom - S(8) - y - S(112), TRUE);
        LVCOLUMNW col;
        memset(&col, 0, sizeof col);
        col.mask = LVCF_WIDTH;
        col.cx = pw - S(90) - S(110) - GetSystemMetrics(SM_CXVSCROLL) - S(4);
        SendMessageW(g_rom_list, LVM_SETCOLUMNW, 0, (LPARAM)&col);
        col.cx = S(90);
        SendMessageW(g_rom_list, LVM_SETCOLUMNW, 1, (LPARAM)&col);
        col.cx = S(110);
        SendMessageW(g_rom_list, LVM_SETCOLUMNW, 2, (LPARAM)&col);
    }

    MoveWindow(g_close, cw - m - S(96), ch - m - S(28), S(96), S(28), TRUE);
}

static void make_font(void)
{
    if (g_font) DeleteObject(g_font);
    g_font = astro_make_font(astro_window_dpi(g_win), 0, FW_NORMAL, NULL);
    astro_set_font_all(g_win, g_font);
}

static void fit_window(const RECT *at)
{
    SIZE outer = astro_outer_size(g_win, S(CW), S(CH), FALSE);
    if (at)
        SetWindowPos(g_win, NULL, at->left, at->top, outer.cx, outer.cy, SWP_NOZORDER | SWP_NOACTIVATE);
    else
        SetWindowPos(g_win, NULL, 0, 0, outer.cx, outer.cy, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
}

static void import_roms(int folder)
{
    if (astro_import_bios(g_win, g_session, folder, 1) < 0)
        return;
    refresh_roms();
    if (g_hooks.imported)
        g_hooks.imported();
}

static LRESULT CALLBACK prefs_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_BIOS:
        case ID_EXP:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                const int bios = LOWORD(wp) == ID_BIOS;
                const char *key = bios ? "bios" : "exp";
                const int def = bios ? ASTROSESSION_BIOS_ASTRO : ASTROSESSION_EXP_NONE;
                const int sel = (int)SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel != astrosession_get_int(g_session, key, def)) {
                    astrosession_set_int(g_session, key, sel);
                    g_dirty = 1;
                }
            }
            return 0;
        case ID_ROM_FILES:  import_roms(0); return 0;
        case ID_ROM_FOLDER: import_roms(1); return 0;
        case ID_CLOSE:
        case IDCANCEL:
            SendMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        default:
            break;
        }
        break;
    case WM_NOTIFY: {
        const NMHDR *nh = (const NMHDR *)lp;
        if (nh->hwndFrom == g_tabs && nh->code == (UINT)TCN_SELCHANGE)
            show_page((int)SendMessageW(g_tabs, TCM_GETCURSEL, 0, 0));
        return 0;
    }
    case WM_SIZE:
        if (g_tabs) layout();
        return 0;
    case WM_DPICHANGED:
        make_font();
        fit_window((const RECT *)lp);
        layout();
        return 0;
    case WM_CLOSE: {
        const int dirty = g_dirty;
        astrosession_settings_flush(g_session);
        DestroyWindow(hwnd);
        if (g_hooks.closed)
            g_hooks.closed(dirty);
        return 0;
    }
    case WM_DESTROY:
        g_win = NULL;
        if (g_font) DeleteObject(g_font);
        g_font = NULL;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void astro_prefs_show(HWND parent, astrosession *session, const astro_prefs_hooks *hooks,
                      int roms_page)
{
    static int registered;
    HINSTANCE inst = GetModuleHandleA(NULL);
    char note[1400];

    if (g_win) {
        SetForegroundWindow(g_win);
        return;
    }
    g_tabs = NULL;
    g_session = session;
    g_hooks = *hooks;
    g_dirty = 0;
    if (!registered) {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = prefs_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"" PREFS_CLASS;
        wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(IDI_APPICON));
        registered = RegisterClassExW(&wc) != 0;
    }
    g_win = CreateWindowExW(WS_EX_DLGMODALFRAME, L"" PREFS_CLASS, L"Preferences", PREFS_STYLE,
                            CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, parent, NULL, inst, NULL);
    if (!g_win) return;

    g_tabs = CreateWindowW(WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                           0, 0, 1, 1, g_win, (HMENU)(INT_PTR)ID_TABS, inst, NULL);
    {
        TCITEMW item;
        memset(&item, 0, sizeof item);
        item.mask = TCIF_TEXT;
        item.pszText = L"Machine";
        SendMessageW(g_tabs, TCM_INSERTITEMW, 0, (LPARAM)&item);
        item.pszText = L"ROMs";
        SendMessageW(g_tabs, TCM_INSERTITEMW, 1, (LPARAM)&item);
    }
    g_bios_label = CreateWindowW(L"STATIC", L"System ROM", WS_CHILD, 0, 0, 1, 1, g_win, NULL, inst, NULL);
    g_bios = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                           0, 0, 1, 1, g_win, (HMENU)(INT_PTR)ID_BIOS, inst, NULL);
    g_exp_label = CreateWindowW(L"STATIC", L"RAM Expansion", WS_CHILD, 0, 0, 1, 1, g_win, NULL, inst, NULL);
    g_exp = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                          0, 0, 1, 1, g_win, (HMENU)(INT_PTR)ID_EXP, inst, NULL);
    g_machine_note = CreateWindowW(L"STATIC", L"Changes take effect when the machine restarts.",
                                   WS_CHILD, 0, 0, 1, 1, g_win, NULL, inst, NULL);
    g_rom_note = CreateWindowW(L"STATIC",
        L"The console's BIOS ROM is not part of the app. Import it from a file, a zip or a "
        L"folder (MAME's astrocde set works); it is recognised by its checksum. Kept in:",
        WS_CHILD | SS_LEFT | SS_NOPREFIX, 0, 0, 1, 1, g_win, NULL, inst, NULL);
    /* the folder on a line of its own, shortened in the middle if it must be */
    snprintf(note, sizeof note, "%s", astrosession_roms_path(session));
    for (char *c = note; *c; c++)
        if (*c == '/') *c = '\\';
    g_rom_path = CreateWindowW(L"STATIC", L"", WS_CHILD | SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS,
                               0, 0, 1, 1, g_win, NULL, inst, NULL);
    astro_set_text_utf8(g_rom_path, note);
    g_rom_files = CreateWindowW(L"BUTTON", L"Import...", WS_CHILD | WS_TABSTOP, 0, 0, 1, 1, g_win,
                                (HMENU)(INT_PTR)ID_ROM_FILES, inst, NULL);
    g_rom_folder = CreateWindowW(L"BUTTON", L"Import a Folder...", WS_CHILD | WS_TABSTOP, 0, 0, 1, 1, g_win,
                                 (HMENU)(INT_PTR)ID_ROM_FOLDER, inst, NULL);
    g_rom_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                 WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER,
                                 0, 0, 1, 1, g_win, (HMENU)(INT_PTR)ID_ROM_LIST, inst, NULL);
    SendMessageW(g_rom_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    {
        static const wchar_t *cols[] = { L"ROM", L"Group", L"State" };
        LVCOLUMNW col;
        memset(&col, 0, sizeof col);
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = 100;
        for (int i = 0; i < 3; i++) {
            col.pszText = (wchar_t *)cols[i];
            SendMessageW(g_rom_list, LVM_INSERTCOLUMNW, (WPARAM)i, (LPARAM)&col);
        }
    }
    g_close = CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                            0, 0, 1, 1, g_win, (HMENU)(INT_PTR)ID_CLOSE, inst, NULL);
    /* the tab body under the pages: its repaints never cover them */
    SetWindowPos(g_tabs, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    for (int i = 0; astrosession_exp_name(i); i++) {
        wchar_t *w = astro_wide_dup(astrosession_exp_name(i));
        if (w) { SendMessageW(g_exp, CB_ADDSTRING, 0, (LPARAM)w); free(w); }
    }
    SendMessageW(g_exp, CB_SETCURSEL,
                 (WPARAM)astrosession_get_int(session, "exp", ASTROSESSION_EXP_NONE), 0);

    make_font();
    fit_window(NULL);
    layout();
    refresh_roms();
    SendMessageW(g_tabs, TCM_SETCURSEL, roms_page ? PAGE_ROMS : PAGE_MACHINE, 0);
    show_page(roms_page ? PAGE_ROMS : PAGE_MACHINE);
    ShowWindow(g_win, SW_SHOW);
}
