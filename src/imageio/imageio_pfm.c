/*
    This file is part of darktable,
    Copyright (C) 2009-2023 darktable developers.

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

dt_imageio_retval_t dt_imageio_open_pfm(dt_image_t *img, const char *filename, dt_mipmap_buffer_t *mbuf)
{
  const char *ext = filename + strlen(filename);
  while(*ext != '.' && ext > filename) ext--;
  if(strcasecmp(ext, ".pfm")) return DT_IMAGEIO_LOAD_FAILED;
  FILE *f = g_fopen(filename, "rb");
  if(!f) return DT_IMAGEIO_LOAD_FAILED;
  int ret = 0;
  int cols = 3;
  float scale_factor;
  char head[2] = { 'X', 'X' };
  ret = fscanf(f, "%c%c\n", head, head + 1);
  if(ret != 2 || head[0] != 'P') goto error_corrupt;
  if(head[1] == 'F')
    cols = 3;
  else if(head[1] == 'f')
    cols = 1;
  else
    goto error_corrupt;
  char width_string[10] = { 0 };
  char height_string[10] = { 0 };
  char scale_factor_string[64] = { 0 };
  ret = fscanf(f, "%9s %9s %63s%*[^\n]", width_string, height_string, scale_factor_string);
  if(ret != 3) goto error_corrupt;
  errno = 0;
  img->width = strtol(width_string, NULL, 0);
  img->height = strtol(height_string, NULL, 0);
  scale_factor = g_ascii_strtod(scale_factor_string, NULL);
  if(errno != 0) goto error_corrupt;
  if(img->width <= 0 || img->height <= 0 ) goto error_corrupt;
  ret = fread(&ret, sizeof(char), 1, f);
  if(ret != 1) goto error_corrupt;
  ret = 0;

  int swap_byte_order = (scale_factor >= 0.0) ^ (G_BYTE_ORDER == G_BIG_ENDIAN);

  float *buf = (float *)dt_mipmap_cache_alloc(mbuf, img);
  if(!buf) goto error_cache_full;

  if(cols == 3)
  {
    ret = fread(buf, 3 * sizeof(float), (size_t)img->width * img->height, f);
    for(size_t i = (size_t)img->width * img->height; i > 0; i--)
      for(int c = 0; c < 3; c++)
      {
        union { float f; guint32 i; } v;
        v.f = buf[3 * (i - 1) + c];
        if(swap_byte_order) v.i = GUINT32_SWAP_LE_BE(v.i);
        buf[4 * (i - 1) + c] = v.f;
      }
  }
  else
    for(size_t j = 0; j < img->height; j++)
      for(size_t i = 0; i < img->width; i++)
      {
        union { float f; guint32 i; } v;
        ret = fread(&v.f, sizeof(float), 1, f);
        if(swap_byte_order) v.i = GUINT32_SWAP_LE_BE(v.i);
        buf[4 * (img->width * j + i) + 2] = buf[4 * (img->width * j + i) + 1]
            = buf[4 * (img->width * j + i) + 0] = v.f;
      }
  float *line = (float *)calloc(4 * img->width, sizeof(float));
  if(line == NULL) goto error_cache_full;
  for(size_t j = 0; j < img->height / 2; j++)
  {
    memcpy(line, buf + img->width * j * 4, sizeof(float) * 4 * img->width);
    memcpy(buf + img->width * j * 4, buf + img->width * (img->height - 1 - j) * 4,
           sizeof(float) * 4 * img->width);
    memcpy(buf + img->width * (img->height - 1 - j) * 4, line, sizeof(float) * 4 * img->width);
  }
  free(line);
  fclose(f);
  img->loader = LOADER_PFM;
  return DT_IMAGEIO_OK;

error_corrupt:
  fclose(f);
  return DT_IMAGEIO_LOAD_FAILED;
error_cache_full:
  fclose(f);
  return DT_IMAGEIO_CACHE_FULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

