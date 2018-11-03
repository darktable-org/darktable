/*
    This file is part of darktable,
    copyright (c) 2010--2011 Henrik Andersson.

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
#include <inttypes.h>

typedef enum dt_collection_query_t
{
  COLLECTION_QUERY_SIMPLE = 0,                 // a query with only select and where statement
  COLLECTION_QUERY_USE_SORT = 1 << 0,          // if query should include order by statement
  COLLECTION_QUERY_USE_LIMIT = 1 << 1,         // if query should include "limit ?1,?2" part
  COLLECTION_QUERY_USE_WHERE_EXT = 1 << 2,     // if query should include extended where part
  COLLECTION_QUERY_USE_ONLY_WHERE_EXT = 1 << 3 // if query should only use extended where part
} dt_collection_query_t;
#define COLLECTION_QUERY_FULL (COLLECTION_QUERY_USE_SORT | COLLECTION_QUERY_USE_LIMIT)

typedef enum dt_collection_filter_comparator_t
{
  COLLECTION_FILTER_NONE = 0,
  COLLECTION_FILTER_FILM_ID = 1 << 0,        // use film_id in filter
  COLLECTION_FILTER_ATLEAST_RATING = 1 << 1, // show all stars including and above selected star filter
  COLLECTION_FILTER_EQUAL_RATING = 1 << 2,   // show only selected star filter
  COLLECTION_FILTER_ALTERED = 1 << 3,        // show only altered images
  COLLECTION_FILTER_UNALTERED = 1 << 4,      // show only unaltered images
  COLLECTION_FILTER_CUSTOM_COMPARE
  = 1 << 5 // use the comparator defined in the comparator field to filter stars
} dt_collection_filter_comparator_t;

typedef enum dt_collection_filter_t
{
  DT_COLLECTION_FILTER_ALL = 0,
  DT_COLLECTION_FILTER_STAR_NO = 1,
  DT_COLLECTION_FILTER_STAR_1 = 2,
  DT_COLLECTION_FILTER_STAR_2 = 3,
  DT_COLLECTION_FILTER_STAR_3 = 4,
  DT_COLLECTION_FILTER_STAR_4 = 5,
  DT_COLLECTION_FILTER_STAR_5 = 6,
  DT_COLLECTION_FILTER_REJECT = 7,
  DT_COLLECTION_FILTER_NOT_REJECT = 8
} dt_collection_filter_t;

typedef enum dt_collection_sort_t
{
  DT_COLLECTION_SORT_NONE = -1,
  DT_COLLECTION_SORT_FILENAME = 0,
  DT_COLLECTION_SORT_DATETIME,
  DT_COLLECTION_SORT_RATING,
  DT_COLLECTION_SORT_ID,
  DT_COLLECTION_SORT_COLOR,
  DT_COLLECTION_SORT_GROUP,
  DT_COLLECTION_SORT_PATH,
  DT_COLLECTION_SORT_CUSTOM_ORDER,
  DT_COLLECTION_SORT_TITLE,
  DT_COLLECTION_SORT_DESCRIPTION,
  DT_COLLECTION_SORT_ASPECT_RATIO
} dt_collection_sort_t;

typedef enum dt_collection_properties_t
{
  DT_COLLECTION_PROP_FILMROLL,
  DT_COLLECTION_PROP_FOLDERS,
  DT_COLLECTION_PROP_CAMERA,
  DT_COLLECTION_PROP_TAG,
  DT_COLLECTION_PROP_DAY,
  DT_COLLECTION_PROP_TIME,
  DT_COLLECTION_PROP_HISTORY,
  DT_COLLECTION_PROP_COLORLABEL,
  DT_COLLECTION_PROP_TITLE,
  DT_COLLECTION_PROP_DESCRIPTION,
  DT_COLLECTION_PROP_CREATOR,
  DT_COLLECTION_PROP_PUBLISHER,
  DT_COLLECTION_PROP_RIGHTS,
  DT_COLLECTION_PROP_LENS,
  DT_COLLECTION_PROP_FOCAL_LENGTH,
  DT_COLLECTION_PROP_ISO,
  DT_COLLECTION_PROP_APERTURE,
  DT_COLLECTION_PROP_EXPOSURE,
  DT_COLLECTION_PROP_ASPECT_RATIO,
  DT_COLLECTION_PROP_FILENAME,
  DT_COLLECTION_PROP_GEOTAGGING,
  DT_COLLECTION_PROP_GROUPING,
  DT_COLLECTION_PROP_LOCAL_COPY
} dt_collection_properties_t;

typedef enum dt_collection_rating_comperator_t
{
  DT_COLLECTION_RATING_COMP_LT = 0,
  DT_COLLECTION_RATING_COMP_LEQ = 1,
  DT_COLLECTION_RATING_COMP_EQ = 2,
  DT_COLLECTION_RATING_COMP_GEQ = 3,
  DT_COLLECTION_RATING_COMP_GT = 4,
  DT_COLLECTION_RATING_COMP_NE = 5,
  DT_COLLECTION_RATING_N_COMPS = 6
} dt_collection_rating_comperator_t;

typedef struct dt_collection_params_t
{
  /** flags for which query parts to use, see COLLECTION_QUERY_x defines... */
  uint32_t query_flags;

  /** flags for which filters to use, see COLLECTION_FILTER_x defines... */
  uint32_t filter_flags;

  /** current film id */
  uint32_t film_id;

  /** current  filter */
  uint32_t rating;
  dt_collection_rating_comperator_t comparator;

  /** sorting **/
  dt_collection_sort_t sort; // Has to be changed to a dt_collection_sort struct
  gint descending;

} dt_collection_params_t;

typedef struct dt_collection_t
{
  int clone;
  gchar *query, *query_no_group;
  gchar **where_ext;
  unsigned int count, count_no_group;
  dt_collection_params_t params;
  dt_collection_params_t store;
} dt_collection_t;


/** instantiates a collection context, if clone equals NULL default query is constructed. */
const dt_collection_t *dt_collection_new(const dt_collection_t *clone);
/** frees a collection context. */
void dt_collection_free(const dt_collection_t *collection);
/** fetch params for collection for storing. */
const dt_collection_params_t *dt_collection_params(const dt_collection_t *collection);
/** get the filtered map between sanitized makermodel and exif maker/model **/
void dt_collection_get_makermodels(const gchar *filter, GList **sanitized, GList **exif);
/** get the sanitized makermodel for exif maker/model **/
gchar *dt_collection_get_makermodel(const char *exif_maker, const char *exif_model);
/** get the generated query for collection */
const gchar *dt_collection_get_query(const dt_collection_t *collection);
/** get the generated query for collection including the images hidden in groups */
const gchar *dt_collection_get_query_no_group(const dt_collection_t *collection);
/** updates sql query for a collection. @return 1 if query changed. */
int dt_collection_update(const dt_collection_t *collection);
/** reset collection to default dummy selection */
void dt_collection_reset(const dt_collection_t *collection);
/** gets an extended where part */
gchar *dt_collection_get_extended_where(const dt_collection_t *collection, int exclude);
/** sets an extended where part */
void dt_collection_set_extended_where(const dt_collection_t *collection, gchar **extended_where);

/** get filter flags for collection */
uint32_t dt_collection_get_filter_flags(const dt_collection_t *collection);
/** set filter flags for collection */
void dt_collection_set_filter_flags(const dt_collection_t *collection, uint32_t flags);

/** get filter flags for collection */
uint32_t dt_collection_get_query_flags(const dt_collection_t *collection);
/** set filter flags for collection */
void dt_collection_set_query_flags(const dt_collection_t *collection, uint32_t flags);

/** set the film_id of collection */
void dt_collection_set_film_id(const dt_collection_t *collection, uint32_t film_id);
/** set the star level for filter */
void dt_collection_set_rating(const dt_collection_t *collection, uint32_t rating);
/** get the star level for filter. The value returned starts on 0 **/
uint32_t dt_collection_get_rating(const dt_collection_t *collection);
/** set the comparator for rating */
void dt_collection_set_rating_comparator(const dt_collection_t *collection,
                                         const dt_collection_rating_comperator_t comparator);
/** get the comparator for rating */
dt_collection_rating_comperator_t dt_collection_get_rating_comparator(const dt_collection_t *collection);

/** set the sort fields and flags used to show the collection **/
void dt_collection_set_sort(const dt_collection_t *collection, dt_collection_sort_t sort, gint reverse);
/** get the sort field used **/
dt_collection_sort_t dt_collection_get_sort_field(const dt_collection_t *collection);
/** get if the collection must be shown in descending order **/
gboolean dt_collection_get_sort_descending(const dt_collection_t *collection);
/** get the part of the query for sorting the collection **/
gchar *dt_collection_get_sort_query(const dt_collection_t *collection);

/** get the count of query */
uint32_t dt_collection_get_count(const dt_collection_t *collection);
/** get the count of query including the images hidden in groups */
uint32_t dt_collection_get_count_no_group(const dt_collection_t *collection);
/** get the nth image in the query */
int dt_collection_get_nth(const dt_collection_t *collection, int nth);
/** get all image ids order as current selection. no more than limit many images are returned, <0 ==
 * unlimited */
GList *dt_collection_get_all(const dt_collection_t *collection, int limit);
/** get selected image ids order as current selection. no more than limit many images are returned, <0 ==
 * unlimited */
GList *dt_collection_get_selected(const dt_collection_t *collection, int limit);
/** get the count of selected images */
uint32_t dt_collection_get_selected_count(const dt_collection_t *collection);

/** update query by conf vars */
void dt_collection_update_query(const dt_collection_t *collection);

/** updates the hint message for collection */
void dt_collection_hint_message(const dt_collection_t *collection);

/** returns the image offset in the collection */
int dt_collection_image_offset(int imgid);

/* serialize and deserialize into a string. */
void dt_collection_deserialize(char *buf);
int dt_collection_serialize(char *buf, int bufsize);

/* splits an input string into a number part and an optional operator part */
void dt_collection_split_operator_number(const gchar *input, char **number1, char **number2, char **operator);
void dt_collection_split_operator_datetime(const gchar *input, char **number1, char **number2,
                                           char **operator);
void dt_collection_split_operator_exposure(const gchar *input, char **number1, char **number2,
                                           char **operator);

int64_t dt_collection_get_image_position(const int32_t image_id);
void dt_collection_shift_image_positions(const unsigned int length, const int64_t image_position);

/* move images with drag and drop */
void dt_collection_move_before(const int32_t image_id, GList * selected_images);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
