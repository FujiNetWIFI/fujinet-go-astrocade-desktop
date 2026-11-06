/*
 * The Win32 Controllers window: the console's 24-key calculator keypad, the
 * two system buttons and all four hand controllers (stick, trigger, knob
 * dial), with Map mode. One window, every control painted by hand (double
 * buffered) and hit-tested here, so a press and a release are real mouse
 * down/up (a Win32 BUTTON only reports a click) and a control is HELD for as
 * long as the mouse is down. The same design as the GNOME and KDE windows.
 *
 * FIXED SIZE and floating: a tool window with no thick frame and no maximize
 * box, sized in 96-dpi units and scaled to the window's DPI. Hidden at
 * startup; F9 or View > Controllers shows it.
 *
 * What the machine sees held (astrosession_target_held: keyboard, pads and
 * this window together) is painted in the accent colour, polled every 50 ms
 * while visible; the dials show the live knob.
 *
 * MAP MODE: Map arms a two-step rebind -- click a control to pick the target,
 * then press a key OR a gamepad button / axis (a dial takes an axis). While
 * armed, clicks pick instead of injecting input and keys go to Map, not the
 * machine. Leaving Map, or hiding the window, cancels the capture.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "keypad_window.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bindings.h"
#include "../resource.h"
#include "../win_common.h"

#define M_PI_D 3.14159265358979323846

#define ACCENT RGB(181, 52, 44)
#define GOLD RGB(232, 193, 74)
#define GOLD_TEXT RGB(58, 44, 0)

/* ---- layout, in 96-dpi units ---------------------------------------------- */

#define KEY_W 62
#define KEY_H 40
#define KEY_GAP 6
#define COL_X 12
#define COL_W 290
#define PL_X 314
#define PL_W 208
#define PL_H 320
#define PL_GAP 12
#define CLIENT_W (PL_X + 2 * PL_W + PL_GAP + 12)
#define CLIENT_H (12 + 2 * PL_H + PL_GAP + 12)

enum { SP_NONE = 0, SP_MAP, SP_CLEAR, SP_DEFAULTS, SP_DIAL };

typedef struct {
    RECT r;                  /* 96-dpi units */
    int target;              /* binding target, or -1 */
    int special;
    int player;              /* SP_DIAL */
    wchar_t face[24];
    int lit;
} control;

static control g_ctl[ASTRO_TARGET_COUNT + 8];
static int g_nctl;
static RECT g_pads_text, g_hint_text;     /* 96-dpi units */
static RECT g_frames[1 + ASTROSESSION_PLAYERS];
static const wchar_t *g_titles[1 + ASTROSESSION_PLAYERS] = {
    L"Console keypad", L"Player 1", L"Player 2", L"Player 3", L"Player 4",
};

static HWND g_window, g_owner;
static astrosession *g_session;
static HFONT g_font, g_font_bold, g_font_small;
static int g_dpi = 96;
static int g_map_state = -2;      /* -2 idle, -1 armed, >= 0 a picked target */
static int g_down = -1;           /* the control held by the mouse, or -1 */
static int g_dial_drag = -1;
static wchar_t g_hint[160], g_pads[256];
static int g_last_knob[ASTROSESSION_PLAYERS] = { -1, -1, -1, -1 };

#define TIMER_POLL 7001

static const wchar_t *const kFaces[ASTROSESSION_KEY_COUNT] = {
    L"C",  L"\x2191", L"\x2193", L"%",
    L"MR", L"MS",     L"CH",     L"\x00F7",
    L"7",  L"8",      L"9",      L"\x00D7",
    L"4",  L"5",      L"6",      L"\x2212",
    L"1",  L"2",      L"3",      L"+",
    L"CE", L"0",      L".",      L"=",
};

static int is_knob_target(int t)
{
    return t >= ASTRO_TARGET_PLAYERS &&
           (t - ASTRO_TARGET_PLAYERS) % ASTRO_ACT_PER_PLAYER == ASTRO_ACT_KNOB;
}

static void add(int x, int y, int w, int h, int target, int special, int player,
                const wchar_t *face)
{
    control *c = &g_ctl[g_nctl++];
    memset(c, 0, sizeof *c);
    c->r.left = x; c->r.top = y; c->r.right = x + w; c->r.bottom = y + h;
    c->target = target;
    c->special = special;
    c->player = player;
    if (face) wcsncpy(c->face, face, 23);
}

static void build_controls(void)
{
    static const wchar_t *const arrows[4] = { L"\x25B2", L"\x25C0", L"\x25B6", L"\x25BC" };
    int p;

    g_nctl = 0;
    /* the keypad frame */
    g_frames[0].left = COL_X; g_frames[0].top = 12;
    g_frames[0].right = COL_X + COL_W; g_frames[0].bottom = 382;
    for (int k = 0; k < ASTROSESSION_KEY_COUNT; k++)
        add(COL_X + 12 + (k % 4) * (KEY_W + KEY_GAP), 46 + (k / 4) * (KEY_H + KEY_GAP),
            KEY_W, KEY_H, ASTRO_TARGET_KEYPAD + k, SP_NONE, 0, kFaces[k]);
    add(COL_X + 12, 326, 119, 44, ASTRO_TARGET_SYSACT + ASTROSESSION_SYSACT_RESET_GAME,
        SP_NONE, 0, L"Reset Game");
    add(COL_X + 12 + 119 + 6, 326, 119, 44,
        ASTRO_TARGET_SYSACT + ASTROSESSION_SYSACT_RESET_CONFIG, SP_NONE, 0,
        L"Reset to CONFIG");

    /* pads text, Map row, hint */
    g_pads_text.left = COL_X; g_pads_text.top = 392;
    g_pads_text.right = COL_X + COL_W; g_pads_text.bottom = 428;
    add(COL_X, 436, 72, 30, -1, SP_MAP, 0, L"Map");
    add(COL_X + 78, 436, 72, 30, -1, SP_CLEAR, 0, L"Clear");
    add(COL_X + 156, 436, 84, 30, -1, SP_DEFAULTS, 0, L"Defaults");
    g_hint_text.left = COL_X; g_hint_text.top = 474;
    g_hint_text.right = COL_X + COL_W; g_hint_text.bottom = 514;

    for (p = 0; p < ASTROSESSION_PLAYERS; p++) {
        const int fx = PL_X + (p % 2) * (PL_W + PL_GAP);
        const int fy = 12 + (p / 2) * (PL_H + PL_GAP);
        const int gx = fx + 22, gy = fy + 32;
        g_frames[1 + p].left = fx; g_frames[1 + p].top = fy;
        g_frames[1 + p].right = fx + PL_W; g_frames[1 + p].bottom = fy + PL_H;
        add(gx + 56, gy, 52, 34, ASTRO_TARGET_PLAYER(p, ASTRO_ACT_UP), SP_NONE, p, arrows[0]);
        add(gx, gy + 38, 52, 34, ASTRO_TARGET_PLAYER(p, ASTRO_ACT_LEFT), SP_NONE, p, arrows[1]);
        add(gx + 112, gy + 38, 52, 34, ASTRO_TARGET_PLAYER(p, ASTRO_ACT_RIGHT), SP_NONE, p, arrows[2]);
        add(gx + 56, gy + 76, 52, 34, ASTRO_TARGET_PLAYER(p, ASTRO_ACT_DOWN), SP_NONE, p, arrows[3]);
        add(gx, gy + 116, 164, 40, ASTRO_TARGET_PLAYER(p, ASTRO_ACT_TRIGGER), SP_NONE, p, L"Trigger");
        add(fx + 12, gy + 168 + 29, 40, 34, ASTRO_TARGET_PLAYER(p, ASTRO_ACT_KNOB_DEC), SP_NONE, p, arrows[1]);
        add(fx + 58, gy + 168, 92, 92, ASTRO_TARGET_PLAYER(p, ASTRO_ACT_KNOB), SP_DIAL, p, L"Knob");
        add(fx + PL_W - 12 - 40, gy + 168 + 29, 40, 34, ASTRO_TARGET_PLAYER(p, ASTRO_ACT_KNOB_INC), SP_NONE, p, arrows[2]);
    }
}

static RECT scaled(const RECT *r)
{
    RECT o;
    o.left = astro_scale(r->left, g_dpi); o.top = astro_scale(r->top, g_dpi);
    o.right = astro_scale(r->right, g_dpi); o.bottom = astro_scale(r->bottom, g_dpi);
    return o;
}

static void set_hint(const char *utf8)
{
    wchar_t *w = astro_wide_dup(utf8);
    if (w) { wcsncpy(g_hint, w, 159); g_hint[159] = 0; free(w); }
    if (g_window) InvalidateRect(g_window, NULL, FALSE);
}

static void set_map_state(int state)
{
    char msg[160];
    g_map_state = state;
    if (state >= 0) {
        snprintf(msg, sizeof msg,
                 is_knob_target(state) ? "Move a gamepad stick or trigger for %s"
                                       : "Press a key or gamepad button for %s",
                 bindings_target_label(state));
        set_hint(msg);
        astrosession_gamepad_capture_begin(g_session);
    } else {
        astrosession_gamepad_capture_cancel(g_session);
        set_hint(state == -1 ? "Click a control to remap" : "");
    }
    if (g_window) InvalidateRect(g_window, NULL, FALSE);
}

/* ---- pressing ----------------------------------------------------------- */

static void press(int target, int down)
{
    if (target >= ASTRO_TARGET_SYSACT && target < ASTRO_TARGET_PLAYERS) {
        if (!down)   /* system actions fire on release, like a real button */
            astrosession_sysaction_fire(g_session,
                (astrosession_sysaction)(target - ASTRO_TARGET_SYSACT));
        return;
    }
    astrosession_press(g_session, target, down);
}

static int hit(int x, int y)
{
    for (int i = 0; i < g_nctl; i++) {
        const RECT r = scaled(&g_ctl[i].r);
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom)
            return i;
    }
    return -1;
}

static void dial_drag_to(int i, int x, int y)
{
    const RECT r = scaled(&g_ctl[i].r);
    const double rad = (r.right - r.left) / 2.0;
    const int v = astrosession_knob_from_point(x - (r.left + r.right) / 2.0,
                                               y - (r.top + r.bottom) / 2.0, rad);
    if (v >= 0)
        astrosession_knob_set(g_session, g_ctl[i].player, (uint8_t)v);
    InvalidateRect(g_window, NULL, FALSE);
}

static void release_mouse_hold(BOOL inside)
{
    if (g_down >= 0) {
        const control *c = &g_ctl[g_down];
        if (c->special == SP_NONE && c->target >= 0) {
            const int sys = c->target >= ASTRO_TARGET_SYSACT && c->target < ASTRO_TARGET_PLAYERS;
            if (!sys || inside) press(c->target, 0);
        }
    }
    g_down = -1;
    g_dial_drag = -1;
}

/* ---- Map mode ----------------------------------------------------------- */

static void on_special(int special)
{
    char msg[128];
    if (special == SP_MAP) {
        set_map_state(g_map_state == -2 ? -1 : -2);
    } else if (special == SP_CLEAR && g_map_state >= 0) {
        const int t = g_map_state;
        bindings_clear(g_session, t);
        set_map_state(-1);
        snprintf(msg, sizeof msg, "%s cleared", bindings_target_label(t));
        set_hint(msg);
    } else if (special == SP_DEFAULTS) {
        bindings_reset_defaults(g_session);
        set_hint("Bindings reset to the defaults");
    }
    InvalidateRect(g_window, NULL, FALSE);
}

static void capture_poll(void)
{
    int id, from = -1;
    const int target = g_map_state;
    char msg[200], name[48];
    if (!astrosession_gamepad_capture_poll(g_session, &id))
        return;
    if (is_knob_target(target) && id < ASTRO_PAD_AXIS_BASE) {
        astrosession_gamepad_capture_begin(g_session);   /* a button cannot turn a knob */
        set_hint("The knob follows an axis: move a stick or trigger");
        return;
    }
    bindings_set_pad(g_session, target, id, &from);
    astrosession_pad_input_name(id, name, sizeof name);
    if (from >= 0)
        snprintf(msg, sizeof msg, "%s = pad %s (was %s)", bindings_target_label(target),
                 name, bindings_target_label(from));
    else
        snprintf(msg, sizeof msg, "%s = pad %s", bindings_target_label(target), name);
    set_map_state(-1);   /* stay armed: remapping several in a row is normal */
    set_hint(msg);
}

static void update_pads(void)
{
    char all[256] = "", name[64];
    const int n = astrosession_gamepad_count(g_session);
    wchar_t *w;
    for (int i = 0; i < n; i++) {
        astrosession_gamepad_name(g_session, i, name, sizeof name);
        snprintf(all + strlen(all), sizeof all - strlen(all), "%sPlayer %d: %s",
                 i ? "   " : "", i + 1, name);
    }
    if (!n) snprintf(all, sizeof all, "No gamepad connected");
    w = astro_wide_dup(all);
    if (w) {
        if (wcscmp(w, g_pads)) { wcsncpy(g_pads, w, 255); g_pads[255] = 0; InvalidateRect(g_window, NULL, FALSE); }
        free(w);
    }
}

static void poll(void)
{
    int changed = 0;
    update_pads();
    if (g_map_state >= 0) { capture_poll(); return; }
    for (int p = 0; p < ASTROSESSION_PLAYERS; p++) {
        const int k = astrosession_knob_get(g_session, p);
        if (k != g_last_knob[p]) { g_last_knob[p] = k; changed = 1; }
    }
    if (g_map_state == -2)
        for (int i = 0; i < g_nctl; i++) {
            const control *c = &g_ctl[i];
            int lit;
            if (c->special != SP_NONE || c->target < 0) continue;
            lit = astrosession_target_held(g_session, c->target) != 0;
            if (lit != c->lit) { g_ctl[i].lit = lit; changed = 1; }
        }
    if (changed) InvalidateRect(g_window, NULL, FALSE);
}

/* ---- keyboard ----------------------------------------------------------- */

static uint32_t keysym_from_lparam(LPARAM lparam)
{
    return astrosession_keysym_from_win_scancode((unsigned)((lparam >> 16) & 0xff),
                                                 (int)((lparam >> 24) & 1));
}

static BOOL map_key(uint32_t keysym)
{
    char name[48], msg[200];
    int from = -1;
    const int target = g_map_state;
    if (target == -2)
        return FALSE;
    if (target == -1)
        return TRUE;     /* swallow keys while choosing a target */
    if (is_knob_target(target)) {
        set_hint("The knob follows a gamepad axis: move a stick or trigger");
        return TRUE;
    }
    bindings_set_keysym(g_session, target, keysym, &from);
    astrosession_keysym_name(keysym, name, sizeof name);
    if (from >= 0)
        snprintf(msg, sizeof msg, "%s = %s (was %s)", bindings_target_label(target), name,
                 bindings_target_label(from));
    else
        snprintf(msg, sizeof msg, "%s = %s", bindings_target_label(target), name);
    set_map_state(-1);   /* stay armed */
    set_hint(msg);
    return TRUE;
}

BOOL astro_controllers_key(astrosession *session, LPARAM lparam, BOOL down)
{
    const uint32_t ks = keysym_from_lparam(lparam);
    BOOL r;
    if (down && g_window && map_key(ks))
        return TRUE;
    r = astrosession_input_key(session, ks, down, GetKeyState(VK_CONTROL) < 0) != 0;
    astrosession_sysaction_drain(session);
    return r;
}

/* ---- painting ----------------------------------------------------------- */

static void fill(HDC dc, const RECT *r, COLORREF c, int round)
{
    HBRUSH b = CreateSolidBrush(c);
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pen);
    RoundRect(dc, r->left, r->top, r->right, r->bottom, round, round);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(pen);
}

static void text(HDC dc, const wchar_t *t, RECT *r, UINT flags, COLORREF c, HFONT f)
{
    HGDIOBJ old = SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, t, -1, r, flags);
    SelectObject(dc, old);
}

static void draw_dial(HDC dc, const control *c, int picked)
{
    const RECT r = scaled(&c->r);
    const double cx = (r.left + r.right) / 2.0, cy = (r.top + r.bottom) / 2.0;
    const double rad = (r.right - r.left) / 2.0 - astro_scale(4, g_dpi);
    const COLORREF fg = GetSysColor(COLOR_WINDOWTEXT);
    HPEN pen = CreatePen(PS_SOLID, astro_scale(2, g_dpi), fg);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    const double a = astrosession_knob_angle(astrosession_knob_get(g_session, c->player)) * M_PI_D / 180.0;
    const double r0 = rad * 0.62;
    HPEN hand;

    /* the 270 degree sweep, clockwise from the lower left to the lower right */
    Arc(dc, (int)(cx - rad), (int)(cy - rad), (int)(cx + rad), (int)(cy + rad),
        (int)(cx + rad * cos(45 * M_PI_D / 180)), (int)(cy + rad * sin(45 * M_PI_D / 180)),
        (int)(cx - rad * cos(45 * M_PI_D / 180)), (int)(cy + rad * sin(45 * M_PI_D / 180)));
    Ellipse(dc, (int)(cx - r0), (int)(cy - r0), (int)(cx + r0), (int)(cy + r0));
    SelectObject(dc, op);
    DeleteObject(pen);
    hand = CreatePen(PS_SOLID, astro_scale(3, g_dpi), picked ? ACCENT : fg);
    SelectObject(dc, hand);
    MoveToEx(dc, (int)(cx + sin(a) * rad * 0.25), (int)(cy - cos(a) * rad * 0.25), NULL);
    LineTo(dc, (int)(cx + sin(a) * rad * 0.95), (int)(cy - cos(a) * rad * 0.95));
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(hand);
}

static void paint(HDC dc, const RECT *client)
{
    const COLORREF bg = GetSysColor(COLOR_BTNFACE), fg = GetSysColor(COLOR_WINDOWTEXT);
    HBRUSH bgb = CreateSolidBrush(bg);
    wchar_t buf[96];
    FillRect(dc, client, bgb);
    DeleteObject(bgb);

    for (int f = 0; f < 1 + ASTROSESSION_PLAYERS; f++) {
        RECT fr = scaled(&g_frames[f]), tr = fr;
        HPEN pen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_3DSHADOW));
        HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, fr.left, fr.top, fr.right, fr.bottom, astro_scale(8, g_dpi), astro_scale(8, g_dpi));
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(pen);
        tr.top += astro_scale(8, g_dpi);
        tr.bottom = tr.top + astro_scale(22, g_dpi);
        text(dc, g_titles[f], &tr, DT_CENTER | DT_SINGLELINE, fg, g_font_bold);
    }

    for (int i = 0; i < g_nctl; i++) {
        const control *c = &g_ctl[i];
        RECT r = scaled(&c->r);
        COLORREF face = GetSysColor(COLOR_BTNFACE) == RGB(240, 240, 240) ? RGB(225, 225, 225)
                                                                          : GetSysColor(COLOR_3DLIGHT);
        COLORREF tc = fg;
        const int gold = c->target >= 0 && c->target < ASTROSESSION_KEY_COUNT && c->target % 4 == 3;
        int lit = c->lit || (g_map_state >= 0 && c->target == g_map_state) ||
                  (c->special == SP_MAP && g_map_state != -2);
        const int pressed = (g_down == i);
        wchar_t label[96];

        if (c->special == SP_DIAL) {
            draw_dial(dc, c, g_map_state == c->target);
            continue;
        }
        if (c->special == SP_CLEAR && g_map_state < 0)
            tc = GetSysColor(COLOR_GRAYTEXT);
        if (gold) { face = GOLD; tc = GOLD_TEXT; }
        if (lit || pressed) { face = ACCENT; tc = RGB(255, 255, 255); }
        fill(dc, &r, face, astro_scale(8, g_dpi));

        wcsncpy(label, c->face, 95);
        label[95] = 0;
        if (g_map_state != -2 && c->special == SP_NONE && c->target >= 0) {
            char key[48], pad[48];
            wchar_t *wk, *wp;
            astrosession_keysym_name(bindings_target_keysym(c->target), key, sizeof key);
            astrosession_pad_input_name(bindings_target_pad(c->target), pad, sizeof pad);
            wk = astro_wide_dup(key[0] ? key : "\xe2\x80\x94");
            wp = astro_wide_dup(pad);
            _snwprintf(label, 95, pad[0] ? L"%ls\n%ls" : L"%ls", wk ? wk : L"", wp ? wp : L"");
            label[95] = 0;
            free(wk); free(wp);
        }
        {
            RECT tr = r;
            const int two = wcschr(label, L'\n') != NULL;
            if (two) { tr.top += astro_scale(2, g_dpi); tr.bottom -= astro_scale(2, g_dpi); }
            text(dc, label, &tr, DT_CENTER | (two ? DT_WORDBREAK | DT_VCENTER : DT_VCENTER | DT_SINGLELINE),
                 tc, (g_map_state != -2 && c->special == SP_NONE) ? g_font_small
                      : (c->target >= 0 && c->target < ASTROSESSION_KEY_COUNT) ? g_font_bold : g_font);
        }
    }

    /* the dials' captions */
    for (int i = 0; i < g_nctl; i++) {
        const control *c = &g_ctl[i];
        RECT r;
        if (c->special != SP_DIAL) continue;
        r = scaled(&c->r);
        r.top = r.bottom + astro_scale(2, g_dpi);
        r.bottom = r.top + astro_scale(18, g_dpi);
        r.left -= astro_scale(20, g_dpi);
        r.right += astro_scale(20, g_dpi);
        if (g_map_state != -2) {
            char pad[48];
            wchar_t *w;
            astrosession_pad_input_name(bindings_target_pad(c->target), pad, sizeof pad);
            w = astro_wide_dup(pad[0] ? pad : "\xe2\x80\x94");
            _snwprintf(buf, 95, L"%ls", w ? w : L"");
            free(w);
        } else {
            _snwprintf(buf, 95, L"Knob");
        }
        text(dc, buf, &r, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS, GetSysColor(COLOR_GRAYTEXT), g_font_small);
    }

    {
        RECT r = scaled(&g_pads_text);
        text(dc, g_pads, &r, DT_LEFT | DT_BOTTOM | DT_WORDBREAK, GetSysColor(COLOR_GRAYTEXT), g_font_small);
        r = scaled(&g_hint_text);
        text(dc, g_hint, &r, DT_LEFT | DT_TOP | DT_WORDBREAK, GetSysColor(COLOR_GRAYTEXT), g_font_small);
    }
}

static void make_fonts(void)
{
    if (g_font) DeleteObject(g_font);
    if (g_font_bold) DeleteObject(g_font_bold);
    if (g_font_small) DeleteObject(g_font_small);
    g_font = astro_make_font(g_dpi, 0, FW_NORMAL, NULL);
    g_font_bold = astro_make_font(g_dpi, 16, FW_BOLD, NULL);
    g_font_small = astro_make_font(g_dpi, 12, FW_NORMAL, NULL);
}

static void size_window(const RECT *at)
{
    SIZE outer;
    g_dpi = astro_window_dpi(g_window);
    make_fonts();
    outer = astro_outer_size(g_window, astro_scale(CLIENT_W, g_dpi), astro_scale(CLIENT_H, g_dpi), FALSE);
    if (at)
        SetWindowPos(g_window, NULL, at->left, at->top, outer.cx, outer.cy,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    else
        SetWindowPos(g_window, NULL, 0, 0, outer.cx, outer.cy,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
}

/* ---- the window ----------------------------------------------------------- */

static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_LBUTTONDOWN: {
        const int i = hit((short)LOWORD(l), (short)HIWORD(l));
        if (i < 0) return 0;
        SetFocus(hwnd);
        if (g_ctl[i].special == SP_NONE || g_ctl[i].special == SP_DIAL) {
            if (g_map_state == -1) { set_map_state(g_ctl[i].target); return 0; }
            if (g_map_state != -2) return 0;
        }
        if (g_ctl[i].special == SP_DIAL) {
            g_down = i;
            g_dial_drag = i;
            SetCapture(hwnd);
            dial_drag_to(i, (short)LOWORD(l), (short)HIWORD(l));
            return 0;
        }
        if (g_ctl[i].special != SP_NONE) { on_special(g_ctl[i].special); return 0; }
        g_down = i;
        SetCapture(hwnd);
        press(g_ctl[i].target, 1);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_dial_drag >= 0)
            dial_drag_to(g_dial_drag, (short)LOWORD(l), (short)HIWORD(l));
        return 0;
    case WM_LBUTTONUP: {
        const int i = hit((short)LOWORD(l), (short)HIWORD(l));
        const BOOL inside = (i >= 0 && i == g_down);
        if (g_down >= 0) {
            release_mouse_hold(inside);
            ReleaseCapture();
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        release_mouse_hold(FALSE);    /* the grab was taken: nothing stays held */
        return 0;
    case WM_KEYDOWN:
        if (l & 0x40000000)      /* ignore auto-repeat */
            return 0;
        if (astro_controllers_key(g_session, l, TRUE))
            return 0;
        if (w == VK_F9) ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_KEYUP:
        astro_controllers_key(g_session, l, FALSE);
        return 0;
    case WM_KILLFOCUS:
        astrosession_release_all(g_session);
        return 0;
    case WM_TIMER:
        if (w == TIMER_POLL) poll();
        return 0;
    case WM_SHOWWINDOW:
        if (w) {
            SetTimer(hwnd, TIMER_POLL, 50, NULL);
            poll();
        } else {
            KillTimer(hwnd, TIMER_POLL);
            release_mouse_hold(FALSE);
            if (g_map_state != -2) set_map_state(-2);
        }
        return 0;
    case WM_DPICHANGED:
        size_window((const RECT *)l);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT c;
        HDC dc = BeginPaint(hwnd, &ps);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp, old;
        GetClientRect(hwnd, &c);
        bmp = CreateCompatibleBitmap(dc, c.right, c.bottom);
        old = SelectObject(mem, bmp);
        paint(mem, &c);
        BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

static void create(void)
{
    HINSTANCE inst = GetModuleHandle(NULL);
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"AstroControllers";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APPICON));
    RegisterClassW(&wc);

    build_controls();
    /* fixed size, floating: no thick frame, no maximize box */
    g_window = CreateWindowExW(WS_EX_TOOLWINDOW, L"AstroControllers", L"Controllers",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, CLIENT_W, CLIENT_H, g_owner, NULL, inst, NULL);
    wcscpy(g_pads, L"No gamepad connected");
    size_window(NULL);
}

void astro_controllers_toggle(HWND owner, astrosession *session)
{
    g_session = session;
    g_owner = owner;
    if (!g_window)
        create();
    ShowWindow(g_window, IsWindowVisible(g_window) ? SW_HIDE : SW_SHOW);
}

BOOL astro_controllers_visible(void)
{
    return g_window && IsWindowVisible(g_window);
}
