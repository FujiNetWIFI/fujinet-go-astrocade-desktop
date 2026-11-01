/*
 * AstroDisplay: paints the emulator's latest XRGB8888 frame with a
 * GdkMemoryTexture, letterboxed to 4:3 inside whatever size the window
 * manager gives us. A frame-clock tick pulls the latest frame; the tick runs
 * only while the widget is mapped and its window is not suspended. A frame
 * identical to the previous one is not published by the core, so a still
 * picture costs a mutex per tick and nothing else. The emulator's host thread
 * paces itself to ~60 Hz, so there is nothing to phase-lock here.
 *
 * The frame is always ASTROSESSION_FB_WIDTH x ASTROSESSION_FB_HEIGHT
 * (352x240 -- the Astrocade data chip's visible raster), presented as a 4:3
 * picture (the console's pixels are not square).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "display.h"

#include <string.h>

#define FB_W ASTROSESSION_FB_WIDTH
#define FB_H ASTROSESSION_FB_HEIGHT

/* ---- frame buffer pool ----------------------------------------------------
 * The session copies a frame straight into a pooled buffer and the texture
 * borrows that buffer: one copy per frame, no allocation in steady state.
 * The buffer returns to the pool when GSK drops the texture's bytes, possibly
 * after the widget is gone, so the pool is reference counted and each
 * outstanding buffer holds a reference. */
typedef struct {
    gatomicrefcount refs;
    GMutex lock;
    GPtrArray *spare;      /* idle guint32 buffers, FB_W * FB_H each */
} FramePool;

typedef struct {
    FramePool *pool;
    guint32 *px;
} FrameLoan;

#define POOL_KEEP 4

static FramePool *pool_new(void)
{
    FramePool *p = g_new0(FramePool, 1);
    g_atomic_ref_count_init(&p->refs);
    g_mutex_init(&p->lock);
    p->spare = g_ptr_array_new_with_free_func(g_free);
    return p;
}

static void pool_unref(FramePool *p)
{
    if (!g_atomic_ref_count_dec(&p->refs))
        return;
    g_ptr_array_free(p->spare, TRUE);
    g_mutex_clear(&p->lock);
    g_free(p);
}

static guint32 *pool_take(FramePool *p)
{
    guint32 *px = NULL;

    g_mutex_lock(&p->lock);
    if (p->spare->len > 0)
        px = g_ptr_array_steal_index(p->spare, p->spare->len - 1);
    g_mutex_unlock(&p->lock);
    return px ? px : g_malloc((gsize)FB_W * FB_H * sizeof(guint32));
}

static void pool_give(FramePool *p, guint32 *px)
{
    g_mutex_lock(&p->lock);
    if (p->spare->len < POOL_KEEP) {
        g_ptr_array_add(p->spare, px);
        px = NULL;
    }
    g_mutex_unlock(&p->lock);
    g_free(px);
}

static void loan_release(gpointer data)
{
    FrameLoan *loan = data;

    pool_give(loan->pool, loan->px);
    pool_unref(loan->pool);
    g_free(loan);
}

struct _AstroDisplay {
    GtkWidget parent_instance;

    astrosession *session;
    FramePool *pool;
    GdkTexture *texture;
    uint64_t serial;
    guint tick_id;
    gulong suspended_id;   /* notify::suspended on the toplevel, while mapped */
    GtkWindow *toplevel;
};

G_DEFINE_FINAL_TYPE(AstroDisplay, astro_display, GTK_TYPE_WIDGET)

/* The core's XRGB8888 is 0x00RRGGBB, so on a little-endian host the bytes run
 * B,G,R,X == GDK_MEMORY_B8G8R8X8, no per-pixel conversion and the X byte is
 * ignored. */
#if G_BYTE_ORDER == G_BIG_ENDIAN
#define ASTRO_GDK_FORMAT GDK_MEMORY_X8R8G8B8
#else
#define ASTRO_GDK_FORMAT GDK_MEMORY_B8G8R8X8
#endif

/* Wrap px in the texture. The previous texture goes in as the update source
 * so GSK can reuse its GPU texture. Takes ownership of px. */
static void publish_texture(AstroDisplay *self, guint32 *px)
{
    GdkMemoryTextureBuilder *b = gdk_memory_texture_builder_new();
    FrameLoan *loan = g_new(FrameLoan, 1);
    GBytes *bytes;

    g_atomic_ref_count_inc(&self->pool->refs);
    loan->pool = self->pool;
    loan->px = px;
    bytes = g_bytes_new_with_free_func(px, (gsize)FB_W * FB_H * sizeof(guint32),
                                       loan_release, loan);
    gdk_memory_texture_builder_set_bytes(b, bytes);
    gdk_memory_texture_builder_set_stride(b, (gsize)FB_W * 4);
    gdk_memory_texture_builder_set_width(b, FB_W);
    gdk_memory_texture_builder_set_height(b, FB_H);
    gdk_memory_texture_builder_set_format(b, ASTRO_GDK_FORMAT);
    if (self->texture) {
        cairo_rectangle_int_t all = { 0, 0, FB_W, FB_H };
        cairo_region_t *region = cairo_region_create_rectangle(&all);

        gdk_memory_texture_builder_set_update_texture(b, self->texture);
        gdk_memory_texture_builder_set_update_region(b, region);
        cairo_region_destroy(region);
    }
    g_clear_object(&self->texture);
    self->texture = gdk_memory_texture_builder_build(b);
    g_bytes_unref(bytes);
    g_object_unref(b);
}

static gboolean tick_cb(GtkWidget *widget, GdkFrameClock *clock,
                        gpointer user_data)
{
    AstroDisplay *self = ASTRO_DISPLAY(widget);
    guint32 *px;
    (void)clock;
    (void)user_data;

    if (!self->session)
        return G_SOURCE_CONTINUE;

    px = pool_take(self->pool);
    if (astrosession_copy_frame(self->session, px, &self->serial)) {
        publish_texture(self, px);
        gtk_widget_queue_draw(widget);
    } else {
        pool_give(self->pool, px);
    }
    return G_SOURCE_CONTINUE;
}

/* The tick runs only while somebody can see the result. The emulator does not
 * care: it paces itself. The first tick after the window comes back copies
 * whatever frame is newest. */
static void update_tick(AstroDisplay *self)
{
    gboolean want = gtk_widget_get_mapped(GTK_WIDGET(self)) &&
                    !(self->toplevel && gtk_window_is_suspended(self->toplevel));

    if (want && !self->tick_id)
        self->tick_id = gtk_widget_add_tick_callback(GTK_WIDGET(self), tick_cb,
                                                     NULL, NULL);
    else if (!want && self->tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
        self->tick_id = 0;
    }
}

static void on_suspended(GObject *obj, GParamSpec *pspec, gpointer user_data)
{
    (void)obj; (void)pspec;
    update_tick(ASTRO_DISPLAY(user_data));
}

static void astro_display_map(GtkWidget *widget)
{
    AstroDisplay *self = ASTRO_DISPLAY(widget);
    GtkRoot *root;

    GTK_WIDGET_CLASS(astro_display_parent_class)->map(widget);
    root = gtk_widget_get_root(widget);
    if (GTK_IS_WINDOW(root)) {
        self->toplevel = GTK_WINDOW(root);
        self->suspended_id = g_signal_connect(self->toplevel, "notify::suspended",
                                              G_CALLBACK(on_suspended), self);
    }
    update_tick(self);
}

static void astro_display_unmap(GtkWidget *widget)
{
    AstroDisplay *self = ASTRO_DISPLAY(widget);

    if (self->toplevel && self->suspended_id)
        g_signal_handler_disconnect(self->toplevel, self->suspended_id);
    self->suspended_id = 0;
    self->toplevel = NULL;
    GTK_WIDGET_CLASS(astro_display_parent_class)->unmap(widget);
    update_tick(self);
}

static void astro_display_snapshot(GtkWidget *widget, GtkSnapshot *snapshot)
{
    AstroDisplay *self = ASTRO_DISPLAY(widget);
    const float w = (float)gtk_widget_get_width(widget);
    const float h = (float)gtk_widget_get_height(widget);
    const float aspect = 4.0f / 3.0f;
    float dw, dh;
    graphene_rect_t dest;

    gtk_snapshot_append_color(snapshot, &(GdkRGBA){0, 0, 0, 1},
                              &GRAPHENE_RECT_INIT(0, 0, w, h));
    if (!self->texture || w < 1 || h < 1)
        return;

    if (w / h > aspect) {
        dh = h;
        dw = h * aspect;
    } else {
        dw = w;
        dh = w / aspect;
    }

    dest = GRAPHENE_RECT_INIT((w - dw) / 2.0f, (h - dh) / 2.0f, dw, dh);
    gtk_snapshot_append_scaled_texture(snapshot, self->texture,
                                       GSK_SCALING_FILTER_NEAREST, &dest);
}

static void astro_display_dispose(GObject *object)
{
    AstroDisplay *self = ASTRO_DISPLAY(object);
    if (self->tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
        self->tick_id = 0;
    }
    g_clear_object(&self->texture);
    if (self->pool) {
        pool_unref(self->pool);
        self->pool = NULL;
    }
    G_OBJECT_CLASS(astro_display_parent_class)->dispose(object);
}

static void astro_display_class_init(AstroDisplayClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    widget_class->snapshot = astro_display_snapshot;
    widget_class->map = astro_display_map;
    widget_class->unmap = astro_display_unmap;
    object_class->dispose = astro_display_dispose;
}

static void astro_display_init(AstroDisplay *self)
{
    self->pool = pool_new();
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);
}

GtkWidget *astro_display_new(astrosession *session)
{
    AstroDisplay *self = g_object_new(ASTRO_TYPE_DISPLAY, NULL);
    self->session = session;
    return GTK_WIDGET(self);
}
