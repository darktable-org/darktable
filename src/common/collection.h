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
#include <glib/gi18n.h>
#include <inttypes.h>
#include "common/metadata.h"

#define DT_COLLECTION_MAX_RULES 10

typedef enum dt_collection_query_t
{
  COLLECTION_QUERY_SIMPLE             = 0,      // a query with only select and where statement
  COLLECTION_QUERY_USE_SORT           = 1 << 0, // if query should include order by statement
  COLLECTION_QUERY_USE_LIMIT          = 1 << 1, // if query should include "limit ?1,?2" part
  COLLECTION_QUERY_USE_WHERE_EXT      = 1 << 2, // if query should include extended where part
  COLLECTION_QUERY_USE_ONLY_WHERE_EXT = 1 << 3  // if query should only use extended where part
} dt_collection_query_t;
#define COLLECTION_QUERY_FULL (COLLECTION_QUERY_USE_SORT | COLLECTION_QUERY_USE_LIMIT)

typedef enum dt_collection_filter_comparator_t
{
  COLLECTION_FILTER_NONE            = 0,
  COLLECTION_FILTER_FILM_ID         = 1 << 0, // use film_id in filter
  COLLECTION_FILTER_ATLEAST_RATING  = 1 << 1, // show all stars including and above selected star filter
  COLLECTION_FILTER_EQUAL_RATING    = 1 << 2, // show only selected star filter
  COLLECTION_FILTER_ALTERED         = 1 << 3, // show only altered images
  COLLECTION_FILTER_UNALTERED       = 1 << 4, // show only unaltered images
  COLLECTION_FILTER_REJECTED        = 1 << 5, // show only rejected images
  COLLECTION_FILTER_CUSTOM_COMPARE  = 1 << 6  // use the comparator defined in the comparator field to filter stars
} dt_collection_filter_comparator_t;

typedef enum dt_collection_sort_t
{
  DT_COLLECTION_SORT_NONE = -1,
  DT_COLLECTION_SORT_FILENAME = 0,
  DT_COLLECTION_SORT_DATETIME,
  DT_COLLECTION_SORT_IMPORT_TIMESTAMP,
  DT_COLLECTION_SORT_CHANGE_TIMESTAMP,
  DT_COLLECTION_SORT_EXPORT_TIMESTAMP,
  DT_COLLECTION_SORT_PRINT_TIMESTAMP,
  DT_COLLECTION_SORT_RATING,
  DT_COLLECTION_SORT_ID,
  DT_COLLECTION_SORT_COLOR,
  DT_COLLECTION_SORT_GROUP,
  DT_COLLECTION_SORT_PATH,
  DT_COLLECTION_SORT_CUSTOM_ORDER,
  DT_COLLECTION_SORT_TITLE,
  DT_COLLECTION_SORT_DESCRIPTION,
  DT_COLLECTION_SORT_ASPECT_RATIO,
  DT_COLLECTION_SORT_SHUFFLE,
  DT_COLLECTION_SORT_LAST
} dt_collection_sort_t;

#define DT_COLLECTION_ORDER_FLAG 0x8000

/* NOTE: any reordeing in this module require a legacy_preset entry in src/libs/collect.c */
typedef enum dt_collection_properties_t
{
  DT_COLLECTION_PROP_FILMROLL = 0,
  DT_COLLECTION_PROP_FOLDERS,
  DT_COLLECTION_PROP_FILENAME,

  DT_COLLECTION_PROP_CAMERA,
  DT_COLLECTION_PROP_LENS,
  DT_COLLECTION_PROP_APERTURE,
  DT_COLLECTION_PROP_EXPOSURE,
  DT_COLLECTION_PROP_FOCAL_LENGTH,
  DT_COLLECTION_PROP_ISO,

  DT_COLLECTION_PROP_DAY,
  DT_COLLECTION_PROP_TIME,
  DT_COLLECTION_PROP_IMPORT_TIMESTAMP,
  DT_COLLECTION_PROP_CHANGE_TIMESTAMP,
  DT_COLLECTION_PROP_EXPORT_TIMESTAMP,
  DT_COLLECTION_PROP_PRINT_TIMESTAMP,

  DT_COLLECTION_PROP_GEOTAGGING,
  DT_COLLECTION_PROP_ASPECT_RATIO,
  DT_COLLECTION_PROP_TAG,
  DT_COLLECTION_PROP_COLORLABEL,
  DT_COLLECTION_PROP_METADATA,
  DT_COLLECTION_PROP_GROUPING = DT_COLLECTION_PROP_METADATA + DT_METADATA_NUMBER,
  DT_COLLECTION_PROP_LOCAL_COPY,

  DT_COLLECTION_PROP_HISTORY,
  DT_COLLECTION_PROP_MODULE,
  DT_COLLECTION_PROP_ORDER,
  DT_COLLECTION_PROP_RATING_RANGE,

  DT_COLLECTION_PROP_TEXTSEARCH,
  DT_COLLECTION_PROP_RATING,

  DT_COLLECTION_PROP_LAST,

  DT_COLLECTION_PROP_UNDEF,
  DT_COLLECTION_PROP_SORT
} dt_collection_properties_t;

typedef enum dt_collection_change_t
{
  DT_COLLECTION_CHANGE_NONE      = 0,
  DT_COLLECTION_CHANGE_NEW_QUERY = 1, // a completly different query
  DT_COLLECTION_CHANGE_FILTER    = 2, // base query has been finetuned (filter, ...)
  DT_COLLECTION_CHANGE_RELOAD    = 3  // we have just reload the collection after images changes (query is identical)
} dt_collection_change_t;

typedef struct dt_collection_params_t
{
  /** flags for which query parts to use, see COLLECTION_QUERY_x defines... */
  uint32_t query_flags;

  /** flags for which filters to use, see COLLECTION_FILTER_x defines... */
  uint32_t filter_flags;

  /** current film id */
  uint32_t film_id;

  /** list of used sort orders */
  gboolean sorts[DT_COLLECTION_SORT_LAST];

} dt_collection_params_t;

typedef struct dt_collection_t
{
  int clone;
  gchar *query, *query_no_group;
  gchar **where_ext;
  unsigned int count, count_no_group;
  unsigned int tagid;
  dt_collection_params_t params;
  dt_collection_params_t store;
} dt_collection_t;

/* returns the name for the given collection property */
const char *dt_collection_name(const dt_collection_properties_t prop);
const char *dt_collection_name_untranslated(const dt_collection_properties_t prop);

/** instantiates a collection context, if clone equals NULL default query is constructed. */
const dt_collection_t *dt_collection_new(const dt_collection_t *clone);
/** frees a collection context. */
void dt_collection_free(const dt_collection_t *collection);
/** fetch params for collection for storing. */
const dt_collection_params_t *dt_collection_params(const dt_collection_t *collection);
/** get the filtered map between sanitized makermodel and exif maker/model **/
void dt_collection_get_makermodels(const gchar *filter, GList **sanitized, GList **exif);
/** get the sanitized makermodel for exif maker/model **/
gchar *dt_collection_get_makermodel(const char *exif_maker,
                                    const char *exif_model);
/** get the generated query for collection */
const gchar *dt_collection_get_query(const dt_collection_t *collection);
/** get the generated query for collection including the images hidden in groups */
const gchar *dt_collection_get_query_no_group(const dt_collection_t *collection);
/** updates sql query for a collection. @return 1 if query changed. */
int dt_collection_update(const dt_collection_t *collection);
/** reset collection to default dummy selection */
void dt_collection_reset(const dt_collection_t *collection);
/** gets an extended where part */
gchar *dt_collection_get_extended_where(const dt_collection_t *collection,
                                        const int exclude);
/** sets an extended where part */
void dt_collection_set_extended_where(const dt_collection_t *collection,
                                      gchar **extended_where);

/** get filter flags for collection */
uint32_t dt_collection_get_filter_flags(const dt_collection_t *collection);
/** set filter flags for collection */
void dt_collection_set_filter_flags(const dt_collection_t *collection,
                                    const uint32_t flags);

/** get filter flags for collection */
uint32_t dt_collection_get_query_flags(const dt_collection_t *collection);
/** set filter flags for collection */
void dt_collection_set_query_flags(const dt_collection_t *collection,
                                   const uint32_t flags);

/** set the film_id of collection */
void dt_collection_set_film_id(const dt_collection_t *collection,
                               const int32_t film_id);
/** set the tagid of collection */
void dt_collection_set_tag_id(dt_collection_t *collection, const uint32_t tagid);

/** get the part of the query for sorting the collection **/
gchar *dt_collection_get_sort_query(const dt_collection_t *collection);
/* serialize and deserialize sorting into a string. */
void dt_collection_sort_deserialize(const char *buf);
void dt_collection_sort_serialize(char *buf, int bufsize);

/** get the count of query */
uint32_t dt_collection_get_count(const dt_collection_t *collection);
/** get the count of query including the images hidden in groups */
uint32_t dt_collection_get_count_no_group(const dt_collection_t *collection);
/** get the nth image in the query */
int dt_collection_get_nth(const dt_collection_t *collection, const int nth);
/** get all image ids order as current selection. no more than limit
 * many images are returned, <0 == unlimited */
GList *dt_collection_get_all(const dt_collection_t *collection, const int limit);
/** get selected image ids order as current selection. no more than
 * limit many images are returned, <0 == unlimited */
GList *dt_collection_get_selected(const dt_collection_t *collection, const int limit);

/** get the count of selected images */
uint32_t dt_collection_get_selected_count(void);
/** get the count of collected images */
uint32_t dt_collection_get_collected_count(void);

/** update query by conf vars */
void dt_collection_update_query(const dt_collection_t *collection,
                                const dt_collection_change_t query_change,
                                const dt_collection_properties_t changed_property,
                                GList *list);

/** updates the hint message for collection */
void dt_collection_hint_message(const dt_collection_t *collection);

/** returns the image offset in the collection */
int dt_collection_image_offset(dt_imgid_t imgid);

/* serialize and deserialize into a string. */
void dt_collection_deserialize(const char *buf, gboolean filtering);
int dt_collection_serialize(char *buf, int bufsize, gboolean filtering);

/* splits an input string into a number part and an optional operator part */
void dt_collection_split_operator_number(const gchar *input,
                                         char **number1,
                                         char **number2,
                                         char **op);
void dt_collection_split_operator_datetime(const gchar *input,
                                           char **number1,
                                           char **number2,
                                           char **op);
void dt_collection_split_operator_exposure(const gchar *input,
                                           char **number1,
                                           char **number2,
                                           char **op);

int64_t dt_collection_get_image_position(const dt_imgid_t image_id,
                                         const int32_t tagid);
void dt_collection_shift_image_positions(const unsigned int length,
                                         const int64_t image_position,
                                         const int32_t tagid);

/* move images with drag and drop */
void dt_collection_move_before(const dt_imgid_t image_id, GList * selected_images);

/* initialize memory table */
void dt_collection_memory_update();

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
