/*
    This file is part of darktable,
    Copyright (C) 2022-2024 darktable developers.

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
#include "imageio/qoi.h"

#include "common/image.h"
#include "develop/imageop.h"
#include "imageio/imageio_common.h"

dt_imageio_retval_t dt_imageio_open_qoi(dt_image_t *img,
                                        const char *filename,
                                        dt_mipmap_buffer_t *mbuf)
{
  // We shouldn't expect QOI images in files with an extension other than .qoi
  char *ext = g_strrstr(filename, ".");
  if(ext && g_ascii_strcasecmp(ext, ".qoi"))
    return DT_IMAGEIO_UNSUPPORTED_FORMAT;

  FILE *f = g_fopen(filename, "rb");
  if(!f)
  {
    dt_print(DT_DEBUG_ALWAYS,"[qoi_open] cannot open file for read: %s", filename);
    return DT_IMAGEIO_FILE_NOT_FOUND;
  }

  fseek(f, 0, SEEK_END);
  size_t filesize = ftell(f);
  fseek(f, 0, SEEK_SET);

  void *read_buffer = g_malloc(filesize);
  if(!read_buffer)
  {
    fclose(f);
    dt_print(DT_DEBUG_ALWAYS, "[qoi_open] failed to allocate buffer for %s", filename);
    return DT_IMAGEIO_LOAD_FAILED;
  }

  // Let's check whether the entire content of the file should be read into the buffer.
  // If we see that it's a non-QOI file, we'll save time by avoiding unnecessary reading
  // of a potentially large file into the buffer.
  if(fread(read_buffer, 1, 4, f) != 4)
  {
    fclose(f);
    g_free(read_buffer);
    dt_print(DT_DEBUG_ALWAYS, "[qoi_open] failed to read from %s", filename);
    // if we can't read even first 4 bytes, it's more like file disappeared
    return DT_IMAGEIO_FILE_CORRUPTED;
  }

  if(memcmp(read_buffer, "qoif", 4) != 0)
  {
    fclose(f);
    g_free(read_buffer);
    dt_print(DT_DEBUG_ALWAYS,
             "[qoi_open] no proper file header in %s", filename);
    return DT_IMAGEIO_UNSUPPORTED_FORMAT;
  }

  if(fread(read_buffer+4, 1, filesize-4, f) != filesize-4)
  {
    fclose(f);
    g_free(read_buffer);
    dt_print(DT_DEBUG_ALWAYS,
             "[qoi_open] failed to read %zu bytes from %s",
             filesize, filename);
    return DT_IMAGEIO_IOERROR;
  }
  fclose(f);

  qoi_desc desc;
  uint8_t *int_RGBA_buf = qoi_decode(read_buffer, (int)filesize, &desc, 4);

  if(!int_RGBA_buf)
  {
    g_free(read_buffer);
    dt_print(DT_DEBUG_ALWAYS,"[qoi_open] failed to decode file: %s", filename);
    return DT_IMAGEIO_FILE_CORRUPTED;
  }

  img->width = desc.width;
  img->height = desc.height;
  img->buf_dsc.channels = 4;
  img->buf_dsc.datatype = TYPE_FLOAT;

  float *mipbuf = (float *)dt_mipmap_cache_alloc(mbuf, img);
  if(!mipbuf)
  {
    g_free(read_buffer);
    dt_print(DT_DEBUG_ALWAYS,
             "[qoi_open] could not alloc full buffer for image: %s",
             img->filename);
    return DT_IMAGEIO_CACHE_FULL;
  }

  const size_t npixels = (size_t)desc.width * desc.height;

  DT_OMP_FOR()
  for(size_t index = 0; index < npixels * 4; index++)
  {
    mipbuf[index] = int_RGBA_buf[index] / 255.f;
  }

  img->buf_dsc.cst = IOP_CS_RGB;
  img->buf_dsc.filters = 0u;
  img->flags &= ~DT_IMAGE_RAW;
  img->flags &= ~DT_IMAGE_S_RAW;
  img->flags &= ~DT_IMAGE_HDR;
  img->flags |= DT_IMAGE_LDR;
  img->loader = LOADER_QOI;

  QOI_FREE(int_RGBA_buf);
  g_free(read_buffer);

  return DT_IMAGEIO_OK;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
