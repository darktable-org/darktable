/*
    This file is part of darktable,
    Copyright (C) 2011-2021 darktable developers.

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

#include <glib.h>
#include "common/image.h"

#define EARTH_RADIUS 6378100.0 /* in meters */
#define DT_MINIMUM_DISTANCE_FOR_GEODESIC 100000.0 /* in meters */
#define DT_MINIMUM_ANGULAR_DELTA_FOR_GEODESIC 1.0
/* DT_MINIMUM_ANGULAR_DELTA_FOR_GEODESIC is in degrees, and is used for longitude and latitude
   0.1 degress ~ 10 km on the earth surface */

struct dt_gpx_t;

typedef struct dt_gpx_track_point_t
{
  gdouble longitude, latitude, elevation;
  GDateTime *time;
  uint32_t segid;
} dt_gpx_track_point_t;

typedef struct dt_gpx_track_segment_t
{
  guint id;
  GDateTime *start_dt;
  GDateTime *end_dt;
  char *name;
  dt_gpx_track_point_t *trkpt;
  uint32_t nb_trkpt;
} dt_gpx_track_segment_t;

/* loads and parses a gpx track file */
struct dt_gpx_t *dt_gpx_new(const gchar *filename);
void dt_gpx_destroy(struct dt_gpx_t *gpx);

/* fetch the lon,lat coords for time t, if within time range
  of gpx record return TRUE, FALSE is returned if out of time frame
  and closest record of lon,lat is filled */
gboolean dt_gpx_get_location(struct dt_gpx_t *, GDateTime *timestamp, dt_image_geoloc_t *geoloc);

// get the list of track segments
GList *dt_gpx_get_trkseg(struct dt_gpx_t *gpx);

// get the list of track points for a track segment
GList *dt_gpx_get_trkpts(struct dt_gpx_t *gpx, const guint segid);

// get the distance on the geodesic line and the angle delta referred to earth center
void dt_gpx_geodesic_distance(double lat1, double lon1,
                              double lat2, double lon2,
                              double *d, double *delta);

/* get the intermediate point (lat, lon) on the geodesic line between point 1 and point 2
   which form an angle delta (calculated by dt_gpx_geodesic_distance)
   corresponding to fraction f
   boolean first_time is in order to reduce unnecessary calculations  */
void dt_gpx_geodesic_intermediate_point(const double lat1, const double lon1,
                                        const double lat2, const double lon2,
                                        const double delta,
                                        const gboolean first_time,
                                        double f,
                                        double *lat, double *lon);


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

