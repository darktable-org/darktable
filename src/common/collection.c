/*
    This file is part of darktable,
    copyright (c) 2010-2012 Henrik Andersson.

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
#include "control/conf.h"
#include "control/control.h"

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
#define ORDER_BY_QUERY "ORDER BY %s"
#define LIMIT_QUERY "LIMIT ?1, ?2"

static const char *comparators[] = {
  "<",  // DT_COLLECTION_RATING_COMP_LT = 0,
  "<=", // DT_COLLECTION_RATING_COMP_LEQ,
  "=",  // DT_COLLECTION_RATING_COMP_EQ,
  ">=", // DT_COLLECTION_RATING_COMP_GEQ,
  ">",  // DT_COLLECTION_RATING_COMP_GT,
  "!=", // DT_COLLECTION_RATING_COMP_NE,
};

/* Stores the collection query, returns 1 if changed.. */
static int _dt_collection_store(const dt_collection_t *collection, gchar *query, gchar *query_no_group);
/* Counts the number of images in the current collection */
static uint32_t _dt_collection_compute_count(const dt_collection_t *collection, gboolean no_group);
/* signal handlers to update the cached count when something interesting might have happened.
 * we need 2 different since there are different kinds of signals we need to listen to. */
static void _dt_collection_recount_callback_1(gpointer instace, gpointer user_data);
static void _dt_collection_recount_callback_2(gpointer instance, uint8_t id, gpointer user_data);

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
  }
  else /* else we just initialize using the reset */
    dt_collection_reset(collection);

  /* connect to all the signals that might indicate that the count of images matching the collection changed
   */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_TAG_CHANGED,
                            G_CALLBACK(_dt_collection_recount_callback_1), collection);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_FILMROLLS_CHANGED,
                            G_CALLBACK(_dt_collection_recount_callback_1), collection);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_FILMROLLS_REMOVED,
                            G_CALLBACK(_dt_collection_recount_callback_1), collection);

  dt_control_signal_connect(darktable.signals, DT_SIGNAL_IMAGE_IMPORT,
                            G_CALLBACK(_dt_collection_recount_callback_2), collection);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_FILMROLLS_IMPORTED,
                            G_CALLBACK(_dt_collection_recount_callback_2), collection);

  return collection;
}

void dt_collection_free(const dt_collection_t *collection)
{
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_dt_collection_recount_callback_1),
                               (gpointer)collection);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_dt_collection_recount_callback_2),
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

int dt_collection_update(const dt_collection_t *collection)
{
  uint32_t result;
  gchar *wq, *wq_no_group, *sq, *selq_pre, *selq_post, *query, *query_no_group;
  wq = wq_no_group = sq = selq_pre = selq_post = query = query_no_group = NULL;

  /* build where part */
  gchar *where_ext = dt_collection_get_extended_where(collection, -1);
  if(!(collection->params.query_flags & COLLECTION_QUERY_USE_ONLY_WHERE_EXT))
  {
    int need_operator = 0;
    dt_collection_filter_t rating = collection->params.rating;
    if(rating == DT_COLLECTION_FILTER_NOT_REJECT) rating = DT_COLLECTION_FILTER_STAR_NO;

    /* add default filters */
    if(collection->params.filter_flags & COLLECTION_FILTER_FILM_ID)
    {
      wq = dt_util_dstrcat(wq, "(film_id = %d)", collection->params.film_id);
      need_operator = 1;
    }
    // DON'T SELECT IMAGES MARKED TO BE DELETED.
    wq = dt_util_dstrcat(wq, " %s (flags & %d) != %d",
                         (need_operator) ? "AND" : ((need_operator = 1) ? "" : ""), DT_IMAGE_REMOVE,
                         DT_IMAGE_REMOVE);

    if(collection->params.filter_flags & COLLECTION_FILTER_CUSTOM_COMPARE)
      wq = dt_util_dstrcat(wq, " %s (flags & 7) %s %d AND (flags & 7) != 6",
                           (need_operator) ? "and" : ((need_operator = 1) ? "" : ""),
                           comparators[collection->params.comparator], rating - 1);
    else if(collection->params.filter_flags & COLLECTION_FILTER_ATLEAST_RATING)
      wq = dt_util_dstrcat(wq, " %s (flags & 7) >= %d AND (flags & 7) != 6",
                           (need_operator) ? "and" : ((need_operator = 1) ? "" : ""), rating - 1);
    else if(collection->params.filter_flags & COLLECTION_FILTER_EQUAL_RATING)
      wq = dt_util_dstrcat(wq, " %s (flags & 7) == %d",
                           (need_operator) ? "AND" : ((need_operator = 1) ? "" : ""), rating - 1);

    if(collection->params.filter_flags & COLLECTION_FILTER_ALTERED)
      wq = dt_util_dstrcat(wq, " %s id IN (SELECT imgid FROM main.history WHERE imgid=id)",
                           (need_operator) ? "AND" : ((need_operator = 1) ? "" : ""));
    else if(collection->params.filter_flags & COLLECTION_FILTER_UNALTERED)
      wq = dt_util_dstrcat(wq, " %s id NOT IN (SELECT imgid FROM main.history WHERE imgid=id)",
                           (need_operator) ? "AND" : ((need_operator = 1) ? "" : ""));

    /* add where ext if wanted */
    if((collection->params.query_flags & COLLECTION_QUERY_USE_WHERE_EXT))
      wq = dt_util_dstrcat(wq, " %s %s", (need_operator) ? "AND" : "", where_ext);
  }
  else
    wq = dt_util_dstrcat(wq, "%s", where_ext);

  g_free(where_ext);

  wq_no_group = g_strdup(wq);

  /* grouping */
  if(darktable.gui && darktable.gui->grouping)
  {
    /* Show the expanded group... */
    wq = dt_util_dstrcat(wq, " AND (group_id = %d OR "
                             /* ...and, in unexpanded groups, show the representative image.
                              * It's possible that the above WHERE clauses will filter out the representative
                              * image, so we have some logic here to pick the image id closest to the
                              * representative image.
                              * The *2+CASE statement are to break ties, so that when id < group_id, it's
                              * weighted a little higher than when id > group_id. */
                             "(ABS(id-group_id)*2 + CASE WHEN (id-group_id) < 0 THEN 1 ELSE 0 END) IN "
                             "(SELECT MIN(ABS(id-group_id)*2 + CASE WHEN (id-group_id) < 0 THEN 1 ELSE 0 END) "
                             "FROM main.images WHERE %s GROUP BY group_id))",
                         darktable.gui->expanded_group_id, wq_no_group);

    /* Additionally, when a group is expanded, make sure the representative image wasn't filtered out.
     * This is important, because otherwise it may be impossible to collapse the group again. */
    wq = dt_util_dstrcat(wq, " OR (id = %d)", darktable.gui->expanded_group_id);
  }

  /* build select part includes where */
  if(collection->params.sort == DT_COLLECTION_SORT_COLOR
     && (collection->params.query_flags & COLLECTION_QUERY_USE_SORT))
  {
    selq_pre = dt_util_dstrcat(selq_pre, "SELECT DISTINCT id FROM (SELECT * FROM main.images WHERE ");
    selq_post = dt_util_dstrcat(selq_post, ") AS a LEFT OUTER JOIN main.color_labels AS b ON a.id = b.imgid");
  }
  else if(collection->params.sort == DT_COLLECTION_SORT_PATH
          && (collection->params.query_flags & COLLECTION_QUERY_USE_SORT))
  {
    selq_pre = dt_util_dstrcat(selq_pre, "SELECT DISTINCT id FROM (SELECT * FROM main.images WHERE ");
    selq_post = dt_util_dstrcat(selq_post, ") JOIN (SELECT id AS film_rolls_id, folder FROM main.film_rolls) ON film_id = film_rolls_id");
  }
  else if(collection->params.query_flags & COLLECTION_QUERY_USE_ONLY_WHERE_EXT)
    selq_pre = dt_util_dstrcat(selq_pre, "SELECT DISTINCT images.id FROM main.images ");
  else
    selq_pre = dt_util_dstrcat(selq_pre, "SELECT DISTINCT id FROM main.images WHERE ");

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
  params->rating = DT_COLLECTION_FILTER_STAR_NO;

  /* apply stored query parameters from previous darktable session */
  params->film_id = dt_conf_get_int("plugins/collection/film_id");
  params->rating = dt_conf_get_int("plugins/collection/rating");
  params->comparator = dt_conf_get_int("plugins/collection/rating_comparator");
  params->filter_flags = dt_conf_get_int("plugins/collection/filter_flags");
  params->sort = dt_conf_get_int("plugins/collection/sort");
  params->descending = dt_conf_get_bool("plugins/collection/descending");
  dt_collection_update_query(collection);
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

  if (exclude >= 0)
  {
    complete_string = g_strdup("");
    char confname[200];
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", exclude);
    const int mode = dt_conf_get_int(confname);
    if (mode != 1) // don't limit the collection for OR
    {
      for(int i = 0; collection->where_ext[i] != NULL; i++)
      {
        // exclude the one rule from extended where
        if (i != exclude)
          complete_string = dt_util_dstrcat(complete_string, "%s", collection->where_ext[i]);
      }
    }
  }
  else
    complete_string = g_strjoinv(complete_string, ((dt_collection_t *)collection)->where_ext);

  gchar *where_ext = dt_util_dstrcat(NULL, "(1=1%s)", complete_string);
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

void dt_collection_set_film_id(const dt_collection_t *collection, uint32_t film_id)
{
  dt_collection_params_t *params = (dt_collection_params_t *)&collection->params;
  params->film_id = film_id;
}

void dt_collection_set_rating(const dt_collection_t *collection, uint32_t rating)
{
  dt_collection_params_t *params = (dt_collection_params_t *)&collection->params;
  params->rating = rating;
}

uint32_t dt_collection_get_rating(const dt_collection_t *collection)
{
  uint32_t i;
  dt_collection_params_t *params = (dt_collection_params_t *)&collection->params;
  i = params->rating;
  return i;
}

void dt_collection_set_rating_comparator(const dt_collection_t *collection,
                                         const dt_collection_rating_comperator_t comparator)
{
  ((dt_collection_t *)collection)->params.comparator = comparator;
}

dt_collection_rating_comperator_t dt_collection_get_rating_comparator(const dt_collection_t *collection)
{
  return collection->params.comparator;
}

static void _collection_update_aspect_ratio(const dt_collection_t *collection)
{
  dt_collection_params_t *params = (dt_collection_params_t *)&collection->params;

  //  Update the aspect ratio for selected images in the collection if needed, we do not do this for all
  //  images as it could take a long time. The aspect ratio is then updated when needed, and at some
  //  point all aspect ratio for all images will be set and this could won't be really needed.

  if (params->sort == DT_COLLECTION_SORT_ASPECT_RATIO)
  {
    const float MAX_TIME = 5.0;
    const gchar *where_ext = dt_collection_get_extended_where(collection, -1);
    gchar *query = NULL;
    sqlite3_stmt *stmt = NULL;

    query = dt_util_dstrcat
      (query, "SELECT id FROM main.images WHERE %s AND (aspect_ratio=0.0 OR aspect_ratio IS NULL)", where_ext);

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);

    double start = dt_get_wtime();
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      const int imgid = sqlite3_column_int(stmt, 0);
      dt_image_set_aspect_ratio(imgid);

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

void dt_collection_set_sort(const dt_collection_t *collection, dt_collection_sort_t sort, gboolean reverse)
{
  dt_collection_params_t *params = (dt_collection_params_t *)&collection->params;

  if(sort != DT_COLLECTION_SORT_NONE) params->sort = sort;
  if(reverse != -1) params->descending = reverse;

  _collection_update_aspect_ratio(collection);
}

dt_collection_sort_t dt_collection_get_sort_field(const dt_collection_t *collection)
{
  return collection->params.sort;
}

gboolean dt_collection_get_sort_descending(const dt_collection_t *collection)
{
  return collection->params.descending;
}

gchar *dt_collection_get_sort_query(const dt_collection_t *collection)
{
  gchar *sq = NULL;

  if(collection->params.descending)
  {
    switch(collection->params.sort)
    {
      case DT_COLLECTION_SORT_DATETIME:
        sq = dt_util_dstrcat(sq, ORDER_BY_QUERY, "datetime_taken DESC, filename DESC, version DESC");
        break;

      case DT_COLLECTION_SORT_RATING:
        sq = dt_util_dstrcat(sq, ORDER_BY_QUERY, "flags & 7, filename DESC, version DESC");
        break;

      case DT_COLLECTION_SORT_FILENAME:
        sq = dt_util_dstrcat(sq, ORDER_BY_QUERY, "filename DESC, version DESC");
        break;

      case DT_COLLECTION_SORT_ID:
        sq = dt_util_dstrcat(sq, ORDER_BY_QUERY, "id DESC");
        break;

      case DT_COLLECTION_SORT_COLOR:
        sq = dt_util_dstrcat(sq, ORDER_BY_QUERY, "color, filename DESC, version DESC");
        break;

      case DT_COLLECTION_SORT_GROUP:
        sq = dt_util_dstrcat(sq, ORDER_BY_QUERY, "group_id DESC, id-group_id != 0, id DESC");
        break;

      case DT_COLLECTION_SORT_PATH:
        sq = dt_util_dstrcat(sq, ORDER_BY_QUERY, "folder DESC, filename DESC, version DESC");
        break;

      case DT_COLLECTION_SORT_CUSTOM_ORDER:
        sq = dt_util_dstrcat(sq, ORDER_BY_QUERY, "position DESC, filename DESC, version DESC");
        break;

      case DT_COLLECTION_SORT_TITLE:
        sq = dt_util_dstrcat(sq, ORDER_BY_QUERY, "caption DESC, filename DESC, version DESC");
        break;

      case DT_COLLECTION_SORT_DESCRIPTION:
        sq = dt_util_dstrcat(sq, ORDER_BY_QUERY, "description DESC, filename DESC, version DESC");
        break;

      case DT_COLLECTION_SORT_ASPECT_RATIO:
        sq = dt_util_dstrcat(sq, ORDER_BY_QUERY, "aspect_ratio DESC, filename DESC, version DESC");
        break;

      case DT_COLLECTION_SORT_NONE:
        // shouldn't happen
        break;
    }
  }
  else
  {
    switch(collection->params.sort)
    {
      case DT_COLLECTION_SORT_DATETIME:
        sq = dt_util_dstrcat(sq, ORDER_BY_QUERY, "datetime_taken, filename, version");
        break;

      case DT_COLLECTION_SORT_RATING:
        sq = dt_util_dstrcat(sq, ORDER_BY_QUERY, "flags & 7 DESC, filename, version");
        break;

      case DT_COLLECTION_SORT_FILENAME:
        sq = dt_util_dstrcat(sq, ORDER_BY_QUERY, "filename, version");
        break;

      case DT_COLLECTION_SORT_ID:
        sq = dt_util_dstrcat(sq, ORDER_BY_QUERY, "id");
        break;

      case DT_COLLECTION_SORT_COLOR:
        sq = dt_util_dstrcat(sq, ORDER_BY_QUERY, "color DESC, filename, version");
        break;

      case DT_COLLECTION_SORT_GROUP:
        sq = dt_util_dstrcat(sq, ORDER_BY_QUERY, "group_id, id-group_id != 0, id");
        break;

      case DT_COLLECTION_SORT_PATH:
        sq = dt_util_dstrcat(sq, ORDER_BY_QUERY, "folder, filename, version");
        break;

      case DT_COLLECTION_SORT_CUSTOM_ORDER:
        sq = dt_util_dstrcat(sq, ORDER_BY_QUERY, "position, filename, version");
        break;

      case DT_COLLECTION_SORT_TITLE:
        sq = dt_util_dstrcat(sq, ORDER_BY_QUERY, "caption, filename, version");
        break;

      case DT_COLLECTION_SORT_DESCRIPTION:
        sq = dt_util_dstrcat(sq, ORDER_BY_QUERY, "description, filename, version");
        break;

      case DT_COLLECTION_SORT_ASPECT_RATIO:
        sq = dt_util_dstrcat(sq, ORDER_BY_QUERY, "aspect_ratio, filename, version");
        break;

      case DT_COLLECTION_SORT_NONE:
        // shouldn't happen
        break;
    }
  }

  return sq;
}


static int _dt_collection_store(const dt_collection_t *collection, gchar *query, gchar *query_no_group)
{
  /* store flags to conf */
  if(collection == darktable.collection)
  {
    dt_conf_set_int("plugins/collection/query_flags", collection->params.query_flags);
    dt_conf_set_int("plugins/collection/filter_flags", collection->params.filter_flags);
    dt_conf_set_int("plugins/collection/film_id", collection->params.film_id);
    dt_conf_set_int("plugins/collection/rating", collection->params.rating);
    dt_conf_set_int("plugins/collection/rating_comparator", collection->params.comparator);
    dt_conf_set_int("plugins/collection/sort", collection->params.sort);
    dt_conf_set_bool("plugins/collection/descending", collection->params.descending);
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
    count_query = dt_util_dstrcat(NULL, "SELECT COUNT(DISTINCT main.images.id) FROM main.images %s", where_ext);
    g_free(where_ext);
  }
  else
    count_query = dt_util_dstrcat(count_query, "SELECT COUNT(DISTINCT id) %s", fq);

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
  gchar *query = NULL;
  gchar *sq = NULL;

  /* get collection order */
  if((collection->params.query_flags & COLLECTION_QUERY_USE_SORT))
    sq = dt_collection_get_sort_query(collection);

  sqlite3_stmt *stmt = NULL;

  /* build the query string */
  query = dt_util_dstrcat(query, "SELECT DISTINCT id FROM main.images ");

  if(collection->params.sort == DT_COLLECTION_SORT_COLOR
     && (collection->params.query_flags & COLLECTION_QUERY_USE_SORT))
    query = dt_util_dstrcat(query, "AS a LEFT OUTER JOIN main.color_labels AS b ON a.id = b.imgid ");
  else if(collection->params.sort == DT_COLLECTION_SORT_PATH
          && (collection->params.query_flags & COLLECTION_QUERY_USE_SORT))
    query = dt_util_dstrcat(
        query, "JOIN (SELECT id AS film_rolls_id, folder FROM main.film_rolls) ON film_id = film_rolls_id ");

  if (selected)
    query = dt_util_dstrcat(query, "WHERE id IN (SELECT imgid FROM main.selected_images) %s LIMIT ?1", sq);
  else
    query = dt_util_dstrcat(query, "%s LIMIT ?1", sq);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, limit);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int imgid = sqlite3_column_int(stmt, 0);
    list = g_list_append(list, GINT_TO_POINTER(imgid));
  }

  sqlite3_finalize(stmt);

  /* free allocated strings */
  g_free(sq);

  g_free(query);

  return list;
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
  int match_count;

  *number1 = *number2 = *operator= NULL;

  // we test the range expression first
  regex = g_regex_new("^\\s*\\[\\s*([0-9]+\\.?[0-9]*)\\s*;\\s*([0-9]+\\.?[0-9]*)\\s*\\]\\s*$", 0, 0, NULL);
  g_regex_match_full(regex, input, -1, 0, 0, &match_info, NULL);
  match_count = g_match_info_get_match_count(match_info);

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
  regex = g_regex_new("^\\s*(=|<|>|<=|>=|<>)?\\s*([0-9]+\\.?[0-9]*)\\s*$", 0, 0, NULL);
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
  int len = strlen(input);
  if(len < 4) return NULL;

  struct tm tm1 = { 0 };

  // we initialise all the values of tm, depending of the operator
  // we allow unreal values like "2014:02:31" as it's just text comparison at the end
  if(strcmp(operator, ">") == 0 || strcmp(operator, "<=") == 0)
  {
    // we set all values to their maximum
    tm1.tm_mon = 11;
    tm1.tm_mday = 31;
    tm1.tm_hour = 23;
    tm1.tm_min = 59;
    tm1.tm_sec = 59;
  }

  // we read the input date, depending of his length
  if(len < 7)
  {
    if(!strptime(input, "%Y", &tm1)) return NULL;
  }
  else if(len < 10)
  {
    if(!strptime(input, "%Y:%m", &tm1)) return NULL;
  }
  else if(len < 13)
  {
    if(!strptime(input, "%Y:%m:%d", &tm1)) return NULL;
  }
  else if(len < 16)
  {
    if(!strptime(input, "%Y:%m:%d %H", &tm1)) return NULL;
  }
  else if(len < 19)
  {
    if(!strptime(input, "%Y:%m:%d %H:%M", &tm1)) return NULL;
  }
  else
  {
    if(!strptime(input, "%Y:%m:%d %H:%M:%S", &tm1)) return NULL;
  }

  // we return the created date
  char *ret = (char *)g_malloc0_n(20, sizeof(char));
  strftime(ret, 20, "%Y:%m:%d %H:%M:%S", &tm1);
  return ret;
}
/* splits an input string into a date-time part and an optional operator part.
   operator can be any of "=", "<", ">", "<=", ">=" and "<>".
   range notation [x;y] can also be used
   datetime values should follow the pattern YYYY:mm:dd HH:MM:SS
   but only year part is mandatory

   datetime and operator are returned as pointers to null terminated strings in g_mallocated
   memory (to be g_free'd after use) - or NULL if no match is found.
*/
void dt_collection_split_operator_datetime(const gchar *input, char **number1, char **number2, char **operator)
{
  GRegex *regex;
  GMatchInfo *match_info;
  int match_count;

  *number1 = *number2 = *operator= NULL;

  // we test the range expression first
  // 2 elements : date-time1 and  date-time2
  regex = g_regex_new("^\\s*\\[\\s*(\\d{4}[:\\d\\s]*)\\s*;\\s*(\\d{4}[:\\d\\s]*)\\s*\\]\\s*$", 0, 0, NULL);
  g_regex_match_full(regex, input, -1, 0, 0, &match_info, NULL);
  match_count = g_match_info_get_match_count(match_info);

  if(match_count == 3)
  {
    gchar *txt = g_match_info_fetch(match_info, 1);
    gchar *txt2 = g_match_info_fetch(match_info, 2);

    *number1 = _dt_collection_compute_datetime(">=", txt);
    *number2 = _dt_collection_compute_datetime("<=", txt2);
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
  regex = g_regex_new("^\\s*(=|<|>|<=|>=|<>)?\\s*(\\d{4}[:\\d\\s]*)?\\s*%?\\s*$", 0, 0, NULL);
  g_regex_match_full(regex, input, -1, 0, 0, &match_info, NULL);
  match_count = g_match_info_get_match_count(match_info);

  if(match_count == 3)
  {
    *operator= g_match_info_fetch(match_info, 1);
    gchar *txt = g_match_info_fetch(match_info, 2);

    if(strcmp(*operator, "") == 0 || strcmp(*operator, "=") == 0 || strcmp(*operator, "<>") == 0)
      *number1 = dt_util_dstrcat(*number1, "%s%%", txt);
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
  int match_count;

  *number1 = *number2 = *operator= NULL;

  // we test the range expression first
  regex = g_regex_new("^\\s*\\[\\s*(1/)?([0-9]+\\.?[0-9]*)(\")?\\s*;\\s*(1/)?([0-9]+\\.?[0-9]*)(\")?\\s*\\]\\s*$", 0, 0, NULL);
  g_regex_match_full(regex, input, -1, 0, 0, &match_info, NULL);
  match_count = g_match_info_get_match_count(match_info);

  if(match_count == 6 || match_count == 7)
  {
    gchar *n1 = g_match_info_fetch(match_info, 2);

    if(strstr(g_match_info_fetch(match_info, 1), "1/") != NULL)
      *number1 = dt_util_dstrcat(NULL, "1.0/%s", n1);
    else
      *number1 = n1;

    gchar *n2 = g_match_info_fetch(match_info, 5);

    if(strstr(g_match_info_fetch(match_info, 4), "1/") != NULL)
      *number2 = dt_util_dstrcat(NULL, "1.0/%s", n2);
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
      *number1 = dt_util_dstrcat(NULL, "1.0/%s", n1);
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

  GHashTable *names = NULL;
  if (sanitized)
    names = g_hash_table_new(g_str_hash, g_str_equal);

  if (filter && filter[0] != '\0')
    needle = g_utf8_strdown(filter, -1);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT maker, model FROM main.images GROUP BY maker, model",
                              -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    char *exif_maker = (char *)sqlite3_column_text(stmt, 0);
    char *exif_model = (char *)sqlite3_column_text(stmt, 1);

    gchar *makermodel =  dt_collection_get_makermodel(exif_maker, exif_model);

    gchar *haystack = g_utf8_strdown(makermodel, -1);
    if (!needle || g_strrstr(haystack, needle) != NULL)
    {
      if (exif)
      {
        // Append a two element list with maker and model
        GList *inner_list = NULL;
        inner_list = g_list_append(inner_list, g_strdup(exif_maker));
        inner_list = g_list_append(inner_list, g_strdup(exif_model));
        *exif = g_list_append(*exif, inner_list);
      }

      if (sanitized)
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
  gchar *makermodel = NULL;

  char maker[64];
  char model[64];
  char alias[64];
  maker[0] = model[0] = alias[0] = '\0';
  dt_rawspeed_lookup_makermodel(exif_maker, exif_model,
                                maker, sizeof(maker),
                                model, sizeof(model),
                                alias, sizeof(alias));

  // Create the makermodel by concatenation

  makermodel = dt_util_dstrcat(makermodel, "%s %s", maker, model);

  return makermodel;
}

static gchar *get_query_string(const dt_collection_properties_t property, const gchar *text)
{
  char *escaped_text = sqlite3_mprintf("%q", text);
  gchar *query = NULL;

  switch(property)
  {
    case DT_COLLECTION_PROP_FILMROLL: // film roll
      if(!(escaped_text && *escaped_text))
        query = dt_util_dstrcat(query, "(film_id IN (SELECT id FROM main.film_rolls WHERE folder LIKE '%s%%'))",
                                escaped_text);
      else
        query = dt_util_dstrcat(query, "(film_id IN (SELECT id FROM main.film_rolls WHERE folder LIKE '%s'))",
                                escaped_text);
      break;

    case DT_COLLECTION_PROP_FOLDERS: // folders
      query = dt_util_dstrcat(
          query, "(film_id IN (SELECT id FROM main.film_rolls WHERE folder LIKE '%1$s' OR folder LIKE '%1$s"
                 G_DIR_SEPARATOR_S "%%'))",
          escaped_text);
      break;

    case DT_COLLECTION_PROP_COLORLABEL: // colorlabel
    {
      int color = 0;
      if(!(escaped_text && *escaped_text) || strcmp(escaped_text, "%") == 0)
        query = dt_util_dstrcat(query, "(id IN (SELECT imgid FROM main.color_labels WHERE color IS NOT NULL))");
      else
      {
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
        query = dt_util_dstrcat(query, "(id IN (SELECT imgid FROM main.color_labels WHERE color=%d))", color);
      }
    }
    break;

    case DT_COLLECTION_PROP_HISTORY: // history
      query = dt_util_dstrcat(query, "(id %s IN (SELECT imgid FROM main.history WHERE imgid=images.id)) ",
                              (strcmp(escaped_text, _("altered")) == 0) ? "" : "not");
      break;

    case DT_COLLECTION_PROP_GEOTAGGING: // geotagging
      query = dt_util_dstrcat(query, "(id %s IN (SELECT id AS imgid FROM main.images WHERE "
                                     "(longitude IS NOT NULL AND latitude IS NOT NULL))) ",
                              (strcmp(escaped_text, _("tagged")) == 0) ? "" : "not");
      break;

    case DT_COLLECTION_PROP_LOCAL_COPY: // local copy
      query = dt_util_dstrcat(query, "(id %s IN (SELECT id AS imgid FROM main.images WHERE "
                                     "(flags & %d))) ",
                              (strcmp(escaped_text, _("not copied locally")) == 0) ? "not" : "",
                              DT_IMAGE_LOCAL_COPY);
      break;

    case DT_COLLECTION_PROP_ASPECT_RATIO: // aspect ratio
    {
      gchar *operator, *number1, *number2;
      dt_collection_split_operator_number(escaped_text, &number1, &number2, &operator);

      if(operator && strcmp(operator, "[]") == 0)
      {
        if(number1 && number2)
          query = dt_util_dstrcat(query, "((aspect_ratio >= %s) AND (aspect_ratio <= %s))", number1, number2);
      }
      else if(operator && number1)
        query = dt_util_dstrcat(query, "(aspect_ratio %s %s)", operator, number1);
      else if(number1)
        query = dt_util_dstrcat(query, "(aspect_ratio = %s)", number1);
      else
        query = dt_util_dstrcat(query, "(aspect_ratio LIKE '%%%s%%')", escaped_text);

      g_free(operator);
      g_free(number1);
      g_free(number2);
    }
    break;

    case DT_COLLECTION_PROP_CAMERA: // camera
      // Start query with a false statement to avoid special casing the first condition
      query = dt_util_dstrcat(query, "((1=0)");
      GList *lists = NULL;
      dt_collection_get_makermodels(text, NULL, &lists);
      GList *element = lists;
      while (element)
      {
        GList *tuple = element->data;
        char *mk = sqlite3_mprintf("%q", tuple->data);
        char *md = sqlite3_mprintf("%q", tuple->next->data);
        query = dt_util_dstrcat(query, " OR (maker = '%s' AND model = '%s')", mk, md);
        sqlite3_free(mk);
        sqlite3_free(md);
        g_free(tuple->data);
        g_free(tuple->next->data);
        g_list_free(tuple);
        element = element->next;
      }
      g_list_free(lists);
      query = dt_util_dstrcat(query, ")");
      break;
    case DT_COLLECTION_PROP_TAG: // tag
      query = dt_util_dstrcat(query, "(id IN (SELECT imgid FROM main.tagged_images AS a JOIN "
                                     "data.tags AS b ON a.tagid = b.id WHERE name LIKE '%s'))",
                              escaped_text);
      break;

    // TODO: How to handle images without metadata? In the moment they are not shown.
    // TODO: Autogenerate this code?
    case DT_COLLECTION_PROP_TITLE: // title
      query = dt_util_dstrcat(query, "(id IN (SELECT id FROM main.meta_data WHERE key = %d AND value "
                                     "LIKE '%%%s%%'))", DT_METADATA_XMP_DC_TITLE, escaped_text);
      break;
    case DT_COLLECTION_PROP_DESCRIPTION: // description
      query = dt_util_dstrcat(query, "(id IN (SELECT id FROM main.meta_data WHERE key = %d AND value "
                                     "LIKE '%%%s%%'))", DT_METADATA_XMP_DC_DESCRIPTION, escaped_text);
      break;
    case DT_COLLECTION_PROP_CREATOR: // creator
      query = dt_util_dstrcat(query, "(id IN (SELECT id FROM main.meta_data WHERE key = %d AND value "
                                     "LIKE '%%%s%%'))", DT_METADATA_XMP_DC_CREATOR, escaped_text);
      break;
    case DT_COLLECTION_PROP_PUBLISHER: // publisher
      query = dt_util_dstrcat(query, "(id IN (SELECT id FROM main.meta_data WHERE key = %d AND value "
                                     "LIKE '%%%s%%'))", DT_METADATA_XMP_DC_PUBLISHER, escaped_text);
      break;
    case DT_COLLECTION_PROP_RIGHTS: // rights
      query = dt_util_dstrcat(query, "(id IN (SELECT id FROM main.meta_data WHERE key = %d AND value "
                                     "LIKE '%%%s%%'))", DT_METADATA_XMP_DC_RIGHTS, escaped_text);
      break;
    case DT_COLLECTION_PROP_LENS: // lens
      query = dt_util_dstrcat(query, "(lens LIKE '%%%s%%')", escaped_text);
      break;

    case DT_COLLECTION_PROP_FOCAL_LENGTH: // focal length
    {
      gchar *operator, *number1, *number2;
      dt_collection_split_operator_number(escaped_text, &number1, &number2, &operator);

      if(operator && strcmp(operator, "[]") == 0)
      {
        if(number1 && number2)
          query = dt_util_dstrcat(query, "((focal_length >= %s) AND (focal_length <= %s))", number1, number2);
      }
      else if(operator && number1)
        query = dt_util_dstrcat(query, "(focal_length %s %s)", operator, number1);
      else if(number1)
        query = dt_util_dstrcat(query, "(focal_length = %s)", number1);
      else
        query = dt_util_dstrcat(query, "(focal_length LIKE '%%%s%%')", escaped_text);

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
          query = dt_util_dstrcat(query, "((iso >= %s) AND (iso <= %s))", number1, number2);
      }
      else if(operator && number1)
        query = dt_util_dstrcat(query, "(iso %s %s)", operator, number1);
      else if(number1)
        query = dt_util_dstrcat(query, "(iso = %s)", number1);
      else
        query = dt_util_dstrcat(query, "(iso LIKE '%%%s%%')", escaped_text);

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
          query = dt_util_dstrcat(query, "((ROUND(aperture,1) >= %s) AND (ROUND(aperture,1) <= %s))", number1,
                                  number2);
      }
      else if(operator && number1)
        query = dt_util_dstrcat(query, "(ROUND(aperture,1) %s %s)", operator, number1);
      else if(number1)
        query = dt_util_dstrcat(query, "(ROUND(aperture,1) = %s)", number1);
      else
        query = dt_util_dstrcat(query, "(ROUND(aperture,1) LIKE '%%%s%%')", escaped_text);

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
          query = dt_util_dstrcat(query, "((exposure >= %s  - 1.0/100000) AND (exposure <= %s  + 1.0/100000))", number1,
                                  number2);
      }
      else if(operator && number1)
        query = dt_util_dstrcat(query, "(exposure %s %s)", operator, number1);
      else if(number1)
        query = dt_util_dstrcat(query,
                                "(CASE WHEN exposure < 0.4 THEN ((exposure >= %s - 1.0/100000) AND  (exposure <= %s + 1.0/100000)) "
                                "ELSE (ROUND(exposure,2) >= %s - 1.0/100000) AND (ROUND(exposure,2) <= %s + 1.0/100000) END)",
                                number1, number1, number1, number1);
      else
        query = dt_util_dstrcat(query, "(exposure LIKE '%%%s%%')", escaped_text);

      g_free(operator);
      g_free(number1);
      g_free(number2);
    }
    break;

    case DT_COLLECTION_PROP_FILENAME: // filename
      query = dt_util_dstrcat(query, "(filename LIKE '%%%s%%')", escaped_text);
      break;

    case DT_COLLECTION_PROP_DAY:
    // query = dt_util_dstrcat(query, "(datetime_taken like '%%%s%%')", escaped_text);
    // break;

    case DT_COLLECTION_PROP_TIME:
    {
      gchar *operator, *number1, *number2;
      dt_collection_split_operator_datetime(escaped_text, &number1, &number2, &operator);

      if(strcmp(operator, "[]") == 0)
      {
        if(number1 && number2)
          query = dt_util_dstrcat(query, "((datetime_taken >= '%s') AND (datetime_taken <= '%s'))", number1,
                                  number2);
      }
      else if((strcmp(operator, "=") == 0 || strcmp(operator, "") == 0) && number1)
        query = dt_util_dstrcat(query, "(datetime_taken LIKE '%s')", number1);
      else if(strcmp(operator, "<>") == 0 && number1)
        query = dt_util_dstrcat(query, "(datetime_taken NOT LIKE '%s')", number1);
      else if(number1)
        query = dt_util_dstrcat(query, "(datetime_taken %s '%s')", operator, number1);
      else
        query = dt_util_dstrcat(query, "(datetime_taken LIKE '%%%s%%')", escaped_text);

      g_free(operator);
      g_free(number1);
      g_free(number2);
    }
    break;

    case DT_COLLECTION_PROP_GROUPING: // grouping
      query = dt_util_dstrcat(query, "(id %s group_id)", (strcmp(escaped_text, _("group leaders")) == 0) ? "=" : "!=");
      break;

    default:
      // we shouldn't be here
      break;
  }
  sqlite3_free(escaped_text);

  if(!query) // We've screwed up and not done a query string, send a placeholder
    query = dt_util_dstrcat(query, "(1=1)");

  return query;
}

int dt_collection_serialize(char *buf, int bufsize)
{
  char confname[200];
  int c;
  const int num_rules = dt_conf_get_int("plugins/lighttable/collect/num_rules");
  c = snprintf(buf, bufsize, "%d:", num_rules);
  buf += c;
  bufsize -= c;
  for(int k = 0; k < num_rules; k++)
  {
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", k);
    const int mode = dt_conf_get_int(confname);
    c = snprintf(buf, bufsize, "%d:", mode);
    buf += c;
    bufsize -= c;
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/item%1d", k);
    const int item = dt_conf_get_int(confname);
    c = snprintf(buf, bufsize, "%d:", item);
    buf += c;
    bufsize -= c;
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", k);
    gchar *str = dt_conf_get_string(confname);
    if(str && (str[0] != '\0'))
      c = snprintf(buf, bufsize, "%s$", str);
    else
      c = snprintf(buf, bufsize, "%%$");
    buf += c;
    bufsize -= c;
    g_free(str);
  }
  return 0;
}

void dt_collection_deserialize(char *buf)
{
  int num_rules = 0;
  char str[400], confname[200];
  int mode = 0, item = 0;
  sscanf(buf, "%d", &num_rules);
  if(num_rules == 0)
  {
    dt_conf_set_int("plugins/lighttable/collect/num_rules", 1);
    dt_conf_set_int("plugins/lighttable/collect/mode0", 0);
    dt_conf_set_int("plugins/lighttable/collect/item0", 0);
    dt_conf_set_string("plugins/lighttable/collect/string0", "%");
  }
  else
  {
    dt_conf_set_int("plugins/lighttable/collect/num_rules", num_rules);
    while(buf[0] != '\0' && buf[0] != ':') buf++;
    if(buf[0] == ':') buf++;
    for(int k = 0; k < num_rules; k++)
    {
      int n = sscanf(buf, "%d:%d:%399[^$]", &mode, &item, str);
      if(n == 3)
      {
        snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", k);
        dt_conf_set_int(confname, mode);
        snprintf(confname, sizeof(confname), "plugins/lighttable/collect/item%1d", k);
        dt_conf_set_int(confname, item);
        snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", k);
        dt_conf_set_string(confname, str);
      }
      else if(num_rules == 1)
      {
        snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", k);
        dt_conf_set_int(confname, 0);
        snprintf(confname, sizeof(confname), "plugins/lighttable/collect/item%1d", k);
        dt_conf_set_int(confname, 0);
        snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", k);
        dt_conf_set_string(confname, "%");
        break;
      }
      else
      {
        dt_conf_set_int("plugins/lighttable/collect/num_rules", k);
        break;
      }
      while(buf[0] != '$' && buf[0] != '\0') buf++;
      if(buf[0] == '$') buf++;
    }
  }
  dt_collection_update_query(darktable.collection);
}

void dt_collection_update_query(const dt_collection_t *collection)
{
  char confname[200];

  const int _n_r = dt_conf_get_int("plugins/lighttable/collect/num_rules");
  const int num_rules = CLAMP(_n_r, 1, 10);
  char *conj[] = { "AND", "OR", "AND NOT" };

  gchar **query_parts = g_new (gchar*, num_rules + 1);
  query_parts[num_rules] =  NULL;

  for(int i = 0; i < num_rules; i++)
  {
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/item%1d", i);
    const int property = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", i);
    gchar *text = dt_conf_get_string(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", i);
    const int mode = dt_conf_get_int(confname);

    if(!text || text[0] == '\0') {
      if (mode == 1) // for OR show all
        query_parts[i] = g_strdup(" OR 1=1");
      else
        query_parts[i] = g_strdup("");
    } else {
      gchar *query = get_query_string(property, text);

      query_parts[i] =  g_strdup_printf(" %s %s", conj[mode], query);

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
  gchar *complete_query = NULL;
  if(cquery && cquery[0] != '\0')
  {
    complete_query
        = dt_util_dstrcat(complete_query, "DELETE FROM main.selected_images WHERE imgid NOT IN (%s)", cquery);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), complete_query, -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, 0);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, -1);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /* free allocated strings */
    g_free(complete_query);
  }

  /* raise signal of collection change, only if this is an original */
  if(!collection->clone) dt_control_signal_raise(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED);
}

gboolean dt_collection_hint_message_internal(void *message)
{
  dt_control_hinter_message(darktable.control, message);
  g_free(message);
  return FALSE;
}

void dt_collection_hint_message(const dt_collection_t *collection)
{
  /* if relevant, determine offset of selection */
  GList *selected_imgids = dt_collection_get_selected(collection, 1);
  int selected = -1;

  if(selected_imgids)
  {
    selected = GPOINTER_TO_INT(selected_imgids->data);
    selected = dt_collection_image_offset_with_collection(collection, selected);
    selected++;
  }
  /* collection hinting */
  gchar *message;

  int c = dt_collection_get_count_no_group(collection);
  int cs = dt_collection_get_selected_count(collection);

  if(cs == 1)
  {
    message = g_strdup_printf(_("%d image of %d (#%d) in current collection is selected"), cs, c, selected);
  }
  else
  {
    message = g_strdup_printf(
      ngettext(
        "%d image of %d in current collection is selected",
        "%d images of %d in current collection are selected",
        cs),
      cs, c);
  }

  g_idle_add(dt_collection_hint_message_internal, message);
}

static int dt_collection_image_offset_with_collection(const dt_collection_t *collection, int imgid)
{
  if(imgid == -1) return 0;
  const gchar *qin = dt_collection_get_query(collection);
  int offset = 0;
  sqlite3_stmt *stmt;

  if(qin)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), qin, -1, &stmt, NULL);

    // was the limit portion of the query tacked on?
    if(collection->params.query_flags & COLLECTION_QUERY_USE_LIMIT)
    {
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, 0);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, -1);
    }

    gboolean found = FALSE;

    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      int id = sqlite3_column_int(stmt, 0);
      if(imgid == id)
      {
        found = TRUE;
        break;
      }
      offset++;
    }

    if(!found) offset = 0;

    sqlite3_finalize(stmt);
  }
  return offset;
}

int dt_collection_image_offset(int imgid)
{
  return dt_collection_image_offset_with_collection(darktable.collection, imgid);
}

static void _dt_collection_recount_callback_1(gpointer instace, gpointer user_data)
{
  dt_collection_t *collection = (dt_collection_t *)user_data;
  int old_count = collection->count;
  collection->count = _dt_collection_compute_count(collection, FALSE);
  collection->count_no_group = _dt_collection_compute_count(collection, TRUE);
  if(!collection->clone)
  {
    if(old_count != collection->count) dt_collection_hint_message(collection);
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED);
  }
}

static void _dt_collection_recount_callback_2(gpointer instance, uint8_t id, gpointer user_data)
{
  dt_collection_t *collection = (dt_collection_t *)user_data;
  int old_count = collection->count;
  collection->count = _dt_collection_compute_count(collection, FALSE);
  collection->count_no_group = _dt_collection_compute_count(collection, TRUE);
  if(!collection->clone)
  {
    if(old_count != collection->count) dt_collection_hint_message(collection);
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED);
  }
}

int64_t dt_collection_get_image_position(const int32_t image_id)
{
  int64_t image_position = -1;

  if (image_id >= 0)
  {
    sqlite3_stmt *stmt = NULL;
    gchar *image_pos_query = NULL;
    image_pos_query = dt_util_dstrcat(
          image_pos_query,
          "SELECT position FROM main.images WHERE id = ?1");

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), image_pos_query, -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, image_id);

    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      image_position = sqlite3_column_int64(stmt, 0);
    }

    sqlite3_finalize(stmt);
    g_free(image_pos_query);
  }

  return image_position;
}

void dt_collection_shift_image_positions(const unsigned int length, const int64_t image_position)
{
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "BEGIN", NULL, NULL, NULL);
  sqlite3_stmt *stmt = NULL;

  // shift image positions to make some space
  gchar * update_image_pos_query = "UPDATE main.images SET position = position + ?1 WHERE position >= ?2 AND position < ?3";

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), update_image_pos_query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, length);
  DT_DEBUG_SQLITE3_BIND_INT64(stmt, 2, image_position);
  DT_DEBUG_SQLITE3_BIND_INT64(stmt, 3, (image_position & 0xFFFFFFFF00000000) + (1ll << 32));

  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "COMMIT", NULL, NULL, NULL);
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
  if (!selected_images)
  {
    return;
  }

  const guint selected_images_length = g_list_length(selected_images);

  if (selected_images_length == 0)
  {
    return;
  }

  // getting the position of the target image
  const int64_t target_image_pos = dt_collection_get_image_position(image_id);

  if (target_image_pos >= 0)
  {
    dt_collection_shift_image_positions(selected_images_length, target_image_pos);

    sqlite3_stmt *stmt = NULL;
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "BEGIN", NULL, NULL, NULL);

    // move images to their intended positons
    int64_t new_image_pos = target_image_pos;

    gchar *insert_image_pos_query = "UPDATE main.images SET position = ?1 WHERE id = ?2";

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), insert_image_pos_query, -1, &stmt, NULL);

    for (const GList * selected_images_iter = selected_images;
        selected_images_iter != NULL;
        selected_images_iter = selected_images_iter->next)
    {
      const int moved_image_id = GPOINTER_TO_INT(selected_images_iter->data);

      DT_DEBUG_SQLITE3_BIND_INT64(stmt, 1, new_image_pos);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, moved_image_id);
      sqlite3_step(stmt);
      sqlite3_reset(stmt);
      new_image_pos++;
    }
    sqlite3_finalize(stmt);
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "COMMIT", NULL, NULL, NULL);
  }
  else
  {
    // move images to the end of the list
    sqlite3_stmt *stmt = NULL;

    // get last position
    int64_t max_position = -1;

    gchar *max_position_query = "SELECT MAX(position) FROM main.images";
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), max_position_query, -1, &stmt, NULL);

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
      max_position = sqlite3_column_int64(stmt, 0);
      max_position = (max_position & 0xFFFFFFFF00000000) >> 32;
    }

    sqlite3_finalize(stmt);
    sqlite3_stmt *update_stmt = NULL;

    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "BEGIN", NULL, NULL, NULL);

    // move images to last position in custom image order table
    gchar *update_query = "UPDATE main.images SET position = ?1 WHERE id = ?2";
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), update_query, -1, &update_stmt, NULL);

    for (const GList * selected_images_iter = selected_images;
        selected_images_iter != NULL;
        selected_images_iter = selected_images_iter->next)
    {
      max_position++;
      const int moved_image_id = GPOINTER_TO_INT(selected_images_iter->data);
      DT_DEBUG_SQLITE3_BIND_INT64(update_stmt, 1, max_position << 32);
      DT_DEBUG_SQLITE3_BIND_INT(update_stmt, 2, moved_image_id);
      sqlite3_step(update_stmt);
      sqlite3_reset(update_stmt);
    }

    sqlite3_finalize(update_stmt);
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "COMMIT", NULL, NULL, NULL);
  }
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
