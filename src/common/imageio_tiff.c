/*
    This file is part of darktable,
    copyright (c) 2010 -- 2014 Henrik Andersson.

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

typedef struct tiff_t {
  TIFF *tiff;
  int orientation;
  uint32_t width;
  uint32_t height;
  uint16_t bpp;
  uint16_t spp;
  uint32_t imagelength;
  uint32_t scanlinesize;
  dt_image_t *image;
  float *mipbuf;
  tdata_t buf[3];
} tiff_t;

static inline void
_read_planar_8(tiff_t *t)
{
  uint8_t *buf;

  buf = (uint8_t *)t->buf[0];

  for (uint32_t row = 0; row < t->imagelength; row++)
  {
    /* read scanline */
    TIFFReadScanline(t->tiff, t->buf[0], row, 0);

    for (uint32_t i=0; i < t->width; i++)
    {
      size_t idx = dt_imageio_write_pos(i, row,
                                        t->width, t->height,
                                        t->width, t->height,
                                        t->orientation);

      /* set rgb to first sample from scanline */
      t->mipbuf[4 * idx + 0] =
        t->mipbuf[4 * idx + 1] =
        t->mipbuf[4 * idx + 2] = buf[t->spp * i + 0] * (1.0/255.0);

      t->mipbuf[4 * idx + 3] = 0;

      /* if grayscale continue */
      if (t->spp == 1)
        continue;

      t->mipbuf[4 * idx + 1] = buf[t->spp * i + 1] * (1.0/255.0);
      t->mipbuf[4 * idx + 2] = buf[t->spp * i + 2] * (1.0/255.0);
    }
  }
}

static inline void
_read_planar_16(tiff_t *t)
{
  uint16_t *buf;

  buf = (uint16_t *)t->buf[0];

  for (uint32_t row = 0; row < t->imagelength; row++)
  {
    /* read scanline */
    TIFFReadScanline(t->tiff, t->buf[0], row, 0);

    for (uint32_t i=0; i < t->width; i++)
    {
      size_t idx = dt_imageio_write_pos(i, row,
                                        t->width, t->height,
                                        t->width, t->height,
                                        t->orientation);

      t->mipbuf[4 * idx + 0] =
        t->mipbuf[4 * idx + 1] =
        t->mipbuf[4 * idx + 2] = buf[t->spp * i + 0] * (1.0/65535.0);
      t->mipbuf[4 * idx + 3] = 0;

      if (t->spp == 1)
        continue;

      t->mipbuf[4 * idx + 1] = buf[t->spp * i + 1] * (1.0/65535.0);
      t->mipbuf[4 * idx + 2] = buf[t->spp * i + 2] * (1.0/65535.0);
    }
  }
}

static inline void
_read_planar_f(tiff_t *t)
{
  float *buf;

  buf = (float *)t->buf[0];

  for (uint32_t row = 0; row < t->imagelength; row++)
  {
    TIFFReadScanline(t->tiff, t->buf[0], row, 0);

    for (uint32_t i=0; i < t->width; i++)
    {
      size_t idx = dt_imageio_write_pos(i, row,
                                        t->width, t->height,
                                        t->width, t->height,
                                        t->orientation);

      t->mipbuf[4 * idx + 0] = t->mipbuf[4 * idx + 1] = t->mipbuf[4 * idx + 2] = buf[t->spp * i + 0];
      t->mipbuf[4 * idx + 3] = 0;

      if (t->spp == 1)
        continue;

      t->mipbuf[4 * idx + 1] = buf[t->spp * i + 1];
      t->mipbuf[4 * idx + 2] = buf[t->spp * i + 2];
    }
  }
}

dt_imageio_retval_t
dt_imageio_open_tiff(
  dt_image_t *img,
  const char *filename,
  dt_mipmap_cache_allocator_t a)
{
  const char *ext = filename + strlen(filename);
  while(*ext != '.' && ext > filename) ext--;
  if(strncmp(ext, ".tif", 4) && strncmp(ext, ".TIF", 4) && strncmp(ext, ".tiff", 5) && strncmp(ext, ".TIFF", 5))
    return DT_IMAGEIO_FILE_CORRUPTED;
  if(!img->exif_inited)
    (void) dt_exif_read(img, filename);

  tiff_t t;
  uint16_t config;

  t.image = img;

  if((t.tiff = TIFFOpen(filename, "rb")) == NULL) return DT_IMAGEIO_FILE_CORRUPTED;

  TIFFGetField(t.tiff, TIFFTAG_IMAGEWIDTH, &t.width);
  TIFFGetField(t.tiff, TIFFTAG_IMAGELENGTH, &t.height);
  TIFFGetField(t.tiff, TIFFTAG_BITSPERSAMPLE, &t.bpp);
  TIFFGetField(t.tiff, TIFFTAG_SAMPLESPERPIXEL, &t.spp);
  TIFFGetField(t.tiff, TIFFTAG_IMAGELENGTH, &t.imagelength);
  TIFFGetField(t.tiff, TIFFTAG_PLANARCONFIG, &config);
  t.scanlinesize = TIFFScanlineSize(t.tiff);

  fprintf(stderr, "[tiff_open] %dx%d %dbpp, %d samples per pixel.\n",
          t.width, t.height, t.bpp, t.spp);

  // we only support 8/16 and 32 bits per pixel formats.
  if(t.bpp != 8 && t.bpp != 16 && t.bpp != 32)
  {
    TIFFClose(t.tiff);
    return DT_IMAGEIO_FILE_CORRUPTED;
  }

  /* we only support 1,3 or 4 samples per pixel */
  if (t.spp != 1 && t.spp != 3 && t.spp != 4)
  {
    TIFFClose(t.tiff);
    return DT_IMAGEIO_FILE_CORRUPTED;
  }

  /* initialize cached image buffer */
  t.orientation = dt_image_orientation(img);
  if(t.orientation & 4)
  {
    t.image->width = t.height;
    t.image->height = t.width;
  }
  else
  {
    t.image->width = t.width;
    t.image->height = t.height;
  }

  t.image->bpp = 4*sizeof(float);

  t.mipbuf = (float *)dt_mipmap_cache_alloc(t.image, DT_MIPMAP_FULL, a);
  if(!t.mipbuf)
  {
    fprintf(stderr, "[tiff_open] could not alloc full buffer for image `%s'\n", t.image->filename);
    TIFFClose(t.tiff);
    return DT_IMAGEIO_CACHE_FULL;
  }

  t.buf[0] = _TIFFmalloc(t.scanlinesize);
  t.buf[1] = _TIFFmalloc(t.scanlinesize);
  t.buf[2] = _TIFFmalloc(t.scanlinesize);

  /* dont depend on planar config if spp == 1 */
  if (t.spp > 1 && config != PLANARCONFIG_CONTIG)
  {
    fprintf(stderr, "[tiff_open] warning: planar config other than contig is not supported.\n");
    TIFFClose(t.tiff);
    return DT_IMAGEIO_FILE_CORRUPTED;
  }

  int ok = 1;

  if      (t.bpp == 8 && config == PLANARCONFIG_CONTIG)  _read_planar_8(&t);
  else if (t.bpp == 16 && config == PLANARCONFIG_CONTIG) _read_planar_16(&t);
  else if (t.bpp == 32 && config == PLANARCONFIG_CONTIG) _read_planar_f(&t);
  else
  {
    fprintf(stderr, "[tiff_open] error: Not an supported tiff image format.");
    ok = 0;
  }


  _TIFFfree(t.buf[0]);
  _TIFFfree(t.buf[1]);
  _TIFFfree(t.buf[2]);
  TIFFClose(t.tiff);
  return (ok == 1 ? DT_IMAGEIO_OK : DT_IMAGEIO_FILE_CORRUPTED);
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
