/*
    This file is part of darktable,
    Copyright (C) 2009-2024 darktable developers.

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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "common/darktable.h"
#include "develop/imageop.h"         // for IOP_CS_RGB
#include "imageio/imageio_pfm.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

dt_imageio_retval_t dt_imageio_open_pfm(dt_image_t *img,
                                        const char *filename,
                                        dt_mipmap_buffer_t *mbuf)
{
  FILE *f = g_fopen(filename, "rb");
  if(!f)
    return DT_IMAGEIO_FILE_NOT_FOUND;

  int ret = 0;
  int channels = 3;
  float scale_factor;
  char head[2] = { 'X', 'X' };

  ret = fscanf(f, "%c%c\n", head, head + 1);

  if(ret != 2 || head[0] != 'P')
    goto error_corrupt;

  if(head[1] == 'F')
    channels = 3;
  else if(head[1] == 'f')
    channels = 1;
  else
    goto error_corrupt;

  int read_byte;
  gboolean made_by_photoshop = TRUE;
  // We expect metadata with a newline character at the end.
  // If there is no whitespace in the first line, then this file
  // was most likely written by Photoshop.
  // We need to know this because Photoshop writes the image rows
  // to the file in a different order.
  for(;;)
  {
    read_byte = fgetc(f);
    if((read_byte == '\n') || (read_byte == EOF))
      break;
    if(read_byte < '0')          // easy way to match all whitespaces
    {
      made_by_photoshop = FALSE; // if present, the file is not saved by Photoshop
      break;
    }
  }

  // Now rewind to start of PFM metadata
  fseek(f, 3, SEEK_SET);

  char width_string[10] = { 0 };
  char height_string[10] = { 0 };
  char scale_factor_string[64] = { 0 };

  ret = fscanf(f, "%9s %9s %63s%*[^\n]", width_string, height_string, scale_factor_string);

  if(ret != 3)
    goto error_corrupt;

  errno = 0;
  img->width = strtol(width_string, NULL, 0);
  img->height = strtol(height_string, NULL, 0);
  scale_factor = g_ascii_strtod(scale_factor_string, NULL);

  if(errno != 0)
    goto error_corrupt;
  if(img->width <= 0 || img->height <= 0 )
    goto error_corrupt;

  ret = fread(&ret, sizeof(char), 1, f);
  if(ret != 1)
    goto error_corrupt;
  ret = 0;

  int swap_byte_order = (scale_factor >= 0.0) ^ (G_BYTE_ORDER == G_BIG_ENDIAN);

  img->buf_dsc.channels = 4;
  img->buf_dsc.datatype = TYPE_FLOAT;
  float *buf = (float *)dt_mipmap_cache_alloc(mbuf, img);
  if(!buf)
    goto error_cache_full;

  const size_t npixels = (size_t)img->width * img->height;

  float *readbuf = dt_alloc_align_float(npixels * 4);
  if(!readbuf)
    goto error_cache_full;

  // We use this union to swap the byte order in the float value if needed
  union { float as_float; guint32 as_int; } value;

  // The de facto standard (set by the first implementation) scanline order
  // of PFM is bottom-to-top, so in the loops below we change the order of
  // the rows in the process of filling the output buffer with data
  if(channels == 3)
  {
    ret = fread(readbuf, 3 * sizeof(float), npixels, f);
    size_t target_row = 0;

DT_OMP_FOR(collapse(2))
    for(size_t row = 0; row < img->height; row++)
      for(size_t column = 0; column < img->width; column++)
      {
        dt_aligned_pixel_t pix = {0.0f, 0.0f, 0.0f, 0.0f};
        if(made_by_photoshop)
          target_row = row;
        else
          target_row = img->height - 1 - row;
        for_three_channels(c)
        {
        value.as_float = readbuf[3 * (target_row * img->width + column) + c];
        if(swap_byte_order) value.as_int = GUINT32_SWAP_LE_BE(value.as_int);
        pix[c] = value.as_float;
        }
        copy_pixel_nontemporal(&buf[4 * (img->width * row + column)], pix);
      }
  }
  else
  {
    ret = fread(readbuf, sizeof(float), npixels, f);
    size_t target_row = 0;

DT_OMP_FOR(collapse(2))
    for(size_t row = 0; row < img->height; row++)
      for(size_t column = 0; column < img->width; column++)
      {
        if(made_by_photoshop)
          target_row = row;
        else
          target_row = img->height - 1 - row;
        value.as_float = readbuf[target_row * img->width + column];
        if(swap_byte_order) value.as_int = GUINT32_SWAP_LE_BE(value.as_int);
        buf[4 * (img->width * row + column) + 2] = buf[4 * (img->width * row + column) + 1]
            = buf[4 * (img->width * row + column) + 0] = value.as_float;
      }
  }

  fclose(f);
  dt_free_align(readbuf);

  img->buf_dsc.cst = IOP_CS_RGB;
  img->buf_dsc.filters = 0u;
  img->flags &= ~DT_IMAGE_LDR;
  img->flags &= ~DT_IMAGE_RAW;
  img->flags &= ~DT_IMAGE_S_RAW;
  img->flags |= DT_IMAGE_HDR;
  img->loader = LOADER_PFM;
  return DT_IMAGEIO_OK;

error_corrupt:
  fclose(f);
  return DT_IMAGEIO_FILE_CORRUPTED;
error_cache_full:
  fclose(f);
  return DT_IMAGEIO_CACHE_FULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
