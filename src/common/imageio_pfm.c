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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "common/darktable.h"
#include "common/imageio_pfm.h"

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <math.h>
#include <assert.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

dt_imageio_retval_t dt_imageio_open_pfm(dt_image_t *img, const char *filename, dt_mipmap_buffer_t *mbuf)
{
  const char *ext = filename + strlen(filename);
  while(*ext != '.' && ext > filename) ext--;
  if(strcasecmp(ext, ".pfm")) return DT_IMAGEIO_FILE_CORRUPTED;
  FILE *f = fopen(filename, "rb");
  if(!f) return DT_IMAGEIO_FILE_CORRUPTED;
  int ret = 0;
  int cols = 3;
  char head[2] = { 'X', 'X' };
  ret = fscanf(f, "%c%c\n", head, head + 1);
  if(ret != 2 || head[0] != 'P') goto error_corrupt;
  if(head[1] == 'F')
    cols = 3;
  else if(head[1] == 'f')
    cols = 1;
  else
    goto error_corrupt;
  ret = fscanf(f, "%d %d\n%*[^\n]", &img->width, &img->height);
  if(ret != 2) goto error_corrupt;
  ret = fread(&ret, sizeof(char), 1, f);
  if(ret != 1) goto error_corrupt;
  ret = 0;

  float *buf = (float *)dt_mipmap_cache_alloc(mbuf, img);
  if(!buf) goto error_cache_full;

  if(cols == 3)
  {
    ret = fread(buf, 3 * sizeof(float), (size_t)img->width * img->height, f);
    for(size_t i = (size_t)img->width * img->height; i > 0; i--)
      for(int c = 0; c < 3; c++) buf[4 * (i - 1) + c] = fmaxf(0.0f, fminf(FLT_MAX, buf[3 * (i - 1) + c]));
  }
  else
    for(size_t j = 0; j < img->height; j++)
      for(size_t i = 0; i < img->width; i++)
      {
        ret = fread(buf + 4 * (img->width * j + i), sizeof(float), 1, f);
        buf[4 * (img->width * j + i) + 2] = buf[4 * (img->width * j + i) + 1]
            = buf[4 * (img->width * j + i) + 0];
      }
  float *line = (float *)calloc(4 * img->width, sizeof(float));
  for(size_t j = 0; j < img->height / 2; j++)
  {
    memcpy(line, buf + img->width * j * 4, 4 * sizeof(float) * img->width);
    memcpy(buf + img->width * j * 4, buf + img->width * (img->height - 1 - j) * 4,
           4 * sizeof(float) * img->width);
    memcpy(buf + img->width * (img->height - 1 - j) * 4, line, 4 * sizeof(float) * img->width);
  }
  free(line);
  fclose(f);
  return DT_IMAGEIO_OK;

error_corrupt:
  fclose(f);
  return DT_IMAGEIO_FILE_CORRUPTED;
error_cache_full:
  fclose(f);
  return DT_IMAGEIO_CACHE_FULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
