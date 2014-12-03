/*
    This file is part of darktable,
    copyright (c) 2010 henrik andersson.

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

#ifndef DT_TAGS_H
#define DT_TAGS_H
#include <sqlite3.h>
#include <glib.h>

typedef struct dt_tag_t
{
  guint id;
  gchar *tag;
} dt_tag_t;

/** creates a new tag, returns tagid \param[in] name the tag name. \param[in] tagid a pointer to tagid of new
 * tag, this can be NULL \return false if failed to create a tag and indicates that tagid is invalid to use.
 * \note If tag already exists the existing tag id is returned. */
gboolean dt_tag_new(const char *name, guint *tagid);

/** creates a new tag, returns tagid \param[in] name the tag name. \param[in] tagid a pointer to tagid of new
 * tag, this can be NULL \return false if failed to create a tag and indicates that tagid is invalid to use.
 * \note If tag already exists the existing tag id is returned. This function will also raise a
 * DT_SIGNAL_TAG_CHANGED signal if necessary, so keywords GUI can refresh. */
gboolean dt_tag_new_from_gui(const char *name, guint *tagid);

/** get the name of specified id */
gchar *dt_tag_get_name(const guint tagid);

/** removes a tag from db and from assigned images. \param final TRUE actually performs the remove  \return
 * the amount of images affected. */
guint dt_tag_remove(const guint tagid, gboolean final);

/** checks if tag exists. \param[in] name of tag to check. \return the id of found tag or -1 i not found. */
gboolean dt_tag_exists(const char *name, guint *tagid);

/** attach a list of tags on selected images. \param[in] tagid id of tag to attach. \param[in] imgid the image
 * id to attach tag to, if < 0 selected images are used. */
void dt_tag_attach(guint tagid, gint imgid);

/** attach a list of tags on selected images. \param[in] tags a list of ids of tags. \param[in] imgid the
 * image id to attach tag to, if < 0 selected images are used. \note If tag not exists it's created.*/
void dt_tag_attach_list(GList *tags, gint imgid);

/** attach a list of tags on selected images. \param[in] tags a comma separated string of tags. \param[in]
 * imgid the image id to attach tag to, if < 0 selected images are used. \note If tag not exists it's
 * created.*/
void dt_tag_attach_string_list(const gchar *tags, gint imgid);

/** detach tag from images. \param[in] tagid if of tag to deattach. \param[in] imgid the image id to attach
 * tag from, if < 0 selected images are used. */
void dt_tag_detach(guint tagid, gint imgid);

/** detach tags from images that matches name, it is valid to use % to match tag */
void dt_tag_detach_by_string(const char *name, gint imgid);

/** retrieves a list of tags of specified imgid \param[out] result a list of dt_tag_t, sorted by tag. */
uint32_t dt_tag_get_attached(gint imgid, GList **result, gboolean ignore_dt_tags);

/** get a list of tags, call dt_util_glist_to_str() to make it into a string.
 *  the difference to dt_tag_get_attached() is that this one splits at '|' and filters out the "darktable|"
 * tags. */
GList *dt_tag_get_list(gint imgid);

/** get a flat list of only hierarchical tags, call dt_util_glist_to_str() to make it into a string.
 *  the difference to dt_tag_get_attached() is that this one filters out the "darktable|" tags. */
GList *dt_tag_get_hierarchical(gint imgid);

/** retrieves a list of suggested tags matching keyword. \param[in] keyword the keyword to search \param[out]
 * result a pointer to list populated with result. \return the count \note the limit of result is decided by
 * conf value "xxx" */
uint32_t dt_tag_get_suggestions(const gchar *keyword, GList **result);

/** retrieves a list of recent tags used. \param[out] result a pointer to list populated with result. \return
 * the count \note the limit of result is decided by conf value "xxx" */
uint32_t dt_tag_get_recent_used(GList **result);

/** frees the memory of a result set. */
void dt_tag_free_result(GList **result);

/** reorganize tags */
void dt_tag_reorganize(const gchar *source, const gchar *dest);


#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
