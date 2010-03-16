/*
    This file is part of darktable,
    copyright (c) 2010 Henrik Andersson.

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
#ifndef DT_IMAGEIO_TIFF_H
#define DT_IMAGEIO_TIFF_H

#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <tiffio.h>

typedef struct dt_imageio_tiff_t
{
  uint32 width, height;
  TIFF *handle;
} dt_imageio_tiff_t;

/** write 8-bit tiff file, with exif if not NULL. */
int dt_imageio_tiff_write_8(const char *filename, const uint8_t *in, const int width, const int height, void *exif, int exif_len);
int dt_imageio_tiff_write_with_icc_profile_8(const char *filename, const uint8_t *in, const int width, const int height, void *exif, int exif_len,int imgid);
/** write 16-bit tiff file, with exif if not NULL. */
int dt_imageio_tiff_write_16(const char *filename, const uint16_t *in, const int width, const int height, void *exif, int exif_len);
int dt_imageio_tiff_write_with_icc_profile_16(const char *filename, const uint16_t *in, const int width, const int height, void *exif, int exif_len,int imgid);
/** read tiff header from file, leave file descriptor open until tiff_read is called. */
int dt_imageio_tiff_read_header(const char *filename, dt_imageio_tiff_t *jpg);
/** reads the jpeg to the (sufficiently allocated) buffer, closes file. */
int dt_imageio_tiff_read(dt_imageio_tiff_t *jpg, uint8_t *out);
#endif
