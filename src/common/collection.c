/*
    This file is part of darktable,
    copyright (c) 2010 Henrik Andersson.

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

#include <stdio.h>
#include <memory.h>
#include <stdlib.h>
#include <unistd.h>
#include <glib.h>

#include "control/conf.h"
#include "control/control.h"

#include "common/collection.h"

#define SELECT_QUERY "select distinct * from %s"
#define ORDER_BY_QUERY "order by %s"
#define LIMIT_QUERY "limit ?1, ?2"

#define MAX_QUERY_STRING_LENGTH 4096
/* Stores the collection query, returns 1 if changed.. */
static int _dt_collection_store (const dt_collection_t *collection, gchar *query);

const dt_collection_t * 
dt_collection_new (const dt_collection_t *clone)
{
  dt_collection_t *collection = g_malloc (sizeof (dt_collection_t));
  memset (collection,0,sizeof (dt_collection_t));
  
  /* initialize collection context*/
  if (clone)   /* if clone is provided let's copy it into this context */
  {
    memcpy (&collection->params,&clone->params,sizeof (dt_collection_params_t));
    memcpy (&collection->store,&clone->store,sizeof (dt_collection_params_t));
    collection->where_ext = g_strdup(clone->where_ext);
    collection->query = g_strdup(clone->query);
    collection->clone =1;
  }
  else  /* else we just initialize using the reset */
    dt_collection_reset (collection);
    
  return collection;
}

void 
dt_collection_free (const dt_collection_t *collection)
{
  if (collection->query)
    g_free (collection->query);
  if (collection->where_ext)
    g_free (collection->where_ext);
  g_free ((dt_collection_t *)collection);
}

const dt_collection_params_t * 
dt_collection_params (const dt_collection_t *collection)
{
  return &collection->params;
}

int 
dt_collection_update (const dt_collection_t *collection)
{
  uint32_t result;
  gchar sq[512]   = {0};   // sort query
  gchar selq[512] = {0};   // selection query
  gchar wq[2048]  = {0};   // where query
  gchar *query=g_malloc (MAX_QUERY_STRING_LENGTH);

  dt_lib_sort_t sort = dt_conf_get_int ("ui_last/combo_sort");
  
  /* build where part */
  if (!(collection->params.query_flags&COLLECTION_QUERY_USE_ONLY_WHERE_EXT))
  {
    int need_operator = 0;
    
    /* add default filters */
    if (collection->params.filter_flags & COLLECTION_FILTER_FILM_ID)
    {
      g_snprintf (wq,2048,"(film_id = %d)",collection->params.film_id);
      need_operator = 1;
    }
    
    if (collection->params.filter_flags & COLLECTION_FILTER_ATLEAST_RATING)
      g_snprintf (wq+strlen(wq),2048-strlen(wq)," %s (flags & 7) >= %d", (need_operator)?"and":((need_operator=1)?"":"") , collection->params.rating);
    else if (collection->params.filter_flags & COLLECTION_FILTER_EQUAL_RATING)
      g_snprintf (wq+strlen(wq),2048-strlen(wq)," %s (flags & 7) == %d", (need_operator)?"and":((need_operator=1)?"":"") , collection->params.rating);

    if (collection->params.filter_flags & COLLECTION_FILTER_ALTERED)
      g_snprintf (wq+strlen(wq),2048-strlen(wq)," %s id in (select imgid from history where imgid=id)", (need_operator)?"and":((need_operator=1)?"":"") );
    else if (collection->params.filter_flags & COLLECTION_FILTER_UNALTERED)
      g_snprintf (wq+strlen(wq),2048-strlen(wq)," %s id not in (select imgid from history where imgid=id)", (need_operator)?"and":((need_operator=1)?"":"") );
    
    /* add where ext if wanted */
    if ((collection->params.query_flags&COLLECTION_QUERY_USE_WHERE_EXT))
       g_snprintf (wq+strlen(wq),2048-strlen(wq)," %s %s",(need_operator)?"and":((need_operator=1)?"":""), collection->where_ext);
  } 
  else
    g_snprintf (wq,512,"%s",collection->where_ext);
  
    
  
  /* build select part includes where */
  if (sort == DT_LIB_SORT_COLOR && (collection->params.query_flags & COLLECTION_QUERY_USE_SORT))
    g_snprintf (selq,512,"select distinct id from (select * from images where %s) as a left outer join color_labels as b on a.id = b.imgid",wq);
  else
    g_snprintf(selq,512, "select distinct id from images where %s",wq);
  
  
  /* build sort order part */
  if ((collection->params.query_flags&COLLECTION_QUERY_USE_SORT))
  {
    if (sort == DT_LIB_SORT_DATETIME)           g_snprintf (sq, 512, ORDER_BY_QUERY, "datetime_taken");
    else if(sort == DT_LIB_SORT_RATING)         g_snprintf (sq, 512, ORDER_BY_QUERY, "flags & 7 desc");
    else if(sort == DT_LIB_SORT_FILENAME)       g_snprintf (sq, 512, ORDER_BY_QUERY, "filename");
    else if(sort == DT_LIB_SORT_ID)             g_snprintf (sq, 512, ORDER_BY_QUERY, "id");
    else if(sort == DT_LIB_SORT_COLOR)          g_snprintf (sq, 512, ORDER_BY_QUERY, "color desc, filename");
  }
  
  /* store the new query */
  g_snprintf (query,MAX_QUERY_STRING_LENGTH,"%s %s%s", selq, sq, (collection->params.query_flags&COLLECTION_QUERY_USE_LIMIT)?" "LIMIT_QUERY:"");
  result = _dt_collection_store (collection,query);
  g_free (query); 
  return result;
}

void 
dt_collection_reset(const dt_collection_t *collection)
{
  dt_collection_params_t *params=(dt_collection_params_t *)&collection->params;

  /* setup defaults */
  params->query_flags = COLLECTION_QUERY_FULL;
  params->filter_flags = COLLECTION_FILTER_FILM_ID | COLLECTION_FILTER_ATLEAST_RATING;
  params->film_id = dt_conf_get_int("ui_last/film_roll");
  params->rating = 1;

  /* check if stored query parameters exist */
  if (dt_conf_key_exists ("plugins/collection/filter_flags"))
  {
  /* apply stored query parameters from previous darktable session */
  params->film_id = dt_conf_get_int("plugins/collection/film_id");
  params->rating = dt_conf_get_int("plugins/collection/rating");
  params->query_flags = dt_conf_get_int("plugins/collection/query_flags");
  params->filter_flags= dt_conf_get_int("plugins/collection/filter_flags");
  }
  dt_collection_update (collection);
}

const gchar *
dt_collection_get_query (const dt_collection_t *collection)
{
  /* ensure there is a query string for collection */
  if(!collection->query) 
    dt_collection_update(collection);

  return collection->query;
}

uint32_t 
dt_collection_get_filter_flags(const dt_collection_t *collection)
{
  return  collection->params.filter_flags;
}

void 
dt_collection_set_filter_flags(const dt_collection_t *collection, uint32_t flags)
{
  dt_collection_params_t *params=(dt_collection_params_t *)&collection->params;
  params->filter_flags = flags;
}

uint32_t 
dt_collection_get_query_flags(const dt_collection_t *collection)
{
  return  collection->params.query_flags;
}

void 
dt_collection_set_query_flags(const dt_collection_t *collection, uint32_t flags)
{
  dt_collection_params_t *params=(dt_collection_params_t *)&collection->params;
  params->query_flags = flags;
}

void 
dt_collection_set_extended_where(const dt_collection_t *collection,gchar *extended_where)
{
  /* free extended where if alread exists */
  if (collection->where_ext)
    g_free (collection->where_ext);
  
  /* set new from parameter */
  ((dt_collection_t *)collection)->where_ext = g_strdup(extended_where);
}

void 
dt_collection_set_film_id (const dt_collection_t *collection, uint32_t film_id)
{
  dt_collection_params_t *params=(dt_collection_params_t *)&collection->params;
  params->film_id = film_id;
}

void 
dt_collection_set_rating (const dt_collection_t *collection, uint32_t rating)
{
  dt_collection_params_t *params=(dt_collection_params_t *)&collection->params;
  params->rating = rating;
}

static int 
_dt_collection_store (const dt_collection_t *collection, gchar *query) 
{
  if (collection->query && strcmp (collection->query,query) == 0) 
    return 0;
  
  /* store flags to gconf */
  if (!collection->clone)
  {
    dt_conf_set_int ("plugins/collection/query_flags",collection->params.query_flags);
    dt_conf_set_int ("plugins/collection/filter_flags",collection->params.filter_flags);
    dt_conf_set_int ("plugins/collection/film_id",collection->params.film_id);
    dt_conf_set_int ("plugins/collection/rating",collection->params.rating);
  }
  
  /* store query in context */
  if (collection->query)
    g_free (collection->query);
  
  ((dt_collection_t *)collection)->query = g_strdup(query);
  
  return 1;
}

uint32_t dt_collection_get_count(const dt_collection_t *collection) {
  sqlite3_stmt *stmt = NULL;
  uint32_t count=1;
  const gchar *query = dt_collection_get_query(collection);
  char countquery[2048]={0};
  snprintf(countquery, 2048, "select count(id) %s", query + 18);
  sqlite3_prepare_v2(darktable.db, countquery, -1, &stmt, NULL);
  sqlite3_bind_int (stmt, 1, 0);
  sqlite3_bind_int (stmt, 2, -1);
  if(sqlite3_step(stmt) == SQLITE_ROW)
    count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return count;
}


GList *dt_collection_get_selected (const dt_collection_t *collection)
{
  GList *list=NULL;
  dt_lib_sort_t sort = dt_conf_get_int ("ui_last/combo_sort");

  /* get collection order */
  char sq[512]={0};
  if ((collection->params.query_flags&COLLECTION_QUERY_USE_SORT))
  {
    if (sort == DT_LIB_SORT_DATETIME)           g_snprintf (sq, 512, ORDER_BY_QUERY, "datetime_taken");
    else if(sort == DT_LIB_SORT_RATING)         g_snprintf (sq, 512, ORDER_BY_QUERY, "flags & 7 desc");
    else if(sort == DT_LIB_SORT_FILENAME)       g_snprintf (sq, 512, ORDER_BY_QUERY, "filename");
    else if(sort == DT_LIB_SORT_ID)             g_snprintf (sq, 512, ORDER_BY_QUERY, "id");
    else if(sort == DT_LIB_SORT_COLOR)          g_snprintf (sq, 512, ORDER_BY_QUERY, "color desc, filename");
  }
  
  
  sqlite3_stmt *stmt = NULL;
  int rc = 0;
  char query[2048]={0};
  snprintf(query, 2048, "select distinct id from selected_images as a left outer join images as b on a.imgid=b.id %s", sq);
  rc = sqlite3_prepare_v2 (darktable.db,query, -1, &stmt, NULL);
  while (sqlite3_step (stmt) == SQLITE_ROW)
  {
    long int imgid = sqlite3_column_int(stmt, 0);
    list = g_list_append (list, (gpointer)imgid);
  }

  return list;
}