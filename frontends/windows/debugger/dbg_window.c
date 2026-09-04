/*
 * The Win32 debugger window: a control row (Pause/Resume, Step, Step Over,
 * Step Out) over a monospace read-only edit showing the Z80 registers,
 * disassembly and a memory hex view, and -- side by side -- the full decoded
 * data-chip + sound register text (every register the Nutting manual
 * documents), with the live screen and 512-pen palette blitted below via
 * StretchDIBits. Refreshed on a WM_TIMER poll of the stop serial, the same
 * shape as the GNOME/KDE debugger windows over the shared astrodebug/astrovid
 * engine.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <stdio.h>
#include <string.h>

#include "astrodebug.h"
#include "astrovid.h"

#define ID_PAUSE 1
#define ID_STEP 2
#define ID_OVER 3
#define ID_OUT 4
#define DISASM_LINES 20
#define MEM_ROWS 12
#define MEM_COLS 16

static HWND g_win, g_cpu_edit, g_vid_edit;
static astrodebug *g_dbg;
static uint32_t g_scratch[ASTROVID_MAX_PIXELS];
static int g_scr_w, g_scr_h, g_pal_rows;

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
        }
        return 0;
    case WM_TIMER:
        refresh_cpu();
        refresh_video();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        /* the screen render sits to the right of the video-text edit */
        astrovid_snapshot snap;
        astrovid_snapshot_get(&snap);
        int w, h;
        astrovid_render_screen(&snap, g_scratch, &w, &h);
        blit(hdc, 470, 300, 352, 240, g_scratch, w, h);
        int rows = astrovid_render_palette(&snap, g_scratch, ASTROVID_PAL_COLS,
                                           ASTROVID_PAL_CELL);
        blit(hdc, 470, 550, ASTROVID_PAL_COLS * 8, rows * 4, g_scratch,
             ASTROVID_PAL_COLS * ASTROVID_PAL_CELL, rows * ASTROVID_PAL_CELL);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        KillTimer(hwnd, 3);
        astrodebug_set_engaged(g_dbg, 0);
        DestroyWindow(hwnd);
        g_win = NULL;
        return 0;
    }
    return DefWindowProc(hwnd, msg, w, l);
}

void astro_debugger_show(HWND parent)
{
    if (g_win) { SetForegroundWindow(g_win); return; }
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
        CW_USEDEFAULT, CW_USEDEFAULT, 860, 820, parent, NULL, inst, NULL);

    const char *btns[] = { "Pause/Resume", "Step", "Step Over", "Step Out" };
    for (int i = 0; i < 4; i++)
        CreateWindowA("BUTTON", btns[i], WS_CHILD | WS_VISIBLE,
            8 + i * 130, 8, 124, 28, g_win, (HMENU)(intptr_t)(ID_PAUSE + i), inst, NULL);

    HFONT mono = (HFONT)GetStockObject(ANSI_FIXED_FONT);
    g_cpu_edit = CreateWindowA("EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 44, 440, 740, g_win, NULL, inst, NULL);
    SendMessage(g_cpu_edit, WM_SETFONT, (WPARAM)mono, TRUE);
    g_vid_edit = CreateWindowA("EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        460, 44, 380, 240, g_win, NULL, inst, NULL);
    SendMessage(g_vid_edit, WM_SETFONT, (WPARAM)mono, TRUE);

    ShowWindow(g_win, SW_SHOW);
    SetTimer(g_win, 3, 200, NULL);
}
