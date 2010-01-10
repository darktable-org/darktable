
#include "common/darktable.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/exif.h"
#include "control/control.h"
#include "control/jobs.h"
#include <math.h>
#include <sqlite3.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <assert.h>

void dt_image_write_dt_files(dt_image_t *img)
{
  // write .dt file
  if(gconf_client_get_bool(darktable.control->gconf, DT_GCONF_DIR"/write_dt_files", NULL))
  {
    char filename[520];
    dt_image_full_path(img, filename, 512);
    char *c = filename + strlen(filename);
    for(;c>filename && *c != '.';c--);
    sprintf(c, ".dt");
    dt_imageio_dt_write(img->id, filename);
    sprintf(c, ".dttags");
    dt_imageio_dttags_write(img->id, filename);
  }
}

void dt_image_full_path(dt_image_t *img, char *pathname, int len)
{
  if(img->film_id == 1)
  {
    snprintf(pathname, len, "%s", img->filename);
  }
  else if(darktable.film->id == img->film_id)
  {
    snprintf(pathname, len, "%s/%s", darktable.film->dirname, img->filename);
  }
  else
  {
    int rc;
    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(darktable.db, "select folder from film_rolls where id = ?1", -1, &stmt, NULL);
    rc = sqlite3_bind_int(stmt, 1, img->film_id);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      snprintf(pathname, len, "%s/%s", (char *)sqlite3_column_text(stmt, 0), img->filename);
    }
    rc = sqlite3_finalize(stmt);
  }
  pathname[len-1] = '\0';
}

void dt_image_export_path(dt_image_t *img, char *pathname, int len)
{
  if(img->film_id == 1)
  {
    snprintf(pathname, len, "%s", img->filename);
  }
  else if(darktable.film->id == img->film_id)
  {
    snprintf(pathname, len, "%s/darktable_exported/%s", darktable.film->dirname, img->filename);
  }
  else
  {
    int rc;
    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(darktable.db, "select folder from film_rolls where id = ?1", -1, &stmt, NULL);
    rc = sqlite3_bind_int(stmt, 1, img->film_id);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      snprintf(pathname, len, "%s/darktable_exported/%s", (char *)sqlite3_column_text(stmt, 0), img->filename);
    }
    rc = sqlite3_finalize(stmt);
  }
  pathname[len-1] = '\0';
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
  const float scale = fminf(DT_IMAGE_WINDOW_SIZE/(float)(img->width), DT_IMAGE_WINDOW_SIZE/(float)(img->height));
  int32_t wd = MIN(img->width, (int)(scale*img->width)), ht = MIN(img->height, (int)(scale*img->height));
  if(wd & 0xf) wd = (wd & ~0xf) + 0x10;
  if(ht & 0xf) ht = (ht & ~0xf) + 0x10;
  dt_image_buffer_t mip = DT_IMAGE_MIP4;
  const int32_t wd2 = width + width/2;
  const int32_t ht2 = height + height/2;
  while((int)mip > (int)DT_IMAGE_MIP0 && wd > wd2 && ht > ht2)
  {
    mip--;
    if(wd > 32 || ht > 32)
    { // only if it's not vanishing completely :)
      wd >>= 1;
      ht >>= 1;
    }
  }
  *w = wd;
  *h = ht;
  return mip;
}

void dt_image_get_exact_mip_size(const dt_image_t *img, dt_image_buffer_t mip, float *w, float *h)
{
  float wd = img->output_width  ? img->output_width  : img->width,
        ht = img->output_height ? img->output_height : img->height;
  if(mip == DT_IMAGE_MIPF)
  { // use input width, mipf is before processing
    wd = img->width;
    ht = img->height;
    const float scale = fminf(1.0, fminf(DT_IMAGE_WINDOW_SIZE/(float)img->width, DT_IMAGE_WINDOW_SIZE/(float)img->height));
    wd *= scale; ht *= scale;
    while((int)mip < (int)DT_IMAGE_MIP4)
    {
      mip++;
      if(wd > 32 || ht > 32)
      { // only if it's not vanishing completely :)
        wd *= .5;
        ht *= .5;
      }
    }
  }
  else if((int)mip < (int)DT_IMAGE_FULL)
  { // full image is full size, rest downscaled by output size
    int mwd, mht;
    dt_image_get_mip_size(img, mip, &mwd, &mht);
    const int owd = img->output_width  ? img->output_width  : img->width,
              oht = img->output_height ? img->output_height : img->height;
    const float scale = fminf(1.0, fminf(mwd/(float)owd, mht/(float)oht));
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
    const float scale = fminf(1.0, fminf(DT_IMAGE_WINDOW_SIZE/(float)img->width, DT_IMAGE_WINDOW_SIZE/(float)img->height));
    wd *= scale; ht *= scale;
    // make exact mip possible (almost power of two)
    if(wd & 0xf) wd = (wd & ~0xf) + 0x10;
    if(ht & 0xf) ht = (ht & ~0xf) + 0x10;
    while((int)mip < (int)DT_IMAGE_MIP4)
    {
      mip++;
      if(wd > 32 && ht > 32)
      { // only if it's not vanishing completely :)
        wd >>= 1;
        ht >>= 1;
      }
    }
  }
  *w = wd;
  *h = ht;
}

int dt_image_preview_to_raw(dt_image_t *img)
{
  int p_wd, p_ht;
  float f_wd, f_ht;
  dt_image_get_mip_size(img, DT_IMAGE_MIPF, &p_wd, &p_ht);
  dt_image_get_exact_mip_size(img, DT_IMAGE_MIPF, &f_wd, &f_ht);

  if(dt_image_alloc(img, DT_IMAGE_MIPF)) return 1;
  dt_image_check_buffer(img, DT_IMAGE_MIP4, 4*p_wd*p_ht*sizeof(uint8_t));
  dt_image_check_buffer(img, DT_IMAGE_MIPF, 3*p_wd*p_ht*sizeof(float));

  // convert and store in db.
  dt_imageio_preview_8_to_f(p_wd, p_ht, img->mip[DT_IMAGE_MIP4], img->mipf);
  dt_image_release(img, DT_IMAGE_MIPF, 'w');
  dt_imageio_preview_write(img, DT_IMAGE_MIPF);
  dt_image_release(img, DT_IMAGE_MIPF, 'r');
  return 0;
}

int dt_image_raw_to_preview(dt_image_t *img)
{
  const int raw_wd = img->width;
  const int raw_ht = img->height;
  int p_wd, p_ht;
  float f_wd, f_ht;
  dt_image_get_mip_size(img, DT_IMAGE_MIPF, &p_wd, &p_ht);
  dt_image_get_exact_mip_size(img, DT_IMAGE_MIPF, &f_wd, &f_ht);

  if(dt_image_alloc(img, DT_IMAGE_MIPF)) return 1;
  dt_image_check_buffer(img, DT_IMAGE_MIPF, 3*p_wd*p_ht*sizeof(float));

  if(raw_wd == p_wd && raw_ht == p_ht)
  { // use 1:1
    for(int j=0;j<raw_ht;j++) for(int i=0;i<raw_wd;i++)
    {
      float *cam = img->pixels + 3*(j*raw_wd + i);
      for(int k=0;k<3;k++) img->mipf[3*(j*p_wd + i) + k] = cam[k];
    }
  }
  else
  { // scale to fit
    bzero(img->mipf, 3*p_wd*p_ht*sizeof(float));
    const float scale = fmaxf(raw_wd/f_wd, raw_ht/f_ht);
    for(int j=0;j<p_ht && scale*j<raw_ht;j++) for(int i=0;i<p_wd && scale*i < raw_wd;i++)
    {
      float *cam = img->pixels + 3*((int)(scale*j)*raw_wd + (int)(scale*i));
      for(int k=0;k<3;k++) img->mipf[3*(j*p_wd + i) + k] = cam[k];
    }
  }
  // store in db.
  dt_imageio_preview_write(img, DT_IMAGE_MIPF);

  dt_image_release(img, DT_IMAGE_MIPF, 'w');
  dt_image_release(img, DT_IMAGE_MIPF, 'r');
  return 0;
}

int dt_image_reimport(dt_image_t *img, const char *filename)
{
  // this brute-force lock might be a bit too much:
  // dt_image_t *imgl = dt_image_cache_use(img->id, 'w');
  // if(!imgl)
  if(img->import_lock)
  {
    // fprintf(stderr, "[image_reimport] someone is already loading `%s'!\n", filename);
    return 1;
  }
  img->import_lock = 1;
  if(dt_imageio_open_preview(img, filename))
  {
    fprintf(stderr, "[image_reimport] could not open %s\n", filename);
    // dt_image_cleanup(img); // still locked buffers. cache will clean itself after a while.
    // dt_image_cache_release(img, 'w');
    int rc;
    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(darktable.db, "delete from images where id = ?1", -1, &stmt, NULL);
    rc = sqlite3_bind_int (stmt, 1, img->id);
    rc = sqlite3_step(stmt);
    rc = sqlite3_prepare_v2(darktable.db, "delete from mipmaps where imgid = ?1", -1, &stmt, NULL);
    rc = sqlite3_bind_int (stmt, 1, img->id);
    rc = sqlite3_step(stmt);
    rc = sqlite3_prepare_v2(darktable.db, "delete from mipmap_timestamps where imgid = ?1", -1, &stmt, NULL);
    rc = sqlite3_bind_int (stmt, 1, img->id);
    rc = sqlite3_step(stmt);
    rc = sqlite3_prepare_v2(darktable.db, "delete from tagged_images where imgid = ?1", -1, &stmt, NULL);
    rc = sqlite3_bind_int (stmt, 1, img->id);
    rc = sqlite3_step(stmt);
  }

  // already some db entry there?
  int rc, altered = 0;
  sqlite3_stmt *stmt;
  rc = sqlite3_prepare_v2(darktable.db, "select num from history where imgid = ?1", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, img->id);
  if(sqlite3_step(stmt) == SQLITE_ROW) altered = 1;
  sqlite3_finalize(stmt);

  // try loading a .dt[tags] file
  char dtfilename[1031];
  strncpy(dtfilename, filename, 1024);
  char *c = dtfilename + strlen(dtfilename);
  for(;c>dtfilename && *c != '.';c--);
  sprintf(c, ".dttags");
  (void)dt_imageio_dttags_read(img, dtfilename);
  sprintf(c, ".dt");
  if(altered || !dt_imageio_dt_read(img->id, dtfilename))
  {
    dt_develop_t dev;
    dt_dev_init(&dev, 0);
    dt_dev_load_preview(&dev, img);
    dt_dev_process_to_mip(&dev);
    dt_dev_cleanup(&dev);
  }
  img->import_lock = 0;
  // dt_image_cache_release(imgl, 'w');
  return 0;
}

int dt_image_import(const int32_t film_id, const char *filename)
{
  if(!g_file_test(filename, G_FILE_TEST_IS_REGULAR)) return 0;
  const char *cc = filename + strlen(filename);
  for(;*cc!='.'&&cc>filename;cc--);
  if(!strcmp(cc, ".dt")) return 0;
  if(!strcmp(cc, ".dttags")) return 0;
  char *ext = g_ascii_strdown(cc+1, -1);
  int supported = 0;
  char **extensions = g_strsplit(dt_supported_extensions, ",", 100);
  for(char **i=extensions;*i!=NULL;i++)
    if(!strcmp(ext, *i)) { supported = 1; break; }
  g_strfreev(extensions);
  g_free(ext);
  if(!supported) return 0;
  int rc;
  int ret = 0, id = -1;
  // select from images; if found => return
  gchar *imgfname;
  if(film_id > 1) imgfname = g_path_get_basename((const gchar*)filename);
  else            imgfname = g_build_filename((const gchar*)filename, NULL);
  sqlite3_stmt *stmt;
  rc = sqlite3_prepare_v2(darktable.db, "select id from images where film_id = ?1 and filename = ?2", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, film_id);
  rc = sqlite3_bind_text(stmt, 2, imgfname, strlen(imgfname), SQLITE_STATIC);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    id = sqlite3_column_int(stmt, 0);
    g_free(imgfname);
    rc = sqlite3_finalize(stmt);
    ret = dt_image_open(id); // image already in db, open this.
    if(ret) return 0;
    else return id;
  }
  rc = sqlite3_finalize(stmt);

  // insert dummy image entry in database
  rc = sqlite3_prepare_v2(darktable.db, "insert into images (id, film_id, filename) values (null, ?1, ?2)", -1, &stmt, NULL);
  HANDLE_SQLITE_ERR(rc);
  rc = sqlite3_bind_int (stmt, 1, film_id);
  rc = sqlite3_bind_text(stmt, 2, imgfname, strlen(imgfname), SQLITE_TRANSIENT);
  pthread_mutex_lock(&(darktable.db_insert));
  rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) fprintf(stderr, "sqlite3 error %d\n", rc);
  id = sqlite3_last_insert_rowid(darktable.db);
  pthread_mutex_unlock(&(darktable.db_insert));
  rc = sqlite3_finalize(stmt);

  // printf("[image_import] importing `%s' to img id %d\n", imgfname, id);
  dt_image_t *img = dt_image_cache_use(id, 'w');
  strncpy(img->filename, imgfname, 256);
  img->id = id;
  img->film_id = film_id;

  // read dttags and exif for database queries!
  (void) dt_exif_read(img, filename);
  char dtfilename[1024];
  strncpy(dtfilename, filename, 1024);
  char *c = dtfilename + strlen(dtfilename);
  for(;c>dtfilename && *c != '.';c--);
  sprintf(c, ".dttags");
  (void)dt_imageio_dttags_read(img, dtfilename);

  dt_image_cache_flush(img);

  g_free(imgfname);

  // single images will probably want to load immediately.
  if(img->film_id == 1) (void)dt_image_reimport(img, filename);

  dt_image_cache_release(img, 'w');
  return id;
}

int dt_image_update_mipmaps(dt_image_t *img)
{
  if(dt_image_lock_if_available(img, DT_IMAGE_MIP4, 'r')) return 1;
  int oldwd, oldht;
  dt_image_get_mip_size(img, DT_IMAGE_MIP4, &oldwd, &oldht);
  // create 8-bit mip maps:
  for(dt_image_buffer_t l=DT_IMAGE_MIP3;(int)l>=(int)DT_IMAGE_MIP0;l--)
  {
    int p_wd, p_ht;
    dt_image_get_mip_size(img, l, &p_wd, &p_ht);
    if(dt_image_alloc(img, l)) return 1;

    dt_image_check_buffer(img, l, p_wd*p_ht*4*sizeof(uint8_t));
    // printf("creating mipmap %d for img %s: %d x %d\n", l, img->filename, p_wd, p_ht);
    // downscale 8-bit mip
    if(oldwd != p_wd)
      for(int j=0;j<p_ht;j++) for(int i=0;i<p_wd;i++)
        for(int k=0;k<4;k++) img->mip[l][4*(j*p_wd + i) + k] = ((int)img->mip[l+1][8*(2*j)*p_wd + 4*(2*i) + k] + (int)img->mip[l+1][8*(2*j)*p_wd + 4*(2*i+1) + k]
                                                    + (int)img->mip[l+1][8*(2*j+1)*p_wd + 4*(2*i+1) + k] + (int)img->mip[l+1][8*(2*j+1)*p_wd + 4*(2*i) + k])/4;
    else memcpy(img->mip[l], img->mip[l+1], 4*sizeof(uint8_t)*p_ht*p_wd);

    if(dt_imageio_preview_write(img, l))
      fprintf(stderr, "[update_mipmaps] could not write mip level %d of image %s to database!\n", l, img->filename);
    dt_image_release(img, l, 'w');
    dt_image_release(img, l+1, 'r');
  }
  dt_image_release(img, DT_IMAGE_MIP0, 'r');
  return 0;
}

void dt_image_init(dt_image_t *img)
{
  for(int k=0;(int)k<(int)DT_IMAGE_MIPF;k++) img->mip[k] = NULL;
  bzero(img->lock, sizeof(dt_image_lock_t)*DT_IMAGE_NONE);
  img->import_lock = 0;
  img->output_width = img->output_height = img->width = img->height = 0;
  img->mipf = NULL;
  img->pixels = NULL;
  img->orientation = -1; // not inited.
  img->raw_params.user_flip = -1;
  img->raw_params.med_passes = 0;
  img->raw_params.wb_auto = 0;
  img->raw_params.wb_cam = 1;
  img->raw_params.cmatrix = 1;
  img->raw_params.no_auto_bright = 0;
  img->raw_params.highlight = 2;
  img->raw_params.demosaic_method = 2;
  img->raw_params.med_passes = 0;
  img->raw_params.four_color_rgb = 0;
  img->raw_params.fill0 = 0;
  img->raw_denoise_threshold = 0.f;
  img->raw_auto_bright_threshold = 0.01f;
  img->film_id = -1;
  img->flags = 1; // every image has one star. zero is deleted.
  img->id = -1;
  img->cacheline = -1;
  bzero(img->exif_maker, sizeof(img->exif_maker));
  bzero(img->exif_model, sizeof(img->exif_model));
  bzero(img->exif_lens, sizeof(img->exif_lens));
  bzero(img->filename, sizeof(img->filename));
  strncpy(img->filename, "(unknown)", 10);
  img->exif_model[0] = img->exif_maker[0] = img->exif_lens[0] = '\0';
  strncpy(img->exif_datetime_taken, "0000:00:00 00:00:00\0", 20);
  img->exif_crop = 1.0;
  img->exif_exposure = img->exif_aperture = img->exif_iso = img->exif_focal_length = 0;
#ifdef _DEBUG
  for(int k=0;(int)k<(int)DT_IMAGE_NONE;k++) img->mip_buf_size[k] = 0;
#endif
}

int dt_image_open(const int32_t id)
{
  if(id < 1) return 1;
  dt_image_t *img = dt_image_cache_use(id, 'w');
  int rc = dt_image_open2(img, id);
  dt_image_cache_release(img, 'w');
  return rc;
}

int dt_image_open2(dt_image_t *img, const int32_t id)
{ // load stuff from db and store in cache:
  int rc, ret = 1;
  sqlite3_stmt *stmt;
  rc = sqlite3_prepare_v2(darktable.db, "select id, film_id, width, height, filename, maker, model, lens, exposure, aperture, iso, focal_length, datetime_taken, flags, output_width, output_height, crop, raw_parameters, raw_denoise_threshold, raw_auto_bright_threshold from images where id = ?1", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, id);
  // rc = sqlite3_bind_text(stmt, 2, img->filename, strlen(img->filename), SQLITE_STATIC);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    img->id      = sqlite3_column_int(stmt, 0);
    img->film_id = sqlite3_column_int(stmt, 1);
    img->width   = sqlite3_column_int(stmt, 2);
    img->height  = sqlite3_column_int(stmt, 3);
    strncpy(img->filename,   (char *)sqlite3_column_text(stmt, 4), 512);
    strncpy(img->exif_maker, (char *)sqlite3_column_text(stmt, 5), 30);
    strncpy(img->exif_model, (char *)sqlite3_column_text(stmt, 6), 30);
    strncpy(img->exif_lens,  (char *)sqlite3_column_text(stmt, 7), 50);
    img->exif_exposure = sqlite3_column_double(stmt, 8);
    img->exif_aperture = sqlite3_column_double(stmt, 9);
    img->exif_iso = sqlite3_column_double(stmt, 10);
    img->exif_focal_length = sqlite3_column_double(stmt, 11);
    strncpy(img->exif_datetime_taken, (char *)sqlite3_column_text(stmt, 12), 20);
    img->flags = sqlite3_column_int(stmt, 13);
    img->output_width  = sqlite3_column_int(stmt, 14);
    img->output_height = sqlite3_column_int(stmt, 15);
    img->exif_crop = sqlite3_column_double(stmt, 16);
    *(int *)&img->raw_params = sqlite3_column_int(stmt, 17);
    img->raw_denoise_threshold = sqlite3_column_double(stmt, 18);
    img->raw_auto_bright_threshold = sqlite3_column_double(stmt, 19);
    
    ret = 0;
  }
  else fprintf(stderr, "[image_open2] failed to open image from database: %s\n", sqlite3_errmsg(darktable.db));
  rc = sqlite3_finalize(stmt);
  if(ret) return ret;
  // read mip 0:
  rc = dt_imageio_preview_read(img, DT_IMAGE_MIP0);
  if(!rc) dt_image_release(img, DT_IMAGE_MIP0, 'r');
  return rc;
}

void dt_image_cleanup(dt_image_t *img)
{
  pthread_mutex_lock(&(darktable.mipmap_cache->mutex));
  for(int k=0;(int)k<(int)DT_IMAGE_NONE;k++) dt_image_free(img, k);
  pthread_mutex_unlock(&(darktable.mipmap_cache->mutex));
}

int dt_image_load(dt_image_t *img, dt_image_buffer_t mip)
{
  int ret = 0;
  if(dt_imageio_preview_read(img, mip))
  { // img not in database. => mip == FULL or kicked out or corrupt or first time load..
    char filename[1024];
    dt_image_full_path(img, filename, 1024);
    if(mip == DT_IMAGE_MIPF)
    {
      ret = 0;
      if(dt_image_lock_if_available(img, DT_IMAGE_FULL, 'r'))
      {
        if(dt_image_reimport(img, filename)) ret = 1;
      }
      else
      {
        ret = dt_image_raw_to_preview(img);
        dt_image_release(img, DT_IMAGE_FULL, 'r');
      }
    }
    else if(mip == DT_IMAGE_FULL)
    {
      ret = dt_imageio_open(img, filename);
      ret = dt_image_raw_to_preview(img);
      dt_image_release(img, mip, 'w');
    }
    else
    {
      ret = dt_image_reimport(img, filename);
      dt_image_release(img, mip, 'w');
    }
  }
  // TODO: insert abstract hook here?
  dt_control_queue_draw_all();
  return ret;
}

void dt_image_prefetch(dt_image_t *img, dt_image_buffer_t mip)
{
  // TODO: alloc buf, dispatch loader job (which will release the rw lock)
}


// =============================
//   mipmap cache functions:
// =============================

void dt_mipmap_cache_init(dt_mipmap_cache_t *cache, int32_t entries)
{
  pthread_mutex_init(&(cache->mutex), NULL);
  for(int k=0;k<(int)DT_IMAGE_NONE;k++)
  {
    if(k == DT_IMAGE_FULL) entries = 3;
    dt_print(DT_DEBUG_CACHE, "mipmap cache has %d entries for mip %d.\n", entries, k);
    cache->num_entries[k] = entries;
    cache->mip_lru[k] = (dt_image_t **)malloc(sizeof(dt_image_t*)*entries);
    bzero(cache->mip_lru[k], sizeof(dt_image_t*)*entries);
    if(entries > 4) entries >>= 1;
  }
}

void dt_mipmap_cache_cleanup(dt_mipmap_cache_t *cache)
{
  // TODO: free all img bufs?
  for(int k=0;k<(int)DT_IMAGE_NONE;k++) free(cache->mip_lru[k]);
  pthread_mutex_destroy(&(cache->mutex));
}

void dt_mipmap_cache_print(dt_mipmap_cache_t *cache)
{
  uint64_t bytes = 0;
  for(int k=0;k<(int)DT_IMAGE_NONE;k++)
  {
    int users = 0, write = 0, entries = 0;
    for(int i=0;i<cache->num_entries[k];i++)
    {
      if(cache->mip_lru[k][i])
      {
        // dt_print(DT_DEBUG_CACHE, "[cache entry] buffer %d, image %s locks: %d r %d w\n", k, cache->mip_lru[k][i]->filename, cache->mip_lru[k][i]->lock[k].users, cache->mip_lru[k][i]->lock[k].write);
        entries++;
        users += cache->mip_lru[k][i]->lock[k].users;
        write += cache->mip_lru[k][i]->lock[k].write;
#ifdef _DEBUG
        bytes += cache->mip_lru[k][i]->mip_buf_size[k];
#endif
      }
    }
    printf("mip %d: fill: %d/%d, users: %d, writers: %d\n", k, entries, cache->num_entries[k], users, write);
  }
  printf("mipmap cache occupies %.2f MB\n", bytes/(1024.0*1024.0));
}

void dt_image_check_buffer(dt_image_t *image, dt_image_buffer_t mip, int32_t size)
{
#ifdef _DEBUG
  assert(image->mip_buf_size[mip] >= size);
#endif
}

int dt_image_alloc(dt_image_t *img, dt_image_buffer_t mip)
{
  int wd, ht;
  dt_image_get_mip_size(img, mip, &wd, &ht);
  size_t size = wd*ht;
  pthread_mutex_lock(&(darktable.mipmap_cache->mutex));
  void *ptr = NULL;
  if     ((int)mip <  (int)DT_IMAGE_MIPF) { size *= 4*sizeof(uint8_t); ptr = (void *)(img->mip[mip]); }
  else if(mip == DT_IMAGE_MIPF) { size *= 3*sizeof(float); ptr = (void *)(img->mipf); }
  else if(mip == DT_IMAGE_FULL) { size *= 3*sizeof(float); ptr = (void *)(img->pixels); }
  else
  {
    pthread_mutex_unlock(&(darktable.mipmap_cache->mutex));
    return 1;
  }
  if(ptr)
  {
    // dt_print(DT_DEBUG_CACHE, "[image_alloc] locking already allocated image %s\n", img->filename);
    img->lock[mip].write = 1; // write lock
    img->lock[mip].users = 1; // read lock
    pthread_mutex_unlock(&(darktable.mipmap_cache->mutex));
    return 0; // all good, already alloc'ed.
  }

  // printf("allocing %d x %d x %d for %s (%d)\n", wd, ht, size/(wd*ht), img->filename, mip);
  if     ((int)mip <  (int)DT_IMAGE_MIPF) { img->mip[mip] = (uint8_t *)malloc(size); }
  else if(mip == DT_IMAGE_MIPF) { img->mipf = (float *)malloc(size); }
  else if(mip == DT_IMAGE_FULL) { img->pixels = (float *)malloc(size); }

  if((mip == DT_IMAGE_FULL && img->pixels == NULL) || (mip == DT_IMAGE_MIPF && img->mipf == NULL) || ((int)mip <  (int)DT_IMAGE_MIPF && img->mip[mip] == NULL))
  {
    fprintf(stderr, "[image_alloc] malloc of %d x %d x %d for image %s mip %d failed!\n", wd, ht, (int)size/(wd*ht), img->filename, mip);
    pthread_mutex_unlock(&(darktable.mipmap_cache->mutex));
    return 1;
  }

  // dt_print(DT_DEBUG_CACHE, "[image_alloc] locking newly allocated image %s of size %f MB\n", img->filename, size/(1024*1024.0));

  // insert image in node list at newest time
  for(int k=0;k<darktable.mipmap_cache->num_entries[mip];k++)
  {
    if(darktable.mipmap_cache->mip_lru[mip][k] == NULL ||
      (darktable.mipmap_cache->mip_lru[mip][k]->lock[mip].users == 0 && darktable.mipmap_cache->mip_lru[mip][k]->lock[mip].write == 0))
    {
      dt_image_free(darktable.mipmap_cache->mip_lru[mip][k], mip);
      memmove(darktable.mipmap_cache->mip_lru[mip] + k, darktable.mipmap_cache->mip_lru[mip] + k + 1, (darktable.mipmap_cache->num_entries[mip] - k - 1)*sizeof(dt_image_t*));
      darktable.mipmap_cache->mip_lru[mip][darktable.mipmap_cache->num_entries[mip]-1] = img;
      img->lock[mip].write = 1; // write lock
      img->lock[mip].users = 1; // read lock
#ifdef _DEBUG
      img->mip_buf_size[mip] = size;
#if 0
      int allsize = 0;
      for(int k=0;k<darktable.mipmap_cache->num_entries[mip];k++) if(darktable.mipmap_cache->mip_lru[mip][k]) allsize += darktable.mipmap_cache->mip_lru[mip][k]->mip_buf_size[mip];
      printf("[image_alloc] alloc'ed additional %d bytes, now storing %f MB for mip %d\n", size, allsize/(1024.*1024.), mip);
#endif
#endif
      pthread_mutex_unlock(&(darktable.mipmap_cache->mutex));
      return 0;
    }
  }
  fprintf(stderr, "[image_alloc] all cache slots seem to be in use! alloc of %d bytes for img id %d mip %d failed!\n", (int)size, img->id, mip);
  pthread_mutex_unlock(&(darktable.mipmap_cache->mutex));
  return 1;
}

void dt_image_free(dt_image_t *img, dt_image_buffer_t mip)
{
  // mutex is already locked, as only alloc is allowed to free.
  if(!img) return;
  // dt_image_release(img, mip, "w");
  if((int)mip < (int)DT_IMAGE_MIPF) { free(img->mip[mip]); img->mip[mip] = NULL; }
  else if(mip == DT_IMAGE_MIPF) { free(img->mipf); img->mipf = NULL; }
  else if(mip == DT_IMAGE_FULL) { free(img->pixels); img->pixels = NULL; }
  else return;
  for(int k=0;k<darktable.mipmap_cache->num_entries[mip];k++)
    if(darktable.mipmap_cache->mip_lru[mip][k] == img) darktable.mipmap_cache->mip_lru[mip][k] = NULL;
#ifdef _DEBUG
  // printf("[image_free] freed %d bytes\n", img->mip_buf_size[mip]);
  if(darktable.control->running)
    assert(img->lock[mip].users == 0 && img->lock[mip].write == 0);
  img->mip_buf_size[mip] = 0;
#endif
}

int dt_image_lock_if_available(dt_image_t *img, const dt_image_buffer_t mip, const char mode)
{
  if(mip == DT_IMAGE_NONE) return 1;
  pthread_mutex_lock(&(darktable.mipmap_cache->mutex));
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
  if(ret == 0)
  {
    if(mode == 'w')
    {
      img->lock[mip].write = 1;
      img->lock[mip].users = 1;
    }
    else img->lock[mip].users++;
  }
  pthread_mutex_unlock(&(darktable.mipmap_cache->mutex));
  return ret;
}

dt_image_buffer_t dt_image_get_blocking(dt_image_t *img, const dt_image_buffer_t mip_in, const char mode)
{
  dt_image_buffer_t mip = mip_in;
  if(mip == DT_IMAGE_NONE) return mip;
  pthread_mutex_lock(&(darktable.mipmap_cache->mutex));
  // get image with no write lock set!
  if((int)mip < (int)DT_IMAGE_MIPF)
  {
    while(mip > 0 && (img->mip[mip] == NULL || img->lock[mip].write)) mip--; // level 0 always there.
  }
  else if(mip == DT_IMAGE_MIPF)
  {
    if(img->mipf == NULL || img->lock[mip].write) mip = DT_IMAGE_NONE;
  }
  else if(mip == DT_IMAGE_FULL)
  {
    if(img->pixels == NULL || img->lock[mip].write) mip = DT_IMAGE_NONE;
  }
  // found?
  if(mip == mip_in)
  {
    if(mode == 'w')
    {
      img->lock[mip].write = 1;
      img->lock[mip].users = 1;
    }
    img->lock[mip].users++;
    pthread_mutex_unlock(&(darktable.mipmap_cache->mutex));
    return mip;
  }
  // already loading? 
  if(img->lock[mip_in].write)
  {
    pthread_mutex_lock(&(darktable.mipmap_cache->mutex));
    return DT_IMAGE_NONE;
  }
  pthread_mutex_unlock(&(darktable.mipmap_cache->mutex));
 
  // dt_print(DT_DEBUG_CACHE, "[image_get_blocking] requested buffer %d, found %d for image %s locks: %d r %d w\n", mip_in, mip, img->filename, img->lock[mip].users, img->lock[mip].write);

  // start job to load this buf in bg.
  dt_print(DT_DEBUG_CACHE, "[image_get_blocking] reloading mip %d for image %d\n", mip_in, img->id);
  dt_image_load(img, mip_in); // this returns with 'r' locked
  mip = mip_in;

  pthread_mutex_lock(&(darktable.mipmap_cache->mutex));
  if(mip != DT_IMAGE_NONE)
  {
    if(mode == 'w')
    {
      img->lock[mip].write = 1;
      img->lock[mip].users = 1;
    }
    // else img->lock[mip].users++; // already incremented by image_load
  }
  pthread_mutex_unlock(&(darktable.mipmap_cache->mutex));
  return mip;
}

dt_image_buffer_t dt_image_get(dt_image_t *img, const dt_image_buffer_t mip_in, const char mode)
{
  dt_image_buffer_t mip = mip_in;
  if(mip == DT_IMAGE_NONE) return mip;
  pthread_mutex_lock(&(darktable.mipmap_cache->mutex));
  // get image with no write lock set!
  if((int)mip < (int)DT_IMAGE_MIPF)
  {
    while(mip > 0 && (img->mip[mip] == NULL || img->lock[mip].write)) mip--; // level 0 always there..?
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
  if(mip != DT_IMAGE_NONE)
  {
    if(mode == 'w')
    {
      img->lock[mip].write = 1;
      img->lock[mip].users = 1;
    }
    else img->lock[mip].users++;
  }

  // dt_print(DT_DEBUG_CACHE, "[image_get] requested buffer %d, found %d for image %s locks: %d r %d w\n", mip_in, mip, img->filename, img->lock[mip].users, img->lock[mip].write);

#if 1
  if(mip != mip_in) // && !img->lock[mip_in].write)
  { // start job to load this buf in bg.
    dt_print(DT_DEBUG_CACHE, "[image_get] reloading mip %d for image %d\n", mip_in, img->id);
    dt_job_t j;
    dt_image_load_job_init(&j, img->id, mip_in);
    // if the job already exists, make it high-priority:
    dt_control_revive_job(darktable.control, &j);
    if(!img->lock[mip_in].write)
    {
      img->lock[mip_in].write = 1;
      if(dt_control_add_job(darktable.control, &j))
        img->lock[mip_in].write = 0;
    }
    if(mip_in == DT_IMAGE_MIP4)
    {
      dt_job_t j;
      dt_image_load_job_init(&j, img->id, DT_IMAGE_MIPF);
      dt_control_revive_job(darktable.control, &j);
      if(!img->lock[DT_IMAGE_MIPF].write)
      { // start job to load this buf in bg.
        img->lock[DT_IMAGE_MIPF].write = 1;
        if(dt_control_add_job(darktable.control, &j))
          img->lock[DT_IMAGE_MIPF].write = 0;
      }
    }
  }
#endif
  pthread_mutex_unlock(&(darktable.mipmap_cache->mutex));
  return mip;
}

void dt_image_release(dt_image_t *img, dt_image_buffer_t mip, const char mode)
{
  pthread_mutex_lock(&(darktable.mipmap_cache->mutex));
  if (mode == 'r' && img->lock[mip].users > 0) img->lock[mip].users --;
  else if (mode == 'w') img->lock[mip].write = 0;  // can only be one writing thread at a time.
  pthread_mutex_unlock(&(darktable.mipmap_cache->mutex));
  // dt_print(DT_DEBUG_CACHE, "[image_release] released lock %c for buffer %d on image %s locks: %d r %d w\n", mode, mip, img->filename, img->lock[mip].users, img->lock[mip].write);
}

