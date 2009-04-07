#ifndef DT_IMAGE_IO_H
#define DT_IMAGE_IO_H

#include <glib.h>
#include <stdio.h>
#include "common/image.h"

#include <inttypes.h>

// opens the file using libraw, doing interpolation and stuff
int dt_imageio_open_raw(dt_image_t *img, const char *filename);
// opens file using imagemagick
int dt_imageio_open_ldr(dt_image_t *img, const char *filename);
// try both, first libraw.
int dt_imageio_open(dt_image_t *img, const char *filename);

// write cache to database, returns 0 on success. assumes buf to be locked.
int dt_imageio_preview_write(dt_image_t *img, dt_image_buffer_t mip);
// read database to cache, returns 0 on success. leaves 'w' locked buf.
int dt_imageio_preview_read(dt_image_t *img, dt_image_buffer_t mip);

int dt_imageio_export_8 (dt_image_t *img, const char *filename);
int dt_imageio_export_16(dt_image_t *img, const char *filename);
int dt_imageio_export_f (dt_image_t *img, const char *filename);

void dt_imageio_preview_f_to_8(int32_t wd, int32_t ht, const float *f, uint8_t *p8);

#endif
