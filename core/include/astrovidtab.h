/*
 * astrovidtab -- the Video chip tab's controller. All four frontends draw the
 * same tab from it, so what they own is widgets: a view and zoom chooser,
 * overlay toggles, a picture, the inspector and four tables. The lessons of
 * the other debuggers' video tabs live here once:
 *
 *   - the machine is only COPIED: a finished frame (picture, registers, and
 *     the registers each line was drawn with) is captured once per frame on
 *     the machine thread, and a stopped machine is read while it is parked.
 *     Decoding, scaling and overlays run on the caller's (UI) thread;
 *   - the picture is rebuilt only when something it shows changed, into one
 *     reused buffer;
 *   - hover and pin are state here, so a mouse move never re-renders the
 *     picture: draw the marks from astrovid_tab_marks() over the cached image;
 *   - pixels are 0xFFRRGGBB (opaque), whatever the toolkit does with alpha;
 *   - one table of overlays and one env-var parser, so the frontends cannot
 *     disagree.
 *
 * The Astrocade can change colours, the colour boundary, VERBL and the mode
 * between lines (from the INLIN interrupt, or code timed to the beam). The
 * Display view is the frame as the TV showed it; Decoded redraws screen RAM
 * through each line's own registers; Values shows the raw pens.
 *
 * UI thread only. Not thread-safe.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ASTROVIDTAB_H
#define ASTROVIDTAB_H

#include <stdint.h>

#include "astrodebug.h"
#include "astrovid.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct astrovid_tab astrovid_tab;

enum astrovid_view {
    ASTROVID_V_DISPLAY = 0,     /* the last finished frame, as the TV showed it */
    ASTROVID_V_DECODED,         /* screen RAM through each line's registers */
    ASTROVID_V_BEAM,            /* this frame above the beam, the last one dimmed below (stopped) */
    ASTROVID_V_VALUES,          /* raw pens 0-3; warm = right set, cool = left set */
    ASTROVID_V_COUNT
};

/* Overlays: one table, bit per entry. */
enum {
    ASTROVID_OV_GRID     = 1u << 0,  /* byte cells; pixel cells when zoomed in */
    ASTROVID_OV_BOUNDARY = 1u << 1,  /* the horizontal colour boundary */
    ASTROVID_OV_VERBL    = 1u << 2,  /* the last active line */
    ASTROVID_OV_INLIN    = 1u << 3,  /* the line the scanline interrupt fires on */
    ASTROVID_OV_BEAM     = 1u << 4,  /* where the beam is */
    ASTROVID_OV_HEAT     = 1u << 5,  /* screen RAM bytes written lately (fades) */
    ASTROVID_OV_CHANGED  = 1u << 6,  /* changed since the last refresh */
    ASTROVID_OV_ACTIVE   = 1u << 7,  /* outline of the data area (Display, Beam) */
    ASTROVID_OV_ALL      = 0xFFu
};
typedef struct {
    unsigned bit;
    const char *name;           /* the env name: grid, boundary, ... */
    const char *label;          /* the checkbox text */
} astrovid_overlay_info;
const astrovid_overlay_info *astrovid_overlays(int *count);
const char *astrovid_view_name(int view);          /* "Display", ... */
const char *astrovid_view_key(int view);           /* "display", ... (env) */

/* Right-hand sections. */
enum { ASTROVID_S_PIXEL = 0, ASTROVID_S_REGISTERS, ASTROVID_S_COLOURS,
       ASTROVID_S_LINES, ASTROVID_S_TIMING, ASTROVID_S_COUNT };
const char *astrovid_section_name(int section);
const char *astrovid_section_key(int section);

astrovid_tab *astrovid_tab_new(void);
void          astrovid_tab_free(astrovid_tab *t);

/* ---- what to show (they mark the picture stale) ------------------------------------ */
void     astrovid_tab_set_view(astrovid_tab *t, int view);
int      astrovid_tab_view(const astrovid_tab *t);
void     astrovid_tab_set_overlays(astrovid_tab *t, unsigned ov);
unsigned astrovid_tab_overlays(const astrovid_tab *t);
/* 1, 2, 3, 4, 6, 8, 12, 16, or 0 for "fit the viewport" */
void     astrovid_tab_set_zoom(astrovid_tab *t, int zoom);
int      astrovid_tab_zoom(const astrovid_tab *t);
/* the room the picture has, for Fit; call when the widget resizes */
void     astrovid_tab_set_viewport(astrovid_tab *t, int w, int h);
/* the zoom steps the chooser offers (0 = Fit first) */
const int *astrovid_zooms(int *count);

/*
 * The scripted-capture variables, read once by astrovid_tab_new:
 *   ASTRO_DEBUGGER_VIDEO_VIEW      display decoded beam values
 *   ASTRO_DEBUGGER_VIDEO_OVERLAYS  grid,boundary,verbl,inlin,beam,heat,changed,active | all | none
 *   ASTRO_DEBUGGER_VIDEO_ZOOM      1..16 or fit
 *   ASTRO_DEBUGGER_VIDEO_SECTION   pixel registers colours lines timing   (names, never indexes)
 * Returns the starting section, ASTROVID_S_PIXEL when unset.
 */
int astrovid_tab_env_section(const astrovid_tab *t);

/* ---- refresh ------------------------------------------------------------------------ */
#define ASTROVID_TAB_PICTURE 1u  /* repaint the picture (astrovid_tab_picture) */
#define ASTROVID_TAB_TABLES  2u  /* refill the status line and the sections */
#define ASTROVID_TAB_OFF     4u  /* no machine: show astrovid_tab_why() instead of a picture */

/* Take the machine's state, update the trackers, and say what to redraw.
 * force: redraw everything. */
unsigned astrovid_tab_refresh(astrovid_tab *t, int force);

/* For tests: use this frame instead of the machine. live_fb is the frame in
 * progress (NULL: a running machine, so no Beam view). */
void astrovid_tab_feed(astrovid_tab *t, const astrovid_capture *cap,
                       const uint32_t *live_fb);

/* ---- the picture ------------------------------------------------------------------- */
/*
 * The rendered picture, 0xFFRRGGBB, valid until the next refresh or set_*. NULL
 * when this view cannot be drawn: astrovid_tab_why() says why, and the tab
 * should show that text.
 */
const uint32_t *astrovid_tab_picture(astrovid_tab *t, int *w, int *h);
const char     *astrovid_tab_why(const astrovid_tab *t);
int             astrovid_tab_effective_zoom(const astrovid_tab *t);

/* ---- the tables -------------------------------------------------------------------- */
const char *astrovid_tab_status(const astrovid_tab *t);        /* two lines, '\n' between */

typedef struct {
    const char *name;           /* "COL2L", "MAGIC", ... */
    char value[24];             /* "5A" */
    char meaning[112];
    int age;                    /* refreshes since it last changed (12+: not recent) */
} astrovid_reg;
#define ASTROVID_REG_FADE 12
int astrovid_tab_regs(const astrovid_tab *t, const astrovid_reg **out);

typedef struct {
    int reg;                    /* register value 0..255 (what COLn holds) */
    int pal;                    /* palette index, 0..510 (reg * 2) */
    uint32_t rgb;               /* 0xFFRRGGBB */
    long uses;                  /* pixels of the Decoded view that use it */
    int regs_mask;              /* bit n: COLn holds this value */
    int inspected;              /* the inspected pixel uses it */
} astrovid_pal_entry;
/* The 256 pens the console can reach (luma bit 0 is always 0). */
int astrovid_tab_palette(const astrovid_tab *t, const astrovid_pal_entry **out);
/* the eight colour registers right now: reg value, rgb, uses (pixels) */
typedef struct {
    const char *name;           /* COL0R .. COL3L */
    int port;                   /* the OUT port that writes it */
    int value;
    uint32_t rgb;
    long uses;
} astrovid_swatch;
int astrovid_tab_swatches(const astrovid_tab *t, const astrovid_swatch **out);
/* an OUT breakpoint on a colour register's port; its id, or -1 */
int astrovid_tab_break_on_swatch(astrovid_tab *t, int index);

/* The per-line table: one entry for line 0 and for each line whose registers
 * differ from the line above. */
typedef struct {
    int line;                   /* screen line */
    int astro_line;             /* line - 22, mod 262 */
    uint8_t colors[8], colorsplit, bgdata, vblank, mode;
    int changed_mask;           /* bits: 0 colours, 1 boundary, 2 VERBL, 3 mode */
} astrovid_line_row;
int astrovid_tab_lines(const astrovid_tab *t, const astrovid_line_row **out);

/* the timing section, as ready-made "label\tvalue" lines */
int  astrovid_tab_timing(const astrovid_tab *t, const char *const **lines);
/* where the beam is in the whole frame, 0..1000 (0 when unknown) */
int  astrovid_tab_beam_permille(const astrovid_tab *t);

/* A section as plain text (the console's `video` command shares it). */
int  astrovid_tab_text(astrovid_tab *t, int section, char *dst, int dstsz);

/* ---- hover, pin ---------------------------------------------------------------------- */
typedef struct {
    int valid;
    int active;                 /* inside the data area: it reads screen RAM */
    int x, y;                   /* picture pixel */
    int line;                   /* screen line 0..239 */
    int astro_line;
    int column;                 /* pixel column within the data area, -1 if outside */
    int offset;                 /* screen RAM offset 0..$FFF */
    unsigned addr;              /* $4000 + offset */
    unsigned mirror;            /* $0000 + offset, the magic-function window */
    int pen;                    /* 0-3 */
    int left;                   /* left of the boundary (COL4-7) */
    int reg;                    /* colour register 0-7 */
    int regval;
    int pal;                    /* palette index */
    uint32_t rgb;
    uint8_t data;               /* the screen RAM byte */
    char text[640];             /* ready-made inspector text */
} astrovid_pixel;

/* x, y in picture pixels at the current zoom; -1,-1 when the pointer left.
 * Returns 1 when the inspector changed. */
int  astrovid_tab_hover(astrovid_tab *t, int x, int y);
void astrovid_tab_pin(astrovid_tab *t, int x, int y);      /* a click; the same spot unpins */
void astrovid_tab_unpin(astrovid_tab *t);
/* the pinned pixel if any, else the hovered one, else NULL */
const astrovid_pixel *astrovid_tab_pixel(const astrovid_tab *t);
/* rectangles x,y,w,h in picture pixels for the marks drawn over the picture;
 * bit0 of the result: hover valid, bit1: pin valid */
int  astrovid_tab_marks(const astrovid_tab *t, int hover[4], int pin[4]);
/* write breakpoints on the inspected pixel's byte, at $4000+ and its mirror; the first id, or -1 */
int  astrovid_tab_break_on_pixel(astrovid_tab *t);
/* the palette index (reg*2) the inspected pixel uses, or -1 */
int  astrovid_tab_pixel_palette_index(const astrovid_tab *t);

#ifdef __cplusplus
}
#endif

#endif
