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
 * internal to the presenter: the layered backend (presenter_layers.c) and
 * its platform transports. a native platform shows a window's images on
 * surfaces of its own above GTK's surface: an image layer in the window's
 * output, and above it a foreground layer with everything else the
 * presenting widget draws. GTK3 knows nothing of those surfaces, so the
 * layers are composed in presenter_layers.c, and a transport only encodes
 * and shows them. see dev-doc/display_presentation.md.
 */

#include "gui/presenter_backend.h"

// what a transport shows for one presenting widget
typedef struct dt_presenter_layers_t
{
  // float RGBA in the window's output, alpha 0 off the images; NULL if
  // unchanged since the last present, which includes size and scale, so a
  // transport sets the image's buffer scale only with a new buffer
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

typedef struct dt_presenter_transport_t dt_presenter_transport_t;

/*
 * what present() is given, and what clear() takes away, must become
 * visible together with GTK's surface for the frame that drew it, or
 * later, never earlier: early layers show a hole or a stale image where
 * GTK has not drawn yet. the transport decides when to call
 * dt_presenter_window_present() to make that true.
 */
struct dt_presenter_transport_t
{
  // show a widget's layers, or return FALSE if they cannot be shown now;
  // *widget_data is the transport's, NULL at first. the layers are only
  // read during the call
  gboolean (*present)(dt_presenter_transport_t *self,
                      GtkWidget *widget,
                      void **widget_data,
                      const dt_presenter_layers_t *layers);
  // stop showing a widget's layers, with the frame whose draw cleared
  // them, or no earlier
  void (*clear)(dt_presenter_transport_t *self, void **widget_data);
  // the widget is gone: free *widget_data and set it to NULL
  void (*release)(dt_presenter_transport_t *self, void **widget_data);
  void (*free)(dt_presenter_transport_t *self);
};

/** for transports: present the layers of every widget of window drawn
 * since the last call */
void dt_presenter_window_present(dt_presenter_window_t *window);

#ifdef HAVE_WAYLAND
/** the Wayland transport for toplevel, or NULL if the compositor cannot do
 * it or its output is plain SDR sRGB: the window then paints with cairo on
 * its system display profile */
dt_presenter_transport_t *dt_presenter_wayland_new(GtkWidget *toplevel,
                                                   dt_presenter_window_t *window);
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
