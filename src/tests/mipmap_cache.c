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

void dt_mipmap_cache_init(dt_image_cache_t *cache)
{
  cache->cache = (dt_cache_t **)malloc(sizeof(dt_cache_t *)*NONE);
  for(int k=0;k<NONE;k++)
  {
    cache->cache[k] = (dt_cache_t *)malloc(sizeof(dt_cache_t));
    dt_cache_init(cache->cache[k]);
  }
}

void dt_mipmap_cache_cleanup(dt_image_cache_t *cache)
{
  for(int k=0;k<NONE;k++)
  {
    dt_cache_cleanup(cache->cache[k]);
    free(cache->cache[k]);
  }
  free(cache->cache);
}

void dt_mipmap_cache_print(dt_image_cache_t *cache)
{
}



