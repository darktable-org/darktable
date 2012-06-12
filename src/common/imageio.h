/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.

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
#ifndef DT_IMAGE_IO_H
#define DT_IMAGE_IO_H

#include <glib.h>
#include <stdio.h>
#include "common/image.h"
#include "common/mipmap_cache.h"

#include <inttypes.h>

// opens the file using pfm, hdr, exr.
dt_imageio_retval_t dt_imageio_open_hdr(dt_image_t *img, const char *filename, dt_mipmap_cache_allocator_t a);
// opens the file using libraw, doing interpolation and stuff
dt_imageio_retval_t dt_imageio_open_raw(dt_image_t *img, const char *filename, dt_mipmap_cache_allocator_t a);
// opens file using imagemagick
dt_imageio_retval_t dt_imageio_open_ldr(dt_image_t *img, const char *filename, dt_mipmap_cache_allocator_t a);
// try both, first libraw.
dt_imageio_retval_t dt_imageio_open(dt_image_t *img, const char *filename, dt_mipmap_cache_allocator_t a);

struct dt_imageio_module_format_t;
struct dt_imageio_module_data_t;
int
dt_imageio_export(
    const uint32_t imgid,
    const char *filename,
    struct dt_imageio_module_format_t *format,
    struct dt_imageio_module_data_t *format_params);
int
dt_imageio_export_with_flags(
    const uint32_t                     imgid,
    const char                        *filename,
    struct dt_imageio_module_format_t *format,
    struct dt_imageio_module_data_t   *format_params,
    const int32_t                      ignore_exif,
    const int32_t                      display_byteorder,
    const int32_t                      high_quality,
    const int32_t                      thumbnail_export);

int dt_imageio_write_pos(int i, int j, int wd, int ht, float fwd, float fht, int orientation);

// general, efficient buffer flipping function using memcopies
void
dt_imageio_flip_buffers(
    char *out,
    const char *in,
    const size_t bpp,         // bytes per pixel
    const int wd,
    const int ht,
    const int fwd,
    const int fht,
    const int stride,
    const int orientation);

void dt_imageio_flip_buffers_ui16_to_float(float *out, const uint16_t *in, const float black, const float white, const int ch, const int wd, const int ht, const int fwd, const int fht, const int stride, const int orientation);
void dt_imageio_flip_buffers_ui8_to_float(float *out, const uint8_t *in, const float black, const float white, const int ch, const int wd, const int ht, const int fwd, const int fht, const int stride, const int orientation);
#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
