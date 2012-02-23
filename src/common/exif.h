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

#ifndef DT_EXIF_H
#define DT_EXIF_H

#include "common/image.h"

/** wrapper around exiv2, C++ */
#ifdef __cplusplus
extern "C"
{
#endif

  /** read exif data from file with full path name, store to image struct. returns 0 on success. */
  int dt_exif_read(dt_image_t *img, const char* path);

  /** write exif to blob, return length in bytes. blob needs to be as large at 65535 bytes. sRGB should be true if sRGB colorspace is used as output. */
  int dt_exif_read_blob(uint8_t *blob, const char* path, const int sRGB, const int imgid);

  /** write blob to file exif. merges with existing exif information.*/
  int dt_exif_write_blob(uint8_t *blob,uint32_t size, const char* path);

  /** write xmp sidecar file. */
  int dt_exif_xmp_write (const int imgid, const char* filename);

  /** write xmp packet inside an image. */
  int dt_exif_xmp_attach (const int imgid, const char* filename);

  /** read xmp sidecar file. */
  int dt_exif_xmp_read (dt_image_t * img, const char* filename, const int history_only);

  /** thread safe init and cleanup. */
  void dt_exif_init();
  void dt_exif_cleanup();

  /** encode / decode op params */
  void dt_exif_xmp_encode (const unsigned char *input, char *output, const int len);
  void dt_exif_xmp_decode (const char *input, unsigned char *output, const int len);

#ifdef __cplusplus
}
#endif

#endif
