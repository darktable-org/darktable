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

#pragma once

#include <glib.h>
#include <inttypes.h>
#include <sqlite3.h>

struct dt_develop_t;
struct dt_iop_module_t;

/** helper function to free a GList of dt_history_item_t */
void dt_history_item_free(gpointer data);

/** adds to dev_dest module mod_src */
int dt_history_merge_module_into_history(struct dt_develop_t *dev_dest, struct dt_develop_t *dev_src, struct dt_iop_module_t *mod_src, GList **_modules_used, const int append);

/** copy history from imgid and pasts on dest_imgid, merge or overwrite... */
int dt_history_copy_and_paste_on_image(int32_t imgid, int32_t dest_imgid, gboolean merge, GList *ops);

/** delete all history for the given image */
void dt_history_delete_on_image(int32_t imgid);

/** as above but control whether to record undo/redo */
void dt_history_delete_on_image_ext(int32_t imgid, gboolean undo);

/** copy history from imgid and pasts on selected images, merge or overwrite... */
int dt_history_copy_and_paste_on_selection(int32_t imgid, gboolean merge, GList *ops);

/** load a dt file and applies to selected images */
int dt_history_load_and_apply_on_selection(gchar *filename);

/** load a dt file and applies to specified image */
int dt_history_load_and_apply(int imgid, gchar *filename, int history_only);

/** delete historystack of selected images */
void dt_history_delete_on_selection();

/** compress history stack */
int dt_history_compress_on_selection();
void dt_history_compress_on_image(int32_t imgid);
/* set or clear a tag representing an error state while compressing history */
void dt_history_set_compress_problem(int32_t imgid, gboolean set);
/* duplicate an history list */
GList *dt_history_duplicate(GList *hist);

typedef struct dt_history_item_t
{
  guint num;
  gchar *op;
  gchar *name;
} dt_history_item_t;

/** get list of history items for image */
GList *dt_history_get_items(int32_t imgid, gboolean enabled);

/** get list of history items for image as a nice string */
char *dt_history_get_items_as_string(int32_t imgid);

/* check if a module exists in the history of corresponding image */
gboolean dt_history_check_module_exists(int32_t imgid, const char *operation);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
