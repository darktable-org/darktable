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

#include "control/conf.h"
#include "control/control.h"
#include "common/collection.h"
#include "common/debug.h"
#include "common/metadata.h"
#include "common/utility.h"
#include "common/image.h"

#include <stdio.h>
#include <memory.h>
#include <stdlib.h>
#include <unistd.h>
#include <glib.h>


#define SELECT_QUERY "select distinct * from %s"
#define ORDER_BY_QUERY "order by %s"
#define LIMIT_QUERY "limit ?1, ?2"

static const char *comparators[] = {
  "<",  // DT_COLLECTION_RATING_COMP_LT = 0,
  "<=", // DT_COLLECTION_RATING_COMP_LEQ,
  "=",  // DT_COLLECTION_RATING_COMP_EQ,
  ">=", // DT_COLLECTION_RATING_COMP_GEQ,
  ">",  // DT_COLLECTION_RATING_COMP_GT,
  "!=", // DT_COLLECTION_RATING_COMP_NE,
};

/* Stores the collection query, returns 1 if changed.. */
static int _dt_collection_store(const dt_collection_t *collection, gchar *query);
/* Counts the number of images in the current collection */
static uint32_t _dt_collection_compute_count(const dt_collection_t *collection);
/* signal handlers to update the cached count when something interesting might have happened.
 * we need 2 different since there are different kinds of signals we need to listen to. */
static void _dt_collection_recount_callback_1(gpointer instace, gpointer user_data);
static void _dt_collection_recount_callback_2(gpointer instance, uint8_t id, gpointer user_data);


const dt_collection_t *dt_collection_new(const dt_collection_t *clone)
{
  dt_collection_t *collection = g_malloc0(sizeof(dt_collection_t));

  /* initialize collection context*/
  if(clone) /* if clone is provided let's copy it into this context */
  {
    memcpy(&collection->params, &clone->params, sizeof(dt_collection_params_t));
    memcpy(&collection->store, &clone->store, sizeof(dt_collection_params_t));
    collection->where_ext = g_strdup(clone->where_ext);
    collection->query = g_strdup(clone->query);
    collection->clone = 1;
    collection->count = clone->count;
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
  g_free(collection->where_ext);
  g_free((dt_collection_t *)collection);
}

const dt_collection_params_t *dt_collection_params(const dt_collection_t *collection)
{
  return &collection->params;
}

int dt_collection_update(const dt_collection_t *collection)
{
  uint32_t result;
  gchar *wq, *sq, *selq, *query;
  wq = sq = selq = query = NULL;

  /* build where part */
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
                         (need_operator) ? "and" : ((need_operator = 1) ? "" : ""), DT_IMAGE_REMOVE,
                         DT_IMAGE_REMOVE);

    if(collection->params.filter_flags & COLLECTION_FILTER_CUSTOM_COMPARE)
      wq = dt_util_dstrcat(wq, " %s (flags & 7) %s %d and (flags & 7) != 6",
                           (need_operator) ? "and" : ((need_operator = 1) ? "" : ""),
                           comparators[collection->params.comparator], rating - 1);
    else if(collection->params.filter_flags & COLLECTION_FILTER_ATLEAST_RATING)
      wq = dt_util_dstrcat(wq, " %s (flags & 7) >= %d and (flags & 7) != 6",
                           (need_operator) ? "and" : ((need_operator = 1) ? "" : ""), rating - 1);
    else if(collection->params.filter_flags & COLLECTION_FILTER_EQUAL_RATING)
      wq = dt_util_dstrcat(wq, " %s (flags & 7) == %d",
                           (need_operator) ? "and" : ((need_operator = 1) ? "" : ""), rating - 1);

    if(collection->params.filter_flags & COLLECTION_FILTER_ALTERED)
      wq = dt_util_dstrcat(wq, " %s id in (select imgid from history where imgid=id)",
                           (need_operator) ? "and" : ((need_operator = 1) ? "" : ""));
    else if(collection->params.filter_flags & COLLECTION_FILTER_UNALTERED)
      wq = dt_util_dstrcat(wq, " %s id not in (select imgid from history where imgid=id)",
                           (need_operator) ? "and" : ((need_operator = 1) ? "" : ""));

    /* add where ext if wanted */
    if((collection->params.query_flags & COLLECTION_QUERY_USE_WHERE_EXT))
      wq = dt_util_dstrcat(wq, " %s %s", (need_operator) ? "and" : "", collection->where_ext);
  }
  else
    wq = dt_util_dstrcat(wq, "%s", collection->where_ext);

  /* grouping */
  if(darktable.gui && darktable.gui->grouping)
  {
    wq = dt_util_dstrcat(wq, " and (group_id = id or group_id = %d)", darktable.gui->expanded_group_id);
  }

  /* build select part includes where */
  if(collection->params.sort == DT_COLLECTION_SORT_COLOR
     && (collection->params.query_flags & COLLECTION_QUERY_USE_SORT))
    selq = dt_util_dstrcat(selq, "select distinct id from (select * from images where %s) as a left outer "
                                 "join color_labels as b on a.id = b.imgid",
                           wq);
  else if(collection->params.query_flags & COLLECTION_QUERY_USE_ONLY_WHERE_EXT)
    selq = dt_util_dstrcat(selq, "select distinct images.id from images %s", wq);
  else
    selq = dt_util_dstrcat(selq, "select distinct id from images where %s", wq);



  /* build sort order part */
  if(!(collection->params.query_flags & COLLECTION_QUERY_USE_ONLY_WHERE_EXT)
     && (collection->params.query_flags & COLLECTION_QUERY_USE_SORT))
  {
    sq = dt_collection_get_sort_query(collection);
  }

  /* store the new query */
  query
      = dt_util_dstrcat(query, "%s %s%s", selq, sq ? sq : "",
                        (collection->params.query_flags & COLLECTION_QUERY_USE_LIMIT) ? " " LIMIT_QUERY : "");
  result = _dt_collection_store(collection, query);

  /* free memory used */
  g_free(sq);
  g_free(wq);
  g_free(selq);
  g_free(query);

  /* update the cached count. collection isn't a real const anyway, we are writing to it in
   * _dt_collection_store, too. */
  ((dt_collection_t *)collection)->count = _dt_collection_compute_count(collection);
  dt_collection_hint_message(collection);

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

void dt_collection_set_extended_where(const dt_collection_t *collection, gchar *extended_where)
{
  /* free extended where if already exists */
  g_free(collection->where_ext);

  /* set new from parameter */
  ((dt_collection_t *)collection)->where_ext = g_strdup(extended_where);
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

void dt_collection_set_sort(const dt_collection_t *collection, dt_collection_sort_t sort, gboolean reverse)
{
  dt_collection_params_t *params = (dt_collection_params_t *)&collection->params;

  if(sort != DT_COLLECTION_SORT_NONE) params->sort = sort;
  if(reverse != -1) params->descending = reverse;
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
        sq = dt_util_dstrcat(sq, ORDER_BY_QUERY, "datetime_taken desc, filename, version");
        break;

      case DT_COLLECTION_SORT_RATING:
        sq = dt_util_dstrcat(sq, ORDER_BY_QUERY, "flags & 7, filename, version");
        break;

      case DT_COLLECTION_SORT_FILENAME:
        sq = dt_util_dstrcat(sq, ORDER_BY_QUERY, "filename desc, version");
        break;

      case DT_COLLECTION_SORT_ID:
        sq = dt_util_dstrcat(sq, ORDER_BY_QUERY, "id desc");
        break;

      case DT_COLLECTION_SORT_COLOR:
        sq = dt_util_dstrcat(sq, ORDER_BY_QUERY, "color, filename, version");
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
        sq = dt_util_dstrcat(sq, ORDER_BY_QUERY, "flags & 7 desc, filename, version");
        break;

      case DT_COLLECTION_SORT_FILENAME:
        sq = dt_util_dstrcat(sq, ORDER_BY_QUERY, "filename, version");
        break;

      case DT_COLLECTION_SORT_ID:
        sq = dt_util_dstrcat(sq, ORDER_BY_QUERY, "id");
        break;

      case DT_COLLECTION_SORT_COLOR:
        sq = dt_util_dstrcat(sq, ORDER_BY_QUERY, "color desc, filename, version");
        break;

      case DT_COLLECTION_SORT_NONE:
        // shouldn't happen
        break;
    }
  }

  return sq;
}


static int _dt_collection_store(const dt_collection_t *collection, gchar *query)
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

  ((dt_collection_t *)collection)->query = g_strdup(query);

  return 1;
}

static uint32_t _dt_collection_compute_count(const dt_collection_t *collection)
{
  sqlite3_stmt *stmt = NULL;
  uint32_t count = 1;
  const gchar *query = dt_collection_get_query(collection);
  gchar *count_query = NULL;

  gchar *fq = g_strstr_len(query, strlen(query), "from");
  if((collection->params.query_flags & COLLECTION_QUERY_USE_ONLY_WHERE_EXT))
    count_query = dt_util_dstrcat(NULL, "select count(images.id) from images %s", collection->where_ext);
  else
    count_query = dt_util_dstrcat(count_query, "select count(id) %s", fq);

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

uint32_t dt_collection_get_selected_count(const dt_collection_t *collection)
{
  sqlite3_stmt *stmt = NULL;
  uint32_t count = 0;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "select count (distinct imgid) from selected_images", -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return count;
}

GList *dt_collection_get_selected(const dt_collection_t *collection, int limit)
{
  GList *list = NULL;
  gchar *query = NULL;
  gchar *sq = NULL;

  /* get collection order */
  if((collection->params.query_flags & COLLECTION_QUERY_USE_SORT))
    sq = dt_collection_get_sort_query(collection);


  sqlite3_stmt *stmt = NULL;

  /* build the query string */
  query = dt_util_dstrcat(query, "select distinct id from images ");

  if(collection->params.sort == DT_COLLECTION_SORT_COLOR
     && (collection->params.query_flags & COLLECTION_QUERY_USE_SORT))
    query = dt_util_dstrcat(query, "as a left outer join color_labels as b on a.id = b.imgid ");

  query = dt_util_dstrcat(query, "where id in (select imgid from selected_images) %s limit ?1", sq);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, limit);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int imgid = sqlite3_column_int(stmt, 0);
    list = g_list_append(list, GINT_TO_POINTER(imgid));
  }


  /* free allocated strings */
  g_free(sq);

  g_free(query);

  return list;
}


/* splits an input string into a number part and an optional operator part.
   number can be a decimal integer or rational numerical item.
   operator can be any of "=", "<", ">", "<=", ">=" and "<>".

   number and operator are returned as pointers to null terminated strings in g_mallocated
   memory (to be g_free'd after use) - or NULL if no match is found.
*/
void dt_collection_split_operator_number(const gchar *input, char **number, char **operator)
{
  GRegex *regex;
  GMatchInfo *match_info;
  int match_count;

  *number = *operator= NULL;

  regex = g_regex_new("\\s*(=|<|>|<=|>=|<>)?\\s*([0-9]+\\.?[0-9]*)\\s*", 0, 0, NULL);
  g_regex_match_full(regex, input, -1, 0, 0, &match_info, NULL);
  match_count = g_match_info_get_match_count(match_info);

  if(match_count == 3)
  {
    *operator= g_match_info_fetch(match_info, 1);
    *number = g_match_info_fetch(match_info, 2);

    if(*operator&& strcmp(*operator, "") == 0)
    {
      g_free(*operator);
      *operator= NULL;
    }
  }

  g_match_info_free(match_info);
  g_regex_unref(regex);
}


static void get_query_string(const dt_collection_properties_t property, const gchar *escaped_text,
                             char *query, size_t query_len)
{
  switch(property)
  {
    case DT_COLLECTION_PROP_FILMROLL: // film roll
      if(!(escaped_text && *escaped_text))
        snprintf(query, query_len, "(film_id in (select id from film_rolls where folder like '%s%%'))",
                 escaped_text);
      else
        snprintf(query, query_len, "(film_id in (select id from film_rolls where folder like '%s'))",
                 escaped_text);
      break;

    case DT_COLLECTION_PROP_FOLDERS: // folders
      snprintf(query, query_len, "(film_id in (select id from film_rolls where folder like '%s%%'))",
               escaped_text);
      break;

    case DT_COLLECTION_PROP_COLORLABEL: // colorlabel
    {
      int color = 0;
      if(!(escaped_text && *escaped_text) || strcmp(escaped_text, "%") == 0)
        snprintf(query, query_len, "(id in (select imgid from color_labels where color IS NOT NULL))");
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
        snprintf(query, query_len, "(id in (select imgid from color_labels where color=%d))", color);
      }
    }
    break;

    case DT_COLLECTION_PROP_HISTORY: // history
      snprintf(query, query_len, "(id %s in (select imgid from history where imgid=images.id)) ",
               (strcmp(escaped_text, _("altered")) == 0) ? "" : "not");
      break;

    case DT_COLLECTION_PROP_GEOTAGGING: // geotagging
      snprintf(query, query_len, "(id %s in (select id AS imgid from images where (longitude IS NOT NULL AND "
                                 "latitude IS NOT NULL))) ",
               (strcmp(escaped_text, _("tagged")) == 0) ? "" : "not");
      break;

    case DT_COLLECTION_PROP_CAMERA: // camera
      snprintf(query, query_len, "(maker || ' ' || model like '%%%s%%')", escaped_text);
      break;
    case DT_COLLECTION_PROP_TAG: // tag
      snprintf(query, query_len, "(id in (select imgid from tagged_images as a join "
                                 "tags as b on a.tagid = b.id where name like '%s'))",
               escaped_text);
      break;

    // TODO: How to handle images without metadata? In the moment they are not shown.
    // TODO: Autogenerate this code?
    case DT_COLLECTION_PROP_TITLE: // title
      snprintf(query, query_len, "(id in (select id from meta_data where key = %d and value like '%%%s%%'))",
               DT_METADATA_XMP_DC_TITLE, escaped_text);
      break;
    case DT_COLLECTION_PROP_DESCRIPTION: // description
      snprintf(query, query_len, "(id in (select id from meta_data where key = %d and value like '%%%s%%'))",
               DT_METADATA_XMP_DC_DESCRIPTION, escaped_text);
      break;
    case DT_COLLECTION_PROP_CREATOR: // creator
      snprintf(query, query_len, "(id in (select id from meta_data where key = %d and value like '%%%s%%'))",
               DT_METADATA_XMP_DC_CREATOR, escaped_text);
      break;
    case DT_COLLECTION_PROP_PUBLISHER: // publisher
      snprintf(query, query_len, "(id in (select id from meta_data where key = %d and value like '%%%s%%'))",
               DT_METADATA_XMP_DC_PUBLISHER, escaped_text);
      break;
    case DT_COLLECTION_PROP_RIGHTS: // rights
      snprintf(query, query_len, "(id in (select id from meta_data where key = %d and value like '%%%s%%'))",
               DT_METADATA_XMP_DC_RIGHTS, escaped_text);
      break;
    case DT_COLLECTION_PROP_LENS: // lens
      snprintf(query, query_len, "(lens like '%%%s%%')", escaped_text);
      break;
    case DT_COLLECTION_PROP_ISO: // iso
    {
      gchar *operator, *number;
      dt_collection_split_operator_number(escaped_text, &number, &operator);

      if(operator&& number)
        snprintf(query, query_len, "(iso %s %s)", operator, number);
      else if(number)
        snprintf(query, query_len, "(iso = %s)", number);
      else
        snprintf(query, query_len, "(iso like '%%%s%%')", escaped_text);

      g_free(operator);
      g_free(number);
    }
    break;

    case DT_COLLECTION_PROP_APERTURE: // aperture
    {
      gchar *operator, *number;
      dt_collection_split_operator_number(escaped_text, &number, &operator);

      if(operator&& number)
        snprintf(query, query_len, "(aperture %s %s)", operator, number);
      else if(number)
        snprintf(query, query_len, "(aperture = %s)", number);
      else
        snprintf(query, query_len, "(aperture like '%%%s%%')", escaped_text);

      g_free(operator);
      g_free(number);
    }
    break;

    case DT_COLLECTION_PROP_FILENAME: // filename
      snprintf(query, query_len, "(filename like '%%%s%%')", escaped_text);
      break;

    default: // day or time
      snprintf(query, query_len, "(datetime_taken like '%%%s%%')", escaped_text);
      break;
  }
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
  char query[1024], confname[200];
  gchar *complete_query = NULL;

  const int _n_r = dt_conf_get_int("plugins/lighttable/collect/num_rules");
  const int num_rules = CLAMP(_n_r, 1, 10);
  char *conj[] = { "and", "or", "and not" };

  complete_query = dt_util_dstrcat(complete_query, "(");

  for(int i = 0; i < num_rules; i++)
  {
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/item%1d", i);
    const int property = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", i);
    gchar *text = dt_conf_get_string(confname);
    if(!text) break;
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", i);
    const int mode = dt_conf_get_int(confname);
    gchar *escaped_text = dt_util_str_replace(text, "'", "''");

    get_query_string(property, escaped_text, query, sizeof(query));

    if(i > 0)
      complete_query = dt_util_dstrcat(complete_query, " %s %s", conj[mode], query);
    else
      complete_query = dt_util_dstrcat(complete_query, "%s", query);

    g_free(escaped_text);
    g_free(text);
  }

  complete_query = dt_util_dstrcat(complete_query, ")");

  // printf("complete query: `%s'\n", complete_query);

  /* set the extended where and the use of it in the query */
  dt_collection_set_extended_where(collection, complete_query);
  dt_collection_set_query_flags(collection,
                                (dt_collection_get_query_flags(collection) | COLLECTION_QUERY_USE_WHERE_EXT));

  /* remove film id from default filter */
  dt_collection_set_filter_flags(collection,
                                 (dt_collection_get_filter_flags(collection) & ~COLLECTION_FILTER_FILM_ID));

  /* update query and at last the visual */
  dt_collection_update(collection);

  /* free string */
  g_free(complete_query);

  // remove from selected images where not in this query.
  sqlite3_stmt *stmt = NULL;
  const gchar *cquery = dt_collection_get_query(collection);
  complete_query = NULL;
  if(cquery && cquery[0] != '\0')
  {
    complete_query
        = dt_util_dstrcat(complete_query, "delete from selected_images where imgid not in (%s)", cquery);
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

void dt_collection_hint_message(const dt_collection_t *collection)
{
  /* collection hinting */
  gchar message[1024];
  int c = dt_collection_get_count(collection);
  int cs = dt_collection_get_selected_count(collection);
  g_snprintf(message, sizeof(message), ngettext("%d image of %d in current collection is selected",
                                                "%d images of %d in current collection are selected", cs),
             cs, c);
  dt_control_hinter_message(darktable.control, message);
}

int dt_collection_image_offset(int imgid)
{
  const gchar *qin = dt_collection_get_query(darktable.collection);
  int offset = 0;
  sqlite3_stmt *stmt;

  if(qin)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), qin, -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, 0);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, -1);

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

static void _dt_collection_recount_callback_1(gpointer instace, gpointer user_data)
{
  dt_collection_t *collection = (dt_collection_t *)user_data;
  int old_count = collection->count;
  collection->count = _dt_collection_compute_count(collection);
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
  collection->count = _dt_collection_compute_count(collection);
  if(!collection->clone)
  {
    if(old_count != collection->count) dt_collection_hint_message(collection);
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED);
  }
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
