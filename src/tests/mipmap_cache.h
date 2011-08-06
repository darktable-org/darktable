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
#ifndef DT_MIPMAP_CACHE_H
#define DT_MIPMAP_CACHE_H

#include "common/cache.h"

typedef struct dt_mipmap_cache_t
{
  // TODO: use this cache to dynamically allocate mipmap buffers
  //       (so it will grow slowly)
  // TODO: one cache per mipmap scale!
  // TODO: implement our own garbage collection to free large buffers first!
  // TODO: need clever hashing img->mip (not just id..?)
  // TODO: say folder id + img filename hash
  dt_cache_t *cache;
}
dt_mipmap_cache_t;

void dt_mipmap_cache_init   (dt_image_cache_t *cache);
void dt_mipmap_cache_cleanup(dt_image_cache_t *cache);
void dt_mipmap_cache_print  (dt_image_cache_t *cache);



#endif
