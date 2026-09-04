/*
 * AstroDisplay: paints the emulator's latest XRGB8888 frame with a
 * GdkMemoryTexture, letterboxed to 4:3 inside whatever size the window
 * manager gives us. A frame-clock tick pulls the latest frame on every redraw
 * opportunity. The emulator's host thread paces itself to ~60 Hz, so there is
 * nothing to phase-lock here.
 *
 * The frame is always ASTROSESSION_FB_WIDTH x ASTROSESSION_FB_HEIGHT
 * (352x240 -- the Astrocade data chip's visible raster), presented as a 4:3
 * picture (the console's pixels are not square).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "display.h"

struct _AstroDisplay {
    GtkWidget parent_instance;

    astrosession *session;
    GdkTexture *texture;
    uint32_t *fb;
    uint64_t serial;
    guint tick_id;
};

G_DEFINE_FINAL_TYPE(AstroDisplay, astro_display, GTK_TYPE_WIDGET)

/* The core's XRGB8888 is 0x00RRGGBB, so on a little-endian host the bytes run
 * B,G,R,X == GDK_MEMORY_B8G8R8X8, no per-pixel conversion. */
#if G_BYTE_ORDER == G_BIG_ENDIAN
#define ASTRO_GDK_FORMAT GDK_MEMORY_X8R8G8B8
#else
#define ASTRO_GDK_FORMAT GDK_MEMORY_B8G8R8X8
#endif

static gboolean tick_cb(GtkWidget *widget, GdkFrameClock *clock,
                        gpointer user_data)
{
    AstroDisplay *self = ASTRO_DISPLAY(widget);
    (void)clock;
    (void)user_data;

    if (!self->session)
        return G_SOURCE_CONTINUE;

    if (astrosession_copy_frame(self->session, self->fb, &self->serial)) {
        GBytes *bytes = g_bytes_new(
            self->fb,
            (gsize)ASTROSESSION_FB_WIDTH * ASTROSESSION_FB_HEIGHT *
                sizeof(uint32_t));
        g_clear_object(&self->texture);
        self->texture = gdk_memory_texture_new(
            ASTROSESSION_FB_WIDTH, ASTROSESSION_FB_HEIGHT, ASTRO_GDK_FORMAT,
            bytes, (gsize)ASTROSESSION_FB_WIDTH * sizeof(uint32_t));
        g_bytes_unref(bytes);
        gtk_widget_queue_draw(widget);
    }
    return G_SOURCE_CONTINUE;
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
    g_clear_pointer(&self->fb, g_free);
    G_OBJECT_CLASS(astro_display_parent_class)->dispose(object);
}

static void astro_display_class_init(AstroDisplayClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    widget_class->snapshot = astro_display_snapshot;
    object_class->dispose = astro_display_dispose;
}

static void astro_display_init(AstroDisplay *self)
{
    self->fb = g_malloc0((gsize)ASTROSESSION_FB_WIDTH *
                         ASTROSESSION_FB_HEIGHT * sizeof(uint32_t));
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);
    self->tick_id =
        gtk_widget_add_tick_callback(GTK_WIDGET(self), tick_cb, NULL, NULL);
}

GtkWidget *astro_display_new(astrosession *session)
{
    AstroDisplay *self = g_object_new(ASTRO_TYPE_DISPLAY, NULL);
    self->session = session;
    return GTK_WIDGET(self);
}
