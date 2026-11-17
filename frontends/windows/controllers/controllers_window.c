/*
 * The Win32 Controllers window (F9): the Bally Professional Arcade as a player
 * sees it -- the console's 24-key calculator keypad on top, under it the four
 * pistol-grip hand controllers (the ribbed knob on top is the paddle: drag it
 * round; the eight wedges ringed about it are the stick; the lever at the
 * front is the trigger), and RESET GAME / Reset to CONFIG at the bottom --
 * drawn from the session's layout (astrosession_controls_layout), so every
 * frontend shows the same parts. Under the console: the gamepads with the
 * player each drives, and Map mode.
 *
 * THE BALLY BASIC OVERLAY ("Overlay: Bally BASIC"): a cream card over the
 * keypad, with a black legend bar above each row of keys carrying, for every
 * key, the three small shift-colour characters (green, red, blue) and the gold
 * BASIC word, and the key's own white legend on the key. With it on, the host
 * keyboard types BASIC: a printable key that is not a bound control is typed
 * through the session (astrosession_type_char: the shift colour, then the key).
 * The overlay switches on by itself when a Bally BASIC cartridge loads.
 *
 * MOUSE: a press on a key or the trigger holds it; a press on the stick or a
 * wedge follows the pointer and holds the direction it points
 * (astrosession_stick_from_vector); a press on the dial turns the knob to the
 * angle of the pointer, and the wheel nudges it. Presses are the raw
 * button-down and button-up, never a click: the console samples its controls
 * once a frame. The mouse is captured on press; letting go anywhere, losing the
 * capture, hiding or deactivating the window releases whatever the pointer
 * holds. RESET GAME and Reset to CONFIG fire on release over the button, like a
 * real one.
 *
 * What the console sees held -- keyboard, gamepads and this window together
 * (astrosession_target_held) -- is polled every 40 ms and painted in the
 * accent, and the dials show the live knob, so the window doubles as a gamepad
 * tester.
 *
 * FIXED SIZE: laid out once, in a caption-only tool window with no thick frame
 * and no maximize box, so neither Map mode nor a long gamepad name changes its
 * size.
 *
 * MAP MODE: Map arms a two-step rebind -- click a control to pick the target
 * (the dial: its left half the knob-left key, its right half the knob-right
 * key, its lower part the knob's axis), then press a host key OR a gamepad
 * button (an axis, for the knob's axis); Esc cancels. While armed the controls
 * show what drives them and typing does not reach the console. Clear takes the
 * picked target's key and button away. "Show bindings" shows the same legends
 * outside Map mode. F9, F11 and F12 are the app's and cannot be bound.
 *
 * The art is drawn at twice the size into a memory bitmap and shrunk with
 * HALFTONE (GDI has no anti-aliasing of its own); the text goes on after at the
 * window's own size.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controllers_window.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* GET_X_LPARAM / GET_Y_LPARAM live here, not in windows.h. */
#include <windowsx.h>

#include "bindings.h"
#include "../debugger/dbg_window.h"
#include "../help/help_window.h"
#include "../win_common.h"

#define PANEL_CLASS   "AstroControllers"
#define PANEL_STYLE   (WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN)
#define PANEL_EXSTYLE WS_EX_TOOLWINDOW

/* Sizes at 96 dpi; SCL() scales them to the window's. */
#define UNIT96     40       /* pixels per layout unit (a keypad key's width) */
#define MARGIN     12
#define CASE_PAD   16
#define BRAND_W    168
#define BRAND_H    22
#define HEAD_H     22
#define ROW_H      26       /* a gamepad row: its name and its Auto / Player 1-4 choice */
#define ROW_GAP    3
#define PAD_ROWS   4        /* gamepad rows the window has room for */
#define BTN_H      28
#define HINT_H     20

#define IDT_CAPTURE 1
#define IDT_HELD    2
#define IDT_PADS    3

#define PI_DEG 3.14159265358979323846
#define PLAYERS ASTROSESSION_PLAYERS
#define KEYS    ASTROSESSION_KEY_COUNT

#define DIR_BITS ((1u << ASTRO_ACT_UP) | (1u << ASTRO_ACT_DOWN) | \
                  (1u << ASTRO_ACT_LEFT) | (1u << ASTRO_ACT_RIGHT))

/* The strip under the pads: Map, Clear, Defaults, Show bindings, the overlay, Help. */
enum { SB_MAP, SB_CLEAR, SB_DEFAULTS, SB_SHOW, SB_OVERLAY, SB_HELP, SB_COUNT };

/* A pad row's choice: Auto, then Player 1-4. */
#define SEGS 5

typedef struct {
    unsigned generation;
    int count, assign[PAD_ROWS], effective[PAD_ROWS];
} pads_state;

/* Where a player's card sits, in pixels (set by layout_static). */
typedef struct {
    double ox, oy, width, bottom;      /* the card's tile */
    double ctrx, ctry, ring, dial;     /* the stick's disc and the knob's */
    int trig;                          /* the layout index of the trigger */
} card_geo;

static HWND g_panel, g_parent;
static astrosession *g_session;
static int g_dpi = 96;

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 waiting for a
 * key or pad input to bind to that target. */
static int g_map_state = -2;
static int g_show_bindings;
static int g_overlay;                   /* the overlay as last painted */
static WPARAM g_swallow_vkey;           /* the key Map took: its repeats and release too */
static int g_drop_char;                 /* the WM_CHAR of a key Map took */
static int g_key_used;                  /* the last key press was a bound control: not typing */
static char g_hint[300];
static int g_hover_part = -1;           /* the part under the idle pointer */
static int g_tracking;

/* The layout (units), and where it sits in the client. */
static const astro_control *g_layout;
static int g_numlayout, g_plate = -1;
static float g_layw, g_layh;
static double g_unit, g_orgx, g_orgy;
static card_geo g_card[PLAYERS];

/* What the pointer holds. */
static int g_mouse_part = -1;           /* a pressed key, trigger or console button (layout index) */
static int g_mouse_player = -1;         /* a stick drag */
static int g_mouse_knob = -1;           /* a dial drag */
static unsigned g_pointer[PLAYERS];     /* the bits given to astrosession_pointer_set_player */

/* What was last painted. */
static unsigned char g_key_held[KEYS];
static unsigned g_act_held[PLAYERS];
static int g_knob_shown[PLAYERS];
static pads_state g_pads_shown;
static int g_total_w, g_total_h;

static RECT g_case_rc, g_brand_rc, g_gp_rc, g_gp_head_rc, g_gp_row_rc[PAD_ROWS];
static RECT g_btn_rc[SB_COUNT], g_hint_rc;

static HFONT g_font, g_bold, g_caption, g_digit, g_legend, g_tiny, g_brand, g_ovsmall, g_knobnum, g_sys, g_ovword;

static void refresh_held(void);
static void binding_label(int target, char *out, int outsz);

static int SCL(int val) { return astro_scale(val, g_dpi); }

/* ---- fonts ------------------------------------------------------------------ */

static void free_fonts(void)
{
    HFONT *const slots[] = { &g_font, &g_bold, &g_caption, &g_digit, &g_legend, &g_tiny,
                             &g_brand, &g_ovsmall, &g_knobnum, &g_sys, &g_ovword };
    size_t idx;
    for (idx = 0; idx < sizeof slots / sizeof slots[0]; idx++)
        if (*slots[idx]) { DeleteObject(*slots[idx]); *slots[idx] = NULL; }
}

static void make_fonts(void)
{
    free_fonts();
    g_font = astro_make_font(g_dpi, 0, FW_NORMAL, NULL);
    g_bold = astro_make_font(g_dpi, 0, FW_BOLD, NULL);
    g_caption = astro_make_font(g_dpi, 13, FW_BOLD, NULL);
    g_digit = astro_make_font(g_dpi, 14, FW_BOLD, NULL);
    g_legend = astro_make_font(g_dpi, 11, FW_BOLD, NULL);
    g_tiny = astro_make_font(g_dpi, 10, FW_NORMAL, NULL);
    g_brand = astro_make_font(g_dpi, 13, FW_BOLD, NULL);
    g_ovsmall = astro_make_font(g_dpi, 9, FW_BOLD, NULL);
    g_knobnum = astro_make_font(g_dpi, 14, FW_BOLD, NULL);
    g_sys = astro_make_font(g_dpi, 9, FW_BOLD, NULL);
    g_ovword = astro_make_font(g_dpi, 8, FW_BOLD, NULL);   /* the gold words: small, above the letters */
}

/* ---- text -------------------------------------------------------------------- */

/* UTF-8 (legends, key names, gamepad names) through DrawTextW. */
static int text_out(HDC hdc, const char *utf8, RECT *rect, UINT fmt)
{
    wchar_t wide[256];
    if (!utf8 || !*utf8) return 0;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, (int)(sizeof wide / sizeof wide[0])) <= 0) return 0;
    return DrawTextW(hdc, wide, -1, rect, fmt | DT_NOPREFIX);
}

static void text_line(HDC hdc, const char *utf8, const RECT *rect, UINT fmt)
{
    RECT copy = *rect;
    text_out(hdc, utf8, &copy, fmt | DT_SINGLELINE | DT_VCENTER);
}

static int text_width(HDC hdc, const char *utf8)
{
    wchar_t wide[128];
    SIZE ext;
    if (!utf8 || MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, 128) <= 0) return 0;
    return GetTextExtentPoint32W(hdc, wide, (int)wcslen(wide), &ext) ? ext.cx : 0;
}

/* Centred both ways, wrapped (a long key name on a small face). */
static void text_block(HDC hdc, const char *utf8, const RECT *rect)
{
    RECT mid = *rect;
    int high;
    InflateRect(&mid, -SCL(1), 0);
    high = text_out(hdc, utf8, &mid, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
    mid.left = rect->left + SCL(1);
    mid.right = rect->right - SCL(1);
    mid.top = rect->top + ((rect->bottom - rect->top) - high) / 2;
    if (mid.top < rect->top) mid.top = rect->top;
    mid.bottom = rect->bottom;
    text_out(hdc, utf8, &mid, DT_CENTER | DT_WORDBREAK);
}

/* The colours live here and only here; the geometry is the core's. From the
 * photos of the real console and hand controller (DESIGN-astrocade.md). */
#define COL_ACCENT     RGB(0xE8, 0xC1, 0x4A)              /* ASTROSESSION_ACCENT_RGB */
#define COL_ON_ACCENT  RGB(0x3A, 0x2C, 0x00)
#define COL_WALNUT_TOP RGB(0x9A, 0x66, 0x38)
#define COL_WALNUT_BOT RGB(0x6E, 0x45, 0x24)
#define COL_WALNUT_EDG RGB(0x3A, 0x22, 0x10)
#define COL_TOP_A      RGB(0x2A, 0x29, 0x28)              /* the black textured console top */
#define COL_TOP_B      RGB(0x1B, 0x1B, 0x1A)
#define COL_PINSTRIPE  RGB(0xB0, 0x8A, 0x3C)
#define COL_BRAND_BG   RGB(0xEF, 0xEF, 0xEA)
#define COL_BRAND_RED  RGB(0xD2, 0x28, 0x2A)
#define COL_BRAND_INK  RGB(0x2A, 0x24, 0x22)
#define COL_GOLD_FRAME RGB(0xC9, 0xA6, 0x4E)
#define COL_PLATE      RGB(0xDC, 0xCB, 0xA8)              /* champagne */
#define COL_GOLD_COL   RGB(0xCF, 0xAE, 0x5E)
#define COL_PLATE_EDGE RGB(0x8A, 0x70, 0x36)
#define COL_CONSOLE_INK RGB(0x4A, 0x3A, 0x30)             /* the legends printed on the plate */
#define COL_KEY_TOP    RGB(0x3C, 0x2E, 0x2E)
#define COL_KEY_SIDE   RGB(0x1B, 0x13, 0x13)
#define COL_KEY_EDGE   RGB(0x10, 0x0A, 0x0A)
#define COL_KEY_INK    RGB(0xEA, 0xDF, 0xCB)
#define COL_CARD_A     RGB(0x57, 0x53, 0x50)              /* a player's tile */
#define COL_CARD_B     RGB(0x3A, 0x37, 0x35)
#define COL_CARD_EDGE  RGB(0x8C, 0x74, 0x30)
#define COL_CAPTION    RGB(0xD4, 0xA9, 0x3A)
#define COL_SHELL      RGB(0x12, 0x11, 0x11)              /* glossy black */
#define COL_SHELL_HI   RGB(0x9A, 0x97, 0x99)
#define COL_PANEL      RGB(0x2A, 0x26, 0x26)
#define COL_KNURL      RGB(0x4C, 0x3D, 0x3D)
#define COL_BADGE      RGB(0xD4, 0xA9, 0x3A)
#define COL_BADGE_IN   RGB(0x6A, 0x50, 0x14)
#define COL_KNOB       RGB(0xC0, 0x64, 0x2A)
#define COL_KNOB_HI    RGB(0xE6, 0x7A, 0x2C)
#define COL_KNOB_LO    RGB(0x8A, 0x43, 0x18)
#define COL_FLUTE      RGB(0x7A, 0x3A, 0x1C)
#define COL_POINTER    RGB(0xF4, 0xE6, 0xCF)
#define COL_TRIG_A     RGB(0xB4, 0xB4, 0xBA)
#define COL_TRIG_B     RGB(0x7E, 0x7E, 0x86)
#define COL_TRIG_EDGE  RGB(0x22, 0x22, 0x24)
#define COL_RING       RGB(0x1D, 0x1C, 0x1B)
#define COL_WEDGE      RGB(0x3E, 0x3B, 0x39)
#define COL_WEDGE_EDG  RGB(0x6A, 0x60, 0x4C)
#define COL_GLYPH      RGB(0xB9, 0xB0, 0xA0)
#define COL_SYS        RGB(0x17, 0x16, 0x16)              /* the console's RESET buttons */
#define COL_SYS_EDGE   RGB(0x55, 0x4C, 0x3A)
#define COL_SYS_INK    RGB(0xD4, 0xB0, 0x4C)
#define COL_DIM_TO     RGB(0x26, 0x25, 0x24)              /* unpicked parts fade toward the tile */
/* the Bally BASIC overlay */
#define COL_OV_CARD    RGB(0xFB, 0xEF, 0xDD)
#define COL_OV_BAR     RGB(0x1A, 0x1A, 0x1A)
#define COL_OV_WINDOW  RGB(0xF7, 0xF2, 0xE8)
#define COL_OV_WHITE   RGB(0xF2, 0xF2, 0xF2)
#define COL_OV_WORDS   RGB(0xE6, 0xBC, 0x4C)
#define COL_OV_GREEN   RGB(0x3F, 0xA5, 0x5A)
#define COL_OV_RED     RGB(0xE0, 0x4A, 0x42)
#define COL_OV_BLUE    RGB(0x28, 0x78, 0xB0)
#define NOFILL         ((COLORREF)0xFFFFFFFFu)

/* ---- art: GDI at a supersampled scale ------------------------------------------- */

/* One art pass draws the client rectangle with its top-left at (g_art_x,
 * g_art_y) into a bitmap g_art_k times the size and g_art_rows rows tall. */
static int g_art_k = 1, g_art_x, g_art_y, g_art_rows;

static int art_xpos(double val) { return (int)floor((val - g_art_x) * g_art_k + 0.5); }
static int art_ypos(double val) { return (int)floor((val - g_art_y) * g_art_k + 0.5); }
static int art_len(double val) { return (int)floor(val * g_art_k + 0.5); }

static COLORREF mix_color(COLORREF start, COLORREF finish, double frac)
{
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    return RGB((int)(GetRValue(start) + (GetRValue(finish) - GetRValue(start)) * frac + 0.5),
               (int)(GetGValue(start) + (GetGValue(finish) - GetGValue(start)) * frac + 0.5),
               (int)(GetBValue(start) + (GetBValue(finish) - GetBValue(start)) * frac + 0.5));
}

/* An unpicked part in Map mode: 40% of its own colours over the tile's. */
static COLORREF tone(COLORREF col, int dim)
{
    return dim ? mix_color(col, COL_DIM_TO, 0.6) : col;
}

static void poly(HDC hdc, const POINT *pnts, int num, COLORREF fill, COLORREF line, double width)
{
    HBRUSH brush = fill == NOFILL ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, width > 0 ? art_len(width) : 0, line);
    HGDIOBJ oldb = SelectObject(hdc, brush);
    HGDIOBJ oldp = SelectObject(hdc, width > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    Polygon(hdc, pnts, num);
    SelectObject(hdc, oldp);
    SelectObject(hdc, oldb);
    DeleteObject(pen);
    if (fill != NOFILL) DeleteObject(brush);
}

static void ellipse(HDC hdc, double ctrx, double ctry, double radx, double rady, COLORREF fill, COLORREF line,
                    double width)
{
    HBRUSH brush = fill == NOFILL ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, width > 0 ? art_len(width) : 0, line);
    HGDIOBJ oldb = SelectObject(hdc, brush);
    HGDIOBJ oldp = SelectObject(hdc, width > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    Ellipse(hdc, art_xpos(ctrx - radx), art_ypos(ctry - rady), art_xpos(ctrx + radx), art_ypos(ctry + rady));
    SelectObject(hdc, oldp);
    SelectObject(hdc, oldb);
    DeleteObject(pen);
    if (fill != NOFILL) DeleteObject(brush);
}

static void rrect(HDC hdc, double xpos, double ypos, double width, double height, double radius, COLORREF fill,
                  COLORREF line, double linew)
{
    HBRUSH brush = fill == NOFILL ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, linew > 0 ? art_len(linew) : 0, line);
    HGDIOBJ oldb = SelectObject(hdc, brush);
    HGDIOBJ oldp = SelectObject(hdc, linew > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    RoundRect(hdc, art_xpos(xpos), art_ypos(ypos), art_xpos(xpos + width), art_ypos(ypos + height),
              art_len(radius * 2), art_len(radius * 2));
    SelectObject(hdc, oldp);
    SelectObject(hdc, oldb);
    DeleteObject(pen);
    if (fill != NOFILL) DeleteObject(brush);
}

static void line_seg(HDC hdc, double x0, double y0, double x1, double y1, COLORREF col, double width)
{
    HPEN pen = CreatePen(PS_SOLID, art_len(width) > 0 ? art_len(width) : 1, col);
    HGDIOBJ oldp = SelectObject(hdc, pen);
    MoveToEx(hdc, art_xpos(x0), art_ypos(y0), NULL);
    LineTo(hdc, art_xpos(x1), art_ypos(y1));
    SelectObject(hdc, oldp);
    DeleteObject(pen);
}

/* A rounded rectangle filled with a vertical gradient, then outlined. */
static void grad_rrect(HDC hdc, double xpos, double ypos, double width, double height, double radius,
                       COLORREF top, COLORREF bottom, COLORREF edge, double edgew)
{
    const int left = art_xpos(xpos), right = art_xpos(xpos + width);
    const int first = art_ypos(ypos), last = art_ypos(ypos + height);
    HRGN clip = CreateRoundRectRgn(left, first, right + 1, last + 1, art_len(radius * 2), art_len(radius * 2));
    int row;
    HGDIOBJ oldb;
    SelectClipRgn(hdc, clip);
    oldb = SelectObject(hdc, GetStockObject(DC_BRUSH));
    for (row = first < 0 ? 0 : first; row <= last && row < g_art_rows; row++) {
        SetDCBrushColor(hdc, mix_color(top, bottom, last > first ? (double)(row - first) / (last - first) : 0));
        PatBlt(hdc, left, row, right - left + 1, 1, PATCOPY);
    }
    SelectObject(hdc, oldb);
    SelectClipRgn(hdc, NULL);
    DeleteObject(clip);
    if (edgew > 0) rrect(hdc, xpos, ypos, width, height, radius, NOFILL, edge, edgew);
}

/* The pixel rectangle of a layout part. */
static void part_box(const astro_control *ctl, double *xpos, double *ypos, double *width, double *height)
{
    *xpos = g_orgx + (double)ctl->xpos * g_unit;
    *ypos = g_orgy + (double)ctl->ypos * g_unit;
    *width = (double)ctl->width * g_unit;
    *height = (double)ctl->height * g_unit;
}

static void part_rect(const astro_control *ctl, RECT *rect)
{
    double xpos, ypos, width, height;
    part_box(ctl, &xpos, &ypos, &width, &height);
    SetRect(rect, (int)floor(xpos), (int)floor(ypos), (int)ceil(xpos + width), (int)ceil(ypos + height));
}

static int binding_mode(void)
{
    return g_map_state != -2 || g_show_bindings;
}

static int is_knob_axis_target(int target)
{
    return target >= ASTRO_TARGET_PLAYERS && (target - ASTRO_TARGET_PLAYERS) % ASTRO_ACT_PER_PLAYER == ASTRO_ACT_KNOB;
}

/* The three binding targets of a dial: its left key, its right key, its axis. */
static int knob_target(int player, int zone)     /* zone 0 left, 1 right, 2 axis */
{
    return ASTRO_TARGET_PLAYER(player, zone == 0 ? ASTRO_ACT_KNOB_DEC : zone == 1 ? ASTRO_ACT_KNOB_INC : ASTRO_ACT_KNOB);
}

static int is_knob_target(int target)
{
    return target >= ASTRO_TARGET_PLAYERS &&
           ((target - ASTRO_TARGET_PLAYERS) % ASTRO_ACT_PER_PLAYER == ASTRO_ACT_KNOB_DEC ||
            (target - ASTRO_TARGET_PLAYERS) % ASTRO_ACT_PER_PLAYER == ASTRO_ACT_KNOB_INC ||
            is_knob_axis_target(target));
}

/* Whether a part is shown held: what the console sees, in the accent. In Map
 * mode the picked target is the one lit. */
static int part_lit(const astro_control *ctl)
{
    if (g_map_state != -2)
        return g_map_state >= 0 && ctl->target >= 0 && ctl->target == g_map_state;
    switch (ctl->kind) {
    case ASTRO_CTL_KEY:
        return ctl->target >= 0 && ctl->target - ASTRO_TARGET_KEYPAD < KEYS &&
               g_key_held[ctl->target - ASTRO_TARGET_KEYPAD];
    case ASTRO_CTL_TRIGGER: return (g_act_held[ctl->player] >> ASTRO_ACT_TRIGGER) & 1u;
    case ASTRO_CTL_DIRECTION: return (g_act_held[ctl->player] & DIR_BITS) == ctl->act_mask;
    default: return 0;
    }
}

/* Map mode with a target picked: every other part is drawn at 40%. */
static int part_dim(const astro_control *ctl)
{
    if (g_map_state < 0) return 0;
    switch (ctl->kind) {
    case ASTRO_CTL_KEY: case ASTRO_CTL_TRIGGER: case ASTRO_CTL_DIRECTION:
    case ASTRO_CTL_SYSBUTTON:
        return ctl->target != g_map_state;
    case ASTRO_CTL_STICK:
        return 1;
    case ASTRO_CTL_KNOB:
        return !(is_knob_target(g_map_state) && (g_map_state - ASTRO_TARGET_PLAYERS) / ASTRO_ACT_PER_PLAYER == ctl->player);
    default: return 0;
    }
}

/* The sector index of a wedge (0 = RIGHT, counter-clockwise) from its bits. */
static int wedge_sector(unsigned mask)
{
    static const unsigned sector_bits[8] = {
        1u << ASTRO_ACT_RIGHT,
        (1u << ASTRO_ACT_UP) | (1u << ASTRO_ACT_RIGHT),
        1u << ASTRO_ACT_UP,
        (1u << ASTRO_ACT_UP) | (1u << ASTRO_ACT_LEFT),
        1u << ASTRO_ACT_LEFT,
        (1u << ASTRO_ACT_DOWN) | (1u << ASTRO_ACT_LEFT),
        1u << ASTRO_ACT_DOWN,
        (1u << ASTRO_ACT_DOWN) | (1u << ASTRO_ACT_RIGHT),
    };
    int idx;
    for (idx = 0; idx < 8; idx++)
        if (sector_bits[idx] == mask) return idx;
    return 0;
}

/* One of the eight wedges: the annular sector between the dial's edge and the
 * ring's, a little narrower than 45 degrees so they read as separate. */
static void wedge(HDC hdc, double ctrx, double ctry, double router, double rinner, int sector,
                  COLORREF fill, COLORREF line)
{
    POINT pnts[40];
    int num = 0, idx;
    const double start = (sector * 45.0 - 21.5) * PI_DEG / 180.0, span = 43.0 * PI_DEG / 180.0;
    for (idx = 0; idx <= 9; idx++) {
        const double ang = start + span * idx / 9.0;
        pnts[num].x = art_xpos(ctrx + router * cos(ang));
        pnts[num++].y = art_ypos(ctry - router * sin(ang));
    }
    for (idx = 9; idx >= 0; idx--) {
        const double ang = start + span * idx / 9.0;
        pnts[num].x = art_xpos(ctrx + rinner * cos(ang));
        pnts[num++].y = art_ypos(ctry - rinner * sin(ang));
    }
    poly(hdc, pnts, num, fill, line, 1.0);
}

/* An arrowhead at (ctrx, ctry) pointing along maths angle `ang` (degrees,
 * y up). */
static void arrow(HDC hdc, double ctrx, double ctry, double ang, double size, COLORREF col)
{
    POINT pnts[3];
    const double rad = ang * PI_DEG / 180.0, dirx = cos(rad), diry = -sin(rad);
    pnts[0].x = art_xpos(ctrx + dirx * size);
    pnts[0].y = art_ypos(ctry + diry * size);
    pnts[1].x = art_xpos(ctrx - dirx * size * 0.6 - diry * size * 0.8);
    pnts[1].y = art_ypos(ctry - diry * size * 0.6 + dirx * size * 0.8);
    pnts[2].x = art_xpos(ctrx - dirx * size * 0.6 + diry * size * 0.8);
    pnts[2].y = art_ypos(ctry - diry * size * 0.6 - dirx * size * 0.8);
    poly(hdc, pnts, 3, col, col, 0.5);
}

/* ---- the keypad ---------------------------------------------------------------- */

/* The legends printed on the console's plate, above each key. */
static const char *const k_console[KEYS] = {
    "C",  "\xe2\x86\x91", "\xe2\x86\x93", "%",
    "MR", "MS",           "CH",           "\xc3\xb7",
    "7",  "8",            "9",            "\xc3\x97",
    "4",  "5",            "6",            "\xe2\x88\x92",
    "1",  "2",            "3",            "+",
    "CE", "0",            ".",            "=",
};

static COLORREF shift_colour(int shift)
{
    switch (shift) {
    case ASTRO_SHIFT_GREEN: return COL_OV_GREEN;
    case ASTRO_SHIFT_RED:   return COL_OV_RED;
    case ASTRO_SHIFT_BLUE:  return COL_OV_BLUE;
    case ASTRO_SHIFT_WORDS: return COL_OV_WORDS;
    default:                return COL_OV_WHITE;
    }
}

/* The keypad key a layout part is, or -1. */
static int key_of(const astro_control *ctl)
{
    return ctl->kind == ASTRO_CTL_KEY && ctl->target >= ASTRO_TARGET_KEYPAD &&
           ctl->target < ASTRO_TARGET_KEYPAD + KEYS ? ctl->target - ASTRO_TARGET_KEYPAD : -1;
}

/* The plate: the gold frame, then the champagne plate with the gold column of
 * operators, or the cream overlay card with its black legend bars and a window
 * round every key. */
static void art_plate(HDC hdc, const astro_control *plate)
{
    double xpos, ypos, width, height;
    const double unit = g_unit;
    int row;
    part_box(plate, &xpos, &ypos, &width, &height);
    grad_rrect(hdc, xpos - unit * 0.12, ypos - unit * 0.12, width + unit * 0.24, height + unit * 0.24, unit * 0.22,
               mix_color(COL_GOLD_FRAME, RGB(0xFF, 0xF0, 0xB0), 0.25), mix_color(COL_GOLD_FRAME, RGB(0, 0, 0), 0.25),
               COL_PLATE_EDGE, 1.0);
    if (!g_overlay) {
        rrect(hdc, xpos, ypos, width, height, unit * 0.12, tone(COL_PLATE, 0), COL_PLATE_EDGE, 1.0);
        /* the gold stripe down the operator column, from the second row down */
        rrect(hdc, xpos + unit * (0.45 + 3 * 1.35 - 0.12), ypos + unit * (0.2 + 1.3),
              unit * 1.24, unit * (5 * 1.3), unit * 0.1, COL_GOLD_COL, COL_PLATE_EDGE, 0.8);
        return;
    }
    rrect(hdc, xpos, ypos, width, height, unit * 0.12, COL_OV_CARD, COL_OV_BAR, 1.0);
    for (row = 0; row < 6; row++)       /* a black legend bar above each row of windows */
        rrect(hdc, xpos + unit * 0.12, ypos + unit * (0.2 + row * 1.3) - unit * 0.02, width - unit * 0.24,
              unit * 0.5, unit * 0.06, COL_OV_BAR, COL_OV_BAR, 0);
}

static void art_keys(HDC hdc)
{
    int idx;
    const double side = SCL(3);
    for (idx = 0; idx < g_numlayout; idx++) {
        const astro_control *ctl = &g_layout[idx];
        double xpos, ypos, width, height;
        int lit, dim;
        if (ctl->kind != ASTRO_CTL_KEY) continue;
        lit = part_lit(ctl);
        dim = part_dim(ctl);
        part_box(ctl, &xpos, &ypos, &width, &height);
        if (g_overlay)      /* the window of the card the key comes through */
            rrect(hdc, xpos - g_unit * 0.07, ypos - g_unit * 0.07, width + g_unit * 0.14,
                  height + g_unit * 0.14, g_unit * 0.1, tone(COL_OV_WINDOW, dim), COL_OV_BAR, 0.8);
        rrect(hdc, xpos, ypos + side, width, height - side, g_unit * 0.1, tone(COL_KEY_SIDE, dim),
              tone(COL_KEY_SIDE, dim), 0);
        rrect(hdc, xpos, ypos, width, height - side, g_unit * 0.1, lit ? COL_ACCENT : tone(COL_KEY_TOP, dim),
              lit ? COL_ON_ACCENT : tone(COL_KEY_EDGE, dim), lit ? 2.0 : 1.0);
        if (g_overlay && !binding_mode()) {
            /* the shift keys carry a coloured dot: green, red, blue */
            const int shift = astrosession_overlay_key_shift(key_of(ctl));
            if (shift > ASTRO_SHIFT_NONE && shift < ASTRO_SHIFT_WORDS)
                ellipse(hdc, xpos + width / 2, ypos + (height - side) / 2, g_unit * 0.2, g_unit * 0.2,
                        shift_colour(shift), tone(COL_KEY_EDGE, dim), 0.8);
        }
    }
}

/* ---- a player's card ------------------------------------------------------------- */

/* A point on a card from card units. */
#define CARD_X(p, ux) (g_card[p].ox + (ux) * g_unit)
#define CARD_Y(p, uy) (g_card[p].oy + (uy) * g_unit)

/* The Bally hand controller as the photos show it from the side: a glossy
 * black pistol grip, the trigger at the front (right), a recess of diamond
 * knurling down the front of the handle and the round gold badge on the
 * shoulder. */
static void art_grip(HDC hdc, int player, int dim)
{
    static const double shell[][2] = {
        { 0.55, 3.10 }, { 3.05, 3.10 }, { 3.20, 3.95 }, { 2.50, 4.05 }, { 2.55, 5.20 }, { 0.85, 5.25 },
        { 0.62, 4.15 },
    };
    POINT pnts[8];
    int idx, col, row;
    for (idx = 0; idx < 7; idx++) {
        pnts[idx].x = art_xpos(CARD_X(player, shell[idx][0]));
        pnts[idx].y = art_ypos(CARD_Y(player, shell[idx][1]));
    }
    poly(hdc, pnts, 7, tone(COL_SHELL, dim), tone(COL_SHELL_HI, dim), 1.6);
    /* the gloss on the shoulder and down the back of the handle, and the
     * chrome parting line between the shell halves */
    line_seg(hdc, CARD_X(player, 0.78), CARD_Y(player, 3.3), CARD_X(player, 0.72), CARD_Y(player, 4.1),
             tone(COL_SHELL_HI, dim), 2.0);
    line_seg(hdc, CARD_X(player, 0.9), CARD_Y(player, 4.25), CARD_X(player, 0.98), CARD_Y(player, 5.1),
             tone(mix_color(COL_SHELL_HI, COL_SHELL, 0.4), dim), 1.4);
    line_seg(hdc, CARD_X(player, 2.4), CARD_Y(player, 3.2), CARD_X(player, 2.45), CARD_Y(player, 4.0),
             tone(mix_color(COL_SHELL_HI, COL_SHELL, 0.5), dim), 1.2);
    /* the recessed knurled panel down the front of the handle */
    rrect(hdc, CARD_X(player, 0.98), CARD_Y(player, 4.28), 1.28 * g_unit, 0.8 * g_unit, g_unit * 0.1,
          tone(COL_PANEL, dim), tone(COL_SHELL_HI, dim), 0.7);
    for (row = 0; row < 5; row++)
        for (col = 0; col < 7; col++) {
            const double dotx = CARD_X(player, 1.08 + col * 0.18 + (row & 1) * 0.09);
            const double doty = CARD_Y(player, 4.38 + row * 0.15);
            ellipse(hdc, dotx, doty, g_unit * 0.03, g_unit * 0.03, tone(COL_KNURL, dim), tone(COL_KNURL, dim), 0);
        }
    /* the round gold Bally badge, above the knurled panel and clear of the knob ring */
    ellipse(hdc, CARD_X(player, 1.62), CARD_Y(player, 4.0), g_unit * 0.22, g_unit * 0.22, tone(COL_BADGE, dim),
            tone(COL_BADGE_IN, dim), 1.0);
    ellipse(hdc, CARD_X(player, 1.62), CARD_Y(player, 4.0), g_unit * 0.14, g_unit * 0.14, tone(COL_BADGE_IN, dim),
            tone(COL_BADGE, dim), 0.8);
}

static void art_trigger(HDC hdc, const astro_control *ctl)
{
    double xpos, ypos, width, height;
    const int lit = part_lit(ctl), dim = part_dim(ctl);
    part_box(ctl, &xpos, &ypos, &width, &height);
    /* a pale lever curved into the grip: a tall pill, lit in the accent */
    rrect(hdc, xpos + g_unit * 0.05, ypos + g_unit * 0.06, width - g_unit * 0.05, height - g_unit * 0.02,
          g_unit * 0.3, tone(COL_TRIG_EDGE, dim), tone(COL_TRIG_EDGE, dim), 0);
    grad_rrect(hdc, xpos, ypos, width - g_unit * 0.04, height - g_unit * 0.06, g_unit * 0.3,
               lit ? COL_ACCENT : tone(COL_TRIG_A, dim), lit ? COL_ACCENT : tone(COL_TRIG_B, dim),
               lit ? COL_ON_ACCENT : tone(COL_TRIG_EDGE, dim), lit ? 2.0 : 1.0);
}

/* The ring of eight wedges round the knob. */
static void art_ring(HDC hdc, int player)
{
    const double ctrx = g_card[player].ctrx, ctry = g_card[player].ctry, ring = g_card[player].ring;
    const double inner = g_card[player].dial + g_unit * 0.05;
    const int mapping = binding_mode();
    int idx;
    ellipse(hdc, ctrx, ctry, ring * 1.05, ring * 1.05, tone(COL_RING, g_map_state >= 0), tone(COL_WEDGE_EDG, g_map_state >= 0), 1.0);
    for (idx = 0; idx < g_numlayout; idx++) {
        const astro_control *ctl = &g_layout[idx];
        const int lit = part_lit(ctl), dim = part_dim(ctl);
        int sector;
        if (ctl->kind != ASTRO_CTL_DIRECTION || ctl->player != player) continue;
        sector = wedge_sector(ctl->act_mask);
        wedge(hdc, ctrx, ctry, ring, inner, sector, lit ? COL_ACCENT : tone(COL_WEDGE, dim),
              lit ? COL_ON_ACCENT : tone(COL_WEDGE_EDG, dim));
        if (!mapping) {
            const double mid = (ring + inner) / 2, ang = sector * 45.0 * PI_DEG / 180.0;
            arrow(hdc, ctrx + mid * cos(ang), ctry - mid * sin(ang), sector * 45.0, g_unit * 0.1,
                  lit ? COL_ON_ACCENT : tone(COL_GLYPH, dim));
        }
    }
}

/* The ribbed orange-brown knob on its dark collar, with a pointer that shows
 * the paddle's position. */
static void art_knob(HDC hdc, int player, const astro_control *knob)
{
    const double ctrx = g_card[player].ctrx, ctry = g_card[player].ctry, rad = g_card[player].dial;
    const int dim = part_dim(knob);
    const int picked = g_map_state >= 0 && is_knob_target(g_map_state) &&
                       (g_map_state - ASTRO_TARGET_PLAYERS) / ASTRO_ACT_PER_PLAYER == player;
    double ang;
    int idx;
    ellipse(hdc, ctrx, ctry, rad * 1.1, rad * 1.1, tone(COL_KNOB_LO, dim), picked ? COL_ACCENT : tone(COL_SHELL, dim),
            picked ? 2.0 : 1.0);
    ellipse(hdc, ctrx, ctry, rad * 0.96, rad * 0.96, tone(COL_KNOB, dim), tone(COL_KNOB_LO, dim), 1.0);
    for (idx = 0; idx < 20; idx++) {        /* the flutes */
        const double fang = idx * 18.0 * PI_DEG / 180.0;
        line_seg(hdc, ctrx + rad * 0.6 * cos(fang), ctry - rad * 0.6 * sin(fang), ctrx + rad * 0.93 * cos(fang),
                 ctry - rad * 0.93 * sin(fang), tone(COL_FLUTE, dim), 1.0);
    }
    ellipse(hdc, ctrx - rad * 0.06, ctry - rad * 0.08, rad * 0.56, rad * 0.56, tone(COL_KNOB_HI, dim),
            tone(COL_KNOB, dim), 0);
    ellipse(hdc, ctrx, ctry, rad * 0.46, rad * 0.46, tone(COL_KNOB, dim), tone(COL_KNOB_LO, dim), 1.0);
    if (!binding_mode()) {
        ang = astrosession_knob_angle(g_knob_shown[player]) * PI_DEG / 180.0;
        line_seg(hdc, ctrx + rad * 0.5 * sin(ang), ctry - rad * 0.5 * cos(ang), ctrx + rad * 0.92 * sin(ang),
                 ctry - rad * 0.92 * cos(ang), COL_POINTER, 2.0);
    }
}

static void art_card(HDC hdc, int player)
{
    const card_geo *geo = &g_card[player];
    int idx;
    grad_rrect(hdc, geo->ox, geo->oy, geo->width, geo->bottom - geo->oy, g_unit * 0.16, COL_CARD_A, COL_CARD_B,
               COL_CARD_EDGE, 1.0);
    art_grip(hdc, player, g_map_state >= 0);
    for (idx = 0; idx < g_numlayout; idx++) {
        const astro_control *ctl = &g_layout[idx];
        if (ctl->player != player) continue;
        if (ctl->kind == ASTRO_CTL_TRIGGER) art_trigger(hdc, ctl);
    }
    art_ring(hdc, player);
    for (idx = 0; idx < g_numlayout; idx++)
        if (g_layout[idx].kind == ASTRO_CTL_KNOB && g_layout[idx].player == player)
            art_knob(hdc, player, &g_layout[idx]);
}

static void art_sysbuttons(HDC hdc)
{
    int idx;
    for (idx = 0; idx < g_numlayout; idx++) {
        const astro_control *ctl = &g_layout[idx];
        double xpos, ypos, width, height;
        const int lit = part_lit(ctl), dim = part_dim(ctl);
        const int pressed = idx == g_mouse_part;
        if (ctl->kind != ASTRO_CTL_SYSBUTTON) continue;
        part_box(ctl, &xpos, &ypos, &width, &height);
        grad_rrect(hdc, xpos, ypos + (pressed ? 1 : 0), width, height - (pressed ? 1 : 0), g_unit * 0.14,
                   lit ? COL_ACCENT : tone(mix_color(COL_SYS, RGB(0x44, 0x40, 0x3A), 0.5), dim),
                   lit ? COL_ACCENT : tone(COL_SYS, dim), lit ? COL_ON_ACCENT : tone(COL_SYS_EDGE, dim), 1.2);
    }
}

/* The console: walnut sides, the black top with its gold pinstripe, the
 * "Bally ARCADE" label, then the keypad, the four hand controllers and the
 * console's buttons. */
static void art_case(HDC hdc)
{
    int player;
    grad_rrect(hdc, g_case_rc.left, g_case_rc.top, g_case_rc.right - g_case_rc.left,
               g_case_rc.bottom - g_case_rc.top, SCL(16), COL_WALNUT_TOP, COL_WALNUT_BOT, COL_WALNUT_EDG, 1.5);
    grad_rrect(hdc, g_case_rc.left + SCL(6), g_case_rc.top + SCL(6), g_case_rc.right - g_case_rc.left - SCL(12),
               g_case_rc.bottom - g_case_rc.top - SCL(12), SCL(11), COL_TOP_A, COL_TOP_B, COL_PINSTRIPE, 1.2);
    rrect(hdc, g_brand_rc.left, g_brand_rc.top, g_brand_rc.right - g_brand_rc.left,
          g_brand_rc.bottom - g_brand_rc.top, SCL(6), COL_BRAND_BG, COL_PINSTRIPE, 1.5);
    if (g_plate >= 0) art_plate(hdc, &g_layout[g_plate]);
    art_keys(hdc);
    for (player = 0; player < PLAYERS; player++) art_card(hdc, player);
    art_sysbuttons(hdc);
}

/* In Map mode and with Show bindings a target shows what drives it. */
static void binding_label(int target, char *out, int outsz)
{
    const uint32_t keysym = bindings_target_keysym(target);
    out[0] = '\0';
    if (keysym) {
        astrosession_keysym_name(keysym, out, outsz);
        /* the keycaps are small: "Keypad 5" reads "KP 5" */
        if (strncmp(out, "Keypad ", 7) == 0) {
            char shortname[48];
            snprintf(shortname, sizeof shortname, "KP %.40s", out + 7);
            snprintf(out, (size_t)outsz, "%s", shortname);
        }
    }
    if (!out[0]) snprintf(out, (size_t)outsz, "%s", "\xe2\x80\x94");
}

/* A pad input's name for a target ("A", "Right Stick X"), or "none". */
static void pad_label(int target, char *out, int outsz)
{
    const int pad = bindings_target_pad(target);
    out[0] = '\0';
    if (pad >= 0) astrosession_pad_input_name(pad, out, outsz);
    if (!out[0]) snprintf(out, (size_t)outsz, "%s", "none");
}

/* A pad axis on a small face: "Right Stick X" is RX, "Left Trigger" LT. */
static void axis_short(const char *name, char *out, int outsz)
{
    if (strncmp(name, "Left Stick ", 11) == 0) snprintf(out, (size_t)outsz, "L%.1s", name + 11);
    else if (strncmp(name, "Right Stick ", 12) == 0) snprintf(out, (size_t)outsz, "R%.1s", name + 12);
    else if (strncmp(name, "Left Trigger", 12) == 0) snprintf(out, (size_t)outsz, "LT");
    else if (strncmp(name, "Right Trigger", 13) == 0) snprintf(out, (size_t)outsz, "RT");
    else snprintf(out, (size_t)outsz, "%.8s", name);
}

/* The key's rectangle, and the legend zone above it. */
static void zone_rect(const RECT *key, RECT *zone)
{
    zone->left = key->left;
    zone->right = key->right;
    zone->bottom = key->top - SCL(2);
    zone->top = key->top - (int)floor(g_unit * 0.5);
}

/* The text of the overlay on one key: the three shift-colour characters in the
 * legend bar, the gold word above them, and the white legend on the key. */
static void text_overlay_key(HDC hdc, int key, const RECT *face, const RECT *zone, int dim)
{
    const astro_overlay_key *ov = &astrosession_overlay_layout()[key];
    const int shift = astrosession_overlay_key_shift(key);
    RECT top = *zone, bottom = *zone, cell;
    int idx;
    const int third = (zone->right - zone->left) / 3;

    top.bottom = zone->top + ((zone->bottom - zone->top) * 9) / 20;     /* the words: the upper 45% */
    bottom.top = top.bottom;                                            /* the letters: the rest */
    SelectObject(hdc, g_ovword);
    if (ov->legend[ASTRO_SHIFT_WORDS]) {
        SetTextColor(hdc, tone(COL_OV_WORDS, dim));
        text_line(hdc, ov->legend[ASTRO_SHIFT_WORDS], &top, DT_CENTER);
    }
    SelectObject(hdc, g_ovsmall);
    for (idx = ASTRO_SHIFT_GREEN; idx <= ASTRO_SHIFT_BLUE; idx++) {
        if (!ov->legend[idx]) continue;
        cell = bottom;
        cell.left = zone->left + (idx - 1) * third;
        cell.right = cell.left + third;
        SetTextColor(hdc, tone(shift_colour(idx), dim));
        text_line(hdc, ov->legend[idx], &cell, DT_CENTER);
    }
    if (shift == ASTRO_SHIFT_WORDS) {   /* the gold shift: its own word on the key */
        RECT word = *face;
        word.bottom -= SCL(3);
        SetTextColor(hdc, tone(COL_OV_WORDS, dim));
        text_line(hdc, "WORDS", &word, DT_CENTER);
    } else if (ov->legend[ASTRO_SHIFT_NONE]) {
        RECT main = *face;
        main.bottom -= SCL(3);
        SetTextColor(hdc, tone(COL_OV_WHITE, dim));
        SelectObject(hdc, g_digit);
        if (text_width(hdc, ov->legend[ASTRO_SHIFT_NONE]) > (main.right - main.left) - SCL(4))
            SelectObject(hdc, g_tiny);
        if (text_width(hdc, ov->legend[ASTRO_SHIFT_NONE]) > (main.right - main.left) - SCL(2))
            SelectObject(hdc, g_ovsmall);
        text_line(hdc, ov->legend[ASTRO_SHIFT_NONE], &main, DT_CENTER);
    }
}

/* The labels and legends over the art, at the window's own size. */
static void text_parts(HDC hdc)
{
    int idx;
    char legend[96];
    SetBkMode(hdc, TRANSPARENT);

    /* the "Bally ARCADE" label */
    {
        RECT left = g_brand_rc, right = g_brand_rc;
        const int mid = g_brand_rc.left + (g_brand_rc.right - g_brand_rc.left) * 4 / 10;
        left.right = mid;
        right.left = mid;
        SelectObject(hdc, g_brand);
        SetTextColor(hdc, COL_BRAND_RED);
        text_line(hdc, "Bally", &left, DT_RIGHT);
        SetTextColor(hdc, COL_BRAND_INK);
        right.left += SCL(4);
        text_line(hdc, "ARCADE", &right, DT_LEFT);
    }

    for (idx = 0; idx < g_numlayout; idx++) {
        const astro_control *ctl = &g_layout[idx];
        RECT rect;
        const int lit = part_lit(ctl), dim = part_dim(ctl);
        part_rect(ctl, &rect);
        switch (ctl->kind) {
        case ASTRO_CTL_CAPTION:
            SelectObject(hdc, g_caption);
            SetTextColor(hdc, tone(COL_CAPTION, g_map_state >= 0));
            text_line(hdc, ctl->label, &rect, DT_CENTER);
            break;
        case ASTRO_CTL_KEY: {
            const int key = key_of(ctl);
            RECT face = rect, zone;
            face.bottom -= SCL(3);
            zone_rect(&rect, &zone);
            if (key < 0) break;
            if (g_overlay) {
                text_overlay_key(hdc, key, &face, &zone, dim);
                if (binding_mode()) {
                    /* the key's own white legend gives way to what drives it */
                    binding_label(ctl->target, legend, sizeof legend);
                    SelectObject(hdc, g_tiny);
                    SetTextColor(hdc, lit ? COL_ON_ACCENT : tone(COL_KEY_INK, dim));
                    {
                        RECT clear = face;
                        InflateRect(&clear, -SCL(2), -SCL(2));
                        HBRUSH back = CreateSolidBrush(lit ? COL_ACCENT : tone(COL_KEY_TOP, dim));
                        FillRect(hdc, &clear, back);
                        DeleteObject(back);
                    }
                    text_line(hdc, legend, &face, DT_CENTER | DT_END_ELLIPSIS);
                }
            } else {
                SelectObject(hdc, g_legend);
                SetTextColor(hdc, tone(COL_CONSOLE_INK, dim));
                text_line(hdc, k_console[key], &zone, DT_CENTER);
                if (binding_mode()) {
                    binding_label(ctl->target, legend, sizeof legend);
                    SelectObject(hdc, g_tiny);
                    SetTextColor(hdc, lit ? COL_ON_ACCENT : tone(COL_KEY_INK, dim));
                    text_line(hdc, legend, &face, DT_CENTER | DT_END_ELLIPSIS);
                }
            }
            break;
        }
        case ASTRO_CTL_TRIGGER:
            if (binding_mode()) {
                binding_label(ctl->target, legend, sizeof legend);
                SelectObject(hdc, g_tiny);
                SetTextColor(hdc, lit ? COL_ON_ACCENT : tone(COL_ON_ACCENT, dim));
                text_block(hdc, legend, &rect);
            }
            break;
        case ASTRO_CTL_DIRECTION:
            if (binding_mode() && ctl->target >= 0) {
                /* a wedge's box is its bounding box: the legend sits at the middle of the sector */
                const double ang = wedge_sector(ctl->act_mask) * 45.0 * PI_DEG / 180.0;
                const double rad = (g_card[ctl->player].ring + g_card[ctl->player].dial) / 2;
                const int midx = (int)(g_card[ctl->player].ctrx + rad * cos(ang));
                const int midy = (int)(g_card[ctl->player].ctry - rad * sin(ang));
                RECT box;
                binding_label(ctl->target, legend, sizeof legend);
                SelectObject(hdc, g_ovsmall);
                SetTextColor(hdc, lit ? COL_ON_ACCENT : tone(COL_GLYPH, dim));
                SetRect(&box, midx - SCL(16), midy - SCL(6), midx + SCL(16), midy + SCL(6));
                text_line(hdc, legend, &box, DT_CENTER | DT_END_ELLIPSIS);
            }
            break;
        case ASTRO_CTL_KNOB:
            if (binding_mode()) {
                /* left key, right key and the axis: what Map picks, by where on the dial */
                const double ctrx = g_card[ctl->player].ctrx, ctry = g_card[ctl->player].ctry;
                const double rad = g_card[ctl->player].dial;
                int zone;
                SelectObject(hdc, g_ovsmall);
                for (zone = 0; zone < 3; zone++) {
                    const int target = knob_target(ctl->player, zone);
                    const int picked = g_map_state == target;
                    RECT box;
                    const int px = (int)(ctrx + (zone == 0 ? -0.5 * rad : zone == 1 ? 0.5 * rad : 0));
                    const int py = (int)(ctry + (zone == 2 ? 0.5 * rad : -0.08 * rad));
                    SetRect(&box, px - SCL(13), py - SCL(6), px + SCL(13), py + SCL(6));
                    if (zone == 2) {                       /* the axis: a pad's */
                        char full[48];
                        pad_label(target, full, sizeof full);
                        axis_short(full, legend, sizeof legend);
                    } else {
                        binding_label(target, legend, sizeof legend);
                    }
                    SetTextColor(hdc, picked ? COL_ACCENT : RGB(0xFF, 0xF0, 0xDC));
                    text_line(hdc, legend, &box, DT_CENTER | DT_END_ELLIPSIS);
                }
            } else {
                char num[16];
                RECT box = rect;
                SelectObject(hdc, g_knobnum);
                SetTextColor(hdc, RGB(0xF6, 0xDC, 0xB8));
                snprintf(num, sizeof num, "%d", ctl->player + 1);
                text_line(hdc, num, &box, DT_CENTER);
            }
            break;
        case ASTRO_CTL_SYSBUTTON: {
            const int pressed = idx == g_mouse_part;
            RECT face = rect;
            if (pressed) OffsetRect(&face, 0, 1);
            SelectObject(hdc, g_sys);
            SetTextColor(hdc, lit ? COL_ON_ACCENT : tone(COL_SYS_INK, dim));
            if (binding_mode() && ctl->target >= 0) {
                char both[160];
                binding_label(ctl->target, legend, sizeof legend);
                snprintf(both, sizeof both, "%s: %.40s",
                         ctl->target == ASTRO_TARGET_SYSACT + ASTROSESSION_SYSACT_RESET_GAME ? "Game" : "CONFIG", legend);
                SelectObject(hdc, g_tiny);
                text_line(hdc, both, &face, DT_CENTER | DT_END_ELLIPSIS);
            } else {
                text_line(hdc, ctl->target == ASTRO_TARGET_SYSACT + ASTROSESSION_SYSACT_RESET_GAME ? "RESET GAME"
                                                                                                    : "Reset to CONFIG",
                          &face, DT_CENTER);
            }
            break;
        }
        default:
            break;
        }
    }
}

/* ---- the strip: gamepads, Map, the overlay ------------------------------------------- */

static void draw_face(HDC hdc, const RECT *bounds, int pushed, int accent, int inactive)
{
    RECT rect = *bounds;
    if (accent) {
        HBRUSH back = CreateSolidBrush(COL_ACCENT);
        FillRect(hdc, &rect, back);
        DeleteObject(back);
        FrameRect(hdc, &rect, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetTextColor(hdc, COL_ON_ACCENT);
    } else {
        DrawFrameControl(hdc, &rect, DFC_BUTTON,
                         DFCS_BUTTONPUSH | (pushed ? DFCS_PUSHED : 0) | (inactive ? DFCS_INACTIVE : 0));
        SetTextColor(hdc, GetSysColor(inactive ? COLOR_GRAYTEXT : COLOR_BTNTEXT));
    }
}

static const char *strip_label(int btn)
{
    switch (btn) {
    case SB_MAP: return g_map_state == -2 ? "Map" : "Done";
    case SB_CLEAR: return "Clear";
    case SB_DEFAULTS: return "Defaults";
    case SB_SHOW: return "Show bindings";
    case SB_OVERLAY: return "Overlay: Bally BASIC";
    default: return "Help";
    }
}

/* The segment of a pad row: 0 is Automatic, 1-4 the players. */
static void segment_rect(int row, int seg, RECT *rect)
{
    const RECT *rowrc = &g_gp_row_rc[row];
    const int gap = SCL(3), auto_w = SCL(48), play_w = SCL(64);
    const int total = auto_w + 4 * play_w + 4 * gap;
    int left = rowrc->right - total;
    if (seg > 0) left += auto_w + gap + (seg - 1) * (play_w + gap);
    SetRect(rect, left, rowrc->top, left + (seg == 0 ? auto_w : play_w), rowrc->bottom - 1);
}

/* One line per gamepad: its name and the player it drives now, then the
 * Automatic / Player 1-4 choice, the current one in the accent. */
static void paint_gamepads(HDC hdc)
{
    static const char *const segs[SEGS] = { "Auto", "Player 1", "Player 2", "Player 3", "Player 4" };
    const int num = astrosession_gamepad_count(g_session);
    int idx, seg;

    SelectObject(hdc, g_bold);
    SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
    text_line(hdc, "Gamepads", &g_gp_head_rc, DT_LEFT);
    SelectObject(hdc, g_font);
    SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
    if (num == 0) {
        text_line(hdc, "No gamepads connected - plug one in", &g_gp_row_rc[0], DT_LEFT);
        return;
    }
    if (num > PAD_ROWS) {
        char more[48];
        snprintf(more, sizeof more, "%d more not shown", num - PAD_ROWS);
        text_line(hdc, more, &g_gp_head_rc, DT_RIGHT);
    }
    for (idx = 0; idx < num && idx < PAD_ROWS; idx++) {
        const int assign = astrosession_gamepad_assignment(g_session, idx);
        const int effective = astrosession_gamepad_effective_port(g_session, idx);
        const int chosen = assign < 0 || assign >= PLAYERS ? 0 : assign + 1;
        char name[128], line[200];
        RECT name_rc, first;

        segment_rect(idx, 0, &first);
        name_rc = g_gp_row_rc[idx];
        name_rc.right = first.left - SCL(6);
        astrosession_gamepad_name(g_session, idx, name, sizeof name);
        if (effective >= 0) snprintf(line, sizeof line, "%s  (player %d)", name, effective + 1);
        else snprintf(line, sizeof line, "%s  (no player)", name);
        SelectObject(hdc, g_font);
        SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
        text_line(hdc, line, &name_rc, DT_LEFT | DT_END_ELLIPSIS);
        for (seg = 0; seg < SEGS; seg++) {
            RECT rect;
            segment_rect(idx, seg, &rect);
            draw_face(hdc, &rect, 0, seg == chosen, 0);
            text_line(hdc, segs[seg], &rect, DT_CENTER);
        }
    }
}

/* What the hint line says about the part under an idle pointer. */
static void hover_text(int part, char *out, int outsz)
{
    const astro_control *ctl;
    char key[48], pad[48];
    out[0] = '\0';
    if (part < 0 || part >= g_numlayout) return;
    ctl = &g_layout[part];
    if (ctl->kind == ASTRO_CTL_KNOB) {
        snprintf(out, (size_t)outsz, "Player %d knob: drag the dial, or scroll over it", ctl->player + 1);
        return;
    }
    if (ctl->target < 0) return;
    binding_label(ctl->target, key, sizeof key);
    pad_label(ctl->target, pad, sizeof pad);
    snprintf(out, (size_t)outsz, "%s   Key: %s   Gamepad: %s", bindings_target_label(ctl->target), key, pad);
}

static void paint_strip(HDC hdc)
{
    int btn;
    char hover[200];
    SelectObject(hdc, g_font);
    for (btn = 0; btn < SB_COUNT; btn++) {
        const int armed = (btn == SB_MAP && g_map_state != -2) || (btn == SB_SHOW && g_show_bindings) ||
                          (btn == SB_OVERLAY && g_overlay);
        const int inactive = btn == SB_CLEAR && g_map_state < 0;
        draw_face(hdc, &g_btn_rc[btn], 0, armed, inactive);
        text_line(hdc, strip_label(btn), &g_btn_rc[btn], DT_CENTER);
    }
    SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
    hover_text(g_map_state >= 0 ? -1 : g_hover_part, hover, sizeof hover);
    text_line(hdc, hover[0] ? hover : g_hint, &g_hint_rc, DT_LEFT | DT_END_ELLIPSIS);
}

/* The whole window: the console's art supersampled, its text, the strips.
 * Only what the update region needs is drawn. */
static void paint_rect(HDC hdc, const RECT *inval)
{
    RECT art;
    const int scale = 2;
    HDC mem;
    HBITMAP bmp;
    void *bits = NULL;
    BITMAPINFO bmi;
    HGDIOBJ old;

    FillRect(hdc, inval, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(hdc, TRANSPARENT);

    /* the art: the intersection with the case, at 2x */
    IntersectRect(&art, inval, &g_case_rc);
    if (!IsRectEmpty(&art)) {
        const int width = art.right - art.left, height = art.bottom - art.top;
        memset(&bmi, 0, sizeof bmi);
        bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
        bmi.bmiHeader.biWidth = width * scale;
        bmi.bmiHeader.biHeight = -height * scale;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        mem = CreateCompatibleDC(hdc);
        bmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (bmp) {
            HBRUSH face = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
            RECT all = { 0, 0, width * scale, height * scale };
            old = SelectObject(mem, bmp);
            FillRect(mem, &all, face);
            DeleteObject(face);
            SetBkMode(mem, TRANSPARENT);
            g_art_k = scale;
            g_art_x = art.left;
            g_art_y = art.top;
            g_art_rows = height * scale;
            art_case(mem);
            g_art_k = 1;
            SetStretchBltMode(hdc, HALFTONE);
            SetBrushOrgEx(hdc, 0, 0, NULL);
            StretchBlt(hdc, art.left, art.top, width, height, mem, 0, 0, width * scale, height * scale, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
        }
        DeleteDC(mem);
        SelectObject(hdc, g_font);
        text_parts(hdc);
    }
    paint_gamepads(hdc);
    paint_strip(hdc);
}

/* ---- pressing ----------------------------------------------------------------- */

static void invalidate_part(int part)
{
    RECT rect;
    if (!g_panel || part < 0 || part >= g_numlayout) return;
    part_rect(&g_layout[part], &rect);
    InflateRect(&rect, SCL(14), SCL(14));
    InvalidateRect(g_panel, &rect, FALSE);
}

static void invalidate_card(int player)
{
    RECT rect;
    if (!g_panel || player < 0 || player >= PLAYERS) return;
    SetRect(&rect, (int)g_card[player].ox - SCL(2), (int)g_card[player].oy - SCL(2),
            (int)(g_card[player].ox + g_card[player].width) + SCL(2), (int)g_card[player].bottom + SCL(2));
    InvalidateRect(g_panel, &rect, FALSE);
}

static void set_pointer(int player, unsigned mask)
{
    if (player < 0 || player >= PLAYERS || g_pointer[player] == mask) return;
    g_pointer[player] = mask;
    astrosession_pointer_set_player(g_session, player, mask);
}

/* Lets go of what the pointer holds in this window (only that: the main
 * window releases its own keys). A console button fires on release inside
 * it, like any button. */
static void mouse_release(int fire_sys)
{
    const int part = g_mouse_part, player = g_mouse_player, knob = g_mouse_knob;
    int idx;
    g_mouse_part = -1;
    g_mouse_player = -1;
    g_mouse_knob = -1;
    if (g_session) astrosession_pointer_release(g_session);
    for (idx = 0; idx < PLAYERS; idx++) g_pointer[idx] = 0;
    if (part >= 0 && part < g_numlayout) {
        const astro_control *ctl = &g_layout[part];
        if (fire_sys && ctl->kind == ASTRO_CTL_SYSBUTTON && ctl->target >= ASTRO_TARGET_SYSACT &&
            ctl->target < ASTRO_TARGET_PLAYERS)
            astrosession_sysaction_post(g_session, (astrosession_sysaction)(ctl->target - ASTRO_TARGET_SYSACT));
        invalidate_part(part);
    }
    if (player >= 0) invalidate_card(player);
    if (knob >= 0) invalidate_card(knob);
}

static void set_hint(const char *text)
{
    snprintf(g_hint, sizeof g_hint, "%s", text);
    g_hover_part = -1;
    if (g_panel) InvalidateRect(g_panel, &g_hint_rc, FALSE);
}

#define IDLE_HINT "Press a key or the trigger to hold it, drag a grip's wedges to tilt it, turn its knob."

static void set_map_state(int state)
{
    g_map_state = state;
    if (state < 0) {
        if (g_panel) KillTimer(g_panel, IDT_CAPTURE);
        astrosession_gamepad_capture_cancel(g_session);
    }
    if (state == -2) {
        set_hint(IDLE_HINT);
    } else if (state == -1) {
        set_hint("Click a control to remap (the dial: left, right or lower half). Esc finishes.");
    } else {
        char msg[200];
        mouse_release(0);
        snprintf(msg, sizeof msg,
                 is_knob_axis_target(state) ? "Move a gamepad stick or trigger for %s. Esc cancels."
                                            : "Press a key or a gamepad button for %s. Esc cancels.",
                 bindings_target_label(state));
        set_hint(msg);
        if (is_knob_axis_target(state)) astrosession_gamepad_capture_begin_axis(g_session);
        else astrosession_gamepad_capture_begin(g_session);
        if (g_panel) SetTimer(g_panel, IDT_CAPTURE, 50, NULL);
    }
    if (g_panel) {
        InvalidateRect(g_panel, NULL, FALSE);
        refresh_held();
    }
}

/* After a rebind: what it is now, and whom it was taken from. */
static void bound_hint(const char *target, const char *what, int from)
{
    char msg[300];
    if (from >= 0) snprintf(msg, sizeof msg, "%s = %s (was %s)", target, what, bindings_target_label(from));
    else snprintf(msg, sizeof msg, "%s = %s", target, what);
    set_hint(msg);
}

/* ---- layout --------------------------------------------------------------------- */

/* The case with the keypad, the four controllers and the console buttons in
 * it, then the gamepad rows and the strip: the window is this size always. */
static void layout_static(void)
{
    const int left = SCL(MARGIN);
    int ypos = SCL(MARGIN), idx, xpos, content_w;
    static const int widths[SB_COUNT] = { 52, 54, 72, 108, 156, 54 };

    g_layout = astrosession_controls_layout(&g_numlayout, &g_layw, &g_layh);
    g_unit = (double)UNIT96 * g_dpi / 96.0;
    content_w = (int)ceil((double)g_layw * g_unit) + 2 * SCL(CASE_PAD);
    g_total_w = 2 * left + content_w;

    SetRect(&g_case_rc, left, ypos, left + content_w, 0);
    SetRect(&g_brand_rc, left + (content_w - SCL(BRAND_W)) / 2, g_case_rc.top + SCL(10),
            left + (content_w + SCL(BRAND_W)) / 2, g_case_rc.top + SCL(10) + SCL(BRAND_H));
    g_orgx = g_case_rc.left + (content_w - (double)g_layw * g_unit) / 2;
    g_orgy = g_brand_rc.bottom + SCL(12);
    g_case_rc.bottom = (int)ceil(g_orgy + (double)g_layh * g_unit) + SCL(CASE_PAD);
    ypos = g_case_rc.bottom + SCL(10);

    g_plate = -1;
    memset(g_card, 0, sizeof g_card);
    for (idx = 0; idx < g_numlayout; idx++) {
        const astro_control *ctl = &g_layout[idx];
        const int player = ctl->player;
        if (ctl->kind == ASTRO_CTL_OVERLAY) g_plate = idx;
        if (player < 0 || player >= PLAYERS) continue;
        switch (ctl->kind) {
        case ASTRO_CTL_CAPTION:
            g_card[player].ox = g_orgx + (double)ctl->xpos * g_unit;
            g_card[player].oy = g_orgy + (double)ctl->ypos * g_unit;
            g_card[player].width = (double)ctl->width * g_unit;
            break;
        case ASTRO_CTL_STICK:
            g_card[player].ctrx = g_orgx + ((double)ctl->xpos + ctl->width / 2) * g_unit;
            g_card[player].ctry = g_orgy + ((double)ctl->ypos + ctl->height / 2) * g_unit;
            g_card[player].ring = (double)ctl->width / 2 * g_unit;
            break;
        case ASTRO_CTL_KNOB:
            g_card[player].dial = (double)ctl->width / 2 * g_unit;
            break;
        case ASTRO_CTL_TRIGGER:
            g_card[player].trig = idx;
            /* the tile reaches a little below the lever */
            g_card[player].bottom = g_orgy + ((double)ctl->ypos + ctl->height + 0.2) * g_unit;
            break;
        default:
            break;
        }
    }

    SetRect(&g_gp_head_rc, left, ypos, left + content_w, ypos + SCL(HEAD_H));
    ypos += SCL(HEAD_H);
    for (idx = 0; idx < PAD_ROWS; idx++) {
        SetRect(&g_gp_row_rc[idx], left, ypos, left + content_w, ypos + SCL(ROW_H));
        ypos += SCL(ROW_H + ROW_GAP);
    }
    SetRect(&g_gp_rc, left, g_gp_head_rc.top, left + content_w, ypos);
    ypos += SCL(6);

    xpos = left;
    for (idx = 0; idx < SB_COUNT; idx++) {
        if (idx == SB_HELP) xpos = left + content_w - SCL(widths[idx]);
        SetRect(&g_btn_rc[idx], xpos, ypos, xpos + SCL(widths[idx]), ypos + SCL(BTN_H));
        xpos += SCL(widths[idx]) + SCL(6);
    }
    ypos += SCL(BTN_H) + SCL(6);
    SetRect(&g_hint_rc, left, ypos, left + content_w, ypos + SCL(HINT_H));
    g_total_h = ypos + SCL(HINT_H) + SCL(MARGIN);
}

static void rebuild(void)
{
    mouse_release(0);
    if (g_panel && GetCapture() == g_panel) ReleaseCapture();
    make_fonts();
    layout_static();
    if (g_panel) {
        InvalidateRect(g_panel, NULL, FALSE);
        refresh_held();
    }
}

/* ---- polling -------------------------------------------------------------------------- */

/* What the console sees held, lit; the live knob; the overlay. */
static void refresh_held(void)
{
    int player, key, act;
    int overlay;
    if (!g_panel) return;
    overlay = astrosession_overlay_get(g_session) != 0;
    if (overlay != g_overlay) {
        g_overlay = overlay;
        InvalidateRect(g_panel, NULL, FALSE);
    }
    if (g_map_state != -2) return;
    for (key = 0; key < KEYS; key++) {
        const unsigned char now = (unsigned char)(astrosession_target_held(g_session, ASTRO_TARGET_KEYPAD + key) != 0);
        if (now != g_key_held[key]) {
            int idx;
            g_key_held[key] = now;
            for (idx = 0; idx < g_numlayout; idx++)
                if (key_of(&g_layout[idx]) == key) invalidate_part(idx);
        }
    }
    for (player = 0; player < PLAYERS; player++) {
        unsigned held = 0;
        int knob;
        for (act = 0; act < ASTRO_ACT_KNOB; act++)
            if (astrosession_target_held(g_session, ASTRO_TARGET_PLAYER(player, act))) held |= 1u << act;
        knob = astrosession_knob_get(g_session, player);
        if (held != g_act_held[player] || knob != g_knob_shown[player]) {
            g_act_held[player] = held;
            g_knob_shown[player] = knob;
            invalidate_card(player);
        }
    }
}

/* Hot-plugged pads and auto-assignment changes. */
static void refresh_pads(void)
{
    pads_state now;
    int idx;
    memset(&now, 0, sizeof now);
    now.generation = astrosession_gamepad_generation(g_session);
    now.count = astrosession_gamepad_count(g_session);
    for (idx = 0; idx < PAD_ROWS && idx < now.count; idx++) {
        now.assign[idx] = astrosession_gamepad_assignment(g_session, idx);
        now.effective[idx] = astrosession_gamepad_effective_port(g_session, idx);
    }
    if (memcmp(&now, &g_pads_shown, sizeof now) != 0) {
        g_pads_shown = now;
        InvalidateRect(g_panel, &g_gp_rc, FALSE);
    }
}

/* ---- keys ------------------------------------------------------------------------------ */

static void hide_panel(void)
{
    set_map_state(-2);
    mouse_release(0);
    if (GetCapture() == g_panel) ReleaseCapture();
    g_swallow_vkey = 0;
    ShowWindow(g_panel, SW_HIDE);
}

/* The app's own keys, never the console's or Map's. */
static int app_key(WPARAM vkey)
{
    return vkey == VK_F9 || vkey == VK_F11 || vkey == VK_F12;
}

static uint32_t win_keysym(LPARAM lparam)
{
    return astrosession_keysym_from_win_scancode((unsigned)((lparam >> 16) & 0xff), (int)((lparam >> 24) & 1));
}

/* A key while Map is armed: TRUE if Map took it. Waiting for a target, a key
 * is not typing; waiting for a binding, the key is the binding (not F9, F11
 * or F12, which are the app's). */
BOOL astro_controllers_map_key(UINT msg, WPARAM wparam, LPARAM lparam)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;

    if (!g_panel) return FALSE;
    if (g_swallow_vkey && wparam == g_swallow_vkey) {
        if (!down) g_swallow_vkey = 0;
        return TRUE;
    }
    if (g_map_state == -2) return FALSE;
    /* Map is the panel's: with the game window active its keys (releases
     * included) are the console's, not Map's. */
    if (GetActiveWindow() != g_panel) return FALSE;
    if (app_key(wparam) || wparam == VK_F10) return FALSE;
    if (msg == WM_SYSKEYDOWN && wparam == VK_F4) return FALSE;
    if (!down || (lparam & (1 << 30))) return TRUE;
    g_drop_char = 1;                    /* the WM_CHAR of this key is Map's too */
    if (wparam == VK_ESCAPE) {
        set_map_state(g_map_state >= 0 ? -1 : -2);
        g_swallow_vkey = wparam;
        return TRUE;
    }
    if (g_map_state >= 0) {
        char name[48];
        const int target = g_map_state;
        const uint32_t keysym = win_keysym(lparam);
        int from = -1;
        if (!keysym) return TRUE;       /* nothing to call it: keep waiting */
        g_swallow_vkey = wparam;
        if (is_knob_axis_target(target)) {
            set_hint("The knob's axis follows a gamepad axis: move a stick or trigger");
            return TRUE;
        }
        bindings_set_keysym(g_session, target, keysym, &from);
        astrosession_keysym_name(keysym, name, sizeof name);
        set_map_state(-1);              /* stay armed: remapping several in a row is normal */
        bound_hint(bindings_target_label(target), name, from);
    }
    return TRUE;
}

void astro_controllers_run_sysactions(HWND owner, astrosession *session)
{
    const unsigned bits = astrosession_sysaction_take(session);
    int act;
    for (act = 0; act < ASTROSESSION_SYSACT_COUNT; act++) {
        if (!(bits & (1u << act))) continue;
        astrosession_sysaction_fire(session, (astrosession_sysaction)act);
        if (act == ASTROSESSION_SYSACT_DEBUG_STOP)
            astro_debugger_show(owner, session);
    }
}

/* The key path both windows share: Map first, then the bindings. */
BOOL astro_controllers_key(astrosession *session, UINT msg, WPARAM wparam, LPARAM lparam)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    BOOL used;

    g_session = session;
    if (astro_controllers_map_key(msg, wparam, lparam)) return TRUE;
    used = astrosession_input_key(session, win_keysym(lparam), down, GetKeyState(VK_CONTROL) < 0) != 0;
    if (down && !(lparam & (1 << 30))) g_key_used = used;
    astro_controllers_run_sysactions(g_parent, session);
    return used;
}

void astro_controllers_char(astrosession *session, WPARAM wparam, LPARAM lparam)
{
    if (lparam & (1 << 30)) return;     /* an auto-repeat */
    if (g_drop_char) { g_drop_char = 0; return; }
    if (g_key_used) { g_key_used = 0; return; }
    if (g_map_state != -2) return;
    /* a control character is typing only as Enter or Backspace */
    if (wparam < 32 && wparam != '\r' && wparam != '\b') return;
    astrosession_type_char(session, (uint32_t)wparam);
}

/* A key with the window focused: TRUE if taken. The same path as the main
 * window's, with F9 hiding the window and the app's other keys forwarded. */
static int panel_key(UINT msg, WPARAM wparam, LPARAM lparam)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;

    if (down && !(lparam & (1 << 30))) g_drop_char = 0;
    if (wparam == VK_F9) {
        if (down && !(lparam & (1 << 30))) hide_panel();
        return 1;
    }
    if (app_key(wparam)) {
        /* the main window's shortcuts, as if it had the focus */
        if (g_parent && down && !(lparam & (1 << 30))) SendMessageA(g_parent, WM_KEYDOWN, wparam, lparam);
        return 1;
    }
    if (wparam == VK_F10) return 0;
    if (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) {
        if (astro_controllers_map_key(msg, wparam, lparam)) return 1;
        return 0;                       /* Alt chords: the system's */
    }
    astro_controllers_key(g_session, msg, wparam, lparam);
    return 1;
}

/* Exactly the laid-out client size, whatever the frame is at this DPI. */
static void fit_client(void)
{
    RECT wnd, client;
    GetWindowRect(g_panel, &wnd);
    GetClientRect(g_panel, &client);
    if (client.right == g_total_w && client.bottom == g_total_h) return;
    SetWindowPos(g_panel, NULL, 0, 0, (wnd.right - wnd.left) - client.right + g_total_w,
                 (wnd.bottom - wnd.top) - client.bottom + g_total_h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* The window's outer size for the laid-out client. */
static void window_size(HWND hwnd, SIZE *dims)
{
    RECT wnd, client;
    if (hwnd && GetWindowRect(hwnd, &wnd) && GetClientRect(hwnd, &client) && client.right > 0) {
        dims->cx = (wnd.right - wnd.left) - client.right + g_total_w;
        dims->cy = (wnd.bottom - wnd.top) - client.bottom + g_total_h;
    } else {
        SetRect(&wnd, 0, 0, g_total_w, g_total_h);
        AdjustWindowRectEx(&wnd, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
        dims->cx = wnd.right - wnd.left;
        dims->cy = wnd.bottom - wnd.top;
    }
}

static void rescale(int dpi)
{
    g_dpi = dpi;
    rebuild();
}

/* ---- the mouse ------------------------------------------------------------------------ */

/* The layout part under a client point, or -1. */
static int part_at(POINT pnt)
{
    if (!g_layout) return -1;
    return astrosession_controls_hit((float)((pnt.x - g_orgx) / g_unit), (float)((pnt.y - g_orgy) / g_unit));
}

/* A stick drag: where the pointer is on the disc decides what is held, out
 * over the wedges and beyond them while dragging, with nothing near the
 * middle. */
static void stick_follow(int player, POINT pnt)
{
    const double delx = pnt.x - g_card[player].ctrx, dely = pnt.y - g_card[player].ctry;
    set_pointer(player, astrosession_stick_from_vector((float)delx, (float)dely, (float)g_card[player].ring));
    invalidate_card(player);
    refresh_held();
}

/* A dial drag: the knob follows the angle of the pointer, nothing in the dead
 * centre. */
static void knob_follow(int player, POINT pnt)
{
    const int val = astrosession_knob_from_point(pnt.x - g_card[player].ctrx, pnt.y - g_card[player].ctry,
                                                 g_card[player].dial);
    if (val >= 0) astrosession_knob_set(g_session, player, (uint8_t)val);
    refresh_held();
}

/* Map mode on a dial: its left half is the knob-left key, its right half the
 * knob-right key, its lower part the axis. */
static int knob_zone_target(int player, POINT pnt)
{
    const double delx = pnt.x - g_card[player].ctrx, dely = pnt.y - g_card[player].ctry;
    const double rad = g_card[player].dial;
    if (dely > 0.3 * rad && fabs(delx) < 0.6 * rad) return knob_target(player, 2);
    return knob_target(player, delx < 0 ? 0 : 1);
}

static void mouse_down(HWND hwnd, POINT pnt)
{
    int part, row, seg, btn;
    SetFocus(hwnd);
    if (g_hover_part >= 0) {
        g_hover_part = -1;
        InvalidateRect(hwnd, &g_hint_rc, FALSE);
    }

    for (btn = 0; btn < SB_COUNT; btn++) {
        if (!PtInRect(&g_btn_rc[btn], pnt)) continue;
        switch (btn) {
        case SB_MAP: set_map_state(g_map_state == -2 ? -1 : -2); return;
        case SB_CLEAR:
            if (g_map_state >= 0) {
                bindings_clear(g_session, g_map_state);
                set_hint("Cleared. Press a key or a gamepad button to bind it again");
                astrosession_gamepad_capture_begin(g_session);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return;
        case SB_DEFAULTS:
            bindings_reset_defaults(g_session);
            set_hint("Every key and button is back to its default");
            InvalidateRect(hwnd, NULL, FALSE);
            return;
        case SB_SHOW:
            g_show_bindings = !g_show_bindings;
            astrosession_set_int(g_session, "controllers_show_bindings", g_show_bindings);
            InvalidateRect(hwnd, NULL, FALSE);
            return;
        case SB_OVERLAY:
            mouse_release(0);
            astrosession_overlay_set(g_session, !astrosession_overlay_get(g_session));
            refresh_held();
            InvalidateRect(hwnd, NULL, FALSE);
            return;
        case SB_HELP: astro_help_show(hwnd, "controllers"); return;
        default: return;
        }
    }
    for (row = 0; row < PAD_ROWS && row < astrosession_gamepad_count(g_session); row++)
        for (seg = 0; seg < SEGS; seg++) {
            RECT rect;
            segment_rect(row, seg, &rect);
            if (PtInRect(&rect, pnt)) {
                /* 0 = automatic (connection order), 1-4 = that player */
                astrosession_gamepad_assign(g_session, row, seg - 1);
                refresh_pads();
                InvalidateRect(hwnd, &g_gp_rc, FALSE);
                return;
            }
        }

    part = part_at(pnt);
    if (part < 0) return;
    {
        const astro_control *ctl = &g_layout[part];
        if (g_map_state != -2) {
            /* armed: picks (or changes) the target */
            if (ctl->kind == ASTRO_CTL_KNOB) set_map_state(knob_zone_target(ctl->player, pnt));
            else if (ctl->target >= 0 && ctl->kind != ASTRO_CTL_STICK) set_map_state(ctl->target);
            return;
        }
        mouse_release(0);
        switch (ctl->kind) {
        case ASTRO_CTL_STICK:
        case ASTRO_CTL_DIRECTION:
            g_mouse_player = ctl->player;
            stick_follow(ctl->player, pnt);
            break;
        case ASTRO_CTL_KNOB:
            g_mouse_knob = ctl->player;
            knob_follow(ctl->player, pnt);
            break;
        case ASTRO_CTL_KEY:
        case ASTRO_CTL_TRIGGER:
            g_mouse_part = part;
            astrosession_press(g_session, ctl->target, 1);
            break;
        case ASTRO_CTL_SYSBUTTON:
            g_mouse_part = part;
            break;
        default:
            return;
        }
        invalidate_part(part);
        SetCapture(hwnd);
        refresh_held();
    }
}

static void mouse_move(HWND hwnd, POINT pnt)
{
    if (g_mouse_player >= 0) {
        stick_follow(g_mouse_player, pnt);
        return;
    }
    if (g_mouse_knob >= 0) {
        knob_follow(g_mouse_knob, pnt);
        return;
    }
    if (g_mouse_part >= 0) {
        /* dragged off what it holds: let go, as a button does */
        if (g_mouse_part >= g_numlayout || part_at(pnt) != g_mouse_part) {
            mouse_release(0);
            if (GetCapture() == hwnd) ReleaseCapture();
            refresh_held();
        }
        return;
    }
    {   /* idle: the hint line says what drives the part under the pointer */
        const int part = part_at(pnt);
        if (!g_tracking) {
            TRACKMOUSEEVENT track;
            memset(&track, 0, sizeof track);
            track.cbSize = sizeof track;
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            g_tracking = TrackMouseEvent(&track) != 0;
        }
        if (part != g_hover_part) {
            g_hover_part = part;
            InvalidateRect(hwnd, &g_hint_rc, FALSE);
        }
    }
}

static void mouse_up(HWND hwnd)
{
    if (g_mouse_part >= 0 || g_mouse_player >= 0 || g_mouse_knob >= 0) {
        /* a console button fires only if the release is over it */
        POINT pnt;
        int fire = 0;
        if (g_mouse_part >= 0 && GetCursorPos(&pnt) && ScreenToClient(hwnd, &pnt))
            fire = part_at(pnt) == g_mouse_part;
        mouse_release(fire);
        refresh_held();
    }
    if (GetCapture() == hwnd) ReleaseCapture();
}

/* The wheel over a dial turns the knob a little. */
static void mouse_wheel(HWND hwnd, int delta, POINT screen)
{
    POINT pnt = screen;
    int part;
    if (g_map_state != -2 || !ScreenToClient(hwnd, &pnt)) return;
    part = part_at(pnt);
    if (part < 0 || g_layout[part].kind != ASTRO_CTL_KNOB) return;
    {
        const int player = g_layout[part].player;
        int val = astrosession_knob_get(g_session, player) + (delta / WHEEL_DELTA) * 8;
        if (val < 0) val = 0;
        if (val > 255) val = 255;
        astrosession_knob_set(g_session, player, (uint8_t)val);
        refresh_held();
    }
}

/* ---- the window ----------------------------------------------------------------------- */

static LRESULT CALLBACK panel_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC hdc = BeginPaint(hwnd, &paint);
        /* Off-screen first: the held highlights repaint at 25 Hz. */
        RECT inval = paint.rcPaint;
        HDC mem;
        HBITMAP bmp;
        HGDIOBJ oldbmp, oldfont;
        const int width = inval.right - inval.left, height = inval.bottom - inval.top;
        if (width <= 0 || height <= 0) { EndPaint(hwnd, &paint); return 0; }
        mem = CreateCompatibleDC(hdc);
        bmp = CreateCompatibleBitmap(hdc, width, height);
        oldbmp = SelectObject(mem, bmp);
        oldfont = SelectObject(mem, g_font);
        SetViewportOrgEx(mem, -inval.left, -inval.top, NULL);
        paint_rect(mem, &inval);
        SetViewportOrgEx(mem, 0, 0, NULL);
        BitBlt(hdc, inval.left, inval.top, width, height, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldfont);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: {
        POINT pnt;
        pnt.x = GET_X_LPARAM(lparam);
        pnt.y = GET_Y_LPARAM(lparam);
        mouse_down(hwnd, pnt);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pnt;
        pnt.x = GET_X_LPARAM(lparam);
        pnt.y = GET_Y_LPARAM(lparam);
        mouse_move(hwnd, pnt);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT pnt;
        pnt.x = GET_X_LPARAM(lparam);
        pnt.y = GET_Y_LPARAM(lparam);
        mouse_wheel(hwnd, GET_WHEEL_DELTA_WPARAM(wparam), pnt);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_tracking = 0;
        if (g_hover_part >= 0) {
            g_hover_part = -1;
            InvalidateRect(hwnd, &g_hint_rc, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        /* released wherever the mouse ended up: capture means this arrives
         * even if the pointer left the part */
        mouse_up(hwnd);
        return 0;
    case WM_CAPTURECHANGED:
        /* lost the mouse some other way (Alt+Tab mid-press): let go */
        if ((HWND)lparam != hwnd) {
            mouse_release(0);
            refresh_held();
        }
        return 0;

    case WM_TIMER:
        if (wparam == IDT_CAPTURE) {
            int input;
            if (g_map_state < 0) {
                KillTimer(hwnd, IDT_CAPTURE);
            } else if (astrosession_gamepad_capture_poll(g_session, &input)) {
                const int target = g_map_state;
                char name[48];
                int from = -1;
                if (is_knob_axis_target(target) != (input >= ASTRO_PAD_AXIS_BASE && input < ASTRO_PAD_AXIS_BASE + ASTRO_PAD_AXIS_COUNT)) {
                    /* a button cannot turn a knob, and an axis cannot press a key: wait again */
                    if (is_knob_axis_target(target)) astrosession_gamepad_capture_begin_axis(g_session);
                    else astrosession_gamepad_capture_begin(g_session);
                    return 0;
                }
                bindings_set_pad(g_session, target, input, &from);
                astrosession_pad_input_name(input, name, sizeof name);
                set_map_state(-1);   /* stay armed: remapping several in a row is normal */
                bound_hint(bindings_target_label(target), name, from);
            }
        } else if (wparam == IDT_HELD) {
            refresh_held();
        } else if (wparam == IDT_PADS) {
            refresh_pads();
        }
        return 0;

    /* WM_SYSKEY* too: a bound Alt is the console's */
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
        if (panel_key(msg, wparam, lparam)) return 0;
        break;
    case WM_CHAR:
        /* typed here as in the main window, except what Map took */
        astro_controllers_char(g_session, wparam, lparam);
        return 0;
    case WM_SYSCHAR:
        /* Alt+letter: the main window's menu mnemonics, as when it has the focus */
        if (wparam != ' ' && g_parent) { SendMessageA(g_parent, msg, wparam, lparam); return 0; }
        break;

    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE) {
            const HWND other = (HWND)lparam;
            mouse_release(0);
            if (GetCapture() == hwnd) ReleaseCapture();
            g_swallow_vkey = 0;     /* its release goes elsewhere */
            /* Leaving the app: nothing typed here stays held (within it, the
             * window that gets the key-up lets go). */
            if (!other || GetWindowThreadProcessId(other, NULL) != GetCurrentThreadId())
                astrosession_release_all(g_session);
            refresh_held();
        }
        break;                      /* DefWindowProc gives the window the focus */
    case WM_SHOWWINDOW:
        if (wparam) {
            refresh_held();
            SetTimer(hwnd, IDT_HELD, 40, NULL);
            SetTimer(hwnd, IDT_PADS, 250, NULL);
        } else {
            KillTimer(hwnd, IDT_HELD);
            KillTimer(hwnd, IDT_PADS);
        }
        break;
    case WM_GETMINMAXINFO:
        /* Fixed: the size limits say so too, so neither maximizing nor a
         * compositor (Wine passes them on) makes it any other size. */
        if (g_total_w) {
            MINMAXINFO *limits = (MINMAXINFO *)lparam;
            SIZE dims;
            window_size(hwnd, &dims);
            limits->ptMinTrackSize.x = limits->ptMaxTrackSize.x = limits->ptMaxSize.x = dims.cx;
            limits->ptMinTrackSize.y = limits->ptMaxTrackSize.y = limits->ptMaxSize.y = dims.cy;
            return 0;
        }
        break;
    case WM_DPICHANGED: {
        const RECT *suggested = (const RECT *)lparam;
        rescale(HIWORD(wparam));
        SetWindowPos(hwnd, NULL, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        fit_client();
        return 0;
    }
    case WM_CLOSE:
        /* Hide, do not destroy: a remap and the position survive closing it. */
        hide_panel();
        return 0;
    case WM_DESTROY:
        /* with the main window: the next toggle starts afresh */
        mouse_release(0);
        set_map_state(-2);
        g_panel = NULL;
        free_fonts();
        break;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

static int create_panel(void)
{
    static int registered;
    HINSTANCE inst = GetModuleHandleA(NULL);
    RECT want;

    {   /* "controllers_show_bindings"; the older spellings are read when it is absent */
        int saved = astrosession_get_int(g_session, "controllers_show_bindings", -1);
        if (saved < 0) saved = astrosession_get_int(g_session, "controllers_bindings", -1);
        if (saved < 0) saved = astrosession_get_int(g_session, "show_bindings", -1);
        g_show_bindings = saved > 0;
    }
    g_overlay = astrosession_overlay_get(g_session) != 0;

    if (!registered) {
        WNDCLASSEXA wcls;
        memset(&wcls, 0, sizeof wcls);
        wcls.cbSize = sizeof wcls;
        wcls.lpfnWndProc = panel_proc;      /* no CS_DBLCLKS: fast clicks stay press/release pairs */
        wcls.hInstance = inst;
        wcls.hCursor = LoadCursor(NULL, IDC_ARROW);
        wcls.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wcls.lpszClassName = PANEL_CLASS;
        registered = RegisterClassExA(&wcls) != 0;
    }
    g_dpi = astro_window_dpi(g_parent);
    make_fonts();
    layout_static();

    SetRect(&want, 0, 0, g_total_w, g_total_h);
    /* A tool window with a caption and no thick frame or maximize box: it is
     * exactly the size of its contents, and stays off the taskbar. */
    AdjustWindowRectEx(&want, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
    g_panel = CreateWindowExA(PANEL_EXSTYLE, PANEL_CLASS, "Controllers", PANEL_STYLE,
                              CW_USEDEFAULT, CW_USEDEFAULT, want.right - want.left, want.bottom - want.top,
                              g_parent, NULL, inst, NULL);
    if (!g_panel) return 0;
    astro_help_set_context(g_panel, "controllers", 0);    /* F1 is bindable here */
    rescale(astro_window_dpi(g_panel));    /* placed on another monitor than the main window */
    fit_client();
    memset(&g_pads_shown, 0, sizeof g_pads_shown);
    memset(g_key_held, 0, sizeof g_key_held);
    memset(g_act_held, 0, sizeof g_act_held);
    g_hover_part = -1;
    set_map_state(-2);
    return 1;
}

static void show_panel(void)
{
    refresh_pads();
    ShowWindow(g_panel, SW_SHOW);
    SetForegroundWindow(g_panel);
}

void astro_controllers_show(HWND owner, astrosession *session)
{
    g_session = session;
    if (owner)
        g_parent = owner;
    if (!g_panel && !create_panel())
        return;
    if (!IsWindowVisible(g_panel))
        show_panel();
    else
        SetForegroundWindow(g_panel);
}

void astro_controllers_toggle(HWND owner, astrosession *session)
{
    g_session = session;
    if (owner)
        g_parent = owner;
    if (!g_panel && !create_panel())
        return;
    if (IsWindowVisible(g_panel))
        hide_panel();
    else
        show_panel();
}

BOOL astro_controllers_visible(void)
{
    return g_panel && IsWindowVisible(g_panel);
}

void astro_controllers_refresh(void)
{
    if (!g_panel)
        return;
    if (IsWindowVisible(g_panel)) {
        refresh_pads();
        refresh_held();
        InvalidateRect(g_panel, NULL, FALSE);
    }
}
