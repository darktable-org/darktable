/*
    This file is part of darktable,
    copyright (c) 2010-2011 Henrik Andersson.

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
#include <glib/gstdio.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include "common/darktable.h"
#include "common/imageio_module.h"
#include "common/imageio.h"
#include "common/imageio_format.h"

DT_MODULE(1)

void init(dt_imageio_module_format_t *self)
{
}
void cleanup(dt_imageio_module_format_t *self)
{
}

int write_image(dt_imageio_module_data_t *ppm, const char *filename, const void *in_tmp, void *exif,
                int exif_len, int imgid, int num, int total)
{
  const uint16_t *in = (const uint16_t *)in_tmp;
  int status = 0;
  uint16_t *row = (uint16_t *)in;
  uint16_t swapped[3];
  FILE *f = fopen(filename, "wb");
  if(f)
  {
    (void)fprintf(f, "P6\n%d %d\n65535\n", ppm->width, ppm->height);
    for(int y = 0; y < ppm->height; y++)
    {
      for(int x = 0; x < ppm->width; x++)
      {
        for(int c = 0; c < 3; c++) swapped[c] = (0xff00 & (row[c] << 8)) | (row[c] >> 8);
        int cnt = fwrite(&swapped, sizeof(uint16_t), 3, f);
        if(cnt != 3) break;
        row += 4;
      }
    }
    fclose(f);
    status = 0;
  }
  return status;
}

size_t params_size(dt_imageio_module_format_t *self)
{
  return sizeof(dt_imageio_module_data_t);
}

void *get_params(dt_imageio_module_format_t *self)
{
  dt_imageio_module_data_t *d = (dt_imageio_module_data_t *)calloc(1, sizeof(dt_imageio_module_data_t));
  return d;
}

void free_params(dt_imageio_module_format_t *self, dt_imageio_module_data_t *params)
{
  free(params);
}

int set_params(dt_imageio_module_format_t *self, const void *params, const int size)
{
  if(size != self->params_size(self)) return 1;
  return 0;
}

int bpp(dt_imageio_module_data_t *p)
{
  return 16;
}

int levels(dt_imageio_module_data_t *p)
{
  return IMAGEIO_RGB | IMAGEIO_INT16;
}

const char *mime(dt_imageio_module_data_t *data)
{
  return "image/x-portable-pixmap";
}

const char *extension(dt_imageio_module_data_t *data)
{
  return "ppm";
}

const char *name()
{
  return _("PPM (16-bit)");
}

// TODO: some quality/compression stuff?
void gui_init(dt_imageio_module_format_t *self)
{
}
void gui_cleanup(dt_imageio_module_format_t *self)
{
}
void gui_reset(dt_imageio_module_format_t *self)
{
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
