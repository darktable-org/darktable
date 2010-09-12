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
#include "common/darktable.h"
#include "common/imageio.h"
#include "common/image_cache.h"
#include "common/history.h"


void dt_history_delete_on_image(int32_t imgid)
{
  sqlite3_stmt *stmt;
  sqlite3_prepare_v2 (darktable.db, "delete from history where imgid = ?1", -1, &stmt, NULL);
  sqlite3_bind_int (stmt, 1, imgid);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  dt_image_t tmp;
  dt_image_init (&tmp);
  dt_image_t *img = dt_image_cache_get(imgid, 'r');
  img->force_reimport = 1;
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
  
}

void 
dt_history_delete_on_selection()
{
  sqlite3_stmt *stmt;
  sqlite3_prepare_v2(darktable.db, "select * from selected_images", -1, &stmt, NULL);
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
  sqlite3_prepare_v2(darktable.db, "select * from selected_images", -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int imgid = sqlite3_column_int(stmt, 0);
    if (dt_imageio_dt_read(imgid, filename))
    {
      res=1;
      break;
    }
    dt_image_t *img = dt_image_cache_get(imgid, 'r');
    img->force_reimport = 1;
    dt_image_cache_flush(img);
    
    /* if current image in develop reload history */
    if (dt_dev_is_current_image(darktable.develop, imgid))
      dt_dev_reload_history_items (darktable.develop);
    
    dt_image_cache_release(img, 'r');
  }
  sqlite3_finalize(stmt);
  return res;
}

int 
dt_history_copy_and_paste_on_image (int32_t imgid, int32_t dest_imgid, gboolean merge)
{
  int rc;
  sqlite3_stmt *stmt;
  if(imgid==dest_imgid) return 1;
  
  dt_image_t *oimg = dt_image_cache_get (imgid, 'r');
  
  /* if merge onto history stack, lets find history offest in destination image */
  int32_t offs = 0; 
  if (merge)
  { 
    /* apply on top of history stack */
    rc = sqlite3_prepare_v2 (darktable.db, "select num from history where imgid = ?1", -1, &stmt, NULL);
    rc = sqlite3_bind_int (stmt, 1, dest_imgid);
    while (sqlite3_step (stmt) == SQLITE_ROW) offs++;
  }
  else
  { 
    /* replace history stack */
    rc = sqlite3_prepare_v2 (darktable.db, "delete from history where iimgid = ?1", -1, &stmt, NULL);
    rc = sqlite3_bind_int (stmt, 1, dest_imgid);
    rc = sqlite3_step (stmt);
  }
  rc = sqlite3_finalize (stmt);

  /* add the history items to stack offest */
  rc = sqlite3_prepare_v2 (darktable.db, "insert into history (imgid, num, module, operation, op_params, enabled) select ?1, num+?2, module, operation, op_params, enabled from history where imgid = ?3", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, dest_imgid);
  rc = sqlite3_bind_int (stmt, 2, offs);
  rc = sqlite3_bind_int (stmt, 3, imgid);
  rc = sqlite3_step (stmt);
  rc = sqlite3_finalize (stmt);
  rc = sqlite3_prepare_v2 (darktable.db, "delete from mipmaps where imgid = ?1", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, dest_imgid);
  rc = sqlite3_step (stmt);
  rc = sqlite3_finalize (stmt);
  
  /* reimport image updated image */
  dt_image_t *img = dt_image_cache_get (dest_imgid, 'r');
  img->force_reimport = 1;
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

int 
dt_history_copy_and_paste_on_selection (int32_t imgid, gboolean merge)
{
  if (imgid < 0) return 1;

  int rc;
  int res=0;
  sqlite3_stmt *stmt;
  rc = sqlite3_prepare_v2 (darktable.db, "select * from selected_images", -1, &stmt, NULL);
  if (sqlite3_step(stmt) == SQLITE_ROW)
  {
    do
    {
      /* get imgid of selected image */
      int32_t dest_imgid = sqlite3_column_int (stmt, 0);
      
      /* dont past history into source */
      if (dest_imgid == imgid) continue;
      
      /* past history stack onto image id */
      dt_history_copy_and_paste_on_image(imgid,dest_imgid,merge);

    }while (sqlite3_step (stmt) == SQLITE_ROW);
  }
  else res = 1;
  
  sqlite3_finalize(stmt);
  return res;
}
