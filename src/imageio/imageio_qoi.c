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
#include "develop/imageop.h"         // for IOP_CS_RGB
#include "imageio/imageio_common.h"

static gboolean _qoi_validate(const uint8_t *bytes, const int size)
{
  if(size < QOI_HEADER_SIZE + (int)sizeof(qoi_padding)) return FALSE;

  int pos = 0;
  const unsigned int magic = qoi_read_32(bytes, &pos);
  const unsigned int width = qoi_read_32(bytes, &pos);
  const unsigned int height = qoi_read_32(bytes, &pos);
  const int channels = bytes[pos++];
  const int colorspace = bytes[pos++];
  if(magic != QOI_MAGIC || width == 0 || height == 0
     || channels < 3 || channels > 4 || colorspace > 1
     || height >= QOI_PIXELS_MAX / width)
    return FALSE;

  const int chunks_len = size - (int)sizeof(qoi_padding);
  if(memcmp(bytes + chunks_len, qoi_padding, sizeof(qoi_padding)) != 0)
    return FALSE;

  size_t remaining = (size_t)width * height;
  while(remaining > 0)
  {
    if(pos >= chunks_len) return FALSE;

    const int byte = bytes[pos++];
    size_t count = 1;
    if(byte == QOI_OP_RGB)
    {
      if(chunks_len - pos < 3) return FALSE;
      pos += 3;
    }
    else if(byte == QOI_OP_RGBA)
    {
      if(chunks_len - pos < 4) return FALSE;
      pos += 4;
    }
    else if((byte & QOI_MASK_2) == QOI_OP_LUMA)
    {
      if(chunks_len - pos < 1) return FALSE;
      pos++;
    }
    else if((byte & QOI_MASK_2) == QOI_OP_RUN)
      count = (byte & 0x3f) + 1;

    if(count > remaining) return FALSE;
    remaining -= count;
  }

  return pos == chunks_len;
}

dt_imageio_retval_t dt_imageio_open_qoi(dt_image_t *img,
                                        const char *filename,
                                        dt_mipmap_buffer_t *mbuf)
{
  FILE *f = g_fopen(filename, "rb");
  if(!f)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[qoi_open] cannot open file for read: %s",
             filename);
    return DT_IMAGEIO_FILE_NOT_FOUND;
  }

  if(fseek(f, 0, SEEK_END) != 0)
  {
    fclose(f);
    return DT_IMAGEIO_IOERROR;
  }
  const long file_size = ftell(f);
  if(file_size < 0 || fseek(f, 0, SEEK_SET) != 0)
  {
    fclose(f);
    return DT_IMAGEIO_IOERROR;
  }
  if(file_size < QOI_HEADER_SIZE + (long)sizeof(qoi_padding)
     || file_size > G_MAXINT)
  {
    fclose(f);
    return DT_IMAGEIO_FILE_CORRUPTED;
  }
  const size_t filesize = file_size;

  void *read_buffer = g_try_malloc(filesize);
  if(!read_buffer)
  {
    fclose(f);
    dt_print(DT_DEBUG_ALWAYS,
             "[qoi_open] failed to allocate read buffer for %s",
             filename);
    return DT_IMAGEIO_LOAD_FAILED;
  }

  if(fread(read_buffer, 1, filesize, f) != filesize)
  {
    fclose(f);
    g_free(read_buffer);
    dt_print(DT_DEBUG_ALWAYS,
             "[qoi_open] failed to read entire file (%zu bytes) from %s",
             filesize, filename);
    return DT_IMAGEIO_IOERROR;
  }
  fclose(f);

  qoi_desc desc;
  uint8_t *int_RGBA_buf = NULL;
  if(_qoi_validate(read_buffer, (int)filesize))
    int_RGBA_buf = qoi_decode(read_buffer, (int)filesize, &desc, 4);

  g_free(read_buffer);

  if(!int_RGBA_buf)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[qoi_open] failed to decode file: %s",
             filename);
    return DT_IMAGEIO_FILE_CORRUPTED;
  }

  img->width = desc.width;
  img->height = desc.height;
  img->buf_dsc.channels = 4;
  img->buf_dsc.datatype = TYPE_FLOAT;

  float *mipbuf = (float *)dt_mipmap_cache_alloc(mbuf, img);
  if(!mipbuf)
  {
    QOI_FREE(int_RGBA_buf);
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

  return DT_IMAGEIO_OK;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
