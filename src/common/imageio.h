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
#ifndef DT_IMAGE_IO_H
#define DT_IMAGE_IO_H

#include <glib.h>
#include <stdio.h>
#include "common/image.h"

#include <inttypes.h>

// opens the file using pfm, hdr, exr.
dt_imageio_retval_t dt_imageio_open_hdr_preview(dt_image_t *img, const char *filename);
// opens the file using libraw, doing interpolation and stuff
dt_imageio_retval_t dt_imageio_open_raw_preview(dt_image_t *img, const char *filename);
// opens file using imagemagick
dt_imageio_retval_t dt_imageio_open_ldr_preview(dt_image_t *img, const char *filename);
// try both, first libraw.
dt_imageio_retval_t dt_imageio_open_preview(dt_image_t *img, const char *filename);

// opens the file using pfm, hdr, exr.
dt_imageio_retval_t dt_imageio_open_hdr(dt_image_t *img, const char *filename);
// opens the file using libraw, doing interpolation and stuff
dt_imageio_retval_t dt_imageio_open_raw(dt_image_t *img, const char *filename);
// opens file using imagemagick
dt_imageio_retval_t dt_imageio_open_ldr(dt_image_t *img, const char *filename);
// try both, first libraw.
dt_imageio_retval_t dt_imageio_open(dt_image_t *img, const char *filename);

// write cache to database, returns 0 on success. assumes buf to be locked.
int dt_imageio_preview_write(dt_image_t *img, dt_image_buffer_t mip);
// read database to cache, returns 0 on success. leaves 'w' locked buf.
int dt_imageio_preview_read(dt_image_t *img, dt_image_buffer_t mip);

// writes out all image information to a .dt-file
int dt_imageio_dt_write(const int imgid, const char *filename);
// reads the history stack etc from disk and synchs with the db.
int dt_imageio_dt_read (const int imgid, const char *filename);

// writes out human-readable .dttags file containing stars and tags.
int dt_imageio_dttags_write (const int imgid, const char *filename);
// reads .dttags file to database. requires a locked img as argument.
int dt_imageio_dttags_read (dt_image_t *img, const char *filename);

int dt_imageio_export_8 (dt_image_t *img, const char *filename);
int dt_imageio_export_16(dt_image_t *img, const char *filename);
int dt_imageio_export_f (dt_image_t *img, const char *filename);

void dt_imageio_preview_f_to_8(int32_t wd, int32_t ht, const float *f, uint8_t *p8);
void dt_imageio_preview_8_to_f(int32_t wd, int32_t ht, const uint8_t *p8, float *f);

#endif
