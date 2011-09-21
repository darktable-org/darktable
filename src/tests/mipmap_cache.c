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
#include "control/conf.h"

static inline uint32_t
get_key(const uint32_t imgid, const dt_mipmap_size_t size)
{
  // imgid can't be >= 2^29 (~500 million images)
  return (size << 29) | imgid;
}

static inline uint32_t
get_imgid(const uint32_t key)
{
  return key & 0x1fffffff;
}

static inline dt_mipmap_size_t
get_size(const uint32_t key)
{
  return key >> 29;
}

void*
dt_mipmap_cache_allocate(void *data, const uint32_t key, int32_t *cost)
{
  dt_mipmap_cache_one_t *c = (dt_mipmap_cache_one_t *)data;
  const uint32_t hash = key;
  const uint32_t slot = hash & c->cache->bucket_mask;
  *cost = c->buffer_size;
  return c->buf + slot * c->buffer_size;
}
void
dt_mipmap_cache_cleanup(void *data, const uint32_t key, void *payload)
{
  // nothing. memory is only allocated once.
}

void*
dt_mipmap_cache_allocate_dynamic(void *data, const uint32_t key, int32_t *cost)
{
  // for float preview and full image buffers
  dt_mipmap_cache_one_t *c     = (dt_mipmap_cache_one_t *)data;
  const uint32_t imgid         = get_imgid(key);
  const dt_mipmap_size_t size  = get_size(key);
  const dt_image_t *dt_image_cache_read_get(darktable.image_cache, (int32_t)imgid);
  const uint32_t bpp = img->bpp;
  const uint32_t buffer_size = (size == DT_MIPMAP_FULL) ?
                               (img->width * img->height * bpp) :
                               (DT_IMAGE_WIDOW_SIZE * DT_IMAGE_WINDOW_SIZE * 4*sizeof(float));
  dt_image_cache_read_release(darktable.image_cache, img);
  *cost = buffer_size;
  return dt_alloc_align(64, buffer_size);
}
void
dt_mipmap_cache_cleanup_dynamic(void *data, const uint32_t key, void *payload)
{
  free(payload);
}

void dt_mipmap_cache_init(dt_mipmap_cache_t *cache)
{
  const int32_t max_th = 1000000, min_th = 20;
  int32_t thumbnails = dt_conf_get_int ("mipmap_cache_thumbnails");
  thumbnails = CLAMPS(thumbnails, min_th, max_th);
  const int32_t max_size = 2048, min_size = 32;
  int32_t width  = dt_conf_get_int ("plugins/lighttable/thumbnail_width");
  int32_t height = dt_conf_get_int ("plugins/lighttable/thumbnail_height");
  width  = CLAMPS(width,  min_size, max_size);
  height = CLAMPS(height, min_size, max_size);

  for(int k=0;k<DT_MIPMAP_F;k++)
  {
    dt_cache_init(&cache->mip[k].cache, thumbnails, 16, 64, 1);
    dt_cache_set_allocate_callback(&cache->mip[k].cache,
        &dt_mipmap_cache_allocate, &cache->mip[k].cache);
    dt_cache_set_cleanup_callback(&cache->mip[k].cache,
        &dt_mipmap_cache_cleanup, &cache->mip[k].cache);
    // buffer stores width and height + actual data
    cache->mip[k].buffer_size = (2 + width * height);
    cache->mip[k].size = k;
    cache->mip[k].buf = dt_alloc_align(64, thumbnails * cache->mip[k].buffer_size*sizeof(uint32_t));
    thumbnails >>= 2;
    thumbnails = CLAMPS(thumbnails, min_th, max_th);
  }
  // full buffer count for these:
  int32_t full_bufs = dt_conf_get_int ("mipmap_cache_full_images");
  dt_cache_init(&cache->mip[DT_MIPMAP_F].cache, full_bufs, 16, 64, 1);
  dt_cache_set_allocate_callback(&cache->mip[DT_MIPMAP_F].cache,
      &dt_mipmap_cache_allocate_dynamic, &cache->mip[DT_MIPMAP_F].cache);
  dt_cache_set_cleanup_callback(&cache->mip[DT_MIPMAP_F].cache,
      &dt_mipmap_cache_cleanup_dynamic, &cache->mip[DT_MIPMAP_F].cache);
  cache->mip[DT_MIPMAP_F].buffer_size = 0;
  cache->mip[DT_MIPMAP_F].buffer_cnt  = 0;
  cache->mip[DT_MIPMAP_F].size = DT_MIPMAP_F;
  cache->mip[DT_MIPMAP_F].buf = NULL;

  dt_cache_init(&cache->mip[DT_MIPMAP_FULL].cache, full_bufs, 16, 64, 1);
  dt_cache_set_allocate_callback(&cache->mip[DT_MIPMAP_FULL].cache,
      &dt_mipmap_cache_allocate_dynamic, &cache->mip[DT_MIPMAP_FULL].cache);
  dt_cache_set_cleanup_callback(&cache->mip[DT_MIPMAP_FULL].cache,
      &dt_mipmap_cache_cleanup_dynamic, &cache->mip[DT_MIPMAP_FULL].cache);
  cache->mip[DT_MIPMAP_FULL].buffer_size = 0;
  cache->mip[DT_MIPMAP_FULL].buffer_cnt  = 0;
  cache->mip[DT_MIPMAP_FULL].size = DT_MIPMAP_FULL;
  cache->mip[DT_MIPMAP_FULL].buf = NULL;
}

void dt_mipmap_cache_cleanup(dt_mipmap_cache_t *cache)
{
  for(int k=0;k<DT_MIPMAP_F;k++)
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
  // TODO: get cache cost:
}

const dt_mipmap_buffer_t*
dt_mipmap_cache_lock_if_available(dt_mipmap_cache_t *cache, const uint32_t key, dt_mipmap_size_t mip)
{
  // TODO:
}

const dt_mipmap_buffer_t*
dt_mipmap_cache_read_get(dt_cache_image_t *cache, const uint32_t key, dt_mipmap_size_t mip)
{
  // TODO: if _F or _FULL!

  // best-effort, might also return NULL.
  for(int k=mip;k>DT_MIPMAP_0;k--)
  {
    // TODO: query cache->cache[k]
    // TODO: 
  }
  return NULL;
}


