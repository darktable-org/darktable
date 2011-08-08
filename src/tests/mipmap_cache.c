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

void*
dt_mipmap_cache_allocate(void *data, const uint32_t key, int32_t *cost)
{
  // TODO: get mipmap level and dt_mipmap_cache_t from *data
  // TODO: extract image id from key
  // TODO: 
}
void*
dt_mipmap_cache_cleanup(void *data, const uint32_t key, void *payload)
{
}

void dt_mipmap_cache_init(dt_mipmap_cache_t *cache)
{
  const int32_t max_th = 100000, min_th = 20;
  int32_t thumbnails = dt_conf_get_int ("mipmap_cache_thumbnails");
  thumbnails = CLAMPS(thumbnails, min_th, max_th);

  for(int k=0;k<NONE;k++)
  {
    dt_cache_init(&cache->cache[k], thumbnails, 16, 64, 1);
    thumbnails >>= 2;
    thumbnails = CLAMPS(thumbnails, min_th, max_th);
  }
}

void dt_mipmap_cache_cleanup(dt_mipmap_cache_t *cache)
{
  for(int k=0;k<DT_MIPMAP_NONE;k++)
  {
    dt_cache_cleanup(&cache->cache[k]);
  }
}

void dt_mipmap_cache_print(dt_mipmap_cache_t *cache)
{
}

const dt_mipmap_buffer_t*
dt_mipmap_cache_lock_if_available(dt_mipmap_cache_t *cache, const uint32_t key, dt_mipmap_size_t mip)
{
  // TODO:
}

const dt_mipmap_buffer_t*
dt_mipmap_cache_read_get(dt_cache_image_t *cache, const uint32_t key, dt_mipmap_size_t mip)
{
  // best-effort, might also return NULL.
  for(int k=mip;k>DT_MIPMAP_0;k--)
  {
    // TODO: query cache->cache[k]
    // TODO: 
  }
  return NULL;
}


