/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

#include "develop/masks/scratch_image.h"

#include "common/darktable.h"
#include "common/debug.h"
#include "common/iop_order.h"
#include "develop/imageop.h"
#include "develop/masks.h"

#include <sqlite3.h>
#include <string.h>

void dt_masks_scratch_seed_image(const dt_imgid_t imgid,
                                 const int width,
                                 const int height)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
                        "INSERT OR IGNORE INTO main.film_rolls (id, folder)"
                        " VALUES (1, 'scratch')", NULL, NULL, NULL);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT OR REPLACE INTO main.images"
                              " (id, group_id, film_id, width, height, filename,"
                              "  version, max_version, history_end, flags)"
                              " VALUES (?1, ?1, 1, ?2, ?3, 'scratch.raw', 0, 0, 1, 0)",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, width);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, height);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_masks_scratch_open(dt_develop_t *dev, const dt_imgid_t imgid)
{
  dt_dev_init(dev, FALSE);
  // dt_dev_init leaves dev->iop NULL and dt_dev_read_history_ext refuses to do
  // anything without it
  dev->iop = dt_iop_load_modules(dev);

  // id is what migration's content probe binds; the dimensions come along
  // because the same struct is what anything else asking "how big is this
  // image" would read, and leaving them zero invites a divide-by-zero
  // somewhere far from here
  dev->image_storage.id = imgid;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT width, height FROM main.images WHERE id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dev->image_storage.width = sqlite3_column_int(stmt, 0);
    dev->image_storage.height = sqlite3_column_int(stmt, 1);
  }
  sqlite3_finalize(stmt);

  dt_dev_read_history_ext(dev, imgid, TRUE);
}

// copy out the mask of the last history item; FALSE if it has none
static gboolean _copy_last_mask(const dt_develop_t *dev,
                                dt_develop_blend_params_t *bp_out,
                                GList **forms_out)
{
  const GList *last = g_list_last(dev->history);
  const dt_dev_history_item_t *h = last ? last->data : NULL;
  if(!h || !h->blend_params) return FALSE;
  if(bp_out) memcpy(bp_out, h->blend_params, sizeof(dt_develop_blend_params_t));
  if(forms_out) *forms_out = dt_masks_dup_forms_deep(dev->forms, NULL);
  return TRUE;
}

gboolean dt_masks_scratch_read_last(const dt_imgid_t imgid,
                                    dt_develop_blend_params_t *bp_out,
                                    GList **forms_out)
{
  dt_develop_t dev;
  dt_masks_scratch_open(&dev, imgid);
  const gboolean ok = _copy_last_mask(&dev, bp_out, forms_out);
  dt_dev_cleanup(&dev);
  return ok;
}

gboolean dt_masks_scratch_reset_to_migrated(const dt_imgid_t imgid,
                                            const dt_masks_harvest_edit_t *edit,
                                            dt_develop_blend_params_t *bp_out,
                                            GList **forms_out)
{
  if(!edit->operation) return FALSE;
  dt_masks_scratch_wipe_history(imgid);
  dt_masks_scratch_seed_image(imgid, edit->width, edit->height);
  // the iop-order entry must exist before the history row referencing it, or
  // dt_dev_read_history_ext() drops the row without a word
  dt_masks_scratch_seed_iop_order(imgid, edit->operation, edit->multi_priority);
  if(!dt_masks_scratch_seed_history(imgid, 0, edit->operation, edit->multi_priority,
                                    edit->blendop_version, &edit->bp, edit->forms))
    return FALSE;

  // deliberately a plain open with no edit of our own: the baseline has to be
  // what a user gets by opening the image and nothing more
  dt_develop_t dev;
  dt_masks_scratch_open(&dev, imgid);
  const gboolean ok = (bp_out || forms_out) ? _copy_last_mask(&dev, bp_out, forms_out)
                                            : dev.history != NULL;
  dt_dev_cleanup(&dev);
  return ok;
}

void dt_masks_scratch_wipe_history(const dt_imgid_t imgid)
{
  const char *const stmts[] = {
    "DELETE FROM main.history WHERE imgid = ?1",
    "DELETE FROM main.masks_history WHERE imgid = ?1",
    "DELETE FROM main.module_order WHERE imgid = ?1",
  };
  for(size_t i = 0; i < sizeof(stmts) / sizeof(stmts[0]); i++)
  {
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), stmts[i], -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
}

void dt_masks_scratch_seed_iop_order(const dt_imgid_t imgid,
                                     const char *operation,
                                     const int multi_priority)
{
  // the default list already carries instance 0 for every module
  if(multi_priority <= 0) return;

  GList *list = dt_ioppr_get_iop_order_list(imgid, FALSE);
  if(!list) return;

  dt_iop_order_entry_t *entry = calloc(1, sizeof(dt_iop_order_entry_t));
  if(!entry)
  {
    dt_ioppr_iop_order_list_free(list);
    return;
  }
  dt_strlcpy_to_fixed(entry->operation, operation, sizeof(entry->operation));
  entry->instance = multi_priority;
  entry->name[0] = '\0';
  entry->o.iop_order = 0;

  // same placement rule as dt_ioppr_insert_module_instance()
  GList *place = NULL;
  int max_instance = -1;
  for(GList *l = list; l; l = g_list_next(l))
  {
    const dt_iop_order_entry_t *const e = l->data;
    if(!strcmp(e->operation, operation) && e->instance > max_instance)
    {
      place = l;
      max_instance = e->instance;
    }
  }
  if(!place)
  {
    // the module is not in the default order at all -- nothing to hang the
    // instance off, and writing it anywhere would be inventing an order
    free(entry);
    dt_ioppr_iop_order_list_free(list);
    return;
  }
  list = g_list_insert_before(list, place, entry);

  // renumber. The only contract on these values is "starts above 0 and
  // increases" (see _ioppr_reset_iop_order in iop_order.c); the gaps leave
  // room for anything inserted later.
  int order = 100;
  for(GList *l = list; l; l = g_list_next(l))
  {
    dt_iop_order_entry_t *e = l->data;
    e->o.iop_order = order;
    order += 100;
  }

  dt_ioppr_write_iop_order_list(list, imgid);
  dt_ioppr_iop_order_list_free(list);
}

gboolean dt_masks_scratch_seed_history(const dt_imgid_t imgid,
                                       const int num,
                                       const char *operation,
                                       const int multi_priority,
                                       const int blendop_version,
                                       const dt_develop_blend_params_t *bp,
                                       GList *forms)
{
  dt_iop_module_so_t *so = NULL;
  for(GList *l = darktable.iop; l; l = g_list_next(l))
  {
    dt_iop_module_so_t *cand = l->data;
    if(cand && !strcmp(cand->op, operation)) { so = cand; break; }
  }
  if(!so) return FALSE;

  // a throwaway instance, only to borrow default_params/params_size/version()
  dt_iop_module_t module;
  dt_develop_t scratch;
  memset(&scratch, 0, sizeof(scratch));
  // note the sense: returns TRUE on failure
  if(dt_iop_load_module_by_so(&module, so, &scratch)) return FALSE;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO main.history"
                              " (imgid, num, module, operation, op_params, enabled,"
                              "  blendop_params, blendop_version, multi_priority,"
                              "  multi_name, multi_name_hand_edited)"
                              " VALUES (?1, ?8, ?2, ?3, ?4, 1, ?5, ?6, ?7, '', 0)",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, module.version());
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 4, module.default_params, module.params_size,
                             SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 5, bp, sizeof(dt_develop_blend_params_t),
                             SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 6, blendop_version);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, multi_priority);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 8, num);
  const gboolean ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);

  dt_iop_cleanup_module(&module);
  if(!ok) return FALSE;

  // the forms, under the same num -- the row dt_masks_read_masks_history()
  // treats as current for this module
  for(GList *l = forms; l; l = g_list_next(l))
    dt_masks_write_masks_history_item(imgid, num, l->data);

  return TRUE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
