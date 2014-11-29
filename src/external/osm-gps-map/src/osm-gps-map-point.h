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

#ifndef _OSM_GPS_MAP_POINT_H
#define _OSM_GPS_MAP_POINT_H

#include <glib.h>
#include <glib-object.h>

G_BEGIN_DECLS

#define OSM_TYPE_GPS_MAP_POINT              osm_gps_map_point_get_type()

typedef struct _OsmGpsMapPoint OsmGpsMapPoint;

struct _OsmGpsMapPoint
{
    /* radians */
    float  rlat;
    float  rlon;
};

GType osm_gps_map_point_get_type (void) G_GNUC_CONST;

OsmGpsMapPoint *    osm_gps_map_point_new_degrees   (float lat, float lon);
OsmGpsMapPoint *    osm_gps_map_point_new_radians   (float rlat, float rlon);
void                osm_gps_map_point_get_degrees   (OsmGpsMapPoint *point, float *lat, float *lon);
void                osm_gps_map_point_get_radians   (OsmGpsMapPoint *point, float *rlat, float *rlon);
void                osm_gps_map_point_set_degrees   (OsmGpsMapPoint *point, float lat, float lon);
void                osm_gps_map_point_set_radians   (OsmGpsMapPoint *point, float rlat, float rlon);
void                osm_gps_map_point_free          (OsmGpsMapPoint *point);
OsmGpsMapPoint *    osm_gps_map_point_copy          (const OsmGpsMapPoint *point);

G_END_DECLS

#endif /* _OSM_GPS_MAP_POINT_H */
