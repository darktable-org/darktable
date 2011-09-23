/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#ifndef DT_IMAGE_H
#define DT_IMAGE_H
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib.h>
#include <inttypes.h>
#include "common/dtpthread.h"

// how large would the average screen be (largest mip map size) ?
// this is able to develop images on a 1920 monitor (-2*300 - 20 for the panels).
#define DT_IMAGE_WINDOW_SIZE 1300

/** define for max path/filename length */
#define DT_MAX_PATH 1024

/** return value of image io functions. */
typedef enum dt_imageio_retval_t
{
  DT_IMAGEIO_OK = 0,          // all good :)
  DT_IMAGEIO_FILE_NOT_FOUND,  // file has been lost
  DT_IMAGEIO_FILE_CORRUPTED,  // file contains garbage
  DT_IMAGEIO_CACHE_FULL       // dt's caches are full :(
}
dt_imageio_retval_t;

typedef enum
{
  // the first 0x7 in flags are reserved for star ratings.
  DT_IMAGE_DELETE = 1,
  DT_IMAGE_OKAY = 2,
  DT_IMAGE_NICE = 3,
  DT_IMAGE_EXCELLENT = 4,
  // this refers to the state of the mipf buffer and its source.
  DT_IMAGE_THUMBNAIL = 16,
  // set during import if the image is low-dynamic range, i.e. doesn't need demosaic, wb, highlight clipping etc.
  DT_IMAGE_LDR = 32,
  // set during import if the image is raw data, i.e. it needs demosaicing.
  DT_IMAGE_RAW = 64,
  // set during import if images is a high-dynamic range image..
  DT_IMAGE_HDR = 128,
  // set when marked for deletion
  DT_IMAGE_REMOVE = 256
}
dt_image_flags_t;

typedef enum
{
  DT_IMAGE_MIP0 = 0,
  DT_IMAGE_MIP1 = 1,
  DT_IMAGE_MIP2 = 2,
  DT_IMAGE_MIP3 = 3,
  DT_IMAGE_MIP4 = 4,
  DT_IMAGE_MIPF = 5,
  DT_IMAGE_FULL = 6,
  DT_IMAGE_NONE = 7
}
dt_image_buffer_t;

typedef struct dt_image_lock_t
{
  unsigned write : 1;
  unsigned users : 7;
}
dt_image_lock_t;

typedef struct dt_image_raw_parameters_t
{
  unsigned pre_median : 1, wb_cam : 1, greeneq : 1,
           no_auto_bright : 1, demosaic_method : 2,
           med_passes : 4, four_color_rgb : 1,
           highlight : 4,
           fill0 : 9; // 24 bits
  int8_t user_flip; // +8 = 32 bits.
}
dt_image_raw_parameters_t;

// __attribute__ ((aligned (128)))
typedef struct dt_image_t
{
  // minimal exif data here (all in multiples of 4-byte to interface nicely with c++):
  int32_t exif_inited;
  int32_t orientation;
  float exif_exposure;
  float exif_aperture;
  float exif_iso;
  float exif_focal_length;
  float exif_focus_distance;
  float exif_crop;
  char exif_maker[32];
  char exif_model[32];
  char exif_lens[52];
  char exif_datetime_taken[20];
  char filename[DT_MAX_PATH];

  // common stuff
  int32_t width, height, output_width, output_height;
  // used by library
  int32_t num, flags, film_id, id, group_id;
  // cache
  int32_t cacheline; // for image_cache
  uint8_t *mip[DT_IMAGE_MIPF]; // for mipmap_cache
  float *mipf;
  int32_t mip_width [DT_IMAGE_FULL]; // mipmap buffer extents of the buffers in mip[.] and mipf
  int32_t mip_height[DT_IMAGE_FULL];
  float mip_width_f [DT_IMAGE_FULL]; // precise mipmap widths inside the buffers in mip[.] and mipf
  uint8_t mip_invalid; // bit map to invalidate buffers.
  float mip_height_f[DT_IMAGE_FULL];
  dt_image_lock_t lock[DT_IMAGE_NONE];
  char lock_last[DT_IMAGE_NONE][100];
  int32_t import_lock;
  int32_t force_reimport;
  int32_t dirty;

  // raw image parameters
  float black, maximum;
  float raw_denoise_threshold, raw_auto_bright_threshold;
  dt_image_raw_parameters_t raw_params;
  uint32_t filters;  // demosaic pattern
  float *pixels;
  int32_t mip_buf_size[DT_IMAGE_NONE];
  int32_t bpp;       // bytes per pixel
}
dt_image_t;

// image buffer operations:
/** inits basic values to sensible defaults. */
void dt_image_init(dt_image_t *img);
/** returns non-zero if the image contains low-dynamic range data. */
int dt_image_is_ldr(const dt_image_t *img);
/** returns the full path name where the image was imported from. */
void dt_image_full_path(const int imgid, char *pathname, int len);
/** returns the portion of the path used for the film roll name. */
const char *dt_image_film_roll_name(const char *path);
/** returns the film roll name, i.e. without the path. */
void dt_image_film_roll(dt_image_t *img, char *pathname, int len);
/** appends version numbering for duplicated images. */
void dt_image_path_append_version(int imgid, char *pathname, const int len);
/** prints a one-line exif information string. */
void dt_image_print_exif(dt_image_t *img, char *line, int len);
/** opens an image with minimal storage from the data base and stores it in image cache. */
int dt_image_open(const int32_t id);
int dt_image_open2(dt_image_t *img, const int32_t id);
/** imports a new image from raw/etc file and adds it to the data base and image cache. */
int dt_image_import(const int32_t film_id, const char *filename, gboolean override_ignore_jpegs);
/** image is in db, mipmaps aren't? call this: */
int dt_image_reimport(dt_image_t *img, const char *filename, dt_image_buffer_t mip);
/** removes the given image from the database. */
void dt_image_remove(const int32_t imgid);
/** duplicates the given image in the database. */
int32_t dt_image_duplicate(const int32_t imgid);
/** flips the image, clock wise, if given flag. */
void dt_image_flip(const int32_t imgid, const int32_t cw);
/** returns 1 if there is history data found for this image, 0 else. */
int dt_image_altered(const dt_image_t *img);
/** returns the orientation bits of the image, exif or user override, if set. */
static inline int dt_image_orientation(const dt_image_t *img)
{
  return img->raw_params.user_flip > 0 ? img->raw_params.user_flip : (img->orientation > 0 ?img->orientation : 0);
}
/** returns the (flipped) filter string for the demosaic pattern. */
static inline uint32_t
dt_image_flipped_filter(const dt_image_t *img)
{
  // from the dcraw source code documentation:
  //
  //   0x16161616:     0x61616161:     0x49494949:     0x94949494:

  //   0 1 2 3 4 5     0 1 2 3 4 5     0 1 2 3 4 5     0 1 2 3 4 5
  // 0 B G B G B G   0 G R G R G R   0 G B G B G B   0 R G R G R G
  // 1 G R G R G R   1 B G B G B G   1 R G R G R G   1 G B G B G B
  // 2 B G B G B G   2 G R G R G R   2 G B G B G B   2 R G R G R G
  // 3 G R G R G R   3 B G B G B G   3 R G R G R G   3 G B G B G B
  //
  // orient:     0               5               6               3
  // orient:     6               0               3               5
  // orient:     5               3               0               6
  // orient:     3               6               5               0
  //
  // orientation: &1 : flip y    &2 : flip x    &4 : swap x/y
  //
  // if image height is odd (and flip y), need to switch pattern by one row:
  // 0x16161616 <-> 0x61616161
  // 0x49494949 <-> 0x94949494
  //
  // if image width is odd (and flip x), need to switch pattern by one column:
  // 0x16161616 <-> 0x49494949
  // 0x61616161 <-> 0x94949494

  const int orient = dt_image_orientation(img);
  int filters = img->filters;
  if((orient & 1) && (img->height & 1))
  {
    switch(filters)
    {
      case 0x16161616u:
        filters = 0x49494949u;
        break;
      case 0x49494949u:
        filters = 0x16161616u;
        break;
      case 0x61616161u:
        filters = 0x94949494u;
        break;
      case 0x94949494u:
        filters = 0x61616161u;
        break;
      default:
        filters = 0;
        break;
    }
  }
  if((orient & 2) && (img->width & 1))
  {
    switch(filters)
    {
      case 0x16161616u:
        filters = 0x61616161u;
        break;
      case 0x49494949u:
        filters = 0x94949494u;
        break;
      case 0x61616161u:
        filters = 0x16161616u;
        break;
      case 0x94949494u:
        filters = 0x49494949u;
        break;
      default:
        filters = 0;
        break;
    }
  }
  switch(filters)
  {
    case 0:
      // no mosaic is no mosaic, even rotated:
      return 0;
    case 0x16161616u:
      switch(orient)
      {
        case 5:
          return 0x61616161u;
        case 6:
          return 0x49494949u;
        case 3:
          return 0x94949494u;
        default:
          return 0x16161616u;
      }
      break;
    case 0x61616161u:
      switch(orient)
      {
        case 6:
          return 0x16161616u;
        case 3:
          return 0x49494949u;
        case 5:
          return 0x94949494u;
        default:
          return 0x61616161u;
      }
      break;
    case 0x49494949u:
      switch(orient)
      {
        case 3:
          return 0x61616161u;
        case 6:
          return 0x94949494u;
        case 5:
          return 0x16161616u;
        default:
          return 0x49494949u;
      }
      break;
    default: // case 0x94949494u:
      switch(orient)
      {
        case 6:
          return 0x61616161u;
        case 5:
          return 0x49494949u;
        case 3:
          return 0x16161616u;
        default:
          return 0x94949494u;
      }
      break;
  }
}
/** return the raw orientation, from jpg orientation. */
static inline int
dt_image_orientation_to_flip_bits(const int orient)
{
  switch(orient)
  {
    case 1:
      return 0 | 0 | 0;
    case 2:
      return 0 | 2 | 0;
    case 3:
      return 0 | 2 | 1;
    case 4:
      return 0 | 0 | 1;
    case 5:
      return 4 | 0 | 0;
    case 6:
      return 4 | 2 | 0;
    case 7:
      return 4 | 2 | 1;
    case 8:
      return 4 | 0 | 1;
    default:
      return 0;
  }
}
/** cleanup. */
void dt_image_cleanup(dt_image_t *img);
/** loads the requested buffer to cache, with read lock set. */
int dt_image_load(dt_image_t *img, dt_image_buffer_t mip);
/** returns appropriate mip map size for given area to paint on (width, height). */
dt_image_buffer_t dt_image_get_matching_mip_size(const dt_image_t *img, const int32_t width, const int32_t height, int32_t *w, int32_t *h);
/** returns appropriate mip map size for given mip level. */
void dt_image_get_mip_size(const dt_image_t *img, dt_image_buffer_t mip, int32_t *w, int32_t *h);
/** returns real image extends within mip map buffer size, in floating point. */
void dt_image_get_exact_mip_size(const dt_image_t *img, dt_image_buffer_t mip, float *w, float *h);
/** writes mip4 through to all smaller levels. */
dt_imageio_retval_t dt_image_update_mipmaps(dt_image_t *img);
/** this writes an xmp file for this image. */
void dt_image_write_sidecar_file(int imgid);
/** this writes xmp files for this image or all selected if selected == -1. Convenience wrapper around dt_image_write_sidecar_file(). */
void dt_image_synch_xmp(const int selected);
/** synchonizes .xmp sidecars file when duplicates to the actual number of duplicates present in database */
void dt_image_synch_all_xmp(const gchar *pathname);

// memory management interface
typedef struct dt_mipmap_cache_t
{
  dt_pthread_mutex_t mutex;
  int32_t num_entries[DT_IMAGE_NONE];
  dt_image_t **mip_lru[DT_IMAGE_NONE];
  size_t total_size[DT_IMAGE_NONE];
}
dt_mipmap_cache_t;

void dt_mipmap_cache_init(dt_mipmap_cache_t *cache, int32_t entries);
void dt_mipmap_cache_cleanup(dt_mipmap_cache_t *cache);
/** print some cache statistics. */
void dt_mipmap_cache_print(dt_mipmap_cache_t *cache);

/** if in debug mode, asserts image buffer size for mip is alloc'ed this large. */
void dt_image_check_buffer(dt_image_t *image, dt_image_buffer_t mip, int32_t size);
/** destroy buffer. */
void dt_image_free(dt_image_t *img, dt_image_buffer_t mip);

// locking-related functions:
void dt_image_invalidate(dt_image_t *image, dt_image_buffer_t mip);
void dt_image_validate(dt_image_t *image, dt_image_buffer_t mip);

#ifdef _DEBUG
// macros wrapping the stack trace information:
#define dt_image_get(A, B, C)    dt_image_get_with_caller  (A, B, C,  __FILE__, __LINE__, __FUNCTION__)
#define dt_image_alloc(img, mip) dt_image_alloc_with_caller(img, mip, __FILE__, __LINE__, __FUNCTION__)
#define dt_image_get_blocking(img, mip, mode)  dt_image_get_blocking_with_caller(img, mip, mode, __FILE__, __LINE__, __FUNCTION__)
#define dt_image_lock_if_available(img, mip_in, mode) dt_image_lock_if_available_with_caller(img, mip_in, mode, __FILE__, __LINE__, __FUNCTION__)
#define dt_image_prefetch(img, mip) dt_image_prefetch_with_caller(img, mip, __FILE__, __LINE__, __FUNCTION__)

// same as the non-debug versions, but with stack trace information:
dt_image_buffer_t dt_image_get_with_caller(dt_image_t *img, const dt_image_buffer_t mip, const char mode,
    const char *file, const int line, const char *function);
int dt_image_alloc_with_caller(dt_image_t *img, dt_image_buffer_t mip,
                               const char *file, const int line, const char *function);
dt_image_buffer_t dt_image_get_blocking_with_caller(dt_image_t *img, const dt_image_buffer_t mip, const char mode,
    const char *file, const int line, const char *function);
int dt_image_lock_if_available_with_caller(dt_image_t *img, const dt_image_buffer_t mip_in, const char mode,
    const char *file, const int line, const char *function);
void dt_image_prefetch_with_caller(dt_image_t *img, dt_image_buffer_t mip,
                                   const char *file, const int line, const char *function);
#else

/** gets the requested image buffer or a smaller preview if it is not available (w lock || =NULL), marking this with the read lock. returns found mip level. */
dt_image_buffer_t dt_image_get(dt_image_t *img, const dt_image_buffer_t mip, const char mode);
/** alloc new buffer for this mip map and image. also lock for writing. */
int dt_image_alloc(dt_image_t *img, dt_image_buffer_t mip);
/** returns the requested image buffer. loads while blocking, if necessary. */
dt_image_buffer_t dt_image_get_blocking(dt_image_t *img, const dt_image_buffer_t mip, const char mode);
/** locks the given mode if the buffer is available. returns non-zero and does nothing else on failure (no async loading is scheduled). */
int dt_image_lock_if_available(dt_image_t *img, const dt_image_buffer_t mip_in, const char mode);
/** prefetches given image buffer (mip map level/float preview/full raw), without marking it as used. */
void dt_image_prefetch(dt_image_t *img, dt_image_buffer_t mip);

#endif
/** unflags the used flag of given mip map level. these remove r and w locks, respectively. dropping the w lock will leave the r lock in place. */
void dt_image_release(dt_image_t *img, dt_image_buffer_t mip, const char mode);

/** converts img->pixels to img->mipf to img->mip[4--0]. needs full image buffer r locked. */
dt_imageio_retval_t dt_image_raw_to_preview(dt_image_t *img, const float *raw);
/** up-converts mip4 to mipf using guessed gamma values. needs mip4 r locked. */
dt_imageio_retval_t dt_image_preview_to_raw(dt_image_t *img);
#endif

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
