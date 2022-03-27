/*
    This file is part of darktable,
    Copyright (C) 2011-2021 darktable developers.

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

#pragma once

#include "common/cache.h"
#include "common/colorspaces.h"
#include "common/image.h"

// sizes stored in the mipmap cache, set to fixed values in mipmap_cache.c
typedef enum dt_mipmap_size_t {
  DT_MIPMAP_0 = 0,
  DT_MIPMAP_1,
  DT_MIPMAP_2,
  DT_MIPMAP_3,
  DT_MIPMAP_4,
  DT_MIPMAP_5,
  DT_MIPMAP_6,
  DT_MIPMAP_7,
  DT_MIPMAP_8,
  DT_MIPMAP_F,
  DT_MIPMAP_FULL,
  DT_MIPMAP_NONE
} dt_mipmap_size_t;

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
  // similar to prefetching, but only prefetch in case
  // we hit the disk cache (don't run the more expensive pipeline)
  DT_MIPMAP_PREFETCH_DISK = 2,
  // only return when the requested buffer is loaded.
  // blocks until that happens.
  DT_MIPMAP_BLOCKING = 3,
  // don't actually acquire the lock if it is not
  // in cache (i.e. would have to be loaded first)
  DT_MIPMAP_TESTLOCK = 4
} dt_mipmap_get_flags_t;

// struct to be alloc'ed by the client, filled by dt_mipmap_cache_get()
typedef struct dt_mipmap_buffer_t
{
  dt_mipmap_size_t size;
  uint32_t imgid;
  int32_t width, height;
  float iscale;
  uint8_t *buf;
  dt_colorspaces_color_profile_type_t color_space;
  dt_cache_entry_t *cache_entry;
} dt_mipmap_buffer_t;

typedef struct dt_mipmap_cache_one_t
{
  // one cache per mipmap scale!
  dt_cache_t cache;

  // a few stats on usage in this run.
  // long int to give 32-bits on old archs, so __sync* calls will work.
  long int stats_requests;   // number of total requests
  long int stats_near_match; // served with smaller mip res
  long int stats_misses;     // nothing returned at all.
  long int stats_fetches;    // texture was fetched (either as a stand-in or as per request)
  long int stats_standin;    // texture used as stand-in
} dt_mipmap_cache_one_t;

typedef struct dt_mipmap_cache_t
{
  // real width and height are stored per element
  // (could be smaller than the max for this mip level,
  // due to aspect ratio)
  uint32_t max_width[DT_MIPMAP_NONE], max_height[DT_MIPMAP_NONE];
  // size of an element inside buf
  size_t buffer_size[DT_MIPMAP_NONE];

  // one cache per mipmap level
  dt_mipmap_cache_one_t mip_thumbs;
  dt_mipmap_cache_one_t mip_f;
  dt_mipmap_cache_one_t mip_full;
  char cachedir[PATH_MAX]; // cached sha1sum filename for faster access
} dt_mipmap_cache_t;

// dynamic memory allocation interface for imageio backend: a write locked
// mipmap buffer is passed in, it might already contain a valid buffer. this
// function takes care of re-allocating, if necessary.
void *dt_mipmap_cache_alloc(dt_mipmap_buffer_t *buf, const dt_image_t *img);

void dt_mipmap_cache_init(dt_mipmap_cache_t *cache);
void dt_mipmap_cache_cleanup(dt_mipmap_cache_t *cache);
void dt_mipmap_cache_print(dt_mipmap_cache_t *cache);

// get a buffer and lock according to mode ('r' or 'w').
// see dt_mipmap_get_flags_t for explanation of the exact
// behaviour. pass 0 as flags for the default (best effort)
#define dt_mipmap_cache_get(A,B,C,D,E,F) dt_mipmap_cache_get_with_caller(A,B,C,D,E,F,__FILE__,__LINE__)
void dt_mipmap_cache_get_with_caller(
    dt_mipmap_cache_t *cache,
    dt_mipmap_buffer_t *buf,
    const uint32_t imgid,
    const dt_mipmap_size_t mip,
    const dt_mipmap_get_flags_t flags,
    const char mode,
    const char *file,
    int line);

// convenience function with fewer params
#define dt_mipmap_cache_write_get(A,B,C,D) dt_mipmap_cache_write_get_with_caller(A,B,C,D,__FILE__,__LINE__)
void dt_mipmap_cache_write_get_with_caller(
    dt_mipmap_cache_t *cache,
    dt_mipmap_buffer_t *buf,
    const uint32_t imgid,
    const int mip,
    const char *file,
    int line);

// drop a lock
#define dt_mipmap_cache_release(A, B) dt_mipmap_cache_release_with_caller(A, B, __FILE__, __LINE__)
void dt_mipmap_cache_release_with_caller(dt_mipmap_cache_t *cache, dt_mipmap_buffer_t *buf, const char *file,
                                         int line);

// remove thumbnails, so they will be regenerated:
void dt_mipmap_cache_remove(dt_mipmap_cache_t *cache, const uint32_t imgid);
void dt_mipmap_cache_remove_at_size(dt_mipmap_cache_t *cache, const uint32_t imgid, const dt_mipmap_size_t mip);

// evict thumbnails from cache. They will be written to disc if not existing
void dt_mimap_cache_evict(dt_mipmap_cache_t *cache, const uint32_t imgid);
void dt_mipmap_cache_evict_at_size(dt_mipmap_cache_t *cache, const uint32_t imgid, const dt_mipmap_size_t mip);

// return the closest mipmap size
// for the given window you wish to draw.
// a dt_mipmap_size_t has always a fixed resolution associated with it,
// depending on the user parameter for the maximum thumbnail dimensions.
// actual resolution depends on the image and is only known after
// the thumbnail is loaded.
dt_mipmap_size_t dt_mipmap_cache_get_matching_size(
    const dt_mipmap_cache_t *cache,
    const int32_t width,
    const int32_t height);

// returns the colorspace to use for created thumbnails, takes config into account
dt_colorspaces_color_profile_type_t dt_mipmap_cache_get_colorspace();

// copy over thumbnails. used by file operation that copies raw files, to speed up thumbnail generation.
// only copies over the jpg backend on disk, doesn't directly affect the in-memory cache.
void dt_mipmap_cache_copy_thumbnails(const dt_mipmap_cache_t *cache, const uint32_t dst_imgid, const uint32_t src_imgid);

// return the mipmap corresponding to text value saved in prefs
dt_mipmap_size_t dt_mipmap_cache_get_min_mip_from_pref(const char *value);
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

