/*
    This file is part of darktable,
    copyright (c) 2010 -- 2014 Henrik Andersson.
    copyright (c) 2014 LebedevRI.

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
#include "common/darktable.h"
#include "imageio.h"
#include "imageio_tiff.h"
#include "develop/develop.h"
#include "common/exif.h"
#include "common/colorspaces.h"
#include "control/conf.h"

#include <memory.h>
#include <stdio.h>
#include <tiffio.h>
#include <inttypes.h>
#include <strings.h>

typedef struct tiff_t
{
  TIFF *tiff;
  uint32_t width;
  uint32_t height;
  uint16_t bpp;
  uint16_t spp;
  uint16_t sampleformat;
  uint32_t scanlinesize;
  dt_image_t *image;
  float *mipbuf;
  tdata_t buf;
} tiff_t;

static inline int _read_planar_8(tiff_t *t)
{
  for(uint32_t row = 0; row < t->height; row++)
  {
    uint8_t *in = ((uint8_t *)t->buf);
    float *out = ((float *)t->mipbuf) + (size_t)4 * row * t->width;

    /* read scanline */
    if(TIFFReadScanline(t->tiff, in, row, 0) == -1) return -1;

    for(uint32_t i = 0; i < t->width; i++, in += t->spp, out += 4)
    {
      /* set rgb to first sample from scanline */
      out[0] = ((float)in[0]) * (1.0f / 255.0f);

      if(t->spp == 1)
      {
        out[1] = out[2] = out[0];
      }
      else
      {
        out[1] = ((float)in[1]) * (1.0f / 255.0f);
        out[2] = ((float)in[2]) * (1.0f / 255.0f);
      }

      out[3] = 0;
    }
  }

  return 1;
}

static inline int _read_planar_16(tiff_t *t)
{
  for(uint32_t row = 0; row < t->height; row++)
  {
    uint16_t *in = ((uint16_t *)t->buf);
    float *out = ((float *)t->mipbuf) + (size_t)4 * row * t->width;

    /* read scanline */
    if(TIFFReadScanline(t->tiff, in, row, 0) == -1) return -1;

    for(uint32_t i = 0; i < t->width; i++, in += t->spp, out += 4)
    {
      out[0] = ((float)in[0]) * (1.0f / 65535.0f);

      if(t->spp == 1)
      {
        out[1] = out[2] = out[0];
      }
      else
      {
        out[1] = ((float)in[1]) * (1.0f / 65535.0f);
        out[2] = ((float)in[2]) * (1.0f / 65535.0f);
      }

      out[3] = 0;
    }
  }

  return 1;
}

static inline int _read_planar_f(tiff_t *t)
{
  for(uint32_t row = 0; row < t->height; row++)
  {
    float *in = ((float *)t->buf);
    float *out = ((float *)t->mipbuf) + (size_t)4 * row * t->width;

    /* read scanline */
    if(TIFFReadScanline(t->tiff, in, row, 0) == -1) return -1;

    for(uint32_t i = 0; i < t->width; i++, in += t->spp, out += 4)
    {
      out[0] = in[0];

      if(t->spp == 1)
      {
        out[1] = out[2] = out[0];
      }
      else
      {
        out[1] = in[1];
        out[2] = in[2];
      }

      out[3] = 0;
    }
  }

  return 1;
}

dt_imageio_retval_t dt_imageio_open_tiff(dt_image_t *img, const char *filename, dt_mipmap_buffer_t *mbuf)
{
  const char *ext = filename + strlen(filename);
  while(*ext != '.' && ext > filename) ext--;
  if(strncmp(ext, ".tif", 4) && strncmp(ext, ".TIF", 4) && strncmp(ext, ".tiff", 5)
     && strncmp(ext, ".TIFF", 5))
    return DT_IMAGEIO_FILE_CORRUPTED;
  if(!img->exif_inited) (void)dt_exif_read(img, filename);

  tiff_t t;
  uint16_t config;

  t.image = img;

  if((t.tiff = TIFFOpen(filename, "rb")) == NULL) return DT_IMAGEIO_FILE_CORRUPTED;

  TIFFGetField(t.tiff, TIFFTAG_IMAGEWIDTH, &t.width);
  TIFFGetField(t.tiff, TIFFTAG_IMAGELENGTH, &t.height);
  TIFFGetField(t.tiff, TIFFTAG_BITSPERSAMPLE, &t.bpp);
  TIFFGetField(t.tiff, TIFFTAG_SAMPLESPERPIXEL, &t.spp);
  TIFFGetFieldDefaulted(t.tiff, TIFFTAG_SAMPLEFORMAT, &t.sampleformat);
  TIFFGetField(t.tiff, TIFFTAG_PLANARCONFIG, &config);
  t.scanlinesize = TIFFScanlineSize(t.tiff);

  fprintf(stderr, "[tiff_open] %dx%d %dbpp, %d samples per pixel.\n", t.width, t.height, t.bpp, t.spp);

  // we only support 8/16 and 32 bits per pixel formats.
  if(t.bpp != 8 && t.bpp != 16 && t.bpp != 32)
  {
    TIFFClose(t.tiff);
    return DT_IMAGEIO_FILE_CORRUPTED;
  }

  /* we only support 1,3 or 4 samples per pixel */
  if(t.spp != 1 && t.spp != 3 && t.spp != 4)
  {
    TIFFClose(t.tiff);
    return DT_IMAGEIO_FILE_CORRUPTED;
  }

  /* initialize cached image buffer */
  t.image->width = t.width;
  t.image->height = t.height;

  t.image->bpp = 4 * sizeof(float);

  t.mipbuf = (float *)dt_mipmap_cache_alloc(mbuf, t.image);
  if(!t.mipbuf)
  {
    fprintf(stderr, "[tiff_open] could not alloc full buffer for image `%s'\n", t.image->filename);
    TIFFClose(t.tiff);
    return DT_IMAGEIO_CACHE_FULL;
  }

  /* dont depend on planar config if spp == 1 */
  if(t.spp > 1 && config != PLANARCONFIG_CONTIG)
  {
    fprintf(stderr, "[tiff_open] warning: planar config other than contig is not supported.\n");
    TIFFClose(t.tiff);
    return DT_IMAGEIO_FILE_CORRUPTED;
  }

  if((t.buf = _TIFFmalloc(t.scanlinesize)) == NULL)
  {
    TIFFClose(t.tiff);
    return DT_IMAGEIO_CACHE_FULL;
  }

  int ok = 1;

  if(t.bpp == 8 && t.sampleformat == SAMPLEFORMAT_UINT && config == PLANARCONFIG_CONTIG)
    ok = _read_planar_8(&t);
  else if(t.bpp == 16 && t.sampleformat == SAMPLEFORMAT_UINT && config == PLANARCONFIG_CONTIG)
    ok = _read_planar_16(&t);
  else if(t.bpp == 32 && t.sampleformat == SAMPLEFORMAT_IEEEFP && config == PLANARCONFIG_CONTIG)
    ok = _read_planar_f(&t);
  else
  {
    fprintf(stderr, "[tiff_open] error: Not an supported tiff image format.");
    ok = 0;
  }

  _TIFFfree(t.buf);
  TIFFClose(t.tiff);

  return (ok == 1 ? DT_IMAGEIO_OK : DT_IMAGEIO_FILE_CORRUPTED);
}

int dt_imageio_tiff_read_profile(const char *filename, uint8_t **out)
{
  TIFF *tiff = NULL;
  uint32_t profile_len = 0;
  uint8_t *profile = NULL;

  if(!(filename && *filename && out)) return 0;

  if((tiff = TIFFOpen(filename, "rb")) == NULL) return 0;

  if(TIFFGetField(tiff, TIFFTAG_ICCPROFILE, &profile_len, &profile))
  {
    *out = (uint8_t *)malloc(profile_len);
    memcpy(*out, profile, profile_len);
  }
  else
    profile_len = 0;

  TIFFClose(tiff);

  return profile_len;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
