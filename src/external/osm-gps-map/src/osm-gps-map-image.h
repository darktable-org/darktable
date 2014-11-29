/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4; tab-width: 4 -*- */
/* vim:set et sw=4 ts=4 */
/*
 * Copyright (C) 2013 John Stowers <john.stowers@gmail.com>
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

#ifndef _OSM_GPS_MAP_IMAGE_H
#define _OSM_GPS_MAP_IMAGE_H

#include <glib-object.h>
#include <gdk/gdk.h>
#include <cairo.h>

#include "osm-gps-map-point.h"

G_BEGIN_DECLS

#define OSM_TYPE_GPS_MAP_IMAGE              osm_gps_map_image_get_type()
#define OSM_GPS_MAP_IMAGE(obj)              (G_TYPE_CHECK_INSTANCE_CAST ((obj), OSM_TYPE_GPS_MAP_IMAGE, OsmGpsMapImage))
#define OSM_GPS_MAP_IMAGE_CLASS(klass)      (G_TYPE_CHECK_CLASS_CAST ((klass), OSM_TYPE_GPS_MAP_IMAGE, OsmGpsMapImageClass))
#define OSM_IS_GPS_MAP_IMAGE(obj)           (G_TYPE_CHECK_INSTANCE_TYPE ((obj), OSM_TYPE_GPS_MAP_IMAGE))
#define OSM_IS_GPS_MAP_IMAGE_CLASS(klass)   (G_TYPE_CHECK_CLASS_TYPE ((klass), OSM_TYPE_GPS_MAP_IMAGE))
#define OSM_GPS_MAP_IMAGE_GET_CLASS(obj)    (G_TYPE_INSTANCE_GET_CLASS ((obj), OSM_TYPE_GPS_MAP_IMAGE, OsmGpsMapImageClass))

typedef struct _OsmGpsMapImage OsmGpsMapImage;
typedef struct _OsmGpsMapImageClass OsmGpsMapImageClass;
typedef struct _OsmGpsMapImagePrivate OsmGpsMapImagePrivate;

struct _OsmGpsMapImage
{
    GObject parent;

    OsmGpsMapImagePrivate *priv;
};

struct _OsmGpsMapImageClass
{
    GObjectClass parent_class;
};

GType osm_gps_map_image_get_type (void) G_GNUC_CONST;

OsmGpsMapImage *osm_gps_map_image_new (void);
void            osm_gps_map_image_draw (OsmGpsMapImage *object, cairo_t *cr, GdkRectangle *rect);
const OsmGpsMapPoint *osm_gps_map_image_get_point(OsmGpsMapImage *object);
gint osm_gps_map_image_get_zorder(OsmGpsMapImage *object);

G_END_DECLS

#endif /* _OSM_GPS_MAP_IMAGE_H */
