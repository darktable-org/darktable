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
#ifndef DT_IMAGEIO_EXR_H
#define DT_IMAGEIO_EXR_H
#include <inttypes.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct dt_imageio_exr_t
{
  uint32_t width, height;
} dt_imageio_exr_t;

/** write 16-bit exr file, with exif if not NULL. */
int dt_imageio_exr_write_f(const char *filename, const float *in, const int width, const int height, void *exif, int exif_len);
int dt_imageio_exr_write_with_icc_profile_f(const char *filename,const float *in, const int width, const int height, void *exif, int exif_len,int imgid);
  
#ifdef __cplusplus
}
#endif

#endif   
