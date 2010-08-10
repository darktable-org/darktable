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
#define ORDER_BY_QUERY "order by %s limit ?1, ?2"

#define MAX_QUERY_STRING_LENGTH 4096
/* Stores the collection query, returns 1 if changed.. */
static int _dt_collection_store (gchar *query);

const dt_collection_t * 
dt_collection_new (dt_collection_params_t *params)
{
  dt_collection_t *collection = g_malloc (sizeof (dt_collection_t));
  memset (collection,0,sizeof (dt_collection_t));
  
  /* initialize collection context*/
  if (params)   /* if params are provided let's copy them into this context */
    memcpy (&collection->params,params,sizeof (dt_collection_params_t));
  else  /* else we just initialize using the reset */
    dt_collection_reset (collection);
    
  return collection;
}

void 
dt_collection_free (const dt_collection_t *collection)
{
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
  gchar sq[512]={0};   // sort query
  gchar selq[512]={0};   // selection query
  gchar wq[512]={0};   // where query
  gchar *query=g_malloc (MAX_QUERY_STRING_LENGTH);

  dt_lib_sort_t sort = dt_conf_get_int ("ui_last/combo_sort");
//  dt_lib_filter_t filter = dt_conf_get_int ("ui_last/combo_filter");
  
  /* build where part */
  if (collection->params.filter_flags & COLLECTION_FILTER_FILM_ID)
    g_snprintf (wq,512,"(film_id = %d)",collection->params.film_id);
  
  if (collection->params.filter_flags & COLLECTION_FILTER_ATLEAST_STAR)
    g_snprintf (wq+strlen(wq),512-strlen(wq)," and (flags & 7) >= %d",collection->params.star);
  else if (collection->params.filter_flags & COLLECTION_FILTER_EQUAL_STAR)
    g_snprintf (wq+strlen(wq),512-strlen(wq)," and (flags & 7) == %d",collection->params.star);
    
  /* build select part includes where */
  if (sort == DT_LIB_SORT_COLOR)
    g_snprintf (selq,512,"select distinct id from (select * from images where %s) as a left outer join color_labels as b on a.id = b.imgid",wq);
  else
    g_snprintf(selq,512, "select distinct id from images where %s",wq);
  
  /* build sort order part */
  if (sort == DT_LIB_SORT_DATETIME)              g_snprintf (sq,512,ORDER_BY_QUERY, "datetime_taken");
  else if(sort == DT_LIB_SORT_RATING)           g_snprintf (sq,512,ORDER_BY_QUERY, "flags & 7 desc");
  else if(sort == DT_LIB_SORT_FILENAME)       g_snprintf (sq,512,ORDER_BY_QUERY, "filename");
  else if(sort == DT_LIB_SORT_ID)                   g_snprintf (sq,512,ORDER_BY_QUERY, "id");
  else if(sort == DT_LIB_SORT_COLOR)            g_snprintf (sq,512, ORDER_BY_QUERY, "color desc, filename");
  
  /* store the new query */
  g_snprintf (query,MAX_QUERY_STRING_LENGTH,"%s %s", selq, sq);
  result = _dt_collection_store (query);
  g_free (query); 
  return result;
}

void 
dt_collection_reset(const dt_collection_t *collection)
{
  dt_collection_params_t *params=(dt_collection_params_t *)&collection->params;
  params->film_id = -1;
  params->filter_flags = COLLECTION_FILTER_FILM_ID | COLLECTION_FILTER_ATLEAST_STAR;
  params->star = 1;
  dt_conf_set_string ("plugins/lighttable/query", "select * from images where (film_id = -1) and (flags & 7) >= 1 order by filename limit ?1, ?2");
  
  dt_collection_update (collection);
   // void dt_control_log("[DTCOLLECTION] Failed to ", ...);

}

const gchar *
dt_collection_get_query (dt_collection_t *collection)
{
  return collection.query;
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

void 
dt_collection_set_film_id (const dt_collection_t *collection, uint32_t film_id)
{
  dt_collection_params_t *params=(dt_collection_params_t *)&collection->params;
  params->film_id = film_id;
}

void 
dt_collection_set_star (const dt_collection_t *collection, uint32_t star)
{
  dt_collection_params_t *params=(dt_collection_params_t *)&collection->params;
  params->star = star;
}

static int 
_dt_collection_store (gchar *query) 
{
  if ( strcmp (collection.query,query) == 0) 
    return 0;
  
  /* store query in context */
  if (collection.query)
    g_free (collection.query);
  
  collection.query = g_strdup(query);
  
  /* store query in gconf */
  dt_conf_set_string ("plugins/lighttable/query", collection.query);
  
  fprintf (stderr,"Collection query: %s\n",collection.query);
  
  return 1;
}
