/*
    This file is part of darktable,
    Copyright (C) 2022 darktable developers.

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

#include <inttypes.h>

// We want to include the actual implementation from qoi.h
#define QOI_IMPLEMENTATION

// It makes no sense to include unused features
#define QOI_NO_STDIO

// This header file contains implementation of the QOI format
#include "common/qoi.h"

#include "develop/imageop.h"

#include "image.h"
#include "imageio.h"

dt_imageio_retval_t dt_imageio_open_qoi(dt_image_t *img, const char *filename, dt_mipmap_buffer_t *mbuf)
{
  FILE *f = g_fopen(filename, "rb");
  if(!f)
  {
    fprintf(stderr,"[qoi_open] cannot open file for read: %s\n", filename);
    return DT_IMAGEIO_LOAD_FAILED;
  }

  fseek(f, 0, SEEK_END);
  size_t filesize = ftell(f);
  fseek(f, 0, SEEK_SET);

  void *read_buffer = g_malloc(filesize);

  if(fread(read_buffer, 1, filesize, f) != filesize)
  {
    fclose(f);
    g_free(read_buffer);
    fprintf(stderr,"[qoi_open] failed to read %zu bytes from %s\n", filesize, filename);
    return DT_IMAGEIO_LOAD_FAILED;
  }
  fclose(f);

// void *qoi_decode(const void *data, int size, qoi_desc *desc, int channels);
  qoi_desc desc;
  uint8_t *int_RGBA_buf = qoi_decode(read_buffer, (int)filesize, &desc, 4);

  if(!int_RGBA_buf)
  {
    g_free(read_buffer);
    fprintf(stderr,"[qoi_open] failed to decode file: %s\n", filename);
    return DT_IMAGEIO_LOAD_FAILED;
  }

  img->width = desc.width;
  img->height = desc.height;
  img->buf_dsc.channels = 4;
  img->buf_dsc.datatype = TYPE_FLOAT;

  float *mipbuf = (float *)dt_mipmap_cache_alloc(mbuf, img);
  if(!mipbuf)
  {
    g_free(read_buffer);
    fprintf(stderr, "[qoi_open] could not alloc full buffer for image: %s\n", img->filename);
    return DT_IMAGEIO_CACHE_FULL;
  }

  uint8_t intval;
  float floatval;

#ifdef _OPENMP
#pragma omp parallel for private(intval, floatval)
#endif
  for(int i=0; i < desc.width * desc.height * 4; i++)
  {
    intval = *(int_RGBA_buf+i);
    floatval = intval / 255.f;
    *(mipbuf+i) = floatval;
  }

  img->buf_dsc.cst = IOP_CS_RGB;
  img->buf_dsc.filters = 0u;
  img->flags &= ~DT_IMAGE_RAW;
  img->flags &= ~DT_IMAGE_S_RAW;
  img->flags &= ~DT_IMAGE_HDR;
  img->flags |= DT_IMAGE_LDR;
  img->loader = LOADER_QOI;

  QOI_FREE(int_RGBA_buf);

  return DT_IMAGEIO_OK;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
