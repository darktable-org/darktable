/*
    This file is part of darktable,
    Copyright (C) 2009-2020 darktable developers.

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

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
// this fixes a rather annoying, long time bug in libjpeg :(
#undef HAVE_STDLIB_H
#undef HAVE_STDDEF_H
#include <jpeglib.h>
#undef HAVE_STDLIB_H
#undef HAVE_STDDEF_H

#include "common/colorspaces.h"
#include "common/image.h"
#include "common/mipmap_cache.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct dt_imageio_jpeg_t
{
  int width, height;
  struct jpeg_source_mgr src;
  struct jpeg_destination_mgr dest;
  struct jpeg_decompress_struct dinfo;
  struct jpeg_compress_struct cinfo;
  FILE *f;
} dt_imageio_jpeg_t;

/** reads the header and fills width/height in jpg struct. */
int dt_imageio_jpeg_decompress_header(const void *in, size_t length, dt_imageio_jpeg_t *jpg);
/** reads the whole image to the out buffer, which has to be large enough. */
int dt_imageio_jpeg_decompress(dt_imageio_jpeg_t *jpg, uint8_t *out);
/** compresses in to out buffer with given quality (0..100). out buffer must be large enough. returns actual
 * data length. */
int dt_imageio_jpeg_compress(const uint8_t *in, uint8_t *out, const int width, const int height,
                             const int quality);

/** write jpeg to file, with exif if not NULL. */
int dt_imageio_jpeg_write(const char *filename, const uint8_t *in, const int width, const int height,
                          const int quality, const void *exif, int exif_len);
/** this will collect the images icc profile (or the global export override) and append it during write. */
int dt_imageio_jpeg_write_with_icc_profile(const char *filename, const uint8_t *in, const int width,
                                           const int height, const int quality, const void *exif, int exif_len,
                                           dt_imgid_t imgid);
/** read jpeg header from file, leave file descriptor open until jpeg_read is called. */
int dt_imageio_jpeg_read_header(const char *filename, dt_imageio_jpeg_t *jpg);
/** reads the jpeg to the (sufficiently allocated) buffer, closes file. */
int dt_imageio_jpeg_read(dt_imageio_jpeg_t *jpg, uint8_t *out);
/** reads the color profile attached to the jpeg, closes file. */
int dt_imageio_jpeg_read_profile(dt_imageio_jpeg_t *jpg, uint8_t **out);
/** return the color space of the image, this only distinguishs between sRGB, AdobeRGB and unknown. used for mipmaps */
dt_colorspaces_color_profile_type_t dt_imageio_jpeg_read_color_space(dt_imageio_jpeg_t *jpg);

/** utility function to read and open jpeg from imagio.c */
dt_imageio_retval_t dt_imageio_open_jpeg(dt_image_t *img, const char *filename, dt_mipmap_buffer_t *buf);

#ifdef __cplusplus
} // extern "C"
#endif /* __cplusplus */

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

