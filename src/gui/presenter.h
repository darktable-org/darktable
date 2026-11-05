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
#include "develop/frame.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

/*
 * the presenter shows frames (develop/frame.h) in a widget. see
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

/** paint frame at (x, y), as cairo_paint() would paint a surface set
 * with cairo_set_source_surface(cr, surface, x, y), with the context's
 * current transformation and the extents of its clip. a frame in another
 * encoding than the window's output is shown converted to it. the
 * presenter takes its own reference */
void dt_presenter_image(cairo_t *cr,
                        const double x,
                        const double y,
                        dt_frame_t *frame,
                        const cairo_filter_t filter);

/** keep this draw: the presentation dt_presenter_keep() shows from now on,
 * unless GTK restricted the draw to a damaged area. a full draw that
 * neither retains itself nor shows the kept presentation drops it */
void dt_presenter_retain(cairo_t *cr);

/** paint the kept presentation, everything its draw showed, where it was;
 * what is drawn afterwards goes on top. FALSE if there is none */
gboolean dt_presenter_keep(cairo_t *cr);

/** drop the images and the kept presentation of a widget */
void dt_presenter_clear(GtkWidget *widget);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
