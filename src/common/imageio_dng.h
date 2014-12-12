/*
    This file is part of darktable,
    copyright (c) 2011 johannes hanika.

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
#ifndef DT_DNG_WRITER_H
#define DT_DNG_WRITER_H

// writes buffers as digital negative (dng) raw images

#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
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

static inline uint8_t *dt_imageio_dng_make_tag(uint16_t tag, uint16_t type, uint32_t lng, uint32_t fld,
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

static inline void dt_imageio_dng_write_tiff_header(FILE *fp, uint32_t xs, uint32_t ys, float Tv, float Av,
                                                    float f, float iso, uint32_t filter,
                                                    const float whitelevel)
{
  const uint32_t channels = 1;
  uint8_t *b /*, *offs1, *offs2*/;
  // uint32_t exif_offs;
  uint8_t buf[1024];
  uint8_t cnt = 0;

  memset(buf, 0, sizeof(buf));
  /* TIFF file header.  */
  buf[0] = 0x4d;
  buf[1] = 0x4d;
  buf[3] = 42;
  buf[7] = 10;

  b = buf + 12;
  b = dt_imageio_dng_make_tag(254, LONG, 1, 0, b, &cnt);           /* New subfile type.  */
  b = dt_imageio_dng_make_tag(256, SHORT, 1, (xs << 16), b, &cnt); /* Image width.  */
  b = dt_imageio_dng_make_tag(257, SHORT, 1, (ys << 16), b, &cnt); /* Image length.  */
  // b = dt_imageio_dng_make_tag(  258, SHORT, channels, 506, b, &cnt ); /* Bits per sample.  */
  b = dt_imageio_dng_make_tag(258, SHORT, 1, 32 << 16, b, &cnt); /* Bits per sample.  */
  // bits per sample: 32-bit float
  // buf[507] = buf[509] = buf[511] = 32;
  b = dt_imageio_dng_make_tag(259, SHORT, 1, (1 << 16), b, &cnt); /* Compression.  */
  b = dt_imageio_dng_make_tag(262, SHORT, 1, 32803 << 16, b, &cnt);
      /* cfa */ // 34892, b, &cnt ); // linear raw /* Photo interp.  */
  // b = dt_imageio_dng_make_tag(  271, ASCII, 8, 494, b, &cnt); // maker, needed for dcraw
  // b = dt_imageio_dng_make_tag(  272, ASCII, 9, 484, b, &cnt); // model
  //   offs2 = b + 8;
  b = dt_imageio_dng_make_tag(273, LONG, 1, 584, b, &cnt);             /* Strip offset.  */
  b = dt_imageio_dng_make_tag(274, SHORT, 1, 1 << 16, b, &cnt);        /* Orientation. */
  b = dt_imageio_dng_make_tag(277, SHORT, 1, channels << 16, b, &cnt); /* Samples per pixel.  */
  b = dt_imageio_dng_make_tag(278, SHORT, 1, (ys << 16), b, &cnt);     /* Rows per strip.  */
  b = dt_imageio_dng_make_tag(279, LONG, 1, (ys * xs * channels * 4), b,
                              &cnt);                              // 32 bits/channel /* Strip byte count.  */
  b = dt_imageio_dng_make_tag(284, SHORT, 1, (1 << 16), b, &cnt); /* Planar configuration.  */
  b = dt_imageio_dng_make_tag(339, SHORT, 1, (3 << 16), b,
                              &cnt); /* SampleFormat = 3 => ieee floating point */

  b = dt_imageio_dng_make_tag(33421, SHORT, 2, (2 << 16) | 2, b, &cnt); /* CFAREPEATEDPATTERNDIM */

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
  b = dt_imageio_dng_make_tag(33422, BYTE, 4, cfapattern, b, &cnt); /* CFAREPEATEDPATTERNDIM */

  // b = dt_imageio_dng_make_tag(  306, ASCII, 20, 428, b, &cnt ); // DateTime
  //   offs1 = b + 8;// + 3;
  // b = dt_imageio_dng_make_tag(34665, LONG, 1, 264, b, &cnt); // exif ifd
  b = dt_imageio_dng_make_tag(50706, BYTE, 4, (1 << 24) | (2 << 16), b, &cnt); // DNG Version/backward version
  b = dt_imageio_dng_make_tag(50707, BYTE, 4, (1 << 24) | (1 << 16), b, &cnt);
  uint32_t whitei = *(uint32_t *)&whitelevel;
  b = dt_imageio_dng_make_tag(50717, LONG, 1, whitei, b, &cnt); // WhiteLevel in float, actually.
  // b = dt_imageio_dng_make_tag(50708, ASCII, 9, 484, b, &cnt); // unique camera model
  // b = dt_imageio_dng_make_tag(50721, SRATIONAL, 9, 328, b, &cnt); // ColorMatrix1 (XYZ->native cam)
  // b = dt_imageio_dng_make_tag(50728, RATIONAL, 3, 512, b, &cnt); // AsShotNeutral
  // b = dt_imageio_dng_make_tag(50729, RATIONAL, 2, 512, b, &cnt); // AsShotWhiteXY
  b = dt_imageio_dng_make_tag(0, 0, 0, 0, b, &cnt); /* Next IFD.  */
  buf[11] = cnt - 1;
#if 0
  // exif is written later, by exiv2:
  // printf("offset: %d\n", b - buf);
  // set exif IFD offset
  exif_offs = b - buf;
  dt_imageio_dng_write_buf(buf, offs1 - buf, b - buf);

  b += 2;
  b = dt_imageio_dng_make_tag(33434, RATIONAL, 1, 400, b, &cnt); // exposure time
  b = dt_imageio_dng_make_tag(33437, RATIONAL, 1, 408, b, &cnt); // FNumber
  b = dt_imageio_dng_make_tag(34855, SHORT, 1, ((int)iso)<<16, b, &cnt); // iso speed rating
  b = dt_imageio_dng_make_tag(37386, RATIONAL, 1, 416, b, &cnt); // focal length
  b = dt_imageio_dng_make_tag( 0, 0, 0, 0, b, &cnt ); /* Next IFD.  */
  // buf[253] = cnt-buf[11]-1;
  buf[exif_offs + 1] = cnt-buf[11]-1;

  // printf("offset: %d\n", b - buf);
#endif

  // mostly garbage below, but i'm too lazy to clean it up:

  int32_t num, den;
  // ColorMatrix1
  float m[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
  // colorspace_get_xyz_to_cam(m);
  for(int k = 0; k < 9; k++)
  {
    dt_imageio_dng_convert_rational(m[3 * (k % 3) + k / 3], &num, &den);
    dt_imageio_dng_write_buf(buf, 328 + 8 * k, num);
    dt_imageio_dng_write_buf(buf, 328 + 8 * k + 4, den);
  }
  // for(int k=332;k<400;k+=8) dt_imageio_dng_write_buf(buf, k, 1); // den
  // dt_imageio_dng_write_buf(buf, 328, 1);// color matrix1: identity
  // dt_imageio_dng_write_buf(buf, 360, 1);
  // dt_imageio_dng_write_buf(buf, 392, 1);
  dt_imageio_dng_convert_rational(Tv, &num, &den);
  dt_imageio_dng_write_buf(buf, 400, num); // exposure time
  dt_imageio_dng_write_buf(buf, 404, den);
  dt_imageio_dng_convert_rational(Av, &num, &den);
  dt_imageio_dng_write_buf(buf, 408, num); // fnumber
  dt_imageio_dng_write_buf(buf, 412, den);
  dt_imageio_dng_convert_rational(f, &num, &den);
  dt_imageio_dng_write_buf(buf, 416, num); // focal length
  dt_imageio_dng_write_buf(buf, 420, den);
  strncpy((char *)buf + 428, "2008:07:15 13:37:00\0", 20); // DateTime
  strncpy((char *)buf + 484, "corona-6\0", 9);
  strncpy((char *)buf + 494, "hanatos\0", 8);

  // AsShotNeutral
  // dt_imageio_dng_convert_rational(0.333, &num, &den);
  // for(int k=0;k<3;k++)
  // {
  //   dt_imageio_dng_write_buf(buf, 518+8*k,   num);
  //   dt_imageio_dng_write_buf(buf, 518+8*k+4, den);
  // }
  // AsShotWhiteXY
  dt_imageio_dng_convert_rational(0.3333, &num, &den);
  dt_imageio_dng_write_buf(buf, 512, num);
  dt_imageio_dng_write_buf(buf, 516, den);
  dt_imageio_dng_convert_rational(0.333, &num, &den);
  dt_imageio_dng_write_buf(buf, 520, num);
  dt_imageio_dng_write_buf(buf, 524, den);

  // dt_imageio_dng_write_buf(buf, offs2-buf, 584);
  int k = fwrite(buf, 1, 584, fp);
  if(k != 584) fprintf(stderr, "[dng_write_header] failed to write image header!\n");
}

static inline void dt_imageio_write_dng(const char *filename, const float *const pixel, const int wd,
                                        const int ht, void *exif, const int exif_len, const uint32_t filter,
                                        const float whitelevel)
{
  FILE *f = fopen(filename, "wb");
  int k = 0;
  if(f)
  {
    dt_imageio_dng_write_tiff_header(f, wd, ht, 1.0f / 100.0f, 1.0f / 4.0f, 50.0f, 100.0f, filter, whitelevel);
    k = fwrite(pixel, sizeof(float), wd * ht, f);
    if(k != wd * ht) fprintf(stderr, "[dng_write] Error writing image data to %s\n", filename);
    fclose(f);
    if(exif) dt_exif_write_blob(exif, exif_len, filename);
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
#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
