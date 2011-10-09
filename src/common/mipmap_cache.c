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
#include "common/imageio_module.h"
#include "common/imageio_jpeg.h"
#include "common/mipmap_cache.h"
#include "control/conf.h"
#include "control/jobs.h"

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/fcntl.h>
#include <glib/gstdio.h>
#include <errno.h>

#define DT_MIPMAP_CACHE_FILE_MAGIC 0xD71337
#define DT_MIPMAP_CACHE_FILE_VERSION 20
#define DT_MIPMAP_CACHE_FILE_NAME "mipmaps"

static inline int32_t
buffer_is_broken(dt_mipmap_buffer_t *buf)
{
  if(!buf->buf) return 0;
  uint32_t *b = (uint32_t *)(buf->buf);
  if(buf->width  != b[-4]) return 1;
  if(buf->height != b[-3]) return 2;
  // somewhat loose bound:
  if(buf->width*buf->height > b[-2]) return 3;
  return 0;
}

static inline uint32_t
get_key(const uint32_t imgid, const dt_mipmap_size_t size)
{
  // imgid can't be >= 2^29 (~500 million images)
  return (((uint32_t)size) << 29) | (imgid-1);
}

static inline uint32_t
get_imgid(const uint32_t key)
{
  return (key & 0x1fffffff) + 1;
}

static inline dt_mipmap_size_t
get_size(const uint32_t key)
{
  return (dt_mipmap_size_t)(key >> 29);
}

// TODO: cache read/write functions!
// see old common/image_cache.c for backup file magic.

typedef struct _iterate_data_t
{
  FILE *f;
  uint8_t *blob;
}
_iterate_data_t;

static int
_write_buffer(const uint32_t key, const void *data, void *user_data)
{
  _iterate_data_t *d = (_iterate_data_t *)user_data;
  int written = fwrite(&key, sizeof(uint32_t), 1, d->f);
  if(written != 1) return 1;

  dt_mipmap_buffer_t buf;
  if(!data) return 1;
  buf.width  = ((uint32_t *)data)[0];
  buf.height = ((uint32_t *)data)[1];
  buf.imgid  = get_imgid(key);
  buf.size   = get_size(key);
  // skip to next 8-byte alignment, for sse buffers.
  buf.buf    = (uint8_t *)(&((uint32_t *)data)[4]);

  const int32_t length = dt_imageio_jpeg_compress(buf.buf, d->blob, buf.width, buf.height, MIN(100, MAX(10, dt_conf_get_int("database_cache_quality"))));
  written = fwrite(&length, sizeof(int32_t), 1, d->f);
  if(written != 1) return 1;
  written = fwrite(d->blob, sizeof(uint8_t), length, d->f);
  if(written != length) return 1;

  fprintf(stderr, "[mipmap_cache] serializing image %u (%d x %d) with %d bytes\n", get_imgid(key), buf.width, buf.height, length);

  return 0;
}

static int
dt_mipmap_cache_serialize(dt_mipmap_cache_t *cache)
{
  char cachedir[1024];
  char dbfilename[1024];
  dt_util_get_user_cache_dir(cachedir,1024);
  gchar *filename = dt_conf_get_string("cachefile");

  if(!filename || filename[0] == '\0') snprintf(dbfilename, 512, "%s/%s", cachedir, DT_MIPMAP_CACHE_FILE_NAME);
  else if(filename[0] != '/')          snprintf(dbfilename, 512, "%s/%s", cachedir, filename);
  else                                 snprintf(dbfilename, 512, "%s", filename);
  g_free(filename);

  // only store smallest thumbs.
  const dt_mipmap_size_t mip = DT_MIPMAP_0;

  _iterate_data_t d;
  d.f = NULL;
  d.blob = (uint8_t *)malloc(cache->mip[mip].buffer_size);
  int written = 0;
  FILE *f = fopen(dbfilename, "wb");
  if(!f) goto write_error;
  d.f = f;
  fprintf(stderr, "[mipmap_cache] serializing to `%s'\n", dbfilename);

  // write version info:
  const int32_t magic = DT_MIPMAP_CACHE_FILE_MAGIC + DT_MIPMAP_CACHE_FILE_VERSION;
  written = fwrite(&magic, sizeof(int32_t), 1, f);
  if(written != 1) goto write_error;

  // print max sizes for this cache
  written = fwrite(&cache->mip[mip].max_width, sizeof(int32_t), 1, f);
  if(written != 1) goto write_error;
  written = fwrite(&cache->mip[mip].max_height, sizeof(int32_t), 1, f);
  if(written != 1) goto write_error;

  if(dt_cache_for_all(&cache->mip[mip].cache, _write_buffer, &d)) goto write_error;

  free(d.blob);
  fclose(f);
  return 0;

write_error:
  fprintf(stderr, "[mipmap_cache] serialization to `%s' failed!\n", dbfilename);
  if(f) fclose(f);
  free(d.blob);
  return 1;
}

static int
dt_mipmap_cache_deserialize(dt_mipmap_cache_t *cache)
{
  // FIXME: currently broken:
  return 1;
  uint8_t *blob = NULL;
  int32_t rd = 0;
  const dt_mipmap_size_t mip = DT_MIPMAP_0;

  char cachedir[1024];
  char dbfilename[1024];
  dt_util_get_user_cache_dir(cachedir, 1024);
  gchar *filename = dt_conf_get_string ("cachefile");
  if(!filename || filename[0] == '\0') snprintf (dbfilename, 512, "%s/%s", cachedir, DT_MIPMAP_CACHE_FILE_NAME);
  else if(filename[0] != '/')          snprintf (dbfilename, 512, "%s/%s", cachedir, filename);
  else                                 snprintf (dbfilename, 512, "%s", filename);
  g_free(filename);

  FILE *f = fopen(dbfilename, "rb");
  if(!f) 
  {
    if (errno == ENOENT)
    {
      fprintf(stderr, "[mipmap_cache] cache is empty, file `%s' doesn't exist\n", dbfilename);
    }
    else
    {
      fprintf(stderr, "[mipmap_cache] failed to open the cache from `%s'\n", dbfilename);
    }
    goto read_finalize;
  }

  // read version info:
  const int32_t magic = DT_MIPMAP_CACHE_FILE_MAGIC + DT_MIPMAP_CACHE_FILE_VERSION;
  int32_t magic_file = 0;
  rd = fread(&magic_file, sizeof(int32_t), 1, f);
  if(rd != 1) goto read_error;
  if(magic_file != magic)
  {
    if(magic_file > DT_MIPMAP_CACHE_FILE_MAGIC && magic_file < magic)
        fprintf(stderr, "[mipmap_cache] cache version too old, dropping `%s' cache\n", dbfilename);
    else
        fprintf(stderr, "[mipmap_cache] invalid cache file, dropping `%s' cache\n", dbfilename);
    goto read_finalize;
  }
  int file_width = 0, file_height = 0;
  rd = fread(&file_width, sizeof(int32_t), 1, f);
  if(rd != 1) goto read_error;
  rd = fread(&file_height, sizeof(int32_t), 1, f);
  if(rd != 1) goto read_error;
  if(file_width  != cache->mip[mip].max_width ||
     file_height != cache->mip[mip].max_height)
  {
    fprintf(stderr, "[mipmap_cache] cache settings changed, dropping `%s' cache\n", dbfilename);
    goto read_finalize;
  }
  blob = (uint8_t *)malloc(4*sizeof(uint8_t)*file_width*file_height);

  while(!feof(f))
  {
    int32_t key = 0;
    rd = fread(&key, sizeof(int32_t), 1, f);
    if(rd != 1) goto read_error;
    fprintf(stderr, "[mipmap_cache] thumbnail for image %d\n", get_imgid(key));
    // FIXME: need to read w/h here, of the actual thumbnail
    int32_t length = 0;
    rd = fread(&length, sizeof(int32_t), 1, f);
    fprintf(stderr, "[mipmap_cache] thumbnail for image %d length %d bytes\n", get_imgid(key), length);
    if(rd != 1 || length > 4*sizeof(uint8_t)*file_width*file_height);
      goto read_error;
    rd = fread(blob, sizeof(uint8_t), length, f);
    if(rd != length) goto read_error;

    dt_mipmap_buffer_t buf;
    dt_imageio_jpeg_t jpg;
    fprintf(stderr, "[mipmap_cache] thumbnail for image %d\n", get_imgid(key));

    // FIXME: this won't work, as it will be reading stuff from disk!
    // TODO:  use low level cache interface!
    dt_mipmap_cache_read_get(cache, &buf, get_imgid(key), get_size(key), DT_MIPMAP_BLOCKING);
    if(!buf.buf) goto read_error;
    dt_mipmap_cache_write_get(cache, &buf);
    fprintf(stderr, "[mipmap_cache] thumbnail for image %d\n", get_imgid(key));

    if(dt_imageio_jpeg_decompress_header(blob, length, &jpg) ||
        (jpg.width != file_width|| jpg.height != file_height) ||
        dt_imageio_jpeg_decompress(&jpg, buf.buf))
    {
      fprintf(stderr, "[mipmap_cache] failed to decompress thumbnail for image %d!\n", get_imgid(key));
    }
    dt_mipmap_cache_write_release(cache, &buf);
    dt_mipmap_cache_read_release(cache, &buf);
    fprintf(stderr, "[mipmap_cache] decompressed thumbnail for image %d!\n", get_imgid(key));
  }

  fclose(f);
  free(blob);
  return 0;

read_error:
  fprintf(stderr, "[mipmap_cache] failed to recover the cache from `%s'\n", dbfilename);
read_finalize:
  if(f) fclose(f);
  free(blob);
  g_unlink(dbfilename);
  return 1;
}

static void _init_f(float   *buf, uint32_t *width, uint32_t *height, const uint32_t imgid);
static void _init_8(uint8_t *buf, uint32_t *width, uint32_t *height, const uint32_t imgid, const dt_mipmap_size_t size);

int32_t
dt_mipmap_cache_allocate(void *data, const uint32_t key, int32_t *cost, void **buf)
{
  dt_mipmap_cache_one_t *c = (dt_mipmap_cache_one_t *)data;
  const uint32_t hash = key;
  // slot is exactly aligned with encapsulated cache's position
  const uint32_t slot = (hash & c->cache.bucket_mask);
  *cost = c->buffer_size;

  *buf = c->buf + slot * (c->buffer_size/sizeof(uint32_t));
  uint32_t *ibuf = (uint32_t *)*buf;
  // set width and height:
  ibuf[0] = c->max_width;
  ibuf[1] = c->max_height;
  ibuf[2] = c->buffer_size;
  ibuf[3] = 1; // mark as not initialized yet

  // fprintf(stderr, "[mipmap cache alloc] slot %d/%d for imgid %d size %d buffer size %d (%lX)\n", slot, c->cache.bucket_mask+1, get_imgid(key), get_size(key), c->buffer_size, (uint64_t)*buf);
  return 1;
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
  if(!(*buf) || ((*buf)[2] < buffer_size))
  {
    free(*buf);
    *buf = dt_alloc_align(64, buffer_size);
    if(!(*buf)) return NULL;
    // set buffer size only if we're making it larger.
    (*buf)[2] = buffer_size;
  }
  (*buf)[0] = img->width;
  (*buf)[1] = img->height;
  (*buf)[3] = 1; // mark as not initialized yet

  // fprintf(stderr, "full buffer allocating img %u %d x %d = %u bytes (%lX)\n", img->id, img->width, img->height, buffer_size, (uint64_t)*buf);

  // trick the user into using a pointer without the header:
  return (*buf)+4;
}

// callback for the cache backend to initialize payload pointers
int32_t
dt_mipmap_cache_allocate_dynamic(void *data, const uint32_t key, int32_t *cost, void **buf)
{
  // for full image buffers
  // FIXME: this is meaningless at this point!
  // TODO:  pimp cache_realloc function to take another cost
  // cost is always what we alloced in this realloc buffer, regardless of what the
  // image actually uses (could be less than this)
  *cost = 1;//data[2];

  uint32_t *ibuf = *buf;
  // alloc mere minimum for the header:
  if(!ibuf)
  {
    *buf = dt_alloc_align(64, 4*sizeof(uint32_t));
    ibuf = *buf;
    ibuf[0] = ibuf[1] = 0;
    ibuf[2] = 4*sizeof(uint32_t);
  }
  ibuf[3] = 1; // mark as not initialized yet
  // fprintf(stderr, "dummy allocing %lX\n", (uint64_t)*buf);
  return 1; // request write lock
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
  // FIXME: adjust numbers to be large enough to hold what mem limit suggests!
  const uint32_t max_mem = 100*1024*1024;
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
    // buffer stores width and height + actual data
    const int width  = cache->mip[k].max_width;
    const int height = cache->mip[k].max_height;
    if(k == DT_MIPMAP_F)
      cache->mip[k].buffer_size = (4 + 4 * width * height)*sizeof(float);
    else
      cache->mip[k].buffer_size = (4 + width * height)*sizeof(uint32_t);
    cache->mip[k].size = k;
    uint32_t thumbnails = (uint32_t)(1.2f * max_mem/cache->mip[k].buffer_size);

    dt_cache_init(&cache->mip[k].cache, thumbnails, 8, 64, 100*1024*1024);
    // might have been rounded to power of two:
    thumbnails = dt_cache_capacity(&cache->mip[k].cache);
    dt_cache_set_allocate_callback(&cache->mip[k].cache,
        dt_mipmap_cache_allocate, &cache->mip[k]);
    // dt_cache_set_cleanup_callback(&cache->mip[k].cache,
        // &dt_mipmap_cache_deallocate, &cache->mip[k]);

    cache->mip[k].buf = dt_alloc_align(64, thumbnails * cache->mip[k].buffer_size);

    dt_print(DT_DEBUG_CACHE,
        "[mipmap_cache_init] cache has % 5d entries for mip %d (% 4.02f MB).\n",
        thumbnails, k, thumbnails * cache->mip[k].buffer_size/(1024.0*1024.0));
  }
  // full buffer needs dynamic alloc:
  const uint32_t full_bufs = 3;
  dt_cache_init(&cache->mip[DT_MIPMAP_FULL].cache, 1.2*full_bufs, 2, 64, full_bufs);//500*1024*1024);
  dt_cache_set_allocate_callback(&cache->mip[DT_MIPMAP_FULL].cache,
      dt_mipmap_cache_allocate_dynamic, &cache->mip[DT_MIPMAP_FULL]);
  // dt_cache_set_cleanup_callback(&cache->mip[DT_MIPMAP_FULL].cache,
      // &dt_mipmap_cache_deallocate_dynamic, &cache->mip[DT_MIPMAP_FULL]);
  cache->mip[DT_MIPMAP_FULL].buffer_size = 0;
  cache->mip[DT_MIPMAP_FULL].size = DT_MIPMAP_FULL;
  cache->mip[DT_MIPMAP_FULL].buf = NULL;

  dt_mipmap_cache_deserialize(cache);
}

void dt_mipmap_cache_cleanup(dt_mipmap_cache_t *cache)
{
  dt_mipmap_cache_serialize(cache);
  for(int k=0;k<=DT_MIPMAP_F;k++)
  {
    dt_cache_cleanup(&cache->mip[k].cache);
    // now mem is actually freed, not during cache cleanup
    free(cache->mip[k].buf);
  }
  dt_cache_cleanup(&cache->mip[DT_MIPMAP_FULL].cache);
}

void dt_mipmap_cache_print(dt_mipmap_cache_t *cache)
{
  for(int k=0; k<(int)DT_MIPMAP_FULL; k++)
  {
    printf("[mipmap_cache] level %d fill %.2f/%.2f MB (%.2f%% in %u/%u buffers)\n", k, cache->mip[k].cache.cost/(1024.0*1024.0),
      cache->mip[k].cache.cost_quota/(1024.0*1024.0),
      100.0f*(float)cache->mip[k].cache.cost/(float)cache->mip[k].cache.cost_quota,
      dt_cache_size(&cache->mip[k].cache),
      dt_cache_capacity(&cache->mip[k].cache));
  }
  printf("[mipmap_cache] level %d fill %d/%d (%.2f%% in %u/%u buffers)\n", DT_MIPMAP_FULL, cache->mip[DT_MIPMAP_FULL].cache.cost,
    cache->mip[DT_MIPMAP_FULL].cache.cost_quota,
    100.0f*(float)cache->mip[DT_MIPMAP_FULL].cache.cost/(float)cache->mip[DT_MIPMAP_FULL].cache.cost_quota,
    dt_cache_size(&cache->mip[DT_MIPMAP_FULL].cache),
    dt_cache_capacity(&cache->mip[DT_MIPMAP_FULL].cache));

  // very verbose stats about locks/users
  dt_cache_print(&cache->mip[DT_MIPMAP_FULL].cache);
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
      buf->buf    = (uint8_t *)(&data[4]);
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
      // fprintf(stderr, "[mipmap cache get] no data in cache for imgid %u size %d!\n", imgid, mip);
      // sorry guys, no image for you :(
      buf->width = buf->height = 0;
      buf->imgid = 0;
      buf->size  = DT_MIPMAP_NONE;
      buf->buf   = NULL;
    }
    else
    {
      // fprintf(stderr, "[mipmap cache get] found data in cache for imgid %u size %d (%lX)\n", imgid, mip, (uint64_t)data);
      // uninitialized?
      assert(data[3] == 1 || data[3] == 0);
      if(data[3] == 1)
      {
        // fprintf(stderr, "[mipmap cache get] now initializing buffer for img %u mip %d!\n", imgid, mip);
        // we're write locked here, as requested by the alloc callback.
        // now fill it with data:
        if(mip == DT_MIPMAP_FULL)
        {
          // load the image:
          // make sure we access the r/w lock a shortly as possible!
          dt_image_t buffered_image;
          const dt_image_t *cimg = dt_image_cache_read_get(darktable.image_cache, imgid);
          buffered_image = *cimg;
          // dt_image_t *img = dt_image_cache_write_get(darktable.image_cache, cimg);
          // dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);
          dt_image_cache_read_release(darktable.image_cache, cimg);
          char filename[DT_MAX_PATH_LEN];
          dt_image_full_path(buffered_image.id, filename, DT_MAX_PATH_LEN);
          dt_mipmap_cache_allocator_t a = (dt_mipmap_cache_allocator_t)&data;
          dt_imageio_retval_t ret = dt_imageio_open(&buffered_image, filename, a);
          // write back to cache, too.
          data = *(uint32_t **)a;
          dt_cache_realloc(&cache->mip[mip].cache, key, (void *)data);
          if(ret != DT_IMAGEIO_OK)
          {
            // fprintf(stderr, "[mipmap read get] error loading image: %d\n", ret);
            // in case something went wrong, still keep the buffer and return it to the hashtable
            // so we don't produce mem leaks or unnecessary mem fragmentation.
            // 
            // but we can return a zero dimension buffer, so cache_read_get will return
            // an invalid buffer to the user:
            if(data) data[0] = data[1] = 0;
          }
          else
          {
            // swap back new image data:
            cimg = dt_image_cache_read_get(darktable.image_cache, imgid);
            dt_image_t *img = dt_image_cache_write_get(darktable.image_cache, cimg);
            *img = buffered_image;
            // fprintf(stderr, "[mipmap read get] initializing full buffer img %u with %u %u -> %d %d (%lX)\n", imgid, data[0], data[1], img->width, img->height, (uint64_t)data);
            // don't write xmp for this (we only changed db stuff):
            dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);
            dt_image_cache_read_release(darktable.image_cache, img);
          }
        }
        else if(mip == DT_MIPMAP_F)
          _init_f((float *)&data[4], data+0, data+1, imgid);
        else
          _init_8((uint8_t *)&data[4], data+0, data+1, imgid, mip);
        data[3] = 0;
        // drop the write lock
        dt_cache_write_release(&cache->mip[mip].cache, key);
        /* raise signal that mipmaps has been flushed to cache */
        dt_control_signal_raise(darktable.signals, DT_SIGNAL_DEVELOP_MIPMAP_UPDATED);
      }
      buf->width  = data[0];
      buf->height = data[1];
      buf->imgid  = imgid;
      buf->size   = mip;
      if(data[0] && data[1])
        buf->buf = (uint8_t *)(&data[4]);
      else
      {
        // fprintf(stderr, "[mipmap cache get] got a zero-sized image for img %u mip %d (%lX)!\n", imgid, mip, (uint64_t)data);
        buf->buf = NULL;
        dt_cache_read_release(&cache->mip[mip].cache, key);
      }
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
    // fprintf(stderr, "[mipmap cache get] image not found in cache: imgid %u mip %d!\n", imgid, mip);
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
  uint32_t *data = (uint32_t *)dt_cache_write_get(&cache->mip[buf->size].cache, get_key(buf->imgid, buf->size));
  buf->width  = data[0];
  buf->height = data[1];
  buf->buf    = (uint8_t *)(&data[4]);
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
  dt_cache_read_release(&cache->mip[buf->size].cache, get_key(buf->imgid, buf->size));
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
  dt_cache_write_release(&cache->mip[buf->size].cache, get_key(buf->imgid, buf->size));
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

static void
_init_f(
    float          *out,
    uint32_t       *width,
    uint32_t       *height,
    const uint32_t  imgid)
{
  const uint32_t wd = *width, ht = *height;

  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_read_get(darktable.mipmap_cache, &buf, imgid, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING);

  // lock image after we have the buffer, we might need to lock the image struct for
  // writing during raw loading, to write to width/height.
  const dt_image_t *image = dt_image_cache_read_get(darktable.image_cache, imgid);

  dt_iop_roi_t roi_in, roi_out;
  roi_in.x = roi_in.y = 0;
  roi_in.width = image->width;
  roi_in.height = image->height;
  roi_in.scale = 1.0f;
  
  roi_out.x = roi_out.y = 0;
  roi_out.scale = fminf(wd/(float)image->width, ht/(float)image->height);
  roi_out.width  = roi_out.scale * roi_in.width;
  roi_out.height = roi_out.scale * roi_in.height;

  if(!buf.buf)
  {
    dt_control_log(_("image `%s' is not available!"), image->filename);
    dt_image_cache_read_release(darktable.image_cache, image);
    *width = *height = 0;
    return;
  }

  assert(!buffer_is_broken(&buf));

  if(image->filters)
  {
    // demosaic during downsample
    if(image->bpp == sizeof(float))
      dt_iop_clip_and_zoom_demosaic_half_size_f(
          out, (const float *)buf.buf,
          &roi_out, &roi_in, roi_out.width, roi_in.width,
          dt_image_flipped_filter(image));
    else
      dt_iop_clip_and_zoom_demosaic_half_size(
          out, (const uint16_t *)buf.buf,
          &roi_out, &roi_in, roi_out.width, roi_in.width,
          dt_image_flipped_filter(image));
  }
  else
  {
    // downsample
    dt_iop_clip_and_zoom(out, (const float *)buf.buf,
          &roi_out, &roi_in, roi_out.width, roi_in.width);
  }
  dt_image_cache_read_release(darktable.image_cache, image);
  dt_mipmap_cache_read_release(darktable.mipmap_cache, &buf);

  *width  = roi_out.width;
  *height = roi_out.height;
}


// dummy functions for `export' to mipmap buffers:
typedef struct _dummy_data_t
{
  dt_imageio_module_data_t head;
  uint8_t *buf;
}
_dummy_data_t;

static int
_bpp(dt_imageio_module_data_t *data)
{
  return 8;
}

static int
_write_image(
    dt_imageio_module_data_t *data,
    const char               *filename,
    const void               *in,
    void                     *exif,
    int                       exif_len,
    int                       imgid)
{
  _dummy_data_t *d = (_dummy_data_t *)data;
  memcpy(d->buf, in, data->width*data->height*sizeof(uint32_t));
  return 0;
}

static void 
_init_8(
    uint8_t                *buf,
    uint32_t               *width,
    uint32_t               *height,
    const uint32_t          imgid,
    const dt_mipmap_size_t  size)
{
  const uint32_t wd = *width, ht = *height;
  dt_imageio_module_format_t format;
  _dummy_data_t dat;
  format.bpp = _bpp;
  format.write_image = _write_image;
  dat.head.max_width  = wd;
  dat.head.max_height = ht;
  dat.buf = buf;
  // export with flags: ignore exif (don't load from disk), don't swap byte order, and don't do hq processing
  int res = dt_imageio_export_with_flags(imgid, "unused", &format, (dt_imageio_module_data_t *)&dat, 1, 1, 0);

  // fprintf(stderr, "[mipmap init 8] export finished (sizes %d %d => %d %d)!\n", wd, ht, dat.head.width, dat.head.height);

  // any errors?
  if(res)
  {
    fprintf(stderr, "[mipmap init 8] could not process mipmap thumbnail!\n");
    *width = *height = 0;
    return;
  }

  // might be smaller, or have a different aspect than what we got as input.
  *width  = dat.head.width;
  *height = dat.head.height;

  // TODO: various speed optimizations:
  // TODO: get mip from larger mip (testget)!
  // TODO: also init all smaller mips!
  // TODO: use mipf, but:
  // TODO: if output is cropped, don't use mipf!
}

// *************************************************************
// TODO: some glue code down here. should be transparently
//       incorporated into the rest.
// *************************************************************


#if 0
// old code to resample mip maps
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


