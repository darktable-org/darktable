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

#pragma once

/*
 * internal to the presenter. a native backend shows a window's images on
 * surfaces of its own above GTK's surface: an image layer in the window's
 * output, and above it a foreground layer with everything else the
 * presenting widget draws. GTK3 knows nothing of those surfaces, so the
 * layers are composed here (presenter_layers.c), and a backend only
 * encodes and shows them. see dev-doc/display_presentation.md.
 */

#include "common/colorspaces.h"

#include <gtk/gtk.h>

// a frame converted for presentation, owned by the presenter
typedef struct dt_presenter_frame_t
{
  // identity of the source, never dereferenced
  const float *source;
  uint32_t id;
  int width, height;
  dt_display_output_t encoding;
  // cairo backend: the frame quantized for GTK
  cairo_surface_t *surface;
  // native backends: a copy of the float pixels
  float *pixels;
} dt_presenter_frame_t;

typedef struct dt_presenter_image_t
{
  dt_presenter_frame_t *frame;
  // frame pixels to the user space the widget's draw started in
  cairo_matrix_t matrix;
  // the clip's extents in that space
  double clip[4];
  cairo_filter_t filter;
} dt_presenter_image_t;

// what a backend shows for one presenting widget
typedef struct dt_presenter_layers_t
{
  // float RGBA in the window's output, alpha 0 off the images; NULL if
  // unchanged since the last present, which includes size and scale, so a
  // backend sets the image's buffer scale only with a new buffer
  const float *image;
  // premultiplied ARGB32, stride 4 * width
  const uint8_t *foreground;
  // in device pixels
  int width, height;
  // device pixels per logical pixel
  int scale;
  // logical position in the toplevel's surface
  int x, y;
} dt_presenter_layers_t;

typedef struct dt_presenter_backend_t dt_presenter_backend_t;

/*
 * what present() is given, and what clear() takes away, must become
 * visible together with GTK's surface for the frame that drew it, or
 * later, never earlier: early layers show a hole or a stale image where
 * GTK has not drawn yet. the backend decides when to call
 * dt_presenter_window_present() to make that true.
 */
struct dt_presenter_backend_t
{
  // show a widget's layers, or return FALSE if they cannot be shown now;
  // *widget_data is the backend's, NULL at first
  gboolean (*present)(dt_presenter_backend_t *self,
                      GtkWidget *widget,
                      void **widget_data,
                      const dt_presenter_layers_t *layers);
  // stop showing a widget's layers, with the frame whose draw cleared
  // them, or no earlier
  void (*clear)(dt_presenter_backend_t *self, void **widget_data);
  // optional: a draw left layers to present
  void (*drawn)(dt_presenter_backend_t *self);
  // the widget is gone: free *widget_data and set it to NULL
  void (*release)(dt_presenter_backend_t *self, void **widget_data);
  void (*free)(dt_presenter_backend_t *self);
};

typedef struct dt_presenter_window_t dt_presenter_window_t;

/** for backends, and for a setup that leaves the window on cairo: declare
 * the window's output and what GTK's own surface there is shown in, or
 * give both back with NULL; redraws the window */
void dt_presenter_window_set_output(dt_presenter_window_t *window,
                                    const dt_display_output_t *output,
                                    const dt_display_output_t *gtk);
/** for backends: draw the window's presenting widgets again */
void dt_presenter_window_redraw(dt_presenter_window_t *window);
/** for backends: present the layers of every widget drawn since the last
 * call */
void dt_presenter_window_present(dt_presenter_window_t *window);

#ifdef HAVE_WAYLAND
/** the Wayland backend for toplevel, or NULL if the compositor cannot do
 * it; if the compositor manages color anyway, the window's output is then
 * declared sRGB */
dt_presenter_backend_t *dt_presenter_wayland_new(GtkWidget *toplevel,
                                                 dt_presenter_window_t *window);
#endif

/* for presenter.c */

typedef struct dt_presenter_layered_t dt_presenter_layered_t;

/** the layers of widget, or NULL if its window paints with cairo; without
 * create, only layers that exist */
dt_presenter_layered_t *dt_presenter_layered_get(GtkWidget *widget,
                                                 const gboolean create);
/** the output the window of widget declared, or FALSE if it declared none */
gboolean dt_presenter_declared_output(GtkWidget *widget,
                                      dt_display_output_t *output);
/** start a draw: returns the context of the foreground */
cairo_t *dt_presenter_layered_begin(dt_presenter_layered_t *layered);
/** an image takes the pixels it covers: clear them in the foreground */
void dt_presenter_layered_image(dt_presenter_layered_t *layered,
                                const dt_presenter_image_t *image);
/** finish a draw of images, which stay valid until the next one */
void dt_presenter_layered_end(dt_presenter_layered_t *layered,
                              cairo_t *widget_cr,
                              const GArray *images);
/** stop showing the widget's layers */
void dt_presenter_layered_clear(dt_presenter_layered_t *layered);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
