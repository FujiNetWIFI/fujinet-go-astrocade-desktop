/*
 * vidtab.c -- the Video tab's controller (core/include/astrovidtab.h).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "astrovidtab.h"

#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "astro_internal.h"
#include "debugger_internal.h"
#include "host.h"

#define SW ASTROVID_SCREEN_W
#define SH ASTROVID_SCREEN_H
#define MAXPIX 6000000L         /* the largest picture we will build */
#define MAX_REGS 32
#define MAX_TIMING 12
#define HEAT_FULL 255

static const int k_zooms[] = { 0, 1, 2, 3, 4, 6, 8, 12, 16 };

struct astrovid_tab {
    int view, zoom, vw, vh;
    unsigned ov;
    int env_section, capture_on;

    /* the machine's state as presented */
    astrovid_capture *cap;      /* working frame: snapshot, lines, last finished fb */
    astrovid_capture *last;     /* last captured by the machine thread */
    uint64_t last_serial;
    uint32_t *live;             /* the frame in progress (stopped only) */
    int have, has_fb, has_live, stopped, fed;
    uint64_t sig;

    /* screen RAM trackers */
    uint8_t prev_vram[0x1000], heat[0x1000], changed[0x1000];
    int have_prev, any_heat;

    /* the picture */
    uint32_t *base, *pic;
    size_t base_cap, pic_cap;
    int bw, bh, pw, ph, ez;
    int dirty;
    char why[160];

    /* tables */
    astrovid_reg regs[MAX_REGS];
    char prev_val[MAX_REGS][24];
    int nregs, regs_primed;
    astrovid_pal_entry pal[256];
    astrovid_swatch sw[8];
    long reg_uses[256];
    long sw_uses[8];
    astrovid_line_row rows[SH];
    int nrows;
    char status[320];
    char timing_buf[MAX_TIMING][112];
    const char *timing_ptr[MAX_TIMING];
    int ntiming;

    /* pixels */
    int hx, hy, px, py;         /* -1: none */
    astrovid_pixel hover, pin;
};

/* ---- tables of names ----------------------------------------------------------------- */

static const astrovid_overlay_info k_overlays[] = {
    { ASTROVID_OV_GRID,     "grid",     "Grid" },
    { ASTROVID_OV_BOUNDARY, "boundary", "Colour boundary" },
    { ASTROVID_OV_VERBL,    "verbl",    "Last active line (VERBL)" },
    { ASTROVID_OV_INLIN,    "inlin",    "Interrupt line (INLIN)" },
    { ASTROVID_OV_BEAM,     "beam",     "Beam" },
    { ASTROVID_OV_HEAT,     "heat",     "Written lately" },
    { ASTROVID_OV_CHANGED,  "changed",  "Changed since last refresh" },
    { ASTROVID_OV_ACTIVE,   "active",   "Data area outline" },
};
#define N_OVERLAYS ((int)(sizeof k_overlays / sizeof *k_overlays))

const astrovid_overlay_info *astrovid_overlays(int *count)
{
    if (count) *count = N_OVERLAYS;
    return k_overlays;
}

static const char *const k_view_names[] = { "Display", "Decoded", "Beam", "Values" };
static const char *const k_view_keys[] = { "display", "decoded", "beam", "values" };
static const char *const k_sec_names[] = { "Pixel", "Registers", "Colours", "Lines", "Timing" };
static const char *const k_sec_keys[] = { "pixel", "registers", "colours", "lines", "timing" };

const char *astrovid_view_name(int v) { return v >= 0 && v < ASTROVID_V_COUNT ? k_view_names[v] : ""; }
const char *astrovid_view_key(int v) { return v >= 0 && v < ASTROVID_V_COUNT ? k_view_keys[v] : ""; }
const char *astrovid_section_name(int s) { return s >= 0 && s < ASTROVID_S_COUNT ? k_sec_names[s] : ""; }
const char *astrovid_section_key(int s) { return s >= 0 && s < ASTROVID_S_COUNT ? k_sec_keys[s] : ""; }

const int *astrovid_zooms(int *count)
{
    if (count) *count = (int)(sizeof k_zooms / sizeof *k_zooms);
    return k_zooms;
}

/* ---- small helpers ---------------------------------------------------------------------- */

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n--) { h ^= *b++; h *= 1099511628211ull; }
    return h;
}

static uint32_t pen_rgb(const astrovid_snapshot *s, int regval)
{
    return s->palette[(regval << 1) & 0x1ff] | 0xff000000u;
}

static uint32_t blend(uint32_t dst, uint32_t rgb, int a256)
{
    const uint32_t dr = (dst >> 16) & 255, dg = (dst >> 8) & 255, db = dst & 255;
    const uint32_t sr = (rgb >> 16) & 255, sg = (rgb >> 8) & 255, sb = rgb & 255;
    const uint32_t r = (dr * (256 - a256) + sr * a256) >> 8;
    const uint32_t g = (dg * (256 - a256) + sg * a256) >> 8;
    const uint32_t b = (db * (256 - a256) + sb * a256) >> 8;
    return 0xff000000u | r << 16 | g << 8 | b;
}

static int fits_zoom(int w, int h, int z)
{
    return (long)w * z * (long)h * z <= MAXPIX;
}

static void why_set(astrovid_tab *t, const char *s)
{
    snprintf(t->why, sizeof t->why, "%s", s);
}

static int ensure(uint32_t **buf, size_t *cap, size_t need)
{
    if (need > *cap) {
        uint32_t *n = realloc(*buf, need * sizeof **buf);
        if (!n) return 0;
        *buf = n;
        *cap = need;
    }
    return 1;
}

/* the geometry of a screen RAM picture in the machine's current mode */
static void geom(const astrovid_snapshot *s, int *step, int *bpl, int *w, int *h)
{
    *step = 2 - (s->video_mode & 1);
    *bpl = 80 / *step;
    *w = *bpl * 4;
    *h = 204 / *step;
}

static int view_is_screen(int v) { return v == ASTROVID_V_DISPLAY || v == ASTROVID_V_BEAM; }

/* ---- new / free / settings -------------------------------------------------------------- */

astrovid_tab *astrovid_tab_new(void)
{
    astrovid_tab *t = calloc(1, sizeof *t);
    const char *e;
    if (!t) return NULL;
    t->cap = calloc(1, sizeof *t->cap);
    t->last = calloc(1, sizeof *t->last);
    t->live = calloc((size_t)SW * SH, sizeof *t->live);
    if (!t->cap || !t->last || !t->live) {
        astrovid_tab_free(t);
        return NULL;
    }
    t->view = ASTROVID_V_DISPLAY;
    t->ov = ASTROVID_OV_BOUNDARY | ASTROVID_OV_BEAM;
    t->zoom = 0;
    t->vw = SW * 2;
    t->vh = SH * 2;
    t->hx = t->hy = t->px = t->py = -1;
    t->dirty = 1;

    if ((e = getenv("ASTRO_DEBUGGER_VIDEO_VIEW")) && *e) {
        for (int i = 0; i < ASTROVID_V_COUNT; i++)
            if (!strncasecmp(e, k_view_keys[i], strlen(e))) { t->view = i; break; }
    }
    if ((e = getenv("ASTRO_DEBUGGER_VIDEO_OVERLAYS"))) {
        char buf[200], *tok, *sp;
        unsigned ov = 0;
        snprintf(buf, sizeof buf, "%s", e);
        for (tok = strtok_r(buf, ", ", &sp); tok; tok = strtok_r(NULL, ", ", &sp)) {
            if (!strcasecmp(tok, "all")) ov |= ASTROVID_OV_ALL;
            else if (!strcasecmp(tok, "none")) ov = 0;
            else
                for (int i = 0; i < N_OVERLAYS; i++)
                    if (!strcasecmp(tok, k_overlays[i].name)) ov |= k_overlays[i].bit;
        }
        t->ov = ov;
    }
    if ((e = getenv("ASTRO_DEBUGGER_VIDEO_ZOOM")) && *e) {
        if (!strcasecmp(e, "fit")) t->zoom = 0;
        else {
            int z = atoi(e);
            for (size_t i = 1; i < sizeof k_zooms / sizeof *k_zooms; i++)
                if (k_zooms[i] == z) t->zoom = z;
        }
    }
    t->env_section = ASTROVID_S_PIXEL;
    if ((e = getenv("ASTRO_DEBUGGER_VIDEO_SECTION")) && *e) {
        for (int i = 0; i < ASTROVID_S_COUNT; i++)
            if (!strncasecmp(e, k_sec_keys[i], strlen(e))) { t->env_section = i; break; }
    }
    astro_host_capture_enable(1);
    t->capture_on = 1;
    return t;
}

void astrovid_tab_free(astrovid_tab *t)
{
    if (!t) return;
    if (t->capture_on)
        astro_host_capture_enable(0);
    free(t->cap);
    free(t->last);
    free(t->live);
    free(t->base);
    free(t->pic);
    free(t);
}

void astrovid_tab_set_view(astrovid_tab *t, int v)
{
    if (t && v >= 0 && v < ASTROVID_V_COUNT && v != t->view) { t->view = v; t->dirty = 1; }
}
int astrovid_tab_view(const astrovid_tab *t) { return t ? t->view : 0; }
void astrovid_tab_set_overlays(astrovid_tab *t, unsigned ov)
{
    if (t && (ov & ASTROVID_OV_ALL) != t->ov) { t->ov = ov & ASTROVID_OV_ALL; t->dirty = 1; }
}
unsigned astrovid_tab_overlays(const astrovid_tab *t) { return t ? t->ov : 0; }
void astrovid_tab_set_zoom(astrovid_tab *t, int z)
{
    if (t && z >= 0 && z <= 16 && z != t->zoom) { t->zoom = z; t->dirty = 1; }
}
int astrovid_tab_zoom(const astrovid_tab *t) { return t ? t->zoom : 0; }
void astrovid_tab_set_viewport(astrovid_tab *t, int w, int h)
{
    if (!t) return;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w != t->vw || h != t->vh) {
        t->vw = w; t->vh = h;
        if (t->zoom == 0) t->dirty = 1;
    }
}
int astrovid_tab_env_section(const astrovid_tab *t) { return t ? t->env_section : 0; }

/* ---- feeding ----------------------------------------------------------------------------- */

static void note_vram(astrovid_tab *t)
{
    const uint8_t *v = t->cap->snap.vram;
    t->any_heat = 0;
    for (int i = 0; i < 0x1000; i++) {
        const int ch = t->have_prev && v[i] != t->prev_vram[i];
        t->changed[i] = (uint8_t)ch;
        t->heat[i] = ch ? HEAT_FULL : (uint8_t)(t->heat[i] * 4 / 5);
        if (t->heat[i]) t->any_heat = 1;
    }
    memcpy(t->prev_vram, v, sizeof t->prev_vram);
    t->have_prev = 1;
}

void astrovid_tab_feed(astrovid_tab *t, const astrovid_capture *cap, const uint32_t *live_fb)
{
    if (!t || !cap) return;
    memcpy(t->cap, cap, sizeof *t->cap);
    t->fed = 1;
    t->have = 1;
    t->has_fb = cap->serial != 0;
    t->has_live = live_fb != NULL;
    t->stopped = live_fb != NULL;
    if (live_fb) memcpy(t->live, live_fb, (size_t)SW * SH * sizeof *t->live);
}

/* Take what the machine has. 0 if there is no machine. */
static int acquire(astrovid_tab *t)
{
    astro_machine_t *m;
    astrodebug *d;

    if (t->fed)
        return t->have;
    m = astro_host_machine();
    d = astrodebug_get();
    t->have = 0;
    if (!m)
        return 0;
    astro_host_capture_get(t->last, &t->last_serial);
    t->has_fb = t->last->serial != 0;
    t->stopped = d && astrodebug_is_stopped(d);
    t->has_live = t->stopped;
    if (t->stopped) {
        astrovid_snapshot_from(m, &t->cap->snap);
        for (int y = 0; y < SH; y++)
            t->cap->lines[y] = (y < m->scanline || !t->has_fb) ? m->line_regs[y]
                                                               : t->last->lines[y];
        if (t->has_fb)
            memcpy(t->cap->fb, t->last->fb, sizeof t->cap->fb);
        memcpy(t->live, m->fb, (size_t)SW * SH * sizeof *t->live);
        t->cap->frame = m->frame;
        t->cap->serial = t->last->serial;
    } else if (t->has_fb) {
        memcpy(t->cap, t->last, sizeof *t->cap);
    } else {
        /* nothing captured yet: the live machine, racy but harmless */
        astrovid_snapshot_from(m, &t->cap->snap);
        memcpy(t->cap->lines, m->line_regs, sizeof t->cap->lines);
        t->cap->serial = 0;
    }
    t->have = 1;
    return 1;
}

static uint64_t signature(const astrovid_tab *t)
{
    uint64_t h = 1469598103934665603ull;
    h = fnv(h, &t->cap->snap, offsetof(astrovid_snapshot, palette));
    h = fnv(h, t->cap->lines, sizeof t->cap->lines);
    h = fnv(h, &t->cap->serial, sizeof t->cap->serial);
    h = fnv(h, &t->has_live, sizeof t->has_live);
    h = fnv(h, &t->has_fb, sizeof t->has_fb);
    return h;
}

/* ---- decoding screen RAM ----------------------------------------------------------------- */

static const uint32_t k_right_pens[4] = { 0xff2a1010u, 0xff882424u, 0xffd85a34u, 0xfff4bc66u };
static const uint32_t k_left_pens[4]  = { 0xff101a2au, 0xff243c8cu, 0xff3466dcu, 0xff7cb8f4u };

/* Walks screen RAM through each line's own registers. dst (w*h) may be NULL
 * when only the uses are wanted. mode: 0 colours, 1 values. */
static void decode(astrovid_tab *t, uint32_t *dst, int values)
{
    const astrovid_capture *c = t->cap;
    int step, bpl, w, h;

    geom(&c->snap, &step, &bpl, &w, &h);
    memset(t->reg_uses, 0, sizeof t->reg_uses);
    memset(t->sw_uses, 0, sizeof t->sw_uses);
    for (int r = 0; r < h; r++) {
        const int a = r * step;
        int y = a + ASTRO_VERT_OFFSET;
        if (y >= SH) y = SH - 1;
        const astrovid_line *L = &c->lines[y];
        for (int b = 0; b < bpl; b++) {
            const int effx = b * step;
            const int set = effx < L->colorsplit ? 4 : 0;
            uint8_t data = a < L->vblank ? c->snap.vram[(r * bpl + b) & 0xfff] : L->bgdata;
            for (int p = 0; p < 4; p++) {
                const int pen = (data >> 6) & 3;
                const int reg = L->colors[set + pen];
                t->reg_uses[reg]++;
                t->sw_uses[set + pen]++;
                if (dst)
                    dst[(size_t)r * w + b * 4 + p] =
                        values ? (set ? k_left_pens[pen] : k_right_pens[pen])
                               : pen_rgb(&c->snap, reg);
                data <<= 2;
            }
        }
    }
}

/* ---- the base picture (1x) -------------------------------------------------------------------- */

static int build_base(astrovid_tab *t)
{
    const astrovid_capture *c = t->cap;
    int step, bpl, w, h;

    geom(&c->snap, &step, &bpl, &w, &h);
    switch (t->view) {
    case ASTROVID_V_DISPLAY:
        if (!t->has_fb) {
            why_set(t, "No frame has finished yet. Let the machine run for a moment.");
            return 0;
        }
        if (!ensure(&t->base, &t->base_cap, (size_t)SW * SH)) return 0;
        memcpy(t->base, c->fb, (size_t)SW * SH * sizeof *t->base);
        t->bw = SW; t->bh = SH;
        return 1;
    case ASTROVID_V_BEAM: {
        const int beam = c->snap.scanline;
        if (!t->has_live) {
            why_set(t, "The Beam view needs the machine stopped: it shows this frame as far as the beam has drawn it, and the last one dimmed below.");
            return 0;
        }
        if (!ensure(&t->base, &t->base_cap, (size_t)SW * SH)) return 0;
        for (int y = 0; y < SH; y++)
            for (int x = 0; x < SW; x++) {
                uint32_t px;
                if (y < beam) px = t->live[(size_t)y * SW + x];
                else if (t->has_fb) px = blend(c->fb[(size_t)y * SW + x], 0x000000, 150);
                else px = 0xff000000u;
                t->base[(size_t)y * SW + x] = px;
            }
        t->bw = SW; t->bh = SH;
        return 1;
    }
    default:
        if (!ensure(&t->base, &t->base_cap, (size_t)w * h)) return 0;
        decode(t, t->base, t->view == ASTROVID_V_VALUES);
        t->bw = w; t->bh = h;
        return 1;
    }
}

/* ---- overlays ---------------------------------------------------------------------------- */

typedef struct { uint32_t *p; int w, h; } canvas;

static void px_blend(canvas *c, int x, int y, uint32_t rgb, int a)
{
    if (x >= 0 && y >= 0 && x < c->w && y < c->h)
        c->p[(size_t)y * c->w + x] = blend(c->p[(size_t)y * c->w + x], rgb, a);
}
static void hline(canvas *c, int y, int x0, int x1, uint32_t rgb, int a)
{
    for (int x = x0; x < x1; x++) px_blend(c, x, y, rgb, a);
}
static void vline(canvas *c, int x, int y0, int y1, uint32_t rgb, int a)
{
    for (int y = y0; y < y1; y++) px_blend(c, x, y, rgb, a);
}
static void fill_rect(canvas *c, int x, int y, int w, int h, uint32_t rgb, int a)
{
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++) px_blend(c, x + xx, y + yy, rgb, a);
}

/* picture coordinates for a position in the data area / for a line */
static int map_x(const astrovid_tab *t, int effx)
{
    const int step = 2 - (t->cap->snap.video_mode & 1);
    return view_is_screen(t->view) ? ASTRO_HORZ_OFFSET + effx * 4 : effx * 4 / step;
}
static int map_astro_y(const astrovid_tab *t, int a)
{
    const int step = 2 - (t->cap->snap.video_mode & 1);
    return view_is_screen(t->view) ? a + ASTRO_VERT_OFFSET : a / step;
}

static void draw_overlays(astrovid_tab *t, canvas *cv, int z)
{
    const astrovid_capture *c = t->cap;
    const int screen = view_is_screen(t->view);
    const int step = 2 - (c->snap.video_mode & 1);
    const int bpl = 80 / step;
    const int rows = screen ? SH : t->bh;
    const int bw = t->bw, bh = t->bh;

    if (t->ov & ASTROVID_OV_HEAT || t->ov & ASTROVID_OV_CHANGED) {
        if (!screen)
            for (int r = 0; r < bh; r++)
                for (int b = 0; b < bpl; b++) {
                    const int off = (r * bpl + b) & 0xfff;
                    if ((t->ov & ASTROVID_OV_HEAT) && t->heat[off])
                        fill_rect(cv, b * 4 * z, r * z, 4 * z, z, 0xffff8a00u, t->heat[off] * 190 / 255);
                    if ((t->ov & ASTROVID_OV_CHANGED) && t->changed[off])
                        fill_rect(cv, b * 4 * z, r * z, 4 * z, z, 0xff00e0ff, 120);
                }
    }
    if (t->ov & ASTROVID_OV_GRID) {
        const int cw = screen ? 4 * step : 4;
        const int chh = screen ? step : 1;
        const int x0 = screen ? ASTRO_HORZ_OFFSET : 0;
        const int y0 = screen ? ASTRO_VERT_OFFSET : 0;
        if (z >= 2) {
            for (int x = x0 % cw, n = 0; x < bw; x += cw, n++)
                vline(cv, x * z, 0, bh * z, 0xff000000u + 0xffffff, (n % 10 == 0) ? 90 : 45);
            for (int y = y0 % chh; y < bh; y += chh)
                if (z >= 4) hline(cv, y * z, 0, bw * z, 0xffffffff, 30);
        }
        if (z >= 8 && !screen)
            for (int x = 0; x < bw; x++)
                if (x % 4) vline(cv, x * z, 0, bh * z, 0xffffffff, 22);
    }
    if (t->ov & ASTROVID_OV_BOUNDARY) {
        for (int r = 0; r < rows; r++) {
            int ly, x;
            if (screen) ly = r;
            else { ly = r * step + ASTRO_VERT_OFFSET; if (ly >= SH) ly = SH - 1; }
            x = map_x(t, c->lines[ly].colorsplit) * z;
            vline(cv, x, r * z, (r + 1) * z, 0xff66ff66, 230);
        }
    }
    if (t->ov & ASTROVID_OV_VERBL) {
        const int y = map_astro_y(t, c->snap.vblank) * z;
        hline(cv, y, 0, bw * z, 0xffffd24a, 220);
    }
    if ((t->ov & ASTROVID_OV_INLIN) && (c->snap.interrupt_enabl & 0x08)) {
        const int y = map_astro_y(t, c->snap.interrupt_scanline) * z;
        hline(cv, y, 0, bw * z, 0xffff5ad2, 220);
    }
    if (t->ov & ASTROVID_OV_BEAM) {
        const int s = c->snap.scanline;
        int y = -1;
        if (screen) y = s * z;
        else if (s >= ASTRO_VERT_OFFSET) y = ((s - ASTRO_VERT_OFFSET) / step) * z;
        if (y >= 0) hline(cv, y, 0, bw * z, 0xffff4040, 230);
    }
    if ((t->ov & ASTROVID_OV_ACTIVE) && screen) {
        const int x0 = ASTRO_HORZ_OFFSET * z, x1 = (ASTRO_HORZ_OFFSET + 320) * z;
        const int y0 = ASTRO_VERT_OFFSET * z;
        int y1 = (ASTRO_VERT_OFFSET + c->snap.vblank) * z;
        if (y1 > bh * z) y1 = bh * z;
        hline(cv, y0, x0, x1, 0xffffffff, 160);
        hline(cv, y1 - 1, x0, x1, 0xffffffff, 160);
        vline(cv, x0, y0, y1, 0xffffffff, 160);
        vline(cv, x1 - 1, y0, y1, 0xffffffff, 160);
    }
}

static int pick_zoom(const astrovid_tab *t)
{
    int z = t->zoom;
    if (z == 0) {
        z = 1;
        for (size_t i = 1; i < sizeof k_zooms / sizeof *k_zooms; i++)
            if (t->bw * k_zooms[i] <= t->vw && t->bh * k_zooms[i] <= t->vh)
                z = k_zooms[i];
    }
    while (z > 1 && !fits_zoom(t->bw, t->bh, z)) {
        int lower = 1;
        for (size_t i = 1; i < sizeof k_zooms / sizeof *k_zooms; i++)
            if (k_zooms[i] < z) lower = k_zooms[i];
        z = lower;
    }
    return z;
}

static void build_picture(astrovid_tab *t)
{
    int z;
    canvas cv;

    t->why[0] = '\0';
    t->pw = t->ph = 0;
    if (!build_base(t))
        return;
    z = pick_zoom(t);
    t->ez = z;
    t->pw = t->bw * z;
    t->ph = t->bh * z;
    if (!ensure(&t->pic, &t->pic_cap, (size_t)t->pw * t->ph)) {
        t->pw = t->ph = 0;
        why_set(t, "Out of memory.");
        return;
    }
    if (z == 1)
        memcpy(t->pic, t->base, (size_t)t->bw * t->bh * sizeof *t->pic);
    else
        for (int y = 0; y < t->ph; y++) {
            const uint32_t *src = &t->base[(size_t)(y / z) * t->bw];
            uint32_t *dst = &t->pic[(size_t)y * t->pw];
            for (int x = 0; x < t->pw; x++)
                dst[x] = src[x / z];
        }
    cv.p = t->pic; cv.w = t->pw; cv.h = t->ph;
    draw_overlays(t, &cv, z);
}

/* ---- the inspector ----------------------------------------------------------------------- */

static void locate(astrovid_tab *t, int x, int y, astrovid_pixel *o)
{
    const astrovid_capture *c = t->cap;
    int z = t->ez ? t->ez : 1;
    int px, py, step, bpl, w, h, a, ly, b = -1, pix = 0, active = 0;
    unsigned off = 0;

    memset(o, 0, sizeof *o);
    o->column = -1;
    if (!t->pw || x < 0 || y < 0 || x >= t->pw || y >= t->ph)
        return;
    px = x / z; py = y / z;
    o->valid = 1;
    o->x = px; o->y = py;

    if (view_is_screen(t->view)) {
        const astrovid_line *L;
        ly = py;
        L = &c->lines[ly];
        step = 2 - (L->mode & 1);
        bpl = 80 / step;
        a = ly - ASTRO_VERT_OFFSET;
        if (a < 0) a += ASTRO_LINES_PER_FRAME;
        {
            const int iter = px / (4 * step);
            const int effx = iter * step - ASTRO_HORZ_OFFSET / 4;
            if (effx >= 0 && effx < 80 && a < L->vblank) {
                active = 1;
                b = effx / step;
                pix = (px % (4 * step)) / step;
                off = (unsigned)((a / step) * bpl + b);
                o->column = b * 4 + pix;
            }
        }
    } else {
        geom(&c->snap, &step, &bpl, &w, &h);
        b = px / 4; pix = px % 4;
        a = py * step;
        ly = a + ASTRO_VERT_OFFSET;
        if (ly >= SH) ly = SH - 1;
        off = (unsigned)(py * bpl + b);
        o->column = px;
        active = a < c->lines[ly].vblank;
    }
    o->line = ly;
    o->astro_line = a;
    o->active = active;
    {
        const astrovid_line *L = &c->lines[ly];
        const int effx = b >= 0 ? b * step : 0;
        o->offset = (int)(off & 0xfff);
        o->addr = 0x4000u + (off & 0xfff);
        o->mirror = off & 0xfff;
        if (active) {
            o->data = c->snap.vram[off & 0xfff];
            o->pen = (o->data >> (6 - 2 * pix)) & 3;
            o->left = effx < L->colorsplit;
            o->reg = (o->left ? 4 : 0) + o->pen;
            o->regval = L->colors[o->reg];
        } else {
            o->pen = L->bgdata & 3;
            o->left = 0;
            o->reg = -1;
            o->regval = -1;
        }
        o->pal = o->regval >= 0 ? (o->regval << 1) & 0x1ff : -1;
        o->rgb = view_is_screen(t->view)
                 ? (t->base ? t->base[(size_t)py * t->bw + px] : 0)
                 : (o->regval >= 0 ? pen_rgb(&c->snap, o->regval) : 0xff000000u);
        {
            static const char *const nm[8] = { "COL0R", "COL1R", "COL2R", "COL3R",
                                               "COL0L", "COL1L", "COL2L", "COL3L" };
            int n = snprintf(o->text, sizeof o->text,
                             "Pixel %d,%d   screen line %d (astrocade line %d)\n", px, py, ly, a);
            if (active) {
                n += snprintf(o->text + n, sizeof o->text - (size_t)n,
                              "Screen RAM $%04X (magic window $%04X)  byte %02X\n"
                              "Bits %d-%d: pen %d\n"
                              "%s of the boundary: %s = %02X  (palette $%03X)\n"
                              "Colour #%06X\n",
                              o->addr, o->mirror, o->data, 7 - 2 * pix, 6 - 2 * pix, o->pen,
                              o->left ? "Left" : "Right", nm[o->reg], o->regval, o->pal,
                              o->rgb & 0xffffff);
            } else {
                n += snprintf(o->text + n, sizeof o->text - (size_t)n,
                              "Outside the data area: border or background (pen %d)\n", o->pen);
            }
            snprintf(o->text + n, sizeof o->text - (size_t)n,
                     "This line: R %02X %02X %02X %02X  L %02X %02X %02X %02X\n"
                     "boundary %u  VERBL %u  %s",
                     L->colors[0], L->colors[1], L->colors[2], L->colors[3],
                     L->colors[4], L->colors[5], L->colors[6], L->colors[7],
                     L->colorsplit, L->vblank, (L->mode & 1) ? "high-res" : "low-res");
        }
    }
}

int astrovid_tab_hover(astrovid_tab *t, int x, int y)
{
    char old[sizeof t->hover.text];
    if (!t) return 0;
    snprintf(old, sizeof old, "%s", t->hover.valid ? t->hover.text : "");
    t->hx = x; t->hy = y;
    locate(t, x, y, &t->hover);
    return strcmp(old, t->hover.valid ? t->hover.text : "") != 0;
}

void astrovid_tab_pin(astrovid_tab *t, int x, int y)
{
    int z;
    if (!t) return;
    z = t->ez ? t->ez : 1;
    if (t->px >= 0 && x / z == t->px / z && y / z == t->py / z) {
        astrovid_tab_unpin(t);
        return;
    }
    t->px = x; t->py = y;
    locate(t, x, y, &t->pin);
    if (!t->pin.valid) astrovid_tab_unpin(t);
}

void astrovid_tab_unpin(astrovid_tab *t)
{
    if (!t) return;
    t->px = t->py = -1;
    memset(&t->pin, 0, sizeof t->pin);
}

const astrovid_pixel *astrovid_tab_pixel(const astrovid_tab *t)
{
    if (!t) return NULL;
    if (t->pin.valid) return &t->pin;
    if (t->hover.valid) return &t->hover;
    return NULL;
}

int astrovid_tab_marks(const astrovid_tab *t, int hover[4], int pin[4])
{
    int r = 0, z;
    if (!t) return 0;
    z = t->ez ? t->ez : 1;
    if (t->hover.valid) {
        hover[0] = t->hover.x * z; hover[1] = t->hover.y * z; hover[2] = z; hover[3] = z;
        r |= 1;
    }
    if (t->pin.valid) {
        pin[0] = t->pin.x * z; pin[1] = t->pin.y * z; pin[2] = z; pin[3] = z;
        r |= 2;
    }
    return r;
}

int astrovid_tab_break_on_pixel(astrovid_tab *t)
{
    const astrovid_pixel *p = astrovid_tab_pixel(t);
    astrodebug *d = astrodebug_get();
    char err[64];
    int id, id2;

    if (!p || !p->active || !d) return -1;
    {   /* not twice for the same byte */
        astrodebug_breakpoint bl[128];
        const int n = astrodebug_breakpoint_list(d, bl, 128);
        for (int i = 0; i < n; i++)
            if ((bl[i].type & ASTRODEBUG_BP_WRITE) && bl[i].start == p->addr && bl[i].end == p->addr)
                return bl[i].id;
    }
    id = astrodebug_breakpoint_add(d, ASTRODEBUG_BP_WRITE, (uint16_t)p->addr, (uint16_t)p->addr,
                                   NULL, err, sizeof err);
    id2 = astrodebug_breakpoint_add(d, ASTRODEBUG_BP_WRITE, (uint16_t)p->mirror, (uint16_t)p->mirror,
                                    NULL, err, sizeof err);
    (void)id2;
    return id;
}

int astrovid_tab_pixel_palette_index(const astrovid_tab *t)
{
    const astrovid_pixel *p = astrovid_tab_pixel(t);
    return p && p->valid ? p->pal : -1;
}

int astrovid_tab_break_on_swatch(astrovid_tab *t, int index)
{
    astrodebug *d = astrodebug_get();
    char err[64];
    (void)t;
    if (!d || index < 0 || index > 7) return -1;
    return astrodebug_breakpoint_add(d, ASTRODEBUG_BP_OUT, (uint16_t)index, (uint16_t)index,
                                     NULL, err, sizeof err);
}

/* ---- the tables ---------------------------------------------------------------------------- */

static void reg_set(astrovid_tab *t, int *n, const char *name, const char *value,
                    const char *fmt, ...) __attribute__((format(printf, 5, 6)));
static void reg_set(astrovid_tab *t, int *n, const char *name, const char *value,
                    const char *fmt, ...)
{
    astrovid_reg *r;
    va_list ap;
    int prev_age;
    if (*n >= MAX_REGS) return;
    r = &t->regs[*n];
    prev_age = r->age;
    r->name = name;
    snprintf(r->value, sizeof r->value, "%s", value);
    va_start(ap, fmt);
    vsnprintf(r->meaning, sizeof r->meaning, fmt, ap);
    va_end(ap);
    if (!t->regs_primed || strcmp(t->prev_val[*n], r->value) != 0)
        r->age = t->regs_primed ? 0 : 99;
    else
        r->age = prev_age < 99 ? prev_age + 1 : 99;
    memcpy(t->prev_val[*n], r->value, sizeof t->prev_val[*n]);
    (*n)++;
}

static void build_tables(astrovid_tab *t, int tick)
{
    const astrovid_snapshot *s = &t->cap->snap;
    static const char *const cn[8] = { "COL0R", "COL1R", "COL2R", "COL3R",
                                       "COL0L", "COL1L", "COL2L", "COL3L" };
    int n = 0, step, bpl, w, h;
    char v[24];

    geom(s, &step, &bpl, &w, &h);
    (void)tick;

    /* registers (ages carry over from the previous build) */
    for (int i = 0; i < 8; i++) {
        snprintf(v, sizeof v, "%02X", s->colors[i]);
        reg_set(t, &n, cn[i], v, "port %02X: hue %d, luma %d  #%06X", i,
                s->colors[i] >> 3, (s->colors[i] & 7) << 1, pen_rgb(s, s->colors[i]) & 0xffffff);
    }
    snprintf(v, sizeof v, "%d", s->video_mode & 1);
    reg_set(t, &n, "MODE 08", v, "%s", (s->video_mode & 1) ? "high-res 320x204 (commercial)"
                                                            : "low-res 160x102 (consumer)");
    snprintf(v, sizeof v, "%02X", ((s->bgdata & 3) << 6) | (s->colorsplit / 2));
    reg_set(t, &n, "HORCB 09", v, "boundary at pixel %d of %d (%u groups), background pen %d",
            s->colorsplit * 4 / step, w, s->colorsplit, s->bgdata & 3);
    snprintf(v, sizeof v, "%u", s->vblank);
    reg_set(t, &n, "VERBL 0A", v, "last active line; %d rows of screen RAM drawn", (s->vblank + step - 1) / step);
    snprintf(v, sizeof v, "%02X", s->interrupt_vector);
    reg_set(t, &n, "INFBK 0D", v, "interrupt vector");
    snprintf(v, sizeof v, "%02X", s->interrupt_enabl);
    reg_set(t, &n, "INMOD 0E", v, "scanline interrupt %s (mode %d), light pen %s",
            (s->interrupt_enabl & 8) ? "on" : "off", (s->interrupt_enabl & 4) ? 1 : 0,
            (s->interrupt_enabl & 2) ? "on" : "off");
    snprintf(v, sizeof v, "%u", s->interrupt_scanline);
    reg_set(t, &n, "INLIN 0F", v, "fires on astrocade line %u (screen line %u)",
            s->interrupt_scanline, s->interrupt_scanline + ASTRO_VERT_OFFSET);
    snprintf(v, sizeof v, "%u/%u", s->expand_color[0], s->expand_color[1]);
    reg_set(t, &n, "XPAND 19", v, "expand colours 0/1");
    snprintf(v, sizeof v, "%02X", s->funcgen_control);
    reg_set(t, &n, "MAGIC 0C", v, "shift %u%s%s%s%s%s", s->funcgen_control & 3,
            (s->funcgen_control & 0x04) ? ", rotate" : "", (s->funcgen_control & 0x08) ? ", expand" : "",
            (s->funcgen_control & 0x10) ? ", OR" : "", (s->funcgen_control & 0x20) ? ", XOR" : "",
            (s->funcgen_control & 0x40) ? ", flop" : "");
    snprintf(v, sizeof v, "%02X", s->intercept);
    reg_set(t, &n, "IN 08", v, "magic intercept feedback");
    {
        static const char *const sn[8] = { "TONMO 10", "TONEA 11", "TONEB 12", "TONEC 13",
                                           "VIBR 14", "VOLC 15", "VOLAB 16", "VOLN 17" };
        for (int i = 0; i < 8; i++) {
            snprintf(v, sizeof v, "%02X", s->snd_reg[i]);
            reg_set(t, &n, sn[i], v, "sound register");
        }
    }
    t->nregs = n;
    t->regs_primed = 1;

    /* colours: usage counts need a decode pass when the picture is not Decoded/Values */
    if (t->view != ASTROVID_V_DECODED && t->view != ASTROVID_V_VALUES)
        decode(t, NULL, 0);
    for (int i = 0; i < 8; i++) {
        t->sw[i].name = cn[i];
        t->sw[i].port = i;
        t->sw[i].value = s->colors[i];
        t->sw[i].rgb = pen_rgb(s, s->colors[i]);
        t->sw[i].uses = t->sw_uses[i];
    }
    {
        const astrovid_pixel *p = astrovid_tab_pixel(t);
        for (int r = 0; r < 256; r++) {
            t->pal[r].reg = r;
            t->pal[r].pal = r * 2;
            t->pal[r].rgb = pen_rgb(s, r);
            t->pal[r].uses = t->reg_uses[r];
            t->pal[r].regs_mask = 0;
            for (int i = 0; i < 8; i++)
                if (s->colors[i] == r) t->pal[r].regs_mask |= 1 << i;
            t->pal[r].inspected = p && p->regval == r;
        }
    }

    /* lines: where the registers change */
    t->nrows = 0;
    for (int y = 0; y < SH; y++) {
        const astrovid_line *L = &t->cap->lines[y];
        int mask = 0;
        if (y > 0) {
            const astrovid_line *P = &t->cap->lines[y - 1];
            if (memcmp(L->colors, P->colors, 8)) mask |= 1;
            if (L->colorsplit != P->colorsplit || L->bgdata != P->bgdata) mask |= 2;
            if (L->vblank != P->vblank) mask |= 4;
            if (L->mode != P->mode) mask |= 8;
        }
        if (y == 0 || mask) {
            astrovid_line_row *r = &t->rows[t->nrows++];
            r->line = y;
            r->astro_line = (y - ASTRO_VERT_OFFSET + ASTRO_LINES_PER_FRAME) % ASTRO_LINES_PER_FRAME;
            memcpy(r->colors, L->colors, 8);
            r->colorsplit = L->colorsplit; r->bgdata = L->bgdata;
            r->vblank = L->vblank; r->mode = L->mode;
            r->changed_mask = mask;
        }
    }

    /* timing */
    t->ntiming = 0;
#define TL(...) do { if (t->ntiming < MAX_TIMING) { \
        snprintf(t->timing_buf[t->ntiming], sizeof t->timing_buf[0], __VA_ARGS__); \
        t->timing_ptr[t->ntiming] = t->timing_buf[t->ntiming]; t->ntiming++; } } while (0)
    TL("Beam\tline %d of %d", s->scanline, ASTRO_LINES_PER_FRAME);
    TL("Astrocade line\t%d", (s->scanline - ASTRO_VERT_OFFSET + ASTRO_LINES_PER_FRAME) % ASTRO_LINES_PER_FRAME);
    TL("Frame\t%llu", (unsigned long long)t->cap->frame);
    TL("Scanline interrupt\t%s, astrocade line %u", (s->interrupt_enabl & 8) ? "on" : "off",
       s->interrupt_scanline);
    TL("Colour changes this frame\t%d line%s", t->nrows - 1, t->nrows == 2 ? "" : "s");
    TL("Machine\t%s", t->stopped ? "stopped" : "running");
#undef TL

    snprintf(t->status, sizeof t->status,
             "%s view, %s, %d colour change%s in the frame\n%s%s",
             k_view_names[t->view], (s->video_mode & 1) ? "high-res 320x204" : "low-res 160x102",
             t->nrows - 1, t->nrows == 2 ? "" : "s",
             t->stopped ? "Stopped" : "Running: showing the last finished frame",
             t->stopped ? (t->has_fb ? "" : " (no frame has finished yet)") : "");
}

/* ---- refresh -------------------------------------------------------------------------------- */

unsigned astrovid_tab_refresh(astrovid_tab *t, int force)
{
    uint64_t sig;
    unsigned r = 0;
    int fresh;

    if (!t) return 0;
    if (!acquire(t)) {
        why_set(t, "The machine is not running.");
        t->pw = t->ph = 0;
        return ASTROVID_TAB_OFF | ASTROVID_TAB_PICTURE | ASTROVID_TAB_TABLES;
    }
    sig = signature(t);
    fresh = force || sig != t->sig;
    if (fresh) {
        t->sig = sig;
        note_vram(t);
    } else if (t->any_heat) {
        note_vram(t);               /* the fade goes on while nothing happens */
    }
    if (fresh || t->dirty || (t->any_heat && (t->ov & ASTROVID_OV_HEAT))) {
        build_picture(t);
        t->dirty = 0;
        r |= ASTROVID_TAB_PICTURE;
    }
    if (fresh) {
        if (t->hx >= 0) locate(t, t->hx, t->hy, &t->hover);
        if (t->px >= 0) locate(t, t->px, t->py, &t->pin);
        build_tables(t, 1);
        r |= ASTROVID_TAB_TABLES;
    }
    return r;
}

const uint32_t *astrovid_tab_picture(astrovid_tab *t, int *w, int *h)
{
    if (!t || !t->pw) return NULL;
    if (w) *w = t->pw;
    if (h) *h = t->ph;
    return t->pic;
}
const char *astrovid_tab_why(const astrovid_tab *t) { return t ? t->why : ""; }
int astrovid_tab_effective_zoom(const astrovid_tab *t) { return t ? t->ez : 1; }
const char *astrovid_tab_status(const astrovid_tab *t) { return t ? t->status : ""; }
int astrovid_tab_regs(const astrovid_tab *t, const astrovid_reg **out)
{
    if (out) *out = t->regs;
    return t->nregs;
}
int astrovid_tab_palette(const astrovid_tab *t, const astrovid_pal_entry **out)
{
    /* the inspected pixel moves between refreshes */
    const astrovid_pixel *p = astrovid_tab_pixel(t);
    astrovid_pal_entry *pe = ((astrovid_tab *)t)->pal;
    for (int r = 0; r < 256; r++)
        pe[r].inspected = p && p->regval == r;
    if (out) *out = t->pal;
    return 256;
}
int astrovid_tab_swatches(const astrovid_tab *t, const astrovid_swatch **out)
{
    if (out) *out = t->sw;
    return 8;
}
int astrovid_tab_lines(const astrovid_tab *t, const astrovid_line_row **out)
{
    if (out) *out = t->rows;
    return t->nrows;
}
int astrovid_tab_timing(const astrovid_tab *t, const char *const **lines)
{
    if (lines) *lines = (const char *const *)t->timing_ptr;
    return t->ntiming;
}
int astrovid_tab_beam_permille(const astrovid_tab *t)
{
    return t && t->have ? t->cap->snap.scanline * 1000 / ASTRO_LINES_PER_FRAME : 0;
}

int astrovid_tab_text(astrovid_tab *t, int section, char *dst, int dstsz)
{
    int n = 0;
#define P(...) do { if (n < dstsz) n += snprintf(dst + n, (size_t)(dstsz - n), __VA_ARGS__); } while (0)
    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!t || !t->have) { P("The machine is not running.\n"); return n; }
    switch (section) {
    case ASTROVID_S_REGISTERS:
        for (int i = 0; i < t->nregs; i++)
            P("%-9s %-5s %s\n", t->regs[i].name, t->regs[i].value, t->regs[i].meaning);
        break;
    case ASTROVID_S_COLOURS:
        for (int i = 0; i < 8; i++)
            P("%s  %02X  #%06X  %ld pixels\n", t->sw[i].name, t->sw[i].value,
              t->sw[i].rgb & 0xffffff, t->sw[i].uses);
        break;
    case ASTROVID_S_LINES:
        for (int i = 0; i < t->nrows; i++) {
            const astrovid_line_row *r = &t->rows[i];
            P("line %3d  R %02X %02X %02X %02X  L %02X %02X %02X %02X  boundary %3u  VERBL %3u  %s\n",
              r->line, r->colors[0], r->colors[1], r->colors[2], r->colors[3], r->colors[4],
              r->colors[5], r->colors[6], r->colors[7], r->colorsplit, r->vblank,
              r->mode ? "high" : "low");
        }
        break;
    case ASTROVID_S_TIMING:
        for (int i = 0; i < t->ntiming; i++) P("%s\n", t->timing_buf[i]);
        break;
    default:
        P("%s\n", t->status);
        if (astrovid_tab_pixel(t)) P("%s\n", astrovid_tab_pixel(t)->text);
        break;
    }
#undef P
    return n < dstsz ? n : dstsz - 1;
}
