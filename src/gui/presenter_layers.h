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

// where a widget's layers go
typedef struct dt_presenter_layers_t
{
  // in device pixels
  int width, height;
  // device pixels per logical pixel
  int scale;
  // logical position in the toplevel's surface
  int x, y;
} dt_presenter_layers_t;

/*
 * how the image layer stores a straight color c of the window's output
 * image encoding, which the transport reported (1 is reference white):
 * each channel clipped to [clip[0], clip[1]], outside which colorimetry is
 * undefined, then matrix x c, premultiplied, in signed half floats
 */
typedef struct dt_presenter_encoding_t
{
  float clip[2];
  float matrix[3][3];
} dt_presenter_encoding_t;

/*
 * the storage of one present of a widget's layers, which the transport
 * holds for it from reserve() until commit(), or until the next reserve()
 * when it is not committed
 */
typedef struct dt_presenter_reservation_t
{
  // the foreground: premultiplied ARGB32 as cairo's, 4 * width bytes of
  // each row used
  uint8_t *foreground;
  size_t foreground_stride;
  // the image: RGBA half floats, rows of image_stride bytes; NULL while
  // the platform holds every image buffer
  uint16_t *image;
  size_t image_stride;
  dt_presenter_encoding_t encoding;
} dt_presenter_reservation_t;

/*
 * what commit() shows, and what clear() takes away, must become visible
 * together with GTK's surface for the frame that drew it, or later, never
 * earlier: early layers show a hole or a stale image where GTK has not
 * drawn yet. the transport decides when to call
 * dt_presenter_window_present() to make that true.
 */
struct dt_presenter_transport_t
{
  // reserve the storage of a present of a widget's layers, or return
  // FALSE if they cannot be shown now; the layered backend then draws
  // again to retry. *widget_data is the transport's, NULL at first
  gboolean (*reserve)(dt_presenter_transport_t *self,
                      GtkWidget *widget,
                      void **widget_data,
                      const dt_presenter_layers_t *layers,
                      dt_presenter_reservation_t *reservation);
  // show the reservation with the foreground written into it, and the
  // image if image is TRUE; otherwise the image shown last stays, which
  // has the reservation's size, scale and encoding, since a change of
  // those composes a new one. the present is accepted as a whole
  void (*commit)(dt_presenter_transport_t *self,
                 void **widget_data,
                 const gboolean image);
  // stop showing a widget's layers, with the frame whose draw cleared
  // them, or no earlier
  void (*clear)(dt_presenter_transport_t *self, void **widget_data);
  // the widget is gone: free *widget_data and set it to NULL
  void (*release)(dt_presenter_transport_t *self, void **widget_data);
  void (*free)(dt_presenter_transport_t *self);
  // the window's headroom target changed: describe the image for it, and
  // once reservations encode for it, set the window's output with that
  // headroom (dt_presenter_backend_t's target()). until then, and if the
  // description fails, the one in use stays
  void (*target)(dt_presenter_transport_t *self, const float headroom);
  // GTK's popovers are surfaces of their own above the layers, as on
  // Wayland, so they need no replay. where GTK3 draws them into the
  // toplevel's surface (Quartz, Win32), the layers cover them
  gboolean popovers_above;
};

/** for transports: present the layers of every widget of window drawn
 * since the last call */
void dt_presenter_window_present(dt_presenter_window_t *window);

#ifdef HAVE_WAYLAND
/** the Wayland platform of toplevel, or NULL if the compositor cannot take
 * the transport: the window then paints with cairo on its system display
 * profile for as long as it exists */
dt_presenter_platform_t *dt_presenter_wayland_new(GtkWidget *toplevel,
                                                  dt_presenter_window_t *window);
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
