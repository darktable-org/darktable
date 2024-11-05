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

#include <webp/decode.h>
#include <webp/mux.h>

#include "common/image.h"
#include "develop/imageop.h"         // for IOP_CS_RGB
#include "imageio/imageio_common.h"

dt_imageio_retval_t dt_imageio_open_webp(dt_image_t *img,
                                         const char *filename,
                                         dt_mipmap_buffer_t *mbuf)
{
  FILE *f = g_fopen(filename, "rb");
  if(!f)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[webp_open] cannot open file for read: %s",
             filename);
    return DT_IMAGEIO_FILE_NOT_FOUND;
  }

  fseek(f, 0, SEEK_END);
  size_t filesize = ftell(f);
  rewind(f);

  void *read_buffer = g_try_malloc(filesize);
  if(!read_buffer)
  {
    fclose(f);
    dt_print(DT_DEBUG_ALWAYS,
             "[webp_open] failed to allocate read buffer for %s",
             filename);
    return DT_IMAGEIO_LOAD_FAILED;
  }

  if(fread(read_buffer, 1, filesize, f) != filesize)
  {
    fclose(f);
    g_free(read_buffer);
    dt_print(DT_DEBUG_ALWAYS,
             "[webp_open] failed to read entire file (%zu bytes) from %s",
             filesize,
             filename);
    return DT_IMAGEIO_IOERROR;
  }
  fclose(f);

  // WebPGetInfo will tell us the image dimensions needed for buffers
  // allocation and calling the decoder
  int width, height;
  if(!WebPGetInfo(read_buffer, filesize, &width, &height))
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[webp_open] failed to parse header and get dimensions for %s",
             filename);
    g_free(read_buffer);
    return DT_IMAGEIO_LOAD_FAILED;
  }

  // The maximum pixel dimensions of a WebP image is 16383 x 16383,
  // so the number of pixels will never overflow int
  const int npixels = width * height;

  // libwebp can only decode into 8-bit integer channel format, so
  // we have to use an intermediate buffer from which we will then
  // perform the data presentation conversion to the output buffer
  uint8_t *int_RGBA_buffer = dt_alloc_align_uint8(npixels * 4);
  if(!int_RGBA_buffer)
  {
    g_free(read_buffer);
    dt_print(DT_DEBUG_ALWAYS,
             "[webp_open] failed to alloc RGBA buffer for %s",
             filename);
    return DT_IMAGEIO_LOAD_FAILED;
  }
  uint8_t *decoded = WebPDecodeRGBAInto(read_buffer,
                                        filesize,
                                        int_RGBA_buffer,
                                        npixels * 4,
                                        width * 4);
  if(!decoded)
  {
    g_free(read_buffer);
    dt_free_align(int_RGBA_buffer);
    dt_print(DT_DEBUG_ALWAYS,
             "[webp_open] failed to decode file: %s",
             filename);
    return DT_IMAGEIO_LOAD_FAILED;
  }

  // Try to get the embedded ICC profile if there is one
  WebPData wp_data;
  wp_data.bytes = (uint8_t *)read_buffer;
  wp_data.size = filesize;
  // 0 in the call below means that data will NOT be copied to the mux object
  WebPMux *mux = WebPMuxCreate(&wp_data, 0);

  if(mux)
  {
    WebPData icc_profile;
    WebPMuxGetChunk(mux, "ICCP", &icc_profile);
    if(icc_profile.size)
    {
      img->profile_size = icc_profile.size;
      img->profile = (uint8_t *)g_malloc0(icc_profile.size);
      memcpy(img->profile, icc_profile.bytes, icc_profile.size);
    }
    WebPMuxDelete(mux);
  }

  // We've done with decoding and retrieving the ICC profile
  // (successful or not), the file read buffer can be freed
  g_free(read_buffer);

  img->width = width;
  img->height = height;
  img->buf_dsc.channels = 4;
  img->buf_dsc.datatype = TYPE_FLOAT;

  float *mipbuf = (float *)dt_mipmap_cache_alloc(mbuf, img);
  if(!mipbuf)
  {
    g_free(read_buffer);
    dt_free_align(int_RGBA_buffer);
    dt_print(DT_DEBUG_ALWAYS,
             "[webp_open] could not alloc full buffer for image: %s",
             img->filename);
    return DT_IMAGEIO_CACHE_FULL;
  }

  DT_OMP_FOR()
  for(int i = 0; i < npixels; i++)
  {
    dt_aligned_pixel_t pix = {0.0f, 0.0f, 0.0f, 0.0f};
    for_three_channels(c)
      pix[c] = *(int_RGBA_buffer + i * 4 + c) / 255.f;
    copy_pixel_nontemporal(&mipbuf[i * 4], pix);
  }

  dt_free_align(int_RGBA_buffer);

  img->buf_dsc.cst = IOP_CS_RGB;
  img->buf_dsc.filters = 0u;
  img->flags &= ~DT_IMAGE_RAW;
  img->flags &= ~DT_IMAGE_S_RAW;
  img->flags &= ~DT_IMAGE_HDR;
  img->flags |= DT_IMAGE_LDR;
  img->loader = LOADER_WEBP;
  return DT_IMAGEIO_OK;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
