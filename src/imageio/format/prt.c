/*
    This file is part of darktable,
    copyright (c) 2014 pascal obry.

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

#include <memory.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <inttypes.h>
#include <tiffio.h>
#include "common/darktable.h"
#include "common/imageio_module.h"
#include "common/imageio.h"
#include "common/exif.h"
#include "common/colorspaces.h"
#include "control/conf.h"
#include "common/imageio_format.h"
#include "bauhaus/bauhaus.h"

DT_MODULE(1)

typedef struct dt_imageio_prt_t
{
  int max_width, max_height;
  int width, height;
  char style[128];
  gboolean style_append;
  int bpp;
  int compress;
  TIFF *handle;
} dt_imageio_prt_t;

int write_image(dt_imageio_module_data_t *d_tmp, const char *filename, const void *in_void, void *exif,
                int exif_len, int imgid)
{
  const dt_imageio_prt_t *d = (dt_imageio_prt_t *)d_tmp;

  uint8_t *profile = NULL;
  uint32_t profile_len = 0;

  TIFF *tif = NULL;

  void *rowdata = NULL;

  int rc = 1; // default to error

  if(imgid > 0)
  {
    cmsHPROFILE out_profile = dt_colorspaces_create_output_profile(imgid);
    cmsSaveProfileToMem(out_profile, 0, &profile_len);
    if(profile_len > 0)
    {
      profile = malloc(profile_len);
      if(!profile)
      {
        rc = 1;
        goto exit;
      }
      cmsSaveProfileToMem(out_profile, profile, &profile_len);
    }
    dt_colorspaces_cleanup_profile(out_profile);
  }

  const int top = dt_conf_get_int("plugins/imageio/format/print/margin-top");
  const int left = dt_conf_get_int("plugins/imageio/format/print/margin-left");
  const int right = dt_conf_get_int("plugins/imageio/format/print/margin-right");
  const int bottom = dt_conf_get_int("plugins/imageio/format/print/margin-bottom");

  const uint32_t width = d->width + left + right;
  const uint32_t height = d->height + top + bottom;

  const uint16_t white[3 * sizeof(uint16_t)] = { 0xffff, 0xffff, 0xffff };

  // Create little endian tiff image
  tif = TIFFOpen(filename, "wl");
  if(!tif)
  {
    rc = 1;
    goto exit;
  }

  // http://partners.adobe.com/public/developer/en/tiff/TIFFphotoshop.pdf (dated 2002)
  // "A proprietary ZIP/Flate compression code (0x80b2) has been used by some"
  // "software vendors. This code should be considered obsolete. We recommend"
  // "that TIFF implentations recognize and read the obsolete code but only"
  // "write the official compression code (0x0008)."
  // http://www.awaresystems.be/imaging/tiff/tifftags/compression.html
  // http://www.awaresystems.be/imaging/tiff/tifftags/predictor.html

  // no compression
  TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);

  TIFFSetField(tif, TIFFTAG_FILLORDER, (uint16_t)FILLORDER_MSB2LSB);
  if(profile != NULL)
  {
    TIFFSetField(tif, TIFFTAG_ICCPROFILE, (uint32_t)profile_len, profile);
  }
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, (uint16_t)3);
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, (uint16_t)d->bpp);
  TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, (uint16_t)SAMPLEFORMAT_UINT);
  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, (uint32_t)width);
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, (uint32_t)height);
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, (uint16_t)PHOTOMETRIC_RGB);
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, (uint16_t)PLANARCONFIG_CONTIG);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, (uint32_t)1);
  TIFFSetField(tif, TIFFTAG_ORIENTATION, (uint16_t)ORIENTATION_TOPLEFT);

  int resolution = dt_conf_get_int("metadata/resolution");
  if(resolution > 0)
  {
    TIFFSetField(tif, TIFFTAG_XRESOLUTION, (float)resolution);
    TIFFSetField(tif, TIFFTAG_YRESOLUTION, (float)resolution);
    TIFFSetField(tif, TIFFTAG_RESOLUTIONUNIT, (uint16_t)RESUNIT_INCH);
  }

  const size_t rowsize = (width * 3) * d->bpp / 8;
  if((rowdata = malloc(rowsize)) == NULL)
  {
    rc = 1;
    goto exit;
  }

  if(d->bpp == 16)
  {
    // top

    for(int y = 0; y < top; y++)
    {
      uint16_t *out = (uint16_t *)rowdata;
      for(int x = 0; x < width; x++, out += 3)
      {
        memcpy(out, white, 3 * sizeof(uint16_t));
      }

      if(TIFFWriteScanline(tif, rowdata, y, 0) == -1)
      {
        rc = 1;
        goto exit;
      }
    }

    // middle

    for(int y = 0; y < d->height; y++)
    {
      uint16_t *in = (uint16_t *)in_void + (size_t)4 * y * d->width;
      uint16_t *out = (uint16_t *)rowdata;

      // left

      for(int x = 0; x < left; x++, out += 3)
      {
        memcpy(out, white, 3 * sizeof(uint16_t));
      }

      // image

      for(int x = 0; x < d->width; x++, in += 4, out += 3)
      {
        memcpy(out, in, 3 * sizeof(uint16_t));
      }

      // right

      for(int x = 0; x < right; x++, out += 3)
      {
        memcpy(out, white, 3 * sizeof(uint16_t));
      }

      if(TIFFWriteScanline(tif, rowdata, y + top, 0) == -1)
      {
        rc = 1;
        goto exit;
      }
    }
  }
  else
  {
    // top

    for(int y = 0; y < top; y++)
    {
      uint8_t *out = (uint8_t *)rowdata;
      for(int x = 0; x < width; x++, out += 3)
      {
        memcpy(out, white, 3 * sizeof(uint8_t));
      }

      if(TIFFWriteScanline(tif, rowdata, y, 0) == -1)
      {
        rc = 1;
        goto exit;
      }
    }

    // middle

    for(int y = 0; y < d->height; y++)
    {
      uint8_t *in = (uint8_t *)in_void + (size_t)4 * y * d->width;
      uint8_t *out = (uint8_t *)rowdata;

      // left

      for(int x = 0; x < left; x++, out += 3)
      {
        memcpy(out, white, 3 * sizeof(uint8_t));
      }

      // image

      for(int x = 0; x < d->width; x++, in += 4, out += 3)
      {
        memcpy(out, in, 3 * sizeof(uint8_t));
      }

      // right

      for(int x = 0; x < right; x++, out += 3)
      {
        memcpy(out, white, 3 * sizeof(uint8_t));
      }

      if(TIFFWriteScanline(tif, rowdata, y + top, 0) == -1)
      {
        rc = 1;
        goto exit;
      }
    }
  }

  // success
  rc = 0;

exit:
  // close the file before adding exif data
  if(tif)
  {
    TIFFClose(tif);
    tif = NULL;
  }
  if(!rc && exif)
  {
    rc = dt_exif_write_blob(exif, exif_len, filename);
    // Until we get symbolic error status codes, if rc is 1, return 0
    rc = (rc == 1) ? 0 : 1;
  }
  free(profile);
  profile = NULL;
  free(rowdata);
  rowdata = NULL;

  return rc;
}

size_t params_size(dt_imageio_module_format_t *self)
{
  return sizeof(dt_imageio_prt_t) - sizeof(TIFF *);
}

void *get_params(dt_imageio_module_format_t *self)
{
  dt_imageio_prt_t *d = (dt_imageio_prt_t *)calloc(1, sizeof(dt_imageio_prt_t));

  d->bpp = dt_conf_get_int("plugins/imageio/format/print/bpp");
  if(d->bpp == 16)
    d->bpp = 16;
  else
    d->bpp = 8;

  d->compress = dt_conf_get_int("plugins/imageio/format/print/compress");
  return d;
}

void free_params(dt_imageio_module_format_t *self, dt_imageio_module_data_t *params)
{
  free(params);
}

int set_params(dt_imageio_module_format_t *self, const void *params, const int size)
{
  return 0;
}

int bpp(dt_imageio_module_data_t *p)
{
  return ((dt_imageio_prt_t *)p)->bpp;
}

int compress(dt_imageio_module_data_t *p)
{
  return ((dt_imageio_prt_t *)p)->compress;
}

void init(dt_imageio_module_format_t *self)
{
}

void cleanup(dt_imageio_module_format_t *self)
{
}

const char *mime(dt_imageio_module_data_t *data)
{
  return "image/tiff";
}

const char *extension(dt_imageio_module_data_t *data)
{
  return "tif";
}

const char *name()
{
  return _("print");
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
