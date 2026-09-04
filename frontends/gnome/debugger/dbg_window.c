/*
 * The debugger window: two tabs over the singleton astrodebug engine.
 *
 *  CPU    -- a control row (Pause/Resume, Step, Step Over, Step Out), the Z80
 *            register file, a disassembly listing from the current PC, and a
 *            memory hex view.
 *  Video  -- the live screen, the screen-RAM bitmap, the 512-pen palette
 *            strip, and the full decoded data-chip + sound register text
 *            (every register the Nutting manual documents).
 *
 * A GtkTimer polls astrodebug_stop_serial and refreshes when it changes,
 * rather than being called back on the emulator thread. One RGBA scratch
 * buffer, sized to the MAX of all the Video renders (the sibling ports'
 * Windows debugger corrupted memory sizing it to only one).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include "astrodebug.h"
#include "astrovid.h"

#define DISASM_LINES 24
#define MEM_ROWS 16
#define MEM_COLS 16

typedef struct {
    GtkWindow *win;
    astrosession *session;
    astrodebug *dbg;
    guint timer_id;
    uint64_t last_serial;

    GtkLabel *regs_label;
    GtkLabel *disasm_label;
    GtkLabel *mem_label;
    GtkLabel *status_label;

    GtkWidget *screen_pic;
    GtkWidget *bitmap_pic;
    GtkWidget *palette_pic;
    GtkLabel *vid_text;

    uint16_t mem_addr;
    uint32_t scratch[ASTROVID_MAX_PIXELS];  /* MAX of every Video render */
} DbgWindow;

static DbgWindow *g_dbg;

/* ---- CPU tab refresh ---- */

static void refresh_cpu(DbgWindow *d)
{
    if (!astrodebug_is_paused(d->dbg)) {
        gtk_label_set_text(d->regs_label, "(running — Pause to inspect)");
        gtk_label_set_text(d->disasm_label, "");
        gtk_label_set_text(d->mem_label, "");
        return;
    }

    astrodebug_regs r;
    if (!astrodebug_regs_get(d->dbg, &r))
        return;

    char buf[512];
    g_snprintf(buf, sizeof buf,
        "AF %04X   BC %04X   DE %04X   HL %04X\n"
        "AF'%04X   BC'%04X   DE'%04X   HL'%04X\n"
        "IX %04X   IY %04X   SP %04X   PC %04X\n"
        "I %02X  R %02X  IM %d  IFF1 %d  IFF2 %d  %s  WZ %04X",
        r.af, r.bc, r.de, r.hl, r.af2, r.bc2, r.de2, r.hl2,
        r.ix, r.iy, r.sp, r.pc, r.i, r.r, r.im, r.iff1, r.iff2,
        r.halt ? "HALT" : "    ", r.wz);
    gtk_label_set_text(d->regs_label, buf);

    astrodebug_dasm_line lines[DISASM_LINES];
    int n = astrodebug_disassemble(d->dbg, r.pc, lines, DISASM_LINES);
    char dis[2048];
    int p = 0;
    for (int i = 0; i < n; i++) {
        int isbp = astrodebug_breakpoint_is_set(d->dbg, lines[i].addr);
        p += g_snprintf(dis + p, sizeof dis - p, "%s%c %04X  %s\n",
                        i == 0 ? "▶" : " ", isbp ? '*' : ' ',
                        lines[i].addr, lines[i].text);
        if (p >= (int)sizeof dis - 64) break;
    }
    if (p == 0) dis[0] = '\0';
    gtk_label_set_text(d->disasm_label, dis);

    /* memory hex view from mem_addr */
    char mem[2048];
    p = 0;
    for (int row = 0; row < MEM_ROWS; row++) {
        uint16_t base = (uint16_t)(d->mem_addr + row * MEM_COLS);
        uint8_t bytes[MEM_COLS];
        astrodebug_read(d->dbg, base, bytes, MEM_COLS);
        p += g_snprintf(mem + p, sizeof mem - p, "%04X ", base);
        for (int c = 0; c < MEM_COLS; c++)
            p += g_snprintf(mem + p, sizeof mem - p, "%02X ", bytes[c]);
        p += g_snprintf(mem + p, sizeof mem - p, " ");
        for (int c = 0; c < MEM_COLS; c++) {
            uint8_t b = bytes[c];
            p += g_snprintf(mem + p, sizeof mem - p, "%c",
                            (b >= 0x20 && b < 0x7f) ? b : '.');
        }
        p += g_snprintf(mem + p, sizeof mem - p, "\n");
        if (p >= (int)sizeof mem - 96) break;
    }
    gtk_label_set_text(d->mem_label, mem);
}

/* ---- Video tab refresh ---- */

static GdkTexture *texture_from_scratch(DbgWindow *d, int w, int h)
{
    GBytes *bytes = g_bytes_new(d->scratch, (gsize)w * h * sizeof(uint32_t));
#if G_BYTE_ORDER == G_BIG_ENDIAN
    GdkMemoryFormat fmt = GDK_MEMORY_X8R8G8B8;
#else
    GdkMemoryFormat fmt = GDK_MEMORY_B8G8R8X8;
#endif
    GdkTexture *tex = gdk_memory_texture_new(w, h, fmt, bytes,
                                             (gsize)w * sizeof(uint32_t));
    g_bytes_unref(bytes);
    return tex;
}

static void set_picture(GtkWidget *pic, GdkTexture *tex)
{
    gtk_picture_set_paintable(GTK_PICTURE(pic), GDK_PAINTABLE(tex));
    g_object_unref(tex);
}

static void refresh_video(DbgWindow *d)
{
    astrovid_snapshot snap;
    astrovid_snapshot_get(&snap);
    int w, h;

    astrovid_render_screen(&snap, d->scratch, &w, &h);
    set_picture(d->screen_pic, texture_from_scratch(d, w, h));

    astrovid_render_bitmap(&snap, d->scratch, &w, &h);
    set_picture(d->bitmap_pic, texture_from_scratch(d, ASTROVID_BITMAP_W, h));

    int rows = astrovid_render_palette(&snap, d->scratch, ASTROVID_PAL_COLS,
                                       ASTROVID_PAL_CELL);
    set_picture(d->palette_pic,
                texture_from_scratch(d, ASTROVID_PAL_COLS * ASTROVID_PAL_CELL,
                                     rows * ASTROVID_PAL_CELL));

    char text[4096];
    astrovid_format_state(&snap, text, sizeof text);
    gtk_label_set_text(d->vid_text, text);
}

static gboolean poll_tick(gpointer user_data)
{
    DbgWindow *d = user_data;
    /* the Video tab is live even while running; the CPU tab needs a pause */
    refresh_video(d);
    uint64_t s = astrodebug_stop_serial(d->dbg);
    if (s != d->last_serial || !astrodebug_is_paused(d->dbg)) {
        d->last_serial = s;
        refresh_cpu(d);
        gtk_label_set_text(d->status_label,
                           astrodebug_is_paused(d->dbg) ? "Paused" : "Running");
    }
    return G_SOURCE_CONTINUE;
}

/* ---- controls ---- */

static void on_pause(GtkButton *b, gpointer u)
{
    DbgWindow *d = u; (void)b;
    if (astrodebug_is_paused(d->dbg)) astrodebug_resume(d->dbg);
    else astrodebug_pause(d->dbg);
}
static void on_step(GtkButton *b, gpointer u)      { (void)b; astrodebug_step(((DbgWindow*)u)->dbg); }
static void on_step_over(GtkButton *b, gpointer u) { (void)b; astrodebug_step_over(((DbgWindow*)u)->dbg); }
static void on_step_out(GtkButton *b, gpointer u)  { (void)b; astrodebug_step_out(((DbgWindow*)u)->dbg); }

static void on_mem_addr(GtkEntry *e, gpointer u)
{
    DbgWindow *d = u;
    const char *t = gtk_editable_get_text(GTK_EDITABLE(e));
    d->mem_addr = (uint16_t)strtol(t, NULL, 16);
    refresh_cpu(d);
}

static void on_bp_addr(GtkEntry *e, gpointer u)
{
    DbgWindow *d = u;
    const char *t = gtk_editable_get_text(GTK_EDITABLE(e));
    if (*t) {
        astrodebug_breakpoint_toggle(d->dbg, (uint16_t)strtol(t, NULL, 16));
        gtk_editable_set_text(GTK_EDITABLE(e), "");
        refresh_cpu(d);
    }
}

/* ---- construction ---- */

static GtkWidget *labeled_mono(GtkLabel **out)
{
    GtkWidget *lbl = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_label_set_yalign(GTK_LABEL(lbl), 0.0f);
    gtk_widget_add_css_class(lbl, "monospace");
    *out = GTK_LABEL(lbl);
    return lbl;
}

static GtkWidget *build_cpu_tab(DbgWindow *d)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(box, 8); gtk_widget_set_margin_bottom(box, 8);
    gtk_widget_set_margin_start(box, 8); gtk_widget_set_margin_end(box, 8);

    GtkWidget *ctl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    struct { const char *l; GCallback cb; } btns[] = {
        {"Pause/Resume", G_CALLBACK(on_pause)},
        {"Step", G_CALLBACK(on_step)},
        {"Step Over", G_CALLBACK(on_step_over)},
        {"Step Out", G_CALLBACK(on_step_out)},
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(btns); i++) {
        GtkWidget *b = gtk_button_new_with_label(btns[i].l);
        g_signal_connect(b, "clicked", btns[i].cb, d);
        gtk_box_append(GTK_BOX(ctl), b);
    }
    d->status_label = GTK_LABEL(gtk_label_new("Running"));
    gtk_box_append(GTK_BOX(ctl), GTK_WIDGET(d->status_label));
    gtk_box_append(GTK_BOX(box), ctl);

    gtk_box_append(GTK_BOX(box), labeled_mono(&d->regs_label));

    GtkWidget *cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_append(GTK_BOX(cols), labeled_mono(&d->disasm_label));
    gtk_box_append(GTK_BOX(cols), labeled_mono(&d->mem_label));
    gtk_box_append(GTK_BOX(box), cols);

    /* address entries */
    GtkWidget *entries = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *mem_e = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(mem_e), "mem addr (hex)");
    g_signal_connect(mem_e, "activate", G_CALLBACK(on_mem_addr), d);
    GtkWidget *bp_e = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(bp_e), "toggle breakpoint (hex)");
    g_signal_connect(bp_e, "activate", G_CALLBACK(on_bp_addr), d);
    gtk_box_append(GTK_BOX(entries), mem_e);
    gtk_box_append(GTK_BOX(entries), bp_e);
    gtk_box_append(GTK_BOX(box), entries);

    return box;
}

static GtkWidget *framed(const char *title, GtkWidget *child)
{
    GtkWidget *frame = gtk_frame_new(title);
    gtk_frame_set_child(GTK_FRAME(frame), child);
    return frame;
}

static GtkWidget *build_video_tab(DbgWindow *d)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(box, 8); gtk_widget_set_margin_bottom(box, 8);
    gtk_widget_set_margin_start(box, 8); gtk_widget_set_margin_end(box, 8);

    GtkWidget *pics = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    d->screen_pic = gtk_picture_new();
    gtk_widget_set_size_request(d->screen_pic, 352, 240);
    d->bitmap_pic = gtk_picture_new();
    gtk_widget_set_size_request(d->bitmap_pic, 320, 204);
    gtk_box_append(GTK_BOX(pics), framed("Screen", d->screen_pic));
    gtk_box_append(GTK_BOX(pics), framed("Screen RAM", d->bitmap_pic));
    gtk_box_append(GTK_BOX(box), pics);

    d->palette_pic = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(d->palette_pic), GTK_CONTENT_FIT_FILL);
    gtk_widget_set_size_request(d->palette_pic, ASTROVID_PAL_COLS * ASTROVID_PAL_CELL,
                                16 * ASTROVID_PAL_CELL);
    gtk_box_append(GTK_BOX(box), framed("Palette (512 pens)", d->palette_pic));

    gtk_box_append(GTK_BOX(box), framed("Registers", labeled_mono(&d->vid_text)));

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    return scroll;
}

static void install_css(void)
{
    static gboolean done;
    if (done) return;
    done = TRUE;
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        ".monospace { font-family: monospace; }");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

static void on_close(GtkWindow *w, gpointer u)
{
    DbgWindow *d = u;
    (void)w;
    if (d->timer_id) { g_source_remove(d->timer_id); d->timer_id = 0; }
    astrodebug_set_engaged(d->dbg, 0);   /* release: resumes if paused */
    g_free(d);
    g_dbg = NULL;
}

void astro_debugger_show(GtkWindow *parent, astrosession *session)
{
    if (g_dbg) {
        gtk_window_present(g_dbg->win);
        return;
    }
    install_css();
    DbgWindow *d = g_new0(DbgWindow, 1);
    g_dbg = d;
    d->session = session;
    d->dbg = astrodebug_get();
    astrodebug_set_engaged(d->dbg, 1);

    d->win = GTK_WINDOW(gtk_window_new());
    gtk_window_set_title(d->win, "Debugger");
    gtk_window_set_default_size(d->win, 900, 640);
    gtk_window_set_transient_for(d->win, parent);

    GtkWidget *nb = gtk_notebook_new();
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_cpu_tab(d),
                             gtk_label_new("Z80"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_video_tab(d),
                             gtk_label_new("Video chip"));
    gtk_window_set_child(d->win, nb);

    g_signal_connect(d->win, "close-request", G_CALLBACK(on_close), d);
    d->timer_id = g_timeout_add(100, poll_tick, d);
    gtk_window_present(d->win);
}
