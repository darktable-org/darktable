/*
 * private.h
 * Copyright (C) Marcus Bauer 2008 <marcus.bauer@gmail.com>
 * Copyright (C) John Stowers 2009 <john.stowers@gmail.com>
 *
 * Contributions by
 * Everaldo Canuto 2009 <everaldo.canuto@gmail.com>
 *
 * This is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _PRIVATE_H_
#define _PRIVATE_H_

#include <glib.h>
#include <gdk/gdk.h>
#include <gtk/gtk.h>
#if USE_LIBSOUP22
#include <libsoup/soup.h>
#endif
#include "osm-gps-map-widget.h"

#define TILESIZE 256
#define MAX_ZOOM 20
#define MIN_ZOOM 0

#define MAX_TILE_ZOOM_OFFSET 10
#define MIN_TILE_ZOOM_OFFSET 0

#define OSM_REPO_URI        "http://tile.openstreetmap.org/#Z/#X/#Y.png"
#define OSM_MIN_ZOOM        1
#define OSM_MAX_ZOOM        18
#define OSM_IMAGE_FORMAT    "png"

#define URI_MARKER_X    "#X"
#define URI_MARKER_Y    "#Y"
#define URI_MARKER_Z    "#Z"
#define URI_MARKER_S    "#S"
#define URI_MARKER_Q    "#Q"
#define URI_MARKER_Q0   "#W"
#define URI_MARKER_YS   "#U"
#define URI_MARKER_R    "#R"

#define URI_HAS_X   (1 << 0)
#define URI_HAS_Y   (1 << 1)
#define URI_HAS_Z   (1 << 2)
#define URI_HAS_S   (1 << 3)
#define URI_HAS_Q   (1 << 4)
#define URI_HAS_Q0  (1 << 5)
#define URI_HAS_YS  (1 << 6)
#define URI_HAS_R   (1 << 7)
//....
#define URI_FLAG_END (1 << 8)

/* equatorial radius in meters */
#define OSM_EQ_RADIUS   (6378137.0)

#if !GLIB_CHECK_VERSION (2, 16, 0)
int g_strcmp0(const char *str1, const char *str2)
{
    if( str1 == NULL && str2 == NULL ) return 0;
    if( str1 == NULL ) return -1;
    if( str2 == NULL ) return 1;
    return strcmp(str1, str2);
}
#endif

#if !GTK_CHECK_VERSION (2, 20, 0)
#define gtk_widget_get_realized(widget)                         (GTK_WIDGET_REALIZED (widget))
#define gtk_widget_get_mapped(widget)                           (GTK_WIDGET_MAPPED (widget))
#endif /* GTK < 2.20.0 */

#if !GTK_CHECK_VERSION (2, 18, 0)
#define gtk_cell_renderer_get_alignment(cell, xalign, yalign)   g_object_get (cell, "xalign", xalign, "yalign", yalign, NULL);
#define gtk_cell_renderer_get_padding(cell, xpad, ypad)         g_object_get (cell, "xpad", xpad, "ypad", ypad, NULL);
#define gtk_cell_renderer_set_padding(cell, xpad, ypad)         g_object_set (cell, "xpad", xpad, "ypad", ypad, NULL);
#define gtk_widget_get_allocation(widget, alloc)                (*(alloc) = (widget)->allocation)
#define gtk_widget_set_allocation(widget, alloc)                ((widget)->allocation = *(alloc))
#define gtk_widget_get_app_paintable(widget)                    (GTK_WIDGET_APP_PAINTABLE (widget))
#define gtk_widget_set_can_default(widget, can_default)         { if (can_default) { GTK_WIDGET_SET_FLAGS (widget, GTK_CAN_DEFAULT); } else { GTK_WIDGET_UNSET_FLAGS (widget, GTK_CAN_DEFAULT); } }
#define gtk_widget_set_can_focus(widget, can_focus)             { if (can_focus) { GTK_WIDGET_SET_FLAGS ((widget), GTK_CAN_FOCUS); } else { GTK_WIDGET_UNSET_FLAGS ((widget), GTK_CAN_FOCUS); } }
#define gtk_widget_set_double_buffered(widget, double_buffered) { if (double_buffered) { GTK_WIDGET_SET_FLAGS (widget, GTK_DOUBLE_BUFFERED); } else { GTK_WIDGET_UNSET_FLAGS (widget, GTK_DOUBLE_BUFFERED); } }
#define gtk_widget_is_drawable(widget)                          (GTK_WIDGET_DRAWABLE (widget))
#define gtk_widget_has_focus(widget)                            (GTK_WIDGET_HAS_FOCUS (widget))
#define gtk_widget_get_has_window(widget)                       (!GTK_WIDGET_NO_WINDOW (widget))
#define gtk_widget_get_state(widget)                            ((widget)->state)
#define gtk_widget_get_visible(widget)                          (GTK_WIDGET_VISIBLE (widget))
#define gtk_widget_set_window(widget, _window)                  ((widget)->window = _window)
#endif /* GTK+ < 2.18.0 */

#endif /* _PRIVATE_H_ */
