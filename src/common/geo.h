/*
 *    This file is part of darktable,
 *    copyright (c) 2016 tobias ellinghaus.
 *
 *    darktable is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    darktable is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef __GEO_H__
#define __GEO_H__

typedef enum dt_geo_map_display_t
{
  MAP_DISPLAY_NONE,
  MAP_DISPLAY_POINT,
  MAP_DISPLAY_TRACK,
  MAP_DISPLAY_POLYGON
} dt_geo_map_display_t;

typedef struct dt_geo_map_display_point_t
{
  float lat, lon;
} dt_geo_map_display_point_t;

#endif // __GEO_H__
