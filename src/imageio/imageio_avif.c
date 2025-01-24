/*
    This file is part of darktable,
    Copyright (C) 2019-2024 darktable developers.

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

#include <avif/avif.h>
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <inttypes.h>
#include <memory.h>
#include <stdio.h>
#include <strings.h>

#include "control/control.h"
#include "common/image.h"
#include "common/exif.h"
#include "control/conf.h"
#include "develop/imageop.h"  // for IOP_CS_RGB
#include "imageio_common.h"
#include "imageio_avif.h"

dt_imageio_retval_t dt_imageio_open_avif(dt_image_t *img,
                                         const char *filename,
                                         dt_mipmap_buffer_t *mbuf)
{
  dt_imageio_retval_t ret;
  avifImage *avif_image = avifImageCreateEmpty();
  avifDecoder *decoder = avifDecoderCreate();
  avifRGBImage rgb = { .format = AVIF_RGB_FORMAT_RGB, };
  avifResult result;

  if(decoder == NULL || avif_image == NULL)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "[avif_open] failed to create decoder or image struct for '%s'",
             filename);
    ret = DT_IMAGEIO_LOAD_FAILED;
    goto out;
  }

  // Be permissive so we can load even slightly-offspec files
  decoder->strictFlags = AVIF_STRICT_DISABLED;

  result = avifDecoderReadFile(decoder, avif_image, filename);
  if(result != AVIF_RESULT_OK)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "[avif_open] failed to parse '%s': %s",
             filename,
             avifResultToString(result));
    ret = DT_IMAGEIO_FILE_CORRUPTED;
    goto out;
  }

  // Read Exif blob if Exiv2 did not succeed
  // (for example, because it was built without the required feature)
  if(!img->exif_inited)
  {
    avifRWData *exif = &avif_image->exif;
    if(exif && exif->size > 0)
    {
      // Workaround for non-zero offset not handled by libavif as of 0.11.1
      size_t offset = 0;
#if AVIF_VERSION <= 110100
      while(offset < exif->size - 1
            && ((exif->data[offset] != 'I' && exif->data[offset] != 'M')
                || exif->data[offset] != exif->data[offset + 1]))
        ++offset;
#else
      result = avifGetExifTiffHeaderOffset(exif->data, exif->size, &offset);
      if(result != AVIF_RESULT_OK)
      {
        dt_print(DT_DEBUG_IMAGEIO,
                 "[avif_open] failed to read tiff header from '%s': %s",
                 filename,
                 avifResultToString(result));
        ret = DT_IMAGEIO_LOAD_FAILED;
        goto out;
      }
#endif
      dt_exif_read_from_blob(img, exif->data + offset, exif->size - offset);
    }
  }

  // Override any Exif orientation from AVIF irot/imir transformations
  // TODO: Add user crop from AVIF clap transformation
  const int angle = avif_image->transformFlags & AVIF_TRANSFORM_IROT
                    ? avif_image->irot.angle
                    : 0;
  const int flip = avif_image->transformFlags & AVIF_TRANSFORM_IMIR
#if AVIF_VERSION <= 110100
                   ? avif_image->imir.mode
#else
                   ? avif_image->imir.axis
#endif
                   : -1;
  img->orientation = dt_image_transformation_to_flip_bits(angle, flip);

  avifRGBImageSetDefaults(&rgb, avif_image);

  rgb.format = AVIF_RGB_FORMAT_RGB;

#if AVIF_VERSION > 110100
  result = avifRGBImageAllocatePixels(&rgb);
  if(result != AVIF_RESULT_OK)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "[avif_open] failed to allocate pixels for '%s' : %s",
             filename,
             avifResultToString(result));
    ret = DT_IMAGEIO_LOAD_FAILED;
    goto out;
  }
#else
  avifRGBImageAllocatePixels(&rgb);
#endif

  result = avifImageYUVToRGB(avif_image, &rgb);
  if(result != AVIF_RESULT_OK)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "[avif_open] failed to convert '%s' from YUV to RGB: %s",
             filename,
             avifResultToString(result));
    ret = DT_IMAGEIO_LOAD_FAILED;
    goto out;
  }

  const size_t width = rgb.width;
  const size_t height = rgb.height;
  const size_t bit_depth = rgb.depth;

  img->width = width;
  img->height = height;
  img->buf_dsc.channels = 4;
  img->buf_dsc.datatype = TYPE_FLOAT;
  img->buf_dsc.cst = IOP_CS_RGB;

  float *mipbuf = (float *)dt_mipmap_cache_alloc(mbuf, img);
  if(mipbuf == NULL)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "[avif_open] failed to allocate mipmap buffer for '%s'",
             filename);
    ret = DT_IMAGEIO_CACHE_FULL;
    goto out;
  }

  img->buf_dsc.filters = 0u;
  img->flags &= ~DT_IMAGE_RAW;
  img->flags &= ~DT_IMAGE_S_RAW;

  const float max_channel_f = (float)((1 << bit_depth) - 1);

  const size_t rowbytes = rgb.rowBytes;

  const uint8_t *const restrict in = (const uint8_t *)rgb.pixels;

  switch(bit_depth) {
  case 12:
  case 10: {
    img->flags |= DT_IMAGE_HDR;
    img->flags &= ~DT_IMAGE_LDR;
    DT_OMP_FOR_SIMD(collapse(2))
    for(size_t y = 0; y < height; y++)
    {
      for(size_t x = 0; x < width; x++)
      {
          uint16_t *in_pixel = (uint16_t *)&in[(y * rowbytes)
                                               + (3 * sizeof(uint16_t) * x)];
          float *out_pixel = &mipbuf[(size_t)4 * ((y * width) + x)];

          // max_channel_f is 1023.0f for 10bit, 4095.0f for 12bit
          out_pixel[0] = ((float)in_pixel[0]) * (1.0f / max_channel_f);
          out_pixel[1] = ((float)in_pixel[1]) * (1.0f / max_channel_f);
          out_pixel[2] = ((float)in_pixel[2]) * (1.0f / max_channel_f);
          out_pixel[3] = 0.0f; // alpha
      }
    }
    break;
  }
  case 8: {
    img->flags |= DT_IMAGE_LDR;
    img->flags &= ~DT_IMAGE_HDR;
    DT_OMP_FOR_SIMD(collapse(2))
    for(size_t y = 0; y < height; y++)
    {
      for(size_t x = 0; x < width; x++)
      {
          uint8_t *in_pixel = (uint8_t *)&in[(y * rowbytes)
                                             + (3 * sizeof(uint8_t) * x)];
          float *out_pixel = &mipbuf[(size_t)4 * ((y * width) + x)];

          // max_channel_f is 255.0f for 8bit
          out_pixel[0] = (float)(in_pixel[0]) * (1.0f / max_channel_f);
          out_pixel[1] = (float)(in_pixel[1]) * (1.0f / max_channel_f);
          out_pixel[2] = (float)(in_pixel[2]) * (1.0f / max_channel_f);
          out_pixel[3] = 0.0f; // alpha
      }
    }
    break;
  }
  default:
    dt_print(DT_DEBUG_IMAGEIO,
             "[avif_open] invalid bit depth for '%s'",
             filename);
    ret = DT_IMAGEIO_CACHE_FULL;
    goto out;
  }

  // Get the ICC profile if available
  avifRWData *icc = &avif_image->icc;
  if(icc->size && icc->data)
  {
    img->profile = g_try_malloc0(icc->size);
    if(img->profile)
    {
      memcpy(img->profile, icc->data, icc->size);
      img->profile_size = icc->size;
    }
  }

  img->loader = LOADER_AVIF;
  ret = DT_IMAGEIO_OK;

out:
  avifImageDestroy(avif_image);
  avifDecoderDestroy(decoder);
  avifRGBImageFreePixels(&rgb);

  return ret;
}


int dt_imageio_avif_read_profile(const char *filename,
                                 uint8_t **out,
                                 dt_colorspaces_cicp_t *cicp)
{
  // Set default return values
  int size = 0;
  *out = NULL;
  cicp->color_primaries = AVIF_COLOR_PRIMARIES_UNSPECIFIED;
  cicp->transfer_characteristics = AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED;
  cicp->matrix_coefficients = AVIF_MATRIX_COEFFICIENTS_UNSPECIFIED;

  avifDecoder *decoder = avifDecoderCreate();
  avifImage *avif_image = avifImageCreateEmpty();
  avifResult result;

  if(decoder == NULL || avif_image == NULL)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "[avif read profile] failed to create decoder or image struct for '%s'",
             filename);
    goto out;
  }

  result = avifDecoderReadFile(decoder, avif_image, filename);
  if(result != AVIF_RESULT_OK)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "[avif read profile] failed to parse '%s': %s",
             filename,
             avifResultToString(result));
    goto out;
  }

  avifRWData *icc = &avif_image->icc;
  if(icc->size && icc->data)
  {
    *out = g_try_malloc0(icc->size);
    if(*out)
    {
      memcpy(*out, icc->data, icc->size);
      size = icc->size;
    }
  }
  else
  {
    cicp->color_primaries = avif_image->colorPrimaries;
    cicp->transfer_characteristics = avif_image->transferCharacteristics;
    cicp->matrix_coefficients = avif_image->matrixCoefficients;

    // Fix up mistagged legacy AVIFs
    if(avif_image->colorPrimaries == AVIF_COLOR_PRIMARIES_BT709)
    {
      gboolean over = FALSE;
      // Detect mistagged Rec. 709 AVIFs exported before darktable 3.6
      if(avif_image->transferCharacteristics == AVIF_TRANSFER_CHARACTERISTICS_BT470M
         && avif_image->matrixCoefficients == AVIF_MATRIX_COEFFICIENTS_BT709)
      {
        // Must be actual Rec. 709 instead of 2.2 gamma
        cicp->transfer_characteristics = AVIF_TRANSFER_CHARACTERISTICS_BT709;
        over = TRUE;
      }

      if(over)
      {
        dt_print(DT_DEBUG_IMAGEIO,
                 "[avif_open] overriding nclx color profile for '%s': 1/%d/%d to 1/%d/%d",
                 filename,
                 avif_image->transferCharacteristics,
                 avif_image->matrixCoefficients,
                 cicp->transfer_characteristics,
                 cicp->matrix_coefficients);
      }
    }
  }

out:
  avifImageDestroy(avif_image);
  avifDecoderDestroy(decoder);

  return size;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
