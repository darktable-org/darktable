/*
    This file is part of darktable,
    copyright (c) 2010 Henrik Andersson.

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
#include <assert.h>

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

  TIFF *image;
  uint32_t width, height, config;
  uint16_t spp, bpp;

  if((image = TIFFOpen(filename, "rb")) == NULL) return DT_IMAGEIO_FILE_CORRUPTED;

  TIFFGetField(image, TIFFTAG_IMAGEWIDTH, &width);
  TIFFGetField(image, TIFFTAG_IMAGELENGTH, &height);
  TIFFGetField(image, TIFFTAG_BITSPERSAMPLE, &bpp);
  TIFFGetField(image, TIFFTAG_SAMPLESPERPIXEL, &spp);

  // we only support 8-bit and 16-bit here. in case of other formats let's hope for GraphicsMagick to deal them
  if(bpp != 8 && bpp != 16)
  {
    TIFFClose(image);
    return DT_IMAGEIO_FILE_CORRUPTED;
  }

  const int orientation = dt_image_orientation(img);

  if(orientation & 4)
  {
    img->width = height;
    img->height = width;
  }
  else
  {
    img->width = width;
    img->height = height;
  }

  img->bpp = 4*sizeof(float);

  float *mipbuf = (float *)dt_mipmap_cache_alloc(img, DT_MIPMAP_FULL, a);
  if(!mipbuf)
  {
    fprintf(stderr, "[tiff_open] could not alloc full buffer for image `%s'\n", img->filename);
    TIFFClose(image);
    return DT_IMAGEIO_CACHE_FULL;
  }

  uint32_t imagelength;
  int32_t scanlinesize = TIFFScanlineSize(image);
  tdata_t buf;
  buf = _TIFFmalloc(scanlinesize);
  uint16_t *buf16 = (uint16_t *)buf;
  uint8_t *buf8 = (uint8_t *)buf;
  uint32_t row;

  const int ht2 = orientation & 4 ? img->width  : img->height; // pretend unrotated, rotate in write_pos
  const int wd2 = orientation & 4 ? img->height : img->width;
  TIFFGetField(image, TIFFTAG_IMAGELENGTH, &imagelength);
  TIFFGetField(image, TIFFTAG_PLANARCONFIG, &config);
  if (config != PLANARCONFIG_CONTIG)
  {
    fprintf(stderr, "[tiff_open] warning: config other than contig found, trying anyways\n");
    config = PLANARCONFIG_CONTIG;
  }
  if (config == PLANARCONFIG_CONTIG)
  {
    for (row = 0; row < imagelength; row++)
    {
      TIFFReadScanline(image, buf, row, 0);
      if(bpp == 8) for(int i=0; i<width; i++)
          for(int k=0; k<3; k++) mipbuf[4*dt_imageio_write_pos(i, row, wd2, ht2, wd2, ht2, orientation) + k] = buf8[spp*i + k]*(1.0/255.0);
      else for(int i=0; i<width; i++)
          for(int k=0; k<3; k++) mipbuf[4*dt_imageio_write_pos(i, row, wd2, ht2, wd2, ht2, orientation) + k] = buf16[spp*i + k]*(1.0/65535.0);
      // for(int k=0;k<3;k++) mipbuf[3*(width*row + i) + k] = ((buf16[mul*i + k]>>8)|((buf16[mul*i + k]<<8)&0xff00))*(1.0/65535.0);
    }
  }
  else if (config == PLANARCONFIG_SEPARATE)
  {
    assert(0);
    // uint16_t s, nsamples;
    // TIFFGetField(tif, TIFFTAG_SAMPLESPERPIXEL, &nsamples);
    // for (s = 0; s < nsamples; s++)
    //   for (row = 0; row < imagelength; row++)
    //     TIFFReadScanline(image, buf, row, s);
  }
  _TIFFfree(buf);
  TIFFClose(image);
  return DT_IMAGEIO_OK;
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
