/*
 *    This file is part of darktable,
 *    Copyright (C) 2016-2020 darktable developers.
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
#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

float *read_pfm(const char *filename, int *wd, int *ht)
{
  FILE *f = g_fopen(filename, "rb");

  if(!f)
  {
    fprintf(stderr, "can't open input file\n");
    return NULL;
  }

  char magic[2];
  char scale_factor_string[64] = { 0 };
  int width, height, cols, unused = 0;
  // using fscanf to read floats only really works with LANG=C :(
  unused = fscanf(f, "%c%c %d %d %63s%*[^\n]", &magic[0], &magic[1], &width, &height, scale_factor_string);
  if(magic[0] != 'P' || unused != 5 || fgetc(f) != '\n')
  {
    fprintf(stderr, "wrong input file format\n");
    fclose(f);
    return NULL;
  }
  if(magic[1] == 'F')
    cols = 3;
  else if(magic[1] == 'f')
    cols = 1;
  else
  {
    fprintf(stderr, "wrong input file format\n");
    fclose(f);
    return NULL;
  }

  float scale_factor = g_ascii_strtod(scale_factor_string, NULL);
  int swap_byte_order = (scale_factor >= 0.0) ^ (G_BYTE_ORDER == G_BIG_ENDIAN);

  float *image = (float *)dt_alloc_align_float((size_t)3 * width * height);
  if(!image)
  {
    fprintf(stderr, "error allocating memory\n");
    fclose(f);
    return NULL;
  }

  if(cols == 3)
  {
    int ret = fread(image, 3 * sizeof(float), (size_t)width * height, f);
    if(ret != width * height)
    {
      fprintf(stderr, "error reading PFM\n");
      dt_free_align(image);
      fclose(f);
      return NULL;
    }
    if(swap_byte_order)
    {
      for(size_t i = (size_t)width * height; i > 0; i--)
        for(int c = 0; c < 3; c++)
        {
          union {
            float f;
            guint32 i;
          } v;
          v.f = image[3 * (i - 1) + c];
          v.i = GUINT32_SWAP_LE_BE(v.i);
          image[3 * (i - 1) + c] = v.f;
        }
    }
  }
  else
    for(size_t j = 0; j < height; j++)
      for(size_t i = 0; i < width; i++)
      {
        union {
          float f;
          guint32 i;
        } v;
        int ret = fread(&v.f, sizeof(float), 1, f);
        if(ret != 1)
        {
          fprintf(stderr, "error reading PFM\n");
          dt_free_align(image);
          fclose(f);
          return NULL;
        }
        if(swap_byte_order) v.i = GUINT32_SWAP_LE_BE(v.i);
        image[3 * (width * j + i) + 2] = image[3 * (width * j + i) + 1] = image[3 * (width * j + i) + 0] = v.f;
      }
  float *line = (float *)calloc(3 * width, sizeof(float));
  for(size_t j = 0; j < height / 2; j++)
  {
    memcpy(line, image + width * j * 3, sizeof(float) * width * 3);
    memcpy(image + width * j * 3, image + width * (height - 1 - j) * 3, sizeof(float) * width * 3);
    memcpy(image + width * (height - 1 - j) * 3, line, sizeof(float) * width * 3);
  }
  free(line);
  fclose(f);

  if(wd) *wd = width;
  if(ht) *ht = height;
  return image;
}

void write_pfm(const char *filename, int width, int height, float *data)
{
  FILE *f = g_fopen(filename, "wb");
  if(f)
  {
    // INFO: per-line fwrite call seems to perform best. LebedevRI, 18.04.2014
    (void)fprintf(f, "PF\n%d %d\n-1.0\n", width, height);
    void *buf_line = dt_alloc_align_float((size_t)3 * width);
    for(int j = 0; j < height; j++)
    {
      // NOTE: pfm has rows in reverse order
      const int row_in = height - 1 - j;
      const float *in = data + 3 * (size_t)width * row_in;
      float *out = (float *)buf_line;
      for(int i = 0; i < width; i++, in += 3, out += 3)
      {
        memcpy(out, in, sizeof(float) * 3);
      }
      int cnt = fwrite(buf_line, sizeof(float) * 3, width, f);
      if(cnt != width) break;
    }
    dt_free_align(buf_line);
    buf_line = NULL;
    fclose(f);
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
