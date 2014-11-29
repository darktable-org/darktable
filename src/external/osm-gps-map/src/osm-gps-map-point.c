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

/**
 * SECTION:osm-gps-map-point
 * @short_description: A geographic location (latitude, longitude)
 * @stability: Stable
 * @include: osm-gps-map.h
 *
 * #OsmGpsMapPoint describes a geographic location (latitude, longitude). 
 * Helper functions exist to create such a point from either radian co-ordinates
 * (osm_gps_map_point_new_radians()) or degrees (osm_gps_map_new_degrees()).
 **/

#include "converter.h"
#include "osm-gps-map-point.h"

GType
osm_gps_map_point_get_type (void)
{
    static GType our_type = 0;

    if (our_type == 0)
        our_type = g_boxed_type_register_static (g_intern_static_string ("OsmGpsMapPoint"),
				         (GBoxedCopyFunc)osm_gps_map_point_copy,
				         (GBoxedFreeFunc)osm_gps_map_point_free);
    return our_type;
}

OsmGpsMapPoint *
osm_gps_map_point_new_degrees(float lat, float lon)
{
    OsmGpsMapPoint *p = g_new0(OsmGpsMapPoint, 1);
    p->rlat = deg2rad(lat);
    p->rlon = deg2rad(lon);
    return p;
}

OsmGpsMapPoint *
osm_gps_map_point_new_radians(float rlat, float rlon)
{
    OsmGpsMapPoint *p = g_new0(OsmGpsMapPoint, 1);
    p->rlat = rlat;
    p->rlon = rlon;
    return p;
}

/**
 * osm_gps_map_point_get_degrees:
 * @point: The point ( latitude and longitude in radian )
 * @lat: (out): latitude in degrees
 * @lon: (out): longitude in degrees
 *
 * Returns the lagitude and longitude in degrees.
 * of the current window, i.e the top left and bottom right corners.
 **/
void
osm_gps_map_point_get_degrees(OsmGpsMapPoint *point, float *lat, float *lon)
{
    *lat = rad2deg(point->rlat);
    *lon = rad2deg(point->rlon);
}

void
osm_gps_map_point_get_radians(OsmGpsMapPoint *point, float *rlat, float *rlon)
{
    *rlat = point->rlat;
    *rlon = point->rlon;
}

void
osm_gps_map_point_set_degrees(OsmGpsMapPoint *point, float lat, float lon)
{
    point->rlat = deg2rad(lat);
    point->rlon = deg2rad(lon);
}

void
osm_gps_map_point_set_radians(OsmGpsMapPoint *point, float rlat, float rlon)
{
    point->rlat = rlat;
    point->rlon = rlon;
}

/**
 * osm_gps_map_point_copy:
 *
 * Since: 0.7.2
 */
OsmGpsMapPoint *
osm_gps_map_point_copy (const OsmGpsMapPoint *point)
{
    OsmGpsMapPoint *result = g_new (OsmGpsMapPoint, 1);
    *result = *point;

    return result;
}

/**
 * osm_gps_map_point_free:
 *
 * Since: 0.7.2
 */
void
osm_gps_map_point_free (OsmGpsMapPoint *point)
{
    g_free(point);
}
