/*
    This file is part of darktable,
    copyright (c) 2011 johannes hanika.

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

void dt_image_cache_init   (dt_image_cache_t *cache)
{
}
void dt_image_cache_cleanup(dt_image_cache_t *cache)
{
}
void dt_image_cache_print  (dt_image_cache_t *cache)
{
}

void*
dt_image_cache_allocate(void *data, const uint32_t key, int32_t *cost)
{
  // TODO: check cost and keep it below 80%!
  // TODO: *cost = 1; ?
  // TODO: if key = 0 or -1: insert dummy into sql
  // TODO: get the image struct from sql:

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



  // TODO: grab image * from static pool
}

const dt_image_t*
dt_image_cache_read_get(dt_cache_image_t *cache, const int32_t id)
{
  return (const dt_image_t *)dt_cache_read_get(&cache->cache, id);
}

// drops the read lock on an image struct
void              dt_image_cache_read_release(dt_cache_image_t *cache, const dt_image_t *img);
// augments the already acquired read lock on an image to write the struct.
// blocks until all readers have stepped back from this image (all but one,
// which is assumed to be this thread)
dt_image_t       *dt_image_cache_write_get(dt_cache_image_t *cache, const dt_image_t *img);



// *******************************************************
// xmp stuff
// *******************************************************

// TODO: all the xmp stuff hidden in the image cache!
// TODO: users should now use dt_image_cache_write_release
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


// drops the write priviledges on an image struct.
// thtis triggers a write-through to sql, and if the setting
// is present, also to xmp sidecar files (safe setting).
void              dt_image_cache_write_release(dt_cache_image_t *cache, dt_image_t *img)
{
  // TODO: take care of sql and xmp dumping
}


