/*
    This file is part of darktable,
    Copyright (C) 2010-2020 darktable developers.

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
#include <sqlite3.h>
#include <stdint.h>


typedef enum dt_map_locations_type_t
{
  MAP_LOCATION_SHAPE_CIRCLE,
  MAP_LOCATION_SHAPE_RECTANGLE,
  MAP_LOCATION_SHAPE_FORM_MAX
} dt_map_locations_type_t;

typedef struct dt_map_location_data_t
{
  double lon, lat, delta1, delta2;
  int shape;
} dt_map_location_data_t;

typedef struct dt_map_location_t
{
  guint id;
  gchar *tag;
  guint count;
} dt_map_location_t;

// create a new location
const guint dt_map_location_new(const char *const name);

// remove a location
void dt_map_location_delete(const guint locid);

// rename a location
void dt_map_location_rename(const guint locid, const char *const name);

// does the location name already exist
gboolean dt_map_location_name_exists(const char *const name);

// retrieve list of tags which are on that path
// to be freed with dt_map_location_free_result()
GList *dt_map_location_get_locations_by_path(const gchar *path,
                                             const gboolean remove_root);

// free map location list
void dt_map_location_free_result(GList **result);

// sort the tag list considering the '|' character
GList *dt_map_location_sort(GList *tags);

// get location's data
dt_map_location_data_t *dt_map_location_get_data(const guint locid);

// set locations's data
void dt_map_location_set_data(const guint locid, const dt_map_location_data_t *g);

// find locations which match with that image
GList *dt_map_location_find_locations(const guint imgid);

// update image's locations - remove old ones and add new ones
void dt_map_location_update_locations(const guint imgid, const GList *tags);

// update location's images - remove old ones and add new ones
void dt_map_location_update_images(const guint locid);

// return root tag for location geotagging
const char *dt_map_location_data_tag_root();

// tell if the point (lon, lat) belongs to location
gboolean dt_map_location_included(const float lon, const float lat,
                                  dt_map_location_data_t *g);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
