/*
 * Debugger window (GTK4/libadwaita) over the session's Z80 debugger engine,
 * via core/include/astrodebug.h (the same contract as the ADAM, SMS
 * and MSX siblings, and this window follows theirs).
 *
 * Tabs (ASTRO_DEBUGGER_TAB takes the number, or the start of the name):
 *   0 Console          the contract's command set, Tab completion, history
 *   1 Disassembly      breakpoint gutter, follow-PC, jump box, selectable row
 *   2 Memory           a dump with go-to (a range is highlighted), search
 *                      (hex or text, next and previous) and poke
 *   3 Breakpoints      why it stopped; execute, read/write watchpoints, port
 *                      in/out, conditions
 *   4 Watches          addresses, labels and ranges with their live values
 *   5 Cart & I/O       the cartridge, the memory map, the ports and the chip
 *   6 Trace            the instruction ring, live while the machine runs
 *   7 History          the same ring, read when the machine stops
 *   8 Video chip       the screen, the screen-RAM bitmap, the 512-pen
 *                      palette and the decoded data-chip registers
 *   9 FujiNet          every FujiBus packet, with the BoIP link, with hex
 * The registers (editable) and flags sit under the tabs on every tab. Any
 * tab can be popped out into a window of its own (Pop Out, then Dock; closing
 * that window docks it); one tab always stays in the main window.
 * A toolbar carries the stepping controls, symbol loading and file saves;
 * the family's keys apply (F5 run/stop, F7 step, F8 step over, Shift+F8 step
 * out, Shift+F7 frame, F9 breakpoint on the selected line, F12 close).
 * ASTRO_OPEN_DEBUGGER=1 opens the window at startup (window.c).
 *
 * The engine is engaged only while this window is showing: showing it
 * attaches (which stops the machine, as on every sibling), hiding it
 * detaches and lets the machine run on at full speed. F12 in the main
 * window toggles it. The window itself is created once and hidden, not
 * destroyed.
 *
 * The window polls the engine's generation counter on a short timer and
 * refreshes only the tab showing, and only when something changed, so a
 * stopped machine costs nothing and a running one shows live values twice a
 * second. It does not use the engine's stop callback: that fires off the UI
 * thread, and the generation counter says the same thing.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <stdlib.h>
#include <string.h>

#include "astrodebug.h"
#include "../window.h"
#include "../help/help_window.h"
#include "astrovid.h"
#include "astromemtools.h"

#define DISASM_WINDOW 48
#define NREGS 17
#define MEM_ROWS 32
#define MEM_PAGE (MEM_ROWS * 16)
#define POKE_MAX 64
#define TRACE_MAX 512
#define HIST_SHOWN 2000   /* lines on a Trace or History page */
#define HIST_READ 65536   /* the ring */
#define NTABS 10

typedef struct {
    GtkWindow *win;
    astrosession *session;
    astrodebug *dbg;
    unsigned seen_generation;
    gboolean was_stopped;
    gboolean closing;         /* the window is being destroyed: touch nothing */
    int running_ticks;
    guint timer;

    AdwWindowTitle *title;    /* "Debugger", with the stop reason under it */
    GtkButton *run_btn;
    AdwToastOverlay *toasts;
    GtkNotebook *notebook;

    /* console */
    GtkTextView *console_out;
    GtkEntry *console_in;
    GPtrArray *history;
    guint history_pos;

    /* the registers strip */
    GtkEntry *reg[NREGS];
    GtkCheckButton *flag[8];  /* S Z Y H X P/V N C */
    GtkLabel *beam;
    gboolean updating_flags;
    gboolean force_entries;   /* a refresh may overwrite a focused entry */

    /* memory */
    GtkTextView *mem_view;
    GtkEntry *mem_goto, *mem_write;
    int mem_base;
    gboolean mem_hl;          /* a range is highlighted (Go to, a search hit, a watch) */
    int mem_hl_lo, mem_hl_hi;
    GtkDropDown *srch_kind;
    GtkEntry *srch_entry;
    GtkLabel *srch_note;

    /* tabs: the page widget of each, and the window it is popped out into */
    GtkWidget *pages[NTABS];
    GtkWindow *detached[NTABS];
    uint64_t seen_serial;     /* astrodebug_stop_serial last acted on */

    /* disassembly */
    GtkTextView *disasm;
    GtkCheckButton *follow_pc;
    GtkEntry *jump;
    int disasm_top;           /* address of the first instruction */
    int line_addr[DISASM_WINDOW * 2];   /* per buffer line; -1 for a label line */
    int line_count;
    int selected;             /* the selected row's address, -1 none */

    /* video chip */
    GtkPicture *pic_screen, *pic_bitmap, *pic_pal;
    GtkTextView *vid_state;
    astrovid_snapshot last_snap;   /* what the tab last drew */
    gboolean have_snap;
    uint32_t scratch[ASTROVID_MAX_PIXELS];   /* the MAX of every render */

    /* breakpoints */
    GtkListBox *bp_list;
    GtkDropDown *bp_type;
    GtkEntry *bp_start, *bp_end, *bp_cond;
    GtkLabel *bp_msg;
    GtkLabel *bp_stop;        /* "Stopped: ..." / "Running." */
    GString *bp_sig;          /* what the list shows, to skip a needless rebuild */

    /* watches */
    GtkEntry *wt_entry;
    GtkListBox *wt_list;
    GPtrArray *wt_labels;     /* a GtkLabel per row, rewritten in place */
    unsigned wt_gen;
    gboolean wt_built;

    /* cart & i/o */
    GtkTextView *cart_text;

    /* trace (0) and history (1): the same ring, two views */
    GtkToggleButton *h_rec[2];
    GtkDropDown *h_filter[2];
    GtkEntry *h_range[2];
    GtkLabel *h_note[2];
    GtkTextView *h_text[2];
    uint64_t h_sig[2];        /* what each page last drew; ~0 forces a redraw */
    gboolean h_sync;          /* the Record toggles are being set from the engine */
    int trace_ticks;

    /* fujinet */
    GtkLabel *fuji_note;
    GtkTextView *fuji_text, *fuji_state;
    uint64_t fuji_last;
    int fuji_count;
} DbgWin;

static DbgWin *g_win;

/* The register entries, each with how many hex digits it shows. */
static const struct { const char *name; int reg; int digits; } k_regs[NREGS] = {
    { "PC", ASTRO_REG_PC, 4 }, { "SP", ASTRO_REG_SP, 4 }, { "AF", ASTRO_REG_AF, 4 },
    { "BC", ASTRO_REG_BC, 4 }, { "DE", ASTRO_REG_DE, 4 }, { "HL", ASTRO_REG_HL, 4 },
    { "IX", ASTRO_REG_IX, 4 }, { "IY", ASTRO_REG_IY, 4 },
    { "AF'", ASTRO_REG_AF2, 4 }, { "BC'", ASTRO_REG_BC2, 4 }, { "DE'", ASTRO_REG_DE2, 4 },
    { "HL'", ASTRO_REG_HL2, 4 }, { "I", ASTRO_REG_I, 2 }, { "R", ASTRO_REG_R, 2 },
    { "IM", ASTRO_REG_IM, 1 }, { "IFF1", ASTRO_REG_IFF1, 1 }, { "IFF2", ASTRO_REG_IFF2, 1 },
};
#define REGS_FIRST_ROW 8

/* The tabs, in order; ASTRO_DEBUGGER_TAB takes a number or the start of a name. */
static const char *const k_tab_names[NTABS] = {
    "Console", "Disassembly", "Memory", "Breakpoints", "Watches", "Cart & I/O", "Trace",
    "History", "Video chip", "FujiNet",
};
enum { TAB_CONSOLE, TAB_DISASM, TAB_MEMORY, TAB_BREAKS, TAB_WATCHES, TAB_CART, TAB_TRACE,
       TAB_HISTORY, TAB_VIDEO, TAB_FUJINET };

/* ---- helpers -------------------------------------------------------------- */

/* Engine text for a text buffer: valid UTF-8, control bytes dropped. */
static char *clean_text(const char *text)
{
    char *v = g_utf8_make_valid(text, -1), *s, *d;
    for (s = d = v; *s; s++)
        if ((unsigned char)*s >= 0x20 || *s == '\n' || *s == '\t') *d++ = *s;
    *d = '\0';
    return v;
}

static GtkAdjustment *vadj(GtkTextView *view)
{
    GtkWidget *parent = gtk_widget_get_parent(GTK_WIDGET(view));
    return GTK_IS_SCROLLED_WINDOW(parent)
        ? gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(parent)) : NULL;
}

/* A log (the console, the FujiNet packets) follows its end while its
 * reader is there. New text grows the view only once it is laid out (or its
 * tab first shows), so the end is pinned whenever the view's height
 * changes; whether to follow is decided as text arrives. */
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
    /* tied to the view: the adjustment can outlive it while the window dies */
    g_signal_connect_object(vadj(view), "changed", G_CALLBACK(on_log_resized), view, 0);
}

/* Before new text: follow if the reader is at the end (or never looked). */
static void log_note_reader(GtkTextView *view)
{
    GtkAdjustment *adj = vadj(view);
    g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(
        gtk_adjustment_get_page_size(adj) <= 0.0 ||
        gtk_adjustment_get_value(adj) + gtk_adjustment_get_page_size(adj) >=
            gtk_adjustment_get_upper(adj) - 2.0));
}

/* Replaces a view's text where the reader left it: a refresh (a step, or
 * the live values twice a second) must not throw a scrolled dump back to
 * its top. The top line is noted and scrolled back to once the new text is
 * laid out (scroll_to_mark waits for that). A log that is following goes
 * to its end instead. */
static void set_text_at(GtkTextView *view, const char *text, gboolean follow)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(view);
    GtkAdjustment *adj = vadj(view);
    char *clean = clean_text(text);
    GtkTextIter it;
    int top = 0;

    if (!follow && adj && gtk_adjustment_get_value(adj) > 0.0) {
        gtk_text_view_get_line_at_y(view, &it, (int)gtk_adjustment_get_value(adj), NULL);
        top = gtk_text_iter_get_line(&it);
    }
    {
        GtkTextIter s, e;
        char *old;
        gboolean same;

        /* unchanged text is not set again: it would reset the selection and
         * relayout for nothing */
        gtk_text_buffer_get_bounds(b, &s, &e);
        old = gtk_text_buffer_get_text(b, &s, &e, FALSE);
        same = strcmp(old, clean) == 0;
        g_free(old);
        if (same) {
            g_free(clean);
            if (follow) {
                g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
                if (adj) on_log_resized(adj, view);
            }
            return;
        }
    }
    gtk_text_buffer_set_text(b, clean, -1);
    g_free(clean);
    if (follow) {
        g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
        if (adj) on_log_resized(adj, view);
        return;
    }
    if (top > 0) {
        GtkTextMark *mark;
        gtk_text_buffer_get_iter_at_line(b, &it, top);
        mark = gtk_text_buffer_create_mark(b, NULL, &it, TRUE);
        gtk_text_view_scroll_to_mark(view, mark, 0.0, TRUE, 0.0, 0.0);
        gtk_text_buffer_delete_mark(b, mark);
    }
}

static void set_text(GtkTextView *view, const char *text)
{
    set_text_at(view, text, FALSE);
}

static void append_text(GtkTextView *view, const char *text, gboolean force_follow)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(view);
    GtkTextIter end;
    char *clean = clean_text(text);
    log_note_reader(view);
    if (force_follow)
        g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
    gtk_text_buffer_get_end_iter(b, &end);
    gtk_text_buffer_insert(b, &end, clean, -1);
    g_free(clean);
}

/* A number as typed: hex by default, $ or 0x or a trailing h for hex, #
 * for decimal. */
static int parse_num(const char *text, long *out)
{
    char buf[64], *end;
    size_t n;
    long v;
    while (*text == ' ') text++;
    g_strlcpy(buf, text, sizeof buf);
    n = strlen(buf);
    while (n && buf[n - 1] == ' ') buf[--n] = '\0';
    if (!n) return 0;
    if (buf[0] == '$') v = strtol(buf + 1, &end, 16);
    else if (buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X')) v = strtol(buf + 2, &end, 16);
    else if (buf[0] == '#') v = strtol(buf + 1, &end, 10);
    else if (buf[n - 1] == 'h' || buf[n - 1] == 'H') { buf[n - 1] = '\0'; v = strtol(buf, &end, 16); }
    else v = strtol(buf, &end, 16);
    if (end == buf || *end) return 0;
    *out = v;
    return 1;
}

/* An address box: a label (as the engine does, a name wins over a hex
 * literal), bare hex, or any expression the console takes ($C000+4, pc,
 * hl, [sp], label+2). 0 with the reason in err. */
static int parse_addr(DbgWin *w, const char *text, long *out, char *err, int errsz)
{
    char buf[128];
    long v;
    int a;
    g_strlcpy(buf, text, sizeof buf);
    g_strstrip(buf);
    if (!buf[0]) {
        g_strlcpy(err, "Enter an address, a label or an expression", errsz);
        return 0;
    }
    a = astrodebug_label_address(w->dbg, buf);
    if (a >= 0) { *out = a & 0xFFFF; return 1; }
    if (parse_num(buf, &v)) {
        if (v < 0 || v > 0xFFFF) {
            g_strlcpy(err, "Out of range (0 to $FFFF)", errsz);
            return 0;
        }
        *out = v;
        return 1;
    }
    if (astrodebug_eval(w->dbg, buf, &v, err, errsz) != 0) {
        if (!err[0]) g_strlcpy(err, "Not an address or a label", errsz);
        return 0;
    }
    *out = v & 0xFFFF;
    return 1;
}

/* A refresh does not overwrite a value being typed: a focused entry whose
 * text is not what was last put there keeps it, until Enter (force_entries)
 * says to take the engine's. */
static void set_entry(DbgWin *w, GtkEntry *e, const char *text)
{
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(e));
    GtkWidget *focus = root ? gtk_root_get_focus(root) : NULL;
    const char *shown = g_object_get_data(G_OBJECT(e), "shown");
    if (focus && !w->force_entries && shown &&
        (focus == GTK_WIDGET(e) || gtk_widget_is_ancestor(focus, GTK_WIDGET(e))) &&
        strcmp(gtk_editable_get_text(GTK_EDITABLE(e)), shown) != 0)
        return;
    gtk_editable_set_text(GTK_EDITABLE(e), text);
    g_object_set_data_full(G_OBJECT(e), "shown", g_strdup(text), g_free);
}

static GtkWidget *mono_view(GtkTextView **out, gboolean wrap)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), wrap ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);
    *out = GTK_TEXT_VIEW(view);
    return scroll;
}

static GtkWidget *labeled(const char *text, GtkWidget *child)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "dim-label");
    gtk_box_append(GTK_BOX(box), l);
    gtk_box_append(GTK_BOX(box), child);
    return box;
}

/* A heading over a block (the VDP and FujiNet panes). */
static GtkWidget *headed(const char *text, GtkWidget *child)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "heading");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_append(GTK_BOX(box), l);
    gtk_box_append(GTK_BOX(box), child);
    return box;
}

static GtkWidget *padded(GtkWidget *w)
{
    gtk_widget_set_margin_top(w, 8);
    gtk_widget_set_margin_bottom(w, 8);
    gtk_widget_set_margin_start(w, 8);
    gtk_widget_set_margin_end(w, 8);
    return w;
}

/* The Astrocade accent is dark red: white text on it, in light and dark. */
static void accent_tag(GtkTextView *view, const char *name)
{
    gtk_text_buffer_create_tag(gtk_text_view_get_buffer(view), name,
                               "paragraph-background", "#b5342c", "foreground", "#ffffff", NULL);
}

/* Messages from Load Symbols and Save land in the console's transcript and,
 * since the console may not be the tab showing, in a toast. */
static void notice(DbgWin *w, const char *msg)
{
    char line[700];
    g_snprintf(line, sizeof line, "%s\n", msg);
    append_text(w->console_out, line, FALSE);
    adw_toast_overlay_add_toast(w->toasts, adw_toast_new(msg));
}

static void toast(DbgWin *w, const char *msg)
{
    adw_toast_overlay_add_toast(w->toasts, adw_toast_new(msg));
}

/* The step controls wait a moment for the engine to come to rest, so a
 * step does not flash "Running" before the status catches up. */
static void settle(DbgWin *w, gboolean want_stopped)
{
    int i;
    for (i = 0; i < 20 && (astrodebug_is_stopped(w->dbg) != 0) != want_stopped; i++)
        g_usleep(2000);
}

/* ---- refresh -------------------------------------------------------------- */

static void refresh_cpu(DbgWin *w)
{
    astrodebug_cpu c;
    char buf[160];
    int i;
    astrodebug_cpu_get(w->dbg, &c);
    {
        const int vals[NREGS] = { c.pc, c.sp, c.af, c.bc, c.de, c.hl, c.ix, c.iy,
                                  c.af2, c.bc2, c.de2, c.hl2, c.i, c.r, c.im,
                                  c.iff1, c.iff2 };
        for (i = 0; i < NREGS; i++) {
            g_snprintf(buf, sizeof buf, "%0*X", k_regs[i].digits, (unsigned)vals[i]);
            set_entry(w, w->reg[i], buf);
        }
    }
    {
        const int flags[8] = { c.sf, c.zf, c.yf, c.hf, c.xf, c.pf, c.nf, c.cf };
        w->updating_flags = TRUE;
        for (i = 0; i < 8; i++)
            gtk_check_button_set_active(w->flag[i], flags[i] != 0);
        w->updating_flags = FALSE;
    }
    g_snprintf(buf, sizeof buf, "line %d   frame %u   cycle %llu%s", c.line, c.frame,
               (unsigned long long)c.cycles, c.halted ? "   HALT" : "");
    gtk_label_set_text(w->beam, buf);
}

/* The dump: a row is "AAAA  " then 3 columns per byte, then the text. The
 * highlighted range gets the accent background on its bytes (hex and text),
 * with the gap between two highlighted bytes filled so it reads as a bar. */
static void refresh_mem(DbgWin *w)
{
    uint8_t data[MEM_PAGE];
    GString *str = g_string_new(NULL);
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->mem_view);
    int row, col;

    memset(data, 0, sizeof data);
    astrodebug_read(w->dbg, (uint16_t)w->mem_base, data, MEM_PAGE);
    g_string_append(str, "      0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n");
    for (row = 0; row < MEM_ROWS; row++) {
        g_string_append_printf(str, "%04X  ", w->mem_base + row * 16);
        for (col = 0; col < 16; col++)
            g_string_append_printf(str, "%02X ", data[row * 16 + col]);
        g_string_append_c(str, ' ');
        for (col = 0; col < 16; col++) {
            uint8_t ch = data[row * 16 + col];
            g_string_append_c(str, (ch >= 0x20 && ch <= 0x7E) ? (char)ch : '.');
        }
        g_string_append_c(str, '\n');
    }
    set_text(w->mem_view, str->str);
    g_string_free(str, TRUE);

    {   /* unchanged text keeps its tags: clear the old highlight first */
        GtkTextIter s, e;
        gtk_text_buffer_get_bounds(b, &s, &e);
        gtk_text_buffer_remove_tag_by_name(b, "mark", &s, &e);
    }
    if (w->mem_hl) {
        int a;
        for (a = w->mem_base; a < w->mem_base + MEM_PAGE; a++) {
            GtkTextIter s, e;
            int line, off;
            gboolean next;
            if (a < w->mem_hl_lo || a > w->mem_hl_hi) continue;
            line = (a - w->mem_base) / 16 + 1;
            col = (a - w->mem_base) % 16;
            next = a < w->mem_hl_hi && col < 15;
            gtk_text_buffer_get_iter_at_line_offset(b, &s, line, 6 + col * 3);
            gtk_text_buffer_get_iter_at_line_offset(b, &e, line, 6 + col * 3 + (next ? 3 : 2));
            gtk_text_buffer_apply_tag_by_name(b, "mark", &s, &e);
            off = 6 + 48 + 1 + col;
            gtk_text_buffer_get_iter_at_line_offset(b, &s, line, off);
            gtk_text_buffer_get_iter_at_line_offset(b, &e, line, off + 1);
            gtk_text_buffer_apply_tag_by_name(b, "mark", &s, &e);
        }
    }
}

static void refresh_disasm(DbgWin *w)
{
    static astrodebug_line lines[DISASM_WINDOW];
    int dim_from[DISASM_WINDOW * 2];    /* per buffer line: grey from this column, -1 none */
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->disasm);
    GString *str = g_string_new(NULL);
    GtkTextIter s, e;
    int n, pc_line, i, line = 0, sel_line = -1, pc_buf = -1;
    gsize head;

    /* Following, the PC sits a third of the way down the window. */
    if (gtk_check_button_get_active(w->follow_pc)) {
        astrodebug_cpu c;
        astrodebug_cpu_get(w->dbg, &c);
        w->disasm_top = astrodebug_row_address(w->dbg, (uint16_t)c.pc, -(DISASM_WINDOW / 3));
        if (w->disasm_top > c.pc) w->disasm_top = 0;   /* near $0000 it wraps: start at the bottom */
    }

    n = astrodebug_disassemble(w->dbg, (uint16_t)w->disasm_top, lines, DISASM_WINDOW, &pc_line);
    for (i = 0; i < n; i++) {
        /* a label gets a line of its own above its instruction */
        if (lines[i].label[0]) {
            g_string_append_printf(str, "      %s:\n", lines[i].label);
            dim_from[line] = 0;
            w->line_addr[line++] = -1;
        }
        head = str->len;
        g_string_append_printf(str, "%s%c %04X  %-11s %-16s",
                               lines[i].has_breakpoint ? "\xe2\x97\x8f" : " ",
                               lines[i].is_pc ? '>' : ' ',
                               lines[i].address, lines[i].bytes, lines[i].disasm);
        dim_from[line] = -1;
        if (lines[i].comment[0]) {
            dim_from[line] = (int)g_utf8_strlen(str->str + head, -1);
            g_string_append_printf(str, "; %s", lines[i].comment);
        }
        g_string_append_c(str, '\n');
        if (lines[i].address == w->selected) sel_line = line;
        if (i == pc_line) pc_buf = line;
        w->line_addr[line++] = lines[i].address;
    }
    w->line_count = line;
    if (n == 0) g_string_append(str, "(no disassembly)\n");
    gtk_text_buffer_set_text(b, str->str, -1);
    g_string_free(str, TRUE);

    /* labels and comments in grey */
    for (i = 0; i < line; i++) {
        if (dim_from[i] < 0) continue;
        gtk_text_buffer_get_iter_at_line(b, &s, i);
        if (dim_from[i] > 0) gtk_text_iter_set_line_offset(&s, dim_from[i]);
        e = s;
        gtk_text_iter_forward_to_line_end(&e);
        gtk_text_buffer_apply_tag_by_name(b, "dim", &s, &e);
    }
    if (sel_line >= 0) {
        gtk_text_buffer_get_iter_at_line(b, &s, sel_line);
        gtk_text_buffer_get_iter_at_line(b, &e, sel_line + 1);
        gtk_text_buffer_apply_tag_by_name(b, "sel", &s, &e);
    }
    /* the PC line in the accent colour (over the selection) */
    if (pc_buf >= 0) {
        gtk_text_buffer_get_iter_at_line(b, &s, pc_buf);
        gtk_text_buffer_get_iter_at_line(b, &e, pc_buf + 1);
        gtk_text_buffer_apply_tag_by_name(b, "pc", &s, &e);
    }
    /* the gutter marker: red while the breakpoint is enabled, grey off (it
     * keeps its colour on the PC line) */
    for (i = 0, line = 0; i < n; i++) {
        if (lines[i].label[0]) line++;
        if (lines[i].has_breakpoint) {
            gtk_text_buffer_get_iter_at_line(b, &s, line);
            e = s;
            gtk_text_iter_forward_char(&e);
            gtk_text_buffer_apply_tag_by_name(b, lines[i].breakpoint_enabled ? "bp_on" : "bp_off",
                                              &s, &e);
        }
        line++;
    }
}

static GdkTexture *texture_from_scratch(DbgWin *w, int tw, int th)
{
    GBytes *bytes = g_bytes_new(w->scratch, (gsize)tw * th * sizeof(uint32_t));
#if G_BYTE_ORDER == G_BIG_ENDIAN
    GdkMemoryFormat fmt = GDK_MEMORY_X8R8G8B8;
#else
    GdkMemoryFormat fmt = GDK_MEMORY_B8G8R8X8;
#endif
    GdkTexture *tex = gdk_memory_texture_new(tw, th, fmt, bytes, (gsize)tw * sizeof(uint32_t));
    g_bytes_unref(bytes);
    return tex;
}

static void set_picture(GtkPicture *pic, GdkTexture *tex)
{
    gtk_picture_set_paintable(pic, GDK_PAINTABLE(tex));
    g_object_unref(tex);
}

/* Redrawn only when the chip's state changed (the snapshot is zeroed before
 * it is filled, so a byte compare is exact). */
static void refresh_video(DbgWin *w)
{
    static astrovid_snapshot snap;
    static char text[4096];
    int tw, th, rows;

    astrovid_snapshot_get(&snap);
    if (w->have_snap && memcmp(&snap, &w->last_snap, sizeof snap) == 0)
        return;
    w->last_snap = snap;
    w->have_snap = TRUE;

    astrovid_render_screen(&snap, w->scratch, &tw, &th);
    set_picture(w->pic_screen, texture_from_scratch(w, tw, th));
    astrovid_render_bitmap(&snap, w->scratch, &tw, &th);
    set_picture(w->pic_bitmap, texture_from_scratch(w, ASTROVID_BITMAP_W, th));
    rows = astrovid_render_palette(&snap, w->scratch, ASTROVID_PAL_COLS, ASTROVID_PAL_CELL);
    set_picture(w->pic_pal, texture_from_scratch(w, ASTROVID_PAL_COLS * ASTROVID_PAL_CELL,
                                                 rows * ASTROVID_PAL_CELL));
    astrovid_format_state(&snap, text, (int)sizeof text);
    set_text(w->vid_state, text);
}

/* The cartridge, then the memory map, then the ports and the data chip. */
static void refresh_cart(DbgWin *w)
{
    static char io[8192];
    astrodebug_cart cart;
    astrodebug_region reg[32];
    GString *t = g_string_new(NULL);
    int n, i;

    astrodebug_cart_info(w->dbg, &cart);
    g_string_append(t, "Cartridge\n");
    g_string_append_printf(t, "  File     %s\n", cart.path[0] ? cart.path : "(none)");
    g_string_append_printf(t, "  Kind     %s\n", cart.kind[0] ? cart.kind : "none");
    g_string_append_printf(t, "  Image    %u bytes, %u 4K page%s\n", (unsigned)cart.image_bytes,
                           cart.pages, cart.pages == 1 ? "" : "s");
    if (cart.bank[0] >= 0 || cart.bank[1] >= 0)
        g_string_append_printf(t, "  Banks    low half page %d, high half page %d\n",
                               cart.bank[0], cart.bank[1]);
    else
        g_string_append(t, "  Banks    none (not banked)\n");
    g_string_append_printf(t, "  Mailbox  %s\n", cart.mailbox_live ? "live" : "dead");
    g_string_append_printf(t, "  Staged   %s\n", cart.staged ? "yes" : "no");
    g_string_append(t, "\nMemory map\n");
    n = astrodebug_memory_map(w->dbg, reg, 32);
    if (!n) g_string_append(t, "  (no map: no machine is loaded)\n");
    for (i = 0; i < n; i++)
        g_string_append_printf(t, "  %04X-%04X  %c%c  %s\n", reg[i].lo, reg[i].hi,
                               reg[i].readable ? 'R' : '-', reg[i].writable ? 'W' : '-',
                               reg[i].name);
    io[0] = '\0';
    astrodebug_io_state(w->dbg, io, sizeof io);
    g_string_append(t, "\nI/O\n");
    g_string_append(t, io);
    set_text(w->cart_text, t->str);
    g_string_free(t, TRUE);
}

/* The packet log is rebuilt only when it changed: a new packet, or a
 * repeat folded into the newest. */
static void refresh_fujinet(DbgWin *w, gboolean force)
{
    static astrodebug_fuji_packet p[TRACE_MAX];
    static char state[4096];
    char note[160];
    GString *t;
    int n = astrodebug_fujinet_trace(w->dbg, p, TRACE_MAX), i;

    if (astrosession_fujinet_external(w->session))
        g_snprintf(note, sizeof note, "External FujiNet at %s, %s.",
                   astrosession_boip_endpoint(w->session),
                   astrosession_fujinet_link(w->session) ? "connected" : "not connected");
    else if (astrosession_fujinet_link(w->session))
        g_strlcpy(note, "FujiNet cartridge, connected.", sizeof note);
    else if (astrosession_fujinet_running(w->session))
        g_strlcpy(note, "FujiNet cartridge, not connected.", sizeof note);
    else
        g_strlcpy(note, "FujiNet cartridge, not connected (the FujiNet runtime is not running).",
                  sizeof note);
    gtk_label_set_text(w->fuji_note, note);
    astrodebug_fujinet_state(w->dbg, state, (int)sizeof state);
    set_text(w->fuji_state, state);

    if (!force && n == w->fuji_count && (n == 0 || p[n - 1].serial == w->fuji_last))
        return;
    w->fuji_count = n;
    w->fuji_last = n > 0 ? p[n - 1].serial : 0;
    log_note_reader(w->fuji_text);
    t = g_string_new(NULL);
    for (i = 0; i < n; i++) {
        int k;
        g_string_append_printf(t, "%s%s", i ? "\n" : "", p[i].text);
        if (p[i].repeat > 1) g_string_append_printf(t, " \xc3\x97%u", p[i].repeat);
        g_string_append(t, "\n     ");
        for (k = 0; k < p[i].nbytes && k < (int)sizeof p[i].data; k++)
            g_string_append_printf(t, "%02X ", p[i].data[k]);
        if (p[i].nbytes > (int)sizeof p[i].data) g_string_append(t, "\xe2\x80\xa6");
    }
    if (n == 0)
        g_string_append(t, "(no FujiBus packets since the trace was cleared)");
    set_text_at(w->fuji_text, t->str, force || following(w->fuji_text));
    g_string_free(t, TRUE);
}

static void bp_enable_toggled(GtkCheckButton *b, gpointer ud);
static void bp_remove_clicked(GtkButton *b, gpointer ud);

static void refresh_bps(DbgWin *w)
{
    static astrodebug_breakpoint bps[128];
    GtkWidget *child;
    GString *sig = g_string_new(NULL);
    int n = astrodebug_breakpoint_list(w->dbg, bps, 128), i;

    {
        char reason[160], line[256];
        int addr;
        if (astrodebug_is_stopped(w->dbg)) {
            astrodebug_stop_reason(w->dbg, reason, sizeof reason, &addr);
            if (!reason[0] || strcmp(reason, "stopped") == 0)
                g_strlcpy(line, "Stopped", sizeof line);
            else
                g_snprintf(line, sizeof line, "Stopped: %s", reason);
            if (addr >= 0 && !strchr(reason, '$'))
                g_snprintf(line + strlen(line), sizeof line - strlen(line), " at $%04X", addr);
        } else
            g_strlcpy(line, "Running.", sizeof line);
        gtk_label_set_text(w->bp_stop, line);
    }

    /* the list is rebuilt only when what it shows changed (hit counts move
     * while the machine runs), so a click on a row is not pulled out from
     * under the pointer */
    for (i = 0; i < n; i++)
        g_string_append_printf(sig, "%d/%d/%u-%u/%d/%u/%s\n", bps[i].id, bps[i].type,
                               bps[i].start, bps[i].end, bps[i].enabled, bps[i].hits,
                               bps[i].condition);
    if (w->bp_sig && g_string_equal(sig, w->bp_sig)) {
        g_string_free(sig, TRUE);
        return;
    }
    if (w->bp_sig) g_string_free(w->bp_sig, TRUE);
    w->bp_sig = sig;

    while ((child = gtk_widget_get_first_child(GTK_WIDGET(w->bp_list))) != NULL)
        gtk_list_box_remove(w->bp_list, child);
    if (n == 0) {
        GtkWidget *l = gtk_label_new("No breakpoints. Select a disassembly line and press F9, "
                                     "or add one below.");
        gtk_widget_add_css_class(l, "dim-label");
        gtk_list_box_append(w->bp_list, l);
        return;
    }
    for (i = 0; i < n; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *en = gtk_check_button_new();
        GtkWidget *rm = gtk_button_new_from_icon_name("user-trash-symbolic");
        GtkWidget *l;
        char text[360], desc[256];
        astrobp_describe(w->dbg, &bps[i], desc, sizeof desc);
        g_snprintf(text, sizeof text, "#%-3d %s    hits %u", bps[i].id, desc, bps[i].hits);
        l = gtk_label_new(text);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_widget_set_hexpand(l, TRUE);
        gtk_widget_add_css_class(l, "monospace");
        gtk_check_button_set_active(GTK_CHECK_BUTTON(en), bps[i].enabled != 0);
        gtk_widget_set_tooltip_text(en, "Enabled");
        g_object_set_data(G_OBJECT(en), "bp", GINT_TO_POINTER(bps[i].id));
        g_object_set_data(G_OBJECT(rm), "bp", GINT_TO_POINTER(bps[i].id));
        g_signal_connect(en, "toggled", G_CALLBACK(bp_enable_toggled), w);
        g_signal_connect(rm, "clicked", G_CALLBACK(bp_remove_clicked), w);
        gtk_widget_add_css_class(rm, "flat");
        gtk_widget_set_tooltip_text(rm, "Remove");
        gtk_box_append(GTK_BOX(row), en);
        gtk_box_append(GTK_BOX(row), l);
        gtk_box_append(GTK_BOX(row), rm);
        gtk_list_box_append(w->bp_list, row);
    }
}

/* ---- tabs, and tabs popped out into windows of their own -------------------- */

static void refresh_now(DbgWin *w);
static void show_in_memory(DbgWin *w, int lo, int hi);

static int page_tab(GtkWidget *page)
{
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(page), "tab"));
}

/* The tab showing in the main window (-1 if the notebook is empty). */
static int current_tab(DbgWin *w)
{
    GtkWidget *page = gtk_notebook_get_nth_page(w->notebook,
                                                gtk_notebook_get_current_page(w->notebook));
    return page ? page_tab(page) : -1;
}

/* In front of the user: the current page, or a tab popped out. */
static gboolean tab_visible(DbgWin *w, int tab)
{
    return w->detached[tab] != NULL || current_tab(w) == tab;
}

static void show_tab(DbgWin *w, int tab)
{
    if (w->detached[tab])
        gtk_window_present(w->detached[tab]);
    else
        gtk_notebook_set_current_page(w->notebook,
                                      gtk_notebook_page_num(w->notebook, w->pages[tab]));
}

/* ---- watches ------------------------------------------------------------------ */

static void on_wt_remove(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    astrowatch_remove(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "wt-idx")));
    refresh_now(w);
}

static void on_wt_memory(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    uint16_t lo, hi;
    if (!astrowatch_range(w->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "wt-idx")),
                          &lo, &hi)) {
        toast(w, "That watch does not resolve to an address yet");
        return;
    }
    show_in_memory(w, lo, hi);
    show_tab(w, TAB_MEMORY);
}

/* Break when the watched bytes change: a write whose value differs from the
 * byte it replaced. */
static void break_on_change(DbgWin *w, uint16_t lo, uint16_t hi)
{
    char err[256] = "";
    if (astrodebug_breakpoint_add(w->dbg, ASTRODEBUG_BP_WRITE, lo, hi, "value != old", err,
                                  sizeof err) < 0) {
        char *clean = clean_text(err[0] ? err : "The breakpoint was refused");
        toast(w, clean);
        g_free(clean);
        return;
    }
    toast(w, "Breaking when it changes");
}

static void on_wt_break(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    uint16_t lo, hi;
    if (!astrowatch_range(w->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "wt-idx")),
                          &lo, &hi)) {
        toast(w, "That watch does not resolve to an address yet");
        return;
    }
    break_on_change(w, lo, hi);
    refresh_now(w);
}

static void wt_rebuild(DbgWin *w)
{
    const int n = astrowatch_count();
    int i;

    gtk_list_box_remove_all(w->wt_list);
    g_ptr_array_set_size(w->wt_labels, 0);
    if (n == 0) {
        GtkWidget *l = gtk_label_new("No watches. Type an address, a label, label+3, [ptr] "
                                     "or a range (C000-C0FF) above.");
        gtk_widget_add_css_class(l, "dim-label");
        gtk_list_box_append(w->wt_list, l);
    }
    for (i = 0; i < n; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *label = gtk_label_new(NULL);
        static const struct { const char *text, *tip; GCallback cb; } btns[] = {
            { "Memory", "Show it in the Memory tab", G_CALLBACK(on_wt_memory) },
            { "Break on change", "Stop when a write changes it", G_CALLBACK(on_wt_break) },
            { "Remove", "Forget this watch", G_CALLBACK(on_wt_remove) },
        };
        size_t k;
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_widget_set_hexpand(label, TRUE);
        gtk_widget_add_css_class(label, "monospace");
        gtk_box_append(GTK_BOX(row), label);
        for (k = 0; k < G_N_ELEMENTS(btns); k++) {
            GtkWidget *b = gtk_button_new_with_label(btns[k].text);
            gtk_widget_add_css_class(b, "flat");
            gtk_widget_set_tooltip_text(b, btns[k].tip);
            g_object_set_data(G_OBJECT(b), "wt-idx", GINT_TO_POINTER(i));
            g_signal_connect(b, "clicked", btns[k].cb, w);
            gtk_box_append(GTK_BOX(row), b);
        }
        gtk_list_box_append(w->wt_list, row);
        g_ptr_array_add(w->wt_labels, label);
    }
}

/* The rows are rebuilt only when the list changed; the values are written
 * into the labels in place, so a click on a row's button is never pulled
 * from under the pointer. */
static void refresh_watches(DbgWin *w)
{
    const unsigned gen = astrowatch_generation();
    int i;
    if (!w->wt_built || gen != w->wt_gen || astrowatch_count() != (int)w->wt_labels->len) {
        wt_rebuild(w);
        w->wt_gen = gen;
        w->wt_built = TRUE;
    }
    for (i = 0; i < (int)w->wt_labels->len; i++) {
        char line[200];
        astrowatch_format(w->dbg, i, line, sizeof line);
        gtk_label_set_text(g_ptr_array_index(w->wt_labels, i), line);
    }
}

static void on_wt_add(GtkWidget *wgt, gpointer ud)
{
    DbgWin *w = ud;
    (void)wgt;
    if (astrowatch_add(gtk_editable_get_text(GTK_EDITABLE(w->wt_entry))) < 0) {
        toast(w, "Type an address, a label, an expression or a range");
        return;
    }
    gtk_editable_set_text(GTK_EDITABLE(w->wt_entry), "");
    refresh_now(w);
}

/* ---- trace and history -------------------------------------------------------- */

/* Both tabs read the one ring. Trace (0) follows it while the machine runs;
 * History (1) is read once the machine has stopped. The newest HIST_SHOWN
 * matches are shown, oldest first. */
static void refresh_hist(DbgWin *w, int idx, gboolean force)
{
    static astrodebug_trace buf[HIST_READ];
    static int hit[HIST_SHOWN];
    const gboolean on = astrodebug_trace_enabled(w->dbg) != 0;
    const gboolean stopped = astrodebug_is_stopped(w->dbg) != 0;
    const int count = astrodebug_trace_count(w->dbg);
    uint16_t lo = 0, hi = 0xFFFF;
    int filter = ASTRODEBUG_TRACE_ALL, n, i, shown = 0, matched = 0;
    uint64_t sig;
    astrodebug_trace newest;
    char note[200];
    GString *t;

    w->h_sync = TRUE;       /* the Record toggles follow the engine */
    gtk_toggle_button_set_active(w->h_rec[0], on);
    gtk_toggle_button_set_active(w->h_rec[1], on);
    w->h_sync = FALSE;

    if (idx == 1 && !stopped) {
        gtk_label_set_text(w->h_note[1], "Stop the machine to read its history.");
        w->h_sig[1] = ~(uint64_t)0;
        return;
    }
    memset(&newest, 0, sizeof newest);
    astrodebug_trace_read(w->dbg, &newest, 1);
    sig = ((uint64_t)count << 40) ^ newest.cycles ^ (on ? 1 : 0);
    if (!force && sig == w->h_sig[idx]) return;

    switch (gtk_drop_down_get_selected(w->h_filter[idx])) {
    case 1: filter = ASTRODEBUG_TRACE_BRANCHES; break;
    case 2:
        if (!astrosym_parse_range(w->dbg, gtk_editable_get_text(GTK_EDITABLE(w->h_range[idx])),
                                  &lo, &hi)) {
            gtk_label_set_text(w->h_note[idx], "Type a range: C000-C0FF, a label, label..label");
            return;
        }
        filter = ASTRODEBUG_TRACE_RANGE;
        break;
    default: break;
    }
    w->h_sig[idx] = sig;
    n = astrodebug_trace_read(w->dbg, buf, filter == ASTRODEBUG_TRACE_ALL ? HIST_SHOWN : HIST_READ);
    for (i = 0; i < n; i++)             /* newest first: take the newest matches */
        if (astrodebug_trace_match(&buf[i], filter, lo, hi)) {
            matched++;
            if (shown < HIST_SHOWN) hit[shown++] = i;
        }
    t = g_string_new(NULL);
    if (shown == 0)
        g_string_append(t, on ? "(nothing recorded yet: let the machine run)"
                              : "Recording is off. Press Record, then let the machine run.");
    for (i = shown - 1; i >= 0; i--) {  /* oldest first */
        char line[256];
        astrodebug_trace_format(&buf[hit[i]], line, sizeof line);
        g_string_append(t, line);
        g_string_append_c(t, '\n');
    }
    set_text_at(w->h_text[idx], t->str, TRUE);
    g_string_free(t, TRUE);
    g_snprintf(note, sizeof note, "%s, %d instruction%s%s", on ? "recording" : "recording off",
               count, count == 1 ? "" : "s",
               matched > shown ? ", newest 2000 shown" : "");
    gtk_label_set_text(w->h_note[idx], note);
}

static void refresh_status(DbgWin *w)
{
    char reason[160], text[256];
    int addr;
    gboolean stopped = astrodebug_is_stopped(w->dbg) != 0;
    astrodebug_stop_reason(w->dbg, reason, sizeof reason, &addr);
    if (stopped) {
        const gboolean plain = !reason[0] || strcmp(reason, "stopped") == 0;
        g_snprintf(text, sizeof text, "Stopped%s%s", plain ? "" : ": ", plain ? "" : reason);
        if (addr >= 0 && !strchr(reason, '$'))
            g_snprintf(text + strlen(text), sizeof text - strlen(text), " at $%04X", addr);
    } else
        g_snprintf(text, sizeof text, "Running");
    adw_window_title_set_subtitle(w->title, text);
    gtk_button_set_label(w->run_btn, stopped ? "Run (F5)" : "Stop (F5)");
    if (stopped) gtk_widget_add_css_class(GTK_WIDGET(w->run_btn), "astro-accent");
    else gtk_widget_remove_css_class(GTK_WIDGET(w->run_btn), "astro-accent");
}

/* Only the tabs in front of the user are brought up to date (the one showing,
 * and any popped out); the others catch up when their tab is switched to. */
static void refresh_tab(DbgWin *w, int tab)
{
    if (w->closing) return;
    switch (tab) {
    case TAB_DISASM: refresh_disasm(w); break;
    case TAB_MEMORY: refresh_mem(w); break;
    case TAB_BREAKS: refresh_bps(w); break;
    case TAB_WATCHES: refresh_watches(w); break;
    case TAB_CART: refresh_cart(w); break;
    case TAB_TRACE: refresh_hist(w, 0, FALSE); break;
    case TAB_HISTORY: refresh_hist(w, 1, FALSE); break;
    case TAB_VIDEO: refresh_video(w); break;
    case TAB_FUJINET: refresh_fujinet(w, FALSE); break;
    default: break;
    }
}

/* The status, the registers and the tabs in front, and the generation they
 * now reflect. */
static void refresh_now(DbgWin *w)
{
    int t;
    if (w->closing) return;
    w->seen_generation = astrodebug_generation(w->dbg);
    w->was_stopped = astrodebug_is_stopped(w->dbg) != 0;
    refresh_status(w);
    refresh_cpu(w);
    for (t = 0; t < NTABS; t++)
        if (tab_visible(w, t)) refresh_tab(w, t);
}

static void attach_and_refresh(DbgWin *w);

static void install_css(void)
{
    static gboolean done;
    GtkCssProvider *css;
    if (done) return;
    done = TRUE;
    css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        "button.astro-accent { background-image: none; background-color: #b5342c;"
        " color: #fff; }"
        "button.astro-toggle:checked, togglebutton.astro-toggle:checked {"
        " background-image: none; background-color: #b5342c; color: #fff; }"
        "label.astro-accent-text { color: #b5342c; }");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

static gboolean tick_refresh(gpointer ud)
{
    DbgWin *w = ud;
    unsigned gen;
    gboolean stopped;
    if (!gtk_widget_get_visible(GTK_WIDGET(w->win))) return G_SOURCE_CONTINUE;
    if (!astrodebug_is_attached(w->dbg)) {
        attach_and_refresh(w);
        return G_SOURCE_CONTINUE;
    }
    gen = astrodebug_generation(w->dbg);
    stopped = astrodebug_is_stopped(w->dbg) != 0;
    {   /* a new stop brings the disassembly back to the PC, wherever it was scrolled */
        const uint64_t serial = astrodebug_stop_serial(w->dbg);
        if (serial != w->seen_serial) {
            GtkAdjustment *adj = vadj(w->disasm);
            w->seen_serial = serial;
            if (adj) gtk_adjustment_set_value(adj, 0);
            if (stopped && !gtk_check_button_get_active(w->follow_pc))
                gtk_check_button_set_active(w->follow_pc, TRUE);
            w->running_ticks = 0;
            refresh_now(w);
            return G_SOURCE_CONTINUE;
        }
    }
    if (!stopped && tab_visible(w, TAB_TRACE) && ++w->trace_ticks >= 4) {
        w->trace_ticks = 0;     /* the trace follows the ring while the machine runs */
        refresh_hist(w, 0, FALSE);
    }
    if (gen != w->seen_generation || stopped != w->was_stopped) {
        w->running_ticks = 0;
        refresh_now(w);
    } else if (!stopped && ++w->running_ticks >= 5) {
        /* live values twice a second while the machine runs */
        w->running_ticks = 0;
        refresh_now(w);
    }
    return G_SOURCE_CONTINUE;
}

/* The refresh timer runs only while the window is mapped. */
static void on_win_map(GtkWidget *widget, gpointer ud)
{
    DbgWin *w = ud;
    (void)widget;
    if (!w->timer && !w->closing)
        w->timer = g_timeout_add(100, tick_refresh, w);
}

static void on_win_unmap(GtkWidget *widget, gpointer ud)
{
    DbgWin *w = ud;
    (void)widget;
    if (w->timer) g_source_remove(w->timer);
    w->timer = 0;
}

/* ---- handlers ------------------------------------------------------------- */

static void on_run(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    if (astrodebug_is_stopped(w->dbg)) {
        astrodebug_resume(w->dbg);
    } else {
        astrodebug_stop(w->dbg);
        settle(w, TRUE);
    }
    refresh_now(w);
}

/* A step: the machine is stopped, runs a moment, stops again. */
#define STEP_HANDLER(name, call) \
    static void name(GtkButton *b, gpointer ud) \
    { \
        DbgWin *w = ud; \
        (void)b; \
        call; \
        settle(w, TRUE); \
        refresh_now(w); \
    }
STEP_HANDLER(on_step, astrodebug_step(w->dbg))
STEP_HANDLER(on_step_over, astrodebug_step_over(w->dbg))
STEP_HANDLER(on_step_out, astrodebug_step_out(w->dbg))
STEP_HANDLER(on_frame, astrodebug_frame(w->dbg, 1))
#undef STEP_HANDLER

static void on_run_to_cursor(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    if (w->selected < 0) {
        toast(w, "Select a disassembly line to run to");
        return;
    }
    astrodebug_run_to(w->dbg, (uint16_t)w->selected);
    settle(w, TRUE);
    refresh_now(w);
}

/* F9 and the gutter: an execute breakpoint on the selected line. */
static void toggle_selected(DbgWin *w)
{
    if (w->selected < 0) {
        toast(w, "Select a disassembly line first");
        return;
    }
    astrodebug_breakpoint_toggle(w->dbg, (uint16_t)w->selected);
    refresh_now(w);
}

static void on_console(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    static char out[65536];
    const char *cmd = gtk_editable_get_text(GTK_EDITABLE(entry));
    char line[512];
    gboolean was_stopped = astrodebug_is_stopped(w->dbg) != 0;
    if (!cmd || !*cmd) return;
    /* the history keeps each command once in a row */
    if (w->history->len == 0 ||
        strcmp(g_ptr_array_index(w->history, w->history->len - 1), cmd) != 0)
        g_ptr_array_add(w->history, g_strdup(cmd));
    w->history_pos = w->history->len;
    g_snprintf(line, sizeof line, "> %s\n", cmd);
    append_text(w->console_out, line, TRUE);
    out[0] = '\0';
    astrodebug_command(w->dbg, cmd, out, sizeof out);
    append_text(w->console_out, out, TRUE);
    if (out[0] && out[strlen(out) - 1] != '\n')
        append_text(w->console_out, "\n", TRUE);
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
    if (was_stopped) settle(w, TRUE);
    refresh_now(w);
}

static void history_show(DbgWin *w)
{
    const char *text = w->history_pos < w->history->len
                           ? g_ptr_array_index(w->history, w->history_pos) : "";
    gtk_editable_set_text(GTK_EDITABLE(w->console_in), text);
    gtk_editable_set_position(GTK_EDITABLE(w->console_in), -1);
}

/* Tab completes the current word against the commands, the registers and
 * the labels; several matches are listed instead. Up and Down walk the
 * command history. */
static gboolean on_console_key(GtkEventControllerKey *c, guint keyval, guint code,
                               GdkModifierType st, gpointer ud)
{
    DbgWin *w = ud;
    char comps[4096];
    const char *text;
    const char *word;
    int n;
    (void)c; (void)code; (void)st;
    if (keyval == GDK_KEY_Up || keyval == GDK_KEY_KP_Up) {
        if (w->history_pos > 0) {
            w->history_pos--;
            history_show(w);
        }
        return TRUE;
    }
    if (keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down) {
        if (w->history_pos < w->history->len) {
            w->history_pos++;
            history_show(w);
        }
        return TRUE;
    }
    if (keyval != GDK_KEY_Tab) return FALSE;
    text = gtk_editable_get_text(GTK_EDITABLE(w->console_in));
    comps[0] = '\0';
    /* the engine completes the end of the whole line (a command, then its
     * arguments), so the whole line goes and the first word is replaced */
    n = astrodebug_completions(w->dbg, text, comps, sizeof comps);
    word = strrchr(text, ' ');
    word = word ? word + 1 : text;
    if (n == 1) {
        char merged[600];
        char *nl = strchr(comps, '\n');
        if (nl) *nl = '\0';
        g_snprintf(merged, sizeof merged, "%.*s%s ", (int)(word - text), text, comps);
        gtk_editable_set_text(GTK_EDITABLE(w->console_in), merged);
        gtk_editable_set_position(GTK_EDITABLE(w->console_in), -1);
    } else if (n > 1) {
        /* the common start is filled in; the candidates are listed */
        char **cand = g_strsplit(comps, "\n", -1);
        size_t common = strlen(cand[0]), len = strlen(word);
        int i;
        for (i = 1; cand[i] && *cand[i]; i++) {
            size_t k = 0;
            while (k < common && cand[i][k] == cand[0][k]) k++;
            common = k;
        }
        if (common > len) {
            char merged[600];
            g_snprintf(merged, sizeof merged, "%.*s%.*s", (int)(word - text), text,
                       (int)common, cand[0]);
            gtk_editable_set_text(GTK_EDITABLE(w->console_in), merged);
            gtk_editable_set_position(GTK_EDITABLE(w->console_in), -1);
        }
        g_strfreev(cand);
        append_text(w->console_out, comps, FALSE);
        if (comps[0] && comps[strlen(comps) - 1] != '\n')
            append_text(w->console_out, "\n", FALSE);
    }
    return TRUE;
}

static void on_reg_activate(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(entry), "reg"));
    long v;
    if (!astrodebug_is_stopped(w->dbg))
        toast(w, "Stop the machine to edit the registers");
    else if (parse_num(gtk_editable_get_text(GTK_EDITABLE(entry)), &v))
        astrodebug_cpu_set(w->dbg, k_regs[i].reg, (int)v);
    /* Enter ends the edit: show the engine's value, or put the old one back */
    w->force_entries = TRUE;
    refresh_now(w);
    w->force_entries = FALSE;
}

static void on_flag_toggled(GtkCheckButton *b, gpointer ud)
{
    DbgWin *w = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "flag"));
    static const int regs[8] = { ASTRO_FLAG_S, ASTRO_FLAG_Z, -1, ASTRO_FLAG_H, -1,
                                 ASTRO_FLAG_PV, ASTRO_FLAG_N, ASTRO_FLAG_C };
    if (w->updating_flags) return;
    if (!astrodebug_is_stopped(w->dbg)) {
        toast(w, "Stop the machine to edit the registers");
        refresh_cpu(w);           /* put the box back */
        return;
    }
    if (regs[i] >= 0)           /* Y and X are not separately settable */
        astrodebug_cpu_set(w->dbg, regs[i], gtk_check_button_get_active(b));
    refresh_now(w);
}

/* "address value...": bytes written through the live bank map. */
static void on_mem_write(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    char **parts = g_strsplit_set(gtk_editable_get_text(GTK_EDITABLE(entry)), " \t:=,", -1);
    char err[128] = "";
    uint8_t bytes[POKE_MAX];
    long a, v;
    int n = 0, i;
    gboolean ok;
    char *addr = NULL;
    for (i = 0; parts[i]; i++) {
        if (!*parts[i]) continue;
        if (!addr) addr = parts[i];
        else if (n < POKE_MAX && parse_num(parts[i], &v) && v >= 0 && v <= 255)
            bytes[n++] = (uint8_t)v;
        else { n = -1; break; }
    }
    ok = addr && n > 0 && parse_addr(w, addr, &a, err, sizeof err);
    if (ok) {
        for (i = 0; i < n; i++)
            astrodebug_write(w->dbg, (uint16_t)(a + i), bytes[i]);
        gtk_editable_set_text(GTK_EDITABLE(entry), "");
        refresh_now(w);
    } else {
        toast(w, err[0] ? err : "Poke takes an address and hex bytes, e.g. C000 41 42");
    }
    g_strfreev(parts);
}

static void mem_set_base(DbgWin *w, int base)
{
    if (base < 0) base = 0;
    if (base > 0x10000 - MEM_PAGE) base = 0x10000 - MEM_PAGE;
    w->mem_base = base & ~0xF;
    refresh_mem(w);
}

/* The page holding a range, the range highlighted (a little context above it). */
static void show_in_memory(DbgWin *w, int lo, int hi)
{
    w->mem_hl = TRUE;
    w->mem_hl_lo = lo;
    w->mem_hl_hi = hi;
    mem_set_base(w, (lo & ~0xF) - 4 * 16);
}

/* Go to: an address, a label, an expression or a range. */
static void on_mem_goto(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    uint16_t lo, hi;
    if (!astrosym_parse_range(w->dbg, gtk_editable_get_text(GTK_EDITABLE(entry)), &lo, &hi)) {
        toast(w, "Not an address, a label or a range");
        return;
    }
    show_in_memory(w, lo, hi);
}

/* Find Next / Find Previous: from the highlight (else the top of the page). */
static void on_search(GtkWidget *button, gpointer ud)
{
    DbgWin *w = ud;
    const int backward = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "back"));
    uint8_t pat[ASTROSEARCH_MAX], mask;
    int n, flags, r;
    uint16_t from, found;

    if (!astrosearch_parse((int)gtk_drop_down_get_selected(w->srch_kind),
                           gtk_editable_get_text(GTK_EDITABLE(w->srch_entry)),
                           pat, &n, &mask, &flags)) {
        gtk_label_set_text(w->srch_note, "Type hex bytes (3E 5A) or some text");
        return;
    }
    if (!astrodebug_is_stopped(w->dbg)) {
        gtk_label_set_text(w->srch_note, "Stop the machine first");
        return;
    }
    if (w->mem_hl)
        from = (uint16_t)w->mem_hl_lo;
    else
        from = backward ? (uint16_t)w->mem_base : (uint16_t)(w->mem_base - 1);
    r = astrodebug_search_next(w->dbg, pat, n, mask, flags, from, backward, &found);
    if (r > 0) {
        char msg[64];
        g_snprintf(msg, sizeof msg, "Found at $%04X", found);
        show_in_memory(w, found, found + n - 1);
        gtk_label_set_text(w->srch_note, msg);
    } else if (r == 0) {
        gtk_label_set_text(w->srch_note, "Not found");
    } else {
        gtk_label_set_text(w->srch_note, "Stop the machine first");
    }
}

static void on_search_activate(GtkEntry *entry, gpointer ud)
{
    on_search(g_object_get_data(G_OBJECT(entry), "next-button"), ud);
}

static void on_mem_page(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    mem_set_base(w, w->mem_base + GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "delta")) * MEM_PAGE);
}

/* Click selects a row; a click on the gutter or a double-click toggles its
 * breakpoint (a label line has no address, so it selects nothing). The
 * address comes from the row, never from the text. */
static void on_disasm_click(GtkGestureClick *g, int n, double x, double y, gpointer ud)
{
    DbgWin *w = ud;
    GtkTextIter it, third;
    GdkRectangle r;
    int bx, by, line, top, height;
    gboolean gutter;
    (void)g;
    gtk_text_view_window_to_buffer_coords(w->disasm, GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &bx, &by);
    /* by line, not by hit-test: the margin left of the gutter is the gutter */
    gtk_text_view_get_line_at_y(w->disasm, &it, by, NULL);
    gtk_text_view_get_line_yrange(w->disasm, &it, &top, &height);
    if (by < top || by >= top + height) return;
    line = gtk_text_iter_get_line(&it);
    if (line < 0 || line >= w->line_count || w->line_addr[line] < 0) return;
    third = it;
    gtk_text_iter_set_line_offset(&third, 2);
    gtk_text_view_get_iter_location(w->disasm, &third, &r);
    gutter = bx < r.x;
    w->selected = w->line_addr[line];
    if ((n == 1 && gutter) || (n == 2 && !gutter))
        astrodebug_breakpoint_toggle(w->dbg, (uint16_t)w->selected);
    refresh_disasm(w);
}

static void on_follow_toggled(GtkCheckButton *b, gpointer ud)
{
    (void)b;
    refresh_disasm(ud);
}

static void on_jump(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    char err[128] = "";
    long a;
    if (!parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(entry)), &a, err, sizeof err)) {
        toast(w, err);
        return;
    }
    gtk_check_button_set_active(w->follow_pc, FALSE);
    w->disasm_top = (int)a;
    refresh_disasm(w);
}

/* The wheel scrolls by instruction. */
static gboolean on_disasm_scroll(GtkEventControllerScroll *c, double dx, double dy, gpointer ud)
{
    DbgWin *w = ud;
    int rows = (int)(dy * 3);
    (void)c; (void)dx;
    if (!rows) rows = dy > 0 ? 1 : -1;
    gtk_check_button_set_active(w->follow_pc, FALSE);
    w->disasm_top = astrodebug_row_address(w->dbg, (uint16_t)w->disasm_top, rows);
    refresh_disasm(w);
    return TRUE;
}

static void bp_enable_toggled(GtkCheckButton *b, gpointer ud)
{
    DbgWin *w = ud;
    astrodebug_breakpoint_enable(w->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "bp")),
                                gtk_check_button_get_active(b));
    refresh_now(w);
}

static void bp_remove_clicked(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    astrodebug_breakpoint_remove(w->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "bp")));
    refresh_now(w);
}

static void on_bp_add(GtkWidget *wgt, gpointer ud)
{
    DbgWin *w = ud;
    static const int types[7] = {
        ASTRODEBUG_BP_EXEC, ASTRODEBUG_BP_READ, ASTRODEBUG_BP_WRITE,
        ASTRODEBUG_BP_READ | ASTRODEBUG_BP_WRITE, ASTRODEBUG_BP_IN, ASTRODEBUG_BP_OUT,
        ASTRODEBUG_BP_IN | ASTRODEBUG_BP_OUT };
    guint sel = gtk_drop_down_get_selected(w->bp_type);
    long a, b;
    int id;
    char err[256] = "";
    const char *end = gtk_editable_get_text(GTK_EDITABLE(w->bp_end));
    (void)wgt;
    if (sel >= G_N_ELEMENTS(types)) sel = 0;
    if (!parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(w->bp_start)), &a, err, sizeof err)) {
        gtk_label_set_text(w->bp_msg, err);
        return;
    }
    b = a;
    if (end && *end && !parse_addr(w, end, &b, err, sizeof err)) {
        gtk_label_set_text(w->bp_msg, err);
        return;
    }
    id = astrodebug_breakpoint_add(w->dbg, types[sel], (uint16_t)a, (uint16_t)b,
                                  gtk_editable_get_text(GTK_EDITABLE(w->bp_cond)),
                                  err, sizeof err);
    if (id < 0) {
        char *clean = clean_text(err[0] ? err : "The breakpoint was refused");
        gtk_label_set_text(w->bp_msg, clean);
        g_free(clean);
        return;
    }
    gtk_label_set_text(w->bp_msg, "");
    gtk_editable_set_text(GTK_EDITABLE(w->bp_start), "");
    gtk_editable_set_text(GTK_EDITABLE(w->bp_end), "");
    gtk_editable_set_text(GTK_EDITABLE(w->bp_cond), "");
    refresh_now(w);
}

static void on_bp_clear(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    astrodebug_breakpoint_clear(w->dbg);
    refresh_now(w);
}

static void on_bp_clear_hits(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    astrodebug_breakpoint_clear_hits(w->dbg);
    refresh_now(w);
}

static void popdown_of(GtkWidget *b)
{
    GtkWidget *pop = gtk_widget_get_ancestor(b, GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
}

static void load_symbols(DbgWin *w, const char *path)
{
    char msg[512] = "";
    astrodebug_load_symbols(w->dbg, path, msg, sizeof msg);
    notice(w, msg);
    refresh_now(w);
}

static void on_symbols_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    DbgWin *w = ud;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    if (!file || w->closing) return;
    path = g_file_get_path(file);
    if (path) load_symbols(w, path);
}

static void on_symbols(GtkWidget *b, gpointer ud)
{
    DbgWin *w = ud;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *syms = gtk_file_filter_new();
    GtkFileFilter *all = gtk_file_filter_new();
    const char *cart = astrosession_get_str(w->session, "cart", "");
    (void)b;

    gtk_file_filter_set_name(syms, "Symbol files (*.sym, *.map, *.noi, *.txt)");
    gtk_file_filter_add_suffix(syms, "sym");
    gtk_file_filter_add_suffix(syms, "map");
    gtk_file_filter_add_suffix(syms, "noi");
    gtk_file_filter_add_suffix(syms, "txt");
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, syms);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_title(dlg, "Load Symbols (HHHH NAME, WLA-DX .sym, z88dk .map, SDCC .noi)");
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    /* a game's symbols usually sit beside it */
    if (cart && *cart) {
        g_autofree char *dir = g_path_get_dirname(cart);
        g_autoptr(GFile) folder = g_file_new_for_path(dir);
        gtk_file_dialog_set_initial_folder(dlg, folder);
    }
    gtk_file_dialog_open(dlg, w->win, NULL, on_symbols_chosen, w);
    g_object_unref(syms);
    g_object_unref(all);
    g_object_unref(filters);
    g_object_unref(dlg);
}

/* <cartridge>.sym/.map/.noi next to the opened cartridge, no picker needed. */
static void on_symbols_beside(GtkButton *b, gpointer ud)
{
    popdown_of(GTK_WIDGET(b));
    load_symbols(ud, NULL);
}

static void on_save_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    DbgWin *w = ud;
    g_autoptr(GFile) file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    const char *kind = g_object_get_data(src, "kind");
    char msg[512] = "";
    if (!file || w->closing) return;
    path = g_file_get_path(file);
    if (!path) return;
    astrodebug_save(w->dbg, kind, path, msg, sizeof msg);
    notice(w, msg);
}

static void on_save(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    const char *kind = g_object_get_data(G_OBJECT(b), "kind");
    char title[64];
    popdown_of(GTK_WIDGET(b));
    g_snprintf(title, sizeof title, "Save %s", (const char *)g_object_get_data(G_OBJECT(b), "title"));
    gtk_file_dialog_set_title(dlg, title);
    gtk_file_dialog_set_initial_name(dlg, g_object_get_data(G_OBJECT(b), "file"));
    g_object_set_data(G_OBJECT(dlg), "kind", (gpointer)kind);
    gtk_file_dialog_save(dlg, w->win, NULL, on_save_chosen, w);
    g_object_unref(dlg);
}

static void on_fuji_clear(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    astrodebug_fujinet_trace_clear(w->dbg);
    refresh_fujinet(w, TRUE);
}

/* Help's contexts (astrohelp.h), one per tab */
static const char *const k_tab_contexts[NTABS] = {
    "debugger/console", "debugger/disassembly", "debugger/memory", "debugger/breakpoints",
    "debugger/watches", "debugger/cart", "debugger/trace", "debugger/history",
    "debugger/video", "debugger/fujinet",
};

/* The notebook also switches pages as it is taken apart with the window
 * (main window closing), before the window's "destroy": only a mapped window
 * has a tab to refresh, a hidden one refreshes when it is shown. */
static void on_switch_page(GtkNotebook *nb, GtkWidget *page, guint num, gpointer ud)
{
    DbgWin *w = ud;
    (void)nb; (void)num;
    if (w->closing) return;
    astro_help_set_context(w->win, k_tab_contexts[page_tab(page)]);
    if (gtk_widget_get_mapped(GTK_WIDGET(w->win)))
        refresh_tab(w, page_tab(page));
}

/* Record on either page is the one recorder. */
static void on_hist_rec(GtkToggleButton *b, gpointer ud)
{
    DbgWin *w = ud;
    if (w->h_sync) return;
    astrodebug_trace_enable(w->dbg, gtk_toggle_button_get_active(b));
    w->h_sig[0] = w->h_sig[1] = ~(uint64_t)0;
    refresh_now(w);
}

static void on_hist_clear(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    astrodebug_trace_clear(w->dbg);
    w->h_sig[0] = w->h_sig[1] = ~(uint64_t)0;
    refresh_now(w);
}

static void on_hist_changed(GObject *o, GParamSpec *p, gpointer ud)
{
    DbgWin *w = ud;
    (void)o; (void)p;
    w->h_sig[0] = w->h_sig[1] = ~(uint64_t)0;
    refresh_now(w);
}

static void on_hist_activate(GtkEntry *e, gpointer ud)
{
    on_hist_changed(NULL, NULL, ud);
    (void)e;
}

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint code,
                       GdkModifierType st, gpointer ud);

/* Puts a popped-out tab back in the notebook, at its place in the order. */
static void dock_tab(DbgWin *w, int tab, gboolean destroy)
{
    GtkWindow *win = w->detached[tab];
    GtkWidget *page = w->pages[tab], *vbox;
    int pos = 0, t;

    if (!win) return;
    vbox = g_object_get_data(G_OBJECT(win), "vbox");
    g_object_ref(page);
    gtk_box_remove(GTK_BOX(vbox), page);
    w->detached[tab] = NULL;
    for (t = 0; t < tab; t++)
        if (!w->detached[t]) pos++;
    gtk_notebook_insert_page(w->notebook, page, gtk_label_new(k_tab_names[tab]), pos);
    g_object_unref(page);
    gtk_notebook_set_current_page(w->notebook, pos);
    if (destroy) gtk_window_destroy(win);
    if (!w->closing) refresh_now(w);
}

static void on_dock_button(GtkButton *b, gpointer ud)
{
    dock_tab(ud, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "tab")), TRUE);
}

static gboolean on_detached_close(GtkWindow *win, gpointer ud)
{
    dock_tab(ud, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(win), "tab")), FALSE);
    return FALSE;     /* let it close */
}

/* Any tab can move to a window of its own; one always stays. */
static void pop_out_tab(DbgWin *w, int tab)
{
    GtkWidget *page, *vbox, *header, *dock;
    GtkWindow *win;
    GtkEventController *keys;
    char title[96];

    if (tab < 0 || tab >= NTABS || w->detached[tab]) return;
    if (gtk_notebook_get_n_pages(w->notebook) <= 1) {
        toast(w, "One tab always stays in the debugger");
        return;
    }
    page = w->pages[tab];
    g_object_ref(page);
    gtk_notebook_remove_page(w->notebook, gtk_notebook_page_num(w->notebook, page));

    win = GTK_WINDOW(adw_window_new());
    g_snprintf(title, sizeof title, "Astrocade Debugger - %s", k_tab_names[tab]);
    gtk_window_set_title(win, title);
    gtk_window_set_icon_name(win, astro_icon_name());
    gtk_window_set_default_size(win, 800, 560);
    gtk_window_set_transient_for(win, w->win);
    header = adw_header_bar_new();
    dock = gtk_button_new_with_label("Dock");
    gtk_widget_set_tooltip_text(dock, "Put this tab back in the debugger");
    g_object_set_data(G_OBJECT(dock), "tab", GINT_TO_POINTER(tab));
    g_signal_connect(dock, "clicked", G_CALLBACK(on_dock_button), w);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), dock);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), astro_help_button(NULL));
    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(vbox), header);
    gtk_box_append(GTK_BOX(vbox), page);
    gtk_widget_set_vexpand(page, TRUE);
    adw_window_set_content(ADW_WINDOW(win), vbox);
    g_object_unref(page);
    g_object_set_data(G_OBJECT(win), "vbox", vbox);
    g_object_set_data(G_OBJECT(win), "tab", GINT_TO_POINTER(tab));
    astro_help_set_context(win, k_tab_contexts[tab]);
    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), w);
    gtk_widget_add_controller(GTK_WIDGET(win), keys);
    g_signal_connect(win, "close-request", G_CALLBACK(on_detached_close), w);
    w->detached[tab] = win;
    gtk_window_present(win);
    refresh_now(w);
}

static void on_pop_out(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    pop_out_tab(w, current_tab(w));
}

/* Closing hides: the machine runs on, and the window is back on F12. */
static void hide_window(DbgWin *w)
{
    int t;
    for (t = 0; t < NTABS; t++)
        dock_tab(w, t, TRUE);       /* popped-out tabs go home with the window */
    astrodebug_detach(w->dbg);
    gtk_widget_set_visible(GTK_WIDGET(w->win), FALSE);
}

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint code,
                       GdkModifierType st, gpointer ud)
{
    DbgWin *w = ud;
    GtkWidget *src = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(c));
    (void)code;
    switch (keyval) {
    case GDK_KEY_F1: astro_help_for(GTK_IS_WINDOW(src) ? GTK_WINDOW(src) : w->win); return TRUE;
    case GDK_KEY_F5: on_run(NULL, w); return TRUE;
    case GDK_KEY_F7:
        if (st & GDK_SHIFT_MASK) on_frame(NULL, w); else on_step(NULL, w);
        return TRUE;
    case GDK_KEY_F8:
        if (st & GDK_SHIFT_MASK) on_step_out(NULL, w); else on_step_over(NULL, w);
        return TRUE;
    case GDK_KEY_F9: toggle_selected(w); return TRUE;
    case GDK_KEY_F12: hide_window(w); return TRUE;
    default: return FALSE;
    }
}

static gboolean on_close(GtkWindow *win, gpointer ud)
{
    (void)win;
    hide_window(ud);
    return TRUE;
}

/* ---- construction --------------------------------------------------------- */

static GtkWidget *build_toolbar(DbgWin *w)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *b, *sym, *sympop, *beside, *save, *pop, *popbox;
    static const struct { const char *kind, *title, *file; } saves[] = {
        { "dis", "Disassembly ($0000-$FFFF)", "disassembly.asm" },
        { "mem", "Memory (64K)", "memory.bin" },
        { "vram", "Screen RAM (4K)", "screenram.bin" },
        { "regs", "Registers (text)", "registers.txt" } };
    unsigned i;

    gtk_widget_set_margin_start(bar, 8);
    gtk_widget_set_margin_end(bar, 8);
    gtk_widget_set_margin_top(bar, 6);
    gtk_widget_set_margin_bottom(bar, 6);
#define TB(label, cb) do { b = gtk_button_new_with_label(label); \
    g_signal_connect(b, "clicked", G_CALLBACK(cb), w); gtk_box_append(GTK_BOX(bar), b); } while (0)
    w->run_btn = GTK_BUTTON(gtk_button_new_with_label("Stop (F5)"));
    g_signal_connect(w->run_btn, "clicked", G_CALLBACK(on_run), w);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->run_btn));
    TB("Step (F7)", on_step);
    TB("Step Over (F8)", on_step_over);
    TB("Step Out (\xe2\x87\xa7""F8)", on_step_out);
    TB("Frame (\xe2\x87\xa7""F7)", on_frame);
    TB("Run to Cursor", on_run_to_cursor);
#undef TB

    /* Load Symbols: a file, or (the arrow) the files beside the cartridge */
    sym = adw_split_button_new();
    adw_split_button_set_label(ADW_SPLIT_BUTTON(sym), "Load Symbols\xe2\x80\xa6");
    adw_split_button_set_dropdown_tooltip(ADW_SPLIT_BUTTON(sym), "More ways to load symbols");
    g_signal_connect(sym, "clicked", G_CALLBACK(on_symbols), w);
    sympop = gtk_popover_new();
    beside = gtk_button_new_with_label("Beside the Cartridge (.sym, .map, .noi)");
    gtk_widget_add_css_class(beside, "flat");
    g_signal_connect(beside, "clicked", G_CALLBACK(on_symbols_beside), w);
    gtk_popover_set_child(GTK_POPOVER(sympop), beside);
    adw_split_button_set_popover(ADW_SPLIT_BUTTON(sym), GTK_POPOVER(sympop));
    gtk_box_append(GTK_BOX(bar), sym);

    save = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(save), "Save\xe2\x80\xa6");
    pop = gtk_popover_new();
    popbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    for (i = 0; i < G_N_ELEMENTS(saves); i++) {
        GtkWidget *sb = gtk_button_new_with_label(saves[i].title);
        gtk_widget_add_css_class(sb, "flat");
        g_object_set_data(G_OBJECT(sb), "kind", (gpointer)saves[i].kind);
        g_object_set_data(G_OBJECT(sb), "title", (gpointer)saves[i].title);
        g_object_set_data(G_OBJECT(sb), "file", (gpointer)saves[i].file);
        g_signal_connect(sb, "clicked", G_CALLBACK(on_save), w);
        gtk_box_append(GTK_BOX(popbox), sb);
    }
    gtk_popover_set_child(GTK_POPOVER(pop), popbox);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(save), pop);
    gtk_box_append(GTK_BOX(bar), save);

    b = gtk_button_new_with_label("Pop Out");
    gtk_widget_set_tooltip_text(b, "Move the tab showing into a window of its own");
    gtk_widget_set_halign(b, GTK_ALIGN_END);
    gtk_widget_set_hexpand(b, TRUE);
    g_signal_connect(b, "clicked", G_CALLBACK(on_pop_out), w);
    gtk_box_append(GTK_BOX(bar), b);
    return bar;
}

static GtkWidget *build_console(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *scroll = mono_view(&w->console_out, TRUE);
    GtkEventController *keys;

    make_log(w->console_out);
    w->console_in = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->console_in,
        "Command (help, step, break, bpw, bpin, mem, poke, disasm, regs, ...) "
        "\xe2\x80\x94 Tab completes, Up/Down recall");
    gtk_widget_set_hexpand(GTK_WIDGET(w->console_in), TRUE);
    g_signal_connect(w->console_in, "activate", G_CALLBACK(on_console), w);
    /* ahead of the entry's own text handling, so Tab and Up/Down are ours */
    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_console_key), w);
    gtk_widget_add_controller(GTK_WIDGET(w->console_in), keys);
    w->history = g_ptr_array_new_with_free_func(g_free);

    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(w->console_in));
    set_text_at(w->console_out, "Astrocade debugger console (Z80). Type 'help' for every command. "
                                "Numbers: $C000, 0xC000, C000h, 49152, %1010.\n", TRUE);
    return padded(box);
}

/* Under the notebook, on every tab: the registers (Enter sets one while the
 * machine is stopped), the flags and the beam. */
static GtkWidget *build_regs(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *regs1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *regs2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *flags = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    static const char *const fnames[8] = { "S", "Z", "Y", "H", "X", "P/V", "N", "C" };
    int i;
    for (i = 0; i < NREGS; i++) {
        w->reg[i] = GTK_ENTRY(gtk_entry_new());
        gtk_entry_set_max_length(w->reg[i], 6);
        gtk_editable_set_width_chars(GTK_EDITABLE(w->reg[i]), k_regs[i].digits + 1);
        gtk_editable_set_max_width_chars(GTK_EDITABLE(w->reg[i]), k_regs[i].digits + 1);
        gtk_widget_add_css_class(GTK_WIDGET(w->reg[i]), "monospace");
        g_object_set_data(G_OBJECT(w->reg[i]), "reg", GINT_TO_POINTER(i));
        g_signal_connect(w->reg[i], "activate", G_CALLBACK(on_reg_activate), w);
        /* PC, SP and the main set on the first row; shadows and control on the second */
        gtk_box_append(GTK_BOX(i < REGS_FIRST_ROW ? regs1 : regs2),
                       labeled(k_regs[i].name, GTK_WIDGET(w->reg[i])));
    }
    for (i = 0; i < 8; i++) {
        w->flag[i] = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(fnames[i]));
        g_object_set_data(G_OBJECT(w->flag[i]), "flag", GINT_TO_POINTER(i));
        /* Y and X (bits 5 and 3 of F) are shown, set through AF */
        if (i == 2 || i == 4) gtk_widget_set_sensitive(GTK_WIDGET(w->flag[i]), FALSE);
        g_signal_connect(w->flag[i], "toggled", G_CALLBACK(on_flag_toggled), w);
        gtk_box_append(GTK_BOX(flags), GTK_WIDGET(w->flag[i]));
    }
    w->beam = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->beam), "dim-label");
    gtk_label_set_xalign(w->beam, 0);
    gtk_widget_set_hexpand(GTK_WIDGET(w->beam), TRUE);
    gtk_box_append(GTK_BOX(flags), GTK_WIDGET(w->beam));
    gtk_box_append(GTK_BOX(box), regs1);
    gtk_box_append(GTK_BOX(box), regs2);
    gtk_box_append(GTK_BOX(box), flags);
    return padded(box);
}

/* Find Next / Previous over the dump, with a note for what happened. */
static GtkWidget *build_search_bar(DbgWin *w)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *next = gtk_button_new_with_label("Find Next");
    GtkWidget *prev = gtk_button_new_with_label("Find Previous");
    static const char *const kinds[] = { "Hex", "Text", NULL };

    g_object_set_data(G_OBJECT(prev), "back", GINT_TO_POINTER(1));
    g_signal_connect(next, "clicked", G_CALLBACK(on_search), w);
    g_signal_connect(prev, "clicked", G_CALLBACK(on_search), w);
    w->srch_kind = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(kinds));
    gtk_widget_set_tooltip_text(GTK_WIDGET(w->srch_kind), "Hex bytes (3E 5A) or text (one byte per character)");
    w->srch_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->srch_entry, "search the 64K: 3E 5A, or text");
    gtk_widget_set_hexpand(GTK_WIDGET(w->srch_entry), TRUE);
    g_object_set_data(G_OBJECT(w->srch_entry), "next-button", next);
    g_signal_connect(w->srch_entry, "activate", G_CALLBACK(on_search_activate), w);
    w->srch_note = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->srch_note), "dim-label");
    gtk_label_set_ellipsize(w->srch_note, PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(w->srch_note, 18);
    gtk_label_set_max_width_chars(w->srch_note, 18);
    gtk_box_append(GTK_BOX(bar), gtk_label_new("Search"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->srch_kind));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->srch_entry));
    gtk_box_append(GTK_BOX(bar), next);
    gtk_box_append(GTK_BOX(bar), prev);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->srch_note));
    return bar;
}

static GtkWidget *build_memory(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *mem = mono_view(&w->mem_view, FALSE);
    GtkWidget *edit = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *up, *down;

    w->mem_goto = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_width_chars(GTK_EDITABLE(w->mem_goto), 16);
    gtk_entry_set_placeholder_text(w->mem_goto, "$C000 / label / lo-hi");
    g_signal_connect(w->mem_goto, "activate", G_CALLBACK(on_mem_goto), w);
    w->mem_write = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_width_chars(GTK_EDITABLE(w->mem_write), 14);
    gtk_entry_set_placeholder_text(w->mem_write, "C000 41 42");
    g_signal_connect(w->mem_write, "activate", G_CALLBACK(on_mem_write), w);
    up = gtk_button_new_with_label("Page Up");
    down = gtk_button_new_with_label("Page Down");
    g_object_set_data(G_OBJECT(up), "delta", GINT_TO_POINTER(-1));
    g_object_set_data(G_OBJECT(down), "delta", GINT_TO_POINTER(1));
    g_signal_connect(up, "clicked", G_CALLBACK(on_mem_page), w);
    g_signal_connect(down, "clicked", G_CALLBACK(on_mem_page), w);
    gtk_box_append(GTK_BOX(edit), labeled("Go to", GTK_WIDGET(w->mem_goto)));
    gtk_box_append(GTK_BOX(edit), up);
    gtk_box_append(GTK_BOX(edit), down);
    gtk_box_append(GTK_BOX(edit), labeled("Poke (address bytes)", GTK_WIDGET(w->mem_write)));

    gtk_box_append(GTK_BOX(box), edit);
    gtk_box_append(GTK_BOX(box), build_search_bar(w));
    gtk_box_append(GTK_BOX(box), mem);
    /* the highlight covers a few bytes, not a paragraph */
    gtk_text_buffer_create_tag(gtk_text_view_get_buffer(w->mem_view), "mark",
                               "background", "#b5342c", "foreground", "#ffffff", NULL);
    w->mem_hl = FALSE;
    return padded(box);
}

static GtkWidget *build_disasm(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *scroll = mono_view(&w->disasm, FALSE);
    GtkGesture *click = gtk_gesture_click_new();
    GtkEventController *scrollc = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->disasm);
    GtkWidget *hint;

    /* later tags win: the selection under the PC line, the gutter marker
     * over both */
    gtk_text_buffer_create_tag(b, "dim", "foreground", "#8a8a8a", NULL);
    gtk_text_buffer_create_tag(b, "sel", "paragraph-background", "rgba(128,128,128,0.35)", NULL);
    accent_tag(w->disasm, "pc");
    gtk_text_buffer_create_tag(b, "bp_on", "foreground", "#e01b24", NULL);
    gtk_text_buffer_create_tag(b, "bp_off", "foreground", "#8a8a8a", NULL);

    w->follow_pc = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Follow PC"));
    gtk_check_button_set_active(w->follow_pc, TRUE);
    g_signal_connect(w->follow_pc, "toggled", G_CALLBACK(on_follow_toggled), w);
    w->jump = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->jump, "$0038 / label / expression");
    gtk_editable_set_width_chars(GTK_EDITABLE(w->jump), 22);
    g_signal_connect(w->jump, "activate", G_CALLBACK(on_jump), w);
    hint = gtk_label_new("Click selects a line; double-click, F9 or the gutter toggles its "
                         "breakpoint; scroll to browse");
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->follow_pc));
    gtk_box_append(GTK_BOX(row), labeled("Jump to", GTK_WIDGET(w->jump)));
    gtk_box_append(GTK_BOX(row), hint);

    g_signal_connect(click, "pressed", G_CALLBACK(on_disasm_click), w);
    gtk_widget_add_controller(GTK_WIDGET(w->disasm), GTK_EVENT_CONTROLLER(click));
    g_signal_connect(scrollc, "scroll", G_CALLBACK(on_disasm_scroll), w);
    gtk_widget_add_controller(GTK_WIDGET(w->disasm), scrollc);
    /* the wheel is ours; EXTERNAL (unlike NEVER) does not ask for a window
     * tall enough to show every one of the DISASM_WINDOW lines */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_EXTERNAL);

    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), scroll);
    w->disasm_top = 0;
    w->selected = -1;
    return padded(box);
}

static GtkWidget *build_video(DbgWin *w)
{
    GtkWidget *grid = gtk_grid_new(), *scroll, *outer;

    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_widget_set_margin_start(grid, 8);
    gtk_widget_set_margin_top(grid, 8);
    gtk_widget_set_margin_end(grid, 8);
    gtk_widget_set_margin_bottom(grid, 8);

    w->pic_screen = GTK_PICTURE(gtk_picture_new());
    gtk_widget_set_size_request(GTK_WIDGET(w->pic_screen), 352, 240);
    w->pic_bitmap = GTK_PICTURE(gtk_picture_new());
    gtk_widget_set_size_request(GTK_WIDGET(w->pic_bitmap), 320, 204);
    w->pic_pal = GTK_PICTURE(gtk_picture_new());
    gtk_picture_set_content_fit(w->pic_pal, GTK_CONTENT_FIT_FILL);
    gtk_widget_set_size_request(GTK_WIDGET(w->pic_pal), ASTROVID_PAL_COLS * ASTROVID_PAL_CELL,
                                16 * ASTROVID_PAL_CELL);
    gtk_widget_set_halign(GTK_WIDGET(w->pic_pal), GTK_ALIGN_START);

    gtk_grid_attach(GTK_GRID(grid), headed("Screen", GTK_WIDGET(w->pic_screen)), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), headed("Screen RAM", GTK_WIDGET(w->pic_bitmap)), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), headed("Palette (512 pens)", GTK_WIDGET(w->pic_pal)), 0, 1, 2, 1);
    scroll = mono_view(&w->vid_state, FALSE);
    gtk_widget_set_size_request(scroll, -1, 360);
    gtk_grid_attach(GTK_GRID(grid), headed("Registers", scroll), 0, 2, 2, 1);

    outer = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(outer), grid);
    return outer;
}

static GtkWidget *build_breaks(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *add, *clear, *hits, *head, *hint;
    static const char *const types[] = { "Execute", "Read", "Write", "Read/Write", "Port In",
                                         "Port Out", "Port In/Out", NULL };

    w->bp_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(w->bp_list, GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(w->bp_list));
    gtk_widget_set_vexpand(scroll, TRUE);

    w->bp_type = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(types));
    w->bp_start = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->bp_start, "$C000 / port / label");
    gtk_editable_set_width_chars(GTK_EDITABLE(w->bp_start), 14);
    w->bp_end = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->bp_end, "(end)");
    gtk_editable_set_width_chars(GTK_EDITABLE(w->bp_end), 8);
    w->bp_cond = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->bp_cond, "condition, e.g. a == 5 && hl > $8000 "
                                               "\xe2\x80\x94 optional");
    gtk_widget_set_hexpand(GTK_WIDGET(w->bp_cond), TRUE);
    g_signal_connect(w->bp_start, "activate", G_CALLBACK(on_bp_add), w);
    g_signal_connect(w->bp_end, "activate", G_CALLBACK(on_bp_add), w);
    g_signal_connect(w->bp_cond, "activate", G_CALLBACK(on_bp_add), w);
    add = gtk_button_new_with_label("Add");
    g_signal_connect(add, "clicked", G_CALLBACK(on_bp_add), w);
    clear = gtk_button_new_with_label("Clear All");
    g_signal_connect(clear, "clicked", G_CALLBACK(on_bp_clear), w);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_type));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_start));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_end));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_cond));
    gtk_box_append(GTK_BOX(row), add);
    gtk_box_append(GTK_BOX(row), clear);
    w->bp_msg = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->bp_msg), "astro-accent-text");
    gtk_label_set_xalign(w->bp_msg, 0);
    gtk_label_set_wrap(w->bp_msg, TRUE);
    hint = gtk_label_new("A port breakpoint takes a port number (0-255). A condition is an "
                         "expression like the console's; `value` and `addr` are the access.");
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);

    w->bp_stop = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(w->bp_stop, 0);
    gtk_widget_set_hexpand(GTK_WIDGET(w->bp_stop), TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(w->bp_stop), "heading");
    hits = gtk_button_new_with_label("Clear Hits");
    gtk_widget_set_tooltip_text(hits, "Zero every hit count");
    g_signal_connect(hits, "clicked", G_CALLBACK(on_bp_clear_hits), w);
    head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(head), GTK_WIDGET(w->bp_stop));
    gtk_box_append(GTK_BOX(head), hits);

    gtk_box_append(GTK_BOX(box), head);
    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(w->bp_msg));
    gtk_box_append(GTK_BOX(box), hint);
    return padded(box);
}

static GtkWidget *build_watches(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *form = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *add;

    w->wt_labels = g_ptr_array_new();
    w->wt_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->wt_entry, "address, label, label+3, [ptr], C000-C0FF");
    gtk_widget_set_hexpand(GTK_WIDGET(w->wt_entry), TRUE);
    g_signal_connect(w->wt_entry, "activate", G_CALLBACK(on_wt_add), w);
    add = gtk_button_new_with_label("Add");
    g_signal_connect(add, "clicked", G_CALLBACK(on_wt_add), w);
    gtk_box_append(GTK_BOX(form), gtk_label_new("Watch"));
    gtk_box_append(GTK_BOX(form), GTK_WIDGET(w->wt_entry));
    gtk_box_append(GTK_BOX(form), add);

    w->wt_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(w->wt_list, GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(w->wt_list));
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(box), form);
    gtk_box_append(GTK_BOX(box), scroll);
    return padded(box);
}

static GtkWidget *build_cart(DbgWin *w)
{
    return padded(mono_view(&w->cart_text, FALSE));
}

/* Trace (idx 0) and History (idx 1): the same controls over the one ring. */
static GtkWidget *build_hist(DbgWin *w, int idx)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = mono_view(&w->h_text[idx], FALSE);
    GtkWidget *clear = gtk_button_new_with_label("Clear");
    static const char *const filters[] = { "All instructions", "Branches only", "In range", NULL };

    w->h_rec[idx] = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Record"));
    gtk_widget_add_css_class(GTK_WIDGET(w->h_rec[idx]), "astro-toggle");
    gtk_widget_set_tooltip_text(GTK_WIDGET(w->h_rec[idx]),
                                "Record every instruction the CPU runs (the machine runs slower)");
    g_signal_connect(w->h_rec[idx], "toggled", G_CALLBACK(on_hist_rec), w);
    w->h_filter[idx] = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(filters));
    g_signal_connect(w->h_filter[idx], "notify::selected", G_CALLBACK(on_hist_changed), w);
    w->h_range[idx] = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->h_range[idx], "range: C000-C0FF or a label");
    g_signal_connect(w->h_range[idx], "activate", G_CALLBACK(on_hist_activate), w);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_hist_clear), w);
    w->h_note[idx] = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->h_note[idx]), "dim-label");
    gtk_widget_set_hexpand(GTK_WIDGET(w->h_note[idx]), TRUE);
    gtk_label_set_xalign(w->h_note[idx], 1.0f);
    make_log(w->h_text[idx]);
    w->h_sig[idx] = ~(uint64_t)0;

    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->h_rec[idx]));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->h_filter[idx]));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->h_range[idx]));
    gtk_box_append(GTK_BOX(bar), clear);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->h_note[idx]));
    gtk_box_append(GTK_BOX(box), bar);
    gtk_box_append(GTK_BOX(box), scroll);
    return padded(box);
}

/* The packets left (a note and Clear over them), the BoIP link and its
 * counts right. */
static GtkWidget *build_fujinet(DbgWin *w)
{
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *clear = gtk_button_new_with_label("Clear");
    GtkWidget *start, *end;

    w->fuji_note = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->fuji_note), "dim-label");
    gtk_label_set_xalign(w->fuji_note, 0);
    gtk_label_set_ellipsize(w->fuji_note, PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(GTK_WIDGET(w->fuji_note), TRUE);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_fuji_clear), w);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->fuji_note));
    gtk_box_append(GTK_BOX(row), clear);
    gtk_box_append(GTK_BOX(left), row);
    gtk_box_append(GTK_BOX(left), mono_view(&w->fuji_text, FALSE));
    make_log(w->fuji_text);

    start = headed("FujiBus packets (SLIP-decoded)", left);
    end = headed("FujiNet cartridge", mono_view(&w->fuji_state, FALSE));
    gtk_paned_set_start_child(GTK_PANED(paned), start);
    gtk_paned_set_end_child(GTK_PANED(paned), end);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), FALSE);
    gtk_widget_set_size_request(start, 600, -1);
    gtk_widget_set_size_request(end, 300, -1);
    w->fuji_count = -1;
    return padded(paned);
}

/* The state outlives the window: the widgets go before the window's
 * "destroy" and can still call back (the notebook's page switches), so it is
 * freed only once the window object itself is gone. */
static void on_win_finalized(gpointer data, GObject *where)
{
    DbgWin *w = data;
    (void)where;
    if (w->history) g_ptr_array_unref(w->history);
    if (w->bp_sig) g_string_free(w->bp_sig, TRUE);
    if (w->wt_labels) g_ptr_array_unref(w->wt_labels);
    g_free(w);
}

static void on_destroy(GtkWidget *widget, gpointer ud)
{
    DbgWin *w = ud;
    (void)widget;
    w->closing = TRUE;
    if (w->timer) g_source_remove(w->timer);
    w->timer = 0;
    astrodebug_detach(w->dbg);
    if (g_win == w) g_win = NULL;
}

static void attach_and_refresh(DbgWin *w)
{
    astrodebug_attach(w->dbg);    /* stops the machine, within a frame */
    settle(w, TRUE);
    w->running_ticks = 0;
    if (w->bp_sig) { g_string_free(w->bp_sig, TRUE); w->bp_sig = NULL; }
    refresh_now(w);
}

/* A tab from an environment variable (ASTRO_DEBUGGER_TAB, ASTRO_POPOUT): a
 * number (0-9), or a tab's name or the start of one, any case. */
static int tab_from_env(const char *var)
{
    const char *tab = g_getenv(var);
    char *end;
    long n;
    unsigned i;
    if (!tab || !*tab) return -1;
    n = strtol(tab, &end, 10);
    if (end != tab && *end == '\0')
        return (n >= 0 && n < (long)G_N_ELEMENTS(k_tab_names)) ? (int)n : -1;
    for (i = 0; i < G_N_ELEMENTS(k_tab_names); i++)
        if (g_ascii_strncasecmp(tab, k_tab_names[i], strlen(tab)) == 0)
            return (int)i;
    if (g_ascii_strncasecmp(tab, "io", 2) == 0 || g_ascii_strncasecmp(tab, "i/o", 3) == 0)
        return TAB_CART;
    return -1;
}

void astro_debugger_show(GtkWindow *parent, astrosession *session)
{
    DbgWin *w;
    GtkWidget *toolbar, *header, *root, *notebook, *overlay;
    GtkEventController *keys;
    int tab, i;

    if (g_win) {
        gboolean shown = gtk_widget_get_visible(GTK_WIDGET(g_win->win));
        gtk_window_present(g_win->win);
        if (!shown) attach_and_refresh(g_win);
        return;
    }
    install_css();
    w = g_new0(DbgWin, 1);
    w->session = session;
    w->dbg = astrosession_debugger(session);
    g_win = w;

    w->win = GTK_WINDOW(adw_window_new());
    g_object_weak_ref(G_OBJECT(w->win), on_win_finalized, w);
    gtk_window_set_title(w->win, "Debugger");
    gtk_window_set_icon_name(w->win, astro_icon_name());
    gtk_window_set_default_size(w->win, 1150, 800);
    gtk_window_set_transient_for(w->win, parent);
    gtk_window_set_destroy_with_parent(w->win, TRUE);
    g_signal_connect(w->win, "close-request", G_CALLBACK(on_close), w);
    g_signal_connect(w->win, "destroy", G_CALLBACK(on_destroy), w);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(root), build_toolbar(w));
    notebook = gtk_notebook_new();
    w->notebook = GTK_NOTEBOOK(notebook);
    gtk_widget_set_vexpand(notebook, TRUE);
    w->pages[TAB_CONSOLE] = build_console(w);
    w->pages[TAB_DISASM] = build_disasm(w);
    w->pages[TAB_MEMORY] = build_memory(w);
    w->pages[TAB_BREAKS] = build_breaks(w);
    w->pages[TAB_WATCHES] = build_watches(w);
    w->pages[TAB_CART] = build_cart(w);
    w->pages[TAB_TRACE] = build_hist(w, 0);
    w->pages[TAB_HISTORY] = build_hist(w, 1);
    w->pages[TAB_VIDEO] = build_video(w);
    w->pages[TAB_FUJINET] = build_fujinet(w);
    for (i = 0; i < NTABS; i++) {
        g_object_set_data(G_OBJECT(w->pages[i]), "tab", GINT_TO_POINTER(i));
        gtk_notebook_append_page(w->notebook, w->pages[i], gtk_label_new(k_tab_names[i]));
    }
    gtk_box_append(GTK_BOX(root), notebook);
    gtk_box_append(GTK_BOX(root), build_regs(w));
    tab = tab_from_env("ASTRO_DEBUGGER_TAB");
    if (tab >= 0) gtk_notebook_set_current_page(w->notebook, tab);
    g_signal_connect(notebook, "switch-page", G_CALLBACK(on_switch_page), w);
    astro_help_set_context(w->win, k_tab_contexts[current_tab(w)]);

    overlay = adw_toast_overlay_new();
    w->toasts = ADW_TOAST_OVERLAY(overlay);
    adw_toast_overlay_set_child(w->toasts, root);

    /* the status line -- why the machine stopped -- is the title's subtitle */
    header = adw_header_bar_new();
    w->title = ADW_WINDOW_TITLE(adw_window_title_new("Debugger", ""));
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), GTK_WIDGET(w->title));
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), astro_help_button(NULL));
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), overlay);
    adw_window_set_content(ADW_WINDOW(w->win), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), w);
    gtk_widget_add_controller(GTK_WIDGET(w->win), keys);

    /* captures: a watch, a break-on-change, a Go to, recording, a popped-out tab */
    {
        const char *wt = g_getenv("ASTRO_WATCH"), *wb = g_getenv("ASTRO_WATCH_BREAK"),
                   *mg = g_getenv("ASTRO_MEM_GOTO");
        uint16_t lo, hi;
        if (wt && *wt) astrowatch_add(wt);
        if (wb && *wb && astrosym_parse_range(w->dbg, wb, &lo, &hi)) break_on_change(w, lo, hi);
        if (mg && *mg && astrosym_parse_range(w->dbg, mg, &lo, &hi)) show_in_memory(w, lo, hi);
        if (g_getenv("ASTRO_HISTORY")) astrodebug_trace_enable(w->dbg, 1);
    }
    attach_and_refresh(w);
    g_signal_connect(w->win, "map", G_CALLBACK(on_win_map), w);
    g_signal_connect(w->win, "unmap", G_CALLBACK(on_win_unmap), w);
    gtk_window_present(w->win);
    if (current_tab(w) == TAB_CONSOLE)
        gtk_widget_grab_focus(GTK_WIDGET(w->console_in));
    tab = tab_from_env("ASTRO_POPOUT");
    if (tab >= 0) pop_out_tab(w, tab);
}

/* F12 and the menu item: show it, or hide it if it is showing. */
void astro_debugger_toggle(GtkWindow *parent, astrosession *session)
{
    if (g_win && gtk_widget_get_visible(GTK_WIDGET(g_win->win)))
        hide_window(g_win);
    else
        astro_debugger_show(parent, session);
}
