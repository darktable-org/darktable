/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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

#include "common/darktable.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/exif.h"
#include "common/debug.h"
#include "common/tags.h"
#include "control/control.h"
#include "control/conf.h"
#include "control/jobs.h"
#include <math.h>
#include <sqlite3.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <assert.h>
#include <glob.h>
#include <glib/gstdio.h>

static int64_t dt_image_debug_malloc_size = 0;

static void *
dt_image_debug_malloc(const void *ptr, const size_t size)
{
#ifdef _DEBUG
  assert(ptr == NULL || ptr == (void *)1);
  dt_pthread_mutex_lock(&darktable.db_insert);
  dt_image_debug_malloc_size += size;
  // dt_image_debug_malloc_size ++;
  dt_pthread_mutex_unlock(&darktable.db_insert);
#endif
  return dt_alloc_align(64, size);
}

static void
dt_image_debug_free(void *p, size_t size)
{
#ifdef _DEBUG
  if(!p) return;
  dt_pthread_mutex_lock(&darktable.db_insert);
  dt_image_debug_malloc_size -= size;
  // dt_image_debug_malloc_size --;
  dt_pthread_mutex_unlock(&darktable.db_insert);
#endif
  free(p);
}

static int
dt_image_single_user()
{
  return 0;
  // if -d cache was given, allow only one reader at the time, to trace stale locks.
  // return (darktable.unmuted & DT_DEBUG_CACHE) && img->lock[mip].users;
}

void dt_image_write_sidecar_file(int imgid)
{
  // write .xmp file
  if(imgid > 0 && dt_conf_get_bool("write_sidecar_files"))
  {
    char filename[DT_MAX_PATH+8];
    dt_image_full_path(imgid, filename, DT_MAX_PATH);
    dt_image_path_append_version(imgid, filename, DT_MAX_PATH);
    char *c = filename + strlen(filename);
    sprintf(c, ".xmp");
    dt_exif_xmp_write(imgid, filename);
  }
}

void dt_image_synch_xmp(const int selected)
{
  if(selected > 0)
  {
    dt_image_write_sidecar_file(selected);
  }
  else if(dt_conf_get_bool("write_sidecar_files"))
  {
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select imgid from selected_images", -1, &stmt, NULL);
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      const int imgid = sqlite3_column_int(stmt, 0);
      dt_image_write_sidecar_file(imgid);
    }
    sqlite3_finalize(stmt);
  }
}

void dt_image_synch_all_xmp(const gchar *pathname)
{
  if(dt_conf_get_bool("write_sidecar_files"))
  {
    // Delete all existing .xmp files.
    glob_t *globbuf = malloc(sizeof(glob_t));
    
    gchar *fname = g_strdup(pathname);
    gchar pattern[1024];
    g_snprintf(pattern, 1024, "%s", pathname);
    char *c1 = pattern + strlen(pattern);
    while(*c1 != '.' && c1 > pattern) c1--;
    g_snprintf(c1, pattern + 1024 - c1, "_*");
    char *c2 = fname + strlen(fname);
    while(*c2 != '.' && c2 > fname) c2--;
    g_snprintf(c1+2, pattern + 1024 - c1 - 2, "%s.xmp", c2);

    if (!glob(pattern, 0, NULL, globbuf))
    {
      for (int i=0; i < globbuf->gl_pathc; i++)
      {
        (void)g_unlink(globbuf->gl_pathv[i]);
      }
      globfree(globbuf);
    }
     
    sqlite3_stmt *stmt;
    gchar *imgfname = g_path_get_basename(pathname);
    gchar *imgpath = g_path_get_dirname(pathname);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id from images where film_id in (select id from film_rolls where folder = ?1) and filename = ?2", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, imgpath, strlen(imgpath), SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, imgfname, strlen(imgfname), SQLITE_TRANSIENT);
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      const int imgid = sqlite3_column_int(stmt, 0);
      dt_image_write_sidecar_file(imgid);
    }
    sqlite3_finalize(stmt);
    g_free(fname);
    g_free(imgfname);
    g_free(imgpath);
  }
}

int dt_image_is_ldr(const dt_image_t *img)
{
  const char *c = img->filename + strlen(img->filename);
  while(*c != '.' && c > img->filename) c--;
  if(!strcasecmp(c, ".jpg") || !strcasecmp(c, ".png") || !strcasecmp(c, ".ppm") || (img->flags & DT_IMAGE_LDR)) return 1;
  else return 0;
}

const char *
dt_image_film_roll_name(const char *path)
{
  const char *folder = path + strlen(path);
  int numparts = CLAMPS(dt_conf_get_int("show_folder_levels"), 1, 5);
  int count = 0;
  if (numparts < 1)
    numparts = 1;
  while (folder > path)
  {
    if (*folder == '/')
      if (++count >= numparts)
      {
        ++folder;
        break;
      }
    --folder;
  }
  return folder;
}

void dt_image_film_roll(dt_image_t *img, char *pathname, int len)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select folder from film_rolls where id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img->film_id);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    char *f = (char *)sqlite3_column_text(stmt, 0);
    const char *c = dt_image_film_roll_name(f);
    snprintf(pathname, len, "%s", c);
  }
  else
  {
    snprintf(pathname, len, "%s", _("orphaned image"));
  }
  sqlite3_finalize(stmt);
  pathname[len-1] = '\0';
}

void dt_image_full_path(const int imgid, char *pathname, int len)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select folder || '/' || filename from images, film_rolls where images.film_id = film_rolls.id and images.id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    g_strlcpy(pathname, (char *)sqlite3_column_text(stmt, 0), len);
  }
  sqlite3_finalize(stmt);
}

void dt_image_path_append_version(int imgid, char *pathname, const int len)
{
  // get duplicate suffix
  int version = 0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select count(id) from images where filename in (select filename from images where id = ?1) and film_id in (select film_id from images where id = ?1) and id < ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
    version = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  if(version != 0)
  {
    // add version information:
    char *filename = g_strdup(pathname);

    char *c = pathname + strlen(pathname);
    while(*c != '.' && c > pathname) c--;
    snprintf(c, pathname + len - c, "_%02d", version);
    char *c2 = filename + strlen(filename);
    while(*c2 != '.' && c2 > filename) c2--;
    snprintf(c+3, pathname + len - c - 3, "%s", c2);
  }
}

void dt_image_print_exif(dt_image_t *img, char *line, int len)
{
  if(img->exif_exposure >= 0.1f)
    snprintf(line, len, "%.1f'' f/%.1f %dmm iso %d", img->exif_exposure, img->exif_aperture, (int)img->exif_focal_length, (int)img->exif_iso);
  else
    snprintf(line, len, "1/%.0f f/%.1f %dmm iso %d", 1.0/img->exif_exposure, img->exif_aperture, (int)img->exif_focal_length, (int)img->exif_iso);
}

dt_image_buffer_t dt_image_get_matching_mip_size(const dt_image_t *img, const int32_t width, const int32_t height, int32_t *w, int32_t *h)
{
  const float scale = fminf(darktable.thumbnail_size/(float)(img->width), darktable.thumbnail_size/(float)(img->height));
  int32_t wd = MIN(img->width, (int)(scale*img->width)), ht = MIN(img->height, (int)(scale*img->height));
  if(wd & 0xf) wd = (wd & ~0xf) + 0x10;
  if(ht & 0xf) ht = (ht & ~0xf) + 0x10;
  dt_image_buffer_t mip = DT_IMAGE_MIP4;
  const int32_t wd2 = width + width/2;
  const int32_t ht2 = height + height/2;
  while((int)mip > (int)DT_IMAGE_MIP0 && wd > wd2 && ht > ht2)
  {
    mip--;
    wd >>= 1;
    ht >>= 1;
  }
  *w = wd;
  *h = ht;
  return mip;
}

void dt_image_get_exact_mip_size(const dt_image_t *img, dt_image_buffer_t mip, float *w, float *h)
{
  float wd = img->output_width  ? img->output_width  : img->width,
        ht = img->output_height ? img->output_height : img->height;
  dt_ctl_gui_mode_t mode = dt_conf_get_int("ui_last/view");
  if(darktable.develop->image == img && mode == DT_DEVELOP)
  {
    int tmpw, tmph;
    dt_dev_get_processed_size(darktable.develop, &tmpw, &tmph);
    wd = tmpw;
    ht = tmph;
  }
  if(mip == DT_IMAGE_MIPF)
  {
    // use input width, mipf is before processing
    wd = img->width;
    ht = img->height;
    const float scale = fminf(darktable.thumbnail_size/(float)img->width, darktable.thumbnail_size/(float)img->height);
    // actually we need to be a bit conservative, because of NaN etc out of the bounding box:
    wd = wd*scale - 1;
    ht = ht*scale - 1;
  }
  else if((int)mip < (int)DT_IMAGE_FULL)
  {
    // full image is full size, rest downscaled by output size
    int mwd, mht;
    dt_image_get_mip_size(img, mip, &mwd, &mht);
    const int owd = (int)wd, oht = (int)ht;
    const float scale = fminf(mwd/(float)owd, mht/(float)oht);
    wd = owd*scale;
    ht = oht*scale;
  }
  *w = wd;
  *h = ht;
}

void dt_image_get_mip_size(const dt_image_t *img, dt_image_buffer_t mip, int32_t *w, int32_t *h)
{
  int32_t wd = img->width, ht = img->height;
  if((int)mip < (int)DT_IMAGE_FULL)
  {
    const float scale = fminf(darktable.thumbnail_size/(float)img->width, darktable.thumbnail_size/(float)img->height);
    wd *= scale;
    ht *= scale;
    // make exact mip possible (almost power of two)
    if(wd & 0xf) wd = (wd & ~0xf) + 0x10;
    if(ht & 0xf) ht = (ht & ~0xf) + 0x10;
    while((int)mip < (int)DT_IMAGE_MIP4)
    {
      mip++;
      // if(wd > 32 && ht > 32)
      {
        // only if it's not vanishing completely :)
        wd >>= 1;
        ht >>= 1;
      }
    }
  }
  *w = wd;
  *h = ht;
}

dt_imageio_retval_t dt_image_preview_to_raw(dt_image_t *img)
{
  dt_image_buffer_t mip = dt_image_get(img, DT_IMAGE_MIP4, 'r');
  if(mip == DT_IMAGE_NONE) return DT_IMAGEIO_FILE_NOT_FOUND;
  int p_wd, p_ht, mip_wd, mip_ht;
  float f_wd, f_ht;
  dt_image_get_mip_size(img, DT_IMAGE_MIPF, &p_wd, &p_ht);
  dt_image_get_mip_size(img, mip, &mip_wd, &mip_ht);
  dt_image_get_exact_mip_size(img, DT_IMAGE_MIPF, &f_wd, &f_ht);

  if(dt_image_alloc(img, DT_IMAGE_MIPF))
  {
    dt_image_release(img, mip, 'r');
    return DT_IMAGEIO_CACHE_FULL;
  }
  dt_image_check_buffer(img, mip, 4*mip_wd*mip_ht*sizeof(uint8_t));
  dt_image_check_buffer(img, DT_IMAGE_MIPF, 4*p_wd*p_ht*sizeof(float));

  const int ldr = dt_image_is_ldr(img);
  if(mip_wd == p_wd && mip_ht == p_ht)
  {
    // use 1:1
    if(ldr) for(int j=0; j<mip_ht; j++) for(int i=0; i<mip_wd; i++)
          for(int k=0; k<3; k++) img->mipf[4*(j*p_wd + i) + k] = img->mip[mip][4*(j*mip_wd + i) + 2-k]*(1.0/255.0);
    else for(int j=0; j<mip_ht; j++) for(int i=0; i<mip_wd; i++)
          for(int k=0; k<3; k++) img->mipf[4*(j*p_wd + i) + k] = dt_dev_de_gamma[img->mip[mip][4*(j*mip_wd + i) + 2-k]];
  }
  else
  {
    // scale to fit
    memset(img->mipf,0, 4*p_wd*p_ht*sizeof(float));
    const float scale = fmaxf(mip_wd/f_wd, mip_ht/f_ht);
    for(int j=0; j<p_ht && (int)(scale*j)<mip_ht; j++) for(int i=0; i<p_wd && (int)(scale*i) < mip_wd; i++)
      {
        if(ldr) for(int k=0; k<3; k++) img->mipf[4*(j*p_wd + i) + k] = img->mip[mip][4*((int)(scale*j)*mip_wd + (int)(scale*i)) + 2-k]*(1.0/255.0);
        else    for(int k=0; k<3; k++) img->mipf[4*(j*p_wd + i) + k] = dt_dev_de_gamma[img->mip[mip][4*((int)(scale*j)*mip_wd + (int)(scale*i)) + 2-k]];
      }
  }
  dt_image_release(img, DT_IMAGE_MIPF, 'w');
  dt_image_release(img, DT_IMAGE_MIPF, 'r');
  dt_image_release(img, mip, 'r');
  return DT_IMAGEIO_OK;
}

dt_imageio_retval_t dt_image_raw_to_preview(dt_image_t *img, const float *raw)
{
  const int raw_wd = img->width;
  const int raw_ht = img->height;
  int p_wd, p_ht;
  float f_wd, f_ht;
  dt_image_get_mip_size(img, DT_IMAGE_MIPF, &p_wd, &p_ht);
  dt_image_get_exact_mip_size(img, DT_IMAGE_MIPF, &f_wd, &f_ht);

  if(dt_image_alloc(img, DT_IMAGE_MIPF)) return DT_IMAGEIO_CACHE_FULL;
  dt_image_check_buffer(img, DT_IMAGE_MIPF, 4*p_wd*p_ht*sizeof(float));
  // memset(img->mipf, 0x0, sizeof(float)*4*p_wd*p_ht);

  dt_iop_roi_t roi_in, roi_out;
  roi_in.x = roi_in.y = 0;
  roi_in.width  = raw_wd;
  roi_in.height = raw_ht;
  roi_in.scale = 1.0f;
  roi_out.x = roi_out.y = 0;
  roi_out.width  = p_wd;//f_wd;
  roi_out.height = p_ht;//f_ht;
  roi_out.scale = fminf(f_wd/(float)raw_wd, f_ht/(float)raw_ht);
  if(img->filters)
  {
    // demosaic during downsample
    if(img->bpp == sizeof(float))
      dt_iop_clip_and_zoom_demosaic_half_size_f(img->mipf, (const float *)raw, &roi_out, &roi_in, p_wd, raw_wd, dt_image_flipped_filter(img));
    else
      dt_iop_clip_and_zoom_demosaic_half_size(img->mipf, (const uint16_t *)raw, &roi_out, &roi_in, p_wd, raw_wd, dt_image_flipped_filter(img));
  }
  else
  {
    // downsample
    dt_iop_clip_and_zoom(img->mipf, raw, &roi_out, &roi_in, p_wd, raw_wd);
  }

  dt_image_release(img, DT_IMAGE_MIPF, 'w');
  dt_image_release(img, DT_IMAGE_MIPF, 'r');
  return DT_IMAGEIO_OK;
}

void dt_image_flip(const int32_t imgid, const int32_t cw)
{
  dt_image_t *img = dt_image_cache_get (imgid, 'r');
  int8_t orientation = dt_image_orientation(img);

  if(cw == 1)
  {
    if(orientation & 4) orientation ^= 1;
    else                orientation ^= 2; // flip x
  }
  else
  {
    if(orientation & 4) orientation ^= 2;
    else                orientation ^= 1; // flip y
  }
  orientation ^= 4;             // flip axes

  if(cw == 2) orientation = -1; // reset
  img->raw_params.user_flip = orientation;
  img->force_reimport = 1;
  img->dirty = 1;
  dt_image_invalidate(img, DT_IMAGE_MIPF);
  dt_image_invalidate(img, DT_IMAGE_FULL);
  dt_image_cache_flush(img);
  dt_image_cache_release(img, 'r');
}

int32_t dt_image_duplicate(const int32_t imgid)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "insert into images "
                              "(id, film_id, width, height, filename, maker, model, lens, exposure, aperture, iso, "
                              "focal_length, focus_distance, datetime_taken, flags, output_width, output_height, crop, "
                              "raw_parameters, raw_denoise_threshold, raw_auto_bright_threshold, raw_black, raw_maximum, orientation) "
                              "select null, film_id, width, height, filename, maker, model, lens, exposure, aperture, iso, "
                              "focal_length, focus_distance, datetime_taken, flags, width, height, crop, "
                              "raw_parameters, raw_denoise_threshold, raw_auto_bright_threshold, raw_black, raw_maximum, orientation "
                              "from images where id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select a.id from images as a join images as b where a.film_id = b.film_id and a.filename = b.filename and b.id = ?1 order by a.id desc", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  int32_t newid = -1;
  if(sqlite3_step(stmt) == SQLITE_ROW) newid = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  if(newid != -1)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "insert into color_labels (imgid, color) select ?1, color from color_labels where imgid = ?2", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "insert into meta_data (id, key, value) select ?1, key, value from meta_data where id = ?2", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "insert into tagged_images (imgid, tagid) select ?1, tagid from tagged_images where imgid = ?2", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "update tagxtag set count = count + 1 where "
                                "(id1 in (select tagid from tagged_images where imgid = ?1)) or "
                                "(id2 in (select tagid from tagged_images where imgid = ?1))", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  return newid;
}

void dt_image_remove(const int32_t imgid)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from images where id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "update tagxtag set count = count - 1 where "
                              "(id2 in (select tagid from tagged_images where imgid = ?1)) or "
                              "(id1 in (select tagid from tagged_images where imgid = ?1))", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from tagged_images where imgid = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from history where imgid = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from color_labels where imgid = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from meta_data where id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from selected_images where imgid = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  dt_image_cache_clear(imgid);
}

int dt_image_altered(const dt_image_t *img)
{
  int altered = 0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select num from history where imgid = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img->id);
  if(sqlite3_step(stmt) == SQLITE_ROW) altered = 1;
  sqlite3_finalize(stmt);
  return altered;
}

int dt_image_import_testlock(dt_image_t *img)
{
  dt_pthread_mutex_lock(&darktable.db_insert);
  int lock = img->import_lock;
  if(!lock) img->import_lock = 1;
  dt_pthread_mutex_unlock(&darktable.db_insert);
  return lock;
}

void dt_image_import_unlock(dt_image_t *img)
{
  dt_pthread_mutex_lock(&darktable.db_insert);
  img->import_lock = 0;
  dt_pthread_mutex_unlock(&darktable.db_insert);
}

int dt_image_reimport(dt_image_t *img, const char *filename, dt_image_buffer_t mip)
{
  if(dt_image_import_testlock(img))
  {
    // fprintf(stderr, "[image_reimport] someone is already loading `%s'!\n", filename);
    return 1;
  }
  if(!img->force_reimport)
  {
    dt_image_buffer_t mip1 = dt_image_get(img, mip, 'r');
    dt_image_release(img, mip1, 'r');
    if(mip1 == mip)
    {
      // already loaded
      dt_image_import_unlock(img);
      return 0;
    }
  }
  img->output_width = img->output_height = 0;
  dt_imageio_retval_t ret = dt_imageio_open_preview(img, filename);
  if(ret == DT_IMAGEIO_CACHE_FULL)
  {
    // handle resource conflicts if user provided very small caches:
    dt_image_import_unlock(img);
    return 1;
  }
  else if(ret != DT_IMAGEIO_OK)
  {
    // fprintf(stderr, "[image_reimport] could not open %s\n", filename);
    // dt_image_cleanup(img); // still locked buffers. cache will clean itself after a while.
    dt_control_log(_("image `%s' is not available"), img->filename);
    dt_image_import_unlock(img);
    // dt_image_remove(img->id);
    return 1;
  }

  // fprintf(stderr, "[image_reimport] loading `%s' to fill mip %d!\n", filename, mip);

  int altered = img->force_reimport;
  img->force_reimport = 0;
  if(dt_image_altered(img)) altered = 1;

  // open_preview actually only gave us a mipf and no mip4?
  if(!altered)
  {
    if(dt_image_lock_if_available(img, DT_IMAGE_MIP4, 'r'))
    {
      if(!dt_image_lock_if_available(img, DT_IMAGE_MIPF, 'r'))
      {
        // we have mipf but not mip4.
        altered = 1;
        dt_image_release(img, DT_IMAGE_MIPF, 'r');
      }
    }
    else dt_image_release(img, DT_IMAGE_MIP4, 'r');
  }

  if(altered)
  {
    dt_develop_t dev;
    dt_dev_init(&dev, 0);
    dt_dev_load_preview(&dev, img);
    dt_dev_process_to_mip(&dev);
    dt_dev_cleanup(&dev);
    // load preview keeps a lock on mipf:
    dt_image_release(img, DT_IMAGE_MIPF, 'r');
  }
  dt_image_import_unlock(img);
  return 0;
}

int dt_image_import(const int32_t film_id, const char *filename, gboolean override_ignore_jpegs)
{
  if(!g_file_test(filename, G_FILE_TEST_IS_REGULAR)) return 0;
  const char *cc = filename + strlen(filename);
  for(; *cc!='.'&&cc>filename; cc--);
  if(!strcmp(cc, ".dt")) return 0;
  if(!strcmp(cc, ".dttags")) return 0;
  if(!strcmp(cc, ".xmp")) return 0;
  char *ext = g_ascii_strdown(cc+1, -1);
  if(override_ignore_jpegs == FALSE && (!strcmp(ext, "jpg") || !strcmp(ext, "jpeg")) && dt_conf_get_bool("ui_last/import_ignore_jpegs"))
    return 0;
  int supported = 0;
  char **extensions = g_strsplit(dt_supported_extensions, ",", 100);
  for(char **i=extensions; *i!=NULL; i++)
    if(!strcmp(ext, *i))
    {
      supported = 1;
      break;
    }
  g_strfreev(extensions);
  if(!supported)
  {
    g_free(ext);
    return 0;
  }
  int rc;
  int ret = 0, id = -1;
  // select from images; if found => return
  gchar *imgfname;
  imgfname = g_path_get_basename((const gchar*)filename);
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id from images where film_id = ?1 and filename = ?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, film_id);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, imgfname, strlen(imgfname), SQLITE_STATIC);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    id = sqlite3_column_int(stmt, 0);
    g_free(imgfname);
    sqlite3_finalize(stmt);
    ret = dt_image_open(id); // image already in db, open this.
    g_free(ext);
    if(ret) return 0;
    else return id;
  }
  sqlite3_finalize(stmt);

  // insert dummy image entry in database
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "insert into images (id, film_id, filename, caption, description, license, sha1sum) values (null, ?1, ?2, '', '', '', '')", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, film_id);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, imgfname, strlen(imgfname), SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) fprintf(stderr, "sqlite3 error %d\n", rc);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id from images where film_id = ?1 and filename = ?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, film_id);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, imgfname, strlen(imgfname), SQLITE_STATIC);
  if(sqlite3_step(stmt) == SQLITE_ROW) id = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  // printf("[image_import] importing `%s' to img id %d\n", imgfname, id);
  dt_image_t *img = dt_image_cache_get_uninited(id, 'w');
  g_strlcpy(img->filename, imgfname, DT_MAX_PATH);
  img->id = id;
  img->film_id = film_id;
  img->dirty = 1;

  // read dttags and exif for database queries!
  (void) dt_exif_read(img, filename);
  char dtfilename[DT_MAX_PATH];
  g_strlcpy(dtfilename, filename, DT_MAX_PATH);
  dt_image_path_append_version(img->id, dtfilename, DT_MAX_PATH);
  char *c = dtfilename + strlen(dtfilename);
  sprintf(c, ".xmp");
  (void)dt_exif_xmp_read(img, dtfilename, 0);

  // add a tag with the file extension
  guint tagid = 0;
  char tagname[512];
  snprintf(tagname, 512, "darktable|format|%s", ext);
  g_free(ext);
  dt_tag_new(tagname, &tagid);
  dt_tag_attach(tagid,id);

  dt_image_cache_flush_no_sidecars(img);

  dt_image_cache_release(img, 'w');

  // Search for sidecar files and import them if found.
  glob_t *globbuf = malloc(sizeof(glob_t));

  // Add version wildcard
  gchar *fname = g_strdup(filename);
  gchar pattern[DT_MAX_PATH];
  g_snprintf(pattern, DT_MAX_PATH, "%s", filename);
  char *c1 = pattern + strlen(pattern);
  while(*c1 != '.' && c1 > pattern) c1--;
  snprintf(c1, pattern + DT_MAX_PATH - c1, "_*");
  char *c2 = fname + strlen(fname);
  while(*c2 != '.' && c2 > fname) c2--;
  snprintf(c1+2, pattern + DT_MAX_PATH - c1 - 2, "%s.xmp", c2);

  if (!glob(pattern, 0, NULL, globbuf))
  {
    for (int i=0; i < globbuf->gl_pathc; i++)
    {
      int newid = -1;
      newid = dt_image_duplicate(id);

      dt_image_t *newimg = dt_image_cache_get(newid, 'w');
      (void)dt_exif_xmp_read(newimg, globbuf->gl_pathv[i], 0);

      dt_image_cache_flush_no_sidecars(newimg);
      dt_image_cache_release(newimg, 'w');
    }
    globfree(globbuf);
  }

  g_free(imgfname);
  g_free(fname);

  return id;
}

dt_imageio_retval_t dt_image_update_mipmaps(dt_image_t *img)
{
  if(dt_image_lock_if_available(img, DT_IMAGE_MIP4, 'r')) return DT_IMAGEIO_CACHE_FULL;
  int oldwd, oldht;
  float fwd, fht;
  dt_image_get_mip_size(img, DT_IMAGE_MIP4, &oldwd, &oldht);
  dt_image_get_exact_mip_size(img, DT_IMAGE_MIP4, &fwd, &fht);
  img->mip_width  [DT_IMAGE_MIP4] = oldwd;
  img->mip_height  [DT_IMAGE_MIP4] = oldht;
  img->mip_width_f[DT_IMAGE_MIP4] = fwd;
  img->mip_height_f[DT_IMAGE_MIP4] = fht;

  // here we got mip4 'r' locked
  // create 8-bit mip maps:
  for(dt_image_buffer_t l=DT_IMAGE_MIP3; (int)l>=(int)DT_IMAGE_MIP0; l--)
  {
    // here we got mip l+1 'r' locked
    int p_wd, p_ht;
    dt_image_get_mip_size(img, l, &p_wd, &p_ht);
    dt_image_get_exact_mip_size(img, l, &fwd, &fht);
    if(dt_image_alloc(img, l))
    {
      dt_image_release(img, l+1, 'r');
      return DT_IMAGEIO_CACHE_FULL;
    }
    img->mip_width  [l] = p_wd;
    img->mip_height  [l] = p_ht;
    img->mip_width_f[l] = fwd;
    img->mip_height_f[l] = fht;

    // here, we got mip l+1 'r' locked, and  mip l 'rw'

    dt_image_check_buffer(img, l, p_wd*p_ht*4*sizeof(uint8_t));
    // printf("creating mipmap %d for img %s: %d x %d\n", l, img->filename, p_wd, p_ht);
    // downscale 8-bit mip
    if(oldwd != p_wd)
      for(int j=0; j<p_ht; j++) for(int i=0; i<p_wd; i++)
          for(int k=0; k<4; k++) img->mip[l][4*(j*p_wd + i) + k] = ((int)img->mip[l+1][8*(2*j)*p_wd + 4*(2*i) + k] + (int)img->mip[l+1][8*(2*j)*p_wd + 4*(2*i+1) + k]
                + (int)img->mip[l+1][8*(2*j+1)*p_wd + 4*(2*i+1) + k] + (int)img->mip[l+1][8*(2*j+1)*p_wd + 4*(2*i) + k])/4;
    else memcpy(img->mip[l], img->mip[l+1], 4*sizeof(uint8_t)*p_ht*p_wd);

    dt_image_release(img, l, 'w');
    dt_image_release(img, l+1, 'r');
    // here we got mip l 'r' locked
  }
  dt_image_release(img, DT_IMAGE_MIP0, 'r');
  return DT_IMAGEIO_OK;
}

void dt_image_init(dt_image_t *img)
{
  for(int k=0; (int)k<(int)DT_IMAGE_MIPF; k++) img->mip[k] = NULL;
  memset(img->lock,0, sizeof(dt_image_lock_t)*DT_IMAGE_NONE);
  img->import_lock = 0;
  img->output_width = img->output_height = img->width = img->height = 0;
  img->mipf = NULL;
  img->pixels = NULL;
  img->orientation = -1;
  img->mip_invalid = 0;

  img->black = 0.0f;
  img->maximum = 1.0f;
  img->raw_params.user_flip = -1;
  img->raw_params.med_passes = 0;
  img->raw_params.wb_cam = 0;
  img->raw_params.pre_median = 0;
  img->raw_params.greeneq = 0;
  img->raw_params.no_auto_bright = 0;
  img->raw_params.highlight = 0;
  img->raw_params.demosaic_method = 2;
  img->raw_params.med_passes = 0;
  img->raw_params.four_color_rgb = 0;
  img->raw_params.fill0 = 2;
  img->raw_denoise_threshold = 0.f;
  img->raw_auto_bright_threshold = 0.01f;
  img->filters = 0;
  img->bpp = 0;

  // try to get default raw parameters from db:
  sqlite3_stmt *stmt;
  sqlite3_prepare_v2(dt_database_get(darktable.db), "select op_params from presets where operation = 'rawimport' and def=1", -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const void *blob = sqlite3_column_blob(stmt, 0);
    int length = sqlite3_column_bytes(stmt, 0);
    if(length == sizeof(dt_image_raw_parameters_t) + 2*sizeof(float))
      memcpy(&(img->raw_denoise_threshold), blob, length);
  }
  sqlite3_finalize(stmt);
  img->film_id = -1;
  img->flags = dt_conf_get_int("ui_last/import_initial_rating");
  if(img->flags < 0 || img->flags > 4)
  {
    img->flags = 1;
    dt_conf_set_int("ui_last/import_initial_rating",1);
  }
  img->id = -1;
  img->force_reimport = 0;
  img->dirty = 0;
  img->exif_inited = 0;
  memset(img->exif_maker,0, sizeof(img->exif_maker));
  memset(img->exif_model,0, sizeof(img->exif_model));
  memset(img->exif_lens,0, sizeof(img->exif_lens));
  memset(img->filename,0, sizeof(img->filename));
  g_strlcpy(img->filename, "(unknown)", 10);
  img->exif_model[0] = img->exif_maker[0] = img->exif_lens[0] = '\0';
  g_strlcpy(img->exif_datetime_taken, "0000:00:00 00:00:00", sizeof(img->exif_datetime_taken));
  img->exif_crop = 1.0;
  img->exif_exposure = img->exif_aperture = img->exif_iso = img->exif_focal_length = img->exif_focus_distance = 0;
  for(int k=0; (int)k<(int)DT_IMAGE_NONE; k++) img->mip_buf_size[k] = 0;
  for(int k=0; (int)k<(int)DT_IMAGE_FULL; k++) img->mip_width[k] = img->mip_height[k] = 0;
}

int dt_image_open(const int32_t id)
{
  if(id < 1) return 1;
  dt_image_t *img = dt_image_cache_get(id, 'w');
  if(!img) return 1;
  dt_image_cache_release(img, 'w');
  return 0;
}

int dt_image_open2(dt_image_t *img, const int32_t id)
{
  // load stuff from db and store in cache:
  if(id <= 0) return 1;
  int rc, ret = 1;
  char *str;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id, film_id, width, height, filename, maker, model, lens, exposure, aperture, iso, focal_length, datetime_taken, flags, output_width, output_height, crop, raw_parameters, raw_denoise_threshold, raw_auto_bright_threshold, raw_black, raw_maximum, orientation, focus_distance from images where id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  // DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, img->filename, strlen(img->filename), SQLITE_STATIC);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    img->id      = sqlite3_column_int(stmt, 0);
    img->film_id = sqlite3_column_int(stmt, 1);
    img->width   = sqlite3_column_int(stmt, 2);
    img->height  = sqlite3_column_int(stmt, 3);
    img->filename[0] = img->exif_maker[0] = img->exif_model[0] = img->exif_lens[0] =
        img->exif_datetime_taken[0] = '\0';
    str = (char *)sqlite3_column_text(stmt, 4);
    if(str) g_strlcpy(img->filename,   str, 512);
    str = (char *)sqlite3_column_text(stmt, 5);
    if(str) g_strlcpy(img->exif_maker, str, 32);
    str = (char *)sqlite3_column_text(stmt, 6);
    if(str) g_strlcpy(img->exif_model, str, 32);
    str = (char *)sqlite3_column_text(stmt, 7);
    if(str) g_strlcpy(img->exif_lens,  str, 52);
    img->exif_exposure = sqlite3_column_double(stmt, 8);
    img->exif_aperture = sqlite3_column_double(stmt, 9);
    img->exif_iso = sqlite3_column_double(stmt, 10);
    img->exif_focal_length = sqlite3_column_double(stmt, 11);
    str = (char *)sqlite3_column_text(stmt, 12);
    if(str) g_strlcpy(img->exif_datetime_taken, str, 20);
    img->flags = sqlite3_column_int(stmt, 13);
    img->output_width  = sqlite3_column_int(stmt, 14);
    img->output_height = sqlite3_column_int(stmt, 15);
    img->exif_crop = sqlite3_column_double(stmt, 16);
    *(int *)&img->raw_params = sqlite3_column_int(stmt, 17);
    img->raw_denoise_threshold = sqlite3_column_double(stmt, 18);
    img->raw_auto_bright_threshold = sqlite3_column_double(stmt, 19);
    img->black   = sqlite3_column_double(stmt, 20);
    img->maximum = sqlite3_column_double(stmt, 21);
    img->orientation = sqlite3_column_int(stmt, 22);
    img->exif_focus_distance = sqlite3_column_double(stmt,23);
    if(img->exif_focus_distance >= 0 && img->orientation >= 0) img->exif_inited = 1;

    ret = 0;
  }
  else fprintf(stderr, "[image_open2] failed to open image from database: %s\n", sqlite3_errmsg(dt_database_get(darktable.db)));
  rc = sqlite3_finalize(stmt);
  if(ret) return ret;
  return rc;
}

void dt_image_cleanup(dt_image_t *img)
{
  dt_pthread_mutex_lock(&(darktable.mipmap_cache->mutex));
  for(int k=0; (int)k<(int)DT_IMAGE_NONE; k++) dt_image_free(img, k);
  dt_pthread_mutex_unlock(&(darktable.mipmap_cache->mutex));
}

// this should load and return with 'r' lock on mip buffer.
int dt_image_load(dt_image_t *img, dt_image_buffer_t mip)
{
  if(!img) return 1;
  int ret = 0;
  char filename[DT_MAX_PATH];
  dt_image_full_path(img->id, filename, DT_MAX_PATH);
  // reimport forced?
  if(mip != DT_IMAGE_FULL &&
      (img->force_reimport || img->width == 0 || img->height == 0))
  {
    dt_image_reimport(img, filename, mip);
    if(dt_image_lock_if_available(img, mip, 'r')) ret = 1;
    else ret = 0;
  }
  // else we might be able to fetch it from the caches.
  else if(mip == DT_IMAGE_MIPF)
  {
    if(dt_image_lock_if_available(img, DT_IMAGE_FULL, 'r'))
    {
      // get mipf from half-size raw
      ret = dt_imageio_open_preview(img, filename);
      dt_image_validate(img, DT_IMAGE_MIPF);
      if(!ret && dt_image_lock_if_available(img, mip, 'r')) ret = 1;
      else ret = 0;
    }
    else
    {
      // downscale full buffer
      dt_image_raw_to_preview(img, img->pixels);
      dt_image_validate(img, DT_IMAGE_MIPF);
      dt_image_release(img, DT_IMAGE_FULL, 'r');
      if(dt_image_lock_if_available(img, mip, 'r')) ret = 1;
      else ret = 0;
    }
  }
  else if(mip == DT_IMAGE_FULL)
  {
    // after _open, the full buffer will be 'r' locked.
    ret = dt_imageio_open(img, filename);
    dt_image_raw_to_preview(img, img->pixels);
    dt_image_validate(img, DT_IMAGE_MIPF);
  }
  else
  {
    // refuse to load thumbnails for currently developed image.
    dt_ctl_gui_mode_t mode = dt_conf_get_int("ui_last/view");
    if(darktable.develop->image == img && mode == DT_DEVELOP) ret = 1;
    else
    {
      dt_image_reimport(img, filename, mip);
      if(dt_image_lock_if_available(img, mip, 'r')) ret = 1;
      else ret = 0;
    }
  }
  if(!ret) dt_image_validate(img, mip);

  return ret;
}

#ifdef _DEBUG
#define dt_image_set_lock_last_auto(A, B, C) dt_image_set_lock_last(A, B, file, line, function, C)
static void
dt_image_set_lock_last(dt_image_t *image, dt_image_buffer_t mip,
                       const char *file, int line, const char *function, const char mode)
{
  snprintf(image->lock_last[mip], 100, "%c by %s:%d %s", mode, file, line, function);
}
#else
#define dt_image_set_lock_last_auto(A, B, C)
#endif

#ifdef _DEBUG
void dt_image_prefetch_with_caller(dt_image_t *img, dt_image_buffer_t mip,
                                   const char *file, const int line, const char *function)
#else
void dt_image_prefetch(dt_image_t *img, dt_image_buffer_t mip)
#endif
{
  if(!img || mip > DT_IMAGE_MIPF || mip < DT_IMAGE_MIP0) return;
  dt_pthread_mutex_lock(&(darktable.mipmap_cache->mutex));
  if(img->mip_buf_size[mip] > 0)
  {
    // already loaded.
    dt_pthread_mutex_unlock(&(darktable.mipmap_cache->mutex));
    return;
  }
  dt_job_t j;
  dt_image_load_job_init(&j, img->id, mip);
  // if the job already exists, make it high-priority, if not, add it:
  if(dt_control_revive_job(darktable.control, &j) < 0)
    dt_control_add_job(darktable.control, &j);
  dt_pthread_mutex_unlock(&(darktable.mipmap_cache->mutex));
}


// =============================
//   mipmap cache functions:
// =============================

void dt_mipmap_cache_init(dt_mipmap_cache_t *cache, int32_t entries)
{
  dt_pthread_mutex_init(&(cache->mutex), NULL);
  for(int k=0; k<(int)DT_IMAGE_NONE; k++)
  {
    cache->total_size[k] = 0;
    // support up to 24 threads working on full images at the time:
    if(k == DT_IMAGE_FULL) entries = 24;
    dt_print(DT_DEBUG_CACHE, "[mipmap_cache_init] cache has %d entries for mip %d.\n", entries, k);
    cache->num_entries[k] = entries;
    cache->mip_lru[k] = (dt_image_t **)malloc(sizeof(dt_image_t*)*entries);
    memset(cache->mip_lru[k],0, sizeof(dt_image_t*)*entries);
  }
}

void dt_mipmap_cache_cleanup(dt_mipmap_cache_t *cache)
{
  // TODO: free all img bufs?
  for(int k=0; k<(int)DT_IMAGE_NONE; k++) free(cache->mip_lru[k]);
  dt_pthread_mutex_destroy(&(cache->mutex));
}

void dt_mipmap_cache_print(dt_mipmap_cache_t *cache)
{
  int64_t buffers = 0;
  uint64_t bytes = 0;
  for(int k=0; k<(int)DT_IMAGE_NONE; k++)
  {
    int users = 0, write = 0, entries = 0;
    for(int i=0; i<cache->num_entries[k]; i++)
    {
      if(cache->mip_lru[k][i])
      {
        // dt_print(DT_DEBUG_CACHE, "[cache entry] buffer %d, image %s locks: %d r %d w\n", k, cache->mip_lru[k][i]->filename, cache->mip_lru[k][i]->lock[k].users, cache->mip_lru[k][i]->lock[k].write);
        entries++;
        users += cache->mip_lru[k][i]->lock[k].users;
        write += cache->mip_lru[k][i]->lock[k].write;
        bytes += cache->mip_lru[k][i]->mip_buf_size[k];
        if(cache->mip_lru[k][i]->mip_buf_size[k]) buffers ++;
#ifdef _DEBUG
        if(cache->mip_lru[k][i]->lock[k].users || cache->mip_lru[k][i]->lock[k].write)
          dt_print(DT_DEBUG_CACHE, "[mipmap_cache] img %d mip %d used by %d %s\n", cache->mip_lru[k][i]->id, k, cache->mip_lru[k][i]->lock[k].users, cache->mip_lru[k][i]->lock_last[k]);
#endif
      }
    }
    printf("[mipmap_cache] mip %d: fill: %d/%d, users: %d, writers: %d\n", k, entries, cache->num_entries[k], users, write);
    printf("[mipmap_cache] total memory in mip %d: %.2f MB\n", k, cache->total_size[k]/(1024.0*1024.0));
  }
  // printf("[mipmap_cache] occupies %.2f MB in %"PRIi64" (%"PRIi64") buffers\n", bytes/(1024.0*1024.0), buffers, dt_image_debug_malloc_size);
  printf("[mipmap_cache] occupies %.2f MB in %"PRIi64" (%.2f) buffers\n", bytes/(1024.0*1024.0), buffers, dt_image_debug_malloc_size/(1024.0*1024.0));
}

void dt_image_check_buffer(dt_image_t *image, dt_image_buffer_t mip, int32_t size)
{
#ifdef _DEBUG
  assert(image->mip_buf_size[mip] >= size);
#endif
}

#ifdef _DEBUG
int dt_image_alloc_with_caller(dt_image_t *img, dt_image_buffer_t mip,
                               const char *file, const int line, const char *function)
#else
int dt_image_alloc(dt_image_t *img, dt_image_buffer_t mip)
#endif
{
  int wd, ht;
  dt_image_get_mip_size(img, mip, &wd, &ht);
  size_t size = wd*ht;
  dt_pthread_mutex_lock(&(darktable.mipmap_cache->mutex));
  void *ptr = NULL;
  if     ((int)mip <  (int)DT_IMAGE_MIPF)
  {
    size *= 4*sizeof(uint8_t);
    ptr = (void *)(img->mip[mip]);
  }
  else if(mip == DT_IMAGE_MIPF)
  {
    size *= 4*sizeof(float);
    ptr = (void *)(img->mipf);
  }
  else if(mip == DT_IMAGE_FULL && (img->filters == 0))
  {
    size *= 4*sizeof(float);
    ptr = (void *)(img->pixels);
  }
  else if(mip == DT_IMAGE_FULL && (img->filters != 0))
  {
    size *= img->bpp;
    ptr = (void *)(img->pixels);
  }
  else
  {
    dt_pthread_mutex_unlock(&(darktable.mipmap_cache->mutex));
    return 1;
  }
  if(ptr)
  {
    if(img->lock[mip].users)
    {
      // still locked by others (only write lock allone doesn't suffice, that's just a singleton thread indicator!)
      dt_print(DT_DEBUG_CACHE, "[image_alloc] buffer mip %d is still locked! (w:%d u:%d)\n", mip, img->lock[mip].write, img->lock[mip].users);
#ifdef _DEBUG
      dt_print(DT_DEBUG_CACHE, "[image_alloc] last for img %d mip %d lock acquired %s\n", img->id, mip, img->lock_last[mip]);
#endif
      dt_pthread_mutex_unlock(&(darktable.mipmap_cache->mutex));
      return 1;
    }
    if(size != img->mip_buf_size[mip])
    {
      // free buffer, alter cache size stats, and continue below.
      dt_image_free(img, mip);
    }
    else
    {
      dt_image_set_lock_last_auto(img, mip, 'w');
      // dt_print(DT_DEBUG_CACHE, "[image_alloc] locking already allocated image %s\n", img->filename);
      img->lock[mip].write = 1; // write lock
      img->lock[mip].users = 1; // read lock
      dt_pthread_mutex_unlock(&(darktable.mipmap_cache->mutex));
      return 0; // all good, already alloc'ed.
    }
  }

  // printf("allocing %d x %d x %d for %s (%d)\n", wd, ht, size/(wd*ht), img->filename, mip);
  if     ((int)mip <  (int)DT_IMAGE_MIPF)
  {
    img->mip[mip] = (uint8_t *)dt_image_debug_malloc(img->mip[mip], size);
  }
  else if(mip == DT_IMAGE_MIPF)
  {
    img->mipf = (float *)dt_image_debug_malloc(img->mipf, size);
  }
  else if(mip == DT_IMAGE_FULL)
  {
    img->pixels = (float *)dt_image_debug_malloc(img->pixels, size);
  }

  if((mip == DT_IMAGE_FULL && img->pixels == NULL) || (mip == DT_IMAGE_MIPF && img->mipf == NULL) || ((int)mip <  (int)DT_IMAGE_MIPF && img->mip[mip] == NULL))
  {
    fprintf(stderr, "[image_alloc] malloc of %d x %d x %d for image %s mip %d failed!\n", wd, ht, (int)size/(wd*ht), img->filename, mip);
    dt_pthread_mutex_unlock(&(darktable.mipmap_cache->mutex));
    return 1;
  }

  // dt_print(DT_DEBUG_CACHE, "[image_alloc] locking newly allocated image %s of size %f MB\n", img->filename, size/(1024*1024.0));

  // garbage collect, free enough space for new buffer:
  dt_mipmap_cache_t *cache = darktable.mipmap_cache;
  // max memory: user supplied number of bytes, evenly distributed among mip levels.
  // clamped to a min of 50MB
  size_t max_mem = (size_t)(MAX(52428800, (size_t)dt_conf_get_int("cache_memory"))/(float)DT_IMAGE_FULL);
  dt_print(DT_DEBUG_CACHE, "[image_alloc] mip %d uses %.3f/%.3f MB, alloc %.3f MB\n", mip,
           cache->total_size[mip]/(1024.0*1024.0), max_mem/(1024.0*1024.0), size/(1024.0*1024.0));
  if(cache->total_size[mip] > 0 && cache->total_size[mip]+size > max_mem)
    for(int k=0; k<cache->num_entries[mip]; k++)
    {
      if(cache->mip_lru[mip][k] != NULL &&
          (cache->mip_lru[mip][k]->lock[mip].users == 0 && cache->mip_lru[mip][k]->lock[mip].write == 0))
      {
        dt_image_t *entry = cache->mip_lru[mip][k];
        dt_image_free(entry, mip);
        dt_print(DT_DEBUG_CACHE, "[image_alloc] free mip %d to %.2f MB\n", mip, cache->total_size[mip]/(1024.0*1024.0));
        if(cache->total_size[mip] < 0)
        {
          fprintf(stderr, "[image_alloc] WARNING: memory usage for mip %d dropped below zero!\n", mip);
          cache->total_size[mip] = 0;
        }
        if(cache->total_size[mip] == 0 || cache->total_size[mip]+size < max_mem) break;
      }
    }
  // insert image in node list at newest time
  for(int k=0; k<darktable.mipmap_cache->num_entries[mip]; k++)
  {
    if(darktable.mipmap_cache->mip_lru[mip][k] == NULL ||
        (darktable.mipmap_cache->mip_lru[mip][k]->lock[mip].users == 0 && darktable.mipmap_cache->mip_lru[mip][k]->lock[mip].write == 0))
    {
      dt_image_free(darktable.mipmap_cache->mip_lru[mip][k], mip);
      memmove(darktable.mipmap_cache->mip_lru[mip] + k, darktable.mipmap_cache->mip_lru[mip] + k + 1, (darktable.mipmap_cache->num_entries[mip] - k - 1)*sizeof(dt_image_t*));
      darktable.mipmap_cache->mip_lru[mip][darktable.mipmap_cache->num_entries[mip]-1] = img;
      img->lock[mip].write = 1; // write lock
      img->lock[mip].users = 1; // read lock
      img->mip_buf_size[mip] = size;
      cache->total_size[mip] += size;
#if 0
      int allsize = 0;
      for(int k=0; k<darktable.mipmap_cache->num_entries[mip]; k++) if(darktable.mipmap_cache->mip_lru[mip][k]) allsize += darktable.mipmap_cache->mip_lru[mip][k]->mip_buf_size[mip];
      printf("[image_alloc] alloc'ed additional %d bytes, now storing %f MB for mip %d\n", size, allsize/(1024.*1024.), mip);
#endif
      dt_pthread_mutex_unlock(&(darktable.mipmap_cache->mutex));
      return 0;
    }
  }
  fprintf(stderr, "[image_alloc] all cache slots seem to be in use! alloc of %d bytes for img id %d mip %d failed!\n", (int)size, img->id, mip);
  for(int k=0; k<darktable.mipmap_cache->num_entries[mip]; k++) fprintf(stderr, "[image_alloc] slot[%d] lock %s %d\n", k,darktable.mipmap_cache->mip_lru[mip][k]->lock[mip].write == 0 ? " " : "w", darktable.mipmap_cache->mip_lru[mip][k]->lock[mip].users);
  dt_pthread_mutex_unlock(&(darktable.mipmap_cache->mutex));
  return 1;
}

void dt_image_free(dt_image_t *img, dt_image_buffer_t mip)
{
  // mutex is already locked, as only alloc is allowed to free.
  if(!img) return;
  // dt_image_release(img, mip, "w");
  if((int)mip < (int)DT_IMAGE_MIPF)
  {
    if(img->mip[mip] != (uint8_t *)1) dt_image_debug_free(img->mip[mip], img->mip_buf_size[mip]);
    img->mip[mip] = NULL;
  }
  else if(mip == DT_IMAGE_MIPF)
  {
    if(img->mipf != (float *)1) dt_image_debug_free(img->mipf, img->mip_buf_size[mip]);
    img->mipf = NULL;
  }
  else if(mip == DT_IMAGE_FULL)
  {
    dt_image_debug_free(img->pixels, img->mip_buf_size[mip]);
    img->pixels = NULL;
  }
  else return;
  // FIXME: for some reason this takes a looong time: (says perf)
  for(int k=0; k<darktable.mipmap_cache->num_entries[mip]; k++)
    if(darktable.mipmap_cache->mip_lru[mip][k] == img) darktable.mipmap_cache->mip_lru[mip][k] = NULL;
  darktable.mipmap_cache->total_size[mip] -= img->mip_buf_size[mip];
  // printf("[image_free] freed %d bytes\n", img->mip_buf_size[mip]);
#ifdef _DEBUG
  if(darktable.control->running)
    assert(img->lock[mip].users == 0);
#endif
  img->mip_buf_size[mip] = 0;
}

#ifdef _DEBUG
int dt_image_lock_if_available_with_caller(dt_image_t *img, const dt_image_buffer_t mip, const char mode,
    const char *file, const int line, const char *function)
#else
int dt_image_lock_if_available(dt_image_t *img, const dt_image_buffer_t mip, const char mode)
#endif
{
  if(mip == DT_IMAGE_NONE) return 1;
  dt_pthread_mutex_lock(&(darktable.mipmap_cache->mutex));
  int ret = 0;
  // get image with no write lock set!
  if((int)mip < (int)DT_IMAGE_MIPF)
  {
    if(img->mip[mip] == NULL || img->lock[mip].write) ret = 1;
  }
  else if(mip == DT_IMAGE_MIPF)
  {
    if(img->mipf == NULL || img->lock[mip].write) ret = 1;
  }
  else if(mip == DT_IMAGE_FULL)
  {
    if(img->pixels == NULL || img->lock[mip].write) ret = 1;
  }
  if(img->mip_invalid & (1<<mip)) ret = 1;
  if(ret == 0)
  {
    if(mode == 'w')
    {
      if(img->lock[mip].users) ret = 1;
      else
      {
        dt_image_set_lock_last_auto(img, mip, 'w');
        img->lock[mip].write = 1;
        img->lock[mip].users = 1;
      }
    }
    else if(dt_image_single_user() && img->lock[mip].users) ret = 1;
    else
    {
      dt_image_set_lock_last_auto(img, mip, 'r');
      img->lock[mip].users++;
    }
  }
  dt_pthread_mutex_unlock(&(darktable.mipmap_cache->mutex));
  return ret;
}

#ifdef _DEBUG
dt_image_buffer_t dt_image_get_blocking_with_caller(dt_image_t *img, const dt_image_buffer_t mip_in, const char mode,
    const char *file, const int line, const char *function)
#else
dt_image_buffer_t dt_image_get_blocking(dt_image_t *img, const dt_image_buffer_t mip_in, const char mode)
#endif
{
  dt_image_buffer_t mip = mip_in;
  if(!img || mip == DT_IMAGE_NONE) return DT_IMAGE_NONE;
#ifndef _WIN32
  dt_print(DT_DEBUG_CONTROL, "[run_job+] 10 %f get blocking image %d mip %d\n", dt_get_wtime(), img->id, mip_in);
#endif
  dt_pthread_mutex_lock(&(darktable.mipmap_cache->mutex));
  // get image with no write lock set!
  if((int)mip < (int)DT_IMAGE_MIPF)
  {
    while(mip > 0 && (img->mip[mip] == NULL || img->lock[mip].write)) mip--;
  }
  else if(mip == DT_IMAGE_MIPF)
  {
    if(img->mipf == NULL || img->lock[mip].write) mip = DT_IMAGE_NONE;
  }
  else if(mip == DT_IMAGE_FULL)
  {
    if(img->pixels == NULL || img->lock[mip].write) mip = DT_IMAGE_NONE;
  }
  const int invalid = img->mip_invalid & (1<<mip);
  if(invalid) mip = DT_IMAGE_NONE;
  // found?
  if(mip == mip_in)
  {
    if(mode == 'w')
    {
      if(img->lock[mip].users) mip = DT_IMAGE_NONE;
      else
      {
        dt_image_set_lock_last_auto(img, mip, 'w');
        img->lock[mip].write = 1;
        img->lock[mip].users = 1;
      }
    }
    // if -d cache was given, allow only one reader at the time, to trace stale locks.
    else if(dt_image_single_user() && img->lock[mip].users) mip = DT_IMAGE_NONE;
    else
    {
      dt_image_set_lock_last_auto(img, mip, 'r');
      img->lock[mip].users++;
    }
    dt_pthread_mutex_unlock(&(darktable.mipmap_cache->mutex));
#ifndef _WIN32
    dt_print(DT_DEBUG_CONTROL, "[run_job-] 10 %f get blocking image %d mip %d\n", dt_get_wtime(), img->id, mip_in);
#endif
    return mip;
  }
  // already loading?
  if(img->lock[mip_in].write)
  {
    dt_pthread_mutex_unlock(&(darktable.mipmap_cache->mutex));
#ifndef _WIN32
    dt_print(DT_DEBUG_CONTROL, "[run_job-] 10 %f get blocking image %d mip %d\n", dt_get_wtime(), img->id, mip_in);
#endif
    return DT_IMAGE_NONE;
  }
  dt_pthread_mutex_unlock(&(darktable.mipmap_cache->mutex));

  // dt_print(DT_DEBUG_CACHE, "[image_get_blocking] requested buffer %d, found %d for image %s locks: %d r %d w\n", mip_in, mip, img->filename, img->lock[mip].users, img->lock[mip].write);

  // start job to load this buf in bg.
  dt_print(DT_DEBUG_CACHE, "[image_get_blocking] reloading mip %d for image %d\n", mip_in, img->id);
//   if(dt_image_load(img, mip_in)) mip = DT_IMAGE_NONE; // this returns with 'r' locked
  dt_image_load(img, mip_in);
  mip = mip_in;

  dt_pthread_mutex_lock(&(darktable.mipmap_cache->mutex));
  if(mip != DT_IMAGE_NONE)
  {
    if(mode == 'w')
    {
      dt_image_set_lock_last_auto(img, mip, 'w');
      img->lock[mip].write = 1;
      img->lock[mip].users = 1;
    }
    // else img->lock[mip].users++; // already incremented by image_load
  }
  dt_pthread_mutex_unlock(&(darktable.mipmap_cache->mutex));
#ifndef _WIN32
  dt_print(DT_DEBUG_CONTROL, "[run_job-] 10 %f get blocking image %d mip %d\n", dt_get_wtime(), img->id, mip_in);
#endif
  return mip;
}

#ifdef _DEBUG
dt_image_buffer_t dt_image_get_with_caller(dt_image_t *img, const dt_image_buffer_t mip_in, const char mode,
    const char *file, const int line, const char *function)
#else
dt_image_buffer_t dt_image_get(dt_image_t *img, const dt_image_buffer_t mip_in, const char mode)
#endif
{
  dt_image_buffer_t mip = mip_in;
  if(!img || mip == DT_IMAGE_NONE) return DT_IMAGE_NONE;
  dt_pthread_mutex_lock(&(darktable.mipmap_cache->mutex));
  // get image with no write lock set!
  if((int)mip < (int)DT_IMAGE_MIPF)
  {
    while(mip > 0 && (img->mip[mip] == NULL || img->lock[mip].write)) mip--;
    if(mip == 0 && (img->mip[mip] == NULL || img->lock[mip].write)) mip = DT_IMAGE_NONE;
  }
  else if(mip == DT_IMAGE_MIPF)
  {
    if(img->mipf == NULL || img->lock[mip].write) mip = DT_IMAGE_NONE;
  }
  else if(mip == DT_IMAGE_FULL)
  {
    if(img->pixels == NULL || img->lock[mip].write) mip = DT_IMAGE_NONE;
  }
  if((mip != DT_IMAGE_MIPF && mip != DT_IMAGE_FULL && img->force_reimport) ||
      (mip != DT_IMAGE_MIPF && img == darktable.develop->image && darktable.develop->image_force_reload))
    mip = DT_IMAGE_NONE;
  const int invalid = img->mip_invalid & (1<<mip);
  if(invalid) mip = DT_IMAGE_NONE;
  if(mip != DT_IMAGE_NONE)
  {
    if(mode == 'w')
    {
      if(img->lock[mip].users) mip = DT_IMAGE_NONE;
      else
      {
        dt_image_set_lock_last_auto(img, mip, 'w');
        img->lock[mip].write = 1;
        img->lock[mip].users = 1;
      }
    }
    // if -d cache was given, allow only one reader at the time, to trace stale locks.
    else if(dt_image_single_user() && img->lock[mip].users) mip = DT_IMAGE_NONE;
    else
    {
      dt_image_set_lock_last_auto(img, mip, 'r');
      img->lock[mip].users++;
    }
  }

  // dt_print(DT_DEBUG_CACHE, "[image_get] requested buffer %d, found %d for image %s locks: %d r %d w\n", mip_in, mip, img->filename, img->lock[mip_in].users, img->lock[mip_in].write);

#if 1
  if(mip != mip_in) // && !img->lock[mip_in].write)
  {
    // start job to load this buf in bg.
    dt_image_buffer_t mip2 = mip_in;
    if(mip2 < DT_IMAGE_MIP4) mip2 = DT_IMAGE_MIP4; // this will fill all smaller maps, too.
#ifdef _DEBUG
    dt_print(DT_DEBUG_CACHE, "[image_get] reloading mip %d for image %d request %s:%d %s\n", mip2, img->id, file, line, function);
#else
    dt_print(DT_DEBUG_CACHE, "[image_get] reloading mip %d for image %d\n", mip2, img->id);
#endif
    dt_job_t j;
    dt_image_load_job_init(&j, img->id, mip2);
    // if the job already exists, make it high-priority, if not, add it:
    if(dt_control_revive_job(darktable.control, &j) < 0)
      dt_control_add_job(darktable.control, &j);
  }
#endif
  dt_pthread_mutex_unlock(&(darktable.mipmap_cache->mutex));
  return mip;
}
#undef dt_image_set_lock_last_auto

void dt_image_release(dt_image_t *img, dt_image_buffer_t mip, const char mode)
{
  if(mip == DT_IMAGE_NONE) return;
  dt_pthread_mutex_lock(&(darktable.mipmap_cache->mutex));
  if (mode == 'r' && img->lock[mip].users > 0) img->lock[mip].users --;
  else if (mode == 'w') img->lock[mip].write = 0;  // can only be one writing thread at a time.
  dt_pthread_mutex_unlock(&(darktable.mipmap_cache->mutex));
  // dt_print(DT_DEBUG_CACHE, "[image_release] released lock %c for buffer %d on image %s locks: %d r %d w\n", mode, mip, img->filename, img->lock[mip].users, img->lock[mip].write);
}

void dt_image_invalidate(dt_image_t *image, dt_image_buffer_t mip)
{
  dt_pthread_mutex_lock(&(darktable.mipmap_cache->mutex));
  image->mip_invalid |= 1<<mip;
  dt_pthread_mutex_unlock(&(darktable.mipmap_cache->mutex));
}

void dt_image_validate(dt_image_t *image, dt_image_buffer_t mip)
{
  dt_pthread_mutex_lock(&(darktable.mipmap_cache->mutex));
  image->mip_invalid &= ~(1<<mip);
  dt_pthread_mutex_unlock(&(darktable.mipmap_cache->mutex));
}


// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
