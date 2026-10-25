/*
 * The FujiNet console log window: a live view of the in-process runtime's
 * captured output, refreshed once a second, following its end while the
 * reader is there (a refresh never throws a scrolled view back to its top).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fujilog.h"

#include <string.h>

#include "resource.h"
#include "win_common.h"

#define LOG_CLASS "AstroLog"
#define IDT_LOG_REFRESH 1
#define IDC_LOG_EDIT 100
#define MONO_PX 13

static HWND g_win, g_edit;
static HFONT g_font;
static astrosession *g_session;
static char *g_shown;           /* the text on screen, so an idle log isn't redrawn */

/* EDIT controls want CR LF and draw control characters as boxes. */
static char *to_crlf(const char *text)
{
    size_t n = strlen(text), i, o = 0;
    char *buf = malloc(n * 2 + 1);
    if (!buf) return NULL;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n') { buf[o++] = '\r'; buf[o++] = '\n'; }
        else if (c >= 0x20 || c == '\t') buf[o++] = (char)c;
    }
    buf[o] = '\0';
    return buf;
}

static void refresh(void)
{
    static char buf[128 * 1024];
    RECT c;
    int first, count, rows = 1, n, at_end;
    char *crlf;
    wchar_t *w;

    if (!g_edit) return;
    n = astrosession_fujinet_copy_log(g_session, buf, sizeof buf);
    if (n <= 0)
        snprintf(buf, sizeof buf, "%s", astrosession_fujinet_running(g_session)
                     ? "(no FujiNet output yet)" : "(the FujiNet runtime is not running)");
    if (g_shown && strcmp(g_shown, buf) == 0)
        return;
    free(g_shown);
    g_shown = _strdup(buf);

    first = (int)SendMessageW(g_edit, EM_GETFIRSTVISIBLELINE, 0, 0);
    count = (int)SendMessageW(g_edit, EM_GETLINECOUNT, 0, 0);
    GetClientRect(g_edit, &c);
    {
        SIZE cell = astro_font_cell(g_edit, g_font);
        if (cell.cy > 0) rows = c.bottom / cell.cy;
    }
    at_end = first + (rows > 0 ? rows : 1) >= count;

    crlf = to_crlf(buf);
    if (!crlf) return;
    w = astro_wide_dup(crlf);       /* the log is UTF-8 */
    if (w) {
        SendMessageW(g_edit, WM_SETREDRAW, FALSE, 0);
        SetWindowTextW(g_edit, w);
        /* follow the tail only while the reader is already at the bottom */
        if (at_end)
            SendMessageW(g_edit, EM_LINESCROLL, 0, SendMessageW(g_edit, EM_GETLINECOUNT, 0, 0));
        else
            SendMessageW(g_edit, EM_LINESCROLL, 0, first);
        SendMessageW(g_edit, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g_edit, NULL, TRUE);
        free(w);
    }
    free(crlf);
}

static void make_font(HWND hwnd)
{
    if (g_font) DeleteObject(g_font);
    g_font = astro_make_font(astro_window_dpi(hwnd), MONO_PX, FW_NORMAL, ASTRO_MONO_FACE);
    if (g_edit) SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_font, TRUE);
}

static LRESULT CALLBACK log_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (g_edit) MoveWindow(g_edit, 0, 0, rc.right - rc.left, rc.bottom - rc.top, TRUE);
        return 0;
    }
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)lp;
        make_font(hwnd);
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        /* a read-only EDIT: the window colour, as a text view */
        SetTextColor((HDC)wp, GetSysColor(COLOR_WINDOWTEXT));
        SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_TIMER:
        if (wp == IDT_LOG_REFRESH) refresh();
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, IDT_LOG_REFRESH);
        g_win = NULL;
        g_edit = NULL;
        if (g_font) DeleteObject(g_font);
        g_font = NULL;
        free(g_shown);
        g_shown = NULL;
        return 0;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

void astro_fujilog_show(HWND parent, astrosession *session)
{
    static int registered;
    HINSTANCE inst = GetModuleHandleA(NULL);
    RECT rc;
    int dpi;

    g_session = session;
    if (g_win) {
        if (IsIconic(g_win)) ShowWindow(g_win, SW_RESTORE);
        SetForegroundWindow(g_win);
        return;
    }
    if (!registered) {
        WNDCLASSEXA wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = log_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = LOG_CLASS;
        wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(IDI_APPICON));
        registered = RegisterClassExA(&wc) != 0;
    }
    dpi = astro_window_dpi(parent);
    /* owned by the main window: it stays over it and goes with it */
    g_win = CreateWindowA(LOG_CLASS, "FujiNet Console Log", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                          CW_USEDEFAULT, astro_scale(820, dpi), astro_scale(560, dpi), parent, NULL, inst, NULL);
    if (!g_win) return;
    GetClientRect(g_win, &rc);
    g_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        0, 0, rc.right - rc.left, rc.bottom - rc.top, g_win, (HMENU)(INT_PTR)IDC_LOG_EDIT, inst, NULL);
    SendMessageW(g_edit, EM_SETLIMITTEXT, 0, 0);
    make_font(g_win);
    SetTimer(g_win, IDT_LOG_REFRESH, 1000, NULL);
    refresh();
    ShowWindow(g_win, SW_SHOW);
}
