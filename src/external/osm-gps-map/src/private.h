/*
 * Copyright (C) Marcus Bauer 2008 <marcus.bauer@gmail.com>
 * Copyright (C) John Stowers 2013 <john.stowers@gmail.com>
 *
 * Contributions by
 * Everaldo Canuto 2009 <everaldo.canuto@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
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

#endif /* _PRIVATE_H_ */
