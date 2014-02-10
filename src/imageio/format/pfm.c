/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.
    copyright (c) 2011 henrik andersson.

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
#include "common/imageio.h"
#include "common/imageio_format.h"

DT_MODULE(1)

int write_image (dt_imageio_module_data_t *pfm, const char *filename, const void *in_tmp, void *exif, int exif_len, int imgid)
{
  const float *in = (const float*)in_tmp;
  int status = 0;
  FILE *f = fopen(filename, "wb");
  if(f)
  {
    (void)fprintf(f, "PF\n%d %d\n-1.0\n", pfm->width, pfm->height);
    for(int j=pfm->height-1; j>=0; j--)
    {
      for(int i=0; i<pfm->width; i++)
      {
        int cnt = fwrite(in + 4*((size_t)pfm->width*j + i), sizeof(float)*3, 1, f);
        if(cnt != 1) status = 1;
        else status = 0;
      }
    }
    fclose(f);
  }
  return status;
}

size_t
params_size(dt_imageio_module_format_t *self)
{
  return sizeof(dt_imageio_module_data_t);
}

void*
get_params(dt_imageio_module_format_t *self)
{
  dt_imageio_module_data_t *d = (dt_imageio_module_data_t *)malloc(sizeof(dt_imageio_module_data_t));
  memset(d,0,sizeof(dt_imageio_module_data_t));
  return d;
}

void
free_params(dt_imageio_module_format_t *self, dt_imageio_module_data_t *params)
{
  free(params);
}

int
set_params(dt_imageio_module_format_t *self, const void* params, int size)
{
  if(size != params_size(self)) return 1;
  return 0;
}

int bpp(dt_imageio_module_data_t *p)
{
  return 32;
}

int levels(dt_imageio_module_data_t *p)
{
  return IMAGEIO_RGB | IMAGEIO_FLOAT;
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
  return _("PFM (float)");
}

void init(dt_imageio_module_format_t *self) {}
void cleanup(dt_imageio_module_format_t *self) {}
void gui_init    (dt_imageio_module_format_t *self) {}
void gui_cleanup (dt_imageio_module_format_t *self) {}
void gui_reset   (dt_imageio_module_format_t *self) {}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
