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

#include "common/darktable.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/mipmap_cache.h"
#include "control/conf.h"
#include "control/jobs.h"

#include <assert.h>
#include <string.h>

static inline uint32_t
get_key(const uint32_t imgid, const dt_mipmap_size_t size)
{
  // imgid can't be >= 2^29 (~500 million images)
  return (((uint32_t)size) << 29) | imgid;
}

static inline uint32_t
get_imgid(const uint32_t key)
{
  return key & 0x1fffffff;
}

static inline dt_mipmap_size_t
get_size(const uint32_t key)
{
  return (dt_mipmap_size_t)(key >> 29);
}

// TODO: cache read/write functions!
// see old common/image_cache.c for backup file magic.

void
dt_mipmap_cache_allocate(void *data, const uint32_t key, int32_t *cost, void **buf)
{
  dt_mipmap_cache_one_t *c = (dt_mipmap_cache_one_t *)data;
  const uint32_t hash = key;
  // slot is exactly aligned with encapsulated cache's position
  const uint32_t slot = (hash & c->cache.bucket_mask);
  *cost = c->buffer_size;

  fprintf(stderr, "[mipmap cache alloc] slot %d/%d for imgid %d size %d buffer size %d\n", slot, c->cache.bucket_mask+1, get_imgid(key), get_size(key), c->buffer_size);
  // TODO: get image id and mip, initialize reload!
  // (in this thread. async loading works by effectively pushing this
  // callback into a worker thread)
  // TODO:
#if 0
// this should load and return with 'r' lock on mip buffer.
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
#endif
  

#if 1
  // FIXME: remove this debug thing (black thumbs)
  *buf = (uint8_t *)(c->buf + slot * (c->buffer_size/sizeof(uint32_t)));
  uint32_t *ibuf = (uint32_t *)*buf;
  memset(ibuf, 0, c->buffer_size);
  // set width and height:
  ibuf[0] = c->max_width;
  ibuf[1] = c->max_height;
  ibuf[2] = c->buffer_size;
#endif
}

#if 0
void
dt_mipmap_cache_deallocate(void *data, const uint32_t key, void *payload)
{
  // nothing. memory is only allocated once.
  // TODO: overwrite buffer with not-found image?
}
#endif


// callback for the imageio core to allocate memory.
// only needed for _F and _FULL buffers, as they change size
// with the input image. will allocate img->width*img->height*img->bpp bytes.
void*
dt_mipmap_cache_alloc(dt_image_t *img, dt_mipmap_size_t size, dt_mipmap_cache_allocator_t a)
{
  assert(size == DT_MIPMAP_FULL);

  const uint32_t buffer_size = 
      (img->width * img->height * img->bpp) +
      sizeof(float)*4; // padding for sse alignment.

  // buf might have been alloc'ed before,
  // so only check size and re-alloc if necessary:
  uint32_t **buf = (uint32_t **)a;
  if(*buf && (*buf[2] < buffer_size))
  {
    free(*buf);
    *buf = dt_alloc_align(64, buffer_size);
    // set buffer size only if we're making it larger.
    *buf[2] = buffer_size;
  }
  if(!*buf) return NULL;
  *buf[0] = img->width;
  *buf[1] = img->height;
  // possibly store some more flags in the remaining 32 bits, so clear it:
  *buf[3] = 0;
  // trick the user into using a pointer without the header:
  return *buf+4;
}

// callback for the cache backend to initialize payload pointers
void
dt_mipmap_cache_allocate_dynamic(void *data, const uint32_t key, int32_t *cost, void **buf)
{
  // for full image buffers
  const uint32_t imgid         = get_imgid(key);
  const dt_mipmap_size_t size  = get_size(key);

  fprintf(stderr, "[cache alloc dyn] called for image %u and mip %d\n", imgid, size);

  if(size == DT_MIPMAP_FULL)
  {
    // load the image:
    const dt_image_t *cimg = dt_image_cache_read_get(darktable.image_cache, imgid);
    dt_image_t *img = dt_image_cache_write_get(darktable.image_cache, cimg);
    char filename[DT_MAX_PATH_LEN];
    dt_image_full_path(img->id, filename, DT_MAX_PATH_LEN);
    dt_imageio_retval_t ret = dt_imageio_open(img, filename, (dt_mipmap_cache_allocator_t)buf);
    uint32_t *ibuf = *buf;
    if(ret != DT_IMAGEIO_OK)
    {
      // in case something went wrong, still keep the buffer and return it to the hashtable
      // so we don't produce mem leaks or unnecessary mem fragmentation.
      // 
      // but we can return a zero dimension buffer, so cache_read_get will return
      // an invalid buffer to the user:
      if(ibuf) ibuf[0] = ibuf[1] = 0;
    }
    // cost is always what we alloced in this realloc buffer, regardless of what the
    // image actually uses (could be less than this)
    if(ibuf)
      *cost = ibuf[2];
    else
      *cost = 0;
    // don't write xmp for this (we only changed db stuff):
    dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);
    dt_image_cache_read_release(darktable.image_cache, img);
    return;
  }
  else
  {
    printf("[mipmap_cache] trying to dynamically allocate an invalid mipmap size! (%d)\n", size);
    return;
  }
}

#if 0
void
dt_mipmap_cache_deallocate_dynamic(void *data, const uint32_t key, void *payload)
{
  // don't clean up anything, as we are re-allocating. 
}
#endif

void dt_mipmap_cache_init(dt_mipmap_cache_t *cache)
{
  // TODO: un-serialize!
  const int32_t max_th = 1000000, min_th = 20;
  const int32_t full_bufs = dt_conf_get_int ("mipmap_cache_full_images");
  int32_t thumbnails = dt_conf_get_int ("mipmap_cache_thumbnails");
  thumbnails = CLAMPS(thumbnails, min_th, max_th);
  const int32_t max_size = 2048, min_size = 32;
  // TODO: use these new user parameters! also in darkroom.c and develop.c
  int32_t wd = DT_IMAGE_WINDOW_SIZE;//dt_conf_get_int ("plugins/lighttable/thumbnail_width");
  int32_t ht = DT_IMAGE_WINDOW_SIZE;//dt_conf_get_int ("plugins/lighttable/thumbnail_height");
  wd = CLAMPS(wd, min_size, max_size);
  ht = CLAMPS(ht, min_size, max_size);
  // round up to a multiple of 8, so we can divide by two 3 times
  if(wd & 0xf) wd = (wd & ~0xf) + 0x10;
  if(ht & 0xf) ht = (ht & ~0xf) + 0x10;
  // cache these, can't change at runtime:
  cache->mip[DT_MIPMAP_F].max_width  = wd;
  cache->mip[DT_MIPMAP_F].max_height = ht;
  cache->mip[DT_MIPMAP_F-1].max_width  = wd;
  cache->mip[DT_MIPMAP_F-1].max_height = ht;
  for(int k=DT_MIPMAP_F-2;k>=DT_MIPMAP_0;k--)
  {
    cache->mip[k].max_width  = cache->mip[k+1].max_width  / 2;
    cache->mip[k].max_height = cache->mip[k+1].max_height / 2;
  }

  for(int k=0;k<=DT_MIPMAP_F;k++)
  {
    // only very few F buffers, but as many as full:
    if(k == DT_MIPMAP_F) thumbnails = full_bufs;
    dt_cache_init(&cache->mip[k].cache, thumbnails, 16, 64, 1);
    // might have been rounded to power of two:
    thumbnails = dt_cache_capacity(&cache->mip[k].cache);
    dt_cache_set_allocate_callback(&cache->mip[k].cache,
        dt_mipmap_cache_allocate, &cache->mip[k]);
    // dt_cache_set_cleanup_callback(&cache->mip[k].cache,
        // &dt_mipmap_cache_deallocate, &cache->mip[k]);
    // buffer stores width and height + actual data
    const int width  = cache->mip[k].max_width;
    const int height = cache->mip[k].max_height;
    if(k == DT_MIPMAP_F)
      cache->mip[k].buffer_size = (4 + 4 * width * height)*sizeof(float);
    else
      cache->mip[k].buffer_size = (4 + width * height)*sizeof(uint32_t);
    cache->mip[k].size = k;
    cache->mip[k].buf = dt_alloc_align(64, thumbnails * cache->mip[k].buffer_size);

    dt_print(DT_DEBUG_CACHE,
        "[mipmap_cache_init] cache has % 5d entries for mip %d (% 4.02f MB).\n",
        thumbnails, k, thumbnails * cache->mip[k].buffer_size/(1024.0*1024.0));

    thumbnails >>= 2;
    thumbnails = CLAMPS(thumbnails, min_th, max_th);
  }
  // full buffer needs dynamic alloc:
  dt_cache_init(&cache->mip[DT_MIPMAP_FULL].cache, full_bufs, 16, 64, 1);
  dt_cache_set_allocate_callback(&cache->mip[DT_MIPMAP_FULL].cache,
      dt_mipmap_cache_allocate_dynamic, &cache->mip[DT_MIPMAP_FULL]);
  // dt_cache_set_cleanup_callback(&cache->mip[DT_MIPMAP_FULL].cache,
      // &dt_mipmap_cache_deallocate_dynamic, &cache->mip[DT_MIPMAP_FULL]);
  cache->mip[DT_MIPMAP_FULL].buffer_size = 0;
  cache->mip[DT_MIPMAP_FULL].size = DT_MIPMAP_FULL;
  cache->mip[DT_MIPMAP_FULL].buf = NULL;
}

void dt_mipmap_cache_cleanup(dt_mipmap_cache_t *cache)
{
  // TODO: serialize
  for(int k=0;k<=DT_MIPMAP_F;k++)
  {
    dt_cache_cleanup(&cache->mip[k].cache);
    // now mem is actually freed, not during cache cleanup
    free(cache->mip[k].buf);
  }
  dt_cache_cleanup(&cache->mip[DT_MIPMAP_F].cache);
  dt_cache_cleanup(&cache->mip[DT_MIPMAP_FULL].cache);
}

void dt_mipmap_cache_print(dt_mipmap_cache_t *cache)
{
  for(int k=0; k<(int)DT_MIPMAP_NONE; k++)
  {
    printf("[mipmap_cache] level %d fill %.2f/%.2f MB (%.2f%%)\n", k, cache->mip[k].cache.cost/(1024.0*1024.0),
      cache->mip[k].cache.cost_quota/(1024.0*1024.0),
      100.0f*(float)cache->mip[k].cache.cost/(float)cache->mip[k].cache.cost_quota);
  }

  // TODO: stats about locks/users
}

void
dt_mipmap_cache_read_get(
    dt_mipmap_cache_t *cache,
    dt_mipmap_buffer_t *buf,
    const uint32_t imgid,
    const dt_mipmap_size_t mip,
    const dt_mipmap_get_flags_t flags)
{
  const uint32_t key = get_key(imgid, mip);
  if(flags == DT_MIPMAP_TESTLOCK)
  {
    // simple case: only get and lock if it's there.
    uint32_t *data = (uint32_t *)dt_cache_read_testget(&cache->mip[mip].cache, key);
    if(data)
    {
      buf->width  = data[0];
      buf->height = data[1];
      buf->imgid  = imgid;
      buf->size   = mip;
      // skip to next 8-byte alignment, for sse buffers.
      buf->buf    = (uint8_t *)(data + 4);
    }
    else
    {
      // set to NULL if failed.
      buf->width = buf->height = 0;
      buf->imgid = 0;
      buf->size  = DT_MIPMAP_NONE;
      buf->buf   = NULL;
    }
  }
  else if(flags == DT_MIPMAP_PREFETCH)
  {
    // and opposite: prefetch without locking
    if(mip > DT_MIPMAP_FULL || mip < DT_MIPMAP_0) return;
    dt_job_t j;
    dt_image_load_job_init(&j, imgid, mip);
    // if the job already exists, make it high-priority, if not, add it:
    if(dt_control_revive_job(darktable.control, &j) < 0)
      dt_control_add_job(darktable.control, &j);
  }
  else if(flags == DT_MIPMAP_BLOCKING)
  {
    // simple case: blocking get
    uint32_t *data = (uint32_t *)dt_cache_read_get(&cache->mip[mip].cache, key);
    if(!data)
    {
      // sorry guys, no image for you :(
      buf->width = buf->height = 0;
      buf->imgid = 0;
      buf->size  = DT_MIPMAP_NONE;
      buf->buf   = NULL;
    }
    else
    {
      buf->width  = data[0];
      buf->height = data[1];
      buf->imgid  = imgid;
      buf->size   = mip;
      buf->buf    = (uint8_t *)(data + 4);
    }
  }
  else if(flags == DT_MIPMAP_BEST_EFFORT)
  {
    // best-effort, might also return NULL.
    // never decrease mip level for float buffer or full image:
    dt_mipmap_size_t min_mip = (mip >= DT_MIPMAP_F) ? mip : DT_MIPMAP_0;
    for(int k=mip;k>=min_mip && k>=0;k--)
    {
      // already loaded?
      dt_mipmap_cache_read_get(cache, buf, imgid, k, DT_MIPMAP_TESTLOCK);
      if(buf->buf && buf->width > 0 && buf->height > 0) return;
      // didn't succeed the first time? prefetch for later!
      if(mip == k)
        dt_mipmap_cache_read_get(cache, buf, imgid, mip, DT_MIPMAP_PREFETCH);
    }
    // nothing found :(
    buf->buf   = NULL;
    buf->imgid = 0;
    buf->size  = DT_MIPMAP_NONE;
    buf->width = buf->height = 0;
  }
}

void
dt_mipmap_cache_write_get(
    dt_mipmap_cache_t *cache,
    dt_mipmap_buffer_t *buf)
{
  assert(buf->imgid > 0);
  assert(buf->size >= DT_MIPMAP_0);
  assert(buf->size <  DT_MIPMAP_NONE);
  // simple case: blocking write get
  uint32_t *data = (uint32_t *)dt_cache_write_get(&cache->mip[buf->size].cache, buf->imgid);
  buf->width  = data[0];
  buf->height = data[1];
  buf->buf    = (uint8_t *)(data + 4);
  // these have already been set in read_get
  // buf->imgid  = imgid;
  // buf->size   = mip;
}

void
dt_mipmap_cache_read_release(
    dt_mipmap_cache_t *cache,
    dt_mipmap_buffer_t *buf)
{
  if(buf->size == DT_MIPMAP_NONE || buf->buf == NULL) return;
  assert(buf->imgid > 0);
  assert(buf->size >= DT_MIPMAP_0);
  assert(buf->size <  DT_MIPMAP_NONE);
  dt_cache_read_release(&cache->mip[buf->size].cache, buf->imgid);
  buf->size = DT_MIPMAP_NONE;
  buf->buf  = NULL;
}

// drop a write lock, read will still remain.
void
dt_mipmap_cache_write_release(
    dt_mipmap_cache_t *cache,
    dt_mipmap_buffer_t *buf)
{
  if(buf->size == DT_MIPMAP_NONE || buf->buf == NULL) return;
  assert(buf->imgid > 0);
  assert(buf->size >= DT_MIPMAP_0);
  assert(buf->size <  DT_MIPMAP_NONE);
  dt_cache_write_release(&cache->mip[buf->size].cache, buf->imgid);
  buf->size = DT_MIPMAP_NONE;
  buf->buf  = NULL;
}



// return the closest mipmap size
dt_mipmap_size_t
dt_mipmap_cache_get_matching_size(
    const dt_mipmap_cache_t *cache,
    const int32_t width,
    const int32_t height)
{
  // find `best' match to width and height.
  uint32_t error = 0xffffffff;
  dt_mipmap_size_t best = DT_MIPMAP_NONE;
  for(int k=DT_MIPMAP_0;k<DT_MIPMAP_F;k++)
  {
    uint32_t new_error = abs(cache->mip[k].max_width + cache->mip[k].max_height
                       - width - height);
    if(new_error < error)
    {
      best = k;
      error = new_error;
    }
  }
  return best;
}

void
dt_mipmap_cache_remove(
    dt_mipmap_cache_t *cache,
    const uint32_t imgid)
{
  // get rid of all ldr thumbnails:
  for(int k=DT_MIPMAP_0;k<DT_MIPMAP_F;k++)
  {
    const uint32_t key = get_key(imgid, k);
    dt_cache_remove(&cache->mip[k].cache, key);
  }
}


// *************************************************************
// TODO: some glue code down here. should be transparently
//       incorporated into the rest.
// *************************************************************

#if 0
// TODO: rewrite with new interface, move to imageio?
// this is called by the innermost core functions when loading a raw preview
// because the full buffer needs to be freed right away!
// also if a full raw is loaded, the cache did it so far.
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
#endif



// currently not needed for debugging, as mips are inited with black dummies.
#if 0



// TODO: all the locking into the cache directly
// TOD: can the locking be done by finding the 'w' lock on the buffer?
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

// TODO: get rid of it:
int dt_image_reimport(dt_image_t *img, const char *filename, dt_image_buffer_t mip)
{
  // TODO: first get the 'w' lock on the mip buffer/and test it
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
    // TODO: dev->image = ?
    // void dt_dev_load_image(dt_develop_t *dev, dt_image_t *image)
    // replace this with code below:
    // and make it work with full buf instead!
    // dt_dev_load_preview(&dev, img);
    // dt_dev_process_to_mip(&dev);
    dt_dev_cleanup(&dev);
    // load preview keeps a lock on mipf:
    dt_image_release(img, DT_IMAGE_MIPF, 'r');
  }
  dt_image_import_unlock(img);
  return 0;
}

// FIXME: make sure this doesn't subsample the mips as grossly as
//        it does currently. it should start from the full raw
//        in case output width/h are much smaller than the input.
// process preview to gain ldr-mipmaps:
void dt_dev_process_to_mip(dt_develop_t *dev)
{
  // TODO: efficiency: check hash on preview_pipe->backbuf
  if(dt_image_get_blocking(dev->image, DT_IMAGE_MIPF, 'r') != DT_IMAGE_MIPF)
  {
    fprintf(stderr, "[dev_process_to_mip] no float buffer is available yet!\n");
    return; // not loaded yet.
  }

  if(!dev->preview_pipe)
  {
    // init pixel pipeline for preview.
    dev->preview_pipe = (dt_dev_pixelpipe_t *)malloc(sizeof(dt_dev_pixelpipe_t));
    dt_dev_pixelpipe_init(dev->preview_pipe);
    dt_image_get_mip_size(dev->image, DT_IMAGE_MIPF, &dev->mipf_width, &dev->mipf_height);
    dt_image_get_exact_mip_size(dev->image, DT_IMAGE_MIPF, &dev->mipf_exact_width, &dev->mipf_exact_height);
    dt_dev_pixelpipe_set_input(dev->preview_pipe, dev, dev->image->mipf, dev->mipf_width, dev->mipf_height, dev->image->width/(float)dev->mipf_width);
    dt_dev_pixelpipe_cleanup_nodes(dev->preview_pipe);
    dt_dev_pixelpipe_create_nodes(dev->preview_pipe, dev);
    dev->preview_loading = 0;
  }

  int wd, ht;
  float fwd, fht;

  dev->preview_downsampling = 1.0;
  dev->preview_pipe->changed |= DT_DEV_PIPE_SYNCH;
  dt_dev_process_preview_job(dev);

  // now the real wd/ht is available.
  dt_dev_get_processed_size(dev, &dev->image->output_width, &dev->image->output_height);
  dt_image_get_mip_size(dev->image, DT_IMAGE_MIP4, &wd, &ht);
  dt_image_get_exact_mip_size(dev->image, DT_IMAGE_MIP4, &fwd, &fht);

  if(dt_image_alloc(dev->image, DT_IMAGE_MIP4))
  {
    fprintf(stderr, "[dev_process_to_mip] could not alloc mip4 to write mipmaps!\n");
    dt_image_release(dev->image, DT_IMAGE_MIPF, 'r');
    return;
  }

  dt_image_check_buffer(dev->image, DT_IMAGE_MIP4, sizeof(uint8_t)*4*wd*ht);
  dt_pthread_mutex_lock(&(dev->preview_pipe->backbuf_mutex));

  // don't if processing failed and backbuf's not there.
  if(dev->preview_pipe->backbuf)
  {
    dt_iop_clip_and_zoom_8(dev->preview_pipe->backbuf, 0, 0, dev->preview_pipe->backbuf_width, dev->preview_pipe->backbuf_height,
                           dev->preview_pipe->backbuf_width, dev->preview_pipe->backbuf_height,
                           dev->image->mip[DT_IMAGE_MIP4], 0, 0, fwd, fht, wd, ht);

  }
  dt_image_release(dev->image, DT_IMAGE_MIP4, 'w');
  dt_pthread_mutex_unlock(&(dev->preview_pipe->backbuf_mutex));

  dt_image_update_mipmaps(dev->image);

  dt_image_cache_flush(dev->image); // write new output size to db.
  dt_image_release(dev->image, DT_IMAGE_MIP4, 'r');
  dt_image_release(dev->image, DT_IMAGE_MIPF, 'r');

  /* raise signal that mipmaps has been flushed to cache */
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_DEVELOP_MIPMAP_UPDATED);
}


// TODO: this should never have to be called explicitly
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
#endif


