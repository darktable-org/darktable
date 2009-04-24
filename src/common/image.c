
#include "common/darktable.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "control/control.h"
#include "control/jobs.h"
#include <math.h>
#include <sqlite3.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <assert.h>

// how large would the average screen be (largest mip map size) ?
#define DT_IMAGE_WINDOW_SIZE 1200

dt_image_buffer_t dt_image_get_matching_mip_size(const dt_image_t *img, const int32_t width, const int32_t height, int32_t *w, int32_t *h)
{
  const float scale = fminf(DT_IMAGE_WINDOW_SIZE/(float)(img->width), DT_IMAGE_WINDOW_SIZE/(float)(img->height));
  int32_t wd = MIN(img->width, (int)(scale*img->width)), ht = MIN(img->height, (int)(scale*img->height));
  if(wd & 0xf) wd = (wd & ~0xf) + 0x10;
  if(ht & 0xf) ht = (ht & ~0xf) + 0x10;
  dt_image_buffer_t mip = DT_IMAGE_MIP4;
  while((int)mip > (int)DT_IMAGE_MIP0 && wd > width && ht > height)
  {
    mip--;
    if(wd > 32 && ht > 32)
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
  float wd = img->width, ht = img->height;
  if((int)mip < (int)DT_IMAGE_FULL)
  {
    const float scale = fminf(1.0, fminf(DT_IMAGE_WINDOW_SIZE/(float)img->width, DT_IMAGE_WINDOW_SIZE/(float)img->height));
    wd *= scale; ht *= scale;
    while((int)mip < (int)DT_IMAGE_MIP4)
    {
      mip++;
      if(wd > 32 && ht > 32)
      { // only if it's not vanishing completely :)
        wd *= .5;
        ht *= .5;
      }
    }
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

int dt_image_raw_to_preview(dt_image_t *img)
{
  const int raw_wd = img->width >> img->shrink;
  const int raw_ht = img->height >> img->shrink;
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

  if(dt_image_alloc(img, DT_IMAGE_MIP4))
  {
    dt_image_release(img, DT_IMAGE_MIPF, 'w');
    dt_image_release(img, DT_IMAGE_MIPF, 'r');
    return 1;
  }
  dt_image_check_buffer(img, DT_IMAGE_MIP4, 4*p_wd*p_ht*sizeof(uint8_t));
  dt_image_check_buffer(img, DT_IMAGE_MIPF, 3*p_wd*p_ht*sizeof(float));
  int ret = 0;
  dt_imageio_preview_f_to_8(p_wd, p_ht, img->mipf, img->mip[DT_IMAGE_MIP4]);
  dt_imageio_preview_write(img, DT_IMAGE_MIP4);
  if(dt_image_update_mipmaps(img)) ret = 1;
  dt_image_release(img, DT_IMAGE_MIPF, 'w');
  dt_image_release(img, DT_IMAGE_MIPF, 'r');
  dt_image_release(img, DT_IMAGE_MIP4, 'w');
  dt_image_release(img, DT_IMAGE_MIP4, 'r');
  return ret;
}

int dt_image_import(const int32_t film_id, const char *filename)
{
  int rc;
  int ret = 0, id = -1;
  // select from images; if found => return
  gchar *imgfname = g_path_get_basename((const gchar*)filename);
  sqlite3_stmt *stmt;
  rc = sqlite3_prepare_v2(darktable.db, "select id from images where film_id = ?1 and filename = ?2", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, film_id);
  rc = sqlite3_bind_text(stmt, 2, imgfname, strlen(imgfname), SQLITE_STATIC);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    id = sqlite3_column_int(stmt, 0);
    g_free(imgfname);
    rc = sqlite3_finalize(stmt);
    return dt_image_open(id); // image already in db, open this.
  }
  rc = sqlite3_finalize(stmt);

  // insert dummy image entry in database
  rc = sqlite3_prepare_v2(darktable.db, "insert into images (id, film_id, filename) values (null, -1, ?1)", -1, &stmt, NULL);
  HANDLE_SQLITE_ERR(rc);
  rc = sqlite3_bind_text(stmt, 1, imgfname, strlen(imgfname), SQLITE_STATIC);
  pthread_mutex_lock(&(darktable.db_insert));
  rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) fprintf(stderr, "sqlite3 error %d\n", rc);
  id = sqlite3_last_insert_rowid(darktable.db);
  pthread_mutex_unlock(&(darktable.db_insert));
  rc = sqlite3_finalize(stmt);

  // insert dummy (zeroblob) entries in database!
  for(dt_image_buffer_t mip=DT_IMAGE_MIP0;(int)mip<=(int)DT_IMAGE_MIPF;mip++)
  {
    rc = sqlite3_prepare_v2(darktable.db, "insert into mipmaps (imgid, level) values (?1, ?2)", -1, &stmt, NULL);
    rc = sqlite3_bind_int(stmt, 1, id);
    rc = sqlite3_bind_int(stmt, 2, mip);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) fprintf(stderr, "[image_import] could not insert mipmap %d for image %d: %s\n", mip, id, sqlite3_errmsg(darktable.db));
    rc = sqlite3_finalize(stmt);
  }

  dt_image_t *img = dt_image_cache_use(id, 'w');
  strncpy(img->filename, imgfname, 256);
  g_free(imgfname);

  // load small raw (try libraw then magick)
  img->shrink = 1;
  if(dt_imageio_open(img, filename))
  {
    dt_image_cleanup(img);
    fprintf(stderr, "[image_import] could not open %s\n", filename);
    dt_image_cache_release(img, 'w');
    rc = sqlite3_prepare_v2(darktable.db, "delete from images where id = ?1", -1, &stmt, NULL);
    rc = sqlite3_bind_int (stmt, 1, id);
    rc = sqlite3_step(stmt);
    rc = sqlite3_prepare_v2(darktable.db, "delete from mipmaps where imgid = ?1", -1, &stmt, NULL);
    rc = sqlite3_bind_int (stmt, 1, id);
    rc = sqlite3_step(stmt);
    return 1;
  }

  // update image data
  rc = sqlite3_prepare_v2(darktable.db, "update images set width = ?1, height = ?2, maker = ?3, model = ?4, exposure = ?5, aperture = ?6, iso = ?7, focal_length = ?8, film_id = ?9, datetime_taken = ?10 where id = ?11", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, img->width);
  rc = sqlite3_bind_int (stmt, 2, img->height);
  rc = sqlite3_bind_text(stmt, 3, img->exif_maker, strlen(img->exif_maker), SQLITE_STATIC);
  rc = sqlite3_bind_text(stmt, 4, img->exif_model, strlen(img->exif_model), SQLITE_STATIC);
  rc = sqlite3_bind_double(stmt, 5, img->exif_exposure);
  rc = sqlite3_bind_double(stmt, 6, img->exif_aperture);
  rc = sqlite3_bind_double(stmt, 7, img->exif_iso);
  rc = sqlite3_bind_double(stmt, 8, img->exif_focal_length);
  rc = sqlite3_bind_int (stmt, 9, film_id);
  rc = sqlite3_bind_text(stmt, 10, img->exif_datetime_taken, strlen(img->exif_datetime_taken), SQLITE_STATIC);
  rc = sqlite3_bind_int (stmt, 11, img->id);
  rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) fprintf(stderr, "sqlite3 error %d\n", rc);
  rc = sqlite3_finalize(stmt);

  // create preview images
  if(dt_image_raw_to_preview(img)) ret = 2;
  dt_image_release(img, DT_IMAGE_FULL, 'r');
  dt_image_cache_release(img, 'w');
  return ret;
}

int dt_image_update_mipmaps(dt_image_t *img)
{
  int oldwd, oldht;
  dt_image_get_mip_size(img, DT_IMAGE_MIP4, &oldwd, &oldht);
  // create 8-bit mip maps:
  for(dt_image_buffer_t l=DT_IMAGE_MIP3;(int)l>=(int)DT_IMAGE_MIP0;l--)
  {
    int p_wd, p_ht;
    dt_image_get_mip_size(img, l, &p_wd, &p_ht);
    if(dt_image_alloc(img, l)) return 1;

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
  img->width = img->height = 0;
  img->mipf = NULL;
  img->pixels = NULL;
  img->orientation = 0;
  img->exposure = 0;
  img->wb_auto = img->wb_cam = 1;
  img->shrink = 0;
  img->film_id = -1;
  img->flags = 0;
  img->id = -1;
  img->cacheline = -1;
  strncpy(img->exif_model, "unknown\0", 20);
  strncpy(img->exif_maker, "unknown\0", 20);
  strncpy(img->exif_datetime_taken, "0000:00:00 00:00:00\0", 20);
  img->exif_exposure = img->exif_aperture = img->exif_iso = img->exif_focal_length = 0;
#ifdef _DEBUG
  for(int k=0;(int)k<(int)DT_IMAGE_NONE;k++) img->mip_buf_size[k] = 0;
#endif
}

int dt_image_open(const int32_t id)
{
  dt_image_t *img = dt_image_cache_use(id, 'w');
  int rc = dt_image_open2(img, id);
  dt_image_cache_release(img, 'w');
  return rc;
}

int dt_image_open2(dt_image_t *img, const int32_t id)
{ // load stuff from db and store in cache:
  int rc, ret = 1;
  sqlite3_stmt *stmt;
  rc = sqlite3_prepare_v2(darktable.db, "select id, film_id, width, height, filename, maker, model, exposure, aperture, iso, focal_length, datetime_taken from images where id = ?1", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, id);
  // rc = sqlite3_bind_text(stmt, 2, img->filename, strlen(img->filename), SQLITE_STATIC);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    img->id      = sqlite3_column_int(stmt, 0);
    img->film_id = sqlite3_column_int(stmt, 1);
    img->width   = sqlite3_column_int(stmt, 2);
    img->height  = sqlite3_column_int(stmt, 3);
    strncpy(img->filename,   (char *)sqlite3_column_text(stmt, 4), 512);
    strncpy(img->exif_maker, (char *)sqlite3_column_text(stmt, 5), 20);
    strncpy(img->exif_model, (char *)sqlite3_column_text(stmt, 6), 20);
    img->exif_exposure = sqlite3_column_double(stmt, 7);
    img->exif_aperture = sqlite3_column_double(stmt, 8);
    img->exif_iso = sqlite3_column_double(stmt, 9);
    img->exif_focal_length = sqlite3_column_double(stmt, 10);
    strncpy(img->exif_datetime_taken, (char *)sqlite3_column_text(stmt, 11), 20);
    
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
  int ret = 1, rc;
  if(mip == DT_IMAGE_FULL)
  {
    char filename[1024];
    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(darktable.db, "select folder from film_rolls where id = ?1", -1, &stmt, NULL);
    rc = sqlite3_bind_int (stmt, 1, img->film_id);
    if(sqlite3_step(stmt) == SQLITE_ROW)
      snprintf(filename, 1024, "%s/%s", sqlite3_column_text(stmt, 0), img->filename);
    rc = sqlite3_finalize(stmt);
    ret = dt_imageio_open(img, filename);
    if(ret) dt_image_cleanup(img);
  }
  else ret = dt_imageio_preview_read(img, mip);
  // TODO: insert abstract hook here?
  dt_control_queue_draw();
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
  for(int k=0;k<(int)DT_IMAGE_NONE;k++)
  {
    int users = 0, write = 0, entries = 0;
    for(int i=0;i<cache->num_entries[k];i++)
    {
      if(cache->mip_lru[k][i])
      {
        entries++;
        users += cache->mip_lru[k][i]->lock[k].users;
        write += cache->mip_lru[k][i]->lock[k].write;
      }
    }
    printf("mip %d: fill: %d/%d, users: %d, writers: %d\n", k, entries, cache->num_entries[k], users, write);
  }
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

  // insert image in node list at newest time
  for(int k=0;k<darktable.mipmap_cache->num_entries[mip];k++)
  {
    if(darktable.mipmap_cache->mip_lru[mip][k] == NULL || darktable.mipmap_cache->mip_lru[mip][k]->lock[mip].users == 0)
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

dt_image_buffer_t dt_image_get(dt_image_t *img, const dt_image_buffer_t mip_in, const char mode)
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
  if(mip != DT_IMAGE_NONE)
  {
    if(mode == 'w')
    {
      img->lock[mip].write = 1;
      img->lock[mip].users = 1;
    }
    else img->lock[mip].users++;
  }

  if(mip != mip_in && !img->lock[mip_in].write)
  { // start job to load this buf in bg.
    dt_print(DT_DEBUG_CACHE, "[image_get] reloading mip %d for image %d\n", mip_in, img->id);
    img->lock[mip_in].write = 1;
    dt_job_t j;
    dt_image_load_job_init(&j, img, mip_in);
    dt_control_add_job(darktable.control, &j);
    if(mip_in == DT_IMAGE_MIP4)
    {
      if(!img->lock[DT_IMAGE_MIPF].write)
      { // start job to load this buf in bg.
        img->lock[DT_IMAGE_MIPF].write = 1;
        dt_job_t j;
        dt_image_load_job_init(&j, img, DT_IMAGE_MIPF);
        dt_control_add_job(darktable.control, &j);
      }
    }
  }
  pthread_mutex_unlock(&(darktable.mipmap_cache->mutex));
  return mip;
}

void dt_image_release(dt_image_t *img, dt_image_buffer_t mip, const char mode)
{
  pthread_mutex_lock(&(darktable.mipmap_cache->mutex));
  if (mode == 'r' && img->lock[mip].users > 0) img->lock[mip].users --;
  else if (mode == 'w') img->lock[mip].write = 0;  // can only be one writing thread at a time.
  pthread_mutex_unlock(&(darktable.mipmap_cache->mutex));
}

