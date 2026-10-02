/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "gui/wayland.h"

#ifdef HAVE_WAYLAND_COLOR_MANAGEMENT
#include "color-management-v1-client-protocol.h"
#include <gdk/gdkwayland.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define DISPLAY_KEY "dt-wayland-color-display"
#define CANVAS_KEY "dt-wayland-color-canvas"
#define WINDOW_KEY "dt-wayland-color-window"

typedef struct dt_wayland_display_t
{
  struct wl_compositor *compositor;
  struct wl_subcompositor *subcompositor;
  struct wl_shm *shm;
  struct wp_color_manager_v1 *manager;
  struct wp_image_description_v1 *description;
  struct wp_image_description_v1 *ui_description;
  guint map_signal;
  gulong map_hook;
  gboolean parametric, rec2020, bt709, gamma22, relative, done, ready, ui_ready, failed;
} dt_wayland_display_t;

typedef struct dt_wayland_window_t
{
  GtkWidget *widget;
  GdkDisplay *display;
  struct wp_color_management_surface_v1 *color;
  gulong unmap_handler, unrealize_handler, display_handler;
} dt_wayland_window_t;

typedef struct dt_wayland_buffer_t
{
  struct wl_buffer *buffer;
  struct dt_wayland_canvas_t *canvas;
  cairo_surface_t *image;
  void *data;
  size_t size;
  int width, height, stride;
  gboolean busy;
} dt_wayland_buffer_t;

typedef struct dt_wayland_canvas_t
{
  GtkWidget *widget;
  GdkDisplay *display;
  struct wl_surface *surface;
  struct wl_subsurface *subsurface;
  struct wp_color_management_surface_v1 *color;
  dt_wayland_buffer_t buffers[3];
  cairo_surface_t *previous;
  gulong unmap_handler, unrealize_handler, display_handler;
  unsigned int references;
  gboolean redraw_on_release, frame_active;
} dt_wayland_canvas_t;

typedef struct dt_wayland_frame_t
{
  dt_wayland_canvas_t *canvas;
  dt_wayland_buffer_t *buffer;
  cairo_t *cr;
  cairo_surface_t *snapshot;
  cairo_matrix_t origin;
  cairo_pattern_t *damage;
  int scale;
  gboolean painted;
} dt_wayland_frame_t;

static cairo_user_data_key_t _frame_key;
static gint _available;

static void _intent(void *data,
                     struct wp_color_manager_v1 *manager,
                     uint32_t intent)
{
  dt_wayland_display_t *d = data;
  if(intent == WP_COLOR_MANAGER_V1_RENDER_INTENT_RELATIVE) d->relative = TRUE;
}

static void _feature(void *data,
                      struct wp_color_manager_v1 *manager,
                      uint32_t feature)
{
  dt_wayland_display_t *d = data;
  if(feature == WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC) d->parametric = TRUE;
}

static void _transfer(void *data,
                       struct wp_color_manager_v1 *manager,
                       uint32_t transfer)
{
  dt_wayland_display_t *d = data;
  if(transfer == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22) d->gamma22 = TRUE;
}

static void _primaries(void *data,
                        struct wp_color_manager_v1 *manager,
                        uint32_t primaries)
{
  dt_wayland_display_t *d = data;
  if(primaries == WP_COLOR_MANAGER_V1_PRIMARIES_BT2020) d->rec2020 = TRUE;
  if(primaries == WP_COLOR_MANAGER_V1_PRIMARIES_SRGB) d->bt709 = TRUE;
}

static void _done(void *data,
                   struct wp_color_manager_v1 *manager)
{
  dt_wayland_display_t *d = data;
  d->done = TRUE;
}

static const struct wp_color_manager_v1_listener _manager_listener = {
  .supported_intent = _intent,
  .supported_feature = _feature,
  .supported_tf_named = _transfer,
  .supported_primaries_named = _primaries,
  .done = _done
};

static void _global(void *data,
                     struct wl_registry *registry,
                     uint32_t name,
                     const char *interface,
                     uint32_t version)
{
  dt_wayland_display_t *d = data;
  if(!strcmp(interface, wl_compositor_interface.name) && version >= 3)
    d->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 3);
  else if(!strcmp(interface, wl_subcompositor_interface.name))
    d->subcompositor = wl_registry_bind(registry, name, &wl_subcompositor_interface, 1);
  else if(!strcmp(interface, wl_shm_interface.name))
    d->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
  else if(!strcmp(interface, wp_color_manager_v1_interface.name))
  {
    d->manager = wl_registry_bind(registry, name, &wp_color_manager_v1_interface, 1);
    wp_color_manager_v1_add_listener(d->manager, &_manager_listener, d);
  }
}

static void _global_remove(void *data,
                            struct wl_registry *registry,
                            uint32_t name)
{
}

static const struct wl_registry_listener _registry_listener = {
  .global = _global,
  .global_remove = _global_remove
};

static void _ready(void *data,
                    struct wp_image_description_v1 *description,
                    uint32_t identity)
{
  dt_wayland_display_t *d = data;
  if(description == d->description) d->ready = TRUE;
  if(description == d->ui_description) d->ui_ready = TRUE;
}

static void _failed(void *data,
                     struct wp_image_description_v1 *description,
                     uint32_t cause,
                     const char *message)
{
  dt_wayland_display_t *d = data;
  d->failed = TRUE;
  g_debug("Wayland color description rejected: %s", message);
}

static const struct wp_image_description_v1_listener _description_listener = {
  .failed = _failed,
  .ready = _ready
};

static void _display_free(gpointer data)
{
  dt_wayland_display_t *d = data;
  if(d->ready) g_atomic_int_set(&_available, FALSE);
  if(d->map_hook) g_signal_remove_emission_hook(d->map_signal, d->map_hook);
  if(d->description) wp_image_description_v1_destroy(d->description);
  if(d->ui_description) wp_image_description_v1_destroy(d->ui_description);
  if(d->manager) wp_color_manager_v1_destroy(d->manager);
  if(d->shm) wl_shm_destroy(d->shm);
  if(d->subcompositor) wl_subcompositor_destroy(d->subcompositor);
  if(d->compositor) wl_compositor_destroy(d->compositor);
  g_free(d);
}

static void _display_closed(GdkDisplay *display,
                             gboolean is_error,
                             gpointer data)
{
  // the GDK backend disconnects before GObject destroys attached data
  g_object_set_data(G_OBJECT(display), DISPLAY_KEY, NULL);
}

static void _window_free(gpointer data)
{
  dt_wayland_window_t *w = data;
  if(g_signal_handler_is_connected(w->widget, w->unmap_handler))
    g_signal_handler_disconnect(w->widget, w->unmap_handler);
  if(g_signal_handler_is_connected(w->widget, w->unrealize_handler))
    g_signal_handler_disconnect(w->widget, w->unrealize_handler);
  g_signal_handler_disconnect(w->display, w->display_handler);
  wp_color_management_surface_v1_destroy(w->color);
  g_object_unref(w->display);
  g_free(w);
}

static void _window_unmap(GtkWidget *widget,
                            gpointer data)
{
  // GDK recreates the wl_surface after hiding a window, even without unrealizing it
  g_object_set_data(G_OBJECT(widget), WINDOW_KEY, NULL);
}

static void _window_display_closed(GdkDisplay *display,
                                    gboolean is_error,
                                    GtkWidget *widget)
{
  g_object_set_data(G_OBJECT(widget), WINDOW_KEY, NULL);
}

static gboolean _window_map(GSignalInvocationHint *hint,
                              guint n_values,
                              const GValue *values,
                              gpointer data)
{
  GtkWidget *widget = g_value_get_object(values);
  if(!GTK_IS_WINDOW(widget)) return TRUE;
  GdkDisplay *display = gtk_widget_get_display(widget);
  dt_wayland_display_t *d = g_object_get_data(G_OBJECT(display), DISPLAY_KEY);
  if(!d || !d->ready || !d->ui_ready
     || g_object_get_data(G_OBJECT(widget), WINDOW_KEY)) return TRUE;
  GdkWindow *window = gtk_widget_get_window(widget);
  if(!GDK_IS_WAYLAND_WINDOW(window)) return TRUE;
  struct wl_surface *surface = gdk_wayland_window_get_wl_surface(window);
  if(!surface) return TRUE;

  dt_wayland_window_t *w = g_malloc0(sizeof(*w));
  w->widget = widget;
  w->display = g_object_ref(display);
  w->color = wp_color_manager_v1_get_surface(d->manager, surface);
  wp_color_management_surface_v1_set_image_description
    (w->color, d->ui_description, WP_COLOR_MANAGER_V1_RENDER_INTENT_RELATIVE);
  w->unmap_handler = g_signal_connect(widget, "unmap", G_CALLBACK(_window_unmap), NULL);
  w->unrealize_handler = g_signal_connect(widget, "unrealize", G_CALLBACK(_window_unmap), NULL);
  w->display_handler =
    g_signal_connect(display, "closed", G_CALLBACK(_window_display_closed), widget);
  g_object_set_data_full(G_OBJECT(widget), WINDOW_KEY, w, _window_free);
  return TRUE;
}

static struct wp_image_description_v1 *_create_description(dt_wayland_display_t *d,
                                                             uint32_t primaries)
{
  struct wp_image_description_creator_params_v1 *creator =
    wp_color_manager_v1_create_parametric_creator(d->manager);
  wp_image_description_creator_params_v1_set_primaries_named(creator, primaries);
  wp_image_description_creator_params_v1_set_tf_named
    (creator, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22);
  struct wp_image_description_v1 *description =
    wp_image_description_creator_params_v1_create(creator);
  wp_image_description_v1_add_listener(description, &_description_listener, d);
  return description;
}

static gboolean _wait_for_descriptions(struct wl_display *connection,
                                       struct wl_event_queue *queue,
                                       dt_wayland_display_t *d)
{
  const gint64 deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
  while(TRUE)
  {
    if(wl_display_dispatch_queue_pending(connection, queue) < 0) return FALSE;
    if(d->failed) return FALSE;
    if(d->ready && d->ui_ready) return TRUE;
    const gint64 remaining = deadline - g_get_monotonic_time();
    if(remaining <= 0) return FALSE;
    if(wl_display_prepare_read_queue(connection, queue) < 0) continue;

    GPollFD fd = { .fd = wl_display_get_fd(connection), .events = G_IO_IN };
    if(wl_display_flush(connection) < 0)
    {
      if(errno != EAGAIN)
      {
        wl_display_cancel_read(connection);
        return FALSE;
      }
      fd.events |= G_IO_OUT;
    }
    const int status = g_poll(&fd, 1, (remaining + 999) / 1000);
    if(status > 0 && (fd.revents & G_IO_IN))
    {
      if(wl_display_read_events(connection) < 0) return FALSE;
    }
    else
    {
      wl_display_cancel_read(connection);
      if(status == 0 || (status < 0 && errno != EINTR)
         || (fd.revents & (G_IO_ERR | G_IO_HUP | G_IO_NVAL))) return FALSE;
    }
  }
}

void dt_wayland_color_init(GdkDisplay *display)
{
  if(display != gdk_display_get_default()
     || !GDK_IS_WAYLAND_DISPLAY(display)
     || gdk_display_is_closed(display)
     || g_object_get_data(G_OBJECT(display), DISPLAY_KEY)) return;

  dt_wayland_display_t *d = g_malloc0(sizeof(*d));
  struct wl_display *connection = gdk_wayland_display_get_wl_display(display);
  struct wl_event_queue *queue = wl_display_create_queue(connection);
  if(!queue)
  {
    g_free(d);
    return;
  }
  // a private queue avoids dispatching GTK callbacks during startup negotiation
  struct wl_proxy *wrapper = wl_proxy_create_wrapper(connection);
  if(!wrapper)
  {
    wl_event_queue_destroy(queue);
    g_free(d);
    return;
  }
  wl_proxy_set_queue(wrapper, queue);
  struct wl_registry *registry = wl_display_get_registry((struct wl_display *)wrapper);
  wl_proxy_wrapper_destroy(wrapper);
  wl_registry_add_listener(registry, &_registry_listener, d);
  const gboolean connected = wl_display_roundtrip_queue(connection, queue) >= 0
                             && wl_display_roundtrip_queue(connection, queue) >= 0;
  if(connected
     && d->compositor
     && d->subcompositor
     && d->shm
     && d->done
     && d->parametric
     && d->rec2020
     && d->bt709
     && d->gamma22
     && d->relative)
  {
    d->description = _create_description(d, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
    d->ui_description = _create_description(d, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB);
    // readiness may arrive after a sync reply
    if(!_wait_for_descriptions(connection, queue, d)) d->ready = FALSE;
  }
  // freeze the fallback before image processing starts
  if(!d->ready || !d->ui_ready)
  {
    if(d->description) wp_image_description_v1_destroy(d->description);
    if(d->ui_description) wp_image_description_v1_destroy(d->ui_description);
    d->description = NULL;
    d->ui_description = NULL;
    d->ready = d->ui_ready = FALSE;
  }
  wl_registry_destroy(registry);
  // the GTK main loop dispatches the default queue; workers own no Wayland objects
  if(d->description) wl_proxy_set_queue((struct wl_proxy *)d->description, NULL);
  if(d->ui_description) wl_proxy_set_queue((struct wl_proxy *)d->ui_description, NULL);
  if(d->manager) wl_proxy_set_queue((struct wl_proxy *)d->manager, NULL);
  if(d->shm) wl_proxy_set_queue((struct wl_proxy *)d->shm, NULL);
  if(d->subcompositor) wl_proxy_set_queue((struct wl_proxy *)d->subcompositor, NULL);
  if(d->compositor) wl_proxy_set_queue((struct wl_proxy *)d->compositor, NULL);
  wl_event_queue_destroy(queue);
  g_object_set_data_full(G_OBJECT(display), DISPLAY_KEY, d, _display_free);
  g_signal_connect(display, "closed", G_CALLBACK(_display_closed), NULL);
  if(d->ready)
  {
    // map hooks run after GTK creates its native window, including menus and tooltips
    d->map_signal = g_signal_lookup("map", GTK_TYPE_WIDGET);
    d->map_hook = g_signal_add_emission_hook(d->map_signal, 0, _window_map, NULL, NULL);
  }
  g_atomic_int_set(&_available, d->ready);
}

gboolean dt_wayland_color_available(void)
{
  return g_atomic_int_get(&_available);
}

void dt_wayland_color_prepare_window(GtkWidget *window)
{
  if(!dt_wayland_color_available()) return;
  GdkVisual *visual = gdk_screen_get_rgba_visual(gtk_widget_get_screen(window));
  if(visual) gtk_widget_set_visual(window, visual);
}

static void _buffer_free(dt_wayland_buffer_t *b)
{
  if(b->image) cairo_surface_destroy(b->image);
  if(b->buffer) wl_buffer_destroy(b->buffer);
  if(b->data) munmap(b->data, b->size);
  memset(b, 0, sizeof(*b));
}

static void _released(void *data,
                       struct wl_buffer *buffer)
{
  dt_wayland_buffer_t *b = data;
  b->busy = FALSE;
  dt_wayland_canvas_t *c = b->canvas;
  if(c->redraw_on_release && c->widget)
  {
    c->redraw_on_release = FALSE;
    gtk_widget_queue_draw(c->widget);
  }
}

static const struct wl_buffer_listener _buffer_listener = { .release = _released };

static gboolean _buffer_allocate(dt_wayland_buffer_t *b,
                                  struct wl_shm *shm,
                                  const int width,
                                  const int height)
{
  const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
  if(stride <= 0 || height <= 0 || height > INT_MAX / stride) return FALSE;
  if(b->width == width && b->height == height) return TRUE;
  _buffer_free(b);

  gchar *filename = NULL;
  const int fd = g_file_open_tmp("darktable-wayland-XXXXXX", &filename, NULL);
  if(fd < 0) return FALSE;
  g_unlink(filename);
  g_free(filename);
  const size_t size = (size_t)stride * height;
  if(ftruncate(fd, size) != 0)
  {
    close(fd);
    return FALSE;
  }
  void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if(data == MAP_FAILED)
  {
    close(fd);
    return FALSE;
  }
  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
  b->buffer = wl_shm_pool_create_buffer
    (pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy(pool);
  close(fd);
  b->data = data;
  b->size = size;
  b->width = width;
  b->height = height;
  b->stride = stride;
  b->image = cairo_image_surface_create_for_data
    (data, CAIRO_FORMAT_ARGB32, width, height, stride);
  if(cairo_surface_status(b->image) != CAIRO_STATUS_SUCCESS)
  {
    _buffer_free(b);
    return FALSE;
  }
  wl_buffer_add_listener(b->buffer, &_buffer_listener, b);
  return TRUE;
}

static void _canvas_unref(dt_wayland_canvas_t *c)
{
  if(--c->references) return;
  if(c->previous) cairo_surface_destroy(c->previous);
  for(int i = 0; i < 3; i++) _buffer_free(&c->buffers[i]);
  g_object_unref(c->display);
  g_free(c);
}

static void _canvas_free(gpointer data)
{
  dt_wayland_canvas_t *c = data;
  if(g_signal_handler_is_connected(c->widget, c->unmap_handler))
    g_signal_handler_disconnect(c->widget, c->unmap_handler);
  if(g_signal_handler_is_connected(c->widget, c->unrealize_handler))
    g_signal_handler_disconnect(c->widget, c->unrealize_handler);
  g_signal_handler_disconnect(c->display, c->display_handler);
  c->widget = NULL;
  // a frame may outlive display closure; release proxies before GDK disconnects
  for(int i = 0; i < 3; i++)
  {
    if(c->buffers[i].buffer) wl_buffer_destroy(c->buffers[i].buffer);
    c->buffers[i].buffer = NULL;
  }
  wp_color_management_surface_v1_destroy(c->color);
  wl_subsurface_destroy(c->subsurface);
  wl_surface_destroy(c->surface);
  c->color = NULL;
  c->subsurface = NULL;
  c->surface = NULL;
  _canvas_unref(c);
}

static void _canvas_unmap(GtkWidget *widget,
                           gpointer data)
{
  g_object_set_data(G_OBJECT(widget), CANVAS_KEY, NULL);
}

static void _canvas_display_closed(GdkDisplay *display,
                                    gboolean is_error,
                                    GtkWidget *widget)
{
  g_object_set_data(G_OBJECT(widget), CANVAS_KEY, NULL);
}

static dt_wayland_canvas_t *_canvas_new(GtkWidget *widget,
                                         dt_wayland_display_t *d,
                                         struct wl_surface *parent)
{
  dt_wayland_canvas_t *c = g_malloc0(sizeof(*c));
  c->widget = widget;
  c->display = g_object_ref(gtk_widget_get_display(widget));
  c->references = 1;
  c->surface = wl_compositor_create_surface(d->compositor);
  c->subsurface = wl_subcompositor_get_subsurface(d->subcompositor, c->surface, parent);
  // synchronized subsurfaces apply the image and GTK's transparent hole in one commit
  wl_subsurface_place_below(c->subsurface, parent);
  struct wl_region *empty = wl_compositor_create_region(d->compositor);
  wl_surface_set_input_region(c->surface, empty);
  wl_region_destroy(empty);
  c->color = wp_color_manager_v1_get_surface(d->manager, c->surface);
  wp_color_management_surface_v1_set_image_description
    (c->color, d->description, WP_COLOR_MANAGER_V1_RENDER_INTENT_RELATIVE);
  c->display_handler =
    g_signal_connect(c->display, "closed", G_CALLBACK(_canvas_display_closed), widget);
  c->unmap_handler = g_signal_connect(widget, "unmap", G_CALLBACK(_canvas_unmap), NULL);
  c->unrealize_handler = g_signal_connect(widget, "unrealize", G_CALLBACK(_canvas_unmap), NULL);
  g_object_set_data_full(G_OBJECT(widget), CANVAS_KEY, c, _canvas_free);
  return c;
}

static void _frame_free(void *data)
{
  dt_wayland_frame_t *f = data;
  cairo_destroy(f->cr);
  if(f->damage) cairo_pattern_destroy(f->damage);
  if(f->snapshot) cairo_surface_destroy(f->snapshot);
  f->canvas->frame_active = FALSE;
  _canvas_unref(f->canvas);
  g_free(f);
}

void dt_wayland_color_begin(GtkWidget *widget,
                             cairo_t *cr)
{
  GdkDisplay *display = gtk_widget_get_display(widget);
  dt_wayland_display_t *d = g_object_get_data(G_OBJECT(display), DISPLAY_KEY);
  if(!d || !d->ready || cairo_get_user_data(cr, &_frame_key)) return;
  GtkWidget *top = gtk_widget_get_toplevel(widget);
  if(!g_object_get_data(G_OBJECT(top), WINDOW_KEY)) return;
  GdkWindow *window = gtk_widget_get_window(top);
  if(!GDK_IS_WAYLAND_WINDOW(window)
     || gdk_visual_get_depth(gdk_window_get_visual(window)) != 32) return;
  struct wl_surface *parent = gdk_wayland_window_get_wl_surface(window);
  if(!parent) return;
  const int scale = gtk_widget_get_scale_factor(widget);
  const int width = gtk_widget_get_allocated_width(widget);
  const int height = gtk_widget_get_allocated_height(widget);
  if(width <= 0 || height <= 0
     || width > INT_MAX / scale || height > INT_MAX / scale) return;

  dt_wayland_canvas_t *c = g_object_get_data(G_OBJECT(widget), CANVAS_KEY);
  if(!c) c = _canvas_new(widget, d, parent);
  // nested GTK draws can use another Cairo context for the same canvas
  if(c->frame_active) return;
  dt_wayland_buffer_t *b = NULL;
  for(int i = 0; i < 3; i++)
    if(!c->buffers[i].busy)
    {
      b = &c->buffers[i];
      break;
    }
  // if the compositor still owns all buffers, let the caller draw its BT709 gamma22 fallback
  if(!b)
  {
    c->redraw_on_release = TRUE;
    return;
  }
  if(!_buffer_allocate(b, d->shm, width * scale, height * scale)) return;
  b->canvas = c;

  // allocation must succeed before image painting makes the GTK parent transparent
  cairo_surface_t *snapshot = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, b->width, b->height);
  if(cairo_surface_status(snapshot) != CAIRO_STATUS_SUCCESS)
  {
    cairo_surface_destroy(snapshot);
    return;
  }
  cairo_surface_set_device_scale(snapshot, scale, scale);
  cairo_surface_set_device_scale(b->image, scale, scale);
  dt_wayland_frame_t *f = g_malloc0(sizeof(*f));
  f->canvas = c;
  c->references++;
  c->frame_active = TRUE;
  f->buffer = b;
  f->scale = scale;
  f->snapshot = snapshot;
  f->cr = cairo_create(b->image);
  if(cairo_status(f->cr) != CAIRO_STATUS_SUCCESS)
  {
    _frame_free(f);
    return;
  }
  cairo_get_matrix(cr, &f->origin);
  cairo_save(cr);
  cairo_push_group(cr);
  cairo_set_source_rgba(cr, 1, 1, 1, 1);
  cairo_paint(cr);
  f->damage = cairo_pop_group(cr);
  cairo_restore(cr);
  if(cairo_set_user_data(cr, &_frame_key, f, _frame_free) != CAIRO_STATUS_SUCCESS)
  {
    _frame_free(f);
    return;
  }
  cairo_set_operator(f->cr, CAIRO_OPERATOR_SOURCE);
  if(c->previous)
    cairo_set_source_surface(f->cr, c->previous, 0, 0);
  else
    cairo_set_source_rgba(f->cr, 0, 0, 0, 0);
  cairo_paint(f->cr);
  cairo_set_operator(f->cr, CAIRO_OPERATOR_OVER);

  double sx, sy, dx, dy;
  cairo_surface_get_device_scale(cairo_get_target(cr), &sx, &sy);
  cairo_surface_get_device_offset(cairo_get_target(cr), &dx, &dy);
  wl_subsurface_set_position(c->subsurface, lround(f->origin.x0 + dx / sx),
                                          lround(f->origin.y0 + dy / sy));
  // an opaque parent would let the compositor discard the image beneath it
  cairo_region_t *empty = cairo_region_create();
  gdk_window_set_opaque_region(window, empty);
  cairo_region_destroy(empty);
}

gboolean dt_wayland_color_paint(cairo_t *cr)
{
  dt_wayland_frame_t *f = cairo_get_user_data(cr, &_frame_key);
  if(!f || !f->canvas->widget) return FALSE;

  if(!f->painted)
  {
    // retain pixels outside GTK's damage clip, including cached preview content
    cairo_save(f->cr);
    cairo_set_operator(f->cr, CAIRO_OPERATOR_DEST_OUT);
    cairo_set_source(f->cr, f->damage);
    cairo_paint(f->cr);
    cairo_restore(f->cr);
  }

  // the group captures the caller's transform and clip without approximating its shape
  cairo_push_group(cr);
  cairo_paint(cr);
  cairo_pattern_t *image = cairo_pop_group(cr);
  cairo_matrix_t local, inverse = f->origin;
  if(cairo_matrix_invert(&inverse) != CAIRO_STATUS_SUCCESS)
  {
    cairo_pattern_destroy(image);
    return FALSE;
  }
  cairo_get_matrix(cr, &local);
  cairo_matrix_multiply(&local, &local, &inverse);
  cairo_save(f->cr);
  cairo_set_matrix(f->cr, &local);
  cairo_set_source(f->cr, image);
  cairo_paint(f->cr);
  cairo_restore(f->cr);

  // only image coverage becomes transparent; overlays stay on the GTK surface
  cairo_save(cr);
  cairo_set_source(cr, image);
  cairo_reset_clip(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_DEST_OUT);
  cairo_paint(cr);
  cairo_restore(cr);
  cairo_pattern_destroy(image);
  f->painted = TRUE;
  return TRUE;
}

gboolean dt_wayland_color_repaint(cairo_t *cr)
{
  dt_wayland_frame_t *f = cairo_get_user_data(cr, &_frame_key);
  if(!f || !f->canvas->previous) return FALSE;
  cairo_save(cr);
  cairo_set_matrix(cr, &f->origin);
  cairo_set_source_surface(cr, f->canvas->previous, 0, 0);
  const gboolean painted = dt_wayland_color_paint(cr);
  cairo_restore(cr);
  return painted;
}

void dt_wayland_color_end(cairo_t *cr)
{
  dt_wayland_frame_t *f = cairo_get_user_data(cr, &_frame_key);
  if(!f) return;
  dt_wayland_canvas_t *c = f->canvas;
  dt_wayland_buffer_t *b = f->buffer;
  if(!c->widget)
  {
    cairo_set_user_data(cr, &_frame_key, NULL, NULL);
    return;
  }
  cairo_surface_flush(b->image);
  // keep a private copy: a released compositor buffer may be reused by another frame
  cairo_surface_flush(f->snapshot);
  memcpy(cairo_image_surface_get_data(f->snapshot), b->data, b->size);
  cairo_surface_mark_dirty(f->snapshot);
  if(c->previous) cairo_surface_destroy(c->previous);
  c->previous = f->snapshot;
  f->snapshot = NULL;

  // coverage is carried by the hole in GTK; applying child alpha again would square it
  cairo_surface_flush(c->previous);
  const uint32_t *const restrict input =
    (const uint32_t *)cairo_image_surface_get_data(c->previous);
  uint32_t *const restrict output = b->data;
  const size_t pixels = b->size / sizeof(*output);
  for(size_t k = 0; k < pixels; k++)
  {
    const uint32_t pixel = input[k];
    const uint32_t alpha = pixel >> 24;
    if(alpha && alpha != 255)
    {
      const uint32_t red = MIN(255u, (((pixel >> 16) & 255u) * 255u + alpha / 2) / alpha);
      const uint32_t green = MIN(255u, (((pixel >> 8) & 255u) * 255u + alpha / 2) / alpha);
      const uint32_t blue = MIN(255u, ((pixel & 255u) * 255u + alpha / 2) / alpha);
      output[k] = 0xff000000u | (red << 16) | (green << 8) | blue;
    }
    else
      output[k] = pixel | 0xff000000u;
  }
  cairo_surface_mark_dirty(b->image);
  wl_surface_set_buffer_scale(c->surface, f->scale);
  wl_surface_attach(c->surface, b->buffer, 0, 0);
  wl_surface_damage(c->surface, 0, 0, b->width / f->scale, b->height / f->scale);
  b->busy = TRUE;
  wl_surface_commit(c->surface);
  cairo_set_user_data(cr, &_frame_key, NULL, NULL);
}

#else

void dt_wayland_color_init(GdkDisplay *display)
{
}
gboolean dt_wayland_color_available(void)
{
  return FALSE;
}
void dt_wayland_color_prepare_window(GtkWidget *window)
{
}
void dt_wayland_color_begin(GtkWidget *widget,
                             cairo_t *cr)
{
}
gboolean dt_wayland_color_paint(cairo_t *cr)
{
  return FALSE;
}
gboolean dt_wayland_color_repaint(cairo_t *cr)
{
  return FALSE;
}
void dt_wayland_color_end(cairo_t *cr)
{
}

#endif
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
