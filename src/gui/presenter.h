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

#include "common/colorspaces.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

/*
 * the presenter shows rendered float frames in a widget. see
 * dev-doc/display_presentation.md.
 *
 * a presenting widget wraps its draw handler in begin() and end() and
 * draws everything on the cairo context begin() returns, in paint order.
 * dt_presenter_image() takes the place of painting an opaque image
 * there, with the context's current transformation and the extents of
 * its clip.
 */

/** start drawing widget: returns the context to draw on */
cairo_t *dt_presenter_begin(GtkWidget *widget,
                            cairo_t *cr);
/** finish drawing widget; cr is the context begin() returned */
void dt_presenter_end(GtkWidget *widget,
                      cairo_t *cr);

/** paint float RGBA pixels in encoding at (x, y), as cairo_paint() would
 * paint a surface set with cairo_set_source_surface(cr, surface, x, y),
 * with the context's current transformation and the extents of its clip.
 * id changes whenever the pixels do (dt_dev_new_frame_id()). the pixels
 * are not used after the call returns. returns FALSE if it is not shown:
 * a window that declared its output shows frames in its profile only */
gboolean dt_presenter_image(cairo_t *cr,
                            const double x,
                            const double y,
                            const float *pixels,
                            const int width,
                            const int height,
                            const dt_display_output_t *encoding,
                            const uint32_t id,
                            const cairo_filter_t filter);

/** show the images of the last full draw again, where they were */
void dt_presenter_keep(cairo_t *cr);

/** keep a display profile control insensitive while the presenter owns
 * the output of window (DT_COLORSPACE_DISPLAY or DISPLAY2), where the
 * selection has no effect */
void dt_presenter_display_control(GtkWidget *control,
                                  const dt_colorspaces_color_profile_type_t window);

/** drop the images of a widget */
void dt_presenter_clear(GtkWidget *widget);

/** an 8-bit surface of float RGBA pixels in encoding, for drawing with GTK:
 * the colors GTK's surface shows them as. converted once per frame id
 * and kept for the next calls; release it with cairo_surface_destroy().
 * GUI thread only */
cairo_surface_t *dt_presenter_gtk_surface(const float *pixels,
                                          const int width,
                                          const int height,
                                          const dt_display_output_t *encoding,
                                          const uint32_t id);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
