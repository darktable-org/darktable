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

/*
 * the cairo backend: a presenting widget draws on GTK's own context, and
 * images are painted there in RGB24. it is a backend like the layered
 * one, not a fallback inside the core, as software rendering is one of
 * WebRender's and GTK4's renderers behind the same interface.
 */

#include "gui/presenter_backend.h"
#include "common/darktable.h"

#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

typedef struct _backend_t
{
  dt_presenter_backend_t backend;
  dt_presenter_window_t *window;
  // the kept presentation is a reference on GTK's target, not a copy
  gboolean reference;
} _backend_t;

typedef struct _widget_t
{
  GtkWidget *widget;
  // the kept presentation, the matrix from the widget's user space to its
  // pattern space, and the widget's size then: a reference holds all of
  // GTK's target, other widgets around this one too
  cairo_surface_t *kept;
  cairo_matrix_t kept_matrix;
  int kept_width, kept_height;
} _widget_t;

static cairo_t *_begin(dt_presenter_backend_t *self,
                       GtkWidget *widget,
                       void **data,
                       cairo_t *widget_cr)
{
  if(!*data)
  {
    _widget_t *w = g_malloc0(sizeof(_widget_t));
    w->widget = widget;
    *data = w;
  }
  return widget_cr;
}

static gboolean _image(dt_presenter_backend_t *self,
                       void *data,
                       cairo_t *cr,
                       const double x,
                       const double y,
                       dt_presenter_image_t *image)
{
  _backend_t *b = (_backend_t *)self;
  dt_display_output_t output;
  dt_presenter_window_output(b->window, &output);
  cairo_surface_t *surface = dt_frame_surface(image->frame, &output.image);
  if(!surface) return FALSE;

  cairo_save(cr);
  cairo_set_source_surface(cr, surface, x, y);
  cairo_pattern_set_filter(cairo_get_source(cr), image->filter);
  cairo_paint(cr);
  cairo_restore(cr);
  return TRUE;
}

static gboolean _keep(dt_presenter_backend_t *self,
                      void *data,
                      cairo_t *cr,
                      const cairo_matrix_t *base,
                      GArray *images)
{
  _widget_t *w = data;
  if(!w || !w->kept) return FALSE;

  cairo_save(cr);
  cairo_set_matrix(cr, base);
  cairo_rectangle(cr, 0, 0, w->kept_width, w->kept_height);
  cairo_clip(cr);
  cairo_set_source_surface(cr, w->kept, 0, 0);
  cairo_pattern_set_matrix(cairo_get_source(cr), &w->kept_matrix);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_paint(cr);
  cairo_restore(cr);
  return TRUE;
}

static void _drop(_widget_t *w)
{
  if(w->kept) cairo_surface_destroy(w->kept);
  w->kept = NULL;
}

// the widget's part of GTK's target, in the widget's user space, into a
// surface of ours, reused while the size stays
static void _copy(_widget_t *w,
                  cairo_surface_t *target,
                  const cairo_matrix_t *base)
{
  const int scale = gtk_widget_get_scale_factor(w->widget);
  const int width = gtk_widget_get_allocated_width(w->widget) * scale;
  const int height = gtk_widget_get_allocated_height(w->widget) * scale;
  double sx = 0.0, sy = 0.0;
  if(w->kept) cairo_surface_get_device_scale(w->kept, &sx, &sy);
  if(!w->kept || cairo_image_surface_get_width(w->kept) != width
     || cairo_image_surface_get_height(w->kept) != height || sx != scale)
  {
    _drop(w);
    w->kept = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if(cairo_surface_status(w->kept) != CAIRO_STATUS_SUCCESS)
    {
      _drop(w);
      return;
    }
    cairo_surface_set_device_scale(w->kept, scale, scale);
  }

  cairo_t *cr = cairo_create(w->kept);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface(cr, target, 0, 0);
  cairo_pattern_set_matrix(cairo_get_source(cr), base);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_matrix_init_identity(&w->kept_matrix);
}

static void _end(dt_presenter_backend_t *self,
                 void *data,
                 cairo_t *widget_cr,
                 const cairo_matrix_t *base,
                 const GArray *images,
                 const dt_presenter_kept_t kept)
{
  _backend_t *b = (_backend_t *)self;
  _widget_t *w = data;
  if(kept == DT_PRESENTER_KEPT_NONE)
    _drop(w);
  else if(kept == DT_PRESENTER_KEPT_THIS)
  {
    // the target holds everything GTK draws in this frame from here on,
    // the widget's drawing and what is drawn above it
    cairo_surface_t *target = cairo_get_target(widget_cr);
    if(b->reference)
    {
      _drop(w);
      w->kept = cairo_surface_reference(target);
      w->kept_matrix = *base;
    }
    else
      _copy(w, target, base);
    w->kept_width = gtk_widget_get_allocated_width(w->widget);
    w->kept_height = gtk_widget_get_allocated_height(w->widget);
  }
}

static void _clear(dt_presenter_backend_t *self,
                   void *data)
{
  if(data) _drop(data);
}

static void _release(dt_presenter_backend_t *self,
                     void **data)
{
  if(!*data) return;
  _drop(*data);
  g_free(*data);
  *data = NULL;
}

static void _free(dt_presenter_backend_t *self)
{
  g_free(self);
}

dt_presenter_backend_t *dt_presenter_cairo_new(dt_presenter_window_t *window)
{
  _backend_t *b = g_malloc0(sizeof(_backend_t));
  b->window = window;
  b->backend = (dt_presenter_backend_t){ .begin = _begin,
                                         .image = _image,
                                         .keep = _keep,
                                         .end = _end,
                                         .clear = _clear,
                                         .release = _release,
                                         .free = _free };
  // GDK on X11 draws each frame into a surface of its own, which a
  // reference keeps as it was, as master kept the darkroom's. GDK on
  // Wayland and Quartz draws into its window's buffer, which the next frame
  // overwrites. on Win32, holding a reference to the target prevents GTK
  // from invalidating the widget surface, so that gtk_widget_queue_draw()
  // silently fails (issue #20831). everywhere but X11 the presentation is
  // copied
#ifdef GDK_WINDOWING_X11
  b->reference = GDK_IS_X11_DISPLAY(gtk_widget_get_display(window->toplevel));
#endif
  return &b->backend;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
