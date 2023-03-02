/*
    This file is part of darktable,
    Copyright (C) 2010-2022 darktable developers.

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

#include <gtk/gtk.h>
#include <inttypes.h>
#include <sqlite3.h>
#include "develop/imageop.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

struct dt_develop_t;
struct dt_iop_module_t;

// history hash is designed to detect any change made on the image
// if current = basic the image has only the mandatory modules with their original settings
// if current = auto the image has the mandatory and auto applied modules with their original settings
// else the image has been changed in some way
// note that if an image has no history (and no history hash) it is considered as basic
typedef enum dt_history_hash_t
{
  DT_HISTORY_HASH_BASIC   = 1 << 0,  // only mandatory modules
  DT_HISTORY_HASH_AUTO    = 1 << 1,  // mandatory modules plus the auto applied ones
  DT_HISTORY_HASH_CURRENT = 1 << 2,  // current state, with or without change
  DT_HISTORY_HASH_MIPMAP  = 1 << 3,  // last mipmap hash
} dt_history_hash_t;

typedef struct dt_history_hash_values_t
{
  guint8 *basic;
  int basic_len;
  guint8 *auto_apply;
  int auto_apply_len;
  guint8 *current;
  int current_len;
} dt_history_hash_values_t;

typedef struct dt_history_copy_item_t
{
  GList *selops;
  GtkTreeView *items;
  int copied_imageid;
  gboolean full_copy;
  gboolean copy_iop_order;
} dt_history_copy_item_t;

/** helper function to free a GList of dt_history_item_t */
void dt_history_item_free(gpointer data);

/** adds to dev_dest module mod_src */
gboolean dt_history_merge_module_into_history(struct dt_develop_t *dev_dest,
                                              struct dt_develop_t *dev_src,
                                              struct dt_iop_module_t *mod_src,
                                              GList **_modules_used,
                                              const gboolean append,
                                              const gboolean auto_init);

/** copy history from imgid and pasts on dest_imgid, merge or overwrite... */
gboolean dt_history_copy_and_paste_on_image(const int32_t imgid,
                                            const int32_t dest_imgid,
                                            const gboolean merge,
                                            GList *ops,
                                            const gboolean copy_iop_order,
                                            const gboolean copy_full);

/** delete all history for the given image */
void dt_history_delete_on_image(const int32_t imgid);

/** as above but control whether to record undo/redo */
void dt_history_delete_on_image_ext(const int32_t imgid, const gboolean undo);

/** copy history from imgid and pasts on selected images, merge or overwrite... */
gboolean dt_history_copy(const int imgid);
gboolean dt_history_copy_parts(const int imgid);
gboolean dt_history_paste_on_list(const GList *list, gboolean undo);
gboolean dt_history_paste_parts_on_list(const GList *list, gboolean undo);

static inline gboolean dt_history_module_skip_copy(const int flags)
{
  return flags & (IOP_FLAGS_DEPRECATED | IOP_FLAGS_UNSAFE_COPY | IOP_FLAGS_HIDDEN);
}

/** load a dt file and applies to selected images */
gboolean dt_history_load_and_apply_on_list(gchar *filename, const GList *list);

/** load a dt file and applies to specified image */
gboolean dt_history_load_and_apply(const int imgid,
                                   gchar *filename,
                                   const gboolean history_only);

/** delete historystack of selected images */
gboolean dt_history_delete_on_list(const GList *list, const gboolean undo);

/** compress history stack */
int dt_history_compress_on_list(const GList *imgs);
void dt_history_compress_on_image(const int32_t imgid);

/** truncate history stack */
void dt_history_truncate_on_image(const int32_t imgid, const int32_t history_end);

/* duplicate an history list */
GList *dt_history_duplicate(GList *hist);



typedef struct dt_history_item_t
{
  guint num;
  gchar *op;
  gchar *name;
  gboolean enabled;
} dt_history_item_t;

/** get list of history items for image */
GList *dt_history_get_items(int32_t imgid, gboolean enabled);

/** get list of history items for image as a nice string */
char *dt_history_get_items_as_string(int32_t imgid);

/** get a single history item as string with enabled status */
char *dt_history_item_as_string(const char *name, gboolean enabled);

/* check if a module exists in the history of corresponding image */
gboolean dt_history_check_module_exists(int32_t imgid,
                                        const char *operation,
                                        const gboolean enabled);

/* check if a module exists in the history of corresponding image */
gboolean dt_history_check_module_exists_list(GList *hist,
                                             const char *operation,
                                             const gboolean enabled);

/** calculate history hash and save it to database*/
void dt_history_hash_write_from_history(const int32_t imgid,
                                        const dt_history_hash_t type);

/** return the hash history status */
dt_history_hash_t dt_history_hash_get_status(const int32_t imgid);

/** return true if mipmap_hash = current_hash */
gboolean dt_history_hash_is_mipmap_synced(const int32_t imgid);

/** update mipmap hash to db (= current_hash) */
void dt_history_hash_set_mipmap(const int32_t imgid);

/** write hash values to db */
void dt_history_hash_write(const int32_t imgid, dt_history_hash_values_t *hash);

/** read hash values from db */
void dt_history_hash_read(const int32_t imgid, dt_history_hash_values_t *hash);

#ifdef __cplusplus
} // extern "C"
#endif /* __cplusplus */

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
