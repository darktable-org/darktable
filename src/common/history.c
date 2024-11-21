/*
    This file is part of darktable,
    Copyright (C) 2010-2024 darktable developers.

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

#include "common/history.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/exif.h"
#include "common/history_snapshot.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "common/overlay.h"
#include "common/tags.h"
#include "common/undo.h"
#include "common/utility.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/masks.h"
#include "gui/hist_dialog.h"
#include "imageio/imageio_common.h"

void dt_history_item_free(gpointer data)
{
  dt_history_item_t *item = (dt_history_item_t *)data;
  g_free(item->op);
  g_free(item->name);
  item->op = NULL;
  item->name = NULL;
  g_free(item);
}

static void _remove_preset_flag(const dt_imgid_t imgid)
{
  dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'w');

  // clear flag
  if(image)
    image->flags &= ~DT_IMAGE_AUTO_PRESETS_APPLIED;

  // write through to sql+xmp
  dt_image_cache_write_release_info(darktable.image_cache, image,
                                    DT_IMAGE_CACHE_SAFE,
                                    "_remove_preset_flag");
}

void dt_history_delete_on_image_ext(const dt_imgid_t imgid,
                                    const gboolean undo,
                                    const gboolean init_history)
{
  dt_undo_lt_history_t *hist = undo?dt_history_snapshot_item_init():NULL;

  if(undo)
  {
    hist->imgid = imgid;
    dt_history_snapshot_undo_create(hist->imgid, &hist->before, &hist->before_history_end);
  }

  dt_lock_image(imgid);

  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM main.history WHERE imgid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM main.module_order WHERE imgid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE main.images"
                              " SET history_end = 0, aspect_ratio = 0.0, thumb_timestamp = -1, thumb_maxmip = 0"
                              " WHERE id = ?1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM main.masks_history WHERE imgid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM main.history_hash WHERE imgid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // remove all overlays for this image
  dt_overlays_remove(imgid);

  // we do not init history for the darkroom snapshots as the whole history
  // (module, masks, module order) will be replaced later.
  if(init_history)
  {
    _remove_preset_flag(imgid);

    /* if current image in develop reload history */
    if(dt_dev_is_current_image(darktable.develop, imgid))
      dt_dev_reload_history_items(darktable.develop);
  }

  /* make sure mipmaps are recomputed */
  dt_mipmap_cache_remove(darktable.mipmap_cache, imgid);
  dt_image_update_final_size(imgid);

  /* remove darktable|style|* tags */
  dt_tag_detach_by_string("darktable|style|%", imgid, FALSE, FALSE);
  dt_tag_detach_by_string("darktable|changed", imgid, FALSE, FALSE);
  /* signal that the tags may have changed */
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_TAG_CHANGED);

  /* unset change timestamp */
  dt_image_cache_unset_change_timestamp(darktable.image_cache, imgid);

  // signal that the mipmap need to be updated
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_DEVELOP_MIPMAP_UPDATED, imgid);

  dt_unlock_image(imgid);

  // update history hash
  dt_history_hash_write_from_history(imgid, DT_HISTORY_HASH_CURRENT);

  if(undo)
  {
    dt_history_snapshot_undo_create(hist->imgid, &hist->after, &hist->after_history_end);

    dt_undo_start_group(darktable.undo, DT_UNDO_LT_HISTORY);
    dt_undo_record(darktable.undo, NULL, DT_UNDO_LT_HISTORY,
                   (dt_undo_data_t)hist,
                   dt_history_snapshot_undo_pop,
                   dt_history_snapshot_undo_lt_history_data_free);
    dt_undo_end_group(darktable.undo);
  }
}

void dt_history_delete_on_image(const dt_imgid_t imgid)
{
  dt_history_delete_on_image_ext(imgid, TRUE, TRUE);
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_TAG_CHANGED);
}

gboolean dt_history_load_and_apply(const dt_imgid_t imgid,
                                   gchar *filename,
                                   const gboolean history_only)
{
  dt_lock_image(imgid);
  dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'w');
  if(img)
  {
    dt_undo_lt_history_t *hist = dt_history_snapshot_item_init();
    hist->imgid = imgid;
    dt_history_snapshot_undo_create(hist->imgid, &hist->before, &hist->before_history_end);

    if(dt_exif_xmp_read(img, filename, history_only))
    {
      dt_image_cache_write_release_info
        (darktable.image_cache, img,
         // ugly but if not history_only => called from crawler - do not write the xmp
         history_only ? DT_IMAGE_CACHE_SAFE : DT_IMAGE_CACHE_RELAXED,
         "dt_history_load_and_apply");
      dt_unlock_image(imgid);
      return TRUE;
    }
    dt_history_snapshot_undo_create(hist->imgid, &hist->after, &hist->after_history_end);
    dt_undo_start_group(darktable.undo, DT_UNDO_LT_HISTORY);
    dt_undo_record(darktable.undo, NULL, DT_UNDO_LT_HISTORY, (dt_undo_data_t)hist,
                   dt_history_snapshot_undo_pop,
                   dt_history_snapshot_undo_lt_history_data_free);
    dt_undo_end_group(darktable.undo);

    /* if current image in develop reload history */
    if(dt_dev_is_current_image(darktable.develop, imgid))
      dt_dev_reload_history_items(darktable.develop);

    dt_image_cache_write_release_info
      (darktable.image_cache, img,
       // ugly but if not history_only => called from crawler - do not write the xmp
       history_only ? DT_IMAGE_CACHE_SAFE : DT_IMAGE_CACHE_RELAXED,
       "dt_history_load_and_apply");
    dt_mipmap_cache_remove(darktable.mipmap_cache, imgid);
    dt_image_update_final_size(imgid);
  }
  dt_unlock_image(imgid);
  // signal that the mipmap need to be updated
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_DEVELOP_MIPMAP_UPDATED, imgid);
  return FALSE;
}

gboolean dt_history_load_and_apply_on_list(gchar *filename,
                                           const GList *list)
{
  gboolean res = FALSE;
  dt_undo_start_group(darktable.undo, DT_UNDO_LT_HISTORY);
  for(GList *l = (GList *)list; l; l = g_list_next(l))
  {
    const dt_imgid_t imgid = GPOINTER_TO_INT(l->data);
    if(dt_history_load_and_apply(imgid, filename, TRUE))
      res = TRUE;
  }
  dt_undo_end_group(darktable.undo);
  return res;
}

// returns the first history item with hist->module == module
static dt_dev_history_item_t *_search_history_by_module(dt_develop_t *dev,
                                                        const dt_iop_module_t *module)
{
  dt_dev_history_item_t *hist_mod = NULL;
  for(GList *history = dev->history; history; history = g_list_next(history))
  {
    dt_dev_history_item_t *hist = history->data;

    if(hist->module == module)
    {
      hist_mod = hist;
      break;
    }
  }
  return hist_mod;
}

// returns the module on modules_list that is equal to module
// used to check if module exists on the list
static dt_iop_module_t *_search_list_iop_by_module(GList *modules_list,
                                                   const dt_iop_module_t *module)
{
  dt_iop_module_t *mod_ret = NULL;
  for(GList *modules = modules_list; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *mod = modules->data;

    if(mod == module)
    {
      mod_ret = mod;
      break;
    }
  }
  return mod_ret;
}

// fills used with formid, if it is a group it recurs and fill all sub-forms
static void _fill_used_forms(GList *forms_list,
                             const dt_mask_id_t formid,
                             int *used,
                             const int nb)
{
  // first, we search for the formid in used table
  for(int i = 0; i < nb; i++)
  {
    if(used[i] == NO_MASKID)
    {
      // we store the formid
      used[i] = formid;
      break;
    }
    if(used[i] == formid) break;
  }

  // if the form is a group, we iterate through the sub-forms
  dt_masks_form_t *form = dt_masks_get_from_id_ext(forms_list, formid);
  if(form && (form->type & DT_MASKS_GROUP))
  {
    for(GList *grpts = form->points; grpts; grpts = g_list_next(grpts))
    {
      dt_masks_point_group_t *grpt = grpts->data;
      _fill_used_forms(forms_list, grpt->formid, used, nb);
    }
  }
}

// dev_src is used only to copy masks, if no mask will be copied it can be null
gboolean dt_history_merge_module_into_history(dt_develop_t *dev_dest,
                                              dt_develop_t *dev_src,
                                              dt_iop_module_t *mod_src,
                                              GList **_modules_used,
                                              const gboolean append,
                                              const gboolean auto_init)
{
  gboolean module_added = TRUE;
  GList *modules_used = *_modules_used;
  dt_iop_module_t *module = NULL;
  dt_iop_module_t *mod_replace = NULL;

  char modsrc_multi_name[128] = { 0 };

  // use multi-name for match copied instance only for hand-edited names
  // for the multi-priority = 0 (first instance).

  g_strlcpy(modsrc_multi_name,
            dt_iop_get_instance_name(mod_src), sizeof(modsrc_multi_name));

  // one-instance modules always replace the existing one
  if(mod_src->flags() & IOP_FLAGS_ONE_INSTANCE)
  {
    mod_replace = dt_iop_get_module_by_op_priority(dev_dest->iop, mod_src->op, -1);
    if(mod_replace == NULL)
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[dt_history_merge_module_into_history]"
               " can't find single instance module %s",
               mod_src->op);
      module_added = FALSE;
    }
  }

  if(module_added
     && mod_replace == NULL
     && (!append || !mod_src->enabled))
  {
    dt_iop_module_t *mod_same_name = NULL;
    dt_iop_module_t *mod_same_prio = NULL;
    dt_iop_module_t *mod_unused = NULL;
    dt_iop_module_t *mod_default = NULL;

    // we haven't found a module to replace check if there's a module
    // with the same (operation, multi_name) on dev->iop

    for(GList *modules_dest = dev_dest->iop;
        modules_dest;
        modules_dest = g_list_next(modules_dest))
    {
      dt_iop_module_t *mod_dest = modules_dest->data;

      // look for not yet used module
      if(strcmp(mod_src->op, mod_dest->op) == 0
         && _search_list_iop_by_module(modules_used, mod_dest) == NULL)
      {
        if(strcmp(mod_src->multi_name, mod_dest->multi_name) == 0)
        {
          mod_same_name = mod_dest;
        }
        else if(!mod_dest->multi_name_hand_edited
                && mod_src->multi_priority == mod_dest->multi_priority)
        {
          mod_same_prio = mod_dest;
        }
        else if(!mod_dest->enabled)
        {
          mod_unused = mod_dest;
        }
        else if(memcmp(mod_dest->params,
                       mod_dest->default_params,
                       mod_dest->params_size) == 0
                && memcmp(mod_dest->blend_params,
                          mod_dest->default_blendop_params,
                          sizeof(dt_develop_blend_params_t)) == 0)
        {
          mod_default = mod_dest;
        }
      }
    }

    // now select the destination module by order of preference:
    // - same name
    // - not enabled
    // - enabled but has default params
    // - same prio
    dt_iop_module_t *mod_dest =
      mod_same_name ? mod_same_name
      : (mod_unused ? mod_unused
         : (mod_default ? mod_default
            : (mod_same_prio ? mod_same_prio
               : NULL)));

    if(mod_dest)
    {
      // we will replace this module
      modules_used = g_list_append(modules_used, mod_dest);
      mod_replace = mod_dest;
    }
  }

  if(module_added)
  {
    // if we are creating a new instance, create a new module
    if(mod_replace == NULL)
    {
      dt_iop_module_t *base =
        dt_iop_get_module_by_op_priority(dev_dest->iop, mod_src->op, -1);

      module = dt_dev_module_duplicate_ext(dev_dest, base, FALSE);

      if(!module)
      {
        dt_print(DT_DEBUG_ALWAYS,
                 "[dt_history_merge_module_into_history]"
                 " can't load module %s", mod_src->op);
        module_added = FALSE;
      }
      else
      {
        dt_ioppr_resync_modules_order(dev_dest);

        // and record this module as we don't want to reuse it later
        modules_used = g_list_append(modules_used, module);
      }
    }
    else
    {
      module = mod_replace;
    }

    module->enabled = mod_src->enabled;

    if(!module->multi_name_hand_edited)
    {
      g_strlcpy(module->multi_name, mod_src->multi_name, sizeof(module->multi_name));
      module->multi_name_hand_edited = mod_src->multi_name_hand_edited;
    }

    if(auto_init)
    {
      module->params = NULL;
      module->params_size = 0;
    }
    else
      memcpy(module->params, mod_src->params, module->params_size);

    if(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
    {
      memcpy(module->blend_params, mod_src->blend_params,
             sizeof(dt_develop_blend_params_t));
      module->blend_params->mask_id = mod_src->blend_params->mask_id;
    }
  }

  // we have the module, we will use the source module iop_order
  // unless there's already a module with that order
  if(module_added && mod_replace == NULL)
  {
    dt_iop_module_t *module_duplicate = NULL;
    // check if there's a module with the same iop_order
    for(GList *modules_dest = dev_dest->iop;
        modules_dest;
        modules_dest = g_list_next(modules_dest))
    {
      dt_iop_module_t *mod = modules_dest->data;

      if(module_duplicate != NULL)
      {
        module_duplicate = mod;
        break;
      }
      if(mod->iop_order == mod_src->iop_order && mod != module)
      {
        module_duplicate = mod;
      }
    }

    // do some checking...
    if(mod_src->iop_order <= 0 || mod_src->iop_order == INT_MAX)
      dt_print(DT_DEBUG_ALWAYS,
               "[dt_history_merge_module_into_history]"
               " invalid source module %s %s(%d)(%i)",
               mod_src->op, mod_src->multi_name,
               mod_src->iop_order, mod_src->multi_priority);

    if(module_duplicate
       && (module_duplicate->iop_order <= 0
           || module_duplicate->iop_order == INT_MAX))
      dt_print(DT_DEBUG_ALWAYS,
               "[dt_history_merge_module_into_history]"
               " invalid duplicate module module %s %s(%d)(%i)",
               module_duplicate->op, module_duplicate->multi_name,
               module_duplicate->iop_order, module_duplicate->multi_priority);

    if(module->iop_order <= 0 || module->iop_order == INT_MAX)
      dt_print(DT_DEBUG_ALWAYS,
               "[dt_history_merge_module_into_history]"
               " invalid iop_order for module %s %s(%d)(%i)",
               module->op, module->multi_name, module->iop_order,
               module->multi_priority);

    if(mod_replace != NULL)
      dev_dest->iop = g_list_sort(dev_dest->iop, dt_sort_iop_by_order);
  }

  // and we add it to history
  if(module_added)
  {
    // copy masks
    guint nbf = 0;
    int *forms_used_replace = NULL;

    if(dev_src)
    {
      // we will copy only used forms
      // record the masks used by this module
      if(mod_src->flags() & IOP_FLAGS_SUPPORTS_BLENDING
         && dt_is_valid_maskid(mod_src->blend_params->mask_id))
      {
        nbf = g_list_length(dev_src->forms);
        forms_used_replace = calloc(nbf, sizeof(int));
        if(forms_used_replace)
          _fill_used_forms(dev_src->forms,
                           mod_src->blend_params->mask_id,
                           forms_used_replace, nbf);

        // now copy masks
        for(int i = 0; i < nbf && forms_used_replace && forms_used_replace[i] > 0; i++)
        {
          dt_masks_form_t *form =
            dt_masks_get_from_id_ext(dev_src->forms, forms_used_replace[i]);
          if(form)
          {
            // check if the form already exists in dest image
            // if so we'll remove it, so it is replaced
            dt_masks_form_t *form_dest =
              dt_masks_get_from_id_ext(dev_dest->forms, forms_used_replace[i]);
            if(form_dest)
            {
              dev_dest->forms = g_list_remove(dev_dest->forms, form_dest);
              // and add it to allforms to cleanup
              dev_dest->allforms = g_list_append(dev_dest->allforms, form_dest);
            }

            // and add it to dest image
            dt_masks_form_t *form_new = dt_masks_dup_masks_form(form);
            dev_dest->forms = g_list_append(dev_dest->forms, form_new);
          }
          else
            dt_print(DT_DEBUG_ALWAYS,
                     "[dt_history_merge_module_into_history]"
                     " form %i not found in source image",
                     forms_used_replace[i]);
        }
      }
    }

    if(nbf > 0 && forms_used_replace[0] > 0)
      dt_dev_add_masks_history_item_ext(dev_dest, module, FALSE, TRUE);
    else
      dt_dev_add_history_item_ext(dev_dest, module, FALSE, TRUE);

    if(mod_replace == NULL)
    {
      dt_ioppr_resync_modules_order(dev_dest);
    }

    dt_dev_pop_history_items_ext(dev_dest, dev_dest->history_end);

    if(forms_used_replace) free(forms_used_replace);
  }

  *_modules_used = modules_used;

  return module_added;
}

static gboolean _history_copy_and_paste_on_image_merge(const dt_imgid_t imgid,
                                                       const dt_imgid_t dest_imgid,
                                                       GList *ops,
                                                       const gboolean copy_iop_order,
                                                       const gboolean copy_full)
{
  GList *modules_used = NULL;

  dt_develop_t _dev_src = { 0 };
  dt_develop_t _dev_dest = { 0 };

  dt_develop_t *dev_src = &_dev_src;
  dt_develop_t *dev_dest = &_dev_dest;

  // we will do the copy/paste on memory so we can deal with masks
  dt_dev_init(dev_src, FALSE);
  dt_dev_init(dev_dest, FALSE);

  dev_src->iop = dt_iop_load_modules_ext(dev_src, TRUE);
  dev_dest->iop = dt_iop_load_modules_ext(dev_dest, TRUE);

  dt_dev_read_history_ext(dev_src, imgid, TRUE);

  // This prepends the default modules and converts just in case it's an empty history
  dt_dev_read_history_ext(dev_dest, dest_imgid, TRUE);

  dt_ioppr_check_iop_order(dev_src, imgid,
                           "_history_copy_and_paste_on_image_merge ");
  dt_ioppr_check_iop_order(dev_dest, dest_imgid,
                           "_history_copy_and_paste_on_image_merge ");

  dt_dev_pop_history_items_ext(dev_src, dev_src->history_end);
  dt_dev_pop_history_items_ext(dev_dest, dev_dest->history_end);

  dt_ioppr_check_iop_order(dev_src, imgid,
                           "_history_copy_and_paste_on_image_merge 1");
  dt_ioppr_check_iop_order(dev_dest, dest_imgid,
                           "_history_copy_and_paste_on_image_merge 1");

  GList *mod_list = NULL;
  GList *autoinit_list = NULL;

  if(ops)
  {
    dt_print(DT_DEBUG_IOPORDER,
             "[history_copy_and_paste_on_image_merge] selected modules");
    // copy only selected history entries
    for(const GList *l = g_list_last(ops); l; l = g_list_previous(l))
    {
      const int num = GPOINTER_TO_INT(l->data);
      const gboolean autoinit = (num < 0);

      const dt_dev_history_item_t *hist = g_list_nth_data(dev_src->history, abs(num));

      // check if we have a change for the same module & same instance in the
      // following of the list. If so, we want to skip this history as we will
      // apply another history for the same module but with possibly different
      // parameters.

      gboolean same_instance_following = FALSE;

      for(const GList *p = ops; p && p != l; p = g_list_next(p))
      {
        const int pnum = GPOINTER_TO_INT(p->data);
        const dt_dev_history_item_t *phist = g_list_nth_data(dev_src->history, abs(pnum));

        if(!phist || !phist->module)
          continue;
        if(!strcmp(hist->module->op, phist->module->op)
           && hist->module->multi_priority == phist->module->multi_priority)
        {
          same_instance_following = TRUE;
          break;
        }
      }

      if(hist && !same_instance_following)
      {
        if(!dt_iop_is_hidden(hist->module))
        {
          dt_print(DT_DEBUG_IOPORDER, "  module %20s, multiprio %i, num %d",
                   hist->module->op, hist->module->multi_priority, num);

          // make sure we use the proper params/blend as the module is set with the
          // last version of this module.
          memcpy(hist->module->params, hist->params,
                 hist->module->params_size);
          memcpy(hist->module->blend_params, hist->blend_params,
                 sizeof(dt_develop_blend_params_t));

          mod_list = g_list_prepend(mod_list, hist->module);
          autoinit_list = g_list_prepend(autoinit_list, GINT_TO_POINTER(autoinit));
        }
      }
    }
  }
  else
  {
    dt_print(DT_DEBUG_IOPORDER, "[history_copy_and_paste_on_image_merge] all modules");
    // we will copy all modules
    for(GList *modules_src = dev_src->iop;
        modules_src;
        modules_src = g_list_next(modules_src))
    {
      dt_iop_module_t *mod_src = modules_src->data;

      // copy from history only if
      if((_search_history_by_module(dev_src, mod_src) != NULL)
         // module is in history of source image
         && !(mod_src->default_enabled && mod_src->enabled
              && !memcmp(mod_src->params, mod_src->default_params, mod_src->params_size)
              // it's not a enabled by default module with unmodified settings
              && !dt_iop_is_hidden(mod_src))
         && (copy_full || !dt_history_module_skip_copy(mod_src->flags()))
        )
      {
        const gboolean autoinit = FALSE;

        mod_list = g_list_prepend(mod_list, mod_src);
        autoinit_list = g_list_prepend(autoinit_list, GINT_TO_POINTER(autoinit));
      }
    }
  }

  // list were built in reverse order, so un-reverse it
  mod_list = g_list_reverse(mod_list);
  autoinit_list = g_list_reverse(autoinit_list);

  // update iop-order list to have entries for the new modules
  if(!copy_iop_order)
    dt_ioppr_update_for_modules(dev_dest, mod_list, FALSE);

  GList *ai = autoinit_list;

  for(GList *l = mod_list; l; l = g_list_next(l))
  {
    dt_iop_module_t *mod = l->data;
    const gboolean autoinit = GPOINTER_TO_INT(ai->data);

    dt_history_merge_module_into_history
      (dev_dest, dev_src, mod, &modules_used, FALSE, autoinit);
    ai = g_list_next(ai);
  }

  // update iop-order list to have entries for the new modules
  if(!copy_iop_order)
    dt_ioppr_update_for_modules(dev_dest, mod_list, FALSE);

  dt_ioppr_check_iop_order(dev_dest, dest_imgid,
                           "_history_copy_and_paste_on_image_merge 2");

  // write history and forms to db
  dt_dev_write_history_ext(dev_dest, dest_imgid);

  dt_dev_cleanup(dev_src);
  dt_dev_cleanup(dev_dest);

  g_list_free(mod_list);
  g_list_free(modules_used);
  g_list_free(autoinit_list);

  return FALSE;
}

static gboolean _history_copy_and_paste_on_image_overwrite(const dt_imgid_t imgid,
                                                           const dt_imgid_t dest_imgid,
                                                           GList *ops,
                                                           const gboolean copy_iop_order,
                                                           const gboolean copy_full)
{
  gboolean ret_val = FALSE;
  sqlite3_stmt *stmt;

  // replace history stack
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM main.history WHERE imgid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // and shapes
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM main.masks_history WHERE imgid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE main.images"
                              " SET history_end = 0, aspect_ratio = 0.0"
                              " WHERE id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // the user wants an exact duplicate of the history, so just copy the db
  if(!ops)
  {
    // let's build the list of IOP to not copy
    gchar *skip_modules = NULL;

    if(!copy_full)
    {
      for(GList *modules = darktable.iop; modules; modules = g_list_next(modules))
      {
        dt_iop_module_so_t *module = modules->data;

        if(dt_history_module_skip_copy(module->flags()))
        {
          if(skip_modules)
            dt_util_str_cat(&skip_modules, ",");

          dt_util_str_cat(&skip_modules, "'%s'", module->op);
        }
      }
    }

    if(!skip_modules)
      skip_modules = g_strdup("'@'");

    // clang-format off
    gchar *query = g_strdup_printf
      ("INSERT INTO main.history "
       "            (imgid, num, module, operation, op_params, enabled, blendop_params,"
       "             blendop_version, multi_priority, multi_name, multi_name_hand_edited)"
       " SELECT ?1, num, module, operation, op_params, enabled, blendop_params,"
       "        blendop_version, multi_priority, multi_name, multi_name_hand_edited"
       " FROM main.history"
       " WHERE imgid=?2"
       "       AND operation NOT IN (%s)"
       " ORDER BY num", skip_modules);
    // clang-format on

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    g_free(query);

    // clang-format off
    query = g_strdup_printf
      ("INSERT INTO main.masks_history "
       "           (imgid, num, formid, form, name, version, points, points_count, source)"
       " SELECT ?1, num, formid, form, name, version, points, points_count, source "
       "  FROM main.masks_history"
       "  WHERE imgid = ?2"
       "    AND num NOT IN (SELECT num FROM history WHERE imgid=?2 AND OPERATION IN (%s))",
       skip_modules);
    // clang-format on

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    g_free(skip_modules);

    int history_end = 0;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT history_end FROM main.images WHERE id = ?1",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      if(sqlite3_column_type(stmt, 0) != SQLITE_NULL)
        history_end = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);

    dt_image_set_history_end(dest_imgid, history_end);

    // copy the module order

    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2
      (dt_database_get(darktable.db),
       "INSERT OR REPLACE INTO main.module_order (imgid, iop_list, version)"
       " SELECT ?2, iop_list, version"
       "   FROM main.module_order"
       "   WHERE imgid = ?1",
       -1, &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, dest_imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // it is possible the source image has no hash yet. make sure this is copied too

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "DELETE FROM main.history_hash WHERE imgid = ?1",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // and finally copy the history hash, except mipmap hash

    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "INSERT INTO main.history_hash"
                                "    (imgid, basic_hash, auto_hash, current_hash)"
                                " SELECT ?2, basic_hash, auto_hash, current_hash"
                                "   FROM main.history_hash "
                                "   WHERE imgid = ?1",
                                -1, &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, dest_imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  else
  {
    // since the history and masks where deleted we can do a merge
    ret_val = _history_copy_and_paste_on_image_merge
      (imgid, dest_imgid, ops, copy_iop_order, copy_full);
  }

  return ret_val;
}

gboolean dt_history_copy_and_paste_on_image(const dt_imgid_t imgid,
                                            const dt_imgid_t dest_imgid,
                                            const gboolean merge,
                                            GList *ops,
                                            const gboolean copy_iop_order,
                                            const gboolean copy_full,
                                            const gboolean sync)
{
  if(imgid == dest_imgid) return FALSE; // not pasted

  if(!dt_is_valid_imgid(imgid))
  {
    dt_control_log(_("you need to copy history from an"
                     " image before you paste it onto another"));
    return FALSE; // not pasted
  }

  dt_lock_image_pair(imgid, dest_imgid);

  // be sure the current history is written before pasting some other history data
  if(dt_view_get_current() == DT_VIEW_DARKROOM)
    dt_dev_write_history(darktable.develop);

  dt_undo_lt_history_t *hist = dt_history_snapshot_item_init();
  hist->imgid = dest_imgid;
  dt_history_snapshot_undo_create(hist->imgid, &hist->before, &hist->before_history_end);

  GList *iop_list = NULL;

  if(copy_iop_order)
  {
    iop_list = dt_ioppr_get_iop_order_list(imgid, FALSE);

    // but we also want to keep the multi-instance on the destination if merge is active
    if(merge)
    {
      GList *dest_iop_list = dt_ioppr_get_iop_order_list(dest_imgid, FALSE);
      GList *mi_iop_list = dt_ioppr_extract_multi_instances_list(dest_iop_list);

      if(mi_iop_list)
        dt_ioppr_merge_multi_instance_iop_order_list(iop_list, mi_iop_list);

      g_list_free_full(dest_iop_list, g_free);
      g_list_free_full(mi_iop_list, g_free);
    }
    dt_ioppr_write_iop_order_list(iop_list, dest_imgid);
  }

  gboolean ret_val = FALSE;
  if(merge)
    ret_val = _history_copy_and_paste_on_image_merge
      (imgid, dest_imgid, ops, copy_iop_order, copy_full);
  else
    ret_val = _history_copy_and_paste_on_image_overwrite
      (imgid, dest_imgid, ops, copy_iop_order, copy_full);

  if(iop_list)
  {
    dt_ioppr_write_iop_order_list(iop_list, dest_imgid);
    g_list_free_full(iop_list, g_free);
  }

  dt_history_snapshot_undo_create(hist->imgid, &hist->after, &hist->after_history_end);
  dt_undo_start_group(darktable.undo, DT_UNDO_LT_HISTORY);
  dt_undo_record(darktable.undo, NULL, DT_UNDO_LT_HISTORY, (dt_undo_data_t)hist,
                 dt_history_snapshot_undo_pop,
                 dt_history_snapshot_undo_lt_history_data_free);
  dt_undo_end_group(darktable.undo);

  /* add possibly new overlay module reference */
  dt_overlay_add_from_history(dest_imgid);

  /* attach changed tag reflecting actual change */
  guint tagid = 0;
  dt_tag_new("darktable|changed", &tagid);
  dt_tag_attach(tagid, dest_imgid, FALSE, FALSE);
  /* set change_timestamp */
  dt_image_cache_set_change_timestamp(darktable.image_cache, dest_imgid);

  /* if current image in develop reload history */
  if(dt_dev_is_current_image(darktable.develop, dest_imgid))
  {
    dt_dev_reload_history_items(darktable.develop);
    dt_dev_modulegroups_set(darktable.develop, dt_dev_modulegroups_get(darktable.develop));
  }

  dt_mipmap_cache_remove(darktable.mipmap_cache, dest_imgid);
  dt_image_update_final_size(imgid);

  /* update the aspect ratio. recompute only if really needed for
   * performance reasons */
  if(darktable.collection->params.sorts[DT_COLLECTION_SORT_ASPECT_RATIO])
    dt_image_set_aspect_ratio(dest_imgid, FALSE);
  else
    dt_image_reset_aspect_ratio(dest_imgid, FALSE);

  /* update xmp file */
  if(sync)
    dt_image_synch_xmp(dest_imgid);

  // signal that the mipmap need to be updated
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_DEVELOP_MIPMAP_UPDATED, dest_imgid);

  dt_unlock_image_pair(imgid, dest_imgid);

  return !ret_val;
}

char *dt_history_item_as_string(const char *name, const gboolean enabled)
{
  return g_strconcat(enabled ? "●" : "○", "  ", name, NULL);
}

char *dt_history_get_name_label(const char *name,
                                const char *label,
                                const gboolean markup)
{
  char *result = NULL;

  if(!label
     || strlen(label) == 0
     || strcmp(label, "0") == 0)
  {
    result = g_markup_escape_text(name, -1);
  }
  else
  {
    result = markup ? g_markup_printf_escaped("%s • <small>%s</small>", name, label)
                    : g_markup_printf_escaped("%s • %s", name, label);
  }

  return result;
}

GList *dt_history_get_items(const dt_imgid_t imgid,
                            const gboolean enabled,
                            const gboolean multi_priority_order,
                            const gboolean markup)
{
  GList *result = NULL;
  sqlite3_stmt *stmt;

  gchar *query = g_strdup_printf
    ("SELECT num, operation, enabled, multi_name, blendop_params"
     " FROM main.history"
     " WHERE imgid=?1"
     "   AND enabled in (1, ?2)"
     " GROUP BY num, operation, multi_priority"
     " ORDER BY %s DESC, %s DESC",
     multi_priority_order ? "multi_priority" : "num",
     multi_priority_order ? "num" : "multi_priority");

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              query,
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, enabled ? 1 : 0);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    if(strcmp((const char*)sqlite3_column_text(stmt, 1), "mask_manager") == 0)
      continue;

    dt_history_item_t *item = g_malloc(sizeof(dt_history_item_t));
    const char *op = (char *)sqlite3_column_text(stmt, 1);
    // first uint32_t of blend_params is the mode
    const uint32_t *blend_params = (uint32_t *)sqlite3_column_blob(stmt, 4);
    const int blend_params_len = sqlite3_column_bytes(stmt, 4);
    item->num = sqlite3_column_int(stmt, 0);
    item->enabled = sqlite3_column_int(stmt, 2);
    item->mask_mode = blend_params_len > 0 ? blend_params[0] : DEVELOP_MASK_DISABLED;

    const char *mname = (char *)sqlite3_column_text(stmt, 3);

    item->name = dt_history_get_name_label(dt_iop_get_localized_name(op), mname, markup);
    item->op = g_strdup(op);
    result = g_list_prepend(result, item);
  }
  sqlite3_finalize(stmt);
  g_free(query);

  return g_list_reverse(result);   // list was built in reverse order, so un-reverse it
}

char *dt_history_get_items_as_string(const dt_imgid_t imgid)
{
  GList *items = NULL;
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT operation, enabled, multi_name"
      " FROM main.history"
      " WHERE imgid=?1 ORDER BY num DESC", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  // collect all the entries in the history from the db
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    char *multi_name = NULL;
    const char *mn = (char *)sqlite3_column_text(stmt, 2);

    if(mn && *mn && g_strcmp0(mn, " ") != 0 && g_strcmp0(mn, "0") != 0)
      multi_name = g_strconcat(" ", sqlite3_column_text(stmt, 2), NULL);

    char *iname = dt_history_item_as_string
      (dt_iop_get_localized_name((char *)sqlite3_column_text(stmt, 0)),
       sqlite3_column_int(stmt, 1));

    char *name = g_strconcat(iname, multi_name ? multi_name : "", NULL);
    items = g_list_prepend(items, name);

    g_free(iname);
    g_free(multi_name);
  }
  sqlite3_finalize(stmt);
  items = g_list_reverse(items); // list was built in reverse order, so un-reverse it
  char *result = dt_util_glist_to_str("\n", items);
  g_list_free_full(items, g_free);
  return result;
}

static int dt_history_end_attop(const dt_imgid_t imgid)
{
  int size=0;
  int end=0;
  sqlite3_stmt *stmt;

  // get highest num in history
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
    "SELECT MAX(num) FROM main.history WHERE imgid=?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  if(sqlite3_step(stmt) == SQLITE_ROW)
    size = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  // get history_end
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
    "SELECT history_end FROM main.images WHERE id=?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
    end = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  // dt_print(DT_DEBUG_ALWAYS,"\ndt_history_end_attop"
  //          "for image %i: size %i, end %i",imgid,size,end);

  // a special case right after removing all history
  // It must be absolutely fresh and untouched so history_end is always on top
  if((size==0) && (end==0)) return -1;

  // return 1 if end is larger than size
  if(end > size) return 1;

  // no compression as history_end is right in the middle of stack
  return 0;
}


/* Please note: dt_history_compress_on_image
  - is used in lighttable and darkroom mode
  - It compresses history *exclusively* in the database and does
    *not* touch anything on the history stack
*/
void dt_history_compress_on_image(const dt_imgid_t imgid)
{
  dt_lock_image(imgid);
  sqlite3_stmt *stmt;

  // get history_end for image
  int my_history_end = 0;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
    "SELECT history_end"
    " FROM main.images"
    " WHERE id=?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  if(sqlite3_step(stmt) == SQLITE_ROW)
    my_history_end = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  if(my_history_end == 0)
  {
    dt_history_delete_on_image(imgid);
    dt_unlock_image(imgid);
    return;
  }

  int masks_count = 0;
  const char *op_mask_manager = "mask_manager";
  gboolean manager_position = FALSE;

  dt_database_start_transaction(darktable.db);

  // We must know for sure whether there is a mask manager at slot 0
  // in history because only if this is **not** true history nums and
  // history_end must be increased

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
    "SELECT COUNT(*)"
    " FROM main.history"
    " WHERE imgid = ?1 AND operation = ?2 AND num = 0", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, op_mask_manager, -1, SQLITE_TRANSIENT);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    if(sqlite3_column_int(stmt, 0) == 1)
      manager_position = TRUE;
  }
  sqlite3_finalize(stmt);

  // compress history, keep disabled modules as documented
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM main.history"
                              " WHERE imgid = ?1 AND num NOT IN"
                              "   (SELECT MAX(num) FROM main.history"
                              "     WHERE imgid = ?1 AND num < ?2"
                              "     GROUP BY operation, multi_priority)",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, my_history_end);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // delete all mask_manager entries
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
    "DELETE FROM main.history"
    " WHERE imgid = ?1 AND operation = ?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, op_mask_manager, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // compress masks history
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get(darktable.db),
     "DELETE FROM main.masks_history"
     " WHERE imgid = ?1 "
     "   AND num NOT IN (SELECT MAX(num)"
     "                   FROM main.masks_history"
     "                   WHERE imgid = ?1 AND num < ?2)", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, my_history_end);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // if there are masks create a mask manager entry, so we need to count them
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
    "SELECT COUNT(*) FROM main.masks_history WHERE imgid = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
    masks_count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  if(masks_count > 0)
  {
    // Set num in masks history to make sure they are owned by the
    // manager at slot 0.
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
      "UPDATE main.masks_history SET num = 0 WHERE imgid = ?1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if(!manager_position)
    {
      // make room for mask manager history entry
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
        "UPDATE main.history SET num=num+1 WHERE imgid = ?1", -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);

      // update history end
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "UPDATE main.images"
                                  " SET history_end = history_end+1"
                                  " WHERE id = ?1", -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
    }

    // create a mask manager entry in history as first entry
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2
      (dt_database_get(darktable.db),
       "INSERT INTO main.history"
       " (imgid, num, operation, op_params, module, enabled,"
       "  blendop_params, blendop_version, multi_priority, multi_name)"
       " VALUES(?1, 0, ?2, NULL, 1, 0, NULL, 0, 0, '')",
       -1, &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, op_mask_manager, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  dt_unlock_image(imgid);
  dt_history_hash_write_from_history(imgid, DT_HISTORY_HASH_CURRENT);

  dt_database_release_transaction(darktable.db);

  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_DEVELOP_MIPMAP_UPDATED, imgid);
}

/* Please note: dt_history_truncate_on_image
  - can be used in lighttable and darkroom mode
  - It truncates history *exclusively* in the database and does
    *not* touch anything on the history stack
*/
void dt_history_truncate_on_image(const dt_imgid_t imgid,
                                  const int32_t history_end)
{
  dt_lock_image(imgid);
  sqlite3_stmt *stmt;

  if(history_end == 0)
  {
    dt_history_delete_on_image(imgid);
    dt_unlock_image(imgid);
    return;
  }

  dt_database_start_transaction(darktable.db);

  // delete end of history
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM main.history"
                              " WHERE imgid = ?1 "
                              "   AND num >= ?2", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, history_end);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // delete end of masks history
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM main.masks_history"
                              " WHERE imgid = ?1 "
                              "   AND num >= ?2", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, history_end);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // update history end
  dt_image_set_history_end(imgid, history_end);

  dt_unlock_image(imgid);
  dt_history_hash_write_from_history(imgid, DT_HISTORY_HASH_CURRENT);

  dt_database_release_transaction(darktable.db);

  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_DEVELOP_MIPMAP_UPDATED, imgid);
}

gboolean dt_history_compress(const dt_imgid_t imgid)
{
  dt_lock_image(imgid);
  const int test = dt_history_end_attop(imgid);
  if(test == 1) // we do a compression and we know for sure history_end is at the top!
  {
    dt_history_compress_on_image(imgid);

    // now the modules are in right order but need renumbering to remove leaks
    int max=0;    // the maximum num in main_history for an image
    int size=0;   // the number of items in main_history for an image
    int done=0;   // used for renumbering index

    sqlite3_stmt *stmt2;

    // get highest num in history
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT MAX(num) FROM main.history WHERE imgid=?1", -1, &stmt2, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt2, 1, imgid);
    if(sqlite3_step(stmt2) == SQLITE_ROW)
      max = sqlite3_column_int(stmt2, 0);
    sqlite3_finalize(stmt2);

    // get number of items in main.history
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT COUNT(*) FROM main.history WHERE imgid = ?1", -1, &stmt2, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt2, 1, imgid);
    if(sqlite3_step(stmt2) == SQLITE_ROW)
      size = sqlite3_column_int(stmt2, 0);
    sqlite3_finalize(stmt2);

    if((size>0) && (max>0))
    {
      for(int index = 0;index < (max+1); index++)
      {
        sqlite3_stmt *stmt3;
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "SELECT num FROM main.history WHERE imgid=?1 AND num=?2", -1, &stmt3, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt3, 1, imgid);
        DT_DEBUG_SQLITE3_BIND_INT(stmt3, 2, index);
        if(sqlite3_step(stmt3) == SQLITE_ROW)
        {
          sqlite3_stmt *stmt4;
          // step by step set the correct num
          DT_DEBUG_SQLITE3_PREPARE_V2
            (dt_database_get(darktable.db),
             "UPDATE main.history"
             " SET num = ?3"
             " WHERE imgid = ?1 AND num = ?2",
             -1, &stmt4, NULL);
          DT_DEBUG_SQLITE3_BIND_INT(stmt4, 1, imgid);
          DT_DEBUG_SQLITE3_BIND_INT(stmt4, 2, index);
          DT_DEBUG_SQLITE3_BIND_INT(stmt4, 3, done);
          sqlite3_step(stmt4);
          sqlite3_finalize(stmt4);

          done++;
        }
        sqlite3_finalize(stmt3);
      }
    }
    // update history end
    dt_image_set_history_end(imgid, done);

    dt_image_write_sidecar_file(imgid);
  }
  dt_unlock_image(imgid);
  dt_history_hash_write_from_history(imgid, DT_HISTORY_HASH_CURRENT);
  return (test == 1);
}

gboolean dt_history_check_module_exists(const dt_imgid_t imgid,
                                        const char *operation,
                                        const gboolean enabled)
{
  gboolean result = FALSE;
  sqlite3_stmt *stmt;

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
    dt_database_get(darktable.db),
    "SELECT imgid"
    " FROM main.history"
    " WHERE imgid= ?1 AND operation = ?2 AND enabled IN (1, ?3)",
    -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, enabled);
  if(sqlite3_step(stmt) == SQLITE_ROW) result = TRUE;
  sqlite3_finalize(stmt);

  return result;
}

gboolean dt_history_check_module_exists_list(GList *hist,
                                             const char *operation,
                                             const gboolean enabled)
{
  for(GList *h = hist; h; h = g_list_next(h))
  {
    const dt_history_item_t *item = h->data;

    if(!g_strcmp0(item->op, operation) && (item->enabled || !enabled))
      return TRUE;
  }
  return FALSE;
}

GList *dt_history_duplicate(GList *hist)
{
  GList *result = NULL;

  for(GList *h = hist; h; h = g_list_next(h))
  {
    const dt_dev_history_item_t *old = h->data;

    dt_dev_history_item_t *new = malloc(sizeof(dt_dev_history_item_t));

    memcpy(new, old, sizeof(dt_dev_history_item_t));

    int32_t params_size = 0;
    if(old->module)
    {
      params_size = old->module->params_size;
    }
    else
    {
      dt_iop_module_t *base = dt_iop_get_module(old->op_name);
      if(base)
      {
        params_size = base->params_size;
      }
      else
      {
        // nothing else to do
        dt_print(DT_DEBUG_ALWAYS, "[_duplicate_history]"
                 " can't find base module for %s\n", old->op_name);
      }
    }

    if(params_size > 0)
    {
      new->params = malloc(params_size);
      if(new->params)
        memcpy(new->params, old->params, params_size);
    }

    new->blend_params = malloc(sizeof(dt_develop_blend_params_t));
    if(new->blend_params)
      memcpy(new->blend_params, old->blend_params, sizeof(dt_develop_blend_params_t));

    if(old->forms)
      new->forms = dt_masks_dup_forms_deep(old->forms, NULL);

    result = g_list_prepend(result, new);
  }
  return g_list_reverse(result);  // list was built in reverse order, so un-reverse it
}

// if the image has no history return 0
static gsize _history_hash_compute_from_db(const dt_imgid_t imgid,
                                           guint8 **hash)
{
  if(!dt_is_valid_imgid(imgid)) return 0;

  GChecksum *checksum = g_checksum_new(G_CHECKSUM_MD5);
  gsize hash_len = 0;

  sqlite3_stmt *stmt;

  // get history end
  int history_end = 0;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT history_end FROM main.images WHERE id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    if(sqlite3_column_type(stmt, 0) != SQLITE_NULL)
      history_end = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);

  // get history. the active history for an image are all the latest
  // operations (MAX(num)) which are enabled. this is important here
  // as we want the hash to represent the actual developement of the
  // image.
  gboolean history_on = FALSE;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get(darktable.db),
     "SELECT operation, op_params, blendop_params, enabled, MAX(num)"
     " FROM main.history"
     " WHERE imgid = ?1 AND num <= ?2"
     " GROUP BY operation, multi_priority"
     " ORDER BY num",
     -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, history_end);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int enabled = sqlite3_column_int(stmt, 3);
    if(enabled)
    {
      // operation
      char *buf = (char *)sqlite3_column_text(stmt, 0);
      if(buf) g_checksum_update(checksum, (const guchar *)buf, -1);
      // op_params
      buf = (char *)sqlite3_column_blob(stmt, 1);
      int params_len = sqlite3_column_bytes(stmt, 1);
      if(buf) g_checksum_update(checksum, (const guchar *)buf, params_len);
      // blendop_params
      buf = (char *)sqlite3_column_blob(stmt, 2);
      params_len = sqlite3_column_bytes(stmt, 2);
      if(buf) g_checksum_update(checksum, (const guchar *)buf, params_len);
      history_on = TRUE;
    }
  }
  sqlite3_finalize(stmt);

  if(history_on)
  {
    // get module order
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT version, iop_list"
                                " FROM main.module_order"
                                " WHERE imgid = ?1",
                                -1, &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      const int version = sqlite3_column_int(stmt, 0);
      g_checksum_update(checksum, (const guchar *)&version, sizeof(version));
      if(version == DT_IOP_ORDER_CUSTOM)
      {
        // iop_list
        const char *buf = (char *)sqlite3_column_text(stmt, 1);
        if(buf) g_checksum_update(checksum, (const guchar *)buf, -1);
      }
    }
    sqlite3_finalize(stmt);

    const gsize checksum_len = g_checksum_type_get_length(G_CHECKSUM_MD5);
    *hash = g_malloc(checksum_len);
    hash_len = checksum_len;
    g_checksum_get_digest(checksum, *hash, &hash_len);
  }
  g_checksum_free(checksum);

  return hash_len;
}

void dt_history_hash_write_from_history(const dt_imgid_t imgid,
                                        const dt_history_hash_t type)
{
  if(!dt_is_valid_imgid(imgid)) return;

  guint8 *hash = NULL;
  gsize hash_len = _history_hash_compute_from_db(imgid, &hash);
  if(hash_len)
  {
    char *fields = NULL;
    char *values = NULL;
    char *conflict = NULL;
    if(type & DT_HISTORY_HASH_BASIC)
    {
      fields = g_strdup_printf("%s,", "basic_hash");
      values = g_strdup("?2,");
      conflict = g_strdup("basic_hash=?2,");
    }
    if(type & DT_HISTORY_HASH_AUTO)
    {
      dt_util_str_cat(&fields, "%s,", "auto_hash");
      dt_util_str_cat(&values, "?2,");
      dt_util_str_cat(&conflict, "auto_hash=?2,");
    }
    if(type & DT_HISTORY_HASH_CURRENT)
    {
      dt_util_str_cat(&fields, "%s,", "current_hash");
      dt_util_str_cat(&values, "?2,");
      dt_util_str_cat(&conflict, "current_hash=?2,");
    }
    // remove the useless last comma
    if(fields) fields[strlen(fields) - 1] = '\0';
    if(values) values[strlen(values) - 1] = '\0';
    if(conflict) conflict[strlen(conflict) - 1] = '\0';

    if(fields)
    {
      sqlite3_stmt *stmt;
      // clang-format off
      char *query = g_strdup_printf("INSERT INTO main.history_hash"
                                    " (imgid, %s) VALUES (?1, %s)"
                                    " ON CONFLICT (imgid)"
                                    " DO UPDATE SET %s",
                                    fields, values, conflict);
      // clang-format on
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
      DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 2, hash, hash_len, SQLITE_TRANSIENT);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
      g_free(query);
      g_free(fields);
      g_free(values);
      g_free(conflict);
    }
    g_free(hash);
  }
}

void dt_history_hash_write(const dt_imgid_t imgid,
                           const dt_history_hash_values_t *const hash)
{
  if(hash->basic || hash->auto_apply || hash->current)
  {
    sqlite3_stmt *stmt;
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "INSERT OR REPLACE INTO main.history_hash"
                                " (imgid, basic_hash, auto_hash, current_hash)"
                                " VALUES (?1, ?2, ?3, ?4)",
                                -1, &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 2, hash->basic,
                               hash->basic_len, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 3, hash->auto_apply,
                               hash->auto_apply_len, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 4, hash->current,
                               hash->current_len, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    g_free(hash->basic);
    g_free(hash->auto_apply);
    g_free(hash->current);
  }
}

void dt_history_hash_read(const dt_imgid_t imgid,
                          dt_history_hash_values_t *hash)
{
  hash->basic = hash->auto_apply = hash->current = NULL;
  hash->basic_len = hash->auto_apply_len = hash->current_len = 0;
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT basic_hash, auto_hash, current_hash"
                              " FROM main.history_hash"
                              " WHERE imgid = ?1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    void *buf = (void *)sqlite3_column_blob(stmt, 0);
    hash->basic_len = sqlite3_column_bytes(stmt, 0);
    if(buf)
    {
      hash->basic = g_malloc(hash->basic_len);
      memcpy(hash->basic, buf, hash->basic_len);
    }
    buf = (void *)sqlite3_column_blob(stmt, 1);
    hash->auto_apply_len = sqlite3_column_bytes(stmt, 1);
    if(buf)
    {
      hash->auto_apply = g_malloc(hash->auto_apply_len);
      memcpy(hash->auto_apply, buf, hash->auto_apply_len);
    }
    buf = (void *)sqlite3_column_blob(stmt, 2);
    hash->current_len = sqlite3_column_bytes(stmt, 2);
    if(buf)
    {
      hash->current = g_malloc(hash->current_len);
      memcpy(hash->current, buf, hash->current_len);
    }
  }
  sqlite3_finalize(stmt);
}

void dt_history_hash_free(dt_history_hash_values_t *hash)
{
  g_free(hash->basic);
  hash->basic = NULL;
  hash->basic_len = 0;

  g_free(hash->auto_apply);
  hash->auto_apply = NULL;
  hash->auto_apply_len = 0;

  g_free(hash->current);
  hash->current = NULL;
  hash->current_len = 0;
}

gboolean dt_history_hash_is_mipmap_synced(const dt_imgid_t imgid)
{
  gboolean status = FALSE;
  if(!dt_is_valid_imgid(imgid)) return status;
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT CASE"
                              "  WHEN mipmap_hash == current_hash THEN 1"
                              "  ELSE 0 END AS status"
                              " FROM main.history_hash"
                              " WHERE imgid = ?1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    status = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return status;
}

void dt_history_hash_set_mipmap(const dt_imgid_t imgid)
{
  if(!dt_is_valid_imgid(imgid)) return;
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE main.history_hash"
                              " SET mipmap_hash = current_hash"
                              " WHERE imgid = ?1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

dt_history_hash_t dt_history_hash_get_status(const dt_imgid_t imgid)
{
  dt_history_hash_t status = DT_HISTORY_HASH_NONE;
  if(!dt_is_valid_imgid(imgid)) return status;
  sqlite3_stmt *stmt;
  // clang-format off
  char *query =
    g_strdup_printf("SELECT CASE"
                    "  WHEN basic_hash == current_hash THEN %d"
                    "  WHEN auto_hash == current_hash THEN %d"
                    "  WHEN (basic_hash IS NULL OR current_hash != basic_hash) AND"
                    "       (auto_hash IS NULL OR current_hash != auto_hash) THEN %d"
                    "  ELSE %d END AS status"
                    " FROM main.history_hash"
                    " WHERE imgid = %d",
                    DT_HISTORY_HASH_BASIC, DT_HISTORY_HASH_AUTO,
                    DT_HISTORY_HASH_CURRENT, DT_HISTORY_HASH_BASIC, imgid);
  // clang-format on
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              query, -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    status = sqlite3_column_int(stmt, 0);
  }
  // if no history_hash basic status
  else status = DT_HISTORY_HASH_BASIC;
  sqlite3_finalize(stmt);
  g_free(query);
  return status;
}

gboolean dt_history_copy(const dt_imgid_t imgid)
{
  // note that this routine does not copy anything, it just setup the
  // copy_paste proxy with the needed information that will be used
  // while pasting.

  if(!dt_is_valid_imgid(imgid)) return FALSE;

  darktable.view_manager->copy_paste.copied_imageid = imgid;
  darktable.view_manager->copy_paste.full_copy = FALSE;

  if(darktable.view_manager->copy_paste.selops)
  {
    g_list_free(darktable.view_manager->copy_paste.selops);
    darktable.view_manager->copy_paste.selops = NULL;
  }

  // check if images is currently loaded in darkroom
  if(dt_dev_is_current_image(darktable.develop, imgid))
    dt_dev_write_history(darktable.develop);

  return TRUE;
}

gboolean dt_history_copy_parts(const dt_imgid_t imgid)
{
  if(dt_history_copy(imgid))
  {
    // we want to copy all history and let user select the parts needed
    darktable.view_manager->copy_paste.full_copy = TRUE;

    // run dialog, it will insert into selops the selected module

    if(dt_gui_hist_dialog_new(&(darktable.view_manager->copy_paste), imgid, TRUE) ==
       GTK_RESPONSE_CANCEL)
      return FALSE;
    return TRUE;
  }
  else
    return FALSE;
}

gboolean dt_history_paste(const dt_imgid_t imgid,
                          const gboolean merge,
                          const gboolean sync)
{
  gboolean res =
    dt_history_copy_and_paste_on_image(darktable.view_manager->copy_paste.copied_imageid,
                                       imgid, merge,
                                       darktable.view_manager->copy_paste.selops,
                                       darktable.view_manager->copy_paste.copy_iop_order,
                                       darktable.view_manager->copy_paste.full_copy,
                                       sync);
  // indicate whether caller needs to ensure that the sidecar is synched
  return !sync && res;
}

gboolean dt_history_delete(const dt_imgid_t imgid,
                           const gboolean undo)
{
  if(undo)
  {
    dt_undo_lt_history_t *hist = dt_history_snapshot_item_init();

    hist->imgid = imgid;
    dt_history_snapshot_undo_create(hist->imgid,
                                    &hist->before, &hist->before_history_end);

    dt_history_delete_on_image_ext(imgid, FALSE, TRUE);

    dt_history_snapshot_undo_create(hist->imgid, &hist->after, &hist->after_history_end);
    dt_undo_record(darktable.undo, NULL, DT_UNDO_LT_HISTORY,
                   (dt_undo_data_t)hist, dt_history_snapshot_undo_pop,
                   dt_history_snapshot_undo_lt_history_data_free);
  }
  else
  {
    dt_history_delete_on_image_ext(imgid, FALSE, TRUE);
  }

  /* update the aspect ratio if the current sorting is based on
     aspect ratio, otherwise the aspect ratio will be recalculated
     when the mimpap will be recreated */
  if(darktable.collection->params.sorts[DT_COLLECTION_SORT_ASPECT_RATIO])
    dt_image_set_aspect_ratio(imgid, FALSE);
  return TRUE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
