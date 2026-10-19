/*
 * The Win32 debugger window, two tabs:
 *
 *  Z80 / Video chip -- a control row (Pause/Resume, Step, Step Over, Step
 *      Out) over a monospace read-only edit showing the Z80 registers,
 *      disassembly and a memory hex view, and -- side by side -- the full
 *      decoded data-chip + sound register text (every register the Nutting
 *      manual documents), with the live screen and 512-pen palette blitted
 *      below via StretchDIBits.
 *  FujiNet -- every FujiBus packet between the console and FujiNet, both
 *      ways, with hex (always traced, from session start), beside the
 *      cartridge's link and mailbox state.
 *
 * Refreshed on a WM_TIMER poll, the same shape as the GNOME/KDE debugger
 * windows over the shared astrodebug/astrovid engine.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <commctrl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "astrodebug.h"
#include "astrovid.h"

#define ID_PAUSE 1
#define ID_STEP 2
#define ID_OVER 3
#define ID_OUT 4
#define ID_TABS 5
#define ID_FUJI_CLEAR 6
#define DISASM_LINES 20
#define MEM_ROWS 12
#define MEM_COLS 16
#define TRACE_MAX 512

enum { PAGE_CPU, PAGE_FUJINET };

static HWND g_win, g_cpu_edit, g_vid_edit, g_btn[4];
static HWND g_tabs, g_fuji_title, g_fuji_note, g_fuji_clear, g_fuji_text;
static HWND g_state_title, g_state_text;
static astrodebug *g_dbg;
static astrosession *g_session;
static uint32_t g_scratch[ASTROVID_MAX_PIXELS];
static int g_scr_w, g_scr_h, g_pal_rows;
static int g_top;               /* below the tab strip: where the pages start */
static int g_page, g_mono_h = 13;
static int g_fuji_count = -1, g_fuji_ticks;
static uint64_t g_fuji_last, g_fuji_serial;

static void refresh_cpu(void)
{
    char out[8192];
    int p = 0;

    if (!astrodebug_is_paused(g_dbg)) {
        SetWindowTextA(g_cpu_edit, "(running — Pause to inspect)");
        return;
    }
    astrodebug_regs r;
    if (!astrodebug_regs_get(g_dbg, &r))
        return;
    p += snprintf(out + p, sizeof out - p,
        "AF %04X  BC %04X  DE %04X  HL %04X\r\n"
        "AF'%04X BC'%04X DE'%04X HL'%04X\r\n"
        "IX %04X  IY %04X  SP %04X  PC %04X\r\n"
        "I %02X R %02X IM %d IFF1 %d IFF2 %d %s WZ %04X\r\n\r\n",
        r.af, r.bc, r.de, r.hl, r.af2, r.bc2, r.de2, r.hl2,
        r.ix, r.iy, r.sp, r.pc, r.i, r.r, r.im, r.iff1, r.iff2,
        r.halt ? "HALT" : "", r.wz);

    astrodebug_dasm_line lines[DISASM_LINES];
    int n = astrodebug_disassemble(g_dbg, r.pc, lines, DISASM_LINES);
    for (int i = 0; i < n && p < (int)sizeof out - 80; i++)
        p += snprintf(out + p, sizeof out - p, "%s %04X  %s\r\n",
                      i == 0 ? ">" : " ", lines[i].addr, lines[i].text);

    p += snprintf(out + p, sizeof out - p, "\r\n");
    for (int row = 0; row < MEM_ROWS && p < (int)sizeof out - 96; row++) {
        uint16_t base = (uint16_t)(row * MEM_COLS);
        uint8_t bytes[MEM_COLS];
        astrodebug_read(g_dbg, base, bytes, MEM_COLS);
        p += snprintf(out + p, sizeof out - p, "%04X ", base);
        for (int c = 0; c < MEM_COLS; c++)
            p += snprintf(out + p, sizeof out - p, "%02X ", bytes[c]);
        p += snprintf(out + p, sizeof out - p, "\r\n");
    }
    SetWindowTextA(g_cpu_edit, out);
}

static void refresh_video(void)
{
    astrovid_snapshot snap;
    astrovid_snapshot_get(&snap);
    char text[4096];
    astrovid_format_state(&snap, text, sizeof text);
    /* the edit control wants CRLF; astrovid uses LF */
    char crlf[8192];
    int o = 0;
    for (int i = 0; text[i] && o < (int)sizeof crlf - 2; i++) {
        if (text[i] == '\n') crlf[o++] = '\r';
        crlf[o++] = text[i];
    }
    crlf[o] = '\0';
    SetWindowTextA(g_vid_edit, crlf);

    astrovid_render_screen(&snap, g_scratch, &g_scr_w, &g_scr_h);
    /* stash palette rows for the paint (rendered on demand there) */
    (void)g_pal_rows;
    InvalidateRect(g_win, NULL, FALSE);
}

/* ---- the FujiNet page's views ---- */

/* EDIT controls want CR LF and draw control characters as boxes: engine
 * text (UTF-8) as the wide text a view shows. */
static wchar_t *wide_view_text(const char *text)
{
    size_t n = strlen(text), i, o = 0;
    char *buf = malloc(n * 2 + 1);
    wchar_t *w = NULL;
    int wn;
    if (!buf) return NULL;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n') { buf[o++] = '\r'; buf[o++] = '\n'; }
        else if (c >= 0x20 || c == '\t') buf[o++] = (char)c;
    }
    buf[o] = '\0';
    wn = MultiByteToWideChar(CP_UTF8, 0, buf, -1, NULL, 0);
    if (wn > 0 && (w = malloc((size_t)wn * sizeof *w)) != NULL)
        MultiByteToWideChar(CP_UTF8, 0, buf, -1, w, wn);
    free(buf);
    return w;
}

static size_t addf(char *s, size_t len, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (len >= cap) return len;
    va_start(ap, fmt);
    n = vsnprintf(s + len, cap - len, fmt, ap);
    va_end(ap);
    if (n < 0) return len;
    return len + (size_t)n < cap ? len + (size_t)n : cap - 1;
}

/* A log follows its end while its reader is there. */
static int edit_at_end(HWND h)
{
    RECT c;
    GetClientRect(h, &c);
    const int rows = g_mono_h > 0 && c.bottom > g_mono_h ? c.bottom / g_mono_h : 1;
    const int first = (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    const int count = (int)SendMessageW(h, EM_GETLINECOUNT, 0, 0);
    return first + rows >= count;
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
static void set_text_at(HWND h, const char *text, int follow)
{
    const int first = (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    wchar_t *w = wide_view_text(text);
    if (!w) return;
    SendMessageW(h, WM_SETREDRAW, FALSE, 0);
    SetWindowTextW(h, w);
    if (follow)
        SendMessageW(h, EM_LINESCROLL, 0, SendMessageW(h, EM_GETLINECOUNT, 0, 0));
    else
        SendMessageW(h, EM_LINESCROLL, 0, first - (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0));
    SendMessageW(h, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(h, NULL, TRUE);
    free(w);
}

static void refresh_fujinet(int force)
{
    static astrodebug_fuji_packet p[TRACE_MAX];
    static char t[TRACE_MAX * 320 + 128];
    char state[2048];
    size_t len = 0;
    int n = astrodebug_fujinet_trace(g_dbg, p, TRACE_MAX), follow;

    if (astrosession_fujinet_link(g_session))
        SetWindowTextA(g_fuji_note, "FujiNet cartridge, connected.");
    else if (astrosession_fujinet_running(g_session))
        SetWindowTextA(g_fuji_note, "FujiNet cartridge, not connected.");
    else
        SetWindowTextA(g_fuji_note,
                       "FujiNet cartridge, not connected (the FujiNet runtime is not running).");
    astrodebug_fujinet_state(g_dbg, state, sizeof state);
    set_text_at(g_state_text, state, 0);

    if (!force && n == g_fuji_count && (n == 0 || p[n - 1].serial == g_fuji_last))
        return;
    follow = force || g_fuji_count < 0 || edit_at_end(g_fuji_text);
    g_fuji_count = n;
    g_fuji_last = n > 0 ? p[n - 1].serial : 0;
    for (int i = 0; i < n; i++) {
        len = addf(t, len, sizeof t, "%s%s", i ? "\n" : "", p[i].text);
        if (p[i].repeat > 1)
            len = addf(t, len, sizeof t, " \xc3\x97%u", p[i].repeat);
        len = addf(t, len, sizeof t, "\n     ");
        for (int k = 0; k < p[i].nbytes && k < (int)sizeof p[i].data; k++)
            len = addf(t, len, sizeof t, "%02X ", p[i].data[k]);
        if (p[i].nbytes > (int)sizeof p[i].data) len = addf(t, len, sizeof t, "\xe2\x80\xa6");
    }
    if (n == 0) len = addf(t, len, sizeof t, "(no FujiBus packets since the trace was cleared)");
    set_text_at(g_fuji_text, t, follow);
}

/* The FujiNet page fills the window: the packets left (a note and Clear
 * over them), the cartridge right. */
static void layout_fujinet(void)
{
    RECT c;
    GetClientRect(g_win, &c);
    const int x = 8, y = g_top, h = c.bottom - y - 8;
    const int rw = 300, lw = c.right - 2 * x - rw - 12;
    MoveWindow(g_tabs, 8, 4, c.right - 16, g_top - 8, TRUE);
    MoveWindow(g_fuji_title, x, y, lw, 18, TRUE);
    MoveWindow(g_fuji_clear, x + lw - 80, y + 22, 80, 26, TRUE);
    MoveWindow(g_fuji_note, x, y + 26, lw - 88, 18, TRUE);
    MoveWindow(g_fuji_text, x, y + 54, lw, h - 54, TRUE);
    MoveWindow(g_state_title, x + lw + 12, y, rw, 18, TRUE);
    MoveWindow(g_state_text, x + lw + 12, y + 22, rw, h - 22, TRUE);
}

static void show_page(int page)
{
    HWND cpu[] = { g_btn[0], g_btn[1], g_btn[2], g_btn[3], g_cpu_edit, g_vid_edit };
    HWND fuji[] = { g_fuji_title, g_fuji_note, g_fuji_clear, g_fuji_text,
                    g_state_title, g_state_text };
    g_page = page;
    for (size_t i = 0; i < sizeof cpu / sizeof cpu[0]; i++)
        ShowWindow(cpu[i], page == PAGE_CPU ? SW_SHOW : SW_HIDE);
    for (size_t i = 0; i < sizeof fuji / sizeof fuji[0]; i++)
        ShowWindow(fuji[i], page == PAGE_FUJINET ? SW_SHOW : SW_HIDE);
    if (page == PAGE_FUJINET)
        refresh_fujinet(0);
    InvalidateRect(g_win, NULL, TRUE);      /* the blits come and go */
}

static void blit(HDC hdc, int x, int y, int dw, int dh, uint32_t *px, int w, int h)
{
    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(hdc, COLORONCOLOR);
    StretchDIBits(hdc, x, y, dw, dh, 0, 0, w, h, px, &bmi, DIB_RGB_COLORS, SRCCOPY);
}

static LRESULT CALLBACK dbg_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_PAUSE:
            if (astrodebug_is_paused(g_dbg)) astrodebug_resume(g_dbg);
            else astrodebug_pause(g_dbg);
            break;
        case ID_STEP: astrodebug_step(g_dbg); break;
        case ID_OVER: astrodebug_step_over(g_dbg); break;
        case ID_OUT:  astrodebug_step_out(g_dbg); break;
        case ID_FUJI_CLEAR:
            astrodebug_fujinet_trace_clear(g_dbg);
            refresh_fujinet(1);
            break;
        }
        return 0;
    case WM_NOTIFY: {
        const NMHDR *nh = (const NMHDR *)l;
        if (nh->hwndFrom == g_tabs && nh->code == (UINT)TCN_SELCHANGE)
            show_page((int)SendMessageA(g_tabs, TCM_GETCURSEL, 0, 0));
        return 0;
    }
    case WM_SIZE:
        if (g_tabs)
            layout_fujinet();
        return 0;
    case WM_TIMER:
        if (g_page == PAGE_CPU) {
            refresh_cpu();
            refresh_video();
        } else {
            /* FujiNet: on a stop, and every other tick while running */
            uint64_t s = astrodebug_stop_serial(g_dbg);
            if (s != g_fuji_serial || (!astrodebug_is_paused(g_dbg) && ++g_fuji_ticks >= 2)) {
                g_fuji_serial = s;
                g_fuji_ticks = 0;
                refresh_fujinet(0);
            }
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (g_page == PAGE_CPU) {
            /* the screen render sits to the right of the video-text edit */
            astrovid_snapshot snap;
            astrovid_snapshot_get(&snap);
            int w, h;
            astrovid_render_screen(&snap, g_scratch, &w, &h);
            blit(hdc, 470, g_top + 300, 352, 240, g_scratch, w, h);
            int rows = astrovid_render_palette(&snap, g_scratch, ASTROVID_PAL_COLS,
                                               ASTROVID_PAL_CELL);
            blit(hdc, 470, g_top + 550, ASTROVID_PAL_COLS * 8, rows * 4, g_scratch,
                 ASTROVID_PAL_COLS * ASTROVID_PAL_CELL, rows * ASTROVID_PAL_CELL);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        KillTimer(hwnd, 3);
        astrodebug_set_engaged(g_dbg, 0);
        DestroyWindow(hwnd);
        g_win = NULL;
        g_tabs = NULL;
        return 0;
    }
    return DefWindowProc(hwnd, msg, w, l);
}

void astro_debugger_show(HWND parent, astrosession *session)
{
    if (g_win) { SetForegroundWindow(g_win); return; }
    g_session = session;
    g_dbg = astrodebug_get();
    astrodebug_set_engaged(g_dbg, 1);

    static int registered;
    HINSTANCE inst = GetModuleHandle(NULL);
    if (!registered) {
        WNDCLASSA wc; ZeroMemory(&wc, sizeof wc);
        wc.lpfnWndProc = dbg_proc; wc.hInstance = inst;
        wc.lpszClassName = "AstroDbg";
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassA(&wc);
        registered = 1;
    }
    g_win = CreateWindowA("AstroDbg", "Debugger", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 860, 856, parent, NULL, inst, NULL);

    /* The tab control is the strip alone: the pages sit on the window's own
     * background below it. */
    g_tabs = CreateWindowA(WC_TABCONTROLA, "", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        8, 4, 828, 30, g_win, (HMENU)(intptr_t)ID_TABS, inst, NULL);
    const char *tabs[] = { "Z80 / Video chip", "FujiNet" };
    TCITEMA item;
    memset(&item, 0, sizeof item);
    item.mask = TCIF_TEXT;
    for (int i = 0; i < 2; i++) {
        item.pszText = (char *)tabs[i];
        SendMessageA(g_tabs, TCM_INSERTITEMA, (WPARAM)i, (LPARAM)&item);
    }
    RECT page;
    SetRect(&page, 0, 0, 400, 400);
    SendMessageA(g_tabs, TCM_ADJUSTRECT, FALSE, (LPARAM)&page);
    g_top = 4 + page.top + 4;

    const char *btns[] = { "Pause/Resume", "Step", "Step Over", "Step Out" };
    for (int i = 0; i < 4; i++)
        g_btn[i] = CreateWindowA("BUTTON", btns[i], WS_CHILD | WS_VISIBLE,
            8 + i * 130, g_top + 8, 124, 28, g_win, (HMENU)(intptr_t)(ID_PAUSE + i), inst, NULL);

    HFONT mono = (HFONT)GetStockObject(ANSI_FIXED_FONT);
    g_cpu_edit = CreateWindowA("EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, g_top + 44, 440, 740, g_win, NULL, inst, NULL);
    SendMessage(g_cpu_edit, WM_SETFONT, (WPARAM)mono, TRUE);
    g_vid_edit = CreateWindowA("EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        460, g_top + 44, 380, 240, g_win, NULL, inst, NULL);
    SendMessage(g_vid_edit, WM_SETFONT, (WPARAM)mono, TRUE);

    /* FujiNet */
    g_fuji_title = CreateWindowA("STATIC", "FujiBus packets (SLIP-decoded)",
        WS_CHILD | SS_LEFT, 0, 0, 1, 1, g_win, NULL, inst, NULL);
    g_fuji_note = CreateWindowA("STATIC", "", WS_CHILD | SS_LEFT | SS_ENDELLIPSIS,
        0, 0, 1, 1, g_win, NULL, inst, NULL);
    g_fuji_clear = CreateWindowA("BUTTON", "Clear", WS_CHILD,
        0, 0, 1, 1, g_win, (HMENU)(intptr_t)ID_FUJI_CLEAR, inst, NULL);
    g_fuji_text = CreateWindowA("EDIT", "",
        WS_CHILD | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOHSCROLL | ES_READONLY,
        0, 0, 1, 1, g_win, NULL, inst, NULL);
    SendMessage(g_fuji_text, WM_SETFONT, (WPARAM)mono, TRUE);
    g_state_title = CreateWindowA("STATIC", "FujiNet cartridge",
        WS_CHILD | SS_LEFT, 0, 0, 1, 1, g_win, NULL, inst, NULL);
    g_state_text = CreateWindowA("EDIT", "",
        WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        0, 0, 1, 1, g_win, NULL, inst, NULL);
    SendMessage(g_state_text, WM_SETFONT, (WPARAM)mono, TRUE);

    /* the monospace line, for following the log */
    HDC dc = GetDC(g_win);
    HGDIOBJ prev = SelectObject(dc, mono);
    TEXTMETRICA tm;
    if (GetTextMetricsA(dc, &tm))
        g_mono_h = tm.tmHeight;
    SelectObject(dc, prev);
    ReleaseDC(g_win, dc);

    g_page = PAGE_CPU;
    g_fuji_count = -1;
    layout_fujinet();
    /* ASTRO_DEBUGGER_TAB=2: open on FujiNet (the GNOME page number) */
    const char *tab = getenv("ASTRO_DEBUGGER_TAB");
    if (tab && atoi(tab) == 2) {
        SendMessageA(g_tabs, TCM_SETCURSEL, PAGE_FUJINET, 0);
        show_page(PAGE_FUJINET);
    }

    ShowWindow(g_win, SW_SHOW);
    SetTimer(g_win, 3, 200, NULL);
}
