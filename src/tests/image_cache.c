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

void dt_image_cache_init   (dt_image_cache_t *cache);
void dt_image_cache_cleanup(dt_image_cache_t *cache);


void*
dt_image_cache_allocate(void *data, const uint32_t key, int32_t *cost)
{
  // TODO: check cost and keep it below 80%!
  // TODO: *cost = 1; ?
  // TODO: if key = 0 or -1: insert dummy into sql
  // TODO: get the image struct from sql.
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
// drops the write priviledges on an image struct.
// thtis triggers a write-through to sql, and if the setting
// is present, also to xmp sidecar files (safe setting).
void              dt_image_cache_write_release(dt_cache_image_t *cache, dt_image_t *img);

