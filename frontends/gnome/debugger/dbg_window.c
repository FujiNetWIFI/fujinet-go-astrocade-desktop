/*
 * The debugger window: three tabs over the singleton astrodebug engine.
 *
 *  CPU    -- a control row (Pause/Resume, Step, Step Over, Step Out), the Z80
 *            register file, a disassembly listing from the current PC, and a
 *            memory hex view.
 *  Video  -- the live screen, the screen-RAM bitmap, the 512-pen palette
 *            strip, and the full decoded data-chip + sound register text
 *            (every register the Nutting manual documents).
 *  FujiNet -- every FujiBus packet between the console and FujiNet, both
 *            ways, with hex (always traced, from session start), beside the
 *            cartridge's link and mailbox state.
 *
 * A GtkTimer polls astrodebug_stop_serial and refreshes when it changes,
 * rather than being called back on the emulator thread; the FujiNet tab also
 * refreshes twice a second while the machine runs. One RGBA scratch
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
#define TRACE_MAX 512

enum { PAGE_CPU, PAGE_VIDEO, PAGE_FUJINET };

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

    GtkNotebook *notebook;
    GtkLabel *fuji_note;
    GtkTextView *fuji_text;
    GtkLabel *fuji_state;
    int fuji_count;
    uint64_t fuji_last;
    int fuji_ticks;

    uint16_t mem_addr;
    uint32_t scratch[ASTROVID_MAX_PIXELS];  /* MAX of every Video render */
} DbgWindow;

static DbgWindow *g_dbg;

/* ---- log views ---- */

/* Engine text for a text buffer: valid UTF-8, control bytes dropped. */
static char *clean_text(const char *text)
{
    char *v = g_utf8_make_valid(text, -1), *s, *o;
    for (s = o = v; *s; s++)
        if ((unsigned char)*s >= 0x20 || *s == '\n' || *s == '\t') *o++ = *s;
    *o = '\0';
    return v;
}

static GtkAdjustment *vadj(GtkTextView *view)
{
    GtkWidget *parent = gtk_widget_get_parent(GTK_WIDGET(view));
    return GTK_IS_SCROLLED_WINDOW(parent)
        ? gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(parent)) : NULL;
}

static gboolean at_end(GtkAdjustment *adj)
{
    return gtk_adjustment_get_value(adj) + gtk_adjustment_get_page_size(adj) >=
           gtk_adjustment_get_upper(adj) - 2.0;
}

/* A log follows its end while its reader is there. New text grows the view
 * only once it is laid out (or its tab first shows), so the end is pinned
 * whenever the view's height changes; whether to follow is decided as text
 * arrives. */
static gboolean following(GtkTextView *view)
{
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(view), "follow")) != 0;
}

static void on_log_resized(GtkAdjustment *adj, gpointer view)
{
    if (following(view))
        gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj) -
                                      gtk_adjustment_get_page_size(adj));
}

static void make_log(GtkTextView *view)
{
    g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
    g_signal_connect(vadj(view), "changed", G_CALLBACK(on_log_resized), view);
}

/* Before new text: follow if the reader is at the end (or never looked). */
static void log_note_reader(GtkTextView *view)
{
    GtkAdjustment *adj = vadj(view);
    g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(
        gtk_adjustment_get_page_size(adj) <= 0.0 || at_end(adj)));
}

static void log_to_end(GtkTextView *view)
{
    g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
    on_log_resized(vadj(view), view);
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
static void set_text_at(GtkTextView *view, const char *text, gboolean follow)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(view);
    GtkAdjustment *adj = vadj(view);
    char *clean = clean_text(text);
    GtkTextMark *mark;
    GtkTextIter it;
    int top = 0;

    if (!follow && adj && gtk_adjustment_get_value(adj) > 0.0) {
        gtk_text_view_get_line_at_y(view, &it, (int)gtk_adjustment_get_value(adj), NULL);
        top = gtk_text_iter_get_line(&it);
    }
    gtk_text_buffer_set_text(b, clean, -1);
    g_free(clean);
    if (follow) {
        log_to_end(view);
        return;
    }
    if (top <= 0) return;
    /* scroll_to_mark waits for the new text's layout */
    gtk_text_buffer_get_iter_at_line(b, &it, top);
    mark = gtk_text_buffer_create_mark(b, NULL, &it, TRUE);
    gtk_text_view_scroll_to_mark(view, mark, 0.0, TRUE, 0.0, 0.0);
    gtk_text_buffer_delete_mark(b, mark);
}

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

/* ---- FujiNet tab refresh ---- */

static void refresh_fujinet(DbgWindow *d, gboolean force)
{
    static astrodebug_fuji_packet p[TRACE_MAX];
    char state[2048];
    int n = astrodebug_fujinet_trace(d->dbg, p, TRACE_MAX);

    if (astrosession_fujinet_link(d->session))
        gtk_label_set_text(d->fuji_note, "FujiNet cartridge, connected.");
    else if (astrosession_fujinet_running(d->session))
        gtk_label_set_text(d->fuji_note, "FujiNet cartridge, not connected.");
    else
        gtk_label_set_text(d->fuji_note,
                           "FujiNet cartridge, not connected (the FujiNet runtime is not running).");
    astrodebug_fujinet_state(d->dbg, state, sizeof state);
    gtk_label_set_text(d->fuji_state, state);

    if (!force && n == d->fuji_count && (n == 0 || p[n - 1].serial == d->fuji_last))
        return;
    d->fuji_count = n;
    d->fuji_last = n > 0 ? p[n - 1].serial : 0;
    log_note_reader(d->fuji_text);
    GString *t = g_string_new(NULL);
    for (int i = 0; i < n; i++) {
        g_string_append_printf(t, "%s%s", i ? "\n" : "", p[i].text);
        if (p[i].repeat > 1)
            g_string_append_printf(t, " \xc3\x97%u", p[i].repeat);
        g_string_append(t, "\n     ");
        for (int k = 0; k < p[i].nbytes && k < (int)sizeof p[i].data; k++)
            g_string_append_printf(t, "%02X ", p[i].data[k]);
        if (p[i].nbytes > (int)sizeof p[i].data) g_string_append(t, "\xe2\x80\xa6");
    }
    if (n == 0)
        g_string_append(t, "(no FujiBus packets since the trace was cleared)");
    set_text_at(d->fuji_text, t->str, force || following(d->fuji_text));
    g_string_free(t, TRUE);
}

static gboolean poll_tick(gpointer user_data)
{
    DbgWindow *d = user_data;
    /* the Video tab is live even while running; the CPU tab needs a pause */
    refresh_video(d);
    uint64_t s = astrodebug_stop_serial(d->dbg);
    /* FujiNet: on a stop, and twice a second while running */
    if (gtk_notebook_get_current_page(d->notebook) == PAGE_FUJINET &&
        (s != d->last_serial || (!astrodebug_is_paused(d->dbg) && ++d->fuji_ticks >= 5))) {
        d->fuji_ticks = 0;
        refresh_fujinet(d, FALSE);
    }
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

static void on_fuji_clear(GtkButton *b, gpointer u)
{
    DbgWindow *d = u;
    (void)b;
    astrodebug_fujinet_trace_clear(d->dbg);
    refresh_fujinet(d, TRUE);
}

static void on_switch_page(GtkNotebook *nb, GtkWidget *page, guint num, gpointer u)
{
    (void)nb; (void)page;
    if (num == PAGE_FUJINET)
        refresh_fujinet(u, FALSE);
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

static GtkWidget *heading(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "heading");
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    return l;
}

static GtkWidget *build_fujinet_tab(DbgWindow *d)
{
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *clear = gtk_button_new_with_label("Clear");
    gtk_widget_set_margin_top(paned, 8); gtk_widget_set_margin_bottom(paned, 8);
    gtk_widget_set_margin_start(paned, 8); gtk_widget_set_margin_end(paned, 8);

    d->fuji_note = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(d->fuji_note), "dim-label");
    gtk_label_set_wrap(d->fuji_note, TRUE);
    gtk_label_set_xalign(d->fuji_note, 0.0f);
    gtk_widget_set_hexpand(GTK_WIDGET(d->fuji_note), TRUE);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_fuji_clear), d);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(d->fuji_note));
    gtk_box_append(GTK_BOX(row), clear);
    gtk_box_append(GTK_BOX(left), heading("FujiBus packets (SLIP-decoded)"));
    gtk_box_append(GTK_BOX(left), row);

    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(left), scroll);
    d->fuji_text = GTK_TEXT_VIEW(view);
    make_log(d->fuji_text);

    gtk_widget_set_margin_start(right, 10);
    gtk_box_append(GTK_BOX(right), heading("FujiNet cartridge"));
    gtk_box_append(GTK_BOX(right), labeled_mono(&d->fuji_state));

    gtk_paned_set_start_child(GTK_PANED(paned), left);
    gtk_paned_set_end_child(GTK_PANED(paned), right);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), FALSE);
    gtk_widget_set_size_request(left, 520, -1);
    gtk_widget_set_size_request(right, 300, -1);
    d->fuji_count = -1;
    return paned;
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
    d->notebook = GTK_NOTEBOOK(nb);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_cpu_tab(d),
                             gtk_label_new("Z80"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_video_tab(d),
                             gtk_label_new("Video chip"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_fujinet_tab(d),
                             gtk_label_new("FujiNet"));
    g_signal_connect(nb, "switch-page", G_CALLBACK(on_switch_page), d);
    /* ASTRO_DEBUGGER_TAB: a page number to open on (0 Z80, 1 Video chip,
     * 2 FujiNet) */
    const char *tab = g_getenv("ASTRO_DEBUGGER_TAB");
    if (tab && *tab)
        gtk_notebook_set_current_page(GTK_NOTEBOOK(nb), atoi(tab));
    gtk_window_set_child(d->win, nb);

    g_signal_connect(d->win, "close-request", G_CALLBACK(on_close), d);
    d->timer_id = g_timeout_add(100, poll_tick, d);
    gtk_window_present(d->win);
}
