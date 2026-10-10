/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable. If not, see <http://www.gnu.org/licenses/>.
*/

#include <gtk/gtk.h>

static gboolean _fail_image;
static cairo_surface_t *_create_image(cairo_format_t format, int width, int height);
#define cairo_image_surface_create _create_image
#include "gui/wayland.c"
#undef cairo_image_surface_create

#include <stdio.h>
#include <stdlib.h>

static GtkWidget *_window, *_areas[2];
static unsigned int _frames, _managed, _alpha_checks, _partial_checks;
static unsigned int _reentrant_checks, _allocation_checks, _repaint_checks, _unpainted_checks;
static unsigned int _stage, _waits;
static gboolean _partial, _unmap_during_draw, _unmap_checked;
static gboolean _unmap_image_released;
static gboolean _close_image_released;
static cairo_user_data_key_t _image_key;

static void _image_released(void *data)
{
  *(gboolean *)data = TRUE;
}

static cairo_surface_t *_create_image(cairo_format_t format, int width, int height)
{
  if(_fail_image)
  {
    _fail_image = FALSE;
    return cairo_image_surface_create(format, -1, -1);
  }
  return cairo_image_surface_create(format, width, height);
}

static void _check(const int condition, const char *message)
{
  if(!condition)
  {
    fprintf(stderr, "Wayland color test: %s\n", message);
    exit(EXIT_FAILURE);
  }
}

static uint32_t _pixel(cairo_surface_t *surface, const int x, const int y)
{
  _check(cairo_surface_get_type(surface) == CAIRO_SURFACE_TYPE_IMAGE,
         "expected an image surface for pixel validation");
  const int width = cairo_image_surface_get_width(surface);
  const int height = cairo_image_surface_get_height(surface);
  if(x < 0 || y < 0 || x >= width || y >= height)
    fprintf(stderr, "pixel (%d,%d) outside %dx%d at stage %u, frame %u, partial %d\n",
            x, y, width, height, _stage, _frames, _partial);
  _check(x >= 0 && y >= 0 && x < width && y < height, "pixel coordinate outside image surface");
  cairo_surface_flush(surface);
  const int stride = cairo_image_surface_get_stride(surface);
  const uint32_t *row = (const uint32_t *)(cairo_image_surface_get_data(surface) + (size_t)y * stride);
  return row[x];
}

static uint32_t _buffer_pixel(const dt_wayland_buffer_t *buffer, const int x, const int y)
{
  _check(x >= 0 && y >= 0 && x < buffer->width && y < buffer->height,
         "pixel coordinate outside submitted buffer");
  const uint32_t *row = (const uint32_t *)((const uint8_t *)buffer->data + (size_t)y * buffer->stride);
  return row[x];
}

static uint32_t _parent_pixel(cairo_t *cr, double x, double y)
{
  cairo_surface_t *surface = cairo_get_group_target(cr);
  double sx, sy, dx, dy;
  cairo_surface_get_device_scale(surface, &sx, &sy);
  cairo_surface_get_device_offset(surface, &dx, &dy);
  cairo_user_to_device(cr, &x, &y);
  return _pixel(surface, floor(x * sx + dx), floor(y * sy + dy));
}

static void _check_fallback(cairo_t *cr)
{
  cairo_pattern_t *source = cairo_get_source(cr);
  cairo_matrix_t before, after;
  cairo_get_matrix(cr, &before);
  const cairo_operator_t operator = cairo_get_operator(cr);
  _check(!dt_wayland_color_paint(cr), "unavailable layer accepted image pixels");
  cairo_get_matrix(cr, &after);
  _check(source == cairo_get_source(cr) && operator == cairo_get_operator(cr)
         && !memcmp(&before, &after, sizeof(before)), "fallback changed the caller's Cairo state");
}

static void _check_reentrant(GtkWidget *widget, cairo_t *cr, dt_wayland_frame_t *frame)
{
  dt_wayland_canvas_t *canvas = frame->canvas;
  const unsigned int references = canvas->references;
  cairo_surface_t *image = canvas->image;
  cairo_t *nested = cairo_create(cairo_get_target(cr));
  dt_wayland_color_begin(widget, nested);
  _check(cairo_get_user_data(nested, &_frame_key) == NULL,
         "nested draw reserved the outer frame's canvas");
  _check(canvas->frame_active && canvas->references == references
         && canvas->image == image, "nested draw modified the outer frame");
  _check_fallback(nested);
  dt_wayland_color_end(nested);
  _check(canvas->frame_active, "nested end released the outer frame's reservation");
  cairo_destroy(nested);
  _reentrant_checks++;
}

static void _check_allocation_failure(GtkWidget *widget, dt_wayland_canvas_t *canvas)
{
  cairo_surface_t *target = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 32, 32);
  cairo_t *cr = cairo_create(target);
  cairo_set_source_rgb(cr, 0.4, 0.5, 0.6);
  cairo_paint(cr);
  const uint32_t before = _pixel(target, 0, 0);
  // detach the retained image so that the next painted frame has to allocate one
  cairo_surface_t *image = canvas->image;
  canvas->image = NULL;
  _fail_image = TRUE;
  dt_wayland_color_begin(widget, cr);
  _check(cairo_get_user_data(cr, &_frame_key) != NULL, "begin did not reserve the canvas");
  _check_fallback(cr);
  _check(!_fail_image && !canvas->image, "failed image allocation was not attempted or kept");
  dt_wayland_color_end(cr);
  _check(cairo_get_user_data(cr, &_frame_key) == NULL
         && !canvas->frame_active && canvas->references == 1,
         "failed image allocation changed canvas ownership");
  _check(_pixel(target, 0, 0) == before, "failed image allocation changed GTK image coverage");
  canvas->image = image;
  cairo_destroy(cr);
  cairo_surface_destroy(target);
  _allocation_checks++;
}

static void _check_unpainted(GtkWidget *widget, dt_wayland_canvas_t *canvas)
{
  cairo_surface_t *target = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 32, 32);
  cairo_t *cr = cairo_create(target);
  gboolean busy[3];
  for(int i = 0; i < 3; i++) busy[i] = canvas->buffers[i].busy;
  cairo_surface_t *image = canvas->image;
  const uint32_t before = _pixel(image, 0, 0);
  dt_wayland_color_begin(widget, cr);
  _check(cairo_get_user_data(cr, &_frame_key) != NULL, "begin did not reserve the canvas");
  dt_wayland_color_end(cr);
  for(int i = 0; i < 3; i++)
    _check(canvas->buffers[i].busy == busy[i], "a frame without image submitted a buffer");
  _check(canvas->image == image && _pixel(image, 0, 0) == before
         && !canvas->frame_active && canvas->references == 1,
         "a frame without image changed the canvas");
  cairo_destroy(cr);
  cairo_surface_destroy(target);
  _unpainted_checks++;
}

static void _check_repaint(GtkWidget *widget, dt_wayland_canvas_t *canvas)
{
  const int scale = gtk_widget_get_scale_factor(widget);
  const int width = gtk_widget_get_allocated_width(widget);
  const int height = gtk_widget_get_allocated_height(widget);
  // a resize replaces the retained image in the next frame
  if(cairo_image_surface_get_width(canvas->image) != width * scale
     || cairo_image_surface_get_height(canvas->image) != height * scale)
    return;
  cairo_surface_t *target = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                       width * scale, height * scale);
  cairo_surface_set_device_scale(target, scale, scale);
  cairo_t *cr = cairo_create(target);
  const int points[2][2] = { { width / 2 * scale, height / 2 * scale }, { 25 * scale, 25 * scale } };
  uint32_t before[2];
  for(int i = 0; i < 2; i++) before[i] = _pixel(canvas->image, points[i][0], points[i][1]);
  dt_wayland_color_begin(widget, cr);
  if(cairo_get_user_data(cr, &_frame_key))
  {
    _check(dt_wayland_color_repaint(cr), "repaint refused the retained image");
    for(int i = 0; i < 2; i++)
      _check(_pixel(canvas->image, points[i][0], points[i][1]) == before[i],
             "repaint did not restore the retained image");
    // end the frame without submitting a buffer to the compositor
    GtkWidget *owner = canvas->widget;
    canvas->widget = NULL;
    dt_wayland_color_end(cr);
    canvas->widget = owner;
    _repaint_checks++;
  }
  cairo_destroy(cr);
  cairo_surface_destroy(target);
}

static gboolean _draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
  _frames++;
  const int width = gtk_widget_get_allocated_width(widget);
  const int height = gtk_widget_get_allocated_height(widget);
  const int scale = gtk_widget_get_scale_factor(widget);
  const double edge = 25.0 + 0.5 / scale;
  cairo_save(cr);
  if(_partial && widget == _areas[0])
  {
    // transparent GTK windows may expand invalidation to the whole widget
    cairo_rectangle(cr, 30, 30, 20, 20);
    cairo_clip(cr);
  }
  const gboolean partial_clip = _partial && widget == _areas[0]
                                && cairo_in_clip(cr, 40, 40)
                                && !cairo_in_clip(cr, width - 40, height - 40);
  dt_wayland_canvas_t *old_canvas = g_object_get_data(G_OBJECT(widget), CANVAS_KEY);
  uint32_t unchanged = 0;
  if(partial_clip && old_canvas && old_canvas->image)
    unchanged = _pixel(old_canvas->image, (width - 40) * scale, (height - 40) * scale);

  // the offscreen frame moves the child surface; the real frame below moves it back
  if(old_canvas && old_canvas->image) _check_repaint(widget, old_canvas);

  cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
  cairo_paint(cr);
  dt_wayland_color_begin(widget, cr);
  dt_wayland_frame_t *frame = cairo_get_user_data(cr, &_frame_key);
  if(!frame)
  {
    _check_fallback(cr);
    cairo_restore(cr);
    return TRUE;
  }
  _check(frame->canvas->frame_active && !frame->buffer && !frame->cr,
         "begin did not defer buffer work to the first image");
  _check_reentrant(widget, cr, frame);

  if(_unmap_during_draw && widget == _areas[0])
  {
    _unmap_during_draw = FALSE;
    dt_wayland_canvas_t *canvas = frame->canvas;
    canvas->references++;
    cairo_set_source_rgb(cr, 0.8, 0.1, 0.1);
    _check(dt_wayland_color_paint(cr), "ready frame refused an image before unmapping");
    _check(cairo_surface_set_user_data(canvas->image, &_image_key,
                                      &_unmap_image_released, _image_released)
           == CAIRO_STATUS_SUCCESS, "failed to observe image lifetime");
    gtk_widget_hide(widget);
    _check(canvas->widget == NULL && canvas->references == 2
           && !_unmap_image_released,
           "unmap did not detach the canvas while retaining the frame");
    _check_fallback(cr);
    dt_wayland_color_end(cr);
    _check(cairo_get_user_data(cr, &_frame_key) == NULL && canvas->references == 1
           && !canvas->frame_active && !_unmap_image_released,
           "unmapped frame retained its reservation or released the held canvas");
    _canvas_unref(canvas);
    _check(_unmap_image_released, "unmapped canvas retained its image");
    _unmap_checked = TRUE;
    cairo_restore(cr);
    return TRUE;
  }

  cairo_save(cr);
  cairo_rectangle(cr, edge, edge, width - 2 * edge, height - 2 * edge);
  cairo_clip(cr);
  cairo_translate(cr, 20, 10);
  cairo_scale(cr, 0.9, 0.8);
  if(partial_clip) cairo_set_source_rgb(cr, 0.1, 0.8, 0.1);
  else cairo_set_source_rgb(cr, 0.8, 0.1, 0.1);
  _check(dt_wayland_color_paint(cr), "ready frame refused an image");
  _managed++;
  cairo_restore(cr);

  const gboolean edge_in_clip = cairo_in_clip(cr, 25.0 + 0.1 / scale,
                                             25.0 + 0.1 / scale);
  if(edge_in_clip)
  {
    const unsigned int alpha = _parent_pixel(cr, 25.0 + 0.1 / scale,
                                             25.0 + 0.1 / scale) >> 24;
    _check(alpha >= 189 && alpha <= 193, "parent clip applied fractional image coverage twice");
    _alpha_checks++;
  }

  dt_wayland_canvas_t *canvas = frame->canvas;
  dt_wayland_buffer_t *buffer = frame->buffer;
  dt_wayland_color_end(cr);
  _check(cairo_get_user_data(cr, &_frame_key) == NULL && canvas->references == 1
         && !canvas->frame_active,
         "frame end failed to release its canvas reference");
  _check(buffer->busy, "submitted buffer is immediately reusable");
  if(edge_in_clip)
  {
    const int x = 25 * scale, y = 25 * scale;
    const unsigned int alpha = _pixel(canvas->image, x, y) >> 24;
    _check(alpha >= 62 && alpha <= 66, "retained image did not preserve premultiplied edge coverage");
    const uint32_t submitted = _buffer_pixel(buffer, x, y);
    _check((submitted >> 24) == 255, "submitted child buffer applies image alpha a second time");
    _check(abs((int)((submitted >> 16) & 255) - 204) <= 3,
           "submitted edge RGB was not unpremultiplied");
  }
  if(partial_clip && unchanged)
  {
    _check(_pixel(canvas->image, (width - 40) * scale, (height - 40) * scale) == unchanged,
           "partial redraw discarded pixels outside the damage region");
    const uint32_t changed = _pixel(canvas->image, 40 * scale, 40 * scale);
    _check(((changed >> 8) & 255) > ((changed >> 16) & 255),
           "partial redraw did not replace pixels inside the damage region");
    _partial_checks++;
  }
  _check_allocation_failure(widget, canvas);
  _check_unpainted(widget, canvas);

  // compositor ownership must produce a fallback without mutating an in-flight buffer
  gboolean busy[3];
  const gboolean redraw_on_release = canvas->redraw_on_release;
  for(int i = 0; i < 3; i++)
  {
    busy[i] = canvas->buffers[i].busy;
    canvas->buffers[i].busy = TRUE;
  }
  canvas->redraw_on_release = FALSE;
  dt_wayland_color_begin(widget, cr);
  _check_fallback(cr);
  _check(canvas->redraw_on_release, "busy-buffer fallback did not request a retry on release");
  dt_wayland_color_end(cr);
  _check(cairo_get_user_data(cr, &_frame_key) == NULL && !canvas->frame_active,
         "busy-buffer fallback retained its frame");
  for(int i = 0; i < 3; i++) canvas->buffers[i].busy = busy[i];
  canvas->redraw_on_release = redraw_on_release;
  cairo_restore(cr);

  cairo_set_source_rgb(cr, 0.0, 1.0, 0.0);
  cairo_set_line_width(cr, 3.0);
  cairo_move_to(cr, 25, 25);
  cairo_line_to(cr, width - 25, height - 25);
  cairo_stroke(cr);
  return TRUE;
}

static gboolean _tick(gpointer data)
{
  if(_stage == 0 && _managed < 2)
  {
    _check(++_waits < 30, "initial frames were never presented");
    return G_SOURCE_CONTINUE;
  }
  if(_stage == 1)
  {
    gboolean resized = gtk_widget_get_allocated_width(_window) == 700
                       && gtk_widget_get_allocated_height(_window) == 450;
    for(int i = 0; i < 2; i++)
    {
      dt_wayland_canvas_t *canvas = g_object_get_data(G_OBJECT(_areas[i]), CANVAS_KEY);
      const int scale = gtk_widget_get_scale_factor(_areas[i]);
      resized &= canvas && canvas->image
                 && cairo_image_surface_get_width(canvas->image)
                    == gtk_widget_get_allocated_width(_areas[i]) * scale
                 && cairo_image_surface_get_height(canvas->image)
                    == gtk_widget_get_allocated_height(_areas[i]) * scale;
      if(!resized) gtk_widget_queue_draw(_areas[i]);
    }
    if(!resized)
    {
      // GTK allocation can precede the first resized frame under a slow compositor
      _check(++_waits < 30, "resized frames were never presented");
      return G_SOURCE_CONTINUE;
    }
  }
  if(_stage == 2 && !_partial_checks)
  {
    _check(++_waits < 30, "partial redraw was never exercised");
    gtk_widget_queue_draw_area(_areas[0], 30, 30, 20, 20);
    return G_SOURCE_CONTINUE;
  }
  if(_stage == 6 && !_unmap_checked)
  {
    _check(++_waits < 30, "unmap during drawing was never exercised");
    gtk_widget_queue_draw(_areas[0]);
    return G_SOURCE_CONTINUE;
  }
  _waits = 0;
  switch(_stage++)
  {
    case 0:
    {
      dt_wayland_canvas_t *first = g_object_get_data(G_OBJECT(_areas[0]), CANVAS_KEY);
      dt_wayland_canvas_t *second = g_object_get_data(G_OBJECT(_areas[1]), CANVAS_KEY);
      _check(first && second && first->surface != second->surface,
             "canvases do not own independent child surfaces");
      _check(g_object_get_data(G_OBJECT(_window), WINDOW_KEY) != NULL,
             "image canvases have no managed GTK parent");
      GtkWidget *popup = gtk_window_new(GTK_WINDOW_POPUP);
      gtk_window_set_transient_for(GTK_WINDOW(popup), GTK_WINDOW(_window));
      gtk_widget_show(popup);
      _check(g_object_get_data(G_OBJECT(popup), WINDOW_KEY) != NULL,
             "popup has no UI color description");
      gtk_widget_hide(popup);
      _check(g_object_get_data(G_OBJECT(popup), WINDOW_KEY) == NULL,
             "hidden popup retained a dead surface binding");
      gtk_widget_show(popup);
      _check(g_object_get_data(G_OBJECT(popup), WINDOW_KEY) != NULL,
             "remapped popup has no UI color description");
      gtk_widget_destroy(popup);
      gtk_window_resize(GTK_WINDOW(_window), 700, 450);
      break;
    }
    case 1:
      _partial = TRUE;
      gtk_widget_queue_draw_area(_areas[0], 30, 30, 20, 20);
      break;
    case 2:
      _partial = FALSE;
      gtk_widget_hide(_window);
      _check(g_object_get_data(G_OBJECT(_window), WINDOW_KEY) == NULL,
             "hidden window retained its parent color binding");
      for(int i = 0; i < 2; i++)
        _check(g_object_get_data(G_OBJECT(_areas[i]), CANVAS_KEY) == NULL,
               "hiding a window retained its native canvas");
      break;
    case 3:
      gtk_widget_show_all(_window);
      break;
    case 4:
      _check(g_object_get_data(G_OBJECT(_window), WINDOW_KEY) != NULL,
             "remapped window has no parent color binding");
      _check(g_object_get_data(G_OBJECT(_areas[0]), CANVAS_KEY) != NULL,
             "showing the window did not recreate its native canvas");
      _unmap_during_draw = TRUE;
      gtk_widget_queue_draw(_areas[0]);
      break;
    case 5:
      break;
    case 6:
      _check(g_object_get_data(G_OBJECT(_window), WINDOW_KEY) != NULL,
             "unmapping a canvas removed the shared parent color binding");
      gtk_widget_show(_areas[0]);
      break;
    default:
      _check(_alpha_checks > 0 && _unmap_checked && _partial_checks > 0
             && _reentrant_checks > 0 && _allocation_checks > 0 && _repaint_checks > 0
             && _unpainted_checks > 0,
             "not all rendering and lifecycle checks ran");
      gtk_main_quit();
      return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static void _realize(GtkWidget *widget, gpointer data)
{
  cairo_region_t *empty = cairo_region_create();
  gtk_widget_input_shape_combine_region(widget, empty);
  cairo_region_destroy(empty);
}

static void _check_close_with_frame(GdkDisplay *display)
{
  const int scale = gtk_widget_get_scale_factor(_areas[0]);
  cairo_surface_t *target = cairo_image_surface_create
    (CAIRO_FORMAT_ARGB32, gtk_widget_get_allocated_width(_areas[0]) * scale,
     gtk_widget_get_allocated_height(_areas[0]) * scale);
  cairo_surface_set_device_scale(target, scale, scale);
  cairo_t *cr = cairo_create(target);
  dt_wayland_color_begin(_areas[0], cr);
  dt_wayland_frame_t *frame = cairo_get_user_data(cr, &_frame_key);
  _check(frame != NULL, "display-close test failed to reserve a frame");
  dt_wayland_canvas_t *canvas = frame->canvas;
  canvas->references++;
  cairo_set_source_rgb(cr, 0.8, 0.1, 0.1);
  _check(dt_wayland_color_paint(cr), "display-close test failed to paint an image");
  _check(cairo_surface_set_user_data(canvas->image, &_image_key,
                                    &_close_image_released, _image_released)
         == CAIRO_STATUS_SUCCESS, "failed to observe display-close image lifetime");

  gdk_display_close(display);
  _check(!dt_wayland_color_available()
         && g_object_get_data(G_OBJECT(display), DISPLAY_KEY) == NULL
         && g_object_get_data(G_OBJECT(_window), WINDOW_KEY) == NULL,
         "closed display retained protocol objects or availability");
  _check(canvas->widget == NULL && canvas->references == 2
         && !canvas->color && !canvas->subsurface && !canvas->surface,
         "held frame retained native proxies after display closure");
  for(int i = 0; i < 3; i++)
    _check(canvas->buffers[i].buffer == NULL,
           "held frame retained a buffer proxy after display closure");
  _check(!_close_image_released && frame->buffer->data,
         "display closure released storage still owned by the frame");
  _check_fallback(cr);
  dt_wayland_color_end(cr);
  _check(cairo_get_user_data(cr, &_frame_key) == NULL && canvas->references == 1
         && !canvas->frame_active && !_close_image_released,
         "closed display frame retained its reservation or released the held canvas");
  cairo_destroy(cr);
  cairo_surface_destroy(target);
  _canvas_unref(canvas);
  _check(_close_image_released, "closed display canvas retained its image");
  // GTK widget destruction needs a live display; this process exits after the close check
}

int main(int argc, char **argv)
{
  if(!gtk_init_check(&argc, &argv))
  {
    puts("skipped: no GTK display available");
    return 77;
  }
  GdkDisplay *display = gdk_display_get_default();
  dt_wayland_color_init(display);
  if(!dt_wayland_color_available())
  {
    puts("skipped: compositor does not support the required Wayland color description");
    return 77;
  }
  dt_wayland_display_t *state = g_object_get_data(G_OBJECT(display), DISPLAY_KEY);
  _check(state && state->ready && state->description && state->ui_ready && state->ui_description,
         "availability published a pending image or UI description");
  dt_wayland_color_init(display);
  _check(g_object_get_data(G_OBJECT(display), DISPLAY_KEY) == state,
         "repeated initialization replaced the display state");
  if(argc == 2 && !strcmp(argv[1], "--disable"))
  {
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_widget_show(window);
    _check(g_object_get_data(G_OBJECT(window), WINDOW_KEY) != NULL,
           "startup window has no color binding");
    dt_wayland_color_disable();
    dt_wayland_color_disable();
    dt_wayland_color_init(display);
    _check(!dt_wayland_color_available() && !state->map_hook
           && !state->ready && !state->ui_ready
           && !g_object_get_data(G_OBJECT(window), WINDOW_KEY),
           "disabled startup retained color management");
    gtk_widget_hide(window);
    gtk_widget_show(window);
    _check(!g_object_get_data(G_OBJECT(window), WINDOW_KEY),
           "remapping restored disabled color management");
    gdk_display_sync(display);
    gtk_widget_destroy(window);
    puts("passed: startup fallback removes color bindings and stays disabled");
    return EXIT_SUCCESS;
  }
  g_object_ref(display);

  _window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(_window), "darktable Wayland color test");
  gtk_window_set_decorated(GTK_WINDOW(_window), FALSE);
  gtk_window_set_accept_focus(GTK_WINDOW(_window), FALSE);
  gtk_window_set_focus_on_map(GTK_WINDOW(_window), FALSE);
  g_signal_connect(_window, "realize", G_CALLBACK(_realize), NULL);
  dt_wayland_color_prepare_window(_window);
  gtk_window_set_default_size(GTK_WINDOW(_window), 600, 400);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_container_set_border_width(GTK_CONTAINER(box), 20);
  gtk_container_add(GTK_CONTAINER(_window), box);
  for(int i = 0; i < 2; i++)
  {
    _areas[i] = gtk_drawing_area_new();
    gtk_box_pack_start(GTK_BOX(box), _areas[i], TRUE, TRUE, 0);
    g_signal_connect(_areas[i], "draw", G_CALLBACK(_draw), NULL);
  }
  gtk_widget_show_all(_window);
  g_timeout_add(250, _tick, NULL);
  gtk_main();
  gdk_display_sync(display);
  _check_close_with_frame(display);
  g_object_unref(display);
  printf("passed: %u frames, %u managed, %u alpha checks, %u partial redraws, "
         "%u reentrant checks, %u allocation failures, %u repaints, %u unpainted frames\n",
         _frames, _managed, _alpha_checks, _partial_checks, _reentrant_checks, _allocation_checks,
         _repaint_checks, _unpainted_checks);
  return EXIT_SUCCESS;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
