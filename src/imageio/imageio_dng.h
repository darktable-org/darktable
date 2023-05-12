/*
    This file is part of darktable,
    Copyright (C) 2011-2023 darktable developers.

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

#pragma once

// writes buffers as digital negative (dng) raw images

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/darktable.h"
#include "common/exif.h"


#define II 1
#define MM 2
#define BYTE 1
#define ASCII 2
#define SHORT 3
#define LONG 4
#define RATIONAL 5
#define SRATIONAL 10

#define HEADBUFFSIZE 1024

static inline void _imageio_dng_write_buf(uint8_t *buf, const uint32_t d, const int val)
{
  if(d + 4 >= HEADBUFFSIZE) return;
  buf[d + 3] = val & 0xff;
  buf[d + 2] = (val >> 8) & 0xff;
  buf[d + 1] = (val >> 16) & 0xff;
  buf[d] = val >> 24;
}

static inline int _imageio_dng_make_tag(
    const uint16_t tag,
    const uint16_t type,
    const uint32_t lng,
    const uint32_t fld,
    uint8_t *buf,
    const uint32_t b,
    uint8_t *cnt)
{
  if(b + 12 < HEADBUFFSIZE)
  {
    _imageio_dng_write_buf(buf, b, (tag << 16) | type);
    _imageio_dng_write_buf(buf, b+4, lng);
    _imageio_dng_write_buf(buf, b+8, fld);
    *cnt = *cnt + 1;
  }
  return b + 12;
}

static inline void _imageio_dng_write_tiff_header(
    FILE *fp,
    uint32_t xs,
    uint32_t ys,
    float Tv,
    float Av,
    float f,
    float iso,
    uint32_t filter,
    const uint8_t xtrans[6][6],
    const float whitelevel,
    const dt_aligned_pixel_t wb_coeffs,
    const float adobe_XYZ_to_CAM[4][3])
{
  const uint32_t channels = 1;
  uint8_t buf[HEADBUFFSIZE];
  uint8_t cnt = 0;

  // this matrix is generic for XYZ->sRGB / D65
  int m[9] = { 3240454, -1537138, -498531, -969266, 1876010, 41556, 55643, -204025, 1057225 };
  int den = 1000000;

  memset(buf, 0, sizeof(buf));
  /* TIFF file header.  */
  buf[0] = 0x4d;
  buf[1] = 0x4d;
  buf[3] = 42;
  buf[7] = 8;
  uint32_t b = 10;

  // If you want to add other tags written to a dng file include the the ID in the enum to
  // keep track of written tags so we don't a) have leaks or b) overwrite anything in data section 
  const int first_tag = __LINE__ + 3;
  enum write_tags
  {
    EXIF_TAG_NEXT_IFD = 0,
    EXIF_TAG_SUBFILE = 254,           /* New subfile type.  */
    EXIF_TAG_IMGWIDTH = 256,          /* Image width.  */
    EXIF_TAG_IMGLENGTH = 257,         /* Image length.  */
    EXIF_TAG_BPS = 258,               /* Bits per sample: 32-bit float */
    EXIF_TAG_COMPRESS = 259,          /* Compression.  */
    EXIF_TAG_PHOTOMINTREP = 262,      /* Photo interp: CFA  */
    EXIF_TAG_STRIP_OFFSET = 273,      /* Strip offset.  */
    EXIF_TAG_ORIENTATION = 274,       /* Orientation. */
    EXIF_TAG_SAMPLES_PER_PIXEL = 277, /* Samples per pixel.  */
    EXIF_TAG_ROWS_PER_STRIP = 278,    /* Rows per strip.  */
    EXIF_TAG_STRIP_BCOUNT = 279,      /* Strip byte count.  */
    EXIF_TAG_PLANAR_CONFIG = 284,     /* Planar configuration.  */
    EXIF_TAG_SAMPLE_FORMAT = 339,     /* SampleFormat = 3 => ieee floating point */
    EXIF_TAG_REPEAT_PATTERN = 33421,  /* pattern repeat */
    EXIF_TAG_SENS_PATTERN = 33422,    /* sensor pattern */
    EXIF_TAG_VERSION = 50706,         /* DNG Version */
    EXIF_TAG_BACK_VERSION = 50707,    /* DNG back Version */
    EXIF_TAG_WHITE_LEVEL = 50717,     /* White level */
    EXIF_TAG_CROP_ORIGIN = 50719,     /* Crop Origin */
    EXIF_TAG_CROP_SIZE = 50720,       /* Crop Size */
    EXIF_TAG_COLOR_MATRIX1 = 50721,   /* ColorMatrix1 (XYZ->native cam) */
    EXIF_TAG_SHOT_NEUTRAL = 50728,    /* AsShotNeutral for rawspeed Dngdecoder camera white balance */
    EXIF_TAG_ILLUMINANT1 = 50778,     /* CalibrationIlluminant1 */
    EXIF_TAG_ACTIVE_AREA = 50829,     /* Active Area */
  };
  uint32_t data = 10 + (__LINE__ - first_tag - 1) * 12 ; // takes care of the header an num of lines

  b = _imageio_dng_make_tag(EXIF_TAG_SUBFILE, LONG, 1, 0, buf, b, &cnt);
  b = _imageio_dng_make_tag(EXIF_TAG_IMGWIDTH, SHORT, 1, (xs << 16), buf, b, &cnt);
  b = _imageio_dng_make_tag(EXIF_TAG_IMGLENGTH, SHORT, 1, (ys << 16), buf, b, &cnt);
  b = _imageio_dng_make_tag(EXIF_TAG_BPS, SHORT, 1, 32 << 16, buf, b, &cnt);
  b = _imageio_dng_make_tag(EXIF_TAG_COMPRESS, SHORT, 1, (1 << 16), buf, b, &cnt);
  b = _imageio_dng_make_tag(EXIF_TAG_PHOTOMINTREP, SHORT, 1, 32803 << 16, buf, b, &cnt);
  b = _imageio_dng_make_tag(EXIF_TAG_ORIENTATION, SHORT, 1, 1 << 16, buf, b, &cnt);
  b = _imageio_dng_make_tag(EXIF_TAG_SAMPLES_PER_PIXEL, SHORT, 1, channels << 16, buf, b, &cnt);
  b = _imageio_dng_make_tag(EXIF_TAG_ROWS_PER_STRIP, SHORT, 1, (ys << 16), buf, b, &cnt);
  b = _imageio_dng_make_tag(EXIF_TAG_STRIP_BCOUNT, LONG, 1, (ys * xs * channels*4), buf, b, &cnt);
  b = _imageio_dng_make_tag(EXIF_TAG_PLANAR_CONFIG, SHORT, 1, (1 << 16), buf, b, &cnt);
  b = _imageio_dng_make_tag(EXIF_TAG_SAMPLE_FORMAT, SHORT, 1, (3 << 16), buf, b, &cnt);

  b = _imageio_dng_make_tag(EXIF_TAG_ACTIVE_AREA, LONG, 4, data, buf, b, &cnt);
  _imageio_dng_write_buf(buf, data, 0);
  _imageio_dng_write_buf(buf, data+4, 0);
  _imageio_dng_write_buf(buf, data+8, ys);
  _imageio_dng_write_buf(buf, data+12, xs);
  data += 16;

  b = _imageio_dng_make_tag(EXIF_TAG_CROP_ORIGIN, LONG, 2, data, buf, b, &cnt);
  _imageio_dng_write_buf(buf, data, 0);
  _imageio_dng_write_buf(buf, data+4, 0);
  data += 8;

  b = _imageio_dng_make_tag(EXIF_TAG_CROP_SIZE, LONG, 2, data, buf, b, &cnt);
  _imageio_dng_write_buf(buf, data, xs);
  _imageio_dng_write_buf(buf, data+4, ys);
  data += 8;

  if(filter == 9u) // xtrans
    b = _imageio_dng_make_tag(EXIF_TAG_REPEAT_PATTERN, SHORT, 2, (6 << 16) | 6, buf, b, &cnt);
  else
    b = _imageio_dng_make_tag(EXIF_TAG_REPEAT_PATTERN, SHORT, 2, (2 << 16) | 2, buf, b, &cnt);

  uint32_t cfapattern = 0;
  switch(filter)
  {
    case 0x94949494:
      cfapattern = (0 << 24) | (1 << 16) | (1 << 8) | 2; // rggb
      break;
    case 0x49494949:
      cfapattern = (1 << 24) | (2 << 16) | (0 << 8) | 1; // gbrg
      break;
    case 0x61616161:
      cfapattern = (1 << 24) | (0 << 16) | (2 << 8) | 1; // grbg
      break;
    default:                                             // case 0x16161616:
      cfapattern = (2 << 24) | (1 << 16) | (1 << 8) | 0; // bggr
      break;
  }

  if(filter == 9u) // xtrans
  {
    b = _imageio_dng_make_tag(EXIF_TAG_SENS_PATTERN, BYTE, 36, data, buf, b, &cnt); /* xtrans PATTERN */
    // apparently this doesn't need byteswap:
    memcpy(buf + data, xtrans, sizeof(uint8_t)*36);
    data += 36;
  }
  else // bayer
    b = _imageio_dng_make_tag(EXIF_TAG_SENS_PATTERN, BYTE, 4, cfapattern, buf, b, &cnt); /* bayer PATTERN */

  b = _imageio_dng_make_tag(EXIF_TAG_VERSION, BYTE, 4, (1 << 24)|(2 << 16), buf, b, &cnt);
  b = _imageio_dng_make_tag(EXIF_TAG_BACK_VERSION, BYTE, 4, (1 << 24)|(1 << 16), buf, b, &cnt);

  union {
      float f;
      uint32_t u;
  } white;
  white.f = whitelevel;

  b = _imageio_dng_make_tag(EXIF_TAG_WHITE_LEVEL, LONG, 1, white.u, buf, b, &cnt); /* WhiteLevel in float, actually. */

  // ColorMatrix1 try to get camera matrix else m[k] like before
  if(dt_is_valid_colormatrix(adobe_XYZ_to_CAM[0][0]))
  {
    den = 10000;
    for(int k= 0; k < 3; k++)
      for(int i= 0; i < 3; i++)
        m[k*3+i] = roundf(adobe_XYZ_to_CAM[k][i] * den);
  }
  b = _imageio_dng_make_tag(EXIF_TAG_COLOR_MATRIX1, SRATIONAL, 9, data, buf, b, &cnt); /* ColorMatrix1 (XYZ->native cam) */
  for(int k = 0; k < 9; k++)
  {
    _imageio_dng_write_buf(buf, data + k*8, m[k]);
    _imageio_dng_write_buf(buf, data+4 + k*8, den);
  }
  data += 9 * 8;

  b = _imageio_dng_make_tag(EXIF_TAG_SHOT_NEUTRAL, RATIONAL, 3, data, buf, b, &cnt);
  den = 1000000;
  for(int k = 0; k < 3; k++)
  {
    const float coeff = roundf(((float)den * wb_coeffs[1]) / wb_coeffs[k]);
    _imageio_dng_write_buf(buf, data + k*8, (int)coeff);
    _imageio_dng_write_buf(buf, data+4 + k*8, den);
  }
  data += 3 * 8;

  b = _imageio_dng_make_tag(EXIF_TAG_ILLUMINANT1, SHORT, 1, DT_LS_D65 << 16, buf, b, &cnt);

  // We have all tags using data now written so we can finally use strip offset 
  b = _imageio_dng_make_tag(EXIF_TAG_STRIP_OFFSET, LONG, 1, data, buf, b, &cnt);
  b = _imageio_dng_make_tag(EXIF_TAG_NEXT_IFD, 0, 0, 0, buf, b, &cnt);

  buf[9] = cnt - 1; /* write number of directory entries of this ifd */

  if(data >= HEADBUFFSIZE)
  {
    dt_print(DT_DEBUG_ALWAYS, "[dng_write_header] can't write valid header as it exceeds buffer size!\n");
    return;
  }

  // exif is written later, by exiv2:
  const int written = fwrite(buf, 1, data, fp);
  if(written != data) dt_print(DT_DEBUG_ALWAYS, "[dng_write_header] failed to write image header!\n");
}


static inline void dt_imageio_write_dng(
    const char *filename, const float *const pixel, const int wd,
    const int ht, void *exif, const int exif_len, const uint32_t filter,
    const uint8_t xtrans[6][6],
    const float whitelevel,
    const dt_aligned_pixel_t wb_coeffs,
    const float adobe_XYZ_to_CAM[4][3])
{
  FILE *f = g_fopen(filename, "wb");
  if(f)
  {
    _imageio_dng_write_tiff_header(f, wd, ht, 1.0f / 100.0f, 1.0f / 4.0f, 50.0f, 100.0f,
                                     filter, xtrans, whitelevel, wb_coeffs, adobe_XYZ_to_CAM);
    const int k = fwrite(pixel, sizeof(float), (size_t)wd * ht, f);
    if(k != wd * ht) dt_print(DT_DEBUG_ALWAYS, "[dng_write] Error writing image data to %s\n", filename);
    fclose(f);
    if(exif) dt_exif_write_blob(exif, exif_len, filename, 0);
  }
}

#undef II
#undef MM
#undef BYTE
#undef ASCII
#undef SHORT
#undef LONG
#undef RATIONAL
#undef SRATIONAL

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
