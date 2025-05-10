/*
 *    This file is part of darktable,
 *    Copyright (C) 2016-2025 darktable developers.
 *
 *    darktable is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    darktable is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/darktable.h"
#include "common/image.h"

#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

float *dt_read_pfm(const char *filename, int *err, int *wd, int *ht, int *ch, const size_t planes)
{
  if(wd) *wd = 0;
  if(ht) *ht = 0;
  if(ch) *ch = 0;
  if(err) *err = DT_IMAGEIO_OK;
  if(!filename || filename[0] == 0)
  {
    if(err) *err = DT_IMAGEIO_FILE_NOT_FOUND;
    return NULL;
  }

  float *readbuf = NULL;
  float *image = NULL;
  FILE *f = g_fopen(filename, "rb");
  if(!f)
  {
    if(err) *err = DT_IMAGEIO_FILE_NOT_FOUND;
    return NULL;
  }

  char head[2] = { 'X', 'X' };
  char scale_factor_string[64] = { 0 };
  char width_string[10] = { 0 };
  char height_string[10] = { 0 };
  int width, height, channels, ret = 0;

  ret = fscanf(f, "%c%c\n", head, head + 1);
  if(ret != 2 || head[0] != 'P')
  {
    if(err) *err = DT_IMAGEIO_FILE_CORRUPTED;
    goto error;
  }

  if(head[1] == 'F')        channels = 3;
  else if(head[1] == 'f')   channels = 1;
  else
  {
    if(err) *err = DT_IMAGEIO_FILE_CORRUPTED;
    goto error;
  }
  if(ch) *ch = channels;


  // We expect metadata with a newline character at the end.
  // If there is no whitespace in the first line, then this file
  // was most likely written by Photoshop.
  // We need to know this because Photoshop writes the image rows
  // to the file in a different order.
  gboolean made_by_photoshop = TRUE;
  for(;;)
  {
    int read_byte = fgetc(f);
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

  ret = fscanf(f, "%9s %9s %63s%*[^\n]", width_string, height_string, scale_factor_string);
  if(ret != 3)
  {
    if(err) *err = DT_IMAGEIO_FILE_CORRUPTED;
    goto error;
  }

  ret = fread(&ret, sizeof(char), 1, f);
  if(ret != 1)
  {
    if(err) *err = DT_IMAGEIO_FILE_CORRUPTED;
    goto error;
  }

  width = strtol(width_string, NULL, 0);
  height = strtol(height_string, NULL, 0);
  if(width <= 0 || height <= 0)
  {
    if(err) *err = DT_IMAGEIO_FILE_CORRUPTED;
    goto error;
  }

  if(wd) *wd = width;
  if(ht) *ht = height;

  const float scale_factor = g_ascii_strtod(scale_factor_string, NULL);
  const int swap_byte_order = (scale_factor >= 0.0) ^ (G_BYTE_ORDER == G_BIG_ENDIAN);
  const size_t npixels = (size_t)width * height;

  readbuf = dt_alloc_align_float(npixels * channels);
  image = dt_alloc_align_float(npixels * planes);
  if(!readbuf || !image)
  {
    if(err) *err = DT_IMAGEIO_IOERROR;
    dt_print(DT_DEBUG_ALWAYS, "can't allocate memory for pfm file `%s'", filename);
    goto error;
  }

  ret = fread(readbuf, sizeof(float) * channels, npixels, f);
  if(ret != npixels)
  {
    if(err) *err = DT_IMAGEIO_IOERROR;
    dt_print(DT_DEBUG_ALWAYS, "can't read all pfm file contents from '%s'", filename);
    goto error;
  }

  // We use this union to swap the byte order in the float value if needed
  union { float as_float; guint32 as_int; } value;

  // The de facto standard (set by the first implementation) scanline order
  // of PFM is bottom-to-top, so in the loops below we change the order of
  // the rows in the process of filling the output buffer with data
  if(channels == 3)
  {
    DT_OMP_FOR()
    for(size_t row = 0; row < height; row++)
    {
      const size_t target_row = made_by_photoshop ? row : height - 1 - row;
      for(size_t column = 0; column < width; column++)
      {
        dt_aligned_pixel_t pix = { 0.0f, 0.0f, 0.0f, 0.0f};
        for_three_channels(c)
        {
          value.as_float = readbuf[3 * (target_row * width + column) + c];
          if(swap_byte_order) value.as_int = GUINT32_SWAP_LE_BE(value.as_int);
          pix[c] = value.as_float;
        }
        for(size_t c = 0; c < planes; c++)
          image[planes*(row*width + column) + c] = pix[c];
      }
    }
  }
  else
  {
    DT_OMP_FOR()
    for(size_t row = 0; row < height; row++)
    {
      const size_t target_row = made_by_photoshop ? row : height - 1 - row;
      for(size_t column = 0; column < width; column++)
      {
        value.as_float = readbuf[target_row * width + column];
        if(swap_byte_order) value.as_int = GUINT32_SWAP_LE_BE(value.as_int);
        for(size_t c = 0; c < planes; c++)
          image[planes*(row*width + column) + c] = value.as_float;
      }
    }
  }
  dt_free_align(readbuf);
  fclose(f);

  return image;

  error:
  dt_free_align(readbuf);
  dt_free_align(image);
  fclose(f);
  return NULL;
}

void dt_write_pfm(const char *filename, const size_t width, const size_t height, const void *data, const size_t bpp)
{
  if(!filename || filename[0] == 0)
  {
    dt_print(DT_DEBUG_ALWAYS, "no filename provided for 'dt_write_pfm'");
    return;
  }

  FILE *f = g_fopen(filename, "wb");
  if(!f)
  {
    dt_print(DT_DEBUG_ALWAYS, "can't write file `%s'", filename);
    return;
  }

  if(bpp==2)
    fprintf(f, "P5\n%d %d\n", (int)width, (int)height);
  else
    fprintf(f, "P%s\n%d %d\n-1.0\n", (bpp == sizeof(float)) ? "f" : "F", (int)width, (int)height);

  void *buf_line = dt_alloc_align_float(width * 4);
  for(size_t row = 0; row < height; row++)
  {
    // NOTE: pfm has rows in reverse order
    const size_t row_in = height - 1 - row;
    if(bpp == 4*sizeof(float))
    {
      const float *in = data + bpp * width * row_in;
      float *out = (float *)buf_line;
      for(size_t i = 0; i < width; i++, in += 4, out += 3)
        memcpy(out, in, sizeof(float) * 3);
      int cnt = fwrite(buf_line, sizeof(float) * 3, width, f);
      if(cnt != width) break;
    }
    else if(bpp == 3*sizeof(float))
    {
      const float *in = data + bpp * width * row_in;
      float *out = (float *)buf_line;
      for(size_t i = 0; i < width; i++, in += 3, out += 3)
        memcpy(out, in, sizeof(float) * 3);
      int cnt = fwrite(buf_line, sizeof(float) * 3, width, f);
      if(cnt != width) break;
    }
    else if(bpp == sizeof(float))
    {
      const void *in = data + bpp * width * row_in;
      int cnt = fwrite(in, sizeof(float), width, f);
      if(cnt != width) break;
    }
    else if(bpp == sizeof(uint16_t))
    {
      const void *in = data + bpp * width * row_in;
      int cnt = fwrite(in, sizeof(uint16_t), width, f);
      if(cnt != width) break;
    }
  }

  dt_free_align(buf_line);
  fclose(f);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

