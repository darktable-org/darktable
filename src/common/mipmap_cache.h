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
#include "common/image.h"


// sizes stored in the mipmap cache.
// _4 can be a user-supplied size. down to _0,
// sizes are divided by two.
// so the range can be e.g. 1440..180 px.
// it does not make sense to cache larger thumbnails, so if more is
// wanted, a new pipe has to be initialized manually (e.g. in darkroom mode).
typedef enum dt_mipmap_size_t
{
  DT_MIPMAP_0    = 0,
  DT_MIPMAP_1    = 1,
  DT_MIPMAP_2    = 2,
  DT_MIPMAP_3    = 3,
  DT_MIPMAP_F    = 4,
  DT_MIPMAP_FULL = 5,
  DT_MIPMAP_NONE = 6
}
dt_mipmap_size_t;

// type to be passed to getter functions
typedef enum dt_mipmap_get_flags_t
{
  // gives you what you requested or a smaller mip,
  // or NULL if none could be found
  // also NULL is the fallback for _F and _FULL buffers.
  DT_MIPMAP_BEST_EFFORT = 0,
  // actually don't lock and return a buffer, but only
  // start a bg job to load it, if it's not in cache already.
  DT_MIPMAP_PREFETCH = 1,
  // only return when the requested buffer is loaded.
  // blocks until that happens.
  DT_MIPMAP_BLOCKING = 2,
  // don't actually acquire the lock if it is not
  // in cache (i.e. would have to be loaded first)
  DT_MIPMAP_TESTLOCK = 3
}
dt_mipmap_get_flags_t;

// struct to be alloc'ed by the client, filled by
// _get functions.
typedef struct dt_mipmap_buffer_t
{
  dt_mipmap_size_t size;
  uint32_t imgid;
  int32_t width, height;
  uint8_t *buf;
}
dt_mipmap_buffer_t;

typedef struct dt_mipmap_cache_one_t
{
  // this cache is for which mipmap type?
  dt_mipmap_size_t size;
  // real width and height are stored per element
  // (could be smaller than the max for this mip level,
  // due to aspect ratio)
  uint32_t max_width, max_height;
  // size of an element inside buf
  size_t buffer_size;
  // 1) no memory fragmentation:
  //    - fixed slots with fixed size (could waste a few bytes for extreme
  //      aspect ratios)
  // 2) dynamically grow? (linux alloc on write)
  // 3) downside: no crosstalk between mip0 and mip4

  // only stores 4*uint8_t per pixel for thumbnails:
  uint32_t *buf;

  // one cache per mipmap scale!
  dt_cache_t cache;

  // a few stats on usage in this run.
  // long int to give 32-bits on old archs, so __sync* calls will work.
  long int stats_requests;    // number of total requests
  long int stats_near_match;  // served with smaller mip res
  long int stats_misses;      // nothing returned at all.
  long int stats_fetches;     // texture was fetched (either as a stand-in or as per request)
  long int stats_standin;     // texture used as stand-in
}
dt_mipmap_cache_one_t;

typedef struct dt_mipmap_cache_t
{
  // one cache per mipmap level
  dt_mipmap_cache_one_t mip[DT_MIPMAP_NONE];
  // global setting: which compression type are we using?
  int compression_type; // 0 - none, 1 - low quality, 2 - slow
  // per-thread cache of uncompressed buffers, in case compression is requested.
  dt_mipmap_cache_one_t scratchmem;
}
dt_mipmap_cache_t;

typedef void** dt_mipmap_cache_allocator_t;
// dynamic memory allocation interface for imageio backend:
// the allocator is passed in, it might already contain a
// valid buffer. this function takes care of re-allocating,
// if necessary.
void*
dt_mipmap_cache_alloc(
  dt_image_t *img,
  dt_mipmap_size_t size,
  dt_mipmap_cache_allocator_t a);

void dt_mipmap_cache_init   (dt_mipmap_cache_t *cache);
void dt_mipmap_cache_cleanup(dt_mipmap_cache_t *cache);
void dt_mipmap_cache_print  (dt_mipmap_cache_t *cache);

// get a buffer for reading.
// see dt_mipmap_get_flags_t for explanation on the exact
// behaviour. pass 0 as flags for the default (best effort)
void
dt_mipmap_cache_read_get(
  dt_mipmap_cache_t *cache,
  dt_mipmap_buffer_t *buf,
  const uint32_t imgid,
  const dt_mipmap_size_t mip,
  const dt_mipmap_get_flags_t flags);

// lock it for writing. this is always blocking.
// requires you already hold a read lock.
void
dt_mipmap_cache_write_get(
  dt_mipmap_cache_t *cache,
  dt_mipmap_buffer_t *buf);

// drop a read lock
void
dt_mipmap_cache_read_release(
  dt_mipmap_cache_t *cache,
  dt_mipmap_buffer_t *buf);

// drop a write lock, read will still remain.
void
dt_mipmap_cache_write_release(
  dt_mipmap_cache_t *cache,
  dt_mipmap_buffer_t *buf);

// remove thumbnails, so they will be regenerated:
void
dt_mipmap_cache_remove(
  dt_mipmap_cache_t *cache,
  const uint32_t imgid);

// return the closest mipmap size
// for the given window you wish to draw.
// a dt_mipmap_size_t has always a fixed resolution associated with it,
// depending on the user parameter for the maximum thumbnail dimensions.
// actual resolution depends on the image and is only known after
// the thumbnail is loaded.
dt_mipmap_size_t
dt_mipmap_cache_get_matching_size(
  const dt_mipmap_cache_t *cache,
  const int32_t width,
  const int32_t height);


// allocate enough memory for an uncompressed thumbnail image.
// returns NULL if the cache is set to not use compression.
uint8_t*
dt_mipmap_cache_alloc_scratchmem(
  const dt_mipmap_cache_t *cache);

// decompress the raw mipmapm buffer into the scratchmemory.
// returns a pointer to the decompressed memory block. that's because
// for uncompressed settings, it will point directly to the mipmap
// buffer and scratchmem can be NULL.
uint8_t*
dt_mipmap_cache_decompress(
  const dt_mipmap_buffer_t *buf,
  uint8_t *scratchmem);

// writes the scratchmem buffer to compressed
// format into the mipmap cache. does nothing
// if compression is disabled.
void
dt_mipmap_cache_compress(
  dt_mipmap_buffer_t *buf,
  uint8_t *const scratchmem);

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
