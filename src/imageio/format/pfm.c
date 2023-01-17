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
#include "imageio/imageio.h"
#include "imageio/imageio_module.h"
#include "imageio/format/imageio_format_api.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE(1)

int write_image(dt_imageio_module_data_t *data, const char *filename, const void *ivoid,
                dt_colorspaces_color_profile_type_t over_type, const char *over_filename,
                void *exif, int exif_len, int imgid, int num, int total, struct dt_dev_pixelpipe_t *pipe,
                const gboolean export_masks)
{
  const dt_imageio_module_data_t *const pfm = data;
  int status = 0;
  FILE *f = g_fopen(filename, "wb");
  if(f)
  {
    // align pfm header to sse, assuming the file will
    // be mmapped to page boundaries.
    char header[1024];
    snprintf(header, 1024, "PF\n%d %d\n-1.0", pfm->width, pfm->height);
    size_t len = strlen(header);
    fprintf(f, "PF\n%d %d\n-1.0", pfm->width, pfm->height);
    ssize_t off = 0;
    while((len + 1 + off) & 0xf) off++;
    while(off-- > 0) fprintf(f, "0");
    fprintf(f, "\n");
    void *buf_line = dt_alloc_align_float((size_t)3 * pfm->width);
    for(int j = 0; j < pfm->height; j++)
    {
      // NOTE: pfm has rows in reverse order
      const int row_in = pfm->height - 1 - j;
      const float *in = (const float *)ivoid + 4 * (size_t)pfm->width * row_in;
      float *out = (float *)buf_line;
      for(int i = 0; i < pfm->width; i++, in += 4, out += 3)
      {
        memcpy(out, in, sizeof(float) * 3);
      }
      // INFO: per-line fwrite call seems to perform best. LebedevRI, 18.04.2014
      int cnt = fwrite(buf_line, sizeof(float) * 3, pfm->width, f);
      if(cnt != pfm->width)
        status = 1;
      else
        status = 0;
    }
    dt_free_align(buf_line);
    buf_line = NULL;
    fclose(f);
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

int set_params(dt_imageio_module_format_t *self, const void *params, int size)
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

const char *mime(dt_imageio_module_data_t *data)
{
  return "image/x-portable-floatmap";
}

const char *extension(dt_imageio_module_data_t *data)
{
  return "pfm";
}

const char *name()
{
  return _("PFM");
}

void init(dt_imageio_module_format_t *self)
{
}
void cleanup(dt_imageio_module_format_t *self)
{
}
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
