/*
    This file is part of darktable,
    Copyright (C) 2010-2023 darktable developers.

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

#include "common/darktable.h"
#include "imageio/imageio_common.h"
#include "imageio/imageio_module.h"
#include "imageio/format/imageio_format_api.h"

#include <glib/gstdio.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

DT_MODULE(1)

void init(dt_imageio_module_format_t *self)
{
}
void cleanup(dt_imageio_module_format_t *self)
{
}

int write_image(dt_imageio_module_data_t *ppm, const char *filename, const void *in_tmp,
                dt_colorspaces_color_profile_type_t over_type, const char *over_filename,
                void *exif, int exif_len, int imgid, int num, int total, struct dt_dev_pixelpipe_t *pipe,
                const gboolean export_masks)
{
  const uint16_t *in = (const uint16_t *)in_tmp;
  int status = 0;
  uint16_t *row = (uint16_t *)in;
  uint16_t swapped[3];
  FILE *f = g_fopen(filename, "wb");
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
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

