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
#ifndef DT_COLLECTION_H
#define DT_COLLECTION_H

#include <inttypes.h>

#define COLLECTION_FILTER_FILM_ID                    1             // use film_id in filter
#define COLLECTION_FILTER_ATLEAST_STAR          2             // show all stars including and above selected star filter
#define COLLECTION_FILTER_EQUAL_STAR            4             // show only selected star filter

typedef struct dt_collection_params_t
{
  /** flags for which filters to use, see COLLECTION_FILTER_x defines... */
  uint32_t filter_flags;
  
  /** current film id */
  uint32_t film_id;
  
  /** current star filter */
  uint32_t star;
  
} dt_collection_params_t;

typedef struct dt_collection_t
{
  gchar *query;
  dt_collection_params_t params;
} dt_collection_t;


/** instansiates a collection context, if params equals NULL default query is constructed. */
const dt_collection_t * dt_collection_new (dt_collection_params_t *params);
/** frees a collection context. */
void dt_collection_free (const dt_collection_t *collection);
/** fetch params for collection for storing. */
const dt_collection_params_t * dt_collection_params (const dt_collection_t *collection);
/** get the generated query for collection */
const gchar *dt_collection_get_query (const dt_collection_t *collection);
/** updates sql query for a collection. @return 1 if query changed. */
int dt_collection_update (const dt_collection_t *collection);
/** reset collection to default dummy selection */
void dt_collection_reset (const dt_collection_t *collection);

/** get filter flags for collection */
uint32_t dt_collection_get_filter_flags (const dt_collection_t *collection);
/** set filter flags for collection */
void dt_collection_set_filter_flags (const dt_collection_t *collection, uint32_t flags);

/** set the film_id of collection */
void dt_collection_set_film_id (const dt_collection_t *collection, uint32_t film_id); 
/** set the star level for filter */
void dt_collection_set_star (const dt_collection_t *collection, uint32_t star); 



#endif