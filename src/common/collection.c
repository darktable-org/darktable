/*
    This file is part of darktable,
    Copyright (C) 2010-2021 darktable developers.

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

#include "common/collection.h"
#include "common/debug.h"
#include "common/image.h"
#include "common/imageio_rawspeed.h"
#include "common/metadata.h"
#include "common/utility.h"
#include "common/map_locations.h"
#include "common/datetime.h"
#include "control/conf.h"
#include "control/control.h"

#include <assert.h>
#include <glib.h>
#include <memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>


#ifdef _WIN32
//MSVCRT does not have strptime implemented
#include "win/strptime.h"
#endif


#define SELECT_QUERY "SELECT DISTINCT * FROM %s"
#define LIMIT_QUERY "LIMIT ?1, ?2"

/* Stores the collection query, returns 1 if changed.. */
static int _dt_collection_store(const dt_collection_t *collection, gchar *query, gchar *query_no_group);
/* Counts the number of images in the current collection */
static uint32_t _dt_collection_compute_count(const dt_collection_t *collection, gboolean no_group);
/* signal handlers to update the cached count when something interesting might have happened.
 * we need 2 different since there are different kinds of signals we need to listen to. */
static void _dt_collection_recount_callback_1(gpointer instance, gpointer user_data);
static void _dt_collection_recount_callback_2(gpointer instance, uint8_t id, gpointer user_data);
static void _dt_collection_filmroll_imported_callback(gpointer instance, uint8_t id, gpointer user_data);

/* determine image offset of specified imgid for the given collection */
static int dt_collection_image_offset_with_collection(const dt_collection_t *collection, int imgid);
/* update aspect ratio for the selected images */
static void _collection_update_aspect_ratio(const dt_collection_t *collection);

const dt_collection_t *dt_collection_new(const dt_collection_t *clone)
{
  dt_collection_t *collection = g_malloc0(sizeof(dt_collection_t));

  /* initialize collection context*/
  if(clone) /* if clone is provided let's copy it into this context */
  {
    memcpy(&collection->params, &clone->params, sizeof(dt_collection_params_t));
    memcpy(&collection->store, &clone->store, sizeof(dt_collection_params_t));
    collection->where_ext = g_strdupv(clone->where_ext);
    collection->query = g_strdup(clone->query);
    collection->query_no_group = g_strdup(clone->query_no_group);
    collection->clone = 1;
    collection->count = clone->count;
    collection->count_no_group = clone->count_no_group;
    collection->tagid = clone->tagid;
  }
  else /* else we just initialize using the reset */
    dt_collection_reset(collection);

  /* connect to all the signals that might indicate that the count of images matching the collection changed
   */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_TAG_CHANGED,
                            G_CALLBACK(_dt_collection_recount_callback_1), collection);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_FILMROLLS_CHANGED,
                            G_CALLBACK(_dt_collection_recount_callback_1), collection);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_FILMROLLS_REMOVED,
                            G_CALLBACK(_dt_collection_recount_callback_1), collection);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_IMAGE_IMPORT,
                            G_CALLBACK(_dt_collection_recount_callback_2), collection);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_FILMROLLS_IMPORTED,
                            G_CALLBACK(_dt_collection_filmroll_imported_callback), collection);
  return collection;
}

void dt_collection_free(const dt_collection_t *collection)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_dt_collection_recount_callback_1),
                               (gpointer)collection);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_dt_collection_recount_callback_2),
                               (gpointer)collection);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_dt_collection_filmroll_imported_callback),
                               (gpointer)collection);

  g_free(collection->query);
  g_free(collection->query_no_group);
  g_strfreev(collection->where_ext);
  g_free((dt_collection_t *)collection);
}

const dt_collection_params_t *dt_collection_params(const dt_collection_t *collection)
{
  return &collection->params;
}


// Return a pointer to a static string for an "AND" operator if the
// number of terms processed so far requires it.  The variable used
// for term should be an int initialized to and_operator_initial()
// before use.
#define and_operator_initial() (0)
static char * and_operator(int *term)
{
  assert(term != NULL);
  if(*term == 0)
  {
    *term = 1;
    return "";
  }
  else
  {
    return " AND ";
  }

  assert(0); // Not reached.
}

void dt_collection_memory_update()
{
  if(!darktable.collection || !darktable.db) return;
  sqlite3_stmt *stmt;

  /* check if we can get a query from collection */
  gchar *query = g_strdup(dt_collection_get_query(darktable.collection));
  if(!query) return;

  // we have a new query for the collection of images to display. For speed reason we collect all images into
  // a temporary (in-memory) table (collected_images).

  // 1. drop previous data

  // clang-format off
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
                        "DELETE FROM memory.collected_images",
                        NULL, NULL, NULL);
  // reset autoincrement. need in star_key_accel_callback
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
                        "DELETE FROM memory.sqlite_sequence"
                        " WHERE name='collected_images'",
                        NULL, NULL, NULL);
  // clang-format on

  // 2. insert collected images into the temporary table
  gchar *ins_query = g_strdup_printf("INSERT INTO memory.collected_images (imgid) %s", query);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), ins_query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, 0);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, -1);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  g_free(query);
  g_free(ins_query);
}

static void _dt_collection_set_selq_pre_sort(const dt_collection_t *collection, char **selq_pre)
{
  const uint32_t tagid = collection->tagid;
  char tag[16] = { 0 };
  snprintf(tag, sizeof(tag), "%u", tagid);

  // clang-format off
  *selq_pre = dt_util_dstrcat(*selq_pre,
                              "SELECT DISTINCT mi.id FROM (SELECT"
                              "  id, group_id, film_id, filename, datetime_taken, "
                              "  flags, version, %s position, aspect_ratio,"
                              "  maker, model, lens, aperture, exposure, focal_length,"
                              "  iso, import_timestamp, change_timestamp,"
                              "  export_timestamp, print_timestamp"
                              "  FROM main.images AS mi %s%s WHERE ",
                              tagid ? "CASE WHEN ti.position IS NULL THEN 0 ELSE ti.position END AS" : "",
                              tagid ? " LEFT JOIN main.tagged_images AS ti"
                                      " ON ti.imgid = mi.id AND ti.tagid = " : "",
                              tagid ? tag : "");
  // clang-format on
}

int dt_collection_update(const dt_collection_t *collection)
{
  uint32_t result;
  gchar *wq, *wq_no_group, *sq, *selq_pre, *selq_post, *query, *query_no_group;
  wq = wq_no_group = sq = selq_pre = selq_post = query = query_no_group = NULL;

  /* build where part */
  gchar *where_ext = dt_collection_get_extended_where(collection, -1);
  if(!(collection->params.query_flags & COLLECTION_QUERY_USE_ONLY_WHERE_EXT))
  {
    char *rejected_check = g_strdup_printf("((flags & %d) == %d)", DT_IMAGE_REJECTED, DT_IMAGE_REJECTED);
    int and_term = and_operator_initial();

    /* add default filters */
    if(collection->params.filter_flags & COLLECTION_FILTER_FILM_ID)
    {
      wq = g_strdup_printf("%s (film_id = %u)", and_operator(&and_term), collection->params.film_id);
    }
    // DON'T SELECT IMAGES MARKED TO BE DELETED.
    wq = dt_util_dstrcat(wq, " %s (flags & %d) != %d",
                         and_operator(&and_term), DT_IMAGE_REMOVE,
                         DT_IMAGE_REMOVE);

    /* add where ext if wanted */
    if((collection->params.query_flags & COLLECTION_QUERY_USE_WHERE_EXT))
      wq = dt_util_dstrcat(wq, " %s %s", and_operator(&and_term), where_ext);

    g_free(rejected_check);
  }
  else
    wq = g_strdup(where_ext);

  g_free(where_ext);

  wq_no_group = g_strdup(wq);

  /* grouping */
  if(darktable.gui && darktable.gui->grouping)
  {
    // clang-format off
    /* Show the expanded group... */
    wq = dt_util_dstrcat(wq, " AND (group_id = %d OR "
                             /* ...and, in unexpanded groups, show the representative image.
                              * It's possible that the above WHERE clauses will filter out the representative
                              * image, so we have some logic here to pick the image id closest to the
                              * representative image.
                              * The *2+CASE statement are to break ties, so that when id < group_id, it's
                              * weighted a little higher than when id > group_id. */
                             "id IN (SELECT id FROM "
                             "(SELECT id, MIN(ABS(id-group_id)*2 + CASE WHEN (id-group_id) < 0 THEN 1 ELSE 0 END) "
                             "FROM main.images WHERE %s GROUP BY group_id)))",
                         darktable.gui->expanded_group_id, wq_no_group);
    // clang-format on

    /* Additionally, when a group is expanded, make sure the representative image wasn't filtered out.
     * This is important, because otherwise it may be impossible to collapse the group again. */
    wq = dt_util_dstrcat(wq, " OR (id = %d)", darktable.gui->expanded_group_id);
  }

  // get all the sort items
  dt_collection_params_t *params = (dt_collection_params_t *)&collection->params;
  for(int i = 0; i < DT_COLLECTION_SORT_LAST; i++) params->sorts[i] = FALSE;
  const int nb_sort = CLAMP(dt_conf_get_int("plugins/lighttable/filtering/num_sort"), 0, DT_COLLECTION_MAX_RULES);
  char confname[200] = { 0 };
  for(int i = 0; i < nb_sort; i++)
  {
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sort%1d", i);
    params->sorts[dt_conf_get_int(confname)] = TRUE;
  }
  // and we also take account of the last sort
  params->sorts[dt_conf_get_int("plugins/lighttable/filtering/lastsort")] = TRUE;

  /* build select part includes where */
  /* COLOR and PATH */
  if(collection->params.sorts[DT_COLLECTION_SORT_COLOR]
     && collection->params.sorts[DT_COLLECTION_SORT_PATH]
     && (collection->params.query_flags & COLLECTION_QUERY_USE_SORT))
  {
    _dt_collection_set_selq_pre_sort(collection, &selq_pre);
    // clang-format off
    selq_post = dt_util_dstrcat
      (selq_post,
       ") AS mi LEFT OUTER JOIN main.color_labels AS b ON mi.id = b.imgid"
       " JOIN (SELECT id AS film_rolls_id, folder FROM main.film_rolls) ON film_id = film_rolls_id");
    // clang-format on
  }
  /* COLOR and TITLE */
  else if(collection->params.sorts[DT_COLLECTION_SORT_COLOR]
          && collection->params.sorts[DT_COLLECTION_SORT_TITLE]
          && (collection->params.query_flags & COLLECTION_QUERY_USE_SORT))
  {
    _dt_collection_set_selq_pre_sort(collection, &selq_pre);
    // clang-format off
    selq_post = dt_util_dstrcat
      (selq_post,
       ") AS mi LEFT OUTER JOIN main.color_labels AS b ON mi.id = b.imgid"
       " LEFT OUTER JOIN main.meta_data AS m ON mi.id = m.id AND m.key = %d", DT_METADATA_XMP_DC_TITLE);
    // clang-format on
  }
  /* COLOR and DESCRIPTION */
  else if(collection->params.sorts[DT_COLLECTION_SORT_COLOR]
          && collection->params.sorts[DT_COLLECTION_SORT_DESCRIPTION]
          && (collection->params.query_flags & COLLECTION_QUERY_USE_SORT))
  {
    _dt_collection_set_selq_pre_sort(collection, &selq_pre);
    // clang-format off
    selq_post = dt_util_dstrcat
      (selq_post,
       ") AS mi LEFT OUTER JOIN main.color_labels AS b ON mi.id = b.imgid"
       " LEFT OUTER JOIN main.meta_data AS m ON mi.id = m.id AND m.key = %d ", DT_METADATA_XMP_DC_DESCRIPTION);
    // clang-format on
  }
  /* PATH and TITLE */
  else if(collection->params.sorts[DT_COLLECTION_SORT_TITLE]
          && collection->params.sorts[DT_COLLECTION_SORT_PATH]
          && (collection->params.query_flags & COLLECTION_QUERY_USE_SORT))
  {
    _dt_collection_set_selq_pre_sort(collection, &selq_pre);
    // clang-format off
    selq_post = dt_util_dstrcat(selq_post, ") AS mi JOIN (SELECT id AS film_rolls_id, folder FROM main.film_rolls) ON film_id = film_rolls_id"
                                                                        " LEFT OUTER JOIN main.meta_data AS m ON mi.id = m.id AND m.key = %d",DT_METADATA_XMP_DC_TITLE);
    // clang-format on
  }
  /* PATH and DESCRIPTION */
  else if(collection->params.sorts[DT_COLLECTION_SORT_DESCRIPTION]
          && collection->params.sorts[DT_COLLECTION_SORT_PATH]
          && (collection->params.query_flags & COLLECTION_QUERY_USE_SORT))
  {
    _dt_collection_set_selq_pre_sort(collection, &selq_pre);
    // clang-format off
    selq_post = dt_util_dstrcat
      (selq_post,
       ") AS mi JOIN (SELECT id AS film_rolls_id, folder FROM main.film_rolls) ON film_id = film_rolls_id"
       " LEFT OUTER JOIN main.meta_data AS m ON mi.id = m.id AND m.key = %d", DT_METADATA_XMP_DC_DESCRIPTION);
    // clang-format on
  }
  /* TITLE and DESCRIPTION */
  else if(collection->params.sorts[DT_COLLECTION_SORT_TITLE]
          && collection->params.sorts[DT_COLLECTION_SORT_DESCRIPTION]
          && (collection->params.query_flags & COLLECTION_QUERY_USE_SORT))
  {
    _dt_collection_set_selq_pre_sort(collection, &selq_pre);
    // clang-format off
    selq_post = dt_util_dstrcat
      (selq_post,
       ") AS mi LEFT OUTER JOIN main.meta_data AS m ON mi.id = m.id AND (m.key = %d OR m.key = %d)",
       DT_METADATA_XMP_DC_TITLE, DT_METADATA_XMP_DC_DESCRIPTION);
    // clang-format on
  }
  /* only COLOR */
  else if(collection->params.sorts[DT_COLLECTION_SORT_COLOR]
          && (collection->params.query_flags & COLLECTION_QUERY_USE_SORT))
  {
    _dt_collection_set_selq_pre_sort(collection, &selq_pre);
    // clang-format off
    selq_post = dt_util_dstrcat(selq_post, ") AS mi LEFT OUTER JOIN main.color_labels AS b ON mi.id = b.imgid");
    // clang-format on
  }
  /* only PATH */
  else if(collection->params.sorts[DT_COLLECTION_SORT_PATH]
          && (collection->params.query_flags & COLLECTION_QUERY_USE_SORT))
  {
    _dt_collection_set_selq_pre_sort(collection, &selq_pre);
    // clang-format off
    selq_post = dt_util_dstrcat
      (selq_post,
       ") AS mi JOIN (SELECT id AS film_rolls_id, folder FROM main.film_rolls) ON film_id = film_rolls_id");
    // clang-format on
  }
  /* only TITLE */
  else if(collection->params.sorts[DT_COLLECTION_SORT_TITLE]
          && (collection->params.query_flags & COLLECTION_QUERY_USE_SORT))
  {
    _dt_collection_set_selq_pre_sort(collection, &selq_pre);
    // clang-format off
    selq_post = dt_util_dstrcat(selq_post, ") AS mi LEFT OUTER JOIN main.meta_data AS m ON mi.id = m.id AND m.key = %d ",
                                DT_METADATA_XMP_DC_TITLE);
    // clang-format on
  }
  /* only DESCRIPTION */
  else if(collection->params.sorts[DT_COLLECTION_SORT_DESCRIPTION]
          && (collection->params.query_flags & COLLECTION_QUERY_USE_SORT))
  {
    _dt_collection_set_selq_pre_sort(collection, &selq_pre);
    // clang-format off
    selq_post = dt_util_dstrcat
      (selq_post, ") AS mi LEFT OUTER JOIN main.meta_data AS m ON mi.id = m.id AND m.key = %d ",
       DT_METADATA_XMP_DC_DESCRIPTION);
    // clang-format on
  }
  else if(collection->params.query_flags & COLLECTION_QUERY_USE_ONLY_WHERE_EXT)
  {
    const uint32_t tagid = collection->tagid;
    char tag[16] = { 0 };
    snprintf(tag, sizeof(tag), "%u", tagid);
    // clang-format off
    selq_pre = dt_util_dstrcat(selq_pre,
                               "SELECT DISTINCT mi.id FROM (SELECT"
                               "  id, group_id, film_id, filename, datetime_taken, "
                               "  flags, version, %s position, aspect_ratio,"
                               "  maker, model, lens, aperture, exposure, focal_length,"
                               "  iso, import_timestamp, change_timestamp,"
                               "  export_timestamp, print_timestamp"
                               "  FROM main.images AS mi %s%s ) AS mi ",
                               tagid ? "CASE WHEN ti.position IS NULL THEN 0 ELSE ti.position END AS" : "",
                               tagid ? " LEFT JOIN main.tagged_images AS ti"
                                       " ON ti.imgid = mi.id AND ti.tagid = " : "",
                               tagid ? tag : "");
    // clang-format on
  }
  else
  {
    const uint32_t tagid = collection->tagid;
    char tag[16] = { 0 };
    snprintf(tag, sizeof(tag), "%u", tagid);
    // clang-format off
    selq_pre = dt_util_dstrcat(selq_pre,
                               "SELECT DISTINCT mi.id FROM (SELECT"
                               "  id, group_id, film_id, filename, datetime_taken, "
                               "  flags, version, %s position, aspect_ratio,"
                               "  maker, model, lens, aperture, exposure, focal_length,"
                               "  iso, import_timestamp, change_timestamp,"
                               "  export_timestamp, print_timestamp"
                               "  FROM main.images AS mi %s%s ) AS mi WHERE ",
                               tagid ? "CASE WHEN ti.position IS NULL THEN 0 ELSE ti.position END AS" : "",
                               tagid ? " LEFT JOIN main.tagged_images AS ti"
                                       " ON ti.imgid = mi.id AND ti.tagid = " : "",
                               tagid ? tag : "");
    // clang-format on
  }


  /* build sort order part */
  if(!(collection->params.query_flags & COLLECTION_QUERY_USE_ONLY_WHERE_EXT)
     && (collection->params.query_flags & COLLECTION_QUERY_USE_SORT))
  {
    sq = dt_collection_get_sort_query(collection);
  }

  /* store the new query */
  query
      = dt_util_dstrcat(query, "%s%s%s %s%s", selq_pre, wq, selq_post ? selq_post : "", sq ? sq : "",
                        (collection->params.query_flags & COLLECTION_QUERY_USE_LIMIT) ? " " LIMIT_QUERY : "");
  query_no_group
      = dt_util_dstrcat(query_no_group, "%s%s%s %s%s", selq_pre, wq_no_group, selq_post ? selq_post : "", sq ? sq : "",
                        (collection->params.query_flags & COLLECTION_QUERY_USE_LIMIT) ? " " LIMIT_QUERY : "");
  result = _dt_collection_store(collection, query, query_no_group);

  /* free memory used */
  g_free(sq);
  g_free(wq);
  g_free(wq_no_group);
  g_free(selq_pre);
  g_free(selq_post);
  g_free(query);
  g_free(query_no_group);

  /* update the cached count. collection isn't a real const anyway, we are writing to it in
   * _dt_collection_store, too. */
  ((dt_collection_t *)collection)->count = _dt_collection_compute_count(collection, FALSE);
  ((dt_collection_t *)collection)->count_no_group = _dt_collection_compute_count(collection, TRUE);
  dt_collection_hint_message(collection);

  _collection_update_aspect_ratio(collection);

  return result;
}

void dt_collection_reset(const dt_collection_t *collection)
{
  dt_collection_params_t *params = (dt_collection_params_t *)&collection->params;

  /* setup defaults */
  params->query_flags = COLLECTION_QUERY_FULL;
  params->filter_flags = COLLECTION_FILTER_FILM_ID | COLLECTION_FILTER_ATLEAST_RATING;
  params->film_id = 1;

  /* apply stored query parameters from previous darktable session */
  params->film_id = dt_conf_get_int("plugins/collection/film_id");
  params->filter_flags = dt_conf_get_int("plugins/collection/filter_flags");
  dt_collection_update_query(collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);
}

const gchar *dt_collection_get_query(const dt_collection_t *collection)
{
  /* ensure there is a query string for collection */
  if(!collection->query) dt_collection_update(collection);

  return collection->query;
}

const gchar *dt_collection_get_query_no_group(const dt_collection_t *collection)
{
  /* ensure there is a query string for collection */
  if(!collection->query_no_group) dt_collection_update(collection);

  return collection->query_no_group;
}
uint32_t dt_collection_get_filter_flags(const dt_collection_t *collection)
{
  return collection->params.filter_flags;
}

void dt_collection_set_filter_flags(const dt_collection_t *collection, uint32_t flags)
{
  dt_collection_params_t *params = (dt_collection_params_t *)&collection->params;
  params->filter_flags = flags;
}

uint32_t dt_collection_get_query_flags(const dt_collection_t *collection)
{
  return collection->params.query_flags;
}

void dt_collection_set_query_flags(const dt_collection_t *collection, uint32_t flags)
{
  dt_collection_params_t *params = (dt_collection_params_t *)&collection->params;
  params->query_flags = flags;
}

gchar *dt_collection_get_extended_where(const dt_collection_t *collection, int exclude)
{
  gchar *complete_string = NULL;

  if(exclude >= 0)
  {
    complete_string = g_strdup("");
    char confname[200];
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", exclude);
    const int mode = dt_conf_get_int(confname);
    // we only want collect rules, not filtering ones
    const int nb_rules = CLAMP(dt_conf_get_int("plugins/lighttable/collect/num_rules"), 1, 10);
    for(int i = 0; (i < nb_rules && collection->where_ext[i] != NULL); i++)
    {
      // exclude the one rule from extended where
      if(i != exclude || mode == 1)
        complete_string = dt_util_dstrcat(complete_string, "%s", collection->where_ext[i]);
    }
  }
  else
    complete_string = g_strjoinv(complete_string, ((dt_collection_t *)collection)->where_ext);

  gchar *where_ext = g_strdup_printf("(1=1%s)", complete_string);
  g_free(complete_string);

  return where_ext;
}

void dt_collection_set_extended_where(const dt_collection_t *collection, gchar **extended_where)
{
  /* free extended where if already exists */
  g_strfreev(collection->where_ext);

  /* set new from parameter */
  ((dt_collection_t *)collection)->where_ext = g_strdupv(extended_where);
}

void dt_collection_set_film_id(const dt_collection_t *collection, const int32_t film_id)
{
  dt_collection_params_t *params = (dt_collection_params_t *)&collection->params;
  params->film_id = film_id;
}

void dt_collection_set_tag_id(dt_collection_t *collection, const uint32_t tagid)
{
  collection->tagid = tagid;
}

static void _collection_update_aspect_ratio(const dt_collection_t *collection)
{
  dt_collection_params_t *params = (dt_collection_params_t *)&collection->params;

  //  Update the aspect ratio for selected images in the collection if needed, we do not do this for all
  //  images as it could take a long time. The aspect ratio is then updated when needed, and at some
  //  point all aspect ratio for all images will be set and this could won't be really needed.

  if(params->sorts[DT_COLLECTION_SORT_ASPECT_RATIO])
  {
    const float MAX_TIME = 7.0;
    const gchar *where_ext = dt_collection_get_extended_where(collection, -1);
    sqlite3_stmt *stmt = NULL;

    // clang-format off
    gchar *query = g_strdup_printf(
       "SELECT id"
       " FROM main.images"
       " WHERE %s AND (aspect_ratio=0.0 OR aspect_ratio IS NULL)", where_ext);
    // clang-format on

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);

    double start = dt_get_wtime();
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      const int imgid = sqlite3_column_int(stmt, 0);
      dt_image_set_raw_aspect_ratio(imgid);

      if(dt_get_wtime() - start > MAX_TIME)
      {
        dt_control_log(_("too much time to update aspect ratio for the collection"));
        break;
      }
    }
    sqlite3_finalize(stmt);
    g_free(query);
  }
}

const char *dt_collection_sort_name_untranslated(dt_collection_sort_t sort)
{
  switch(sort)
  {
    case DT_COLLECTION_SORT_FILENAME:
      return N_("filename");
    case DT_COLLECTION_SORT_DATETIME:
      return N_("capture time");
    case DT_COLLECTION_SORT_IMPORT_TIMESTAMP:
      return N_("import time");
    case DT_COLLECTION_SORT_CHANGE_TIMESTAMP:
      return N_("modification time");
    case DT_COLLECTION_SORT_EXPORT_TIMESTAMP:
      return N_("export time");
    case DT_COLLECTION_SORT_PRINT_TIMESTAMP:
      return N_("print time");
    case DT_COLLECTION_SORT_RATING:
      return N_("rating");
    case DT_COLLECTION_SORT_ID:
      return N_("id");
    case DT_COLLECTION_SORT_COLOR:
      return N_("color label");
    case DT_COLLECTION_SORT_GROUP:
      return N_("group");
    case DT_COLLECTION_SORT_PATH:
      return N_("full path");
    case DT_COLLECTION_SORT_CUSTOM_ORDER:
      return N_("custom sort");
    case DT_COLLECTION_SORT_TITLE:
      return N_("title");
    case DT_COLLECTION_SORT_DESCRIPTION:
      return N_("description");
    case DT_COLLECTION_SORT_ASPECT_RATIO:
      return N_("aspect ratio");
    case DT_COLLECTION_SORT_SHUFFLE:
      return N_("shuffle");
    default:
      return "";
  }
};

const char *dt_collection_sort_name(dt_collection_sort_t sort)
{
  return _(dt_collection_sort_name_untranslated(sort));
};

const char *dt_collection_name_untranslated(dt_collection_properties_t prop)
{
  char *col_name = NULL;
  switch(prop)
  {
    case DT_COLLECTION_PROP_FILMROLL:
      return N_("film roll");
    case DT_COLLECTION_PROP_FOLDERS:
      return N_("folder");
    case DT_COLLECTION_PROP_CAMERA:
      return N_("camera");
    case DT_COLLECTION_PROP_TAG:
      return N_("tag");
    case DT_COLLECTION_PROP_DAY:
      return N_("capture date");
    case DT_COLLECTION_PROP_TIME:
      return N_("capture time");
    case DT_COLLECTION_PROP_IMPORT_TIMESTAMP:
      return N_("import time");
    case DT_COLLECTION_PROP_CHANGE_TIMESTAMP:
      return N_("modification time");
    case DT_COLLECTION_PROP_EXPORT_TIMESTAMP:
      return N_("export time");
    case DT_COLLECTION_PROP_PRINT_TIMESTAMP:
      return N_("print time");
    case DT_COLLECTION_PROP_HISTORY:
      return N_("history");
    case DT_COLLECTION_PROP_COLORLABEL:
      return N_("color label");
    case DT_COLLECTION_PROP_LENS:
      return N_("lens");
    case DT_COLLECTION_PROP_FOCAL_LENGTH:
      return N_("focal length");
    case DT_COLLECTION_PROP_ISO:
      return N_("ISO");
    case DT_COLLECTION_PROP_APERTURE:
      return N_("aperture");
    case DT_COLLECTION_PROP_EXPOSURE:
      return N_("exposure");
    case DT_COLLECTION_PROP_ASPECT_RATIO:
      return N_("aspect ratio");
    case DT_COLLECTION_PROP_FILENAME:
      return N_("filename");
    case DT_COLLECTION_PROP_GEOTAGGING:
      return N_("geotagging");
    case DT_COLLECTION_PROP_GROUPING:
      return N_("grouping");
    case DT_COLLECTION_PROP_LOCAL_COPY:
      return N_("local copy");
    case DT_COLLECTION_PROP_MODULE:
      return N_("module");
    case DT_COLLECTION_PROP_ORDER:
      return N_("module order");
    case DT_COLLECTION_PROP_RATING_RANGE:
      return N_("range rating");
    case DT_COLLECTION_PROP_RATING:
      return N_("rating");
    case DT_COLLECTION_PROP_TEXTSEARCH:
      return N_("search");
    case DT_COLLECTION_PROP_LAST:             return NULL;
    default:
    {
      if(prop >= DT_COLLECTION_PROP_METADATA
         && prop < DT_COLLECTION_PROP_METADATA + DT_METADATA_NUMBER)
      {
        const int i = prop - DT_COLLECTION_PROP_METADATA;
        const int type = dt_metadata_get_type_by_display_order(i);
        if(type != DT_METADATA_TYPE_INTERNAL)
        {
          char *name = (gchar *)dt_metadata_get_name_by_display_order(i);
          char *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", name);
          const gboolean hidden = dt_conf_get_int(setting) & DT_METADATA_FLAG_HIDDEN;
          free(setting);
          if(!hidden) col_name = name;
        }
      }
    }
  }
  return col_name;
}

const char *dt_collection_name(dt_collection_properties_t prop)
{
  return _(dt_collection_name_untranslated(prop));
};

static gchar *_dt_collection_get_sort_text(const dt_collection_sort_t sort, const int sortorder)
{
  // construct the text query
  gchar *sq = NULL;
  switch(sort)
  {
    case DT_COLLECTION_SORT_DATETIME:
    case DT_COLLECTION_SORT_IMPORT_TIMESTAMP:
    case DT_COLLECTION_SORT_CHANGE_TIMESTAMP:
    case DT_COLLECTION_SORT_EXPORT_TIMESTAMP:
    case DT_COLLECTION_SORT_PRINT_TIMESTAMP:
      {
        const int local_order = sort;
        char *colname;

        switch(local_order)
        {
          case DT_COLLECTION_SORT_DATETIME:
            colname = "datetime_taken";
            break;
          case DT_COLLECTION_SORT_IMPORT_TIMESTAMP:
            colname = "import_timestamp";
            break;
          case DT_COLLECTION_SORT_CHANGE_TIMESTAMP:
            colname = "change_timestamp";
            break;
          case DT_COLLECTION_SORT_EXPORT_TIMESTAMP:
            colname = "export_timestamp";
            break;
          case DT_COLLECTION_SORT_PRINT_TIMESTAMP:
            colname = "print_timestamp";
            break;
          default:
            colname = "";
        }
        sq = g_strdup_printf("%s%s", colname, (sortorder) ? " DESC" : "");
        break;
      }

    case DT_COLLECTION_SORT_RATING:
      sq = g_strdup_printf("CASE WHEN flags & 8 = 8 THEN -1 ELSE flags & 7 END%s", (sortorder) ? " DESC" : "");
      break;

    case DT_COLLECTION_SORT_FILENAME:
      sq = g_strdup_printf("filename%s", (sortorder) ? " DESC" : "");
      break;

    case DT_COLLECTION_SORT_ID:
      sq = g_strdup_printf("mi.id%s", (sortorder) ? " DESC" : "");
      break;

    case DT_COLLECTION_SORT_COLOR:
      sq = g_strdup_printf("color%s", (sortorder) ? "" : " DESC");
      break;

    case DT_COLLECTION_SORT_GROUP:
      sq = g_strdup_printf("group_id%s, mi.id-group_id != 0, mi.id", (sortorder) ? " DESC" : "");
      break;

    case DT_COLLECTION_SORT_PATH:
      sq = g_strdup_printf("folder%s, filename%s", (sortorder) ? " DESC" : "", (sortorder) ? " DESC" : "");
      break;

    case DT_COLLECTION_SORT_CUSTOM_ORDER:
      sq = g_strdup_printf("position%s", (sortorder) ? " DESC" : "");
      break;

    case DT_COLLECTION_SORT_TITLE:
      sq = g_strdup_printf("m.value%s", (sortorder) ? " DESC" : "");
      break;

    case DT_COLLECTION_SORT_DESCRIPTION:
      sq = g_strdup_printf("m.value%s", (sortorder) ? " DESC" : "");
      break;

    case DT_COLLECTION_SORT_ASPECT_RATIO:
      sq = g_strdup_printf("aspect_ratio%s", (sortorder) ? " DESC" : "");
      break;

    case DT_COLLECTION_SORT_SHUFFLE:
      sq = g_strdup("RANDOM()"); /* do not consider second order for shuffle */
      /* do not remember shuffle for second order */
      break;

    case DT_COLLECTION_SORT_NONE:
    default: /*fall through for default*/
      // shouldn't happen
      sq = g_strdup("mi.id");
      break;
  }

  return sq;
}

gchar *dt_collection_get_sort_query(const dt_collection_t *collection)
{
  gboolean filename = FALSE;
  int first_order = 0;
  const dt_collection_sort_t lastsort = dt_conf_get_int("plugins/lighttable/filtering/lastsort");
  const int lastsortorder = dt_conf_get_int("plugins/lighttable/filtering/lastsortorder");
  gboolean already_last_sort = FALSE;
  gchar *query = g_strdup("ORDER BY");

  const int nb_sort = CLAMP(dt_conf_get_int("plugins/lighttable/filtering/num_sort"), 0, DT_COLLECTION_MAX_RULES);
  for(int i = 0; i < nb_sort; i++)
  {
    // read the sort value from conf
    char confname[200] = { 0 };
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sort%1d", i);
    const dt_collection_sort_t sort = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sortorder%1d", i);
    const int sortorder = dt_conf_get_int(confname);

    // get the sort query
    gchar *sq = _dt_collection_get_sort_text(sort, sortorder);
    query = dt_util_dstrcat(query, "%s %s", (i == 0) ? "" : ",", sq);
    g_free(sq);

    // set the "already done" values
    if(sort == DT_COLLECTION_SORT_FILENAME) filename = TRUE;
    if(i == 0) first_order = sortorder;
    if(sort == lastsort) already_last_sort = TRUE;
  }

  // and last sort order set
  if(!already_last_sort)
  {
    gchar *lsq = _dt_collection_get_sort_text(lastsort, lastsortorder);
    query = dt_util_dstrcat(query, ", %s", lsq);
    g_free(lsq);
    if(lastsort == DT_COLLECTION_SORT_FILENAME) filename = TRUE;
  }

  // complete the query with fallback if needed
  if(!filename) query = dt_util_dstrcat(query, ", filename%s", (first_order) ? " DESC" : "");

  query = dt_util_dstrcat(query, ", version%s", (first_order) ? " DESC" : "");

  return query;
}


static int _dt_collection_store(const dt_collection_t *collection, gchar *query, gchar *query_no_group)
{
  /* store flags to conf */
  if(collection == darktable.collection)
  {
    dt_conf_set_int("plugins/collection/query_flags", collection->params.query_flags);
    dt_conf_set_int("plugins/collection/filter_flags", collection->params.filter_flags);
    dt_conf_set_int("plugins/collection/film_id", collection->params.film_id);
  }

  /* store query in context */
  g_free(collection->query);
  g_free(collection->query_no_group);

  ((dt_collection_t *)collection)->query = g_strdup(query);
  ((dt_collection_t *)collection)->query_no_group = g_strdup(query_no_group);

  return 1;
}

static uint32_t _dt_collection_compute_count(const dt_collection_t *collection, gboolean no_group)
{
  sqlite3_stmt *stmt = NULL;
  uint32_t count = 1;
  const gchar *query = no_group ? dt_collection_get_query_no_group(collection) : dt_collection_get_query(collection);
  gchar *count_query = NULL;

  gchar *fq = g_strstr_len(query, strlen(query), "FROM");
  if((collection->params.query_flags & COLLECTION_QUERY_USE_ONLY_WHERE_EXT))
  {
    gchar *where_ext = dt_collection_get_extended_where(collection, -1);
    count_query = g_strdup_printf("SELECT COUNT(DISTINCT main.images.id) FROM main.images AS mi %s", where_ext);
    g_free(where_ext);
  }
  else
    count_query = g_strdup_printf("SELECT COUNT(DISTINCT mi.id) %s", fq);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), count_query, -1, &stmt, NULL);
  if((collection->params.query_flags & COLLECTION_QUERY_USE_LIMIT)
     && !(collection->params.query_flags & COLLECTION_QUERY_USE_ONLY_WHERE_EXT))
  {
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, 0);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, -1);
  }

  if(sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  g_free(count_query);
  return count;
}

uint32_t dt_collection_get_count(const dt_collection_t *collection)
{
  return collection->count;
}

uint32_t dt_collection_get_count_no_group(const dt_collection_t *collection)
{
  return collection->count_no_group;
}

uint32_t dt_collection_get_selected_count(const dt_collection_t *collection)
{
  sqlite3_stmt *stmt = NULL;
  uint32_t count = 0;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT COUNT(*) FROM main.selected_images", -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return count;
}

GList *dt_collection_get(const dt_collection_t *collection, int limit, gboolean selected)
{
  GList *list = NULL;
  const gchar *query = dt_collection_get_query_no_group(collection);
  if(query)
  {
    sqlite3_stmt *stmt = NULL;
    if(selected)
    {
      // clang-format off
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "SELECT mi.imgid FROM main.selected_images AS s"
                                  " JOIN memory.collected_images AS mi WHERE mi.imgid = s.imgid LIMIT -1, ?1",
                                  -1, &stmt, NULL);
      // clang-format on
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, limit);
    }
    else
    {
      if(collection->params.query_flags & COLLECTION_QUERY_USE_LIMIT)
      {
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "SELECT imgid FROM memory.collected_images LIMIT -1, ?1",
                                    -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, limit);
      }
      else
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "SELECT imgid FROM memory.collected_images",
                                    -1, &stmt, NULL);
    }

    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      const int imgid = sqlite3_column_int(stmt, 0);
      list = g_list_prepend(list, GINT_TO_POINTER(imgid));
    }

    sqlite3_finalize(stmt);
  }

  return g_list_reverse(list);  // list built in reverse order, so un-reverse it
}

GList *dt_collection_get_all(const dt_collection_t *collection, int limit)
{
  return dt_collection_get(collection, limit, FALSE);
}

int dt_collection_get_nth(const dt_collection_t *collection, int nth)
{
  if(nth < 0 || nth >= dt_collection_get_count(collection))
    return -1;
  const gchar *query = dt_collection_get_query(collection);
  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, nth);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, 1);

  int result = -1;
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    result  = sqlite3_column_int(stmt, 0);
  }

  sqlite3_finalize(stmt);

  return result;

}

GList *dt_collection_get_selected(const dt_collection_t *collection, int limit)
{
  return dt_collection_get(collection, limit, TRUE);
}

/* splits an input string into a number part and an optional operator part.
   number can be a decimal integer or rational numerical item.
   operator can be any of "=", "<", ">", "<=", ">=" and "<>".
   range notation [x;y] can also be used

   number and operator are returned as pointers to null terminated strings in g_mallocated
   memory (to be g_free'd after use) - or NULL if no match is found.
*/
void dt_collection_split_operator_number(const gchar *input, char **number1, char **number2, char **operator)
{
  GRegex *regex;
  GMatchInfo *match_info;

  *number1 = *number2 = *operator= NULL;

  // we test the range expression first
  regex = g_regex_new("^\\s*\\[\\s*([-+]?[0-9]+\\.?[0-9]*)\\s*;\\s*([-+]?[0-9]+\\.?[0-9]*)\\s*\\]\\s*$", 0, 0, NULL);
  g_regex_match_full(regex, input, -1, 0, 0, &match_info, NULL);
  int match_count = g_match_info_get_match_count(match_info);

  if(match_count == 3)
  {
    *number1 = g_match_info_fetch(match_info, 1);
    *number2 = g_match_info_fetch(match_info, 2);
    *operator= g_strdup("[]");
    g_match_info_free(match_info);
    g_regex_unref(regex);
    return;
  }

  g_match_info_free(match_info);
  g_regex_unref(regex);

  // and we test the classic comparison operators
  regex = g_regex_new("^\\s*(=|<|>|<=|>=|<>)?\\s*([-+]?[0-9]+\\.?[0-9]*)\\s*$", 0, 0, NULL);
  g_regex_match_full(regex, input, -1, 0, 0, &match_info, NULL);
  match_count = g_match_info_get_match_count(match_info);

  if(match_count == 3)
  {
    *operator= g_match_info_fetch(match_info, 1);
    *number1 = g_match_info_fetch(match_info, 2);

    if(*operator && strcmp(*operator, "") == 0)
    {
      g_free(*operator);
      *operator= NULL;
    }
  }

  g_match_info_free(match_info);
  g_regex_unref(regex);
}

static char *_dt_collection_compute_datetime(const char *operator, const char *input)
{
  if(strlen(input) < 3) return NULL;

  char bound[DT_DATETIME_LENGTH];
  gboolean res;
  if(strcmp(operator, ">") == 0 || strcmp(operator, "<=") == 0)
    res = dt_datetime_entry_to_exif_upper_bound(bound, sizeof(bound), input);
  else
    res = dt_datetime_entry_to_exif(bound, sizeof(bound), input);
  if(res)
    return g_strdup(bound);
  else return NULL;
}
/* splits an input string into a date-time part and an optional operator part.
   operator can be any of "=", "<", ">", "<=", ">=" and "<>".
   range notation [x;y] can also be used
   datetime values should follow the pattern YYYY:MM:DD hh:mm:ss.sss
   but only year part is mandatory

   datetime and operator are returned as pointers to null terminated strings in g_mallocated
   memory (to be g_free'd after use) - or NULL if no match is found.
*/
void dt_collection_split_operator_datetime(const gchar *input, char **number1, char **number2, char **operator)
{
  GRegex *regex;
  GMatchInfo *match_info;

  *number1 = *number2 = *operator= NULL;

  // we test the range expression first
  // 2 elements : date-time1 and  date-time2
  regex = g_regex_new("^\\s*\\[\\s*([+-]?\\d{4}[:.\\d\\s]*)\\s*;\\s*((?:now)|[+-]?\\d{4}[:.\\d\\s]*)\\s*\\]\\s*$",
                      0, 0, NULL);
  g_regex_match_full(regex, input, -1, 0, 0, &match_info, NULL);
  int match_count = g_match_info_get_match_count(match_info);

  if(match_count == 3)
  {
    gchar *txt = g_match_info_fetch(match_info, 1);
    gchar *txt2 = g_match_info_fetch(match_info, 2);

    if(!g_str_has_prefix(txt, "-")) *number1 = _dt_collection_compute_datetime(">=", txt);
    if(!g_str_has_prefix(txt2, "+")) *number2 = _dt_collection_compute_datetime("<=", txt2);

    // special handle of relative dates
    if(g_str_has_prefix(txt, "-") && *number2)
    {
      dt_datetime_t relative;
      dt_datetime_exif_to_numbers_raw(&relative, txt + 1);
      dt_datetime_exif_add_numbers(*number2, relative, FALSE, &(*number1));
    }
    else if(g_str_has_prefix(txt2, "+") && *number1)
    {
      dt_datetime_t relative;
      dt_datetime_exif_to_numbers_raw(&relative, txt2 + 1);
      dt_datetime_exif_add_numbers(*number1, relative, TRUE, &(*number2));
    }

    *operator= g_strdup("[]");

    g_free(txt);
    g_free(txt2);
    g_match_info_free(match_info);
    g_regex_unref(regex);
    return;
  }

  g_match_info_free(match_info);
  g_regex_unref(regex);

  // and we test the classic comparison operators
  // 2 elements : operator and date-time
  regex = g_regex_new("^\\s*(=|<|>|<=|>=|<>)?\\s*(\\d{4}[:.\\d\\s]*)?\\s*%?\\s*$", 0, 0, NULL);
  g_regex_match_full(regex, input, -1, 0, 0, &match_info, NULL);
  match_count = g_match_info_get_match_count(match_info);

  if(match_count == 3)
  {
    *operator= g_match_info_fetch(match_info, 1);
    gchar *txt = g_match_info_fetch(match_info, 2);

    if(strcmp(*operator, "") == 0 || strcmp(*operator, "=") == 0 || strcmp(*operator, "<>") == 0)
    {
      *number1 = dt_util_dstrcat(*number1, "%s%%", txt);
      *number2 = _dt_collection_compute_datetime(">", txt);
    }
    else
      *number1 = _dt_collection_compute_datetime(*operator, txt);

    g_free(txt);
  }

  // ensure operator is not null
  if(!*operator) *operator= g_strdup("");

  g_match_info_free(match_info);
  g_regex_unref(regex);
}

void dt_collection_split_operator_exposure(const gchar *input, char **number1, char **number2, char **operator)
{
  GRegex *regex;
  GMatchInfo *match_info;

  *number1 = *number2 = *operator= NULL;

  // we test the range expression first
  regex = g_regex_new("^\\s*\\[\\s*(1/)?([0-9]+\\.?[0-9]*)(\")?\\s*;\\s*(1/)?([0-9]+\\.?[0-9]*)(\")?\\s*\\]\\s*$", 0, 0, NULL);
  g_regex_match_full(regex, input, -1, 0, 0, &match_info, NULL);
  int match_count = g_match_info_get_match_count(match_info);

  if(match_count == 6 || match_count == 7)
  {
    gchar *n1 = g_match_info_fetch(match_info, 2);

    if(strstr(g_match_info_fetch(match_info, 1), "1/") != NULL)
      *number1 = g_strdup_printf("1.0/%s", n1);
    else
      *number1 = n1;

    gchar *n2 = g_match_info_fetch(match_info, 5);

    if(strstr(g_match_info_fetch(match_info, 4), "1/") != NULL)
      *number2 = g_strdup_printf("1.0/%s", n2);
    else
      *number2 = n2;

    *operator= g_strdup("[]");
    g_match_info_free(match_info);
    g_regex_unref(regex);
    return;
  }

  g_match_info_free(match_info);
  g_regex_unref(regex);

  // and we test the classic comparison operators
  regex = g_regex_new("^\\s*(=|<|>|<=|>=|<>)?\\s*(1/)?([0-9]+\\.?[0-9]*)(\")?\\s*$", 0, 0, NULL);
  g_regex_match_full(regex, input, -1, 0, 0, &match_info, NULL);
  match_count = g_match_info_get_match_count(match_info);
  if(match_count == 4 || match_count == 5)
  {
    *operator= g_match_info_fetch(match_info, 1);

    gchar *n1 = g_match_info_fetch(match_info, 3);

    if(strstr(g_match_info_fetch(match_info, 2), "1/") != NULL)
      *number1 = g_strdup_printf("1.0/%s", n1);
    else
      *number1 = n1;

    if(*operator && strcmp(*operator, "") == 0)
    {
      g_free(*operator);
      *operator= NULL;
    }
  }

  g_match_info_free(match_info);
  g_regex_unref(regex);
}

void dt_collection_get_makermodels(const gchar *filter, GList **sanitized, GList **exif)
{
  sqlite3_stmt *stmt;
  gchar *needle = NULL;
  gboolean wildcard = FALSE;

  GHashTable *names = NULL;
  if(sanitized)
    names = g_hash_table_new(g_str_hash, g_str_equal);

  if(filter && filter[0] != '\0')
  {
    needle = g_utf8_strdown(filter, -1);
    wildcard = (needle && needle[strlen(needle) - 1] == '%') ? TRUE : FALSE;
    if(wildcard)
      needle[strlen(needle) - 1] = '\0';
  }

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT maker, model FROM main.images GROUP BY maker, model",
                              -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *exif_maker = (char *)sqlite3_column_text(stmt, 0);
    const char *exif_model = (char *)sqlite3_column_text(stmt, 1);

    gchar *makermodel =  dt_collection_get_makermodel(exif_maker, exif_model);

    gchar *haystack = g_utf8_strdown(makermodel, -1);
    if(!needle || (wildcard && g_strrstr(haystack, needle) != NULL)
                || (!wildcard && !g_strcmp0(haystack, needle)))
    {
      if(exif)
      {
        // Append a two element list with maker and model
        GList *inner_list = NULL;
        inner_list = g_list_append(inner_list, g_strdup(exif_maker));
        inner_list = g_list_append(inner_list, g_strdup(exif_model));
        *exif = g_list_append(*exif, inner_list);
      }

      if(sanitized)
      {
        gchar *key = g_strdup(makermodel);
        g_hash_table_add(names, key);
      }
    }
    g_free(haystack);
    g_free(makermodel);
  }
  sqlite3_finalize(stmt);
  g_free(needle);

  if(sanitized)
  {
    *sanitized = g_list_sort(g_hash_table_get_keys(names), (GCompareFunc) strcmp);
    g_hash_table_destroy(names);
  }
}

gchar *dt_collection_get_makermodel(const char *exif_maker, const char *exif_model)
{
  char maker[64];
  char model[64];
  char alias[64];
  maker[0] = model[0] = alias[0] = '\0';
  dt_imageio_lookup_makermodel(exif_maker, exif_model,
                               maker, sizeof(maker),
                               model, sizeof(model),
                               alias, sizeof(alias));

  // Create the makermodel by concatenation
  gchar *makermodel = g_strdup_printf("%s %s", maker, model);
  return makermodel;
}

static gchar *get_query_string(const dt_collection_properties_t property, const gchar *text)
{
  char *escaped_text = sqlite3_mprintf("%q", text);
  const unsigned int escaped_length = strlen(escaped_text);
  gchar *query = NULL;

  switch(property)
  {
    case DT_COLLECTION_PROP_FILMROLL: // film roll
      if(!(escaped_text && *escaped_text))
        // clang-format off
        query = g_strdup_printf("(film_id IN (SELECT id FROM main.film_rolls WHERE folder LIKE '%s%%'))",
                                escaped_text);
        // clang-format on
      else
        // clang-format off
        query = g_strdup_printf("(film_id IN (SELECT id FROM main.film_rolls WHERE folder LIKE '%s'))",
                                escaped_text);
        // clang-format on
      break;

    case DT_COLLECTION_PROP_FOLDERS: // folders
      {
        // replace * at the end with OR-clause to include subfolders
        if((escaped_length > 0) && (escaped_text[escaped_length-1] == '*'))
        {
          escaped_text[escaped_length-1] = '\0';
          // clang-format off
          query = g_strdup_printf("(film_id IN (SELECT id FROM main.film_rolls WHERE folder LIKE '%s' OR folder LIKE '%s"
                                  G_DIR_SEPARATOR_S "%%'))",
                                  escaped_text, escaped_text);
          // clang-format on
        }
        // replace |% at the end with /% to only show subfolders
        else if((escaped_length > 1) && (strcmp(escaped_text+escaped_length-2, "|%") == 0 ))
        {
          escaped_text[escaped_length-2] = '\0';
          // clang-format off
          query = g_strdup_printf("(film_id IN (SELECT id FROM main.film_rolls WHERE folder LIKE '%s"
                                  G_DIR_SEPARATOR_S "%%'))",
                                  escaped_text);
          // clang-format on
        }
        else
        {
          // clang-format off
          query = g_strdup_printf("(film_id IN (SELECT id FROM main.film_rolls WHERE folder LIKE '%s'))",
                                  escaped_text);
          // clang-format on
        }
      }
      break;

    case DT_COLLECTION_PROP_COLORLABEL: // colorlabel
    {
      if(!(escaped_text && *escaped_text) || strcmp(escaped_text, "%") == 0)
        // clang-format off
        query = g_strdup_printf("(id IN (SELECT imgid FROM main.color_labels WHERE color IS NOT NULL))");
        // clang-format on
      else
      {
        // test the "mask" case (used by filters)
        if(g_str_has_prefix(text, "0x"))
        {
          const int val = strtoll(&escaped_text[2], NULL, 16);
          const int colors_set = val & 0xFFF;
          const int colors_unset = (val & 0xFFF000) >> 12;
          const gboolean op = val & 0x80000000;
          if(op) // AND
          {
            if(colors_set)
              // clang-format off
              query = g_strdup_printf("(id IN (SELECT id FROM (SELECT imgid AS id, SUM(1 << color) AS mask"
                                      "  FROM main.color_labels GROUP BY imgid)"
                                      "  WHERE ((mask & %d) = %d) AND (mask & %d = 0)))",
                                      colors_set, colors_set, colors_unset);
              // clang-format on
            else if(colors_unset)
              // clang-format off
              query = g_strdup_printf("(NOT id IN (SELECT id FROM (SELECT imgid AS id, SUM(1 << color) AS mask"
                                      "  FROM main.color_labels GROUP BY imgid)"
                                      "  WHERE ((mask & %d) <> 0)))",
                                      colors_unset);
              // clang-format on
          }
          else // OR
          {
            if(!colors_unset)
              // clang-format off
              query = g_strdup_printf("(id IN (SELECT id FROM (SELECT imgid AS id, SUM(1 << color) AS mask"
                                      "  FROM main.color_labels GROUP BY imgid)"
                                      "  WHERE ((mask & %d) <> 0)))",
                                      colors_set);
              // clang-format on
            else
              // clang-format off
              query = g_strdup_printf("((id IN (SELECT id FROM (SELECT imgid AS id, SUM(1 << color) AS mask"
                                      "  FROM main.color_labels GROUP BY imgid)"
                                      "  WHERE ((mask & %d) <> 0))"
                                      " OR id NOT IN (SELECT id FROM (SELECT imgid AS id, SUM(1 << color) AS mask"
                                      "  FROM main.color_labels GROUP BY imgid)"
                                      "  WHERE ((mask & %d) = %d))))",
                                      colors_set, colors_unset, colors_unset);
              // clang-format on
          }
        }
        else // fallback to the text version
        {
          int color = 0;
          if(strcmp(escaped_text, _("red")) == 0)
            color = 0;
          else if(strcmp(escaped_text, _("yellow")) == 0)
            color = 1;
          else if(strcmp(escaped_text, _("green")) == 0)
            color = 2;
          else if(strcmp(escaped_text, _("blue")) == 0)
            color = 3;
          else if(strcmp(escaped_text, _("purple")) == 0)
            color = 4;
          // clang-format off
          query = g_strdup_printf("(id IN (SELECT imgid FROM main.color_labels WHERE color=%d))", color);
          // clang-format on
        }
      }
    }
    break;

    case DT_COLLECTION_PROP_HISTORY: // history
      if(!g_strcmp0(escaped_text, _("basic")) || !g_strcmp0(escaped_text, "$BASIC"))
      {
        // images without history and basic together
        query = g_strdup("(id not IN (SELECT imgid FROM main.history_hash WHERE (basic_hash IS NULL OR "
                         "current_hash != basic_hash)))");
      }
      else if(!g_strcmp0(escaped_text, _("auto applied")) || !g_strcmp0(escaped_text, "$AUTO_APPLIED"))
      {
        query = g_strdup("(id IN (SELECT imgid FROM main.history_hash WHERE current_hash == auto_hash))");
      }
      else if(!g_strcmp0(escaped_text, _("altered")) || !g_strcmp0(escaped_text, "$ALTERED"))
      {
        // clang-format off
        query = g_strdup("(id IN (SELECT imgid "
                                  "FROM main.history_hash "
                                  "WHERE (basic_hash IS NULL OR current_hash != basic_hash) "
                                  "AND (auto_hash IS NULL OR current_hash != auto_hash) ))");
        // clang-format on
      }
      else // by default, we select all the images
      {
        query = g_strdup("1 = 1");
      }
      break;

    case DT_COLLECTION_PROP_GEOTAGGING: // geotagging
      {
        const gboolean not_tagged = strcmp(escaped_text, _("not tagged")) == 0;
        const gboolean no_location = strcmp(escaped_text, _("tagged")) == 0;
        const gboolean all_tagged = strcmp(escaped_text, _("tagged*")) == 0;
        char *escaped_text2 = g_strstr_len(escaped_text, -1, "|");
        char *name_clause = g_strdup_printf("t.name LIKE \'%s\' || \'%s\'",
            dt_map_location_data_tag_root(), escaped_text2 ? escaped_text2 : "%");

        if(escaped_text2 && (escaped_text2[strlen(escaped_text2)-1] == '*'))
        {
          escaped_text2[strlen(escaped_text2)-1] = '\0';
          name_clause = g_strdup_printf("(t.name LIKE \'%s\' || \'%s\' OR t.name LIKE \'%s\' || \'%s|%%\')",
          dt_map_location_data_tag_root(), escaped_text2 , dt_map_location_data_tag_root(), escaped_text2);
        }

        if(not_tagged || all_tagged)
          // clang-format off
          query = g_strdup_printf("(id %s IN (SELECT id AS imgid FROM main.images "
                                  "WHERE (longitude IS NOT NULL AND latitude IS NOT NULL))) ",
                                  all_tagged ? "" : "not");
          // clang-format on
        else
          // clang-format off
          query = g_strdup_printf("(id IN (SELECT id AS imgid FROM main.images "
                                         "WHERE (longitude IS NOT NULL AND latitude IS NOT NULL))"
                                         "AND id %s IN (SELECT imgid FROM main.tagged_images AS ti"
                                         "  JOIN data.tags AS t"
                                         "  ON t.id = ti.tagid"
                                         "     AND %s)) ",
                                  no_location ? "not" : "",
                                  name_clause);
          // clang-format on
      }
      break;

    case DT_COLLECTION_PROP_LOCAL_COPY: // local copy
      if(!g_strcmp0(escaped_text, _("not copied locally")) || !g_strcmp0(escaped_text, "$NO_LOCAL_COPY"))
      {
        query = g_strdup_printf("(flags & %d = 0) ", DT_IMAGE_LOCAL_COPY);
      }
      else if(!g_strcmp0(escaped_text, _("copied locally")) || !g_strcmp0(escaped_text, "$LOCAL_COPY"))
      {
        query = g_strdup_printf("(flags & %d) ", DT_IMAGE_LOCAL_COPY);
      }
      else // by default, we select all the images
      {
        query = g_strdup("1 = 1");
      }
      break;

    case DT_COLLECTION_PROP_ASPECT_RATIO: // aspect ratio
    {
      gchar *operator, *number1, *number2;
      dt_collection_split_operator_number(escaped_text, &number1, &number2, &operator);

      if(operator && strcmp(operator, "[]") == 0)
      {
        if(number1 && number2)
          query = g_strdup_printf("((aspect_ratio >= %s) AND (aspect_ratio <= %s))", number1, number2);
      }
      else if(operator && number1)
        query = g_strdup_printf("(aspect_ratio %s %s)", operator, number1);
      else if(number1)
        query = g_strdup_printf("(aspect_ratio = %s)", number1);
      else
        query = g_strdup_printf("(aspect_ratio LIKE '%%%s%%')", escaped_text);

      g_free(operator);
      g_free(number1);
      g_free(number2);
    }
    break;

    case DT_COLLECTION_PROP_CAMERA: // camera
      // Start query with a false statement to avoid special casing the first condition
      query = g_strdup_printf("((1=0)");
      GList *lists = NULL;
      dt_collection_get_makermodels(text, NULL, &lists);
      for(GList *element = lists; element; element = g_list_next(element))
      {
        GList *tuple = element->data;
        char *clause = sqlite3_mprintf(" OR (maker = '%q' AND model = '%q')", tuple->data, tuple->next->data);
        query = dt_util_dstrcat(query, "%s", clause);
        sqlite3_free(clause);
        g_free(tuple->data);
        g_free(tuple->next->data);
        g_list_free(tuple);
      }
      g_list_free(lists);
      query = dt_util_dstrcat(query, ")");
      break;

    case DT_COLLECTION_PROP_TAG: // tag
    {
      const gboolean is_insensitive =
        dt_conf_is_equal("plugins/lighttable/tagging/case_sensitivity", "insensitive");

      if(!strcmp(escaped_text, _("not tagged")))
      {
        // clang-format off
        query = g_strdup_printf("(id NOT IN (SELECT DISTINCT imgid FROM main.tagged_images "
                                            "WHERE tagid NOT IN memory.darktable_tags))");
        // clang-format on
      }
      else if(is_insensitive)
      {
        if((escaped_length > 0) && (escaped_text[escaped_length-1] == '*'))
        {
          // shift-click adds an asterix * to include items in and under this hierarchy
          // without using a wildcard % which also would include similar named items
          escaped_text[escaped_length-1] = '\0';
          // clang-format off
          query = g_strdup_printf("(id IN (SELECT imgid FROM main.tagged_images WHERE tagid IN "
                                         "(SELECT id FROM data.tags WHERE name LIKE '%s' OR name LIKE '%s|%%')))",
                                  escaped_text, escaped_text);
          // clang-format on
        }
        else
        {
          // default
          // clang-format off
          query = g_strdup_printf("(id IN (SELECT imgid FROM main.tagged_images WHERE tagid IN "
                                       "(SELECT id FROM data.tags WHERE name LIKE '%s')))",
                                  escaped_text);
          // clang-format on
        }
      }
      else
      {
        if((escaped_length > 0) && (escaped_text[escaped_length-1] == '*'))
        {
          // shift-click adds an asterix * to include items in and under this hierarchy
          // without using a wildcard % which also would include similar named items
          escaped_text[escaped_length-1] = '\0';
          // clang-format off
          query = g_strdup_printf("(id IN (SELECT imgid FROM main.tagged_images WHERE tagid IN "
                                         "(SELECT id FROM data.tags "
                                         "WHERE name = '%s'"
                                         "  OR SUBSTR(name, 1, LENGTH('%s') + 1) = '%s|')))",
                                  escaped_text, escaped_text, escaped_text);
          // clang-format on
        }
        else if((escaped_length > 0) && (escaped_text[escaped_length-1] == '%'))
        {
          // ends with % or |%
          escaped_text[escaped_length-1] = '\0';
          // clang-format off
          query = g_strdup_printf("(id IN (SELECT imgid FROM main.tagged_images WHERE tagid IN "
                                         "(SELECT id FROM data.tags WHERE SUBSTR(name, 1, LENGTH('%s')) = '%s')))",
                                  escaped_text, escaped_text);
          // clang-format on
        }
        else
        {
          // default
          // clang-format off
          query = g_strdup_printf("(id IN (SELECT imgid FROM main.tagged_images WHERE tagid IN "
                                       "(SELECT id FROM data.tags WHERE name = '%s')))",
                                  escaped_text);
          // clang-format on
        }
      }
    }
    break;

    case DT_COLLECTION_PROP_LENS: // lens
      query = g_strdup_printf("(lens LIKE '%%%s%%')", escaped_text);
      break;

    case DT_COLLECTION_PROP_FOCAL_LENGTH: // focal length
    {
      gchar *operator, *number1, *number2;
      dt_collection_split_operator_number(escaped_text, &number1, &number2, &operator);

      if(operator && strcmp(operator, "[]") == 0)
      {
        if(number1 && number2)
          query = g_strdup_printf("((focal_length >= %s) AND (focal_length <= %s))", number1, number2);
      }
      else if(operator && number1)
        query = g_strdup_printf("(focal_length %s %s)", operator, number1);
      else if(number1)
        // clang-format off
        query = g_strdup_printf("(CAST(focal_length AS INTEGER) = CAST(%s AS INTEGER))", number1);
        // clang-format on
      else
        query = g_strdup_printf("(focal_length LIKE '%%%s%%')", escaped_text);

      g_free(operator);
      g_free(number1);
      g_free(number2);
    }
    break;

    case DT_COLLECTION_PROP_ISO: // iso
    {
      gchar *operator, *number1, *number2;
      dt_collection_split_operator_number(escaped_text, &number1, &number2, &operator);

      if(operator && strcmp(operator, "[]") == 0)
      {
        if(number1 && number2)
          query = g_strdup_printf("((iso >= %s) AND (iso <= %s))", number1, number2);
      }
      else if(operator && number1)
        query = g_strdup_printf("(iso %s %s)", operator, number1);
      else if(number1)
        query = g_strdup_printf("(iso = %s)", number1);
      else
        query = g_strdup_printf("(iso LIKE '%%%s%%')", escaped_text);

      g_free(operator);
      g_free(number1);
      g_free(number2);
    }
    break;

    case DT_COLLECTION_PROP_APERTURE: // aperture
    {
      gchar *operator, *number1, *number2;
      dt_collection_split_operator_number(escaped_text, &number1, &number2, &operator);

      if(operator && strcmp(operator, "[]") == 0)
      {
        if(number1 && number2)
          // clang-format off
          query = g_strdup_printf("((ROUND(aperture,1) >= %s) AND (ROUND(aperture,1) <= %s))", number1,
                                  number2);
          // clang-format on
      }
      else if(operator && number1)
        query = g_strdup_printf("(ROUND(aperture,1) %s %s)", operator, number1);
      else if(number1)
        query = g_strdup_printf("(ROUND(aperture,1) = %s)", number1);
      else
        query = g_strdup_printf("(ROUND(aperture,1) LIKE '%%%s%%')", escaped_text);

      g_free(operator);
      g_free(number1);
      g_free(number2);
    }
    break;

    case DT_COLLECTION_PROP_EXPOSURE: // exposure
    {
      gchar *operator, *number1, *number2;
      dt_collection_split_operator_exposure(escaped_text, &number1, &number2, &operator);

      if(operator && strcmp(operator, "[]") == 0)
      {
        if(number1 && number2)
          // clang-format off
          query = g_strdup_printf("((exposure >= %s  - 1.0/100000) AND (exposure <= %s  + 1.0/100000))", number1,
                                  number2);
          // clang-format on
      }
      else if(operator && number1)
        query = g_strdup_printf("(exposure %s %s)", operator, number1);
      else if(number1)
        // clang-format off
        query = g_strdup_printf("(CASE WHEN exposure < 0.4 THEN ((exposure >= %s - 1.0/100000) AND  (exposure <= %s + 1.0/100000)) "
                                "ELSE (ROUND(exposure,2) >= %s - 1.0/100000) AND (ROUND(exposure,2) <= %s + 1.0/100000) END)",
                                number1, number1, number1, number1);
        // clang-format on
      else
        query = g_strdup_printf("(exposure LIKE '%%%s%%')", escaped_text);

      g_free(operator);
      g_free(number1);
      g_free(number2);
    }
    break;

    case DT_COLLECTION_PROP_FILENAME: // filename
    {
      gchar *subquery = NULL;
      gchar **elems = g_strsplit(escaped_text, "/", -1);
      if(g_strv_length(elems) > 0)
      {
        // the main part
        GList *list = dt_util_str_to_glist(",", elems[0]);

        for(GList *l = list; l; l = g_list_next(l))
        {
          char *name = (char *)l->data; // remember the original content of this list node
          l->data = g_strdup_printf("(filename LIKE '%%%s%%')", name);
          g_free(name); // free the original filename
        }

        if(g_list_length(list) > 0) subquery = dt_util_glist_to_str(" OR ", list);
        g_list_free_full(list, g_free); // free the SQL clauses as well as the list
      }
      if(g_strv_length(elems) > 1)
      {
        GList *list = dt_util_str_to_glist(",", elems[1]);

        for(GList *l = list; l; l = g_list_next(l))
        {
          char *name = (char *)l->data; // remember the original content of this list node
          // special case for keywords
          if(!g_strcmp0("RAW", name))
          {
            l->data = g_strdup_printf("(flags & %d)", DT_IMAGE_RAW);
          }
          else if(!g_strcmp0("NOT RAW", name))
          {
            l->data = g_strdup_printf("(flags & %d == 0)", DT_IMAGE_RAW);
          }
          else if(!g_strcmp0("LDR", name))
          {
            l->data = g_strdup_printf("(flags & %d)", DT_IMAGE_LDR);
          }
          else if(!g_strcmp0("HDR", name))
          {
            l->data = g_strdup_printf("(flags & %d)", DT_IMAGE_HDR);
          }
          else
          {
            l->data = g_strdup_printf("(filename LIKE '%%%s%s%%')", g_str_has_prefix(name, ".") ? "" : ".", name);
          }
          g_free(name); // free the original filename
        }

        if(g_list_length(list) > 0)
        {
          if(subquery)
          {
            gchar *s = subquery;
            subquery = g_strdup_printf("(%s) AND (%s)", subquery, dt_util_glist_to_str(" OR ", list));
            g_free(s);
          }
          else
            subquery = dt_util_glist_to_str(" OR ", list);
        }
        g_list_free_full(list, g_free); // free the SQL clauses as well as the list
      }

      g_strfreev(elems);
      if(subquery)
        query = g_strdup_printf("(%s)", subquery);
      else
        query = g_strdup("1=1");

      g_free(subquery);

      break;
    }
    case DT_COLLECTION_PROP_DAY:
    case DT_COLLECTION_PROP_TIME:
    case DT_COLLECTION_PROP_IMPORT_TIMESTAMP:
    case DT_COLLECTION_PROP_CHANGE_TIMESTAMP:
    case DT_COLLECTION_PROP_EXPORT_TIMESTAMP:
    case DT_COLLECTION_PROP_PRINT_TIMESTAMP:
    {
      const int local_property = property;
      char *colname = NULL;

      switch(local_property)
      {
        case DT_COLLECTION_PROP_DAY: colname = "datetime_taken" ; break ;
        case DT_COLLECTION_PROP_TIME: colname = "datetime_taken" ; break ;
        case DT_COLLECTION_PROP_IMPORT_TIMESTAMP: colname = "import_timestamp" ; break ;
        case DT_COLLECTION_PROP_CHANGE_TIMESTAMP: colname = "change_timestamp" ; break ;
        case DT_COLLECTION_PROP_EXPORT_TIMESTAMP: colname = "export_timestamp" ; break ;
        case DT_COLLECTION_PROP_PRINT_TIMESTAMP: colname = "print_timestamp" ; break ;
      }
      gchar *operator, *number1, *number2;
      dt_collection_split_operator_datetime(escaped_text, &number1, &number2, &operator);
      if(number1 && number1[strlen(number1) - 1] == '%')
        number1[strlen(number1) - 1] = '\0';
      GTimeSpan nb1 = number1 ? dt_datetime_exif_to_gtimespan(number1) : 0;
      GTimeSpan nb2 = number2 ? dt_datetime_exif_to_gtimespan(number2) : 0;

      if(strcmp(operator, "[]") == 0)
      {
        if(number1 && number2)
          query = g_strdup_printf("((%s >= %" G_GINT64_FORMAT ") AND (%s <= %" G_GINT64_FORMAT "))", colname, nb1, colname, nb2);
      }
      else if((strcmp(operator, "=") == 0 || strcmp(operator, "") == 0) && number1 && number2)
        query = g_strdup_printf("((%s >= %" G_GINT64_FORMAT ") AND (%s <= %" G_GINT64_FORMAT "))", colname, nb1, colname, nb2);
      else if(strcmp(operator, "<>") == 0 && number1 && number2)
        query = g_strdup_printf("((%s < %" G_GINT64_FORMAT ") AND (%s > %" G_GINT64_FORMAT "))", colname, nb1, colname, nb2);
      else if(number1)
        query = g_strdup_printf("(%s %s %" G_GINT64_FORMAT ")", colname, operator, nb1);
      else
        query = g_strdup("1 = 1");

      g_free(operator);
      g_free(number1);
      g_free(number2);
      break;
    }

    case DT_COLLECTION_PROP_GROUPING: // grouping
      if(!g_strcmp0(escaped_text, "$NO_GROUP"))
      {
        query = g_strdup("(id = group_id AND "
                         "NOT EXISTS(SELECT 1 AS group_count FROM main.images AS gc WHERE gc.group_id = "
                         "mi.group_id AND gc.id != mi.id))");
      }
      else if(!g_strcmp0(escaped_text, "$GROUP"))
      {
        query = g_strdup(
            "(EXISTS(SELECT 1 FROM main.images AS gc WHERE gc.group_id = mi.group_id AND gc.id != mi.id))");
      }
      else if(!g_strcmp0(escaped_text, "$LEADER"))
      {
        query = g_strdup(
            "(mi.id = mi.group_id AND "
            "EXISTS(SELECT 1 FROM main.images AS gc WHERE gc.group_id = mi.group_id AND gc.id != mi.id))");
      }
      else if(!g_strcmp0(escaped_text, "$FOLLOWER"))
      {
        query = g_strdup("(id != group_id)");
      }
      else if(!g_strcmp0(escaped_text, _("group leaders"))) // used in collect.c
      {
        query = g_strdup("(id = group_id)");
      }
      else if(!g_strcmp0(escaped_text, _("group followers"))) // used in collect.c
      {
        query = g_strdup("(id != group_id)");
      }
      else // by default, we select all the images
      {
        query = g_strdup("1 = 1");
      }
      break;

    case DT_COLLECTION_PROP_MODULE: // dev module
      {
        // clang-format off
        query = g_strdup_printf("(id IN (SELECT imgid AS id FROM main.history AS h "
                                "JOIN memory.darktable_iop_names AS m ON m.operation = h.operation "
                                "WHERE h.enabled = 1 AND m.name LIKE '%s'))", escaped_text);
        // clang-format on
      }
      break;

    case DT_COLLECTION_PROP_ORDER: // module order
      {
        int i = 0;
        if(strlen(escaped_text) > 1 && g_str_has_prefix(escaped_text, "$"))
        {
          i = atoi(escaped_text + 1);
        }
        else
        {
          for(i = 0; i < DT_IOP_ORDER_LAST; i++)
          {
            if(strcmp(escaped_text, _(dt_iop_order_string(i))) == 0) break;
          }
        }
        if(i < DT_IOP_ORDER_LAST)
          // clang-format off
          query = g_strdup_printf("(id IN (SELECT imgid FROM main.module_order WHERE version = %d))", i);
          // clang-format on
        else
          // clang-format off
          query = g_strdup_printf("(id NOT IN (SELECT imgid FROM main.module_order))");
          // clang-format on
      }
      break;

      case DT_COLLECTION_PROP_TEXTSEARCH: // text search
      {
        // clang-format off
        if(g_strcmp0(escaped_text, "%%") != 0)
          query = g_strdup_printf
            ("(id IN (SELECT id FROM main.meta_data WHERE value LIKE '%s'"
             " UNION SELECT imgid AS id FROM main.tagged_images AS ti, data.tags AS t"
             "   WHERE t.id=ti.tagid AND (t.name LIKE '%s' OR t.synonyms LIKE '%s')"
             " UNION SELECT id FROM main.images"
             "   WHERE filename LIKE '%s'"
             " UNION SELECT i.id FROM main.images AS i, main.film_rolls AS fr"
             "   WHERE fr.id=i.film_id AND fr.folder LIKE '%s'))",
             escaped_text, escaped_text, escaped_text, escaped_text, escaped_text);
        // clang-format on
      }
      break;

      case DT_COLLECTION_PROP_RATING_RANGE: // image rating
      case DT_COLLECTION_PROP_RATING:
      {
        gchar *operator, *number1, *number2;
        dt_collection_split_operator_number(escaped_text, &number1, &number2, &operator);

        if(operator && strcmp(operator, "[]") == 0)
        {
          if(number1 && number2)
          {
            if(atoi(number1) == -1)
            { // rejected + star rating
              // clang-format off
              query = g_strdup_printf("(flags & 7 >= %s AND flags & 7 <= %s)", number1, number2);
              // clang-format on
            }
            else
            { // non-rejected + star rating
              // clang-format off
              query = g_strdup_printf("((flags & 8 == 0) AND (flags & 7 >= %s AND flags & 7 <= %s))", number1, number2);
              // clang-format on
            }
          }
        }
        else if(operator&& g_strcmp0(operator, "=") && number1)
        {
          if(g_strcmp0(operator, "<=") == 0 || g_strcmp0(operator, "<") == 0)
          { // all below rating + rejected
            // clang-format off
            query = g_strdup_printf("(flags & 8 == 8 OR flags & 7 %s %s)", operator, number1);
            // clang-format on
          }
          else if(g_strcmp0(operator, ">=") == 0 || g_strcmp0(operator, ">") == 0)
          {
            if(atoi(number1) >= 0)
            { // non rejected above rating
              // clang-format off
              query = g_strdup_printf("(flags & 8 == 0 AND flags & 7 %s %s)", operator, number1);
              // clang-format on
            }
            // otherwise no filter (rejected + all ratings)
          }
          else
          { // <> exclusion operator
            if(atoi(number1) == -1)
            { // all except rejected
              query = g_strdup_printf("(flags & 8 == 0)");
            }
            else
            { // all except star rating (including rejected)
              query = g_strdup_printf("(flags & 8 == 8 OR flags & 7 %s %s)", operator, number1);
            }
          }
        }
        else if(number1)
        {
          if(atoi(number1) == -1)
          { // rejected only
            query = g_strdup_printf("(flags & 8 == 8)");
          }
          else
          { // non-rejected + star rating
            query = g_strdup_printf("(flags & 8 == 0 AND flags & 7 == %s)", number1);
          }
        }

        g_free(operator);
        g_free(number1);
        g_free(number2);
      }
      break;

    default:
      {
        if(property >= DT_COLLECTION_PROP_METADATA
           && property < DT_COLLECTION_PROP_METADATA + DT_METADATA_NUMBER)
        {
          const int keyid = dt_metadata_get_keyid_by_display_order(property - DT_COLLECTION_PROP_METADATA);
          if(strcmp(escaped_text, _("not defined")) != 0)
            // clang-format off
            query = g_strdup_printf("(id IN (SELECT id FROM main.meta_data WHERE key = %d AND value "
                                           "LIKE '%%%s%%'))", keyid, escaped_text);
            // clang-format on
          else
            // clang-format off
            query = g_strdup_printf("(id NOT IN (SELECT id FROM main.meta_data WHERE key = %d))",
                                           keyid);
            // clang-format off
        }
      }
      break;
  }
  sqlite3_free(escaped_text);

  if(!query) // We've screwed up and not done a query string, send a placeholder
    query = g_strdup_printf("(1=1)");

  return query;
}

void dt_collection_sort_deserialize(const char *buf)
{
  int num_sort = 0;
  sscanf(buf, "%d", &num_sort);
  int sortid = 0, sortorder = 0;
  dt_conf_set_int("plugins/lighttable/filtering/num_sort", num_sort);
  while(buf[0] != '\0' && buf[0] != ':') buf++;
  if(buf[0] == ':') buf++;
  for(int k = 0; k < num_sort; k++)
  {
    const int n = sscanf(buf, "%d:%d", &sortid, &sortorder);
    if(n == 2)
    {
      char confname[200];
      snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sort%1d", k);
      dt_conf_set_int(confname, sortid);
      snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sortorder%1d", k);
      dt_conf_set_int(confname, sortorder);
    }
    else
    {
      dt_conf_set_int("plugins/lighttable/filtering/num_sort", k);
      break;
    }
    while(buf[0] != '$' && buf[0] != '\0') buf++;
    if(buf[0] == '$') buf++;
  }
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);
}

void dt_collection_sort_serialize(char *buf, int bufsize)
{
  int c;
  const int num_sort = dt_conf_get_int("plugins/lighttable/filtering/num_sort");
  c = snprintf(buf, bufsize, "%d:", num_sort);
  buf += c;
  bufsize -= c;
  for(int k = 0; k < num_sort; k++)
  {
    char confname[200];
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sort%1d", k);
    const int sortid = dt_conf_get_int(confname);
    c = snprintf(buf, bufsize, "%d:", sortid);
    buf += c;
    bufsize -= c;
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/sortorder%1d", k);
    const int sortorder = dt_conf_get_int(confname);
    c = snprintf(buf, bufsize, "%d$", sortorder);
    buf += c;
    bufsize -= c;
  }
}

int dt_collection_serialize(char *buf, int bufsize, gboolean filtering)
{
  const char *plugin_name = filtering ? "plugins/lighttable/filtering" : "plugins/lighttable/collect";
  char confname[200];
  int c;
  snprintf(confname, sizeof(confname), "%s/num_rules", plugin_name);
  const int num_rules = dt_conf_get_int(confname);
  c = snprintf(buf, bufsize, "%d:", num_rules);
  buf += c;
  bufsize -= c;
  for(int k = 0; k < num_rules; k++)
  {
    snprintf(confname, sizeof(confname), "%s/mode%1d", plugin_name, k);
    const int mode = dt_conf_get_int(confname);
    c = snprintf(buf, bufsize, "%d:", mode);
    buf += c;
    bufsize -= c;
    snprintf(confname, sizeof(confname), "%s/item%1d", plugin_name, k);
    const int item = dt_conf_get_int(confname);
    c = snprintf(buf, bufsize, "%d:", item);
    buf += c;
    bufsize -= c;
    if(filtering)
    {
      snprintf(confname, sizeof(confname), "%s/off%1d", plugin_name, k);
      const int off = dt_conf_get_int(confname);
      c = snprintf(buf, bufsize, "%d:", off);
      buf += c;
      bufsize -= c;
      snprintf(confname, sizeof(confname), "%s/top%1d", plugin_name, k);
      const int top = dt_conf_get_int(confname);
      c = snprintf(buf, bufsize, "%d:", top);
      buf += c;
      bufsize -= c;
    }
    snprintf(confname, sizeof(confname), "%s/string%1d", plugin_name, k);
    const char *str = dt_conf_get_string_const(confname);
    if(str && (str[0] != '\0'))
      c = snprintf(buf, bufsize, "%s$", str);
    else
      c = snprintf(buf, bufsize, "%%$");
    buf += c;
    bufsize -= c;
  }
  return 0;
}

void dt_collection_deserialize(const char *buf, gboolean filtering)
{
  const char *plugin_name = filtering ? "plugins/lighttable/filtering" : "plugins/lighttable/collect";
  char confname[200];
  int num_rules = 0;
  sscanf(buf, "%d", &num_rules);
  if(num_rules == 0 && !filtering)
  {
    // we always want at least 1 rule
    snprintf(confname, sizeof(confname), "%s/num_rules", plugin_name);
    dt_conf_set_int(confname, 1);
    snprintf(confname, sizeof(confname), "%s/mode0", plugin_name);
    dt_conf_set_int(confname, 0);
    snprintf(confname, sizeof(confname), "%s/item0", plugin_name);
    dt_conf_set_int(confname, 0);
    snprintf(confname, sizeof(confname), "%s/string0", plugin_name);
    dt_conf_set_string(confname, "%");
  }
  else
  {
    int mode = 0, item = 0, off = 0, top = 0;
    snprintf(confname, sizeof(confname), "%s/num_rules", plugin_name);
    dt_conf_set_int(confname, num_rules);
    while(buf[0] != '\0' && buf[0] != ':') buf++;
    if(buf[0] == ':') buf++;
    char str[400];
    for(int k = 0; k < num_rules; k++)
    {
      const int n = (filtering) ? sscanf(buf, "%d:%d:%d:%d:%399[^$]", &mode, &item, &off, &top, str)
                                : sscanf(buf, "%d:%d:%399[^$]", &mode, &item, str);
      if((!filtering && n == 3) || (filtering && n == 5))
      {
        snprintf(confname, sizeof(confname), "%s/mode%1d", plugin_name, k);
        dt_conf_set_int(confname, mode);
        snprintf(confname, sizeof(confname), "%s/item%1d", plugin_name, k);
        dt_conf_set_int(confname, item);
        if(filtering)
        {
          snprintf(confname, sizeof(confname), "%s/off%1d", plugin_name, k);
          dt_conf_set_int(confname, off);
          snprintf(confname, sizeof(confname), "%s/top%1d", plugin_name, k);
          dt_conf_set_int(confname, top);
        }
        snprintf(confname, sizeof(confname), "%s/string%1d", plugin_name, k);
        dt_conf_set_string(confname, str);
      }
      else if(!filtering && num_rules == 1)
      {
        snprintf(confname, sizeof(confname), "%s/mode%1d", plugin_name, k);
        dt_conf_set_int(confname, 0);
        snprintf(confname, sizeof(confname), "%s/item%1d", plugin_name, k);
        dt_conf_set_int(confname, 0);
        snprintf(confname, sizeof(confname), "%s/string%1d", plugin_name, k);
        dt_conf_set_string(confname, "%");
        break;
      }
      else
      {
        snprintf(confname, sizeof(confname), "%s/num_rules", plugin_name);
        dt_conf_set_int(confname, k);
        break;
      }
      while(buf[0] != '$' && buf[0] != '\0') buf++;
      if(buf[0] == '$') buf++;
    }
  }
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);
}

void dt_collection_update_query(const dt_collection_t *collection, dt_collection_change_t query_change,
                                dt_collection_properties_t changed_property, GList *list)
{
  int next = -1;
  if(!collection->clone && query_change == DT_COLLECTION_CHANGE_NEW_QUERY && darktable.gui)
  {
    // if the query has changed, we reset the expanded group
    darktable.gui->expanded_group_id = -1;
  }

  if(!collection->clone)
  {
    if(list)
    {
      // for changing offsets, thumbtable needs to know the first untouched imageid after the list
      // we do this here

      // 1. create a string with all the imgids of the list to be used inside IN sql query
      gchar *txt = NULL;
      int i = 0;
      for(GList *l = list; l; l = g_list_next(l))
      {
        const int id = GPOINTER_TO_INT(l->data);
        if(i == 0)
          txt = dt_util_dstrcat(txt, "%d", id);
        else
          txt = dt_util_dstrcat(txt, ",%d", id);
        i++;
      }
      // 2. search the first imgid not in the list but AFTER the list (or in a gap inside the list)
      // we need to be carefull that some images in the list may not be present on screen (collapsed groups)
      // clang-format off
      gchar *query = g_strdup_printf("SELECT imgid"
                                     " FROM memory.collected_images"
                                     " WHERE imgid NOT IN (%s)"
                                     "  AND rowid > (SELECT rowid"
                                     "              FROM memory.collected_images"
                                     "              WHERE imgid IN (%s)"
                                     "              ORDER BY rowid LIMIT 1)"
                                     " ORDER BY rowid LIMIT 1",
                                     txt, txt);
      // clang-format on
      sqlite3_stmt *stmt2;
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt2, NULL);
      if(sqlite3_step(stmt2) == SQLITE_ROW)
      {
        next = sqlite3_column_int(stmt2, 0);
      }
      sqlite3_finalize(stmt2);
      g_free(query);
      // 3. if next is still unvalid, let's try to find the first untouched image BEFORE the list
      if(next < 0)
      {
        // clang-format off
        query = g_strdup_printf("SELECT imgid"
                                " FROM memory.collected_images"
                                " WHERE imgid NOT IN (%s)"
                                "   AND rowid < (SELECT rowid"
                                "                FROM memory.collected_images"
                                "                WHERE imgid IN (%s)"
                                "                ORDER BY rowid LIMIT 1)"
                                " ORDER BY rowid DESC LIMIT 1",
                                txt, txt);
        // clang-format on
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt2, NULL);
        if(sqlite3_step(stmt2) == SQLITE_ROW)
        {
          next = sqlite3_column_int(stmt2, 0);
        }
        sqlite3_finalize(stmt2);
        g_free(query);
      }
      g_free(txt);
    }
  }

  char confname[200];

  const int _n_r = dt_conf_get_int("plugins/lighttable/collect/num_rules");
  const int _n_f = dt_conf_get_int("plugins/lighttable/filtering/num_rules");
  const int num_rules = CLAMP(_n_r, 1, 10);
  const int num_filters = MIN(_n_f, 10);
  char *conj[] = { "AND", "OR", "AND NOT" };

  gchar **query_parts = g_new(gchar *, num_rules + num_filters + 1);
  query_parts[num_rules + num_filters] = NULL;

  // the main rules part
  for(int i = 0; i < num_rules; i++)
  {
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/item%1d", i);
    const int property = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", i);
    gchar *text = dt_conf_get_string(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", i);
    const int mode = dt_conf_get_int(confname);

    if(!text || text[0] == '\0')
    {
      if(mode == 1) // for OR show all
        query_parts[i] = g_strdup(" OR 1=1");
      else
        query_parts[i] = g_strdup("");
    }
    else
    {
      gchar *query = get_query_string(property, text);

      query_parts[i] =  g_strdup_printf(" %s %s", conj[mode], query);

      g_free(query);
    }
    g_free(text);
  }

  // the filtering part (same syntax as for collect rules)
  for(int i = 0; i < num_filters; i++)
  {
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/item%1d", i);
    const int property = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/string%1d", i);
    gchar *text = dt_conf_get_string(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/mode%1d", i);
    const int mode = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/filtering/off%1d", i);
    const int off = dt_conf_get_int(confname);

    if(off || !text || text[0] == '\0')
    {
      if(!off && mode == 1) // for OR show all
        query_parts[i + num_rules] = g_strdup(" OR 1=1");
      else
        query_parts[i + num_rules] = g_strdup("");
    }
    else
    {
      gchar *query = get_query_string(property, text);

      query_parts[i + num_rules] = g_strdup_printf(" %s %s", conj[mode], query);

      g_free(query);
    }
    g_free(text);
  }


  /* set the extended where and the use of it in the query */
  dt_collection_set_extended_where(collection, query_parts);
  g_strfreev(query_parts);
  dt_collection_set_query_flags(collection,
                                (dt_collection_get_query_flags(collection) | COLLECTION_QUERY_USE_WHERE_EXT));

  /* remove film id from default filter */
  dt_collection_set_filter_flags(collection,
                                 (dt_collection_get_filter_flags(collection) & ~COLLECTION_FILTER_FILM_ID));

  /* update query and at last the visual */
  dt_collection_update(collection);

  // remove from selected images where not in this query.
  sqlite3_stmt *stmt = NULL;
  const gchar *cquery = dt_collection_get_query_no_group(collection);
  if(cquery && cquery[0] != '\0')
  {
    gchar *complete_query = g_strdup_printf("DELETE FROM main.selected_images WHERE imgid NOT IN (%s)", cquery);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), complete_query, -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, 0);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, -1);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    // if we have remove something from selection, we need to raise a signal
    if(sqlite3_changes(dt_database_get(darktable.db)) > 0)
    {
      DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_SELECTION_CHANGED);
    }

    /* free allocated strings */
    g_free(complete_query);
  }

  /* raise signal of collection change, only if this is an original */
  if(!collection->clone)
  {
    dt_collection_memory_update();
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED, query_change, changed_property,
                                  list, next);
  }
}

gboolean dt_collection_hint_message_internal(void *message)
{
  GtkWidget *count = dt_view_filter_get_count(darktable.view_manager);
  if(count)
  {
    gtk_label_set_markup(GTK_LABEL(count), message);
    gtk_widget_set_tooltip_markup(count, message);
  }

  message = dt_util_dstrcat(message, " %s", _("in current collection"));
  dt_control_hinter_message(darktable.control,
    dt_ui_panel_visible(darktable.gui->ui, DT_UI_PANEL_CENTER_TOP) ? "" : message);

  g_free(message);
  return FALSE;
}

void dt_collection_hint_message(const dt_collection_t *collection)
{
  /* collection hinting */
  gchar *message;

  const int c = dt_collection_get_count_no_group(collection);
  const int cs = dt_collection_get_selected_count(collection);

  if(cs == 1)
  {
    /* determine offset of the single selected image */
    GList *selected_imgids = dt_collection_get_selected(collection, 1);
    int selected = -1;

    if(selected_imgids)
    {
      selected = GPOINTER_TO_INT(selected_imgids->data);
      selected = dt_collection_image_offset_with_collection(collection, selected);
      selected++;
    }
    g_list_free(selected_imgids);
    message = g_strdup_printf(_("<b>%d</b> image (#<b>%d</b>) selected of <b>%d</b>"), cs, selected, c);
  }
  else
  {
    message = g_strdup_printf(
      ngettext(
        "<b>%d</b> image selected of <b>%d</b>",
        "<b>%d</b> images selected of <b>%d</b>",
        cs),
      cs, c);
  }

  g_idle_add(dt_collection_hint_message_internal, message);
}

static int dt_collection_image_offset_with_collection(const dt_collection_t *collection, int imgid)
{
  if(imgid == -1) return 0;
  int offset = 0;
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT imgid FROM memory.collected_images",
                              -1, &stmt, NULL);

  gboolean found = FALSE;

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int id = sqlite3_column_int(stmt, 0);
    if(imgid == id)
    {
      found = TRUE;
      break;
    }
    offset++;
  }

  if(!found) offset = 0;

  sqlite3_finalize(stmt);

  return offset;
}

int dt_collection_image_offset(int imgid)
{
  return dt_collection_image_offset_with_collection(darktable.collection, imgid);
}

static void _dt_collection_recount_callback_1(gpointer instance, gpointer user_data)
{
  dt_collection_t *collection = (dt_collection_t *)user_data;
  const int old_count = collection->count;
  collection->count = _dt_collection_compute_count(collection, FALSE);
  collection->count_no_group = _dt_collection_compute_count(collection, TRUE);
  if(!collection->clone)
  {
    if(old_count != collection->count) dt_collection_hint_message(collection);
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED, DT_COLLECTION_CHANGE_RELOAD,
                                  DT_COLLECTION_PROP_UNDEF, (GList *)NULL, -1);
  }
}

static void _dt_collection_recount_callback_2(gpointer instance, uint8_t id, gpointer user_data)
{
  _dt_collection_recount_callback_1(instance, user_data);
}
static void _dt_collection_filmroll_imported_callback(gpointer instance, uint8_t id, gpointer user_data)
{
  dt_collection_t *collection = (dt_collection_t *)user_data;
  const int old_count = collection->count;
  collection->count = _dt_collection_compute_count(collection, FALSE);
  collection->count_no_group = _dt_collection_compute_count(collection, TRUE);
  if(!collection->clone)
  {
    if(old_count != collection->count) dt_collection_hint_message(collection);
    dt_collection_update_query(collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);
  }
}

int64_t dt_collection_get_image_position(const int32_t image_id, const int32_t tagid)
{
  int64_t image_position = -1;

  if(image_id >= 0)
  {
    sqlite3_stmt *stmt = NULL;
    // clang-format off
    gchar *image_pos_query = g_strdup(
          tagid ? "SELECT position FROM main.tagged_images WHERE imgid = ?1 AND tagid = ?2"
                : "SELECT position FROM main.images WHERE id = ?1");
    // clang-format on

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), image_pos_query, -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, image_id);
    if(tagid) DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, tagid);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      image_position = sqlite3_column_int64(stmt, 0);
    }

    sqlite3_finalize(stmt);
    g_free(image_pos_query);
  }

  return image_position;
}

void dt_collection_shift_image_positions(const unsigned int length,
                                         const int64_t image_position,
                                         const int32_t tagid)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              tagid
                              ?
                              "UPDATE main.tagged_images"
                              " SET position = position + ?1"
                              " WHERE position >= ?2 AND position < ?3"
                              "       AND tagid = ?4"
                              :
                              "UPDATE main.images"
                              " SET position = position + ?1"
                              " WHERE position >= ?2 AND position < ?3",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, length);
  DT_DEBUG_SQLITE3_BIND_INT64(stmt, 2, image_position);
  DT_DEBUG_SQLITE3_BIND_INT64(stmt, 3, (image_position & 0xFFFFFFFF00000000) + (1ll << 32));
  if(tagid) DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, tagid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

/* move images with drag and drop
 *
 * An int64 is used for the position index. The upper 31 bits define the initial order.
 * The lower 32bit provide space to reorder images. That way only a small amount of images must be
 * updated while reordering images.
 *
 * Example: (position values hex)
 * Initial order:
 * Img 1: 0000 0001 0000 0000
 * Img 2: 0000 0002 0000 0000
 * Img 3: 0000 0003 0000 0000
 * Img 3: 0000 0004 0000 0000
 *
 * Putting Img 2 in front of Img 1. Would give
 * Img 2: 0000 0001 0000 0000
 * Img 1: 0000 0001 0000 0001
 * Img 3: 0000 0003 0000 0000
 * Img 4: 0000 0004 0000 0000
 *
 * Img 3 and Img 4 is not updated.
*/
void dt_collection_move_before(const int32_t image_id, GList * selected_images)
{
  if(!selected_images)
  {
    return;
  }

  const uint32_t tagid = darktable.collection->tagid;
  // getting the position of the target image
  const int64_t target_image_pos = dt_collection_get_image_position(image_id, tagid);
  if(target_image_pos >= 0)
  {
    const guint selected_images_length = g_list_length(selected_images);

    dt_collection_shift_image_positions(selected_images_length, target_image_pos, tagid);

    sqlite3_stmt *stmt = NULL;
    dt_database_start_transaction(darktable.db);

    // move images to their intended positions
    int64_t new_image_pos = target_image_pos;
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                tagid
                                ?
                                "UPDATE main.tagged_images"
                                " SET position = ?1"
                                " WHERE imgid = ?2 AND tagid = ?3"
                                :
                                "UPDATE main.images"
                                " SET position = ?1"
                                " WHERE id = ?2",
                                -1, &stmt, NULL);
    // clang-format on

    for(const GList * selected_images_iter = selected_images;
         selected_images_iter != NULL;
         selected_images_iter = g_list_next(selected_images_iter))
    {
      const int moved_image_id = GPOINTER_TO_INT(selected_images_iter->data);

      DT_DEBUG_SQLITE3_BIND_INT64(stmt, 1, new_image_pos);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, moved_image_id);
      if(tagid) DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, tagid);
      sqlite3_step(stmt);
      sqlite3_reset(stmt);
      new_image_pos++;
    }
    sqlite3_finalize(stmt);
    dt_database_release_transaction(darktable.db);
  }
  else
  {
    // move images to the end of the list
    sqlite3_stmt *stmt = NULL;

    // get last position
    int64_t max_position = -1;
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                tagid
                                ?
                                "SELECT MAX(position) FROM main.tagged_images"
                                :
                                "SELECT MAX(position) FROM main.images",
                                -1, &stmt, NULL);
    // clang-format on

    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      max_position = sqlite3_column_int64(stmt, 0);
      max_position = (max_position & 0xFFFFFFFF00000000) >> 32;
    }

    sqlite3_finalize(stmt);
    sqlite3_stmt *update_stmt = NULL;

    dt_database_start_transaction(darktable.db);

    // move images to last position in custom image order table
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                tagid
                                ?
                                "UPDATE main.tagged_images"
                                " SET position = ?1"
                                " WHERE imgid = ?2 AND tagid = ?3"
                                :
                                "UPDATE main.images"
                                " SET position = ?1"
                                " WHERE id = ?2",
                                -1, &update_stmt, NULL);
    // clang-format on

    for(const GList * selected_images_iter = selected_images;
         selected_images_iter != NULL;
         selected_images_iter = g_list_next(selected_images_iter))
    {
      max_position++;
      const int moved_image_id = GPOINTER_TO_INT(selected_images_iter->data);
      DT_DEBUG_SQLITE3_BIND_INT64(update_stmt, 1, max_position << 32);
      DT_DEBUG_SQLITE3_BIND_INT(update_stmt, 2, moved_image_id);
      if(tagid) DT_DEBUG_SQLITE3_BIND_INT(update_stmt, 3, tagid);
      sqlite3_step(update_stmt);
      sqlite3_reset(update_stmt);
    }

    sqlite3_finalize(update_stmt);
    dt_database_release_transaction(darktable.db);
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
