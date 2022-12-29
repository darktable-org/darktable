/*
    This file is part of darktable,
    Copyright (C) 2011-2022 darktable developers.

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

static inline void dt_imageio_dng_write_buf(uint8_t *buf, int adr, int val)
{
  buf[adr + 3] = val & 0xff;
  buf[adr + 2] = (val >> 8) & 0xff;
  buf[adr + 1] = (val >> 16) & 0xff;
  buf[adr] = val >> 24;
}

static inline uint8_t *dt_imageio_dng_make_tag(
    uint16_t tag, uint16_t type, uint32_t lng, uint32_t fld,
    uint8_t *b, uint8_t *cnt)
{
  dt_imageio_dng_write_buf(b, 0, (tag << 16) | type);
  dt_imageio_dng_write_buf(b, 4, lng);
  dt_imageio_dng_write_buf(b, 8, fld);
  *cnt = *cnt + 1;
  return b + 12;
}

static inline void dt_imageio_dng_convert_rational(float f, int32_t *num, int32_t *den)
{
  int32_t sign = 1;
  if(f < 0)
  {
    sign = -1;
    f = -f;
  }
  float mult = 1.0f;
  while(f * mult - (int)(f * mult + 0.00005f) > 0.0001f) mult++;
  *den = mult;
  *num = (int)(*den * f);
  *num *= sign;
}

static inline void dt_imageio_dng_write_tiff_header(
    FILE *fp, uint32_t xs, uint32_t ys, float Tv, float Av,
    float f, float iso, uint32_t filter,
    const uint8_t xtrans[6][6],
    const float whitelevel,
    const dt_aligned_pixel_t wb_coeffs,
    const float adobe_XYZ_to_CAM[4][3])
{
  const uint32_t channels = 1;
  uint8_t buf[1024];
  uint8_t cnt = 0;

  // this matrix is generic for XYZ->sRGB / D65
  int m[9] = { 3240454, -1537138, -498531, -969266, 1876010, 41556, 55643, -204025, 1057225 };
  int den = 1000000;

  memset(buf, 0, sizeof(buf));
  /* TIFF file header.  */
  buf[0] = 0x4d;
  buf[1] = 0x4d;
  buf[3] = 42;
  buf[7] = 10;

  uint8_t *b = buf + 12;
  int data = 512; // take care of room in data segment; the tags may use up to 500 bytes so far 

  b = dt_imageio_dng_make_tag(254, LONG, 1, 0, b, &cnt);                      /* New subfile type.  */
  b = dt_imageio_dng_make_tag(256, SHORT, 1, (xs << 16), b, &cnt);            /* Image width.  */
  b = dt_imageio_dng_make_tag(257, SHORT, 1, (ys << 16), b, &cnt);            /* Image length.  */
  b = dt_imageio_dng_make_tag(258, SHORT, 1, 32 << 16, b, &cnt);              /* Bits per sample: 32-bit float */
  b = dt_imageio_dng_make_tag(259, SHORT, 1, (1 << 16), b, &cnt);             /* Compression.  */
  b = dt_imageio_dng_make_tag(262, SHORT, 1, 32803 << 16, b, &cnt);           /* Photo interp: CFA  */
  b = dt_imageio_dng_make_tag(274, SHORT, 1, 1 << 16, b, &cnt);               /* Orientation. */
  b = dt_imageio_dng_make_tag(277, SHORT, 1, channels << 16, b, &cnt);        /* Samples per pixel.  */
  b = dt_imageio_dng_make_tag(278, SHORT, 1, (ys << 16), b, &cnt);            /* Rows per strip.  */
  b = dt_imageio_dng_make_tag(279, LONG, 1, (ys * xs * channels*4), b, &cnt); /* Strip byte count.  */
  b = dt_imageio_dng_make_tag(284, SHORT, 1, (1 << 16), b, &cnt);             /* Planar configuration.  */
  b = dt_imageio_dng_make_tag(339, SHORT, 1, (3 << 16), b, &cnt);             /* SampleFormat = 3 => ieee floating point */

  b = dt_imageio_dng_make_tag(50829, LONG, 4, data, b, &cnt);                 /* Active Area */
  dt_imageio_dng_write_buf(buf, data, 0);
  dt_imageio_dng_write_buf(buf, data+4, 0);
  dt_imageio_dng_write_buf(buf, data+8, ys);
  dt_imageio_dng_write_buf(buf, data+12, xs);
  data += 16;

  b = dt_imageio_dng_make_tag(50719, LONG, 2, data, b, &cnt);                 /* Crop Origin */
  dt_imageio_dng_write_buf(buf, data, 0);
  dt_imageio_dng_write_buf(buf, data+4, 0);
  data += 8;

  b = dt_imageio_dng_make_tag(50720, LONG, 2, data, b, &cnt);                 /* Crop Size */
  dt_imageio_dng_write_buf(buf, data, xs);
  dt_imageio_dng_write_buf(buf, data+4, ys);
  data += 8;

  if(filter == 9u) // xtrans
    b = dt_imageio_dng_make_tag(33421, SHORT, 2, (6 << 16) | 6, b, &cnt);     /* CFAREPEATEDPATTERNDIM */
  else
    b = dt_imageio_dng_make_tag(33421, SHORT, 2, (2 << 16) | 2, b, &cnt);     /* CFAREPEATEDPATTERNDIM */

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
    b = dt_imageio_dng_make_tag(33422, BYTE, 36, data, b, &cnt);              /* xtrans PATTERN */
    // apparently this doesn't need byteswap:
    memcpy(buf + data, xtrans, sizeof(uint8_t)*36);
    data += 36;
  }
  else // bayer
    b = dt_imageio_dng_make_tag(33422, BYTE, 4, cfapattern, b, &cnt);         /* bayer PATTERN */

  b = dt_imageio_dng_make_tag(50706, BYTE, 4, (1 << 24)|(2 << 16), b, &cnt);  /* DNG Version/backward version */
  b = dt_imageio_dng_make_tag(50707, BYTE, 4, (1 << 24)|(1 << 16), b, &cnt);

  union {
      float f;
      uint32_t u;
  } white;
  white.f = whitelevel;

  b = dt_imageio_dng_make_tag(50717, LONG, 1, white.u, b, &cnt);              /* WhiteLevel in float, actually. */

  // ColorMatrix1 try to get camera matrix else m[k] like before
  if(!isnan(adobe_XYZ_to_CAM[0][0]))
  {
    den = 10000;
    for(int k= 0; k < 3; k++)
      for(int i= 0; i < 3; i++)
        m[k*3+i] = roundf(adobe_XYZ_to_CAM[k][i] * den);
  }
  b = dt_imageio_dng_make_tag(50721, SRATIONAL, 9, data, b, &cnt);            /* ColorMatrix1 (XYZ->native cam) */
  for(int k = 0; k < 9; k++)
  {
    dt_imageio_dng_write_buf(buf, data + k*8, m[k]);
    dt_imageio_dng_write_buf(buf, data+4 + k*8, den);
  }
  data += 9 * 8;

  b = dt_imageio_dng_make_tag(50728, RATIONAL, 3, data, b, &cnt);             /* AsShotNeutral for rawspeed Dngdecoder camera white balance */
  den = 1000000;
  for(int k = 0; k < 3; k++)
  {
    const float coeff = roundf(((float)den * wb_coeffs[1]) / wb_coeffs[k]);
    dt_imageio_dng_write_buf(buf, data + k*8, (int)coeff);
    dt_imageio_dng_write_buf(buf, data+4 + k*8, den);
  }
  data += 3 * 8;

  b = dt_imageio_dng_make_tag(50778, SHORT, 1, DT_LS_D65 << 16, b, &cnt);     /* CalibrationIlluminant1 */

  // We have all tags using data now written so we can finally use strip offset 
  b = dt_imageio_dng_make_tag(273, LONG, 1, data, b, &cnt);                   /* Strip offset.  */
  b = dt_imageio_dng_make_tag(0, 0, 0, 0, b, &cnt);                           /* Next IFD.  */
  buf[11] = cnt - 1;                                                          /* write number of directory entries of this ifd */

  // exif is written later, by exiv2:
  const int written = fwrite(buf, 1, data, fp);
  if(written != data) fprintf(stderr, "[dng_write_header] failed to write image header!\n");
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
    dt_imageio_dng_write_tiff_header(f, wd, ht, 1.0f / 100.0f, 1.0f / 4.0f, 50.0f, 100.0f,
                                     filter, xtrans, whitelevel, wb_coeffs, adobe_XYZ_to_CAM);
    const int k = fwrite(pixel, sizeof(float), (size_t)wd * ht, f);
    if(k != wd * ht) fprintf(stderr, "[dng_write] Error writing image data to %s\n", filename);
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
