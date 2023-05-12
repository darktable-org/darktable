/*
    This file is part of darktable,
    Copyright (C) 2022-2023 darktable developers.

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

#include <webp/decode.h>
#include <webp/mux.h>

#include "common/image.h"
#include "develop/imageop.h"         // for IOP_CS_RGB
#include "imageio/imageio_common.h"

dt_imageio_retval_t dt_imageio_open_webp(dt_image_t *img, const char *filename, dt_mipmap_buffer_t *mbuf)
{
  int w, h;
  WebPMux *mux;
  WebPData wp_data;
  WebPData icc_profile;

  FILE *f = g_fopen(filename, "rb");
  if(!f)
  {
    dt_print(DT_DEBUG_ALWAYS,"[webp_open] cannot open file for read: %s\n", filename);
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
    dt_print(DT_DEBUG_ALWAYS,"[webp_open] failed to read %zu bytes from %s\n", filesize, filename);
    return DT_IMAGEIO_LOAD_FAILED;
  }
  fclose(f);

  // WebPGetInfo should tell us the image dimensions needed for darktable image buffer allocation
  if(!WebPGetInfo(read_buffer, filesize, &w, &h))
  {
    // If we couldn't get the webp metadata, then the file we're trying to read is most likely in
    // a different format (darktable just trying different loaders until it finds the right one).
    // We just have to return without complaining.
    g_free(read_buffer);
    return DT_IMAGEIO_LOAD_FAILED;
  }
  img->width = w;
  img->height = h;
  img->buf_dsc.channels = 4;
  img->buf_dsc.datatype = TYPE_FLOAT;

  float *mipbuf = (float *)dt_mipmap_cache_alloc(mbuf, img);
  if(!mipbuf)
  {
    g_free(read_buffer);
    dt_print(DT_DEBUG_ALWAYS, "[webp_open] could not alloc full buffer for image: %s\n", img->filename);
    return DT_IMAGEIO_CACHE_FULL;
  }

  uint8_t *int_RGBA_buf = WebPDecodeRGBA(read_buffer, filesize, &w, &h);
  if(!int_RGBA_buf)
  {
    g_free(read_buffer);
    dt_print(DT_DEBUG_ALWAYS,"[webp_open] failed to decode file: %s\n", filename);
    return DT_IMAGEIO_LOAD_FAILED;
  }

  uint8_t intval;
  float floatval;

#ifdef _OPENMP
#pragma omp parallel for private(intval, floatval)
#endif
  for(int i=0; i < w*h*4; i++)
  {
    intval = *(int_RGBA_buf+i);
    floatval = intval / 255.f;
    *(mipbuf+i) = floatval;
  }

  WebPFree(int_RGBA_buf);

  wp_data.bytes = (uint8_t *)read_buffer;
  wp_data.size = filesize;
  mux = WebPMuxCreate(&wp_data, 0); // 0 = data will NOT be copied to the mux object

  if(mux)
  {
    WebPMuxGetChunk(mux, "ICCP", &icc_profile);
    if(icc_profile.size)
    {
      img->profile_size = icc_profile.size;
      img->profile = (uint8_t *)g_malloc0(icc_profile.size);
      memcpy(img->profile, icc_profile.bytes, icc_profile.size);
    }
    WebPMuxDelete(mux);
  }

  g_free(read_buffer);

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
