/*
 * This file is part of darktable,
 *
 *  Copyright (c) 2019      Andreas Schneider
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
#include <avif/avif.h>
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
#include "imageio_avif.h"

#include <avif/avif.h>

static dt_imageio_retval_t read_image(const char *filename, avifROData *raw)
{
  size_t nread;
  size_t avif_file_size;
  FILE *f = NULL;
  avifRWData raw_data = AVIF_DATA_EMPTY;
  dt_imageio_retval_t ret;
  int rc;
  const char *ext = strrchr(filename, '.');
  int cmp;

  cmp = strncmp(ext, ".avif", 5);
  if (cmp != 0) {
    return DT_IMAGEIO_FILE_CORRUPTED;
  }

  f = g_fopen(filename, "rb");
  if (f == NULL) {
    return DT_IMAGEIO_FILE_NOT_FOUND;
  }

  rc = fseek(f, 0, SEEK_END);
  if (rc != 0) {
    ret = DT_IMAGEIO_FILE_CORRUPTED;
    goto out;
  }
  avif_file_size = ftell(f);
  if (avif_file_size < 10) {
    ret = DT_IMAGEIO_FILE_CORRUPTED;
    goto out;
  }
  rc = fseek(f, 0, SEEK_SET);
  if (rc != 0) {
    ret = DT_IMAGEIO_FILE_CORRUPTED;
    goto out;
  }

  avifRWDataRealloc(&raw_data, avif_file_size);
  if (raw_data.data == NULL) {
    ret = DT_IMAGEIO_FILE_CORRUPTED;
    goto out;
  }

  nread = fread(raw_data.data, 1, raw_data.size, f);
  if (nread != avif_file_size) {
    ret = DT_IMAGEIO_FILE_CORRUPTED;
    goto out;
  }

  raw->data = raw_data.data;
  raw->size = raw_data.size;

  ret = DT_IMAGEIO_OK;
out:
  fclose(f);

  return ret;
}

dt_imageio_retval_t dt_imageio_open_avif(dt_image_t *img,
                                         const char *filename,
                                         dt_mipmap_buffer_t *mbuf)
{
  dt_imageio_retval_t ret;
  avifROData raw = AVIF_DATA_EMPTY;
  avifImage *avif = NULL;
  avifResult result;

  ret = read_image(filename, &raw);
  if (ret != DT_IMAGEIO_OK) {
    dt_print(DT_DEBUG_IMAGEIO,
             "Failed to read image [%s]\n",
             filename);
    return ret;
  }

  avifBool ok = avifPeekCompatibleFileType(&raw);
  if (!ok) {
    dt_print(DT_DEBUG_IMAGEIO,
             "Invalid avif image [%s]\n",
             filename);
    ret = DT_IMAGEIO_FILE_CORRUPTED;
    goto out;
  }

  avifDecoder *decoder = avifDecoderCreate();
  if (decoder == NULL) {
    dt_print(DT_DEBUG_IMAGEIO,
             "Failed to create AVIF decoder for image [%s]\n",
             filename);
    ret = DT_IMAGEIO_FILE_CORRUPTED;
    goto out;
  }

  result = avifDecoderParse(decoder, &raw);
  if (result != AVIF_RESULT_OK) {
    dt_print(DT_DEBUG_IMAGEIO,
             "Failed to parse AVIF image [%s]: %s\n",
             filename, avifResultToString(result));
    ret = DT_IMAGEIO_FILE_CORRUPTED;
    goto out;
  }
  if (decoder->imageCount > 1) {
    dt_control_log(_("image '%s' has more than one frame!"), filename);
  }
  result = avifDecoderNthImage(decoder, 0);
  if (result != AVIF_RESULT_OK) {
    dt_print(DT_DEBUG_IMAGEIO,
             "Failed to decode first frame of AVIF image [%s]: %s\n",
             filename, avifResultToString(result));
    ret = DT_IMAGEIO_FILE_CORRUPTED;
    goto out;
  }

  avif = decoder->image;

  result = avifImageYUVToRGB(avif);
  if (result != AVIF_RESULT_OK) {
    dt_print(DT_DEBUG_IMAGEIO,
             "Failed to convert AVIF image [%s] from YUV to RGB: %s\n",
             filename, avifResultToString(result));
    ret = DT_IMAGEIO_FILE_CORRUPTED;
    goto out;
  }

  const size_t width = avif->width;
  const size_t height = avif->height;
  /* If `> 8', all plane ptrs are 'uint16_t *' */
  const size_t bit_depth = avif->depth;

  /* Initialize cached image buffer */
  img->width = width;
  img->height = height;

  img->buf_dsc.channels = 4;
  img->buf_dsc.datatype = TYPE_FLOAT;
  img->buf_dsc.cst = iop_cs_rgb;

  float *mipbuf = (float *)dt_mipmap_cache_alloc(mbuf, img);
  if (mipbuf == NULL) {
    dt_print(DT_DEBUG_IMAGEIO,
             "Failed to allocate mipmap buffer for AVIF image [%s]\n",
             filename);
    ret = DT_IMAGEIO_CACHE_FULL;
    goto out;
  }

  /* This can be LDR or HDR, it depends on the ICC profile. */
  img->flags &= ~DT_IMAGE_RAW;
  img->flags |= DT_IMAGE_HDR;

  const float max_channel_f = (float)((1 << bit_depth) - 1);

  const size_t R_rowbytes = avif->rgbRowBytes[AVIF_CHAN_R];
  const size_t G_rowbytes = avif->rgbRowBytes[AVIF_CHAN_G];
  const size_t B_rowbytes = avif->rgbRowBytes[AVIF_CHAN_B];

  const uint8_t *const restrict R_plane8 = (const uint8_t *)avif->rgbPlanes[AVIF_CHAN_R];
  const uint8_t *const restrict G_plane8 = (const uint8_t *)avif->rgbPlanes[AVIF_CHAN_G];
  const uint8_t *const restrict B_plane8 = (const uint8_t *)avif->rgbPlanes[AVIF_CHAN_B];

  switch (bit_depth) {
  case 12:
  case 10: {
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(mipbuf, width, height, R_plane8, G_plane8, B_plane8, \
                      R_rowbytes, G_rowbytes, B_rowbytes, max_channel_f) \
  schedule(simd:static) \
  collapse(2)
#endif
    for (size_t y = 0; y < height; y++) {
      for (size_t x = 0; x < width; x++) {
          float *pixel = &mipbuf[(size_t)4 * ((y * width) + x)];

          /* max_channel_f is 255.0f for 8bit */
          pixel[0] = ((float)*((uint16_t *)&R_plane8[(x * 2) + (y * R_rowbytes)])) * (1.0f / max_channel_f);
          pixel[1] = ((float)*((uint16_t *)&G_plane8[(x * 2) + (y * G_rowbytes)])) * (1.0f / max_channel_f);
          pixel[2] = ((float)*((uint16_t *)&B_plane8[(x * 2) + (y * B_rowbytes)])) * (1.0f / max_channel_f);
          pixel[3] = 0.0f; /* alpha */
      }
    }
    break;
  }
  case 8: {
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(mipbuf, width, height, R_plane8, G_plane8, B_plane8, \
                      R_rowbytes, G_rowbytes, B_rowbytes, max_channel_f) \
  schedule(simd:static) \
  collapse(2)
#endif
    for (size_t y = 0; y < height; y++) {
      for (size_t x = 0; x < width; x++) {
          float *pixel = &mipbuf[(size_t)4 * ((y * width) + x)];

          /* max_channel_f is 255.0f for 8bit */
          pixel[0] = ((float)R_plane8[x + (y * R_rowbytes)]) * (1.0f / max_channel_f);
          pixel[1] = ((float)G_plane8[x + (y * G_rowbytes)]) * (1.0f / max_channel_f);
          pixel[2] = ((float)B_plane8[x + (y * B_rowbytes)]) * (1.0f / max_channel_f);
          pixel[3] = 0.0f; /* alpha */
      }
    }
    break;
  }
  default:
    dt_print(DT_DEBUG_IMAGEIO,
             "Invalid bit depth for AVIF image [%s]\n",
             filename);
    ret = DT_IMAGEIO_CACHE_FULL;
    goto out;
  }

  ret = DT_IMAGEIO_OK;
out:
  avifDecoderDestroy(decoder);
  avifFree((void *)raw.data); /* discard const */

  return ret;
}

dt_imageio_retval_t dt_imageio_avif_read_color_profile(const char *filename, struct avif_color_profile *cp)
{
  dt_imageio_retval_t ret;
  avifROData raw = AVIF_DATA_EMPTY;
  avifResult result;

  ret = read_image(filename, &raw);
  if (ret != DT_IMAGEIO_OK) {
    dt_print(DT_DEBUG_IMAGEIO,
             "Failed to read image [%s]\n",
             filename);
    return ret;
  }

  avifBool ok = avifPeekCompatibleFileType(&raw);
  if (!ok) {
    dt_print(DT_DEBUG_IMAGEIO,
             "Invalid avif image [%s]\n",
             filename);
    ret = DT_IMAGEIO_FILE_CORRUPTED;
    goto out;
  }

  avifDecoder *decoder = avifDecoderCreate();
  if (decoder == NULL) {
    dt_print(DT_DEBUG_IMAGEIO,
             "Failed to create AVIF decoder for image [%s]\n",
             filename);
    ret = DT_IMAGEIO_FILE_CORRUPTED;
    goto out;
  }

  result = avifDecoderParse(decoder, &raw);
  if (result != AVIF_RESULT_OK) {
    dt_print(DT_DEBUG_IMAGEIO,
             "Failed to parse AVIF image [%s]: %s\n",
             filename, avifResultToString(result));
    ret = DT_IMAGEIO_FILE_CORRUPTED;
    goto out;
  }

  if (decoder->imageCount > 1) {
    dt_control_log(_("image '%s' has more than one frame!"), filename);
  }

  result = avifDecoderNthImage(decoder, 0);
  if (result != AVIF_RESULT_OK) {
    dt_print(DT_DEBUG_IMAGEIO,
             "Failed to decode first frame of AVIF image [%s]: %s\n",
             filename, avifResultToString(result));
    ret = DT_IMAGEIO_FILE_CORRUPTED;
    goto out;
  }

  switch(decoder->image->profileFormat) {
  case AVIF_PROFILE_FORMAT_NCLX: {
    avifNclxColorProfile nclx = decoder->image->nclx;

    switch(nclx.colourPrimaries) {
    /*
     * BT709
     */
    case AVIF_NCLX_COLOUR_PRIMARIES_BT709:

      switch (nclx.transferCharacteristics) {
      /*
       * SRGB
       */
      case AVIF_NCLX_TRANSFER_CHARACTERISTICS_SRGB:

        switch (nclx.matrixCoefficients) {
        case AVIF_NCLX_MATRIX_COEFFICIENTS_SRGB:
        case AVIF_NCLX_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL:
          cp->type = DT_COLORSPACE_SRGB;
          break;
        default:
          break;
        }

        break; /* SRGB */

      /*
       * GAMMA22 BT709
       */
      case AVIF_NCLX_TRANSFER_CHARACTERISTICS_GAMMA22:

        switch (nclx.matrixCoefficients) {
        case AVIF_NCLX_MATRIX_COEFFICIENTS_BT709:
        case AVIF_NCLX_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL:
          cp->type = DT_COLORSPACE_REC709;
          break;
        default:
          break;
        }

        break; /* GAMMA22 BT709 */

      /*
       * LINEAR BT709
       */
      case AVIF_NCLX_TRANSFER_CHARACTERISTICS_LINEAR:

        switch (nclx.matrixCoefficients) {
        case AVIF_NCLX_MATRIX_COEFFICIENTS_BT709:
        case AVIF_NCLX_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL:
          cp->type = DT_COLORSPACE_LIN_REC709;
          break;
        default:
          break;
        }

        break; /* LINEAR BT709 */

      default:
        break;
      }

      break; /* BT709 */

    /*
     * BT2020
     */
    case AVIF_NCLX_COLOUR_PRIMARIES_BT2020:

      switch (nclx.transferCharacteristics) {
      /*
       * LINEAR BT2020
       */
      case AVIF_NCLX_TRANSFER_CHARACTERISTICS_LINEAR:

        switch (nclx.matrixCoefficients) {
        case AVIF_NCLX_MATRIX_COEFFICIENTS_BT2020_NCL:
        case AVIF_NCLX_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL:
          cp->type = DT_COLORSPACE_LIN_REC2020;
          break;
        default:
          break;
        }

        break; /* LINEAR BT2020 */

      /*
       * PQ BT2020
       */
      case AVIF_NCLX_TRANSFER_CHARACTERISTICS_BT2100_PQ:

        switch (nclx.matrixCoefficients) {
        case AVIF_NCLX_MATRIX_COEFFICIENTS_BT2020_NCL:
        case AVIF_NCLX_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL:
          cp->type = DT_COLORSPACE_PQ_REC2020;
          break;
        default:
          break;
        }

        break; /* PQ BT2020 */

      /*
       * HLG BT2020
       */
      case AVIF_NCLX_TRANSFER_CHARACTERISTICS_BT2100_HLG:

        switch (nclx.matrixCoefficients) {
        case AVIF_NCLX_MATRIX_COEFFICIENTS_BT2020_NCL:
        case AVIF_NCLX_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL:
          cp->type = DT_COLORSPACE_HLG_REC2020;
          break;
        default:
          break;
        }

        break; /* HLG BT2020 */

      default:
        break;
      }

      break; /* BT2020 */

    /*
     * P3
     */
    case AVIF_NCLX_COLOUR_PRIMARIES_P3:

      switch (nclx.transferCharacteristics) {
      /*
       * PQ P3
       */
      case AVIF_NCLX_TRANSFER_CHARACTERISTICS_BT2100_PQ:

        switch (nclx.matrixCoefficients) {
        case AVIF_NCLX_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL:
          cp->type = DT_COLORSPACE_PQ_P3;
          break;
        default:
          break;
        }

        break; /* PQ P3 */

      /*
       * HLG P3
       */
      case AVIF_NCLX_TRANSFER_CHARACTERISTICS_BT2100_HLG:

        switch (nclx.matrixCoefficients) {
        case AVIF_NCLX_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL:
          cp->type = DT_COLORSPACE_PQ_P3;
          break;
        default:
          break;
        }

        break; /* HLG P3 */

      default:
        break;
      }

      break; /* P3 */

    default:
      dt_print(DT_DEBUG_IMAGEIO,
               "Unsupported color profile for %s\n",
               filename);
      break;
    }

    break; /* AVIF_PROFILE_FORMAT_NCLX */
  }
  case AVIF_PROFILE_FORMAT_ICC: {
    avifRWData icc = decoder->image->icc;

    if (icc.data == NULL || icc.size == 0) {
      ret = DT_IMAGEIO_FILE_CORRUPTED;
      goto out;
    }

    uint8_t *data = (uint8_t *)g_malloc0(icc.size * sizeof(uint8_t));
    if (data == NULL) {
      dt_print(DT_DEBUG_IMAGEIO,
               "Failed to allocate ICC buffer for AVIF image [%s]\n",
               filename);
      ret = DT_IMAGEIO_FILE_CORRUPTED;
      goto out;
    }
    memcpy(data, icc.data, icc.size);

    cp->icc_profile_size = icc.size;
    cp->icc_profile = data;
    break;
  }
  case AVIF_PROFILE_FORMAT_NONE:
    break;
  }

  ret = DT_IMAGEIO_OK;
out:
  avifDecoderDestroy(decoder);
  avifFree((void *)raw.data); /* discard const */

  return ret;
}
