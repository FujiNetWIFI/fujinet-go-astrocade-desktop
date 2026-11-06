/*
 * FujiNet Go Astrocade -- Win32 frontend (raw Win32, no toolkit).
 *
 * The picture is a child window drawn by display.c the way a game draws
 * (Direct3D 11 flip model, presented on vblank from its own thread). Until a
 * BIOS is imported, the No-BIOS page is painted in its place, with the same
 * two buttons fujinet-go-msdos-desktop has. A menu covers cartridge, BIOS
 * import, Preferences, keypad, debugger, resets and FujiNet; the Controllers window
 * (keypad/keypad_window.c) is a fixed-size window of painted controls. FujiNet
 * Configuration opens the system browser via ShellExecute.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <stdio.h>

#include "astrosession.h"
#include "bindings.h"
#include "debugger/dbg_window.h"
#include "display.h"
#include "fujilog.h"
#include "import.h"
#include "keypad/keypad_window.h"
#include "prefs.h"
#include "resource.h"
#include "win_common.h"

static astrosession *g_session;
static HWND g_hwnd, g_display;
static HWND g_bios_files_btn, g_bios_folder_btn;
static HFONT g_ui_font, g_title_font;
static HICON g_big_icon;
static int g_dpi = 96;

#define S(v) astro_scale((v), g_dpi)

static void start_machine(void);

/* ---- the No-BIOS page ---- */

typedef struct {
    RECT icon, title, desc, files, folder;
} nobios_geom;

static const char k_nobios_title[] = "No Astrocade BIOS";
static const char k_nobios_text[] =
    "Bally's BIOS ROM is not part of the app. Import the one you have \xe2\x80\x94 astro.bin, "
    "ballyhlc.bin or bioswhit.bin, a zip such as MAME's astrocde set, or a folder \xe2\x80\x94 "
    "and the Astrocade starts.";

static int text_height(HFONT font, const char *utf8, int width)
{
    wchar_t *w = astro_wide_dup(utf8);
    HDC dc = GetDC(g_hwnd);
    HGDIOBJ old = SelectObject(dc, font);
    RECT r;
    SetRect(&r, 0, 0, width, 0);
    if (w) DrawTextW(dc, w, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_CENTER | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(g_hwnd, dc);
    free(w);
    return r.bottom - r.top;
}

static int button_width(const char *text)
{
    HDC dc = GetDC(g_hwnd);
    HGDIOBJ old = SelectObject(dc, g_ui_font);
    SIZE sz = { 0, 0 };
    GetTextExtentPoint32A(dc, text, (int)strlen(text), &sz);
    SelectObject(dc, old);
    ReleaseDC(g_hwnd, dc);
    return sz.cx + S(28) < S(80) ? S(80) : sz.cx + S(28);
}

static void nobios_layout(const RECT *area, nobios_geom *g)
{
    const int aw = area->right - area->left, ah = area->bottom - area->top;
    const int width = aw - S(48) < S(560) ? aw - S(48) : S(560);
    const int icon = S(64), title_h = text_height(g_title_font, k_nobios_title, width);
    const int desc_h = text_height(g_ui_font, k_nobios_text, width);
    const int bw1 = button_width("Import BIOS..."), bw2 = button_width("Import a Folder...");
    const int row_w = bw1 + S(12) + bw2, bh = S(32);
    const int total = icon + S(16) + title_h + S(10) + desc_h + S(24) + bh;
    const int x0 = area->left + (aw - width) / 2, cx = area->left + aw / 2;
    int y = area->top + (ah - total) / 2;
    if (y < area->top + S(8)) y = area->top + S(8);
    SetRect(&g->icon, cx - icon / 2, y, cx + icon / 2, y + icon);
    y += icon + S(16);
    SetRect(&g->title, x0, y, x0 + width, y + title_h);
    y += title_h + S(10);
    SetRect(&g->desc, x0, y, x0 + width, y + desc_h);
    y += desc_h + S(24);
    SetRect(&g->files, cx - row_w / 2, y, cx - row_w / 2 + bw1, y + bh);
    SetRect(&g->folder, g->files.right + S(12), y, g->files.right + S(12) + bw2, y + bh);
}

static void paint_nobios(HDC dc, const RECT *area)
{
    nobios_geom g;
    wchar_t *w;
    nobios_layout(area, &g);
    FillRect(dc, area, GetSysColorBrush(COLOR_WINDOW));
    SetBkMode(dc, TRANSPARENT);
    if (g_big_icon)
        DrawIconEx(dc, g.icon.left, g.icon.top, g_big_icon, g.icon.right - g.icon.left,
                   g.icon.bottom - g.icon.top, 0, NULL, DI_NORMAL);
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    SelectObject(dc, g_title_font);
    if ((w = astro_wide_dup(k_nobios_title)) != NULL) {
        DrawTextW(dc, w, -1, &g.title, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        free(w);
    }
    SelectObject(dc, g_ui_font);
    SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    if ((w = astro_wide_dup(k_nobios_text)) != NULL) {
        DrawTextW(dc, w, -1, &g.desc, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        free(w);
    }
}

/* The picture fills the client while there is a BIOS; else the page. */
static void layout(void)
{
    RECT c;
    GetClientRect(g_hwnd, &c);
    if (g_display)
        MoveWindow(g_display, 0, 0, c.right, c.bottom, TRUE);
    if (!astrosession_has_system_roms(g_session)) {
        nobios_geom g;
        nobios_layout(&c, &g);
        MoveWindow(g_bios_files_btn, g.files.left, g.files.top, g.files.right - g.files.left,
                   g.files.bottom - g.files.top, TRUE);
        MoveWindow(g_bios_folder_btn, g.folder.left, g.folder.top, g.folder.right - g.folder.left,
                   g.folder.bottom - g.folder.top, TRUE);
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void show_page(void)
{
    const int bios = astrosession_has_system_roms(g_session);
    if (g_display) ShowWindow(g_display, bios ? SW_SHOW : SW_HIDE);
    ShowWindow(g_bios_files_btn, bios ? SW_HIDE : SW_SHOW);
    ShowWindow(g_bios_folder_btn, bios ? SW_HIDE : SW_SHOW);
    layout();
}

static void make_fonts(void)
{
    if (g_ui_font) DeleteObject(g_ui_font);
    if (g_title_font) DeleteObject(g_title_font);
    g_ui_font = astro_make_font(g_dpi, 0, FW_NORMAL, NULL);
    g_title_font = astro_make_font(g_dpi, 22, FW_BOLD, NULL);
    if (g_bios_files_btn) SendMessageW(g_bios_files_btn, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    if (g_bios_folder_btn) SendMessageW(g_bios_folder_btn, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
}

static void toggle_keypad(void)
{
    astro_controllers_toggle(g_hwnd, g_session);
}

/* ---- menu / actions ---- */

static void restart_session(void)
{
    astrosession_start_opts opts;
    astrosession_stop(g_session);
    astrosession_default_opts(g_session, &opts);
    if (astrosession_start(g_session, &opts) != 0)
        astro_message(g_hwnd, astrosession_last_error(g_session), "Could not start",
                      MB_OK | MB_ICONWARNING);
    show_page();
}

/* Starts the machine once a BIOS is there to boot (after an import). */
static void start_machine(void)
{
    show_page();
    if (!astrosession_is_running(g_session) && astrosession_has_system_roms(g_session))
        restart_session();
}

static void prefs_imported(void) { show_page(); }
static void prefs_closed(int dirty) { if (dirty) restart_session(); else start_machine(); }

static void show_prefs(int roms_page)
{
    static const astro_prefs_hooks hooks = { prefs_imported, prefs_closed };
    astro_prefs_show(g_hwnd, g_session, &hooks, roms_page);
}

static int open_file(char *out, int outlen, const char *title)
{
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof ofn);
    out[0] = '\0';
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = "All Files\0*.*\0";
    ofn.lpstrFile = out;
    ofn.nMaxFile = outlen;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn) ? 0 : -1;
}

static void do_command(int id)
{
    char path[MAX_PATH];
    switch (id) {
    case IDM_OPEN_CART:
        if (open_file(path, sizeof path, "Open Cartridge") == 0 &&
            astrosession_load_cart(g_session, path) != 0)
            astro_message(g_hwnd, astrosession_last_error(g_session), "Open Cartridge",
                          MB_OK | MB_ICONWARNING);
        break;
    case IDM_IMPORT_BIOS:
    case IDC_BIOS_FILES:
        if (astro_import_bios(g_hwnd, g_session, 0, 0) >= 0)
            start_machine();
        break;
    case IDM_IMPORT_BIOS_DIR:
    case IDC_BIOS_FOLDER:
        if (astro_import_bios(g_hwnd, g_session, 1, 0) >= 0)
            start_machine();
        break;
    case IDM_PREFS:         show_prefs(0); break;
    case IDM_KEYPAD:        toggle_keypad(); break;
    case IDM_DEBUGGER:      astro_debugger_show(g_hwnd, g_session); break;
    case IDM_RESET_GAME:    astrosession_reset_game(g_session); break;
    case IDM_RESET_CONFIG:  astrosession_reset_to_config(g_session); break;
    case IDM_FUJINET_CONFIG:
        if (astrosession_fujinet_external(g_session)) {
            astro_message(g_hwnd, "FujiNet is external: use the web UI of that instance.",
                          "FujiNet Go Astrocade", MB_OK | MB_ICONINFORMATION);
            break;
        }
        ShellExecuteA(NULL, "open", astrosession_fujinet_webui_url(g_session),
                      NULL, NULL, SW_SHOWNORMAL);
        break;
    case IDM_FUJINET_LOG:
        if (astrosession_fujinet_external(g_session)) {
            astro_message(g_hwnd, "FujiNet is external: its console output is in the "
                          "process that is running it.", "FujiNet Go Astrocade",
                          MB_OK | MB_ICONINFORMATION);
            break;
        }
        astro_fujilog_show(g_hwnd, g_session);
        break;
    case IDM_ABOUT: {
        char text[512];
        snprintf(text, sizeof text,
            "FujiNet Go Astrocade " ASTRO_VERSION_STRING "\n\n"
            "Self-contained Bally Astrocade with built-in FujiNet.\n"
            "Display: %s\n\n"
            "GPL-3.0-or-later - https://fujinet.online/", astro_display_renderer());
        astro_message(g_hwnd, text, "About", MB_OK);
        break;
    }
    case IDM_EXIT:
        SendMessageW(g_hwnd, WM_CLOSE, 0, 0);
        break;
    }
}

static HMENU build_menu(void)
{
    HMENU menu = CreateMenu();
    HMENU machine = CreatePopupMenu();
    AppendMenuA(machine, MF_STRING, IDM_OPEN_CART, "&Open Cartridge...");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_IMPORT_BIOS, "Import &BIOS...");
    AppendMenuA(machine, MF_STRING, IDM_IMPORT_BIOS_DIR, "Import BIOS from a &Folder...");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_PREFS, "Pr&eferences...");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_EXIT, "E&xit");
    AppendMenuA(menu, MF_POPUP, (UINT_PTR)machine, "&Machine");

    HMENU view = CreatePopupMenu();
    AppendMenuA(view, MF_STRING, IDM_KEYPAD, "&Controllers\tF9");
    AppendMenuA(view, MF_STRING, IDM_DEBUGGER, "&Debugger\tF12");
    AppendMenuA(menu, MF_POPUP, (UINT_PTR)view, "&View");

    HMENU fn = CreatePopupMenu();
    AppendMenuA(fn, MF_STRING, IDM_RESET_GAME, "Reset &Game");
    AppendMenuA(fn, MF_STRING, IDM_RESET_CONFIG, "Reset to &CONFIG");
    AppendMenuA(fn, MF_STRING, IDM_FUJINET_CONFIG, "FujiNet &Configuration...");
    AppendMenuA(fn, MF_STRING, IDM_FUJINET_LOG, "FujiNet Console &Log...");
    AppendMenuA(menu, MF_POPUP, (UINT_PTR)fn, "&FujiNet");

    HMENU help = CreatePopupMenu();
    AppendMenuA(help, MF_STRING, IDM_ABOUT, "&About");
    AppendMenuA(menu, MF_POPUP, (UINT_PTR)help, "&Help");
    return menu;
}

/* ---- main window proc ---- */

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_COMMAND:
        do_command(LOWORD(w));
        return 0;
    case WM_KEYDOWN:
        if (l & 0x40000000)      /* ignore auto-repeat */
            return 0;
        /* Map mode takes any key, F9 and F12 included */
        if (astro_controllers_visible() && astro_controllers_key(g_session, l, TRUE))
            return 0;
        if (w == VK_F9) { toggle_keypad(); return 0; }
        if (w == VK_F12) { astro_debugger_show(g_hwnd, g_session); return 0; }
        astro_controllers_key(g_session, l, TRUE);
        return 0;
    case WM_KEYUP:
        astro_controllers_key(g_session, l, FALSE);
        return 0;
    case WM_KILLFOCUS:
        /* the key-ups go to whichever window has focus now: let go of
         * everything, or a held key sticks in the machine */
        astrosession_release_all(g_session);
        return 0;
    case WM_TIMER:
        astrosession_sysaction_drain(g_session);    /* posted from the pad thread */
        return 0;
    case WM_SIZE:
        layout();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (!astrosession_has_system_roms(g_session)) {
            RECT c;
            GetClientRect(hwnd, &c);
            paint_nobios(hdc, &c);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;   /* the picture or the page covers the client */
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)l;
        g_dpi = HIWORD(w);
        make_fonts();
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
        return 0;
    }
    case WM_CLOSE:
        astro_display_destroy();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, w, l);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev; (void)cmd;
    InitCommonControls();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);   /* the folder browser */

    g_session = astrosession_new(NULL);
    if (!g_session) {
        MessageBoxA(NULL, "Could not create the session", "Fatal", MB_ICONERROR);
        return 1;
    }
    if (astrosession_has_system_roms(g_session)) {
        astrosession_start_opts opts;
        astrosession_default_opts(g_session, &opts);
        astrosession_start(g_session, &opts);
    }

    WNDCLASSEXA wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = "AstroMainWindow";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(IDI_APPICON));
    RegisterClassExA(&wc);

    g_hwnd = CreateWindowExA(0, "AstroMainWindow", "FujiNet Go Astrocade",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 720, 620,
        NULL, build_menu(), inst, NULL);
    g_dpi = astro_window_dpi(g_hwnd);
    g_big_icon = (HICON)LoadImageA(inst, MAKEINTRESOURCEA(IDI_APPICON), IMAGE_ICON, S(64), S(64), 0);
    g_bios_files_btn = CreateWindowExA(0, "BUTTON", "Import BIOS...", WS_CHILD | BS_DEFPUSHBUTTON,
                                       0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)IDC_BIOS_FILES, inst, NULL);
    g_bios_folder_btn = CreateWindowExA(0, "BUTTON", "Import a Folder...", WS_CHILD | BS_PUSHBUTTON,
                                        0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)IDC_BIOS_FOLDER, inst, NULL);
    make_fonts();
    {
        /* a 4:3 client, at this monitor's DPI */
        SIZE outer = astro_outer_size(g_hwnd, S(768), S(576), TRUE);
        SetWindowPos(g_hwnd, NULL, 0, 0, outer.cx, outer.cy, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
    }
    g_display = astro_display_create(g_hwnd, g_session);
    show_page();

    ShowWindow(g_hwnd, show);
    SetTimer(g_hwnd, 1, 50, NULL);   /* drains posted sysactions */

    /* developer affordances, matching the GNOME/KDE ports */
    if (getenv("ASTRO_OPEN_DEBUGGER"))
        astro_debugger_show(g_hwnd, g_session);
    if (getenv("ASTRO_OPEN_KEYPAD"))
        toggle_keypad();
    if (getenv("ASTRO_OPEN_PREFS"))
        show_prefs(strcmp(getenv("ASTRO_OPEN_PREFS"), "ROMs") == 0);
    if (getenv("ASTRO_OPEN_LOG"))
        astro_fujilog_show(g_hwnd, g_session);

    MSG m;
    while (GetMessage(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }

    astrosession_free(g_session);
    CoUninitialize();
    return (int)m.wParam;
}
