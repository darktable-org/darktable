/*
    This file is part of darktable,
    Copyright (C) 2011-2024 darktable developers.

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

#include "common/mipmap_cache.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/exif.h"
#include "common/file_location.h"
#include "common/grealpath.h"
#include "common/image_cache.h"
#include "control/conf.h"
#include "control/jobs.h"
#include "develop/imageop_math.h"
#include "imageio/imageio_common.h"
#include "imageio/imageio_jpeg.h"
#include "imageio/imageio_module.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#if !defined(_WIN32)
#include <sys/statvfs.h>
#else
//statvfs does not exist in Windows, providing implementation
#include "win/statvfs.h"
#endif

#define DT_MIPMAP_CACHE_FILE_MAGIC 0xD71337
#define DT_MIPMAP_CACHE_FILE_VERSION 23
#define DT_MIPMAP_CACHE_DEFAULT_FILE_NAME "mipmaps"

// the number of pixels in the largest static image is the smallest number of pixels we
// can allocate for the mipmap buffer, since that gets filled in after we attempt to read
// from the image file on disk
#define MIN_IMG_PIXELS  (29*29)

typedef enum dt_mipmap_buffer_dsc_flags
{
  DT_MIPMAP_BUFFER_DSC_FLAG_NONE = 0,
  DT_MIPMAP_BUFFER_DSC_FLAG_GENERATE = 1 << 0,
  DT_MIPMAP_BUFFER_DSC_FLAG_INVALIDATE = 1 << 1
} dt_mipmap_buffer_dsc_flags;

// the embedded Exif data to tag thumbnails as sRGB or AdobeRGB
static const uint8_t dt_mipmap_cache_exif_data_srgb[] = {
  0x45, 0x78, 0x69, 0x66, 0x00, 0x00, 0x49, 0x49, 0x2a, 0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x69,
  0x87, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
  0x01, 0xa0, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const uint8_t dt_mipmap_cache_exif_data_adobergb[] = {
  0x45, 0x78, 0x69, 0x66, 0x00, 0x00, 0x49, 0x49, 0x2a, 0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x69,
  0x87, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
  0x01, 0xa0, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const int dt_mipmap_cache_exif_data_srgb_length
                      = sizeof(dt_mipmap_cache_exif_data_srgb) / sizeof(*dt_mipmap_cache_exif_data_srgb);
static const int dt_mipmap_cache_exif_data_adobergb_length
                      = sizeof(dt_mipmap_cache_exif_data_adobergb) / sizeof(*dt_mipmap_cache_exif_data_adobergb);

// Define the static images.  We make the definitions macros so that they can be expanded to either
// 4-channel 8 bits/channel integer images or 4-channel float images, depending on how __ and XX
// are defined at the time of use.
#define DEAD_IMAGE \
  { __,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__, \
    __,__,__,__,__,__,__,XX,XX,XX,XX,XX,XX,__,__,__,__,__,__,__, \
    __,__,__,__,__,__,XX,XX,XX,XX,XX,XX,XX,XX,__,__,__,__,__,__, \
    __,__,__,__,__,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,__,__,__,__,__, \
    __,__,__,__,__,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,__,__,__,__,__, \
    __,__,__,__,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,__,__,__,__, \
    __,__,__,__,XX,XX,XX,__,XX,XX,XX,XX,__,XX,XX,XX,__,__,__,__, \
    __,__,__,XX,XX,XX,__,__,__,XX,XX,__,__,__,XX,XX,XX,__,__,__, \
    __,__,__,XX,XX,XX,__,__,__,XX,XX,__,__,__,XX,XX,XX,__,__,__, \
    __,__,XX,XX,XX,__,__,__,__,XX,XX,__,__,__,__,XX,XX,XX,__,__, \
    __,__,XX,XX,XX,__,__,__,__,XX,XX,__,__,__,__,XX,XX,XX,__,__, \
    __,XX,XX,XX,XX,XX,__,__,__,XX,XX,__,__,__,XX,XX,XX,XX,XX,__, \
    __,XX,XX,XX,XX,XX,__,__,XX,XX,XX,XX,__,__,XX,XX,XX,XX,XX,__, \
    __,XX,XX,XX,XX,XX,XX,__,XX,XX,XX,XX,__,XX,XX,XX,XX,XX,XX,__, \
    __,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,__, \
    __,__,XX,XX,XX,XX,XX,XX,XX,__,__,XX,XX,XX,XX,XX,XX,XX,__,__, \
    __,__,XX,XX,XX,XX,XX,XX,XX,__,__,XX,XX,XX,XX,XX,XX,XX,__,__, \
    __,__,__,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,__,__,__, \
    __,__,__,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,__,__,__, \
    __,__,__,XX,XX,__,XX,__,XX,__,XX,__,XX,__,XX,XX,__,__,__,__, \
    __,__,__,XX,XX,__,__,__,__,__,__,__,__,__,XX,XX,__,__,__,__, \
    __,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__, \
    __,__,__,__,XX,__,__,XX,__,XX,__,XX,__,__,XX,__,__,__,__,__, \
    __,__,__,__,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,__,__,__,__,__, \
    __,__,__,__,__,XX,XX,XX,XX,XX,XX,XX,XX,XX,__,__,__,__,__,__, \
    __,__,__,__,__,__,XX,XX,XX,XX,XX,XX,XX,__,__,__,__,__,__,__, \
    __,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__ }

#define UNSUPPORTED_IMAGE \
  { __,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__, \
    __,__,__,__,__,__,__,XX,XX,XX,XX,__,__,__,__,__,__, \
    __,__,__,__,__,XX,XX,XX,XX,XX,XX,XX,XX,__,__,__,__, \
    __,__,__,__,XX,XX,XX,__,__,__,__,XX,XX,XX,__,__,__, \
    __,__,__,XX,XX,__,__,__,__,__,__,__,__,XX,XX,__,__, \
    __,__,XX,XX,__,__,__,__,__,__,__,__,__,XX,XX,__,__, \
    __,__,XX,XX,__,__,__,__,__,__,__,__,__,__,XX,XX,__, \
    __,XX,XX,__,__,__,__,__,__,__,__,__,__,__,XX,XX,__, \
    __,XX,XX,__,__,__,__,__,__,__,__,__,__,__,XX,XX,__, \
    __,XX,XX,__,__,__,__,__,__,__,__,__,__,__,XX,XX,__, \
    __,__,__,__,__,__,__,__,__,__,__,__,__,XX,XX,__,__, \
    __,__,__,__,__,__,__,__,__,__,__,__,__,XX,XX,__,__, \
    __,__,__,__,__,__,__,__,__,__,__,__,XX,XX,__,__,__, \
    __,__,__,__,__,__,__,__,__,__,__,XX,XX,__,__,__,__, \
    __,__,__,__,__,__,__,__,__,__,XX,XX,__,__,__,__,__, \
    __,__,__,__,__,__,__,__,__,XX,XX,__,__,__,__,__,__, \
    __,__,__,__,__,__,__,__,__,XX,XX,__,__,__,__,__,__, \
    __,__,__,__,__,__,__,__,XX,XX,__,__,__,__,__,__,__, \
    __,__,__,__,__,__,__,__,XX,XX,__,__,__,__,__,__,__, \
    __,__,__,__,__,__,__,__,XX,XX,__,__,__,__,__,__,__, \
    __,__,__,__,__,__,__,__,XX,XX,__,__,__,__,__,__,__, \
    __,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__, \
    __,__,__,__,__,__,__,__,XX,XX,__,__,__,__,__,__,__, \
    __,__,__,__,__,__,__,__,XX,XX,__,__,__,__,__,__,__, \
    __,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__ }

#define ERROR_IMAGE \
  { __,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__, \
    __,__,__,__,__,__,__,__,__,__,__,__,__,__,XX,__,__,__,__,__,__,__,__,__,__,__,__,__,__, \
    __,__,__,__,__,__,__,__,__,__,__,__,__,XX,XX,XX,__,__,__,__,__,__,__,__,__,__,__,__,__, \
    __,__,__,__,__,__,__,__,__,__,__,__,__,XX,XX,XX,__,__,__,__,__,__,__,__,__,__,__,__,__, \
    __,__,__,__,__,__,__,__,__,__,__,__,XX,XX,XX,XX,XX,__,__,__,__,__,__,__,__,__,__,__,__, \
    __,__,__,__,__,__,__,__,__,__,__,__,XX,XX,__,XX,XX,__,__,__,__,__,__,__,__,__,__,__,__, \
    __,__,__,__,__,__,__,__,__,__,__,XX,XX,__,__,__,XX,XX,__,__,__,__,__,__,__,__,__,__,__, \
    __,__,__,__,__,__,__,__,__,__,__,XX,XX,__,__,__,XX,XX,__,__,__,__,__,__,__,__,__,__,__, \
    __,__,__,__,__,__,__,__,__,__,XX,XX,XX,__,__,__,XX,XX,XX,__,__,__,__,__,__,__,__,__,__, \
    __,__,__,__,__,__,__,__,__,__,XX,XX,XX,__,__,__,XX,XX,XX,__,__,__,__,__,__,__,__,__,__, \
    __,__,__,__,__,__,__,__,__,XX,XX,XX,XX,__,__,__,XX,XX,XX,XX,__,__,__,__,__,__,__,__,__, \
    __,__,__,__,__,__,__,__,__,XX,XX,XX,XX,__,__,__,XX,XX,XX,XX,__,__,__,__,__,__,__,__,__, \
    __,__,__,__,__,__,__,__,XX,XX,XX,XX,XX,__,__,__,XX,XX,XX,XX,XX,__,__,__,__,__,__,__,__, \
    __,__,__,__,__,__,__,__,XX,XX,XX,XX,XX,__,__,__,XX,XX,XX,XX,XX,__,__,__,__,__,__,__,__, \
    __,__,__,__,__,__,__,XX,XX,XX,XX,XX,XX,__,__,__,XX,XX,XX,XX,XX,XX,__,__,__,__,__,__,__, \
    __,__,__,__,__,__,__,XX,XX,XX,XX,XX,XX,__,__,__,XX,XX,XX,XX,XX,XX,__,__,__,__,__,__,__, \
    __,__,__,__,__,__,XX,XX,XX,XX,XX,XX,XX,__,__,__,XX,XX,XX,XX,XX,XX,XX,__,__,__,__,__,__, \
    __,__,__,__,__,__,XX,XX,XX,XX,XX,XX,XX,__,__,__,XX,XX,XX,XX,XX,XX,XX,__,__,__,__,__,__, \
    __,__,__,__,__,XX,XX,XX,XX,XX,XX,XX,XX,XX,__,XX,XX,XX,XX,XX,XX,XX,XX,XX,__,__,__,__,__, \
    __,__,__,__,__,XX,XX,XX,XX,XX,XX,XX,XX,XX,__,XX,XX,XX,XX,XX,XX,XX,XX,XX,__,__,__,__,__, \
    __,__,__,__,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,__,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,__,__,__,__, \
    __,__,__,__,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,__,__,__,__, \
    __,__,__,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,__,__,__, \
    __,__,__,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,__,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,__,__,__, \
    __,__,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,__,__,__,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,__,__, \
    __,__,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,__,__,__,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,__,__, \
    __,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,__,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,__, \
    __,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,XX,__, \
    __,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__ }


struct dt_mipmap_buffer_dsc
{
  uint32_t width;
  uint32_t height;
  float iscale;
  size_t size;
  dt_mipmap_buffer_dsc_flags flags;
  dt_colorspaces_color_profile_type_t color_space;

#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
  // do not touch!
  // must be the last element.
  // must be no less than 16bytes
  char redzone[16];
#endif

  /* NB: sizeof must be a multiple of 4*sizeof(float) */
} DT_ALIGNED_ARRAY __attribute__((packed));

#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
static const size_t dt_mipmap_buffer_dsc_size __attribute__((unused))
= sizeof(struct dt_mipmap_buffer_dsc) - sizeof(((struct dt_mipmap_buffer_dsc *)0)->redzone);
#else
static const size_t dt_mipmap_buffer_dsc_size __attribute__((unused)) = sizeof(struct dt_mipmap_buffer_dsc);
#endif

// last resort mem alloc for dead images. sizeof(dt_mipmap_buffer_dsc) + max static image pixels (14x15)
// Must be aligned to cache line
static float DT_ALIGNED_ARRAY _mipmap_cache_static_dead_image[sizeof(struct dt_mipmap_buffer_dsc) / sizeof(float) + MIN_IMG_PIXELS * 4];

static inline void _dead_image_8(dt_mipmap_buffer_t *buf)
{
  if(!buf->buf) return;
  struct dt_mipmap_buffer_dsc *dsc = (struct dt_mipmap_buffer_dsc *)buf->buf - 1;
  dsc->width = 20; dsc->height = 27;
  dsc->iscale = 1.0f;
  buf->color_space = dsc->color_space = DT_COLORSPACE_DISPLAY;
  assert(dsc->size > 210 * sizeof(uint32_t));
#define __ 0
#define XX 0xffffffffu
  static const uint32_t image[] = DEAD_IMAGE;
#undef __
#undef XX
  memcpy(buf->buf, image, sizeof(image));
}

static inline void _dead_image_f(dt_mipmap_buffer_t *buf)
{
  if(!buf->buf) return;
  struct dt_mipmap_buffer_dsc *dsc = (struct dt_mipmap_buffer_dsc *)buf->buf - 1;
  dsc->width = 20; dsc->height = 27;
  dsc->iscale = 1.0f;
  buf->color_space = dsc->color_space = DT_COLORSPACE_DISPLAY;
  assert(dsc->size > 210 * 4 * sizeof(float));

#define XX 1.0f, 1.0f, 1.0f, 1.0f
#define __ 0.0f, 0.0f, 0.0f, 0.0f
  static const float image[] = DEAD_IMAGE;
#undef XX
#undef __
  memcpy(buf->buf, image, sizeof(image));
}

static inline void unsupp_image_8(dt_mipmap_buffer_t *buf)
{
  if(!buf->buf) return;
  struct dt_mipmap_buffer_dsc *dsc = (struct dt_mipmap_buffer_dsc *)buf->buf - 1;
  dsc->width = 17; dsc->height = 25;
  dsc->iscale = 1.0f;
  buf->color_space = dsc->color_space = DT_COLORSPACE_DISPLAY;
  assert(dsc->size >= dsc->width * dsc->height * sizeof(uint32_t));
#define __ 0
#define XX 0xffffffffu
  static const uint32_t image[] = UNSUPPORTED_IMAGE;
#undef __
#undef XX
  memcpy(buf->buf, image, sizeof(image));
}

static inline void unsupp_image_f(dt_mipmap_buffer_t *buf)
{
  if(!buf->buf) return;
  struct dt_mipmap_buffer_dsc *dsc = (struct dt_mipmap_buffer_dsc *)buf->buf - 1;
  dsc->width = 17; dsc->height = 25;
  dsc->iscale = 1.0f;
  buf->color_space = dsc->color_space = DT_COLORSPACE_DISPLAY;
  assert(dsc->size > dsc->width * dsc->height * 4 * sizeof(float));

#define XX 1.0f, 1.0f, 1.0f, 1.0f
#define __ 0.0f, 0.0f, 0.0f, 0.0f
  static const float image[] = UNSUPPORTED_IMAGE;
#undef XX
#undef __
  memcpy(buf->buf, image, sizeof(image));
}

static inline void error_image_8(dt_mipmap_buffer_t *buf)
{
  if(!buf->buf) return;
  struct dt_mipmap_buffer_dsc *dsc = (struct dt_mipmap_buffer_dsc *)buf->buf - 1;
  dsc->width = dsc->height = 29;
  dsc->iscale = 1.0f;
  buf->color_space = dsc->color_space = DT_COLORSPACE_DISPLAY;
  assert(dsc->size >= dsc->width * dsc->height * sizeof(uint32_t));
#define __ 0
#define XX 0xffffffffu
  static const uint32_t image[] = ERROR_IMAGE;
#undef __
#undef XX
  memcpy(buf->buf, image, sizeof(image));
}

//static inline
void error_image_f(dt_mipmap_buffer_t *buf)
{
  if(!buf->buf) return;
  struct dt_mipmap_buffer_dsc *dsc = (struct dt_mipmap_buffer_dsc *)buf->buf - 1;
  dsc->width = dsc->height = 29;
  dsc->iscale = 1.0f;
  buf->color_space = dsc->color_space = DT_COLORSPACE_DISPLAY;
  assert(dsc->size >= dsc->width * dsc->height * 4 * sizeof(float));

#define XX 1.0f, 1.0f, 1.0f, 1.0f
#define __ 0.0f, 0.0f, 0.0f, 0.0f
  static const float image[] = ERROR_IMAGE;
#undef XX
#undef __
  memcpy(buf->buf, image, sizeof(image));
}

static inline gboolean _is_static_image(void *buffer)
{
  return (buffer == _mipmap_cache_static_dead_image);
}

#ifndef NDEBUG
static inline int32_t _buffer_is_broken(dt_mipmap_buffer_t *buf)
{
  if(!buf->buf) return 0;
  struct dt_mipmap_buffer_dsc *dsc = (struct dt_mipmap_buffer_dsc *)buf->buf - 1;
  if(buf->width != dsc->width) return 1;
  if(buf->height != dsc->height) return 2;
  // somewhat loose bound:
  if(buf->width * buf->height > dsc->size) return 3;
  return 0;
}
#endif

static inline uint32_t get_key(const dt_imgid_t imgid, const dt_mipmap_size_t size)
{
  // imgid can't be >= 2^28 (~250 million images)
  // also ensure a valid key for bad imgid
  return (((uint32_t)size) << 28) | ((imgid - 1) & 0xfffffff);
}

static inline uint32_t get_imgid(const uint32_t key)
{
  return (key & 0xfffffff) + 1;
}

static inline dt_mipmap_size_t _get_size(const uint32_t key)
{
  return (dt_mipmap_size_t)(key >> 28);
}

static int _mipmap_cache_get_filename(gchar *mipmapfilename, size_t size)
{
  int r = -1;
  char *abspath = NULL;

  // Directory
  char cachedir[PATH_MAX] = { 0 };
  dt_loc_get_user_cache_dir(cachedir, sizeof(cachedir));

  // Build the mipmap filename
  const gchar *dbfilename = dt_database_get_path(darktable.db);
  if(!strcmp(dbfilename, ":memory:"))
  {
    mipmapfilename[0] = '\0';
    r = 0;
    goto exit;
  }

  abspath = g_realpath(dbfilename);
  if(!abspath) abspath = g_strdup(dbfilename);

  GChecksum *chk = g_checksum_new(G_CHECKSUM_SHA1);
  g_checksum_update(chk, (guchar *)abspath, strlen(abspath));
  const gchar *filename = g_checksum_get_string(chk);

  if(!filename || filename[0] == '\0')
    snprintf(mipmapfilename, size, "%s/%s", cachedir, DT_MIPMAP_CACHE_DEFAULT_FILE_NAME);
  else
    snprintf(mipmapfilename, size, "%s/%s-%s", cachedir, DT_MIPMAP_CACHE_DEFAULT_FILE_NAME, filename);

  g_checksum_free(chk);
  r = 0;

exit:
  g_free(abspath);

  return r;
}

static void _init_f(dt_mipmap_buffer_t *mipmap_buf,
                    float *buf,
                    uint32_t *width,
                    uint32_t *height,
                    float *iscale,
                    const dt_imgid_t imgid);
static void _init_8(uint8_t *buf,
                    uint32_t *width,
                    uint32_t *height,
                    float *iscale,
                    dt_colorspaces_color_profile_type_t *color_space,
                    const dt_imgid_t imgid,
                    const dt_mipmap_size_t size);

// callback for the imageio core to allocate memory.
// only needed for _F and _FULL buffers, as they change size
// with the input image. will allocate img->width*img->height*img->bpp bytes.
void *dt_mipmap_cache_alloc(dt_mipmap_buffer_t *buf, const dt_image_t *img)
{
  assert(buf->size == DT_MIPMAP_FULL);

  dt_cache_entry_t *entry = buf->cache_entry;
  struct dt_mipmap_buffer_dsc *dsc = (struct dt_mipmap_buffer_dsc *)entry->data;

  const int wd = img->width;
  const int ht = img->height;

  const size_t bpp = dt_iop_buffer_dsc_to_bpp(&img->buf_dsc);
  const size_t buffer_size = (size_t)wd * ht * bpp + sizeof(*dsc);

  // buf might have been alloc'ed before,
  // so only check size and re-alloc if necessary:
  if(!buf->buf || _is_static_image((void *)dsc) || (entry->data_size < buffer_size))
  {
    if(!_is_static_image((void *)dsc)) dt_free_align(entry->data);

    entry->data_size = 0;

    entry->data = dt_alloc_aligned(buffer_size);

    if(!entry->data)
    {
      // return fallback: at least alloc size for the error image (the largest static image):
      entry->data = (void *)_mipmap_cache_static_dead_image;

      // allocator holds the pointer. but let imageio client know that allocation failed:
      return NULL;
    }

    entry->data_size = buffer_size;

    // set buffer size only if we're making it larger.
    dsc = (struct dt_mipmap_buffer_dsc *)entry->data;
  }

  dsc->size = buffer_size;

  dsc->width = wd;
  dsc->height = ht;
  dsc->iscale = 1.0f;
  dsc->color_space = DT_COLORSPACE_NONE;
  dsc->flags = DT_MIPMAP_BUFFER_DSC_FLAG_GENERATE;
  buf->buf = (uint8_t *)(dsc + 1);

  // dt_print(DT_DEBUG_ALWAYS, "full buffer allocating img %u %d x %d = %u bytes (%p)", img->id, img->width,
  // img->height, buffer_size, *buf);

  assert(entry->data_size);
  assert(dsc->size);
  assert(dsc->size <= entry->data_size);

  ASAN_POISON_MEMORY_REGION(entry->data, entry->data_size);
  ASAN_UNPOISON_MEMORY_REGION(dsc + 1, buffer_size - sizeof(struct dt_mipmap_buffer_dsc));

  // return pointer to start of payload
  return dsc + 1;
}

// callback for the cache backend to initialize payload pointers
static void _mipmap_cache_allocate_dynamic(void *data, dt_cache_entry_t *entry)
{
  dt_mipmap_cache_t *cache = (dt_mipmap_cache_t *)data;
  // for full image buffers
  struct dt_mipmap_buffer_dsc *dsc = entry->data;
  const dt_mipmap_size_t mip = _get_size(entry->key);

  // alloc mere minimum for the header + broken image buffer:
  if(!dsc)
  {
    if(mip == DT_MIPMAP_8)
    {
      int imgfw= 0, imgfh= 0;
      // be sure that we have the right size values
      dt_image_get_final_size(get_imgid(entry->key), &imgfw, &imgfh);
      size_t pixels = MAX((size_t)(imgfw + 4) * (imgfh + 4), MIN_IMG_PIXELS);
      entry->data_size = sizeof(struct dt_mipmap_buffer_dsc) + 4 * pixels;
    }
    else if(mip <= DT_MIPMAP_F)
    {
      // these are fixed-size:
      entry->data_size = MAX(cache->buffer_size[mip], 4 * MIN_IMG_PIXELS);
    }
    else
    {
      entry->data_size = sizeof(*dsc) + sizeof(float) * 4 * MIN_IMG_PIXELS;
    }

    entry->data = dt_alloc_aligned(entry->data_size);

    // dt_print(DT_DEBUG_ALWAYS, "[mipmap cache] alloc dynamic for key %u %p", key, *buf);
    if(!(entry->data))
    {
      dt_print(DT_DEBUG_ALWAYS, "[mipmap_cache] memory allocation failed!");
      exit(1);
    }

    dsc = entry->data;

    if(mip <= DT_MIPMAP_F)
    {
      dsc->width = cache->max_width[mip];
      dsc->height = cache->max_height[mip];
      dsc->iscale = 1.0f;
      dsc->size = entry->data_size;
      dsc->color_space = DT_COLORSPACE_NONE;
    }
    else
    {
      dsc->width = 0;
      dsc->height = 0;
      dsc->iscale = 0.0f;
      dsc->color_space = DT_COLORSPACE_NONE;
      dsc->size = entry->data_size;
    }
  }

  assert(dsc->size >= sizeof(*dsc));

  int loaded_from_disk = 0;
  if(mip < DT_MIPMAP_F)
  {
    if(cache->cachedir[0] && ((dt_conf_get_bool("cache_disk_backend") && mip < DT_MIPMAP_8)
                              || (dt_conf_get_bool("cache_disk_backend_full") && mip == DT_MIPMAP_8)))
    {
      // try and load from disk, if successful set flag
      char filename[PATH_MAX] = {0};
      snprintf(filename, sizeof(filename), "%s.d/%d/%" PRIu32 ".jpg", cache->cachedir, (int)mip,
               get_imgid(entry->key));
      FILE *f = g_fopen(filename, "rb");
      if(f)
      {
        uint8_t *blob = 0;
        fseek(f, 0, SEEK_END);
        const long len = ftell(f);
        if(len <= 0) goto read_error; // coverity madness
        blob = (uint8_t *)dt_alloc_aligned(len);
        if(!blob) goto read_error;
        fseek(f, 0, SEEK_SET);
        const int rd = fread(blob, sizeof(uint8_t), len, f);
        if(rd != len) goto read_error;
        dt_colorspaces_color_profile_type_t color_space;
        dt_imageio_jpeg_t jpg;
        if(dt_imageio_jpeg_decompress_header(blob, len, &jpg)
           || (jpg.width > cache->max_width[mip] || jpg.height > cache->max_height[mip])
           || ((color_space = dt_imageio_jpeg_read_color_space(&jpg)) == DT_COLORSPACE_NONE) // pointless test to keep it in the if clause
           || dt_imageio_jpeg_decompress(&jpg, (uint8_t *)entry->data + sizeof(*dsc)))
        {
          dt_print(DT_DEBUG_ALWAYS,
                   "[mipmap_cache] failed to decompress thumbnail for ID=%d from `%s'!",
                   get_imgid(entry->key), filename);
          goto read_error;
        }
        dt_print(DT_DEBUG_CACHE,
                 "[mipmap_cache] grab mip %d for ID=%d from disk cache", mip,
                 get_imgid(entry->key));
        dsc->width = jpg.width;
        dsc->height = jpg.height;
        dsc->iscale = 1.0f;
        dsc->color_space = color_space;
        loaded_from_disk = 1;
        if(0)
        {
read_error:
          g_unlink(filename);
        }
        dt_free_align(blob);
        fclose(f);
      }
    }
  }

  if(!loaded_from_disk)
    dsc->flags = DT_MIPMAP_BUFFER_DSC_FLAG_GENERATE;
  else dsc->flags = 0;

  // cost is just flat one for the buffer, as the buffers might have different sizes,
  // to make sure quota is meaningful.
  if(mip >= DT_MIPMAP_F)
    entry->cost = 1;
  else if(mip == DT_MIPMAP_8)
    entry->cost = entry->data_size;
  else
    entry->cost = cache->buffer_size[mip];
}

static void _mipmap_cache_unlink_ondisk_thumbnail(void *data,
                                                  const dt_imgid_t imgid,
                                                  dt_mipmap_size_t mip)
{
  dt_mipmap_cache_t *cache = (dt_mipmap_cache_t *)data;

  // also remove jpg backing (always try to do that, in case user just temporarily switched it off,
  // to avoid inconsistencies.
  // if(dt_conf_get_bool("cache_disk_backend"))
  if(cache->cachedir[0])
  {
    char filename[PATH_MAX] = { 0 };
    snprintf(filename, sizeof(filename), "%s.d/%d/%"PRIu32".jpg", cache->cachedir, (int)mip, imgid);
    g_unlink(filename);
  }
}

static void _mipmap_cache_deallocate_dynamic(void *data, dt_cache_entry_t *entry)
{
  dt_mipmap_cache_t *cache = (dt_mipmap_cache_t *)data;
  const dt_mipmap_size_t mip = _get_size(entry->key);
  if(mip < DT_MIPMAP_F)
  {
    struct dt_mipmap_buffer_dsc *dsc = (struct dt_mipmap_buffer_dsc *)entry->data;
    // don't write skulls:
    if(dsc->width > 8 && dsc->height > 8)
    {
      if(dsc->flags & DT_MIPMAP_BUFFER_DSC_FLAG_INVALIDATE)
      {
        _mipmap_cache_unlink_ondisk_thumbnail(data, get_imgid(entry->key), mip);
      }
      else if(cache->cachedir[0] && ((dt_conf_get_bool("cache_disk_backend") && mip < DT_MIPMAP_8)
                                     || (dt_conf_get_bool("cache_disk_backend_full") && mip == DT_MIPMAP_8)))
      {
        // serialize to disk
        char filename[PATH_MAX] = {0};
        snprintf(filename, sizeof(filename), "%s.d/%d", cache->cachedir, mip);
        const int mkd = g_mkdir_with_parents(filename, 0750);
        if(!mkd)
        {
          snprintf(filename, sizeof(filename), "%s.d/%d/%" PRIu32 ".jpg", cache->cachedir, (int)mip,
                   get_imgid(entry->key));
          // Don't write existing files as both performance and quality (lossy jpg) suffer
          FILE *f = NULL;
          if(!g_file_test(filename, G_FILE_TEST_EXISTS) && (f = g_fopen(filename, "wb")))
          {
            // first check the disk isn't full
            struct statvfs vfsbuf;
            if(!statvfs(filename, &vfsbuf))
            {
              const int64_t free_mb = ((vfsbuf.f_frsize * vfsbuf.f_bavail) >> 20);
              if(free_mb < 100)
              {
                dt_print(DT_DEBUG_ALWAYS,
                         "[mipmap_cache] aborting image write as only %" PRId64 " MB free to write %s",
                         free_mb, filename);
                goto write_error;
              }
            }
            else
            {
              dt_print(DT_DEBUG_ALWAYS,
                       "[mipmap_cache] aborting image write since couldn't determine free space available to write %s",
                       filename);
              goto write_error;
            }

            const int cache_quality = dt_conf_get_int("database_cache_quality");
            const uint8_t *exif = NULL;
            int exif_len = 0;
            if(dsc->color_space == DT_COLORSPACE_SRGB)
            {
              exif = dt_mipmap_cache_exif_data_srgb;
              exif_len = dt_mipmap_cache_exif_data_srgb_length;
            }
            else if(dsc->color_space == DT_COLORSPACE_ADOBERGB)
            {
              exif = dt_mipmap_cache_exif_data_adobergb;
              exif_len = dt_mipmap_cache_exif_data_adobergb_length;
            }
            if(dt_imageio_jpeg_write(filename, (uint8_t *)entry->data + sizeof(*dsc), dsc->width, dsc->height, MIN(100, MAX(10, cache_quality)), exif, exif_len))
            {
write_error:
              g_unlink(filename);
            }
          }
          if(f) fclose(f);
        }
      }
    }
  }
  dt_free_align(entry->data);
}

static uint32_t _nearest_power_of_two(const uint32_t value)
{
  uint32_t rc = 1;
  while(rc < value) rc <<= 1;
  return rc;
}

void dt_mipmap_cache_init(dt_mipmap_cache_t *cache)
{
  _mipmap_cache_get_filename(cache->cachedir, sizeof(cache->cachedir));
  // make sure static memory is initialized
  struct dt_mipmap_buffer_dsc *dsc = (struct dt_mipmap_buffer_dsc *)_mipmap_cache_static_dead_image;
  _dead_image_f((dt_mipmap_buffer_t *)(dsc + 1));

  // adjust numbers to be large enough to hold what mem limit suggests.
  // we want at least 100MB, and consider 8G just still reasonable.
  const size_t max_mem = CLAMPS(darktable.dtresources.mipmap_memory, 100u << 20, ((size_t)8) << 30);
  // Fixed sizes for the thumbnail mip levels, selected for coverage of most screen sizes
  int32_t mipsizes[DT_MIPMAP_F][2] = {
    { 180, 110 },             // mip0 - ~1/2 size previous one
    { 360, 225 },             // mip1 - 1/2 size previous one
    { 720, 450 },             // mip2 - 1/2 size previous one
    { 1440, 900 },            // mip3 - covers 720p and 1366x768
    { 1920, 1200 },           // mip4 - covers 1080p and 1600x1200
    { 2560, 1600 },           // mip5 - covers 2560x1440
    { 4096, 2560 },           // mip6 - covers 4K and UHD
    { 5120, 3200 },           // mip7 - covers 5120x2880 panels
    { 999999999, 999999999 }, // mip8 - used for full preview at full size
  };
  // Set mipf to mip2 size as at most the user will be using an 8K screen and
  // have a preview that's ~4x smaller
  const char *preview_downsample = dt_conf_get_string_const("preview_downsampling");
  const float downsample = (!g_strcmp0(preview_downsample, "original")) ? 1.0f
                         : (!g_strcmp0(preview_downsample, "to 1/2")) ? 0.5f
                         : (!g_strcmp0(preview_downsample, "to 1/3")) ? 1/3.0f
                         : 0.25f;
  cache->max_width[DT_MIPMAP_F] = mipsizes[DT_MIPMAP_2][0] * downsample;
  cache->max_height[DT_MIPMAP_F] = mipsizes[DT_MIPMAP_2][1] * downsample;
  for(int k = DT_MIPMAP_F-1; k >= 0; k--)
  {
    cache->max_width[k]  = mipsizes[k][0];
    cache->max_height[k] = mipsizes[k][1];
  }
    // header + buffer
  for(int k = DT_MIPMAP_F-1; k >= 0; k--)
    cache->buffer_size[k] = sizeof(struct dt_mipmap_buffer_dsc)
                                + (size_t)cache->max_width[k] * cache->max_height[k] * 4;

  // clear stats:
  cache->mip_thumbs.stats_requests = 0;
  cache->mip_thumbs.stats_near_match = 0;
  cache->mip_thumbs.stats_misses = 0;
  cache->mip_thumbs.stats_fetches = 0;
  cache->mip_thumbs.stats_standin = 0;
  cache->mip_f.stats_requests = 0;
  cache->mip_f.stats_near_match = 0;
  cache->mip_f.stats_misses = 0;
  cache->mip_f.stats_fetches = 0;
  cache->mip_f.stats_standin = 0;
  cache->mip_full.stats_requests = 0;
  cache->mip_full.stats_near_match = 0;
  cache->mip_full.stats_misses = 0;
  cache->mip_full.stats_fetches = 0;
  cache->mip_full.stats_standin = 0;

  dt_cache_init(&cache->mip_thumbs.cache, 0, max_mem);
  dt_cache_set_allocate_callback(&cache->mip_thumbs.cache, _mipmap_cache_allocate_dynamic, cache);
  dt_cache_set_cleanup_callback(&cache->mip_thumbs.cache, _mipmap_cache_deallocate_dynamic, cache);

  // even with one thread you want two buffers. one for dr one for thumbs.
  // Also have the nr of cache entries larger than worker threads
  const int full_entries = 2 * dt_worker_threads();
  const int32_t max_mem_bufs = _nearest_power_of_two(full_entries);

  // for this buffer, because it can be very busy during import
  dt_cache_init(&cache->mip_full.cache, 0, max_mem_bufs);
  dt_cache_set_allocate_callback(&cache->mip_full.cache, _mipmap_cache_allocate_dynamic, cache);
  dt_cache_set_cleanup_callback(&cache->mip_full.cache, _mipmap_cache_deallocate_dynamic, cache);
  cache->buffer_size[DT_MIPMAP_FULL] = 0;

  // same for mipf:
  dt_cache_init(&cache->mip_f.cache, 0, max_mem_bufs);
  dt_cache_set_allocate_callback(&cache->mip_f.cache, _mipmap_cache_allocate_dynamic, cache);
  dt_cache_set_cleanup_callback(&cache->mip_f.cache, _mipmap_cache_deallocate_dynamic, cache);
  cache->buffer_size[DT_MIPMAP_F] = sizeof(struct dt_mipmap_buffer_dsc)
                                        + 4 * sizeof(float) * cache->max_width[DT_MIPMAP_F]
                                          * cache->max_height[DT_MIPMAP_F];
}

void dt_mipmap_cache_cleanup(dt_mipmap_cache_t *cache)
{
  dt_cache_cleanup(&cache->mip_thumbs.cache);
  dt_cache_cleanup(&cache->mip_full.cache);
  dt_cache_cleanup(&cache->mip_f.cache);
}

void dt_mipmap_cache_print(dt_mipmap_cache_t *cache)
{
  dt_print(DT_DEBUG_ALWAYS,"[mipmap_cache] thumbs fill %.2f/%.2f MB (%.2f%%)",
           cache->mip_thumbs.cache.cost / (1024.0 * 1024.0),
           cache->mip_thumbs.cache.cost_quota / (1024.0 * 1024.0),
           100.0f * (float)cache->mip_thumbs.cache.cost / (float)cache->mip_thumbs.cache.cost_quota);
  dt_print(DT_DEBUG_ALWAYS,"[mipmap_cache] float fill %"PRIu32"/%"PRIu32" slots (%.2f%%)",
           (uint32_t)cache->mip_f.cache.cost, (uint32_t)cache->mip_f.cache.cost_quota,
           100.0f * (float)cache->mip_f.cache.cost / (float)cache->mip_f.cache.cost_quota);
  dt_print(DT_DEBUG_ALWAYS,"[mipmap_cache] full  fill %"PRIu32"/%"PRIu32" slots (%.2f%%)",
           (uint32_t)cache->mip_full.cache.cost, (uint32_t)cache->mip_full.cache.cost_quota,
           100.0f * (float)cache->mip_full.cache.cost / (float)cache->mip_full.cache.cost_quota);

  uint64_t sum = 0;
  uint64_t sum_fetches = 0;
  uint64_t sum_standins = 0;
  sum += cache->mip_thumbs.stats_requests;
  sum_fetches += cache->mip_thumbs.stats_fetches;
  sum_standins += cache->mip_thumbs.stats_standin;
  sum += cache->mip_f.stats_requests;
  sum_fetches += cache->mip_f.stats_fetches;
  sum_standins += cache->mip_f.stats_standin;
  sum += cache->mip_full.stats_requests;
  sum_fetches += cache->mip_full.stats_fetches;
  sum_standins += cache->mip_full.stats_standin;
  dt_print(DT_DEBUG_ALWAYS,"[mipmap_cache] level | near match | miss | stand-in | fetches | total rq");
  dt_print(DT_DEBUG_ALWAYS,"[mipmap_cache] thumb | %6.2f%% | %6.2f%% | %6.2f%%  | %6.2f%% | %6.2f%%",
           100.0 * cache->mip_thumbs.stats_near_match / (float)cache->mip_thumbs.stats_requests,
           100.0 * cache->mip_thumbs.stats_misses / (float)cache->mip_thumbs.stats_requests,
           100.0 * cache->mip_thumbs.stats_standin / (float)sum_standins,
           100.0 * cache->mip_thumbs.stats_fetches / (float)sum_fetches,
           100.0 * cache->mip_thumbs.stats_requests / (float)sum);
  dt_print(DT_DEBUG_ALWAYS,"[mipmap_cache] float | %6.2f%% | %6.2f%% | %6.2f%%  | %6.2f%% | %6.2f%%",
           100.0 * cache->mip_f.stats_near_match / (float)cache->mip_f.stats_requests,
           100.0 * cache->mip_f.stats_misses / (float)cache->mip_f.stats_requests,
           100.0 * cache->mip_f.stats_standin / (float)sum_standins,
           100.0 * cache->mip_f.stats_fetches / (float)sum_fetches,
           100.0 * cache->mip_f.stats_requests / (float)sum);
  dt_print(DT_DEBUG_ALWAYS,"[mipmap_cache] full  | %6.2f%% | %6.2f%% | %6.2f%%  | %6.2f%% | %6.2f%%\n\n",
           100.0 * cache->mip_full.stats_near_match / (float)cache->mip_full.stats_requests,
           100.0 * cache->mip_full.stats_misses / (float)cache->mip_full.stats_requests,
           100.0 * cache->mip_full.stats_standin / (float)sum_standins,
           100.0 * cache->mip_full.stats_fetches / (float)sum_fetches,
           100.0 * cache->mip_full.stats_requests / (float)sum);
}

static gboolean _raise_signal_mipmap_updated(gpointer user_data)
{
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_DEVELOP_MIPMAP_UPDATED, GPOINTER_TO_INT(user_data));
  return FALSE; // only call once
}

static dt_mipmap_cache_one_t *_get_cache(dt_mipmap_cache_t *cache,
                                         const dt_mipmap_size_t mip)
{
  switch(mip)
  {
    case DT_MIPMAP_FULL:
      return &cache->mip_full;
    case DT_MIPMAP_F:
      return &cache->mip_f;
    default:
      return &cache->mip_thumbs;
  }
}

void dt_mipmap_cache_get_with_caller(
    dt_mipmap_cache_t *cache,
    dt_mipmap_buffer_t *buf,
    const dt_imgid_t imgid,
    const dt_mipmap_size_t mip,
    const dt_mipmap_get_flags_t flags,
    const char mode,
    const char *file,
    int line)
{
  const uint32_t key = get_key(imgid, mip);

  // buf may be null when called with DT_MIPMAP_PREFETCH for example
  if(buf)
    buf->loader_status = DT_IMAGEIO_OK;

  if(flags == DT_MIPMAP_TESTLOCK)
  {
    // simple case: only get and lock if it's there.
    dt_cache_entry_t *entry = dt_cache_testget(&_get_cache(cache, mip)->cache, key, mode);
    buf->cache_entry = entry;
    if(entry)
    {
      ASAN_UNPOISON_MEMORY_REGION(entry->data, dt_mipmap_buffer_dsc_size);
      struct dt_mipmap_buffer_dsc *dsc = (struct dt_mipmap_buffer_dsc *)entry->data;
      buf->width = dsc->width;
      buf->height = dsc->height;
      buf->iscale = dsc->iscale;
      buf->color_space = dsc->color_space;
      buf->imgid = imgid;
      buf->size = mip;

      // skip to next 8-byte alignment, for sse buffers.
      buf->buf = (uint8_t *)(dsc + 1);

      ASAN_UNPOISON_MEMORY_REGION(buf->buf, dsc->size - sizeof(struct dt_mipmap_buffer_dsc));
    }
    else
    {
      // set to NULL if failed.
      buf->width = buf->height = 0;
      buf->iscale = 0.0f;
      buf->imgid = NO_IMGID;
      buf->color_space = DT_COLORSPACE_NONE;
      buf->size = DT_MIPMAP_NONE;
      buf->buf = NULL;
    }
  }
  else if(flags == DT_MIPMAP_PREFETCH)
  {
    // and opposite: prefetch without locking
    if(mip > DT_MIPMAP_FULL || (int)mip < DT_MIPMAP_0)
      return; // remove the (int) once we no longer have to support gcc < 4.8 :/
    dt_control_add_job(darktable.control, DT_JOB_QUEUE_SYSTEM_FG, dt_image_load_job_create(imgid, mip));
  }
  else if(flags == DT_MIPMAP_PREFETCH_DISK)
  {
    // only prefetch if the disk cache exists:
    if(!cache->cachedir[0]) return;
    if(mip > DT_MIPMAP_FULL || (int)mip < DT_MIPMAP_0)
      return; // remove the (int) once we no longer have to support gcc < 4.8 :/
    char filename[PATH_MAX] = {0};
    snprintf(filename, sizeof(filename), "%s.d/%d/%"PRIu32".jpg", cache->cachedir, (int)mip, key);
    // don't attempt to load if disk cache doesn't exist
    if(!g_file_test(filename, G_FILE_TEST_EXISTS)) return;
    dt_control_add_job(darktable.control, DT_JOB_QUEUE_SYSTEM_FG, dt_image_load_job_create(imgid, mip));
  }
  else if(flags == DT_MIPMAP_BLOCKING)
  {
    // simple case: blocking get
    dt_cache_entry_t *entry =  dt_cache_get_with_caller(&_get_cache(cache, mip)->cache, key, mode, file, line);

    ASAN_UNPOISON_MEMORY_REGION(entry->data, dt_mipmap_buffer_dsc_size);

    struct dt_mipmap_buffer_dsc *dsc = (struct dt_mipmap_buffer_dsc *)entry->data;
    buf->cache_entry = entry;

    int mipmap_generated = 0;
    if(dsc->flags & DT_MIPMAP_BUFFER_DSC_FLAG_GENERATE)
    {
      mipmap_generated = 1;

      __sync_fetch_and_add(&(_get_cache(cache, mip)->stats_fetches), 1);
      // dt_print(DT_DEBUG_ALWAYS, "[mipmap cache get] now initializing buffer for img %u mip %d!", imgid, mip);
      // we're write locked here, as requested by the alloc callback.
      // now fill it with data:
      if(mip == DT_MIPMAP_FULL)
      {
        // load the image:
        // make sure we access the r/w lock as shortly as possible!
        dt_image_t DT_ALIGNED_ARRAY buffered_image;
        const dt_image_t *cimg = dt_image_cache_get(darktable.image_cache, imgid, 'r');
        buffered_image = *cimg;
        // dt_image_t *img = dt_image_cache_write_get(darktable.image_cache, cimg);
        // dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);
        dt_image_cache_read_release(darktable.image_cache, cimg);

        char filename[PATH_MAX] = { 0 };
        gboolean from_cache = TRUE;
        dt_image_full_path(buffered_image.id, filename, sizeof(filename), &from_cache);

        buf->imgid = imgid;
        buf->size = mip;
        buf->buf = NULL;
        buf->width = buf->height = 0;
        buf->iscale = 0.0f;
        buf->color_space = DT_COLORSPACE_NONE; // TODO: does the full buffer need to know this?
        dt_imageio_retval_t ret = dt_imageio_open(&buffered_image, filename, buf); // TODO: color_space?
        buf->loader_status = ret;
        // might have been reallocated:
        ASAN_UNPOISON_MEMORY_REGION(entry->data, dt_mipmap_buffer_dsc_size);
        dsc = (struct dt_mipmap_buffer_dsc *)buf->cache_entry->data;
        if(ret == DT_IMAGEIO_OK)
        {
          // swap back new image data:
          dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'w');
          *img = buffered_image;
          img->load_status = DT_IMAGEIO_OK;
          // dt_print(DT_DEBUG_ALWAYS, "[mipmap read get] initializing full buffer img %u with %u %u -> %d %d (%p)",
          // imgid, data[0], data[1], img->width, img->height, data);
          // don't write xmp for this (we only changed db stuff):
          dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);
        }
        else
        {
          dt_print(DT_DEBUG_PIPE, "[mipmap read get] error loading ID=%d", imgid);
          //
          // we can only return a zero dimension buffer if the buffer has been allocated.
          // in case dsc couldn't be allocated and points to the static buffer, it contains
          // a dead/unsupported/error image already.
          if(!_is_static_image((void *)dsc))
          {
            dsc->width = dsc->height = 0;
            buf->iscale = 0.0f;
            dsc->color_space = DT_COLORSPACE_NONE;
            buf->color_space = DT_COLORSPACE_NONE;
          }
          // record the error code in the cache, so that later lookups know it actually failed
          dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'w');
          img->load_status = ret;
          // don't write xmp for this (we only changed db stuff):
          dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);
        }
      }
      else if(mip == DT_MIPMAP_F)
      {
        ASAN_UNPOISON_MEMORY_REGION(dsc + 1, dsc->size - sizeof(struct dt_mipmap_buffer_dsc));
        _init_f(buf, (float *)(dsc + 1), &dsc->width, &dsc->height, &dsc->iscale, imgid);
      }
      else
      {
        // 8-bit thumbs
        ASAN_UNPOISON_MEMORY_REGION(dsc + 1, dsc->size - sizeof(struct dt_mipmap_buffer_dsc));
        _init_8((uint8_t *)(dsc + 1), &dsc->width, &dsc->height, &dsc->iscale, &buf->color_space, imgid, mip);
      }
      dsc->color_space = buf->color_space;
      dsc->flags &= ~DT_MIPMAP_BUFFER_DSC_FLAG_GENERATE;
    }

    // image cache is leaving the write lock in place in case the image has been newly allocated.
    // this leads to a slight increase in thread contention, so we opt for dropping the write lock
    // and acquiring a read lock immediately after. since this opens a small window for other threads
    // to get in between, we need to take some care to re-init cache entries and dsc.
    // note that concurrencykit has rw locks that can be demoted from w->r without losing the lock in between.
    if(mode == 'r')
    {
      entry->_lock_demoting = TRUE;
      // drop the write lock
      dt_cache_release(&_get_cache(cache, mip)->cache, entry);
      // get a read lock
      buf->cache_entry = entry = dt_cache_get(&_get_cache(cache, mip)->cache, key, mode);
      ASAN_UNPOISON_MEMORY_REGION(entry->data, dt_mipmap_buffer_dsc_size);
      entry->_lock_demoting = FALSE;
      dsc = (struct dt_mipmap_buffer_dsc *)buf->cache_entry->data;
    }

#ifdef _DEBUG
    const pthread_t writer = dt_pthread_rwlock_get_writer(&(buf->cache_entry->lock));
    if(mode == 'w')
    {
      assert(pthread_equal(writer, pthread_self()));
    }
    else
    {
      assert(!pthread_equal(writer, pthread_self()));
    }
#endif

    buf->width = dsc->width;
    buf->height = dsc->height;
    buf->iscale = dsc->iscale;
    buf->color_space = dsc->color_space;
    buf->imgid = imgid;
    buf->size = mip;

    ASAN_UNPOISON_MEMORY_REGION(dsc + 1, dsc->size - sizeof(struct dt_mipmap_buffer_dsc));
    buf->buf = (uint8_t *)(dsc + 1);

    if(mipmap_generated)
    {
      /* raise signal that mipmaps has been flushed to cache */
      g_idle_add(_raise_signal_mipmap_updated, GINT_TO_POINTER(imgid));
    }
    else if(dsc->width == 0 || dsc->height == 0)
    {
      // get the loading status of the image from the cache, so that we can assign an appropriate static image
      dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
      dt_imageio_retval_t ret = img->load_status;
      dt_image_cache_read_release(darktable.image_cache, img);
      dt_print(DT_DEBUG_PIPE, "[mipmap cache get] got a zero-sized ID=%d mip %d!", imgid, mip);
      if(mip < DT_MIPMAP_F)
      {
        switch(ret)
        {
        case DT_IMAGEIO_FILE_NOT_FOUND:
        default:
          _dead_image_8(buf);
          break;
        case DT_IMAGEIO_UNSUPPORTED_FORMAT:
        case DT_IMAGEIO_UNSUPPORTED_CAMERA:
        case DT_IMAGEIO_UNSUPPORTED_FEATURE:
          unsupp_image_8(buf);
          break;
        case DT_IMAGEIO_LOAD_FAILED:
        case DT_IMAGEIO_FILE_CORRUPTED:
        case DT_IMAGEIO_IOERROR:
          error_image_8(buf);
          break;
        }
      }
      else if(mip == DT_MIPMAP_F)
      {
        switch(ret)
        {
        case DT_IMAGEIO_FILE_NOT_FOUND:
        default:
          _dead_image_f(buf);
          break;
        case DT_IMAGEIO_UNSUPPORTED_FORMAT:
        case DT_IMAGEIO_UNSUPPORTED_CAMERA:
        case DT_IMAGEIO_UNSUPPORTED_FEATURE:
          unsupp_image_f(buf);
          break;
        case DT_IMAGEIO_LOAD_FAILED:
        case DT_IMAGEIO_FILE_CORRUPTED:
        case DT_IMAGEIO_IOERROR:
          error_image_f(buf);
          break;
        }
      }
      else
        buf->buf = NULL; // full images with NULL buffer have to be handled, indicates `missing image', but still return locked slot
    }
  }
  else if(flags == DT_MIPMAP_BEST_EFFORT)
  {
    __sync_fetch_and_add(&(_get_cache(cache, mip)->stats_requests), 1);
    // best-effort, might also return NULL.
    // never decrease mip level for float buffer or full image:
    dt_mipmap_size_t min_mip = (mip >= DT_MIPMAP_F) ? mip : DT_MIPMAP_0;
    for(int k = mip; k >= min_mip && k >= 0; k--)
    {
      // already loaded?
      dt_mipmap_cache_get(cache, buf, imgid, k, DT_MIPMAP_TESTLOCK, 'r');
      if(buf->buf && buf->width > 0 && buf->height > 0)
      {
        if(mip != k) __sync_fetch_and_add(&(_get_cache(cache, mip)->stats_standin), 1);
        return;
      }
      // didn't succeed the first time? prefetch for later!
      if(mip == k)
      {
        __sync_fetch_and_add(&(_get_cache(cache, mip)->stats_near_match), 1);
        dt_mipmap_cache_get(cache, buf, imgid, mip, DT_MIPMAP_PREFETCH, 'r');
      }
    }
    // couldn't find a smaller thumb, try larger ones only now (these will be slightly slower due to cairo rescaling):
    dt_mipmap_size_t max_mip = (mip >= DT_MIPMAP_F) ? mip : DT_MIPMAP_F-1;
    for(int k = mip+1; k <= max_mip; k++)
    {
      // already loaded?
      dt_mipmap_cache_get(cache, buf, imgid, k, DT_MIPMAP_TESTLOCK, 'r');
      if(buf->buf && buf->width > 0 && buf->height > 0)
      {
        __sync_fetch_and_add(&(_get_cache(cache, mip)->stats_standin), 1);
        return;
      }
    }
    __sync_fetch_and_add(&(_get_cache(cache, mip)->stats_misses), 1);
    // in case we don't even have a disk cache for our requested thumbnail,
    // prefetch at least mip0, in case we have that in the disk caches:
    if(cache->cachedir[0])
    {
      char filename[PATH_MAX] = {0};
      snprintf(filename, sizeof(filename), "%s.d/%d/%"PRIu32".jpg", cache->cachedir, (int)mip, key);
      if(g_file_test(filename, G_FILE_TEST_EXISTS))
        dt_mipmap_cache_get(cache, 0, imgid, DT_MIPMAP_0, DT_MIPMAP_PREFETCH_DISK, 0);
    }
    // nothing found :(
    buf->buf = NULL;
    buf->imgid = NO_IMGID;
    buf->size = DT_MIPMAP_NONE;
    buf->width = buf->height = 0;
    buf->iscale = 0.0f;
    buf->color_space = DT_COLORSPACE_NONE;
  }

  dt_print(DT_DEBUG_CACHE | DT_DEBUG_VERBOSE,
    "[dt_mipmap_cache_get] %s%s%s%s%s for ID=%d mip=%d mode=%c at %p",
    flags == DT_MIPMAP_TESTLOCK ? "DT_MIPMAP_TESTLOCK" : "",
    flags == DT_MIPMAP_PREFETCH ? "DT_MIPMAP_PREFETCH" : "",
    flags == DT_MIPMAP_PREFETCH_DISK ? "DT_MIPMAP_PREFETCH_DISK" : "",
    flags == DT_MIPMAP_BLOCKING ? "DT_MIPMAP_BLOCKING" : "",
    flags == DT_MIPMAP_BEST_EFFORT ? "DT_MIPMAP_BEST_EFFORT" : "",
    imgid, mip, mode, buf->buf);
}

void dt_mipmap_cache_release_with_caller(dt_mipmap_cache_t *cache,
                                         dt_mipmap_buffer_t *buf,
                                         const char *file,
                                         int line)
{
  if(buf->size == DT_MIPMAP_NONE) return;
  assert(dt_is_valid_imgid(buf->imgid));
  // assert(buf->size >= DT_MIPMAP_0); // breaks gcc-4.6/4.7 build
  assert(buf->size < DT_MIPMAP_NONE);
  assert(buf->cache_entry);
  dt_cache_release_with_caller(&_get_cache(cache, buf->size)->cache, buf->cache_entry, file, line);
  buf->size = DT_MIPMAP_NONE;
  buf->buf = NULL;
}


// return index dt_mipmap_size_t having at least width and height requested instead of minimum combined diff
// please note that the requested size is in pixels not dots.
dt_mipmap_size_t dt_mipmap_cache_get_matching_size(const dt_mipmap_cache_t *cache,
                                                   const int32_t width,
                                                   const int32_t height)
{
  dt_mipmap_size_t best = DT_MIPMAP_NONE;
  for(int k = DT_MIPMAP_0; k < DT_MIPMAP_F; k++)
  {
    best = k;
    if((cache->max_width[k] >= width) && (cache->max_height[k] >= height))
      break;
  }
  return best;
}

dt_mipmap_size_t dt_mipmap_cache_get_min_mip_from_pref(const char *value)
{
  if(strcmp(value, "always") == 0) return DT_MIPMAP_0;
  if(strcmp(value, "small") == 0)  return DT_MIPMAP_1;
  if(strcmp(value, "VGA") == 0)    return DT_MIPMAP_2;
  if(strcmp(value, "720p") == 0)   return DT_MIPMAP_3;
  if(strcmp(value, "1080p") == 0)  return DT_MIPMAP_4;
  if(strcmp(value, "WQXGA") == 0)  return DT_MIPMAP_5;
  if(strcmp(value, "4K") == 0)     return DT_MIPMAP_6;
  if(strcmp(value, "5K") == 0)     return DT_MIPMAP_7;
  return DT_MIPMAP_NONE;
}

void dt_mipmap_cache_remove_at_size(dt_mipmap_cache_t *cache,
                                    const dt_imgid_t imgid,
                                    const dt_mipmap_size_t mip)
{
  if(mip > DT_MIPMAP_8 || mip < DT_MIPMAP_0) return;
  // get rid of all ldr thumbnails:
  const uint32_t key = get_key(imgid, mip);
  dt_cache_entry_t *entry = dt_cache_testget(&_get_cache(cache, mip)->cache, key, 'w');
  if(entry)
  {
    ASAN_UNPOISON_MEMORY_REGION(entry->data, dt_mipmap_buffer_dsc_size);
    struct dt_mipmap_buffer_dsc *dsc = (struct dt_mipmap_buffer_dsc *)entry->data;
    dsc->flags |= DT_MIPMAP_BUFFER_DSC_FLAG_INVALIDATE;
    dt_cache_release(&_get_cache(cache, mip)->cache, entry);

    // due to DT_MIPMAP_BUFFER_DSC_FLAG_INVALIDATE, removes thumbnail from disc
    dt_cache_remove(&_get_cache(cache, mip)->cache, key);
  }
  else
  {
    // ugly, but avoids alloc'ing thumb if it is not there.
    _mipmap_cache_unlink_ondisk_thumbnail((&_get_cache(cache, mip)->cache)->cleanup_data, imgid, mip);
  }
}

void dt_mipmap_cache_remove(dt_mipmap_cache_t *cache,
                            const dt_imgid_t imgid)
{
  for(dt_mipmap_size_t k = DT_MIPMAP_0; k < DT_MIPMAP_F; k++)
  {
    dt_mipmap_cache_remove_at_size(cache, imgid, k);
  }
}

void dt_mipmap_cache_evict_at_size(dt_mipmap_cache_t *cache,
                                   const dt_imgid_t imgid,
                                   const dt_mipmap_size_t mip)
{
  const uint32_t key = get_key(imgid, mip);
  // write thumbnail to disc if not existing there
  dt_cache_remove(&_get_cache(cache, mip)->cache, key);
}

void dt_mimap_cache_evict(dt_mipmap_cache_t *cache,
                          const dt_imgid_t imgid)
{
  for(dt_mipmap_size_t k = DT_MIPMAP_0; k < DT_MIPMAP_F; k++)
  {
    const uint32_t key = get_key(imgid, k);

    // write thumbnail to disc if not existing there
    dt_cache_remove(&_get_cache(cache, k)->cache, key);
  }
}

static void _init_f(dt_mipmap_buffer_t *mipmap_buf,
                    float *out,
                    uint32_t *width,
                    uint32_t *height,
                    float *iscale,
                    const dt_imgid_t imgid)
{
  const uint32_t wd = *width, ht = *height;

  /* do not even try to process file if it isn't available */
  char filename[PATH_MAX] = { 0 };
  gboolean from_cache = TRUE;
  dt_image_full_path(imgid, filename, sizeof(filename), &from_cache);
  if(!*filename || !g_file_test(filename, G_FILE_TEST_EXISTS))
  {
    *width = *height = 0;
    *iscale = 0.0f;
    return;
  }

  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');

  // lock image after we have the buffer, we might need to lock the image struct for
  // writing during raw loading, to write to width/height.
  const dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'r');

  dt_iop_roi_t roi_in;
  roi_in.x = roi_in.y = 0;
  roi_in.width = image->width;
  roi_in.height = image->height;
  roi_in.scale = 1.0f;

  dt_iop_roi_t roi_out;
  roi_out.x = roi_out.y = 0;

  // now let's figure out the scaling...

  // MIP_F is 4 channels, and we do not demosaic here
  const float coeff = (image->buf_dsc.filters) ? 2.0f : 1.0f;

  roi_out.scale = fminf((coeff * (float)wd) / (float)image->width, (coeff * (float)ht) / (float)image->height);
  roi_out.width = roi_out.scale * roi_in.width;
  roi_out.height = roi_out.scale * roi_in.height;

  if(!buf.buf)
  {
    fprintf(stderr,"load_status = %d\n",(int)image->load_status);
    if(image->load_status == DT_IMAGEIO_FILE_NOT_FOUND)
      dt_control_log(_("image `%s' is not available!"), image->filename);
    else if(image->load_status == DT_IMAGEIO_LOAD_FAILED || image->load_status == DT_IMAGEIO_IOERROR ||
            image->load_status == DT_IMAGEIO_CACHE_FULL)
      dt_control_log(_("unable to load image `%s'!"), image->filename);
    else
      dt_control_log(_("image '%s' not supported"), image->filename);
    dt_image_cache_read_release(darktable.image_cache, image);
    *width = *height = 0;
    *iscale = 0.0f;
    return;
  }

  assert(!_buffer_is_broken(&buf));

  mipmap_buf->color_space = DT_COLORSPACE_NONE; // TODO: do we need that information in this buffer?

  if(image->buf_dsc.filters)
  {
    if(image->buf_dsc.filters != 9u && image->buf_dsc.datatype == TYPE_FLOAT)
    {
      dt_print_pipe(DT_DEBUG_PIPE,
        "mipmap mosaic_half_size_f", NULL, NULL, DT_DEVICE_CPU, &roi_in, &roi_out);
      dt_iop_clip_and_zoom_mosaic_half_size_f((float *const)out, (const float *const)buf.buf, &roi_out, &roi_in,
                                              roi_out.width, roi_in.width, image->buf_dsc.filters);
    }
    else if(image->buf_dsc.filters != 9u && image->buf_dsc.datatype == TYPE_UINT16)
    {
      dt_print_pipe(DT_DEBUG_PIPE,
        "mipmap mosaic_half_size", NULL, NULL, DT_DEVICE_CPU, &roi_in, &roi_out);
      dt_iop_clip_and_zoom_mosaic_half_size((uint16_t * const)out, (const uint16_t *)buf.buf, &roi_out, &roi_in,
                                            roi_out.width, roi_in.width, image->buf_dsc.filters);
    }
    else if(image->buf_dsc.filters == 9u && image->buf_dsc.datatype == TYPE_UINT16)
    {
      dt_print_pipe(DT_DEBUG_PIPE,
        "mipmap mosaic_third_size_xtrans", NULL, NULL, DT_DEVICE_CPU, &roi_in, &roi_out);
      dt_iop_clip_and_zoom_mosaic_third_size_xtrans((uint16_t * const)out, (const uint16_t *)buf.buf, &roi_out,
                                                    &roi_in, roi_out.width, roi_in.width, image->buf_dsc.xtrans);
    }
    else if(image->buf_dsc.filters == 9u && image->buf_dsc.datatype == TYPE_FLOAT)
    {
      dt_print_pipe(DT_DEBUG_PIPE,
        "mipmap mosaic_third_size_xtrans_f", NULL, NULL, DT_DEVICE_CPU, &roi_in, &roi_out);
      dt_iop_clip_and_zoom_mosaic_third_size_xtrans_f(out, (const float *)buf.buf, &roi_out, &roi_in,
                                                      roi_out.width, roi_in.width, image->buf_dsc.xtrans);
    }
    else
    {
      dt_print_pipe(DT_DEBUG_ALWAYS,
        "mipmap unreachable_codepath", NULL, NULL, DT_DEVICE_CPU, &roi_in, &roi_out);
      dt_unreachable_codepath();
    }
  }
  else
  {
    // downsample
    dt_print_pipe(DT_DEBUG_PIPE,
      "mipmap clip and zoom", NULL, NULL, DT_DEVICE_CPU, &roi_in, &roi_out);
    dt_iop_clip_and_zoom(out, (const float *)buf.buf, &roi_out, &roi_in);
  }

  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);

  *width = roi_out.width;
  *height = roi_out.height;
  *iscale = (float)image->width / (float)roi_out.width;

  dt_image_cache_read_release(darktable.image_cache, image);
}


// dummy functions for `export' to mipmap buffers:
typedef struct _dummy_data_t
{
  dt_imageio_module_data_t head;
  uint8_t *buf;
} _dummy_data_t;

static int _levels(dt_imageio_module_data_t *data)
{
  return IMAGEIO_RGB | IMAGEIO_INT8;
}

static int _bpp(dt_imageio_module_data_t *data)
{
  return 8;
}


static int _write_image(dt_imageio_module_data_t *data,
                        const char *filename,
                        const void *in,
                        dt_colorspaces_color_profile_type_t over_type,
                        const char *over_filename,
                        void *exif,
                        int exif_len,
                        dt_imgid_t imgid,
                        int num,
                        int total,
                        dt_dev_pixelpipe_t *pipe,
                        const gboolean export_masks)
{
  _dummy_data_t *d = (_dummy_data_t *)data;
  memcpy(d->buf, in, sizeof(uint32_t) * data->width * data->height);
  return 0;
}

static void _init_8(uint8_t *buf,
                    uint32_t *width,
                    uint32_t *height,
                    float *iscale,
                    dt_colorspaces_color_profile_type_t *color_space,
                    const dt_imgid_t imgid,
                    const dt_mipmap_size_t size)
{
  *iscale = 1.0f;
  const uint32_t wd = *width, ht = *height;
  char filename[PATH_MAX] = { 0 };
  gboolean from_cache = TRUE;

  /* do not even try to process file if it isn't available */
  dt_image_full_path(imgid, filename, sizeof(filename), &from_cache);
  if(!*filename || !g_file_test(filename, G_FILE_TEST_EXISTS))
  {
    *width = *height = 0;
    *iscale = 0.0f;
    *color_space = DT_COLORSPACE_NONE;
    return;
  }

  const gboolean altered = dt_image_altered(imgid);
  gboolean res = TRUE;

  const dt_image_t *cimg = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  // the orientation for this camera is not read correctly from exiv2, so we need
  // to go the full path (as the thumbnail will be flipped the wrong way round)
  const int incompatible = !strncmp(cimg->exif_maker, "Phase One", 9);
  dt_image_cache_read_release(darktable.image_cache, cimg);

  const char *min = dt_conf_get_string_const("plugins/lighttable/thumbnail_raw_min_level");
  const dt_mipmap_size_t min_s = dt_mipmap_cache_get_min_mip_from_pref(min);
  const gboolean use_embedded = (size <= min_s);

  if(!altered && use_embedded && !incompatible)
  {
    const dt_image_orientation_t orientation = dt_image_get_orientation(imgid);

    // try to load the embedded thumbnail in raw
    from_cache = TRUE;
    memset(filename, 0, sizeof(filename));
    dt_image_full_path(imgid, filename, sizeof(filename), &from_cache);

    const char *c = filename + strlen(filename);
    while(*c != '.' && c > filename) c--;
    if(!strcasecmp(c, ".jpg"))
    {
      // try to load jpg
      dt_imageio_jpeg_t jpg;
      if(!dt_imageio_jpeg_read_header(filename, &jpg))
      {
        uint8_t *tmp = dt_alloc_align_uint8((size_t)jpg.width * jpg.height * 4);
        *color_space = dt_imageio_jpeg_read_color_space(&jpg);
        if(!dt_imageio_jpeg_read(&jpg, tmp))
        {
          // scale to fit
          dt_print(DT_DEBUG_CACHE,
                   "[mipmap_cache] generate mip %d for ID=%d from jpeg", size, imgid);
          dt_iop_flip_and_zoom_8(tmp, jpg.width, jpg.height, buf, wd, ht, orientation, width, height);
          res = FALSE;
        }
        dt_free_align(tmp);
      }
    }
    else
    {
      uint8_t *tmp = 0;
      int32_t thumb_width, thumb_height;
      res = dt_imageio_large_thumbnail(filename, &tmp, &thumb_width, &thumb_height, color_space);
      if(!res)
      {
        // if the thumbnail is not large enough, we compute one
        const dt_image_t *img2 = dt_image_cache_get(darktable.image_cache, imgid, 'r');
        const int imgwd = img2->width;
        const int imght = img2->height;
        dt_image_cache_read_release(darktable.image_cache, img2);
        if(thumb_width < wd
           && thumb_height < ht
           && thumb_width < imgwd - 4
           && thumb_height < imght - 4)
        {
          res = TRUE;
        }
        else
        {
          // scale to fit
          dt_print(DT_DEBUG_CACHE,
                   "[mipmap_cache] generate mip %d for ID=%d from embedded jpeg",
                   size, imgid);
          dt_iop_flip_and_zoom_8(tmp, thumb_width, thumb_height,
                                 buf, wd, ht, orientation, width, height);
        }
        dt_free_align(tmp);
      }
    }
  }

  if(res)
  {
    //try to generate mip from larger mip
    for(dt_mipmap_size_t k = size + 1; k < DT_MIPMAP_F; k++)
    {
      dt_mipmap_buffer_t tmp;
      dt_mipmap_cache_get(darktable.mipmap_cache, &tmp, imgid, k, DT_MIPMAP_TESTLOCK, 'r');
      if(tmp.buf == NULL)
        continue;
      dt_print(DT_DEBUG_CACHE,
               "[mipmap_cache] generate mip %d for ID=%d from level %d",
               size, imgid, k);
      *color_space = tmp.color_space;
      // downsample
      dt_iop_flip_and_zoom_8(tmp.buf, tmp.width, tmp.height, buf, wd, ht, ORIENTATION_NONE, width, height);

      dt_mipmap_cache_release(darktable.mipmap_cache, &tmp);
      res = FALSE;
      break;
    }
  }

  if(res)
  {
    // try the real thing: rawspeed + pixelpipe
    dt_imageio_module_format_t format;
    _dummy_data_t dat;
    format.bpp = _bpp;
    format.write_image = _write_image;
    format.levels = _levels;
    dat.head.max_width = wd;
    dat.head.max_height = ht;
    dat.buf = buf;
    // export with flags: ignore exif(don't load from disk), don't
    // swap byte order, don't do hq processing, no upscaling and
    // signal we want thumbnail export
    res = dt_imageio_export_with_flags(imgid, "unused", &format, (dt_imageio_module_data_t *)&dat, TRUE, FALSE, FALSE,
                                       FALSE, FALSE, TRUE, NULL, FALSE, FALSE, DT_COLORSPACE_NONE, NULL, DT_INTENT_LAST, NULL,
                                       NULL, 1, 1, NULL, -1);
    if(!res)
    {
      dt_print(DT_DEBUG_CACHE,
               "[mipmap_cache] generate mip %d for ID=%d from scratch",
               size, imgid);
      // might be smaller, or have a different aspect than what we got as input.
      *width = dat.head.width;
      *height = dat.head.height;
      *iscale = 1.0f;
      *color_space = dt_mipmap_cache_get_colorspace();
    }
    dt_print(DT_DEBUG_PIPE, "[mipmap init 8] export ID=%d finished (sizes %d %d => %d %d)",
      imgid, wd, ht, dat.head.width, dat.head.height);
  }

  // any errors?
  if(res)
  {
    dt_print(DT_DEBUG_PIPE, "[mipmap_cache] could not process thumbnail!");
    *width = *height = 0;
    *iscale = 0.0f;
    *color_space = DT_COLORSPACE_NONE;
    return;
  }

  // TODO: various speed optimizations:
  // TODO: also init all smaller mips!
  // TODO: use mipf, but:
  // TODO: if output is cropped, don't use mipf!
}

dt_colorspaces_color_profile_type_t dt_mipmap_cache_get_colorspace()
{
  if(dt_conf_get_bool("cache_color_managed"))
    return DT_COLORSPACE_ADOBERGB;
  return DT_COLORSPACE_DISPLAY;
}

void dt_mipmap_cache_copy_thumbnails(const dt_mipmap_cache_t *cache,
                                     const dt_imgid_t dst_imgid,
                                     const dt_imgid_t src_imgid)
{
  if(cache->cachedir[0]
    && dt_conf_get_bool("cache_disk_backend")
    && dt_is_valid_imgid(src_imgid)
    && dt_is_valid_imgid(dst_imgid))
  {
    for(dt_mipmap_size_t mip = DT_MIPMAP_0; mip < DT_MIPMAP_F; mip++)
    {
      // try and load from disk, if successful set flag
      char srcpath[PATH_MAX] = {0};
      char dstpath[PATH_MAX] = {0};
      snprintf(srcpath, sizeof(srcpath), "%s.d/%d/%"PRIu32".jpg", cache->cachedir, (int)mip, src_imgid);
      snprintf(dstpath, sizeof(dstpath), "%s.d/%d/%"PRIu32".jpg", cache->cachedir, (int)mip, dst_imgid);
      GFile *src = g_file_new_for_path(srcpath);
      GFile *dst = g_file_new_for_path(dstpath);
      GError *gerror = NULL;
      g_file_copy(src, dst, G_FILE_COPY_NONE, NULL, NULL, NULL, &gerror);
      // ignore errors, we tried what we could.
      g_object_unref(dst);
      g_object_unref(src);
      g_clear_error(&gerror);
    }
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
