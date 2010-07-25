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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "common/darktable.h"
#include "common/imageio_module.h"

DT_MODULE(1)

#if 0
dt_imageio_retval_t dt_imageio_open_pfm(dt_image_t *img, const char *filename)
{
  const char *ext = filename + strlen(filename);
  while(*ext != '.' && ext > filename) ext--;
  if(strncmp(ext, ".pfm", 4) && strncmp(ext, ".PFM", 4) && strncmp(ext, ".Pfm", 4)) return DT_IMAGEIO_FILE_CORRUPTED;
  FILE *f = fopen(filename, "rb");
  if(!f) return DT_IMAGEIO_FILE_CORRUPTED;
  int ret = 0;
  int cols = 3;
  char head[2] = {'X', 'X'};
  ret = fscanf(f, "%c%c\n", head, head+1);
  if(ret != 2 || head[0] != 'P') goto error_corrupt;
  if(head[1] == 'F') cols = 3;
  else if(head[1] == 'f') cols = 1;
  else goto error_corrupt;
  ret = fscanf(f, "%d %d\n%*[^\n]\n", &img->width, &img->height);
  if(ret != 2) goto error_corrupt;

  if(dt_image_alloc(img, DT_IMAGE_FULL)) goto error_cache_full;
  dt_image_check_buffer(img, DT_IMAGE_FULL, 3*img->width*img->height*sizeof(uint8_t));
  if(cols == 3) ret = fread(img->pixels, 3*sizeof(float), img->width*img->height, f);
  else for(int j=0; j < img->height; j++)
    for(int i=0; i < img->width; i++)
    {
      ret = fread(img->pixels + 3*(img->width*j + i), sizeof(float), 1, f);
      img->pixels[3*(img->width*j + i) + 2] =
      img->pixels[3*(img->width*j + i) + 1] =
      img->pixels[3*(img->width*j + i) + 0];
    }
  // repair nan/inf etc
  for(int i=0; i < img->width*img->height*3; i++) img->pixels[i] = fmaxf(0.0f, fminf(10000.0, img->pixels[i]));
  float *line = (float *)malloc(sizeof(float)*3*img->width);
  for(int j=0; j < img->height/2; j++)
  {
    memcpy(line,                                         img->pixels + img->width*j*3,                 3*sizeof(float)*img->width);
    memcpy(img->pixels + img->width*j*3,                 img->pixels + img->width*(img->height-1-j)*3, 3*sizeof(float)*img->width);
    memcpy(img->pixels + img->width*(img->height-1-j)*3, line,                                         3*sizeof(float)*img->width);
  }
  free(line);
  dt_image_release(img, DT_IMAGE_FULL, 'w');
  return DT_IMAGEIO_OK;

error_corrupt:
  fclose(f);
  return DT_IMAGEIO_FILE_CORRUPTED;
error_cache_full:
  fclose(f);
  return DT_IMAGEIO_CACHE_FULL;
}
#endif

int write_image (dt_imageio_module_data_t *pfm, const char *filename, const float *in, void *exif, int exif_len, int imgid)
{
  int status = 0;
  FILE *f = fopen(filename, "wb");
  if(f)
  {
    (void)fprintf(f, "PF\n%d %d\n-1.0\n", pfm->width, pfm->height);
    for(int j=pfm->height-1;j>=0;j--)
    {
      int cnt = fwrite(in + 3*pfm->width*j, sizeof(float)*3, pfm->width, f);
      if(cnt != pfm->width) status = 1;
      else status = 0;
    }
    fclose(f);
  }
  return status;
}

void*
get_params(dt_imageio_module_format_t *self, int *size)
{
  *size = sizeof(dt_imageio_module_data_t);
  dt_imageio_module_data_t *d = (dt_imageio_module_data_t *)malloc(sizeof(dt_imageio_module_data_t));
  return d;
}

void
free_params(dt_imageio_module_format_t *self, void *params)
{
  free(params);
}

int
set_params(dt_imageio_module_format_t *self, void* params, int size)
{
  if(size != sizeof(dt_imageio_module_data_t)) return 1;
  return 0;
}

int bpp(dt_imageio_module_data_t *p)
{
  return 32;
}

const char*
mime(dt_imageio_module_data_t *data)
{
  return "image/x-portable-floatmap";
}
 
const char*
extension(dt_imageio_module_data_t *data)
{
  return "pfm";
}

const char*
name ()
{
  return _("float pfm");
}

void gui_init    (dt_imageio_module_format_t *self) {}
void gui_cleanup (dt_imageio_module_format_t *self) {}
void gui_reset   (dt_imageio_module_format_t *self) {}

