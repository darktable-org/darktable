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

#include "develop/develop.h"
#include "control/control.h"
#include "common/darktable.h"
#include "common/imageio.h"
#include "common/image_cache.h"
#include "common/exif.h"
#include "common/history.h"
#include "common/debug.h"
#include "common/utility.h"
#include "common/tags.h"


void dt_history_delete_on_image(int32_t imgid)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "delete from history where imgid = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  dt_image_t tmp;
  dt_image_init (&tmp);
  dt_image_t *img = dt_image_cache_get(imgid, 'r');
  img->force_reimport = 1;
  img->dirty = 1;
  img->raw_params = tmp.raw_params;
  img->raw_denoise_threshold = tmp.raw_denoise_threshold;
  img->raw_auto_bright_threshold = tmp.raw_auto_bright_threshold;
  img->black = tmp.black;
  img->maximum = tmp.maximum;
  img->output_width = img->width;
  img->output_height = img->height;
  dt_image_cache_flush (img);

  /* if current image in develop reload history */
  if (dt_dev_is_current_image (darktable.develop, imgid))
    dt_dev_reload_history_items (darktable.develop);

  dt_image_cache_release (img, 'r');

  /* remove darktable|changed tag */
  guint tagid = 0;
  dt_tag_new("darktable|changed",&tagid);
  dt_tag_detach(tagid, imgid);
}

void
dt_history_delete_on_selection()
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "select * from selected_images", -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int imgid = sqlite3_column_int (stmt, 0);
    dt_history_delete_on_image (imgid);
  }
  sqlite3_finalize(stmt);
}

int
dt_history_load_and_apply_on_selection (gchar *filename)
{
  int res=0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "select * from selected_images", -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int imgid = sqlite3_column_int(stmt, 0);
    dt_image_t *img = dt_image_cache_get(imgid, 'r');
    if(img)
    {
      if (dt_exif_xmp_read(img, filename, 1))
      {
        res=1;
        break;
      }
      img->force_reimport = 1;
      img->dirty = 1;
      dt_image_cache_flush(img);

      /* if current image in develop reload history */
      if (dt_dev_is_current_image(darktable.develop, imgid))
        dt_dev_reload_history_items (darktable.develop);

      dt_image_cache_release(img, 'r');
    }
  }
  sqlite3_finalize(stmt);
  return res;
}

int
dt_history_copy_and_paste_on_image (int32_t imgid, int32_t dest_imgid, gboolean merge)
{
  sqlite3_stmt *stmt;
  if(imgid==dest_imgid) return 1;

  if(imgid==-1)
  {
    dt_control_log(_("you need to copy history from an image before you paste it onto another"));
    return 1;
  }
    
  dt_image_t *oimg = dt_image_cache_get (imgid, 'r');

  /* if merge onto history stack, lets find history offest in destination image */
  int32_t offs = 0;
  if (merge)
  {
    /* apply on top of history stack */
    DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "select count(num) from history where imgid = ?1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
    if (sqlite3_step (stmt) == SQLITE_ROW) offs = sqlite3_column_int (stmt, 0);
  }
  else
  {
    /* replace history stack */
    DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "delete from history where imgid = ?1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
    sqlite3_step (stmt);
  }
  sqlite3_finalize (stmt);

  /* add the history items to stack offest */
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "insert into history (imgid, num, module, operation, op_params, enabled, blendop_params) select ?1, num+?2, module, operation, op_params, enabled, blendop_params from history where imgid = ?3", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, offs);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, imgid);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  /* reimport image updated image */
  dt_image_t *img = dt_image_cache_get (dest_imgid, 'r');
  img->force_reimport = 1;
  img->dirty = 1;
  img->raw_params = oimg->raw_params;
  img->raw_denoise_threshold = oimg->raw_denoise_threshold;
  img->raw_auto_bright_threshold = oimg->raw_auto_bright_threshold;
  dt_image_cache_flush(img);

  /* if current image in develop reload history */
  if (dt_dev_is_current_image(darktable.develop, dest_imgid))
    dt_dev_reload_history_items (darktable.develop);

  dt_image_cache_release(img, 'r');

  dt_image_cache_release(oimg, 'r');
  return 0;
}

GList *
dt_history_get_items(int32_t imgid)
{
  GList *result=NULL;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "select num, operation, enabled from history where imgid=?1 order by num desc", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  while (sqlite3_step(stmt) == SQLITE_ROW)
  {
    char name[512]= {0};
    dt_history_item_t *item=g_malloc (sizeof (dt_history_item_t));
    item->num = sqlite3_column_int (stmt, 0);
    g_snprintf(name,512,"%s (%s)",sqlite3_column_text (stmt, 1),(sqlite3_column_int (stmt, 2)!=0)?_("on"):_("off"));
    item->name = g_strdup (name);
    result = g_list_append (result,item);
  }
  return result;
}

char *
dt_history_get_items_as_string(int32_t imgid)
{
  // Prepare mapping op -> localized name
  static GHashTable *module_names = NULL;
  if(module_names == NULL)
  {
    module_names = g_hash_table_new(g_str_hash, g_str_equal);
    GList *iop = g_list_first(darktable.iop);
    if(iop != NULL)
    {
      do
      {
        dt_iop_module_so_t * module = (dt_iop_module_so_t *)iop->data;
        g_hash_table_insert(module_names, module->op, _(module->name()));
      }
      while((iop=g_list_next(iop)) != NULL);
    }
  }

  GList *items = NULL;
  const char *onoff[2] = {_("off"), _("on")};
  unsigned int count = 0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "select operation, enabled from history where imgid=?1 order by num desc", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  // collect all the entries in the history from the db
  while (sqlite3_step(stmt) == SQLITE_ROW)
  {
    char name[512]= {0};
    g_snprintf(name,512,"%s (%s)", (char*)g_hash_table_lookup(module_names, sqlite3_column_text(stmt, 0)), (sqlite3_column_int(stmt, 1)==0)?onoff[0]:onoff[1]);
    items = g_list_append(items, g_strdup(name));
    count++;
  }
  return dt_util_glist_to_str("\n", items, count);
}

int
dt_history_copy_and_paste_on_selection (int32_t imgid, gboolean merge)
{
  if (imgid < 0) return 1;

  int res=0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "select * from selected_images where imgid != ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if (sqlite3_step(stmt) == SQLITE_ROW)
  {
    do
    {
      /* get imgid of selected image */
      int32_t dest_imgid = sqlite3_column_int (stmt, 0);

      /* paste history stack onto image id */
      dt_history_copy_and_paste_on_image(imgid,dest_imgid,merge);

    }
    while (sqlite3_step (stmt) == SQLITE_ROW);
  }
  else res = 1;

  sqlite3_finalize(stmt);
  return res;
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
