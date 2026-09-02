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

#include "dtgtk/paint.h"
#include <gtk/gtk.h>

G_BEGIN_DECLS

/** Key for g_object_get_data() on each star button: GINT_TO_POINTER(0..5) */
#define DT_RATING_STARS_VALUE_KEY "dt-rating-value"

/**
 * Horizontal strip of six CSS-targetable rating buttons (unrated + *1-*5).
 * The returned widget is a GtkBox named "lib-rating-stars".
 *
 * @param pressed  gesture "pressed" callback (may be NULL)
 * @param data     user data for pressed
 */
GtkWidget *dtgtk_rating_stars_new(GCallback pressed,
                                  gpointer data);

/** Rating 0..5 for a button from this strip, or -1 if not a strip button. */
int dtgtk_rating_stars_get_value(GtkWidget *button);

void dtgtk_cairo_paint_rating_star(cairo_t *cr,
                                   const gint x,
                                   const gint y,
                                   const gint w,
                                   const gint h,
                                   const gint flags,
                                   void *data);

G_END_DECLS
