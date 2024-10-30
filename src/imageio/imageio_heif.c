/*
    This file is part of darktable,
    Copyright (C) 2021-2024 darktable developers.

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

#include "common/image.h"

#include <libheif/heif.h>
#if LIBHEIF_HAVE_VERSION(1, 17, 0)
#include <libheif/heif_properties.h>
#endif

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
#include "imageio_common.h"
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
             "Unable to allocate HEIF context");
    return DT_IMAGEIO_CACHE_FULL;
  }

  err = heif_context_read_from_file(ctx, filename, NULL);
  if(err.code != heif_error_Ok)
  {
    ret = DT_IMAGEIO_LOAD_FAILED;
    if(err.code == heif_error_Unsupported_feature && err.subcode == heif_suberror_Unsupported_codec)
    {
      /* we want to feedback this to the user, so output to stderr */
      dt_print(DT_DEBUG_ALWAYS,
               "[imageio_heif] Unsupported codec for `%s'. "
               "Check if your libheif is built with HEVC and/or AV1 decoding support",
               filename);
      ret = DT_IMAGEIO_UNSUPPORTED_FEATURE;
    }
    else if(err.code != heif_error_Unsupported_filetype && err.subcode != heif_suberror_No_ftyp_box)
    {
      /* print debug info only if genuine HEIF */
      dt_print(DT_DEBUG_IMAGEIO, "Failed to read HEIF file [%s]: %s", filename, err.message);
      ret = DT_IMAGEIO_UNSUPPORTED_FORMAT;
    }
    goto out;
  }

  // HEIF may contain multiple images or none.
  const int num_images = heif_context_get_number_of_top_level_images(ctx);
  if(num_images == 0)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "No images found in HEIF file [%s]",
             filename);
    ret = DT_IMAGEIO_FILE_CORRUPTED;
    goto out;
  }

  // We can only process a single image
  err = heif_context_get_primary_image_handle(ctx, &handle);
  if(err.code != heif_error_Ok)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "Failed to read primary image from HEIF file [%s]",
             filename);
    ret = DT_IMAGEIO_UNSUPPORTED_FEATURE;
    goto out;
  }

  // Read Exif blob if Exiv2 did not succeed
  if(!img->exif_inited)
  {
    heif_item_id exif_id;
    int count = heif_image_handle_get_list_of_metadata_block_IDs(handle, "Exif", &exif_id, 1);
    if(count == 1)
    {
      const size_t exif_size = heif_image_handle_get_metadata_size(handle, exif_id);
      if(exif_size > 4)
      {
        uint8_t *exif_data = g_malloc0(exif_size);
        if(exif_data)
        {
          err = heif_image_handle_get_metadata(handle, exif_id, exif_data);
          if(err.code == heif_error_Ok)
          {
            const uint32_t exif_offset = exif_data[0] << 24
              | exif_data[1] << 16
              | exif_data[2] << 8
              | exif_data[3];
            if(exif_size > 4 + exif_offset)
              dt_exif_read_from_blob(img,
                                     exif_data + 4 + exif_offset,
                                     exif_size - 4 - exif_offset);
          }
          g_free(exif_data);
        }
      }
    }
  }

#if LIBHEIF_HAVE_VERSION(1, 16, 0)
  // Override any Exif orientation from HEIF irot/imir transformations.
  // TODO: Add user crop from HEIF clap transformation.
  heif_item_id id;
  heif_context_get_primary_image_ID(ctx, &id);
  heif_property_id transforms[3];
  int num_transforms = heif_item_get_transformation_properties(ctx, id, transforms, 3);
  int angle = 0;
  int flip = -1;
  for(int i = 0; i < num_transforms; ++i)
  {
    switch(heif_item_get_property_type(ctx, id, transforms[i]))
    {
      case heif_item_property_type_transform_rotation:
        angle = heif_item_get_property_transform_rotation_ccw(ctx, id, transforms[i]) / 90;
        break;
      case heif_item_property_type_transform_mirror:
        flip = heif_item_get_property_transform_mirror(ctx, id, transforms[i]);
        break;
      default:
        break;
    }
  }
  img->orientation = dt_image_transformation_to_flip_bits(angle, flip);
#endif

  struct heif_decoding_options *decode_options = heif_decoding_options_alloc();
  if(!decode_options)
  {
    ret = DT_IMAGEIO_LOAD_FAILED;
    goto out;
  }
  decode_options->ignore_transformations = TRUE;
  // Darktable only supports LITTLE_ENDIAN systems, so RRGGBB_LE should be fine
  err = heif_decode_image(handle,
                          &heif_img,
                          heif_colorspace_RGB,
                          heif_chroma_interleaved_RRGGBB_LE,
                          decode_options);
  heif_decoding_options_free(decode_options);
  if(err.code != heif_error_Ok)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "Failed to decode HEIF file [%s]",
             filename);
    ret = DT_IMAGEIO_FILE_CORRUPTED;
    goto out;
  }

  int rowbytes = 0;
  const uint8_t* data = heif_image_get_plane_readonly(heif_img,
                                                      heif_channel_interleaved,
                                                      &rowbytes);
  /*
  Get the image dimensions from the 'ispe' box. This is the original image dimensions
  without any transformations applied to it.
  Note that we use these functions due to use of ignore_transformations option. If we didn't use
  ignore_transformations, we'd have to use non-ispe versions of the "get dimensions" functions.
  */
  const size_t width = heif_image_handle_get_ispe_width(handle);
  const size_t height = heif_image_handle_get_ispe_height(handle);

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
             "Failed to allocate mipmap buffer for HEIF image [%s]",
             filename);
    ret = DT_IMAGEIO_CACHE_FULL;
    goto out;
  }

  img->buf_dsc.filters = 0u;
  img->flags &= ~DT_IMAGE_RAW;
  img->flags &= ~DT_IMAGE_S_RAW;

  // Get decoded pixel values bit depth (this is used to scale values to [0..1] range)
  const int decoded_values_bit_depth = heif_image_get_bits_per_pixel_range(heif_img,
                                                                           heif_channel_interleaved);
  // Get original pixel values bit depth by querying the luma channel depth
  // (this may differ from decoded values bit depth)
  const int original_values_bit_depth = heif_image_handle_get_luma_bits_per_pixel(handle);

  dt_print(DT_DEBUG_IMAGEIO,
             "Bit depth: '%d' for HEIF image [%s]",
             original_values_bit_depth,
             filename);

  // If original_values_bit_depth <= 8 it must be LDR image
  if(original_values_bit_depth > 8)
  {
    img->flags |= DT_IMAGE_HDR;
    img->flags &= ~DT_IMAGE_LDR;
  }
  else
  {
    img->flags |= DT_IMAGE_LDR;
    img->flags &= ~DT_IMAGE_HDR;
  }

  float max_channel_f = (float)((1 << decoded_values_bit_depth) - 1);

  const uint8_t *const restrict in = (const uint8_t *)data;

  DT_OMP_FOR_SIMD(collapse(2))
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
    if(img->profile)
    {
      err = heif_image_handle_get_raw_color_profile(handle, img->profile);
      if(err.code == heif_error_Ok)
        img->profile_size = icc_size;
    }
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
             "Unable to allocate HEIF context");
    goto out;
  }

  err = heif_context_read_from_file(ctx, filename, NULL);
  if(err.code != heif_error_Ok)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "Failed to read HEIF file [%s]",
             filename);
    goto out;
  }

  // HEIF may contain multiple images or none.
  const int num_images = heif_context_get_number_of_top_level_images(ctx);
  if(num_images == 0)
  {
        dt_print(DT_DEBUG_IMAGEIO,
             "No images found in HEIF file [%s]",
             filename);
    goto out;
  }

  // We can only process a single image
  err = heif_context_get_primary_image_handle(ctx, &handle);
  if(err.code != heif_error_Ok)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "Failed to read primary image from HEIF file [%s]",
             filename);
    goto out;
  }

  // Get profile information from HEIF file
  enum heif_color_profile_type profile_type = heif_image_handle_get_color_profile_type(handle);

  switch(profile_type)
  {
    case heif_color_profile_type_nclx:
      dt_print(DT_DEBUG_IMAGEIO,
             "Found NCLX color profile for HEIF file [%s]",
             filename);
      err = heif_image_handle_get_nclx_color_profile(handle, &profile_info_nclx);
      if(err.code != heif_error_Ok)
      {
        dt_print(DT_DEBUG_IMAGEIO,
                "Failed to get NCLX color profile data from HEIF file [%s]",
                filename);
        goto out;
      }
      cicp->color_primaries = (uint16_t)profile_info_nclx->color_primaries;
      cicp->transfer_characteristics = (uint16_t)profile_info_nclx->transfer_characteristics;
      cicp->matrix_coefficients = (uint16_t)profile_info_nclx->matrix_coefficients;

      /* fix up mistagged legacy AVIFs */
      if(profile_info_nclx->color_primaries == heif_color_primaries_ITU_R_BT_709_5)
      {
        gboolean over = FALSE;
        /* mistagged Rec. 709 AVIFs exported before dt 3.6 */
        if(profile_info_nclx->transfer_characteristics == heif_transfer_characteristic_ITU_R_BT_470_6_System_M
           && profile_info_nclx->matrix_coefficients == heif_matrix_coefficients_ITU_R_BT_709_5)
        {
          /* must be actual Rec. 709 instead of 2.2 gamma*/
          cicp->transfer_characteristics = (uint16_t)heif_transfer_characteristic_ITU_R_BT_709_5;
          over = TRUE;
        }

        if(over)
        {
          dt_print(DT_DEBUG_IMAGEIO, "Overriding nclx color profile for HEIF file `%s': 1/%d/%d to 1/%d/%d",
                   filename, profile_info_nclx->transfer_characteristics, profile_info_nclx->matrix_coefficients,
                   cicp->transfer_characteristics, cicp->matrix_coefficients);
        }
      }
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
      if(!icc_data)
      {
        goto out;
      }
      err = heif_image_handle_get_raw_color_profile(handle, icc_data);
      if(err.code != heif_error_Ok)
      {
        dt_print(DT_DEBUG_IMAGEIO,
                "Failed to read embedded ICC profile from HEIF image [%s]",
                filename);
        g_free(icc_data);
        goto out;
      }
      size = icc_size;
      *out = icc_data;
      break; /* heif_color_profile_type_rICC / heif_color_profile_type_prof */

    case heif_color_profile_type_not_present:
      dt_print(DT_DEBUG_IMAGEIO,
             "No color profile for HEIF file [%s]",
             filename);
      break; /* heif_color_profile_type_not_present */

    default:
      dt_print(DT_DEBUG_IMAGEIO,
                "Unknown color profile data from HEIF file [%s]",
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
