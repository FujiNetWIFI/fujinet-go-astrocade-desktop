/* vidtab: the Video tab's controller, fed synthetic frames (no ROMs, no machine). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "astrovidtab.h"

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s (line %d)\n", m, __LINE__); fails++; } } while (0)

/* a palette where pen index N has a unique, recognisable colour */
static uint32_t pal_color(int i) { return 0xff000000u | (uint32_t)((i * 37) & 255) << 16 | (uint32_t)((i * 11) & 255) << 8 | (uint32_t)(i & 255); }

static void make_cap(astrovid_capture *c, int mode)
{
    memset(c, 0, sizeof *c);
    for (int i = 0; i < 512; i++) c->snap.palette[i] = pal_color(i);
    c->snap.video_mode = (uint8_t)mode;
    c->snap.vblank = 204;
    c->snap.scanline = 100;
    for (int i = 0; i < 8; i++) c->snap.colors[i] = (uint8_t)(0x10 + i);
    for (int y = 0; y < 240; y++) {
        astrovid_line *L = &c->lines[y];
        for (int i = 0; i < 8; i++) L->colors[i] = (uint8_t)(0x10 + i);      /* right 10..13, left 14..17 */
        L->colorsplit = 0;
        L->vblank = 204;
        L->mode = (uint8_t)mode;
    }
    for (int i = 0; i < 352 * 240; i++) c->fb[i] = 0xff123456u;
    c->serial = 1;
    c->frame = 7;
}

static uint32_t expect(const astrovid_capture *c, int reg) { return c->snap.palette[(reg << 1) & 0x1ff] | 0xff000000u; }

int main(void)
{
    static astrovid_capture cap;
    static uint32_t live[352 * 240];
    astrovid_tab *t = astrovid_tab_new();
    const uint32_t *pic;
    int w, h;
    unsigned r;

    CHECK(t != NULL, "new");
    astrovid_tab_set_overlays(t, 0);
    astrovid_tab_set_zoom(t, 1);

    /* ---- Decoded: low-res geometry and colours ---- */
    make_cap(&cap, 0);
    cap.snap.vram[0] = 0x1B;            /* pens 0,1,2,3 */
    astrovid_tab_feed(t, &cap, NULL);
    astrovid_tab_set_view(t, ASTROVID_V_DECODED);
    r = astrovid_tab_refresh(t, 1);
    CHECK(r & ASTROVID_TAB_PICTURE, "picture built");
    pic = astrovid_tab_picture(t, &w, &h);
    CHECK(pic && w == 160 && h == 102, "low-res decoded is 160x102");
    CHECK(pic && pic[0] == expect(&cap, 0x10) && pic[1] == expect(&cap, 0x11) &&
          pic[2] == expect(&cap, 0x12) && pic[3] == expect(&cap, 0x13),
          "boundary 0: all pixels use the right set");
    {
        int opaque = 1;
        for (int i = 0; pic && i < w * h; i++) if ((pic[i] >> 24) != 0xff) opaque = 0;
        CHECK(opaque, "every pixel opaque");
    }

    /* boundary at 8 groups: the first 4 bytes (low-res: effx 0,2,4,6) use the LEFT set */
    for (int y = 0; y < 240; y++) cap.lines[y].colorsplit = 8;
    astrovid_tab_feed(t, &cap, NULL);
    astrovid_tab_refresh(t, 1);
    pic = astrovid_tab_picture(t, &w, &h);
    CHECK(pic && pic[0] == expect(&cap, 0x14) && pic[3] == expect(&cap, 0x17),
          "left of the boundary uses COL4-7");
    CHECK(pic && pic[4 * 4] == expect(&cap, 0x10), "right of the boundary uses COL0-3 (byte 4, pen 0)");

    /* a colour change from the interrupt: lines from 60 on use other colours */
    for (int y = 60; y < 240; y++) for (int i = 0; i < 8; i++) cap.lines[y].colors[i] = (uint8_t)(0x40 + i);
    astrovid_tab_feed(t, &cap, NULL);
    astrovid_tab_refresh(t, 1);
    pic = astrovid_tab_picture(t, &w, &h);
    {
        /* row r draws screen line 2r+22: row 10 -> line 42 (before the change at 60), row 30 -> 82 (after) */
        CHECK(pic && pic[(size_t)10 * w] == expect(&cap, 0x14), "an early row keeps the old colours");
        CHECK(pic && pic[(size_t)30 * w] == expect(&cap, 0x44), "a row after the change uses the new colours");
    }
    {
        const astrovid_line_row *rows;
        int n = astrovid_tab_lines(t, &rows);
        CHECK(n == 2 && rows[1].line == 60 && (rows[1].changed_mask & 1), "the Lines table finds the change at line 60");
    }

    /* VERBL: rows at or past the last active line show the background pen */
    make_cap(&cap, 0);
    cap.snap.vram[(size_t)20 * 40] = 0x55;              /* row 20 = astrocade line 40 */
    for (int y = 0; y < 240; y++) { cap.lines[y].vblank = 80; cap.lines[y].bgdata = 0xAA; }
    astrovid_tab_feed(t, &cap, NULL);
    astrovid_tab_refresh(t, 1);
    pic = astrovid_tab_picture(t, &w, &h);
    CHECK(pic && pic[(size_t)20 * w] == expect(&cap, 0x11), "inside VERBL: screen RAM");
    CHECK(pic && pic[(size_t)45 * w] == expect(&cap, 0x12), "past VERBL: background pen (bgdata 0xAA = pen 2)");

    /* ---- high-res geometry ---- */
    make_cap(&cap, 1);
    astrovid_tab_feed(t, &cap, NULL);
    astrovid_tab_refresh(t, 1);
    pic = astrovid_tab_picture(t, &w, &h);
    CHECK(pic && w == 320 && h == 204, "high-res decoded is 320x204");

    /* ---- Display / Beam / Values ---- */
    astrovid_tab_set_view(t, ASTROVID_V_DISPLAY);
    astrovid_tab_refresh(t, 1);
    pic = astrovid_tab_picture(t, &w, &h);
    CHECK(pic && w == 352 && h == 240 && pic[0] == 0xff123456u, "Display is the captured frame");

    astrovid_tab_set_view(t, ASTROVID_V_BEAM);
    astrovid_tab_refresh(t, 1);
    CHECK(astrovid_tab_picture(t, &w, &h) == NULL && strlen(astrovid_tab_why(t)) > 10,
          "Beam needs a stopped machine and says why");
    for (int i = 0; i < 352 * 240; i++) live[i] = 0xff00ff00u;
    astrovid_tab_feed(t, &cap, live);
    astrovid_tab_refresh(t, 1);
    pic = astrovid_tab_picture(t, &w, &h);
    CHECK(pic && pic[(size_t)10 * 352] == 0xff00ff00u, "Beam: above the beam is the live frame");
    CHECK(pic && pic[(size_t)200 * 352] != 0xff123456u && pic[(size_t)200 * 352] != 0xff00ff00u,
          "Beam: below it is the last frame, dimmed");

    astrovid_tab_set_view(t, ASTROVID_V_VALUES);
    cap.snap.vram[0] = 0x1B;
    astrovid_tab_feed(t, &cap, NULL);
    astrovid_tab_refresh(t, 1);
    pic = astrovid_tab_picture(t, &w, &h);
    CHECK(pic && pic[0] != pic[1] && pic[1] != pic[2], "Values: four distinct pens");

    /* ---- zoom, fit, cap ---- */
    astrovid_tab_set_view(t, ASTROVID_V_DECODED);
    make_cap(&cap, 0);
    astrovid_tab_feed(t, &cap, NULL);
    astrovid_tab_set_zoom(t, 4);
    astrovid_tab_refresh(t, 1);
    pic = astrovid_tab_picture(t, &w, &h);
    CHECK(pic && w == 640 && h == 408 && astrovid_tab_effective_zoom(t) == 4, "zoom 4");
    astrovid_tab_set_zoom(t, 0);
    astrovid_tab_set_viewport(t, 1000, 500);
    astrovid_tab_refresh(t, 1);
    pic = astrovid_tab_picture(t, &w, &h);
    CHECK(pic && astrovid_tab_effective_zoom(t) == 4 && w <= 1000 && h <= 500, "fit picks the largest zoom that fits");
    astrovid_tab_set_zoom(t, 16);
    astrovid_tab_refresh(t, 1);
    pic = astrovid_tab_picture(t, &w, &h);
    CHECK(pic && (long)w * h <= 6000000L, "the pixel cap holds at zoom 16");

    /* ---- the inspector ---- */
    astrovid_tab_set_zoom(t, 4);
    make_cap(&cap, 0);
    for (int y = 0; y < 240; y++) cap.lines[y].colorsplit = 8;
    cap.snap.vram[2] = 0xC0;            /* byte 2, pixel 0 = pen 3 */
    astrovid_tab_feed(t, &cap, NULL);
    astrovid_tab_refresh(t, 1);
    {
        const astrovid_pixel *p;
        int mk[4], pk[4];
        astrovid_tab_hover(t, 8 * 4 + 1, 0);            /* picture px (8,0) = byte 2 pixel 0 */
        p = astrovid_tab_pixel(t);
        CHECK(p && p->valid && p->active && p->offset == 2 && p->addr == 0x4002 && p->mirror == 2, "hover: byte and addresses");
        CHECK(p && p->pen == 3 && p->left && p->reg == 7 && p->regval == 0x17, "hover: pen, left set, COL3L");
        CHECK(p && p->pal == (0x17 << 1), "hover: palette index");
        CHECK((astrovid_tab_marks(t, mk, pk) & 1) && mk[0] == 32 && mk[2] == 4, "hover mark rect");
        astrovid_tab_pin(t, 8 * 4, 0);
        CHECK(astrovid_tab_marks(t, mk, pk) & 2, "pin");
        astrovid_tab_pin(t, 8 * 4 + 2, 1);
        CHECK(!(astrovid_tab_marks(t, mk, pk) & 2), "pinning the same pixel again unpins");
        astrovid_tab_hover(t, -1, -1);
        CHECK(astrovid_tab_pixel(t) == NULL, "pointer left");
        astrovid_tab_hover(t, 100000, 5);
        CHECK(astrovid_tab_pixel(t) == NULL, "outside the picture");
    }

    /* ---- the Display inspector maps back to the same screen RAM byte ---- */
    astrovid_tab_set_view(t, ASTROVID_V_DISPLAY);
    astrovid_tab_set_zoom(t, 1);
    astrovid_tab_refresh(t, 1);
    {
        /* screen line 22 = astrocade line 0; low-res: 8 screen px per byte, data area starts at x=16 */
        const astrovid_pixel *p;
        astrovid_tab_hover(t, 16 + 8 * 2, 22);
        p = astrovid_tab_pixel(t);
        CHECK(p && p->active && p->offset == 2, "Display: x=32,y=22 is screen RAM byte 2");
        astrovid_tab_hover(t, 5, 22);
        p = astrovid_tab_pixel(t);
        CHECK(p && p->valid && !p->active, "Display: the left border is outside the data area");
    }

    /* ---- registers: change age, colour usage, palette ---- */
    {
        const astrovid_reg *rg; const astrovid_swatch *sw; const astrovid_pal_entry *pe;
        int n;
        astrovid_tab_set_view(t, ASTROVID_V_DECODED);
        make_cap(&cap, 0);
        astrovid_tab_feed(t, &cap, NULL);
        astrovid_tab_refresh(t, 1);
        cap.snap.colors[0] = 0x99;
        cap.snap.interrupt_scanline = 33;
        cap.snap.scanline = 101;                        /* a different signature */
        astrovid_tab_feed(t, &cap, NULL);
        astrovid_tab_refresh(t, 1);
        n = astrovid_tab_regs(t, &rg);
        CHECK(n > 10 && !strcmp(rg[0].name, "COL0R") && rg[0].age == 0, "a changed register has age 0");
        CHECK(rg[1].age > 0, "an unchanged register ages");
        n = astrovid_tab_swatches(t, &sw);
        CHECK(n == 8 && sw[7].port == 7 && sw[7].value == 0x17, "swatches");
        n = astrovid_tab_palette(t, &pe);
        CHECK(n == 256 && pe[0x11].pal == 0x22 && (pe[0x11].regs_mask & 2), "palette entries (reachable pens only)");
    }

    /* ---- overlays ---- */
    {
        const uint32_t *a, *b2;
        astrovid_tab_set_view(t, ASTROVID_V_DECODED);
        astrovid_tab_set_zoom(t, 2);
        make_cap(&cap, 0);
        astrovid_tab_feed(t, &cap, NULL);
        astrovid_tab_set_overlays(t, 0);
        astrovid_tab_refresh(t, 1);
        a = astrovid_tab_picture(t, &w, &h);
        {
            uint32_t *copy = malloc((size_t)w * h * 4);
            int diff = 0;
            memcpy(copy, a, (size_t)w * h * 4);
            astrovid_tab_set_overlays(t, ASTROVID_OV_ALL);
            astrovid_tab_refresh(t, 1);
            b2 = astrovid_tab_picture(t, &w, &h);
            for (int i = 0; i < w * h; i++) if (copy[i] != b2[i]) diff++;
            CHECK(diff > 100, "overlays draw something");
            free(copy);
        }
    }

    /* ---- fuzz: random registers never crash or leave holes ---- */
    srand(12345);
    for (int it = 0; it < 200; it++) {
        make_cap(&cap, rand() & 1);
        for (int i = 0; i < 4096; i++) cap.snap.vram[i] = (uint8_t)rand();
        for (int y = 0; y < 240; y++) {
            for (int i = 0; i < 8; i++) cap.lines[y].colors[i] = (uint8_t)rand();
            cap.lines[y].colorsplit = (uint8_t)(rand() & 0x7e);
            cap.lines[y].vblank = (uint8_t)rand();
            cap.lines[y].bgdata = (uint8_t)rand();
        }
        cap.snap.scanline = rand() % 262;
        cap.snap.interrupt_enabl = (uint8_t)rand();
        cap.snap.interrupt_scanline = (uint8_t)rand();
        astrovid_tab_set_view(t, it % ASTROVID_V_COUNT);
        astrovid_tab_set_zoom(t, (int[]){ 0, 1, 2, 3, 4, 6, 8, 12, 16 }[it % 9]);
        astrovid_tab_set_overlays(t, (unsigned)rand());
        astrovid_tab_feed(t, &cap, (it & 1) ? live : NULL);
        astrovid_tab_refresh(t, 1);
        pic = astrovid_tab_picture(t, &w, &h);
        astrovid_tab_hover(t, rand() % 700, rand() % 500);
        if (pic) {
            int opaque = 1;
            for (int i = 0; i < w * h; i++) if ((pic[i] >> 24) != 0xff) { opaque = 0; break; }
            if (!opaque) { CHECK(0, "fuzz: opaque"); break; }
        }
    }
    astrovid_tab_free(t);
    printf(fails ? "vidtab_test: %d failed\n" : "vidtab_test: ok\n", fails);
    return fails != 0;
}
