/*
    This file is part of darktable,
    Copyright (C) 2018-2023 darktable developers.

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

// pbm -- portable bit map. values are either 0 or 1, single channel
static dt_imageio_retval_t _read_pbm(dt_image_t *img, FILE*f, float *buf)
{
  dt_imageio_retval_t result = DT_IMAGEIO_OK;

  int bytes_needed = (img->width + 7) / 8;
  uint8_t *line = calloc(bytes_needed, sizeof(uint8_t));

  float *buf_iter = buf;
  for(size_t y = 0; y < img->height; y++)
  {
    if(fread(line, sizeof(uint8_t), (size_t)bytes_needed, f) != bytes_needed)
    {
      result = DT_IMAGEIO_LOAD_FAILED;
      break;
    }
    for(size_t x = 0; x < bytes_needed; x++)
    {
      uint8_t byte = line[x] ^ 0xff;
      for(int bit = 0; bit < 8 && x * 8 + bit < img->width; bit++)
      {
        float value = ((byte & 0x80) >> 7) * 1.0;
        buf_iter[0] = buf_iter[1] = buf_iter[2] = value;
        buf_iter[3] = 0.0;
        buf_iter += 4;
        byte <<= 1;
      }
    }
  }

  free(line);

  return result;
}

// pgm -- portable gray map. values are between 0 and max, single channel
static dt_imageio_retval_t _read_pgm(dt_image_t *img, FILE*f, float *buf)
{
  dt_imageio_retval_t result = DT_IMAGEIO_OK;

  unsigned int max;
  // We expect at most a 5-digit number (65535) + a newline + '\0', so 7 characters.
  char maxvalue_string[7];
  if(fgets(maxvalue_string,7,f))
    max = atoi(maxvalue_string);
  else
    return DT_IMAGEIO_LOAD_FAILED;
  if(max == 0 || max > 65535) return DT_IMAGEIO_LOAD_FAILED;

  if(max <= 255)
  {
    uint8_t *line = calloc(img->width, sizeof(uint8_t));

    float *buf_iter = buf;
    for(size_t y = 0; y < img->height; y++)
    {
      if(fread(line, sizeof(uint8_t), (size_t)img->width, f) != img->width)
      {
        result = DT_IMAGEIO_LOAD_FAILED;
        break;
      }
      for(size_t x = 0; x < img->width; x++)
      {
        float value = (float)line[x] / (float)max;
        buf_iter[0] = buf_iter[1] = buf_iter[2] = value;
        buf_iter[3] = 0.0;
        buf_iter += 4;
      }
    }
    free(line);
  }
  else
  {
    uint16_t *line = calloc(img->width, sizeof(uint16_t));

    float *buf_iter = buf;
    for(size_t y = 0; y < img->height; y++)
    {
      if(fread(line, sizeof(uint16_t), (size_t)img->width, f) != img->width)
      {
        result = DT_IMAGEIO_LOAD_FAILED;
        break;
      }
      for(size_t x = 0; x < img->width; x++)
      {
        uint16_t intvalue = line[x];
        if(G_BYTE_ORDER != G_BIG_ENDIAN)
          intvalue = GUINT16_SWAP_LE_BE(intvalue);
        float value = (float)intvalue / (float)max;
        buf_iter[0] = buf_iter[1] = buf_iter[2] = value;
        buf_iter[3] = 0.0;
        buf_iter += 4;
      }
    }
    free(line);
  }

  return result;
}

// ppm -- portable pix map. values are between 0 and max, three channels
static dt_imageio_retval_t _read_ppm(dt_image_t *img, FILE*f, float *buf)
{
  dt_imageio_retval_t result = DT_IMAGEIO_OK;

  unsigned int max;
  // We expect at most a 5-digit number (65535) + a newline + '\0', so 7 characters.
  char maxvalue_string[7];
  if(fgets(maxvalue_string,7,f))
    max = atoi(maxvalue_string);
  else
    return DT_IMAGEIO_LOAD_FAILED;
  if(max == 0 || max > 65535) return DT_IMAGEIO_LOAD_FAILED;

  if(max <= 255)
  {
    uint8_t *line = calloc((size_t)3 * img->width, sizeof(uint8_t));

    float *buf_iter = buf;
    for(size_t y = 0; y < img->height; y++)
    {
      if(fread(line, 3 * sizeof(uint8_t), (size_t)img->width, f) != img->width)
      {
        result = DT_IMAGEIO_LOAD_FAILED;
        break;
      }
      for(size_t x = 0; x < img->width; x++)
      {
        for(int c = 0; c < 3; c++)
        {
          float value = (float)line[x * 3 + c] / (float)max;
          *buf_iter++ = value;
        }
        *buf_iter++ = 0.0;
      }
    }
    free(line);
  }
  else
  {
    uint16_t *line = calloc((size_t)3 * img->width, sizeof(uint16_t));

    float *buf_iter = buf;
    for(size_t y = 0; y < img->height; y++)
    {
      if(fread(line, 3 * sizeof(uint16_t), (size_t)img->width, f) != img->width)
      {
        result = DT_IMAGEIO_LOAD_FAILED;
        break;
      }
      for(size_t x = 0; x < img->width; x++)
      {
        for(int c = 0; c < 3; c++)
        {
          uint16_t intvalue = line[x * 3 + c];
          // PPM files are big endian! http://netpbm.sourceforge.net/doc/ppm.html
          if(G_BYTE_ORDER != G_BIG_ENDIAN)
            intvalue = GUINT16_SWAP_LE_BE(intvalue);
          float value = (float)intvalue / (float)max;
          *buf_iter++ = value;
        }
        *buf_iter++ = 0.0;
      }
    }
    free(line);
  }

  return result;
}

dt_imageio_retval_t dt_imageio_open_pnm(dt_image_t *img, const char *filename, dt_mipmap_buffer_t *mbuf)
{
  const char *ext = filename + strlen(filename);
  while(*ext != '.' && ext > filename) ext--;
  if(strcasecmp(ext, ".pbm") && strcasecmp(ext, ".pgm") && strcasecmp(ext, ".pnm") && strcasecmp(ext, ".ppm"))
    return DT_IMAGEIO_LOAD_FAILED;
  FILE *f = g_fopen(filename, "rb");
  if(!f) return DT_IMAGEIO_LOAD_FAILED;
  int ret = 0;
  dt_imageio_retval_t result = DT_IMAGEIO_LOAD_FAILED;

  char head[2] = { 'X', 'X' };
  ret = fscanf(f, "%c%c ", head, head + 1);
  if(ret != 2 || head[0] != 'P') goto end;

  char width_string[10] = { 0 };
  char height_string[10] = { 0 };
  ret = fscanf(f, "%9s %9s ", width_string, height_string);
  if(ret != 2) goto end;

  errno = 0;
  img->width = strtol(width_string, NULL, 0);
  img->height = strtol(height_string, NULL, 0);
  if(errno != 0 || img->width <= 0 || img->height <= 0) goto end;

  img->buf_dsc.channels = 4;
  img->buf_dsc.datatype = TYPE_FLOAT;

  float *buf = (float *)dt_mipmap_cache_alloc(mbuf, img);
  if(!buf)
  {
    result = DT_IMAGEIO_CACHE_FULL;
    goto end;
  }

  // we don't support ASCII variants or P7 anymaps! thanks to magic numbers those shouldn't reach us anyway.
  if(head[1] == '4')
    result = _read_pbm(img, f, buf);
  else if(head[1] == '5')
    result = _read_pgm(img, f, buf);
  else if(head[1] == '6')
    result = _read_ppm(img, f, buf);

end:
  fclose(f);
  return result;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

