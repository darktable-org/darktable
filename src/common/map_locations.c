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

#include "common/geo.h"
#include "common/map_locations.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/tags.h"

// root for location geotagging
const char *location_tag = "darktable|locations";
const char *location_tag_prefix = "darktable|locations|";

// create a new location
const guint dt_map_location_new(const char *const name)
{
  char *loc_name = g_strconcat(location_tag_prefix, name, NULL);
  guint locid = -1;
  dt_tag_new(loc_name, &locid);
  return locid;
}

// remove a location
void dt_map_location_delete(const guint locid)
{
  if(locid == -1) return;
  char *name = dt_tag_get_name(locid);
  if(name)
  {
    if(g_str_has_prefix(name, location_tag_prefix))
    {
      sqlite3_stmt *stmt;
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "DELETE FROM data.locations WHERE tagid=?1",
                                  -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, locid);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
      dt_tag_remove(locid, TRUE);
    }
  g_free(name);
  }
}

// rename a location
void dt_map_location_rename(const guint locid, const char *const name)
{
  if(locid == -1 || !name || !name[0]) return;
  char *old_name = dt_tag_get_name(locid);
  if(old_name)
  {
    if(g_str_has_prefix(old_name, location_tag_prefix))
    {
      char *new_name = g_strconcat(location_tag_prefix, name, NULL);
      dt_tag_rename(locid, new_name);
      g_free(new_name);
    }
    g_free(old_name);
  }
}

// does the location name already exist
gboolean dt_map_location_name_exists(const char *const name)
{
  char *new_name = g_strconcat(location_tag_prefix, name, NULL);
  const gboolean exists = dt_tag_exists(new_name, NULL);
  g_free(new_name);
  return exists;
}

// retrieve list of tags which are on that path
GList *dt_map_location_get_locations_by_path(const gchar *path,
                                             const gboolean remove_root)
{
  if(!path) return NULL;

  gchar *path1, *path2;
  if(!path[0])
  {
    path1 = g_strdup(location_tag);
    path2 = g_strdup_printf("%s|", path1);
  }
  else
  {
    path1 = g_strconcat(location_tag_prefix, path, NULL);
    path2 = g_strdup_printf("%s|", path1);
  }
  GList *tags = NULL;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT t.id, t.name, ti.count"
                              "  FROM data.tags AS t"
                              "  LEFT JOIN (SELECT tagid,"
                              "               COUNT(DISTINCT imgid) AS count"
                              "             FROM main.tagged_images"
                              "             GROUP BY tagid) AS ti"
                              "  ON ti.tagid = t.id"
                              "  WHERE name = ?1 OR SUBSTR(name, 1, LENGTH(?2)) = ?2",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, path1, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, path2, -1, SQLITE_TRANSIENT);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *name = (const char *)sqlite3_column_text(stmt, 1);
    const int lgth = remove_root ? strlen(path1) + 1 : strlen(location_tag_prefix);
    if(name && strlen(name) > lgth)
    {
      dt_map_location_t *t = g_malloc0(sizeof(dt_map_location_t));
      name += lgth;
      t->tag = g_strdup(name);
      t->id = sqlite3_column_int(stmt, 0);
      t->count = sqlite3_column_int(stmt, 2);
      tags = g_list_prepend(tags, t);
    }
  }
  sqlite3_finalize(stmt);

  g_free(path1);
  g_free(path2);
  return tags;
}

static void _free_result_item(dt_map_location_t *t, gpointer unused)
{
  g_free(t->tag);
  g_free(t);
}

// free map location list
void dt_map_location_free_result(GList **result)
{
  if(result && *result)
  {
    g_list_free_full(*result, (GDestroyNotify)_free_result_item);
  }
}

static gint _sort_by_path(gconstpointer a, gconstpointer b)
{
  const dt_map_location_t *tuple_a = (const dt_map_location_t *)a;
  const dt_map_location_t *tuple_b = (const dt_map_location_t *)b;

  return g_ascii_strcasecmp(tuple_a->tag, tuple_b->tag);
}

// sort the tag list considering the '|' character
GList *dt_map_location_sort(GList *tags)
{
  // order such that sub tags are coming directly behind their parent
  GList *sorted_tags;
  for(GList *taglist = tags; taglist; taglist = g_list_next(taglist))
  {
    gchar *tag = ((dt_map_location_t *)taglist->data)->tag;
    for(char *letter = tag; *letter; letter++)
      if(*letter == '|') *letter = '\1';
  }
  sorted_tags = g_list_sort(tags, _sort_by_path);
  for(GList *taglist = sorted_tags; taglist; taglist = g_list_next(taglist))
  {
    gchar *tag = ((dt_map_location_t *)taglist->data)->tag;
    for(char *letter = tag; *letter; letter++)
      if(*letter == '\1') *letter = '|';
  }
  return sorted_tags;
}

// get location's data
dt_map_location_data_t *dt_map_location_get_data(const guint locid)
{
  if(locid == -1) return NULL;
  dt_map_location_data_t *g = NULL;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT type, longitude, latitude, delta1, delta2"
                              "  FROM data.locations"
                              "  JOIN data.tags ON id = tagid"
                              "  WHERE tagid = ?1 AND longitude IS NOT NULL"
                              "    AND SUBSTR(name, 1, LENGTH(?2)) = ?2",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, locid);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, location_tag_prefix, -1, SQLITE_STATIC);

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    g = (dt_map_location_data_t *)malloc(sizeof(dt_map_location_data_t));
    g->shape = sqlite3_column_int(stmt, 0);
    g->lon = sqlite3_column_double(stmt, 1);
    g->lat = sqlite3_column_double(stmt, 2);
    g->delta1 = sqlite3_column_double(stmt, 3);
    g->delta2 = sqlite3_column_double(stmt, 4);
  }
  sqlite3_finalize(stmt);
  return g;
}

// set locations's data
void dt_map_location_set_data(const guint locid, const dt_map_location_data_t *g)
{
  if(locid == -1) return;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT OR REPLACE INTO data.locations"
                              "  (tagid, type, longitude, latitude, delta1, delta2)"
                              "  VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, locid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, g->shape);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 3, g->lon);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 4, g->lat);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 5, g->delta1);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 6, g->delta2);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

// find locations which match with that image
GList *dt_map_location_find_locations(const guint imgid)
{
  GList *tags = NULL;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT l.tagid FROM main.images AS i"
                              "  JOIN data.locations AS l"
                              "  ON (l.type = ?2 AND"
                              "      ((i.longitude-l.longitude)*(i.longitude-l.longitude)+"
                              "      (i.latitude-l.latitude)*(i.latitude-l.latitude))<="
                              "      delta1*delta1)"
                              "    OR (l.type = ?3 AND"
                              "      i.longitude>=(l.longitude-delta1) AND"
                              "      i.longitude<=(l.longitude+delta1) AND"
                              "      i.latitude>=(l.latitude-delta2) AND"
                              "      i.latitude<=(l.latitude+delta2))"
                              " WHERE i.id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, MAP_LOCATION_SHAPE_CIRCLE);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, MAP_LOCATION_SHAPE_RECTANGLE);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int id = sqlite3_column_int(stmt, 0);
    tags = g_list_prepend(tags, GINT_TO_POINTER(id));
  }
  sqlite3_finalize(stmt);
  return tags;
}

// find images which match with that location
GList *_map_location_find_images(const guint locid)
{
  GList *imgs = NULL;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT i.id FROM main.images AS i"
                              "  JOIN data.locations AS l"
                              "  ON (l.type = ?2 AND"
                              "      ((i.longitude-l.longitude)*(i.longitude-l.longitude)+"
                              "      (i.latitude-l.latitude)*(i.latitude-l.latitude))<="
                              "      delta1*delta1)"
                              "   OR (l.type = ?3 AND"
                              "      i.longitude>=(l.longitude-delta1) AND"
                              "      i.longitude<=(l.longitude+delta1) AND"
                              "      i.latitude>=(l.latitude-delta2) AND"
                              "      i.latitude<=(l.latitude+delta2))"
                              "  WHERE l.tagid = ?1 ",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, locid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, MAP_LOCATION_SHAPE_CIRCLE);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, MAP_LOCATION_SHAPE_RECTANGLE);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int id = sqlite3_column_int(stmt, 0);
    imgs = g_list_prepend(imgs, GINT_TO_POINTER(id));
  }
  sqlite3_finalize(stmt);
  return imgs;
}

// update image's locations - remove old ones and add new ones
void dt_map_location_update_locations(const guint imgid, const GList *tags)
{
  // get current locations
  GList *old_tags = NULL;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT t.id FROM main.tagged_images ti"
                              "  JOIN data.tags AS t ON t.id = ti.tagid"
                              "  JOIN data.locations AS l ON l.tagid = t.id"
                              "  WHERE imgid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int id = sqlite3_column_int(stmt, 0);
    old_tags = g_list_prepend(old_tags, GINT_TO_POINTER(id));
  }
  sqlite3_finalize(stmt);

  // clean up locations which are not valid anymore
  for(GList *tag = old_tags; tag; tag = g_list_next(tag))
  {
    if(!g_list_find((GList *)tags, tag->data))
    {
      dt_tag_detach(GPOINTER_TO_INT(tag->data), imgid,
      FALSE, FALSE);
    }
  }

  // add new locations
  for(GList *tag = (GList *)tags; tag; tag = g_list_next(tag))
  {
    if(!g_list_find(old_tags, tag->data))
    {
      dt_tag_attach(GPOINTER_TO_INT(tag->data), imgid,
                    FALSE, FALSE);
    }
  }
  g_list_free(old_tags);
}

// update location's images - remove old ones and add new ones
void dt_map_location_update_images(const guint locid)
{
  // get previous images
  GList *imgs = dt_tag_get_images(locid);

  // find images in that location
  GList *new_imgs = _map_location_find_images(locid);

  // detach images which are not in location anymore
  for(GList *img = imgs; img; img = g_list_next(img))
  {
    if(!g_list_find(new_imgs, img->data))
    {
      dt_tag_detach(locid, GPOINTER_TO_INT(img->data), FALSE, FALSE);
    }
  }

  // add new images to location
  for(GList *img = new_imgs; img; img = g_list_next(img))
  {
    if(!g_list_find(imgs, img->data))
    {
      dt_tag_attach(locid, GPOINTER_TO_INT(img->data), FALSE, FALSE);
    }
  }
  g_list_free(new_imgs);
  g_list_free(imgs);
}

// return root tag for location geotagging
const char *dt_map_location_data_tag_root()
{
  return location_tag;
}

// tell if the point (lon, lat) belongs to location
gboolean dt_map_location_included(const float lon, const float lat,
                                  dt_map_location_data_t *g)
{
  gboolean included = FALSE;
  if((g->shape == MAP_LOCATION_SHAPE_CIRCLE &&
     ((g->lon - lon) * (g->lon - lon) +
      (g->lat - lat) * (g->lat - lat)) <
      g->delta1 * g->delta1)
     ||
     (g->shape == MAP_LOCATION_SHAPE_RECTANGLE &&
      lon > g->lon - g->delta1 && lon < g->lon + g->delta1 &&
      lat > g->lat - g->delta2 && lat < g->lat + g->delta2))
  {
    included = TRUE;
  }
  return included;
}



// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
