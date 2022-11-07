/*
 * This file is part of darktable,
 * Copyright (C) 2021 darktable developers.
 *
 *  darktable is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  darktable is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/image.h"
#include <libheif/heif.h>
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <inttypes.h>
#include <memory.h>
#include <stdio.h>
#include <strings.h>

#include "control/control.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/exif.h"
#include "control/conf.h"
#include "develop/develop.h"
#include "imageio.h"
#include "imageio_heif.h"


dt_imageio_retval_t dt_imageio_open_heif(dt_image_t *img,
                                         const char *filename,
                                         dt_mipmap_buffer_t *mbuf)
{
  dt_imageio_retval_t ret;
  struct heif_error err;
  struct heif_image_handle* handle = NULL;
  struct heif_image* heif_img = NULL;

  struct heif_context* ctx = heif_context_alloc();
  if(!ctx)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "Unable to allocate HEIF context\n");
    return DT_IMAGEIO_CACHE_FULL;
  }

  err = heif_context_read_from_file(ctx, filename, NULL);
  if(err.code != 0)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "Failed to read HEIF file [%s]\n",
             filename);
    ret = DT_IMAGEIO_FILE_CORRUPTED;
    switch(err.code)
    {
    case heif_error_Unsupported_filetype:
    case heif_error_Unsupported_feature:
      fprintf(stderr, "[imageio_heif] Unsupported file: `%s'! Is your libheif compiled with HEVC support?\n", filename);
      break;
    default:
      break;
    }

    goto out;
  }

  // HEIF may contain multiple images or none.
  const int num_images = heif_context_get_number_of_top_level_images(ctx);
  if(num_images == 0)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "No images found in HEIF file [%s]\n",
             filename);
    ret = DT_IMAGEIO_FILE_CORRUPTED;
    goto out;
  }

  // We can only process a single image
  err = heif_context_get_primary_image_handle(ctx, &handle);
  if(err.code != 0)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "Failed to read primary image from HEIF file [%s]\n",
             filename);
    ret = DT_IMAGEIO_FILE_CORRUPTED;
    goto out;
  }

  // Darktable only supports LITTLE_ENDIAN systems, so RRGGBB_LE should be fine
  err = heif_decode_image(handle, &heif_img, heif_colorspace_RGB, heif_chroma_interleaved_RRGGBB_LE, NULL);
  if(err.code != 0)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "Failed to decode HEIF file [%s]\n",
             filename);
    ret = DT_IMAGEIO_FILE_CORRUPTED;
    goto out;
  }

  int rowbytes = 0;
  const uint8_t* data = heif_image_get_plane_readonly(heif_img, heif_channel_interleaved, &rowbytes);
  const size_t width = heif_image_handle_get_width(handle);
  const size_t height = heif_image_handle_get_height(handle);

  /* Initialize cached image buffer */
  img->width = width;
  img->height = height;

  img->buf_dsc.channels = 4;
  img->buf_dsc.datatype = TYPE_FLOAT;
  img->buf_dsc.cst = IOP_CS_RGB;

  float *mipbuf = (float *)dt_mipmap_cache_alloc(mbuf, img);
  if(mipbuf == NULL)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "Failed to allocate mipmap buffer for HEIF image [%s]\n",
             filename);
    ret = DT_IMAGEIO_CACHE_FULL;
    goto out;
  }

  img->buf_dsc.filters = 0u;
  img->flags &= ~DT_IMAGE_RAW;
  img->flags &= ~DT_IMAGE_S_RAW;

  // Get pixel value bit depth (not storage bit depth)
  const int bit_depth = heif_image_get_bits_per_pixel_range(heif_img, heif_channel_interleaved);

  dt_print(DT_DEBUG_IMAGEIO,
             "Bit depth: '%d' for HEIF image [%s]\n",
             bit_depth,
             filename);

  /* This can be LDR or HDR, it depends on the ICC profile. But if bit_depth <= 8 it must be LDR. */
  if(bit_depth > 8)
  {
    img->flags |= DT_IMAGE_HDR;
    img->flags &= ~DT_IMAGE_LDR;
  }
  else
  {
    img->flags |= DT_IMAGE_LDR;
    img->flags &= ~DT_IMAGE_HDR;
  }

  float max_channel_f = (float)((1 << bit_depth) - 1);

  const uint8_t *const restrict in = (const uint8_t *)data;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(mipbuf, width, height, in, rowbytes, max_channel_f) \
  schedule(simd:static) \
  collapse(2)
#endif
  for(size_t y = 0; y < height; y++)
  {
    for(size_t x = 0; x < width; x++)
    {
        uint16_t *in_pixel = (uint16_t *)&in[(y * rowbytes) + (3 * sizeof(uint16_t) * x)];
        float *out_pixel = &mipbuf[(size_t)4 * ((y * width) + x)];

        /* max_channel_f is 1023.0f for 10bit */
        out_pixel[0] = ((float)in_pixel[0]) * (1.0f / max_channel_f);
        out_pixel[1] = ((float)in_pixel[1]) * (1.0f / max_channel_f);
        out_pixel[2] = ((float)in_pixel[2]) * (1.0f / max_channel_f);
        out_pixel[3] = 0.0f; /* alpha */
    }
  }

  /* Get the ICC profile if available */
  size_t icc_size = heif_image_handle_get_raw_color_profile_size(handle);
  if(icc_size)
  {
    img->profile = (uint8_t *)g_malloc0(icc_size);
    heif_image_handle_get_raw_color_profile(handle, img->profile);
    img->profile_size = icc_size;
  }

  img->loader = LOADER_HEIF;
  ret = DT_IMAGEIO_OK;

out:
  // cleanup handles
  heif_image_release(heif_img);
  heif_image_handle_release(handle);
  heif_context_free(ctx);

  return ret;
}


int dt_imageio_heif_read_profile(const char *filename,
                                uint8_t **out,
                                dt_colorspaces_cicp_t *cicp)
{
  /* set default return values */
  int size = 0;
  *out = NULL;
  cicp->color_primaries = (uint16_t)heif_color_primaries_unspecified;
  cicp->transfer_characteristics = (uint16_t)heif_transfer_characteristic_unspecified;
  cicp->matrix_coefficients = (uint16_t)heif_matrix_coefficients_unspecified;

  struct heif_image_handle* handle = NULL;
  struct heif_error err;
  struct heif_color_profile_nclx *profile_info_nclx = NULL;
  size_t icc_size = 0;
  uint8_t *icc_data = NULL;

  struct heif_context* ctx = heif_context_alloc();
  if(!ctx)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "Unable to allocate HEIF context\n");
    goto out;
  }

  err = heif_context_read_from_file(ctx, filename, NULL);
  if(err.code != 0)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "Failed to read HEIF file [%s]\n",
             filename);
    goto out;
  }

  // HEIF may contain multiple images or none.
  const int num_images = heif_context_get_number_of_top_level_images(ctx);
  if(num_images == 0)
  {
        dt_print(DT_DEBUG_IMAGEIO,
             "No images found in HEIF file [%s]\n",
             filename);
    goto out;
  }

  // We can only process a single image
  err = heif_context_get_primary_image_handle(ctx, &handle);
  if(err.code != 0)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "Failed to read primary image from HEIF file [%s]\n",
             filename);
    goto out;
  }

  // Get profile information from HEIF file
  enum heif_color_profile_type profile_type = heif_image_handle_get_color_profile_type(handle);

  switch(profile_type)
  {
    case heif_color_profile_type_nclx:
      dt_print(DT_DEBUG_IMAGEIO,
             "Found NCLX color profile for HEIF file [%s]\n",
             filename);
      err = heif_image_handle_get_nclx_color_profile(handle, &profile_info_nclx);
      if(err.code != 0)
      {
        dt_print(DT_DEBUG_IMAGEIO,
                "Failed to get NCLX color profile data from HEIF file [%s]\n",
                filename);
        goto out;
      }
      cicp->color_primaries = (uint16_t)profile_info_nclx->color_primaries;
      cicp->transfer_characteristics = (uint16_t)profile_info_nclx->transfer_characteristics;
      cicp->matrix_coefficients = (uint16_t)profile_info_nclx->matrix_coefficients;
      break; /* heif_color_profile_type_nclx */

    case heif_color_profile_type_rICC:
    case heif_color_profile_type_prof:
      icc_size = heif_image_handle_get_raw_color_profile_size(handle);
      if(icc_size == 0)
      {
        // image has no embedded ICC profile
        goto out;
      }
      icc_data = (uint8_t *)g_malloc0(sizeof(uint8_t) * icc_size);
      err = heif_image_handle_get_raw_color_profile(handle, icc_data);
      if(err.code != 0)
      {
        dt_print(DT_DEBUG_IMAGEIO,
                "Failed to read embedded ICC profile from HEIF image [%s]\n",
                filename);
        g_free(icc_data);
        goto out;
      }
      size = icc_size;
      *out = icc_data;
      break; /* heif_color_profile_type_rICC / heif_color_profile_type_prof */

    case heif_color_profile_type_not_present:
      dt_print(DT_DEBUG_IMAGEIO,
             "No color profile for HEIF file [%s]\n",
             filename);
      break; /* heif_color_profile_type_not_present */

    default:
      dt_print(DT_DEBUG_IMAGEIO,
                "Unknown color profile data from HEIF file [%s]\n",
                filename);
      break;
  }

out:
  // cleanup handles
  heif_nclx_color_profile_free(profile_info_nclx);
  heif_image_handle_release(handle);
  heif_context_free(ctx);

  return size;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
