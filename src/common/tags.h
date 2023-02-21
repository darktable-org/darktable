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

#pragma once

#include <glib.h>
#include <sqlite3.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct dt_tag_t
{
  guint id;
  gchar *tag;
  gchar *leave;
  gchar *synonym;
  guint count;
  guint select;
  gint flags;
} dt_tag_t;

typedef enum dt_tag_flags_t
{
  DT_TF_NONE        = 0,
  DT_TF_CATEGORY    = 1 << 0, // this tag (or path) is not a keyword to be exported
  DT_TF_PRIVATE     = 1 << 1, // this tag is private. Will be exported only on demand
  DT_TF_ORDER_SET   = 1 << 2, // set if the tag has got an images order
  DT_TF_DESCENDING  = 1U << 31,
} dt_tag_flags_t;

#define DT_TF_ALL (DT_TF_CATEGORY | DT_TF_PRIVATE | DT_TF_ORDER_SET)

typedef enum dt_tag_selection_t
{
  DT_TS_NO_IMAGE = 0,   // no selection or no tag not attached
  DT_TS_SOME_IMAGES,    // tag attached on some selected images
  DT_TS_ALL_IMAGES      // tag attached on all selected images
} dt_tag_selection_t;

/** creates a new tag, returns tagid \param[in] name the tag name. \param[in] tagid a pointer to tagid of new
 * tag, this can be NULL \return false if failed to create a tag and indicates that tagid is invalid to use.
 * \note If tag already exists the existing tag id is returned. */
gboolean dt_tag_new(const char *name, guint *tagid);

/** creates a new tag, returns tagid \param[in] name the tag name. \param[in] tagid a pointer to tagid of new
 * tag, this can be NULL \return false if failed to create a tag and indicates that tagid is invalid to use.
 * \note If tag already exists the existing tag id is returned. This function will also raise a
 * DT_SIGNAL_TAG_CHANGED signal if necessary, so keywords GUI can refresh. */
gboolean dt_tag_new_from_gui(const char *name, guint *tagid);

// read/import tags from a txt file as written by Lightroom. returns the number of imported tags
// or -1 if an error occurred.
ssize_t dt_tag_import(const char *filename);

// export all tags to a txt file as written by Lightroom. returns the number of exported tags
// or -1 if an error occurred.
ssize_t dt_tag_export(const char *filename);

/** get the name of specified id */
gchar *dt_tag_get_name(const guint tagid);

/** removes a tag from db and from assigned images. \param final TRUE actually performs the remove  \return
 * the amount of images affected. */
guint dt_tag_remove(const guint tagid, gboolean final);

/** removes a list of tags from db and from assigned images. \return the number of tags deleted */
guint dt_tag_remove_list(GList *tag_list);

/** set the name of specified id */
void dt_tag_rename(const guint tagid, const gchar *new_tagname);

/** checks if tag exists. \param[in] name of tag to check. \return the id of found tag or -1 i not found. */
gboolean dt_tag_exists(const char *name, guint *tagid);

/** attach a tag on images list. tagid id of tag to attach. img the list of image
 * id to attach tag to */
gboolean dt_tag_attach_images(const guint tagid, const GList *img, const gboolean undo_on);
/** attach a tag on images. tagid id of tag to attach. imgid the image
 * id to attach tag to, if < 0 images to act on are used. */
gboolean dt_tag_attach(const guint tagid, const gint imgid, const gboolean undo_on, const gboolean group_on);

/** check if a tag is attached to the given image */
gboolean dt_is_tag_attached(const guint tagid, const gint imgid);

/** attach a list of tags on selected images. \param[in] tags a list of ids of tags. \param[in] imgid the
 * image id to attach tag to, if < 0 selected images are used. \note If tag not exists it's created
 * if clear_on TRUE the image tags are cleared before attaching the new ones*/
gboolean dt_tag_set_tags(const GList *tags, const GList *img, const gboolean ignore_dt_tags,
                         const gboolean clear_on, const gboolean undo_on);

/** attach a list of tags on list of images. \param[in] tags a comma separated string of tags. \param[in]
 * img the list of images to attach tag to. \note If tag does not exist, it's created.*/
gboolean dt_tag_attach_string_list(const gchar *tags, const GList *img, const gboolean undo_on);

/** detach tag from images. \param[in] tagid of tag to detach. \param[in] img the list of image id to detach
 * tag from */
gboolean dt_tag_detach_images(const guint tagid, const GList *img, const gboolean undo_on);
/** detach tag from images. \param[in] tagid of tag to detach. \param[in] imgid the image id to detach
 * tag from, if < 0 images to act on are used. */
gboolean dt_tag_detach(const guint tagid, const gint imgid, const gboolean undo_on, const gboolean group_on);

/** detach tags from images that matches name, it is valid to use % to match tag */
gboolean dt_tag_detach_by_string(const char *name, const gint imgid, const gboolean undo_on,
                                 const gboolean group_on);

/** retrieves a list of tags of specified imgid \param[out] result a list of dt_tag_t, sorted by tag. */
uint32_t dt_tag_get_attached(const gint imgid, GList **result, const gboolean ignore_dt_tags);

/** sort tags per name (including '|') or per count (desc) */
GList *dt_sort_tag(GList *tags, gboolean byname);

/** get a list of tags,
 *  the difference to dt_tag_get_attached() is that this one splits at '|' and filters out the "darktable|"
 * tags. */
GList *dt_tag_get_list(gint imgid);

/** get a list of tags,
 *  the difference to dt_tag_get_list() is that this one checks option for exportation */
GList *dt_tag_get_list_export(gint imgid, int32_t flags);

/** get a flat list of only hierarchical tags,
 *  the difference to dt_tag_get_attached() is that this one filters out the "darktable|" tags. */
GList *dt_tag_get_hierarchical(gint imgid);

/** get a flat list of only hierarchical tags,
 *  the difference to dt_tag_get_hierarchical() is that this one checks option for exportation */
GList *dt_tag_get_hierarchical_export(gint imgid, int32_t flags);

/** get a flat list of tags id attached to image id*/
GList *dt_tag_get_tags(const gint imgid, const gboolean ignore_dt_tags);

/** get the subset of images that have a given tag attached */
GList *dt_tag_get_images(const gint tagid);

/** get the subset of images from the given list that have a given tag attached */
GList *dt_tag_get_images_from_list(const GList *img, const gint tagid);

/** retrieves a list of suggested tags matching keyword. \param[in] keyword the keyword to search \param[out]
 * result a pointer to list populated with result. \return the count \note the limit of result is decided by
 * conf value "xxx" */
uint32_t dt_tag_get_suggestions(GList **result);

/** retrieves count of tagged images. \param[in] keyword the keyword to search \return
 * the count \note the limit of result is decided by conf value "xxx" */
void dt_tag_count_tags_images(const gchar *keyword, int *tag_count, int *img_count);

/** retrieves list of tags and tagged images. \param[in] keyword the keyword to search. \param[out] result pointers to list
 * populated with result. \note the limit of result is decided by conf value "xxx" */
void dt_tag_get_tags_images(const gchar *keyword, GList **tag_list, GList **img_list);

/** retrieves the list of tags matching keyword. \param[in] keyword the keyword to search \param[out]
 * result a pointer to list populated with result. \return the count \note the limit of result is decided by
 * conf value "xxx" */
uint32_t dt_tag_get_with_usage(GList **result);

/** retrieves synonyms of the tag */
gchar *dt_tag_get_synonyms(gint tagid);

/** sets synonyms of the tag */
void dt_tag_set_synonyms(gint tagid, gchar *synonyms);

/** retrieves flags of the tag */
gint dt_tag_get_flags(gint tagid);

/** sets flags of the tag */
void dt_tag_set_flags(gint tagid, gint flags);

/** retrieves a list of recent tags used. \param[out] result a pointer to list populated with result. \return
 * the count \note the limit of result is decided by conf value "xxx" */
uint32_t dt_tag_get_recent_used(GList **result);

/** frees the memory of a result set. */
void dt_tag_free_result(GList **result);

/** get number of selected images */
uint32_t dt_selected_images_count();

/** get number of images affected with that tag */
uint32_t dt_tag_images_count(gint tagid);

/** retrieves the subtags of requested level for the requested category */
char *dt_tag_get_subtags(const gint imgid, const char *category, const int level);

/** return the images order associated to that tag */
gboolean dt_tag_get_tag_order_by_id(const uint32_t tagid, uint32_t *sort,
                                          gboolean *descending);

/** save the images order on the tag */
void dt_tag_set_tag_order_by_id(const uint32_t tagid, const uint32_t sort,
                                const gboolean descending);

/** return the tagid of that tag - follow tag sensitivity - return 0 if not found*/
uint32_t dt_tag_get_tag_id_by_name(const char * const name);

/** init the darktable tags table */
void dt_set_darktable_tags();

#ifdef __cplusplus
} // extern "C"
#endif /* __cplusplus */

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

