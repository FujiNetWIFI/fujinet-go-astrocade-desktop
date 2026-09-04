/*
 * FujiNet Go Astrocade -- Win32 frontend (raw Win32 + GDI, no toolkit).
 *
 * The emulator frame is blitted with StretchDIBits, letterboxed to 4:3; a
 * WM_TIMER drives the frame-poll repaint (the core paces itself). A menu
 * covers cartridge/ROM/keypad/reset/FujiNet-config; the keypad is a child
 * window of buttons subclassed so a press and release drive
 * astrosession_keypad_set (Win32 BUTTON only reports a click). FujiNet
 * Configuration opens the system browser via ShellExecute.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>

#include "astrosession.h"
#include "bindings.h"
#include "debugger/dbg_window.h"
#include "key_forward.h"
#include "resource.h"

static astrosession *g_session;
static HWND g_hwnd;
static HWND g_keypad_window;
static uint32_t g_frame[ASTROSESSION_FB_WIDTH * ASTROSESSION_FB_HEIGHT];
static uint64_t g_serial;
static int g_sysact_down[ASTROSESSION_SYSACT_COUNT];

/* ---- display ---- */

static void paint_frame(HWND hwnd, HDC hdc)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;

    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    FillRect(hdc, &rc, black);

    const double aspect = 4.0 / 3.0;
    double dw, dh;
    if ((double)w / h > aspect) { dh = h; dw = h * aspect; }
    else                        { dw = w; dh = w / aspect; }
    int dx = (int)((w - dw) / 2), dy = (int)((h - dh) / 2);

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = ASTROSESSION_FB_WIDTH;
    bmi.bmiHeader.biHeight = -ASTROSESSION_FB_HEIGHT;   /* top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    SetStretchBltMode(hdc, COLORONCOLOR);
    StretchDIBits(hdc, dx, dy, (int)dw, (int)dh,
                  0, 0, ASTROSESSION_FB_WIDTH, ASTROSESSION_FB_HEIGHT,
                  g_frame, &bmi, DIB_RGB_COLORS, SRCCOPY);
}

/* ---- keypad window ---- */

static const char *k_labels[ASTROSESSION_KEY_COUNT] = {
    "C","\x18","\x19","%",  "MR","MS","CH","/",  "7","8","9","*",
    "4","5","6","-",  "1","2","3","+",  "CE","0",".","=",
};
static WNDPROC g_btn_proc;
static HWND g_key_btn[ASTROSESSION_KEY_COUNT];

static LRESULT CALLBACK key_btn_proc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    int key = (int)(intptr_t)GetProp(h, "astro_key");
    if (msg == WM_LBUTTONDOWN)
        astrosession_keypad_set(g_session, (astrosession_key)key, 1);
    else if (msg == WM_LBUTTONUP)
        astrosession_keypad_set(g_session, (astrosession_key)key, 0);
    return CallWindowProc(g_btn_proc, h, msg, w, l);
}

static void sysact_click(int a) { astrosession_sysaction_fire(g_session, (astrosession_sysaction)a); }

/* ---- FujiNet console log window ---- */

static HWND g_log_window, g_log_edit;

static LRESULT CALLBACK log_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_SIZE:
        MoveWindow(g_log_edit, 0, 0, LOWORD(l), HIWORD(l), TRUE);
        return 0;
    case WM_TIMER: {
        static char buf[16384];
        astrosession_fujinet_copy_log(g_session, buf, sizeof buf);
        SetWindowTextA(g_log_edit, buf);
        return 0;
    }
    case WM_CLOSE:
        KillTimer(hwnd, 2);
        DestroyWindow(hwnd);
        g_log_window = NULL;
        return 0;
    }
    return DefWindowProc(hwnd, msg, w, l);
}

static void show_log(void)
{
    if (g_log_window) { SetForegroundWindow(g_log_window); return; }
    static int registered;
    HINSTANCE inst = GetModuleHandle(NULL);
    if (!registered) {
        WNDCLASSA wc; ZeroMemory(&wc, sizeof wc);
        wc.lpfnWndProc = log_proc; wc.hInstance = inst;
        wc.lpszClassName = "AstroLog";
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        RegisterClassA(&wc);
        registered = 1;
    }
    g_log_window = CreateWindowA("AstroLog", "FujiNet Console Log",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 700, 500,
        g_hwnd, NULL, inst, NULL);
    g_log_edit = CreateWindowA("EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        0, 0, 700, 500, g_log_window, NULL, inst, NULL);
    HFONT mono = (HFONT)GetStockObject(ANSI_FIXED_FONT);
    SendMessage(g_log_edit, WM_SETFONT, (WPARAM)mono, TRUE);
    ShowWindow(g_log_window, SW_SHOW);
    SetTimer(g_log_window, 2, 1000, NULL);
    SendMessage(g_log_window, WM_TIMER, 2, 0);
}

static LRESULT CALLBACK keypad_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == 100) sysact_click(ASTROSESSION_SYSACT_RESET_GAME);
        else if (id == 101) sysact_click(ASTROSESSION_SYSACT_RESET_CONFIG);
        return 0;
    }
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProc(hwnd, msg, w, l);
}

static void create_keypad(void)
{
    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = keypad_proc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "AstroKeypad";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassA(&wc);

    g_keypad_window = CreateWindowA("AstroKeypad", "Keypad",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 300, 400, g_hwnd, NULL,
        GetModuleHandle(NULL), NULL);

    HINSTANCE inst = GetModuleHandle(NULL);
    for (int key = 0; key < ASTROSESSION_KEY_COUNT; key++) {
        int row = key / 4, col = key % 4;
        HWND b = CreateWindowA("BUTTON", k_labels[key],
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            8 + col * 68, 8 + row * 44, 64, 40, g_keypad_window,
            (HMENU)(intptr_t)(500 + key), inst, NULL);
        SetProp(b, "astro_key", (HANDLE)(intptr_t)key);
        if (!g_btn_proc)
            g_btn_proc = (WNDPROC)GetWindowLongPtr(b, GWLP_WNDPROC);
        SetWindowLongPtr(b, GWLP_WNDPROC, (LONG_PTR)key_btn_proc);
        g_key_btn[key] = b;
    }
    /* System row */
    CreateWindowA("BUTTON", "Reset Game", WS_CHILD | WS_VISIBLE,
        8, 8 + 6 * 44, 130, 32, g_keypad_window, (HMENU)(intptr_t)100, inst, NULL);
    CreateWindowA("BUTTON", "Reset to CONFIG", WS_CHILD | WS_VISIBLE,
        146, 8 + 6 * 44, 130, 32, g_keypad_window, (HMENU)(intptr_t)101, inst, NULL);
}

static void toggle_keypad(void)
{
    if (!g_keypad_window)
        create_keypad();
    ShowWindow(g_keypad_window, IsWindowVisible(g_keypad_window) ? SW_HIDE : SW_SHOW);
}

/* ---- input ---- */

static void forward_key(int keysym, int down)
{
    astro_mapping m = bindings_resolve_keysym(keysym);
    if (m.kind == ASTRO_MAP_SYSACT) {
        if (down) {
            if (!g_sysact_down[m.value]) {
                g_sysact_down[m.value] = 1;
                astrosession_sysaction_fire(g_session, (astrosession_sysaction)m.value);
            }
        } else {
            g_sysact_down[m.value] = 0;
        }
        return;
    }
    if (m.kind == ASTRO_MAP_KEY)
        astrosession_keypad_set(g_session, (astrosession_key)m.value, down);
}

/* ---- menu / actions ---- */

static void restart_session(void)
{
    astrosession_start_opts opts;
    astrosession_stop(g_session);
    astrosession_default_opts(g_session, &opts);
    astrosession_start(g_session, &opts);
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
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return GetOpenFileNameA(&ofn) ? 0 : -1;
}

static void do_command(int id)
{
    char path[MAX_PATH];
    switch (id) {
    case IDM_OPEN_CART:
        if (open_file(path, sizeof path, "Open Cartridge") == 0)
            astrosession_load_cart(g_session, path);
        break;
    case IDM_IMPORT_ROM:
        if (open_file(path, sizeof path, "Import System ROM (astro.bin)") == 0) {
            char name[64];
            if (astrosession_import_rom(g_session, path, name, sizeof name) == 0) {
                if (!astrosession_is_running(g_session))
                    restart_session();
            } else {
                MessageBoxA(g_hwnd, astrosession_last_error(g_session),
                            "Import failed", MB_OK | MB_ICONWARNING);
            }
        }
        break;
    case IDM_KEYPAD:        toggle_keypad(); break;
    case IDM_DEBUGGER:      astro_debugger_show(g_hwnd); break;
    case IDM_RESET_GAME:    astrosession_reset_game(g_session); break;
    case IDM_RESET_CONFIG:  astrosession_reset_to_config(g_session); break;
    case IDM_FUJINET_CONFIG:
        ShellExecuteA(NULL, "open", astrosession_fujinet_webui_url(g_session),
                      NULL, NULL, SW_SHOWNORMAL);
        break;
    case IDM_FUJINET_LOG:
        show_log();
        break;
    case IDM_ABOUT:
        MessageBoxA(g_hwnd,
            "FujiNet Go Astrocade " ASTRO_VERSION_STRING "\n\n"
            "Self-contained Bally Astrocade with built-in FujiNet.\n"
            "GPL-3.0-or-later - https://fujinet.online/",
            "About", MB_OK);
        break;
    case IDM_EXIT:
        DestroyWindow(g_hwnd);
        break;
    }
}

static HMENU build_menu(void)
{
    HMENU menu = CreateMenu();
    HMENU machine = CreatePopupMenu();
    AppendMenuA(machine, MF_STRING, IDM_OPEN_CART, "&Open Cartridge...");
    AppendMenuA(machine, MF_STRING, IDM_IMPORT_ROM, "&Import System ROM...");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_EXIT, "E&xit");
    AppendMenuA(menu, MF_POPUP, (UINT_PTR)machine, "&Machine");

    HMENU view = CreatePopupMenu();
    AppendMenuA(view, MF_STRING, IDM_KEYPAD, "&Keypad\tF9");
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
        if (w == VK_F9) { toggle_keypad(); return 0; }
        if (w == VK_F12) { astro_debugger_show(g_hwnd); return 0; }
        if (!(l & 0x40000000))   /* ignore auto-repeat */
            forward_key(astro_keysym_from_vk(w), 1);
        return 0;
    case WM_KEYUP:
        forward_key(astro_keysym_from_vk(w), 0);
        return 0;
    case WM_TIMER: {
        unsigned pending = astrosession_sysaction_take(g_session);
        for (int a = 0; a < ASTROSESSION_SYSACT_COUNT; a++)
            if (pending & (1u << a))
                astrosession_sysaction_fire(g_session, (astrosession_sysaction)a);
        if (astrosession_copy_frame(g_session, g_frame, &g_serial))
            InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        paint_frame(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;   /* paint_frame fills the background */
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
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 720, 620,
        NULL, build_menu(), inst, NULL);

    if (!astrosession_has_system_roms(g_session))
        MessageBoxA(g_hwnd, "No system ROM found. Use Machine > Import System ROM.",
                    "FujiNet Go Astrocade", MB_OK | MB_ICONINFORMATION);

    ShowWindow(g_hwnd, show);
    SetTimer(g_hwnd, 1, 16, NULL);   /* ~60 Hz repaint poll */

    /* developer affordances, matching the GNOME/KDE ports */
    if (getenv("ASTRO_OPEN_DEBUGGER"))
        astro_debugger_show(g_hwnd);
    if (getenv("ASTRO_OPEN_KEYPAD"))
        toggle_keypad();

    MSG m;
    while (GetMessage(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }

    astrosession_free(g_session);
    return (int)m.wParam;
}
