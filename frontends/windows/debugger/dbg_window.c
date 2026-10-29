/*
 * The Win32 debugger window, three tabs as on GNOME and KDE:
 *
 *  Z80 -- a control row (Pause/Resume, Step, Step Over, Step Out and the
 *      state) over two monospace views side by side: the registers and the
 *      disassembly from PC, and memory in hex. Each shows as many lines as
 *      its view has room for, so a taller window shows more of both.
 *  Video chip -- the live screen, the screen RAM bitmap and the 512-pen
 *      palette, and every register the Nutting manual documents. The
 *      registers view is sized to its text (measured in its font) and the
 *      pictures take the rest at the largest scale that fits, wrapping as
 *      the window narrows; when it is too narrow for both side by side, the
 *      pictures go above the registers.
 *  FujiNet -- every FujiBus packet between the console and FujiNet, both
 *      ways, with hex (always traced, from session start), beside the
 *      cartridge's link and mailbox state.
 *
 * Every view keeps its scroll position across refreshes and is redrawn only
 * when its text changes. Everything is laid out in 96-dpi units at the
 * window's DPI and laid out again on WM_SIZE and WM_DPICHANGED. Refreshed on
 * a WM_TIMER poll over the shared astrodebug/astrovid engine.
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
#include "win_common.h"

#define ID_PAUSE 1
#define ID_STEP 2
#define ID_OVER 3
#define ID_OUT 4
#define ID_TABS 5
#define ID_FUJI_CLEAR 6
#define TRACE_MAX 512
#define MAX_DISASM 96
#define MAX_MEM_ROWS 256
#define MEM_COLS 16
#define CPU_COLS 44             /* the widest register/disassembly line */
#define MEM_TEXT_COLS 53        /* "0000 " + 16 x "00 " */
#define MONO_PX 13
#define M 8                     /* margin, 96-dpi units */
#define GAP 10
#define LABEL_H 18

enum { PAGE_CPU, PAGE_VIDEO, PAGE_FUJINET, PAGE_COUNT };

static HWND g_win, g_tabs;
/* Z80 */
static HWND g_btn[4], g_status, g_cpu_edit, g_mem_edit;
/* Video chip */
static HWND g_vid_label, g_vid_edit, g_pic_label[3];
static RECT g_pic_rc[3];        /* screen, screen RAM, palette */
/* FujiNet */
static HWND g_fuji_title, g_fuji_note, g_fuji_clear, g_fuji_text;
static HWND g_state_title, g_state_text;

static HFONT g_mono, g_ui, g_bold;
static SIZE g_cell = { 8, 16 };
static int g_vid_cols = 76, g_vid_lines = 24;
static astrodebug *g_dbg;
static astrosession *g_session;
static uint32_t g_px[3][ASTROVID_MAX_PIXELS];
static int g_px_w[3], g_px_h[3];
static int g_page;
static int g_fuji_count = -1, g_fuji_ticks;
static uint64_t g_fuji_last, g_fuji_serial;

static int S(int v) { return astro_scale(v, astro_window_dpi(g_win)); }

/* ---- the views' text ---- */

/* the text each view shows now, so an unchanged refresh is skipped */
static struct { HWND h; char *text; } g_shown[8];

static int unchanged(HWND h, const char *text)
{
    int free_slot = -1;
    for (int i = 0; i < (int)(sizeof g_shown / sizeof g_shown[0]); i++) {
        if (g_shown[i].h == h) {
            if (g_shown[i].text && strcmp(g_shown[i].text, text) == 0)
                return 1;
            free(g_shown[i].text);
            g_shown[i].text = _strdup(text);
            return 0;
        }
        if (!g_shown[i].h && free_slot < 0)
            free_slot = i;
    }
    if (free_slot >= 0) {
        g_shown[free_slot].h = h;
        g_shown[free_slot].text = _strdup(text);
    }
    return 0;
}

static void forget_shown(void)
{
    for (int i = 0; i < (int)(sizeof g_shown / sizeof g_shown[0]); i++) {
        free(g_shown[i].text);
        g_shown[i].text = NULL;
        g_shown[i].h = NULL;
    }
}

/* EDIT controls want CR LF and draw control characters as boxes: engine
 * text (UTF-8) as the wide text a view shows. */
static wchar_t *wide_view_text(const char *text)
{
    size_t n = strlen(text), i, o = 0;
    char *buf = malloc(n * 2 + 1);
    wchar_t *w;
    if (!buf) return NULL;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n') { buf[o++] = '\r'; buf[o++] = '\n'; }
        else if (c >= 0x20 || c == '\t') buf[o++] = (char)c;
    }
    buf[o] = '\0';
    w = astro_wide_dup(buf);
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

/* How many whole lines a view shows. */
static int view_rows(HWND h)
{
    RECT c;
    GetClientRect(h, &c);
    return g_cell.cy > 0 && c.bottom > g_cell.cy ? c.bottom / g_cell.cy : 1;
}

/* A log follows its end while its reader is there. */
static int edit_at_end(HWND h)
{
    const int first = (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    const int count = (int)SendMessageW(h, EM_GETLINECOUNT, 0, 0);
    return first + view_rows(h) >= count;
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
static void set_text_at(HWND h, const char *text, int follow)
{
    if (unchanged(h, text))
        return;
    const int first = (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    const int hpos = GetScrollPos(h, SB_HORZ);
    wchar_t *w = wide_view_text(text);
    if (!w) return;
    SendMessageW(h, WM_SETREDRAW, FALSE, 0);
    SetWindowTextW(h, w);
    if (follow)
        SendMessageW(h, EM_LINESCROLL, 0, SendMessageW(h, EM_GETLINECOUNT, 0, 0));
    else
        SendMessageW(h, EM_LINESCROLL, 0, first - (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0));
    if (hpos > 0 && g_cell.cx > 0)
        SendMessageW(h, EM_LINESCROLL, hpos / g_cell.cx, 0);
    SendMessageW(h, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(h, NULL, TRUE);
    free(w);
}

/* ---- Z80 ---- */

static void refresh_cpu(void)
{
    static char out[16384];
    size_t p = 0;
    const int paused = astrodebug_is_paused(g_dbg);

    SetWindowTextW(g_status, paused ? L"Paused" : L"Running");
    if (!paused) {
        set_text_at(g_cpu_edit, "(running \xe2\x80\x94 Pause to inspect)", 0);
        set_text_at(g_mem_edit, "(running \xe2\x80\x94 Pause to inspect)", 0);
        return;
    }
    astrodebug_regs r;
    if (!astrodebug_regs_get(g_dbg, &r))
        return;
    p = addf(out, p, sizeof out,
        "AF %04X  BC %04X  DE %04X  HL %04X\n"
        "AF'%04X  BC'%04X  DE'%04X  HL'%04X\n"
        "IX %04X  IY %04X  SP %04X  PC %04X\n"
        "I %02X R %02X IM %d IFF1 %d IFF2 %d %s WZ %04X\n\n",
        r.af, r.bc, r.de, r.hl, r.af2, r.bc2, r.de2, r.hl2,
        r.ix, r.iy, r.sp, r.pc, r.i, r.r, r.im, r.iff1, r.iff2,
        r.halt ? "HALT" : "", r.wz);

    /* as many instructions as the view has lines under the registers */
    int want = view_rows(g_cpu_edit) - 5;
    if (want < 4) want = 4;
    if (want > MAX_DISASM) want = MAX_DISASM;
    astrodebug_dasm_line lines[MAX_DISASM];
    int n = astrodebug_disassemble(g_dbg, r.pc, lines, want);
    for (int i = 0; i < n; i++)
        p = addf(out, p, sizeof out, "%c %04X  %s\n",
                 i == 0 ? '>' : astrodebug_breakpoint_is_set(g_dbg, lines[i].addr) ? '*' : ' ',
                 lines[i].addr, lines[i].text);
    set_text_at(g_cpu_edit, out, 0);

    /* memory: as many rows as the view shows */
    static char mem[MAX_MEM_ROWS * 64];
    size_t q = 0;
    int rows = view_rows(g_mem_edit);
    if (rows > MAX_MEM_ROWS) rows = MAX_MEM_ROWS;
    for (int row = 0; row < rows; row++) {
        uint16_t base = (uint16_t)(row * MEM_COLS);
        uint8_t bytes[MEM_COLS];
        astrodebug_read(g_dbg, base, bytes, MEM_COLS);
        q = addf(mem, q, sizeof mem, "%04X ", base);
        for (int c = 0; c < MEM_COLS; c++)
            q = addf(mem, q, sizeof mem, "%02X ", bytes[c]);
        q = addf(mem, q, sizeof mem, "\n");
    }
    set_text_at(g_mem_edit, mem, 0);
}

/* ---- Video chip ---- */

static void refresh_video(void)
{
    static astrovid_snapshot snap;
    char text[4096];
    astrovid_snapshot_get(&snap);
    astrovid_format_state(&snap, text, sizeof text);
    set_text_at(g_vid_edit, text, 0);

    astrovid_render_screen(&snap, g_px[0], &g_px_w[0], &g_px_h[0]);
    astrovid_render_bitmap(&snap, g_px[1], &g_px_w[1], &g_px_h[1]);
    g_px_h[2] = astrovid_render_palette(&snap, g_px[2], ASTROVID_PAL_COLS, ASTROVID_PAL_CELL) *
                ASTROVID_PAL_CELL;
    g_px_w[2] = ASTROVID_PAL_COLS * ASTROVID_PAL_CELL;
    for (int i = 0; i < 3; i++)
        InvalidateRect(g_win, &g_pic_rc[i], FALSE);
}

/* ---- FujiNet ---- */

static void refresh_fujinet(int force)
{
    static astrodebug_fuji_packet p[TRACE_MAX];
    static char t[TRACE_MAX * 320 + 128];
    char state[2048];
    size_t len = 0;
    int n = astrodebug_fujinet_trace(g_dbg, p, TRACE_MAX), follow;

    if (astrosession_fujinet_external(g_session)) {
        char note[160];
        snprintf(note, sizeof note, "External FujiNet at %s, %s.",
                 astrosession_boip_endpoint(g_session),
                 astrosession_fujinet_link(g_session) ? "connected" : "not connected");
        set_text_at(g_fuji_note, note, 0);
    } else if (astrosession_fujinet_link(g_session))
        SetWindowTextW(g_fuji_note, L"FujiNet cartridge, connected.");
    else if (astrosession_fujinet_running(g_session))
        SetWindowTextW(g_fuji_note, L"FujiNet cartridge, not connected.");
    else
        SetWindowTextW(g_fuji_note,
                       L"FujiNet cartridge, not connected (the FujiNet runtime is not running).");
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

/* ---- layout ---- */

/* The size an edit needs to show cols x lines of the monospace font with no
 * scrolling: the text, its margins, the client edge and both scroll bars. */
static SIZE edit_size(int cols, int lines)
{
    SIZE s;
    const int edge = 2 * GetSystemMetrics(SM_CXEDGE);
    s.cx = cols * g_cell.cx + S(8) + edge + GetSystemMetrics(SM_CXVSCROLL);
    s.cy = lines * g_cell.cy + S(4) + edge + GetSystemMetrics(SM_CYHSCROLL);
    return s;
}

/* The video registers' text, measured once in the view's font: its widest
 * line (with room for the counters to grow) and its line count. */
static void measure_video_text(void)
{
    static astrovid_snapshot snap;
    char text[4096];
    int cols = 0, lines = 1, run = 0;
    astrovid_snapshot_get(&snap);
    astrovid_format_state(&snap, text, sizeof text);
    for (const char *c = text; *c; c++) {
        if (*c == '\n') { lines++; run = 0; continue; }
        if (++run > cols) cols = run;
    }
    g_vid_cols = cols + 6;
    g_vid_lines = lines + 1;
}

/* Lays the three pictures out left to right, wrapping, at scale s inside
 * w x h (each with its label above). Returns whether they fit; fills rc. */
static int flow_pictures(double s, int x, int y, int w, int h, RECT rc[3])
{
    static const int nat_w[3] = { ASTROVID_SCREEN_W, ASTROVID_BITMAP_W,
                                  ASTROVID_PAL_COLS * ASTROVID_PAL_CELL };
    static const int nat_h[3] = { ASTROVID_SCREEN_H, ASTROVID_BITMAP_H,
                                  16 * ASTROVID_PAL_CELL };
    const int label = S(LABEL_H), gap = S(GAP);
    int cx = x, cy = y, row_h = 0;
    for (int i = 0; i < 3; i++) {
        const int pw = (int)(S(nat_w[i]) * s), ph = (int)(S(nat_h[i]) * s);
        if (cx > x && cx + pw > x + w) {        /* wrap */
            cx = x;
            cy += row_h + gap;
            row_h = 0;
        }
        if (pw > w)
            return 0;
        SetRect(&rc[i], cx, cy + label, cx + pw, cy + label + ph);
        cx += pw + gap;
        if (label + ph > row_h) row_h = label + ph;
    }
    return cy + row_h <= y + h;
}

/* The largest scale at which the pictures fit the area (at least a third
 * of their own size, so they stay legible). */
static void place_pictures(int x, int y, int w, int h)
{
    double lo = 0.33, hi = 4.0;
    RECT rc[3];
    if (!flow_pictures(lo, x, y, w, h, rc)) {
        flow_pictures(lo, x, y, w, 1 << 20, g_pic_rc);
        goto labels;
    }
    for (int i = 0; i < 24; i++) {
        double mid = (lo + hi) / 2;
        if (flow_pictures(mid, x, y, w, h, rc)) lo = mid;
        else hi = mid;
    }
    flow_pictures(lo, x, y, w, h, g_pic_rc);
labels:
    for (int i = 0; i < 3; i++)
        MoveWindow(g_pic_label[i], g_pic_rc[i].left, g_pic_rc[i].top - S(LABEL_H),
                   g_pic_rc[i].right - g_pic_rc[i].left, S(LABEL_H), TRUE);
}

static void layout(void)
{
    RECT c;
    GetClientRect(g_win, &c);
    const int m = S(M), gap = S(GAP);
    int top, w, h;

    /* the tab control is the strip alone: the pages sit on the window's
     * own background below it */
    top = m / 2 + astro_place_tab_strip(g_tabs, m, m / 2, c.right - 2 * m) + m;
    w = c.right - 2 * m;
    h = c.bottom - top - m;

    /* Z80: the controls, then the two views side by side (stacked when the
     * window is too narrow for both) */
    {
        int x = m, y = top;
        for (int i = 0; i < 4; i++) {
            MoveWindow(g_btn[i], x, y, S(i == 0 ? 112 : 92), S(28), TRUE);
            x += S(i == 0 ? 112 : 92) + S(6);
        }
        MoveWindow(g_status, x + S(6), y + S(6), S(120), S(20), TRUE);
        y += S(28) + S(8);
        const int vh = top + h - y;
        const SIZE cpu = edit_size(CPU_COLS, 1), mem = edit_size(MEM_TEXT_COLS, 1);
        if (w >= cpu.cx + gap + mem.cx || w >= cpu.cx + gap + S(160)) {
            MoveWindow(g_cpu_edit, m, y, cpu.cx, vh, TRUE);
            MoveWindow(g_mem_edit, m + cpu.cx + gap, y, w - cpu.cx - gap, vh, TRUE);
        } else {
            MoveWindow(g_cpu_edit, m, y, w, (vh - gap) * 3 / 5, TRUE);
            MoveWindow(g_mem_edit, m, y + (vh - gap) * 3 / 5 + gap, w, vh - (vh - gap) * 3 / 5 - gap, TRUE);
        }
    }

    /* Video chip: the registers at their own size, the pictures in the rest */
    {
        const SIZE need = edit_size(g_vid_cols, g_vid_lines);
        const int label = S(LABEL_H);
        if (w - need.cx - gap >= S(220)) {
            const int rh = need.cy < h - label ? need.cy : h - label;
            MoveWindow(g_vid_label, m, top, need.cx, label, TRUE);
            MoveWindow(g_vid_edit, m, top + label, need.cx, rh, TRUE);
            place_pictures(m + need.cx + gap, top, w - need.cx - gap, h);
        } else {
            int rh = need.cy;
            if (rh > h / 2) rh = h / 2;
            place_pictures(m, top, w, h - rh - label - gap);
            MoveWindow(g_vid_label, m, top + h - rh - label, w, label, TRUE);
            MoveWindow(g_vid_edit, m, top + h - rh, w, rh, TRUE);
        }
    }

    /* FujiNet: the packets left (a note and Clear over them), the
     * cartridge right */
    {
        const int rw = S(300), lw = w - rw - S(12);
        MoveWindow(g_fuji_title, m, top, lw, S(18), TRUE);
        MoveWindow(g_fuji_clear, m + lw - S(80), top + S(22), S(80), S(26), TRUE);
        MoveWindow(g_fuji_note, m, top + S(26), lw - S(88), S(18), TRUE);
        MoveWindow(g_fuji_text, m, top + S(54), lw, h - S(54), TRUE);
        MoveWindow(g_state_title, m + lw + S(12), top, rw, S(18), TRUE);
        MoveWindow(g_state_text, m + lw + S(12), top + S(22), rw, h - S(22), TRUE);
    }
    /* every child too: a moved label must not leave its old pixels behind */
    RedrawWindow(g_win, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

/* The window size the content wants: both Z80 views side by side, the
 * video registers beside its pictures at about their own size, the FujiNet
 * page's two columns -- within the monitor's work area. */
static SIZE wanted_client(void)
{
    const SIZE cpu = edit_size(CPU_COLS, 26), mem = edit_size(MEM_TEXT_COLS, 1);
    const SIZE vid = edit_size(g_vid_cols, g_vid_lines);
    RECT item;
    int strip = S(24), chrome, ph, pw;
    SIZE s;

    if (SendMessageW(g_tabs, TCM_GETITEMRECT, 0, (LPARAM)&item))
        strip = item.bottom + 1;
    chrome = S(M) / 2 + strip + S(M) * 2;
    s.cy = chrome + S(36) + cpu.cy;
    if (chrome + S(LABEL_H) + vid.cy > s.cy)
        s.cy = chrome + S(LABEL_H) + vid.cy;

    /* the pictures in one column beside the registers, as tall as they are */
    ph = s.cy - chrome - 3 * S(LABEL_H) - 2 * S(GAP);
    pw = (int)((double)ph * (ASTROVID_PAL_COLS * ASTROVID_PAL_CELL) /
               (ASTROVID_SCREEN_H + ASTROVID_BITMAP_H + 16 * ASTROVID_PAL_CELL));
    s.cx = 2 * S(M) + cpu.cx + S(GAP) + mem.cx;
    if (2 * S(M) + vid.cx + S(GAP) + pw + S(4) > s.cx)
        s.cx = 2 * S(M) + vid.cx + S(GAP) + pw + S(4);
    if (s.cx < S(860)) s.cx = S(860);       /* the FujiNet page's two columns */
    return s;
}

static void fit_to_content(const RECT *suggested)
{
    SIZE want = wanted_client(), outer = astro_outer_size(g_win, want.cx, want.cy, FALSE);
    MONITORINFO mi;
    RECT at;
    mi.cbSize = sizeof mi;
    GetWindowRect(g_win, &at);
    if (suggested) at = *suggested;
    if (GetMonitorInfoW(MonitorFromWindow(g_win, MONITOR_DEFAULTTONEAREST), &mi)) {
        const RECT *wa = &mi.rcWork;
        if (outer.cx > wa->right - wa->left) outer.cx = wa->right - wa->left;
        if (outer.cy > wa->bottom - wa->top) outer.cy = wa->bottom - wa->top;
        if (at.left + outer.cx > wa->right) at.left = wa->right - outer.cx;
        if (at.top + outer.cy > wa->bottom) at.top = wa->bottom - outer.cy;
        if (at.left < wa->left) at.left = wa->left;
        if (at.top < wa->top) at.top = wa->top;
    }
    SetWindowPos(g_win, NULL, at.left, at.top, outer.cx, outer.cy, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void make_fonts(void)
{
    const int dpi = astro_window_dpi(g_win);
    if (g_mono) DeleteObject(g_mono);
    if (g_ui) DeleteObject(g_ui);
    if (g_bold) DeleteObject(g_bold);
    g_mono = astro_make_font(dpi, MONO_PX, FW_NORMAL, ASTRO_MONO_FACE);
    g_ui = astro_make_font(dpi, 0, FW_NORMAL, NULL);
    g_bold = astro_make_font(dpi, 0, FW_BOLD, NULL);
    astro_set_font_all(g_win, g_ui);
    HWND mono[] = { g_cpu_edit, g_mem_edit, g_vid_edit, g_fuji_text, g_state_text };
    for (size_t i = 0; i < sizeof mono / sizeof mono[0]; i++)
        SendMessageW(mono[i], WM_SETFONT, (WPARAM)g_mono, TRUE);
    HWND bold[] = { g_vid_label, g_pic_label[0], g_pic_label[1], g_pic_label[2],
                    g_fuji_title, g_state_title };
    for (size_t i = 0; i < sizeof bold / sizeof bold[0]; i++)
        SendMessageW(bold[i], WM_SETFONT, (WPARAM)g_bold, TRUE);
    g_cell = astro_font_cell(g_win, g_mono);
    forget_shown();         /* re-measure: line counts follow the font */
}

static void show_page(int page)
{
    HWND cpu[] = { g_btn[0], g_btn[1], g_btn[2], g_btn[3], g_status, g_cpu_edit, g_mem_edit };
    HWND vid[] = { g_vid_label, g_vid_edit, g_pic_label[0], g_pic_label[1], g_pic_label[2] };
    HWND fuji[] = { g_fuji_title, g_fuji_note, g_fuji_clear, g_fuji_text,
                    g_state_title, g_state_text };
    g_page = page;
    for (size_t i = 0; i < sizeof cpu / sizeof cpu[0]; i++)
        ShowWindow(cpu[i], page == PAGE_CPU ? SW_SHOW : SW_HIDE);
    for (size_t i = 0; i < sizeof vid / sizeof vid[0]; i++)
        ShowWindow(vid[i], page == PAGE_VIDEO ? SW_SHOW : SW_HIDE);
    for (size_t i = 0; i < sizeof fuji / sizeof fuji[0]; i++)
        ShowWindow(fuji[i], page == PAGE_FUJINET ? SW_SHOW : SW_HIDE);
    if (page == PAGE_CPU) refresh_cpu();
    else if (page == PAGE_VIDEO) refresh_video();
    else refresh_fujinet(0);
    InvalidateRect(g_win, NULL, TRUE);      /* the blits come and go */
}

static void blit(HDC hdc, const RECT *r, const uint32_t *px, int w, int h)
{
    BITMAPINFO bmi;
    if (w <= 0 || h <= 0 || r->right <= r->left) return;
    ZeroMemory(&bmi, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(hdc, COLORONCOLOR);
    StretchDIBits(hdc, r->left, r->top, r->right - r->left, r->bottom - r->top, 0, 0, w, h,
                  px, &bmi, DIB_RGB_COLORS, SRCCOPY);
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
            show_page((int)SendMessageW(g_tabs, TCM_GETCURSEL, 0, 0));
        return 0;
    }
    case WM_SIZE:
        if (g_tabs) {
            layout();
            forget_shown();         /* line counts follow the size */
        }
        return 0;
    case WM_GETMINMAXINFO: {
        const int dpi = astro_window_dpi(hwnd);
        ((MINMAXINFO *)l)->ptMinTrackSize.x = astro_scale(640, dpi);
        ((MINMAXINFO *)l)->ptMinTrackSize.y = astro_scale(440, dpi);
        return 0;
    }
    case WM_DPICHANGED:
        make_fonts();
        fit_to_content((const RECT *)l);
        layout();
        return 0;
    case WM_CTLCOLORSTATIC:
        /* the read-only views: the window colour, as text views */
        if ((HWND)l == g_cpu_edit || (HWND)l == g_mem_edit || (HWND)l == g_vid_edit ||
            (HWND)l == g_fuji_text || (HWND)l == g_state_text) {
            SetTextColor((HDC)w, GetSysColor(COLOR_WINDOWTEXT));
            SetBkColor((HDC)w, GetSysColor(COLOR_WINDOW));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        break;
    case WM_TIMER:
        if (g_page == PAGE_CPU) {
            refresh_cpu();
        } else if (g_page == PAGE_VIDEO) {
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
        if (g_page == PAGE_VIDEO)
            for (int i = 0; i < 3; i++)
                blit(hdc, &g_pic_rc[i], g_px[i], g_px_w[i], g_px_h[i]);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        KillTimer(hwnd, 3);
        astrodebug_set_engaged(g_dbg, 0);
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_win = NULL;
        g_tabs = NULL;
        forget_shown();
        if (g_mono) DeleteObject(g_mono);
        if (g_ui) DeleteObject(g_ui);
        if (g_bold) DeleteObject(g_bold);
        g_mono = g_ui = g_bold = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

static HWND child(const wchar_t *cls, const wchar_t *text, DWORD style, DWORD ex, int id)
{
    return CreateWindowExW(ex, cls, text, WS_CHILD | style, 0, 0, 1, 1, g_win,
                           (HMENU)(intptr_t)id, GetModuleHandleW(NULL), NULL);
}

static HWND view(DWORD extra)
{
    HWND h = child(L"EDIT", L"", WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOHSCROLL |
                   ES_AUTOVSCROLL | ES_READONLY | extra, WS_EX_CLIENTEDGE, 0);
    SendMessageW(h, EM_SETLIMITTEXT, 0, 0);
    return h;
}

void astro_debugger_show(HWND parent, astrosession *session)
{
    if (g_win) { SetForegroundWindow(g_win); return; }
    g_session = session;
    g_dbg = astrodebug_get();
    astrodebug_set_engaged(g_dbg, 1);

    static int registered;
    HINSTANCE inst = GetModuleHandleW(NULL);
    if (!registered) {
        WNDCLASSW wc; ZeroMemory(&wc, sizeof wc);
        wc.lpfnWndProc = dbg_proc; wc.hInstance = inst;
        wc.lpszClassName = L"AstroDbg";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassW(&wc);
        registered = 1;
    }
    g_win = CreateWindowW(L"AstroDbg", L"Debugger", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 700, parent, NULL, inst, NULL);

    /* The tab control is the strip alone: the pages sit on the window's own
     * background below it. */
    g_tabs = child(WC_TABCONTROLW, L"", WS_VISIBLE | WS_CLIPSIBLINGS, 0, ID_TABS);
    const wchar_t *tabs[PAGE_COUNT] = { L"Z80", L"Video chip", L"FujiNet" };
    TCITEMW item;
    memset(&item, 0, sizeof item);
    item.mask = TCIF_TEXT;
    for (int i = 0; i < PAGE_COUNT; i++) {
        item.pszText = (wchar_t *)tabs[i];
        SendMessageW(g_tabs, TCM_INSERTITEMW, (WPARAM)i, (LPARAM)&item);
    }

    /* Z80 */
    const wchar_t *btns[] = { L"Pause/Resume", L"Step", L"Step Over", L"Step Out" };
    for (int i = 0; i < 4; i++)
        g_btn[i] = child(L"BUTTON", btns[i], 0, 0, ID_PAUSE + i);
    g_status = child(L"STATIC", L"Running", SS_LEFT, 0, 0);
    g_cpu_edit = view(0);
    g_mem_edit = view(0);

    /* Video chip */
    g_vid_label = child(L"STATIC", L"Registers", SS_LEFT, 0, 0);
    g_vid_edit = view(0);
    g_pic_label[0] = child(L"STATIC", L"Screen", SS_LEFT, 0, 0);
    g_pic_label[1] = child(L"STATIC", L"Screen RAM", SS_LEFT, 0, 0);
    g_pic_label[2] = child(L"STATIC", L"Palette (512 pens)", SS_LEFT | SS_ENDELLIPSIS, 0, 0);

    /* FujiNet */
    g_fuji_title = child(L"STATIC", L"FujiBus packets (SLIP-decoded)", SS_LEFT, 0, 0);
    g_fuji_note = child(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 0, 0);
    g_fuji_clear = child(L"BUTTON", L"Clear", 0, 0, ID_FUJI_CLEAR);
    g_fuji_text = view(0);
    g_state_title = child(L"STATIC", L"FujiNet cartridge", SS_LEFT, 0, 0);
    g_state_text = view(0);

    /* the tab body under the pages: its repaints never cover them */
    SetWindowPos(g_tabs, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    make_fonts();
    measure_video_text();
    fit_to_content(NULL);
    layout();

    /* ASTRO_DEBUGGER_TAB: a page number to open on (0 Z80, 1 Video chip,
     * 2 FujiNet -- the GNOME page numbers) */
    const char *tab = getenv("ASTRO_DEBUGGER_TAB");
    int page = tab ? atoi(tab) : PAGE_CPU;
    if (page < 0 || page >= PAGE_COUNT) page = PAGE_CPU;
    g_fuji_count = -1;
    SendMessageW(g_tabs, TCM_SETCURSEL, (WPARAM)page, 0);
    show_page(page);

    ShowWindow(g_win, SW_SHOW);
    SetTimer(g_win, 3, 200, NULL);
}
