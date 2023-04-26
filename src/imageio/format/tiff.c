/*
    This file is part of darktable,
    Copyright (C) 2010-2023 darktable developers.

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

#include "bauhaus/bauhaus.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/exif.h"
#include "common/math.h"
#include "control/conf.h"
#include "imageio/imageio_common.h"
#include "imageio/imageio_module.h"
#include "imageio/format/imageio_format_api.h"
#include "develop/pixelpipe_hb.h"

#include <inttypes.h>
#include <memory.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <tiffio.h>
#ifdef HAVE_IMATH
#include "Imath/half.h"
#endif

// it would be nice to save space by storing the masks as single channel float data,
// but at least GIMP can't open TIFF files where not all layers have the same format.
#define MASKS_USE_SAME_FORMAT

DT_MODULE(4)

typedef struct dt_imageio_tiff_t
{
  dt_imageio_module_data_t global;
  int bpp;
  int pixelformat;
  int compress;
  int compresslevel;
  int shortfile;
  TIFF *handle;
} dt_imageio_tiff_t;

typedef struct dt_imageio_tiff_gui_t
{
  GtkWidget *bpp;
  GtkWidget *pixelformat;
  GtkWidget *compress;
  GtkWidget *compresslevel;
  GtkWidget *shortfiles;
} dt_imageio_tiff_gui_t;


int write_image(dt_imageio_module_data_t *d_tmp, const char *filename, const void *in_void,
                dt_colorspaces_color_profile_type_t over_type, const char *over_filename,
                void *exif, int exif_len, dt_imgid_t imgid, int num, int total, dt_dev_pixelpipe_t *pipe,
                const gboolean export_masks)
{
  const dt_imageio_tiff_t *d = (dt_imageio_tiff_t *)d_tmp;

  uint8_t *profile = NULL;
  uint32_t profile_len = 0;

  TIFF *tif = NULL;

  void *rowdata = NULL;

  gboolean free_mask = FALSE;
  float *raster_mask = NULL;
#ifdef _WIN32
  wchar_t *wfilename = g_utf8_to_utf16(filename, -1, NULL, NULL, NULL);
#endif
  int rc = 1; // default to error

  cmsHPROFILE out_profile = dt_colorspaces_get_output_profile(imgid, over_type, over_filename)->profile;
  cmsSaveProfileToMem(out_profile, NULL, &profile_len);
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

  uint16_t n_pages = 1;
  // only when masks are to be stored we check for extra pages!
  if(export_masks && pipe)
  {
    for(GList *iter = pipe->nodes; iter; iter = g_list_next(iter))
      n_pages += g_hash_table_size(((dt_dev_pixelpipe_iop_t *)iter->data)->raster_masks);
  }

  // Create little endian tiff image
#ifdef _WIN32
  tif = TIFFOpenW(wfilename, "wl");
#else
  tif = TIFFOpen(filename, "wl");
#endif

  if(!tif)
  {
    rc = 1;
    goto exit;
  }

  if(n_pages > 1)
  {
    TIFFSetField(tif, TIFFTAG_SUBFILETYPE, FILETYPE_PAGE);
    TIFFSetField(tif, TIFFTAG_PAGENAME, _("image"));
    TIFFSetField(tif, TIFFTAG_PAGENUMBER, 0, n_pages);
  }
  else
    TIFFSetField(tif, TIFFTAG_SUBFILETYPE, 0);

  TIFFSetField(tif, TIFFTAG_DOCUMENTNAME, filename);

  // http://partners.adobe.com/public/developer/en/tiff/TIFFphotoshop.pdf (dated 2002)
  // "A proprietary ZIP/Flate compression code (0x80b2) has been used by some"
  // "software vendors. This code should be considered obsolete. We recommend"
  // "that TIFF implementations recognize and read the obsolete code but only"
  // "write the official compression code (0x0008)."
  // http://www.awaresystems.be/imaging/tiff/tifftags/compression.html
  // http://www.awaresystems.be/imaging/tiff/tifftags/predictor.html
  if(d->compress == 1)
  {
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
    TIFFSetField(tif, TIFFTAG_PREDICTOR, PREDICTOR_NONE);
    TIFFSetField(tif, TIFFTAG_ZIPQUALITY, (uint16_t)d->compresslevel);
  }
  else if(d->compress == 2)
  {
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
    if(d->bpp == 32 || (d->bpp == 16 && d->pixelformat))
      TIFFSetField(tif, TIFFTAG_PREDICTOR, PREDICTOR_FLOATINGPOINT);
    else
      TIFFSetField(tif, TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL);
    TIFFSetField(tif, TIFFTAG_ZIPQUALITY, (uint16_t)d->compresslevel);
  }

  if(profile != NULL)
  {
    TIFFSetField(tif, TIFFTAG_ICCPROFILE, (uint32_t)profile_len, profile);
  }

/* Howto check for a grayscale image?
   We test every pixel for differences between the rgb channels using specific thresholds
   for every precision. If there is such a pixel we keep it as an rgb image, otherwise
   it's safe to assume a grayscale.
   As there might be pipeline errors at the border we leave them alone.
   After these checks layers can be used later on.
*/
  uint16_t layers = 3;  // default are rgb images

  if((d->global.height > 4) && (d->global.width > 4) && d->shortfile)
  {
    layers = 1;    // let's now assume a grayscale
    if(d->bpp == 32 || (d->bpp == 16 && d->pixelformat))
    {
      for(int y = 1; y < d->global.height-1; y++)
      {
        float *in = (float *)in_void + (size_t)4 * y * d->global.width;
        for(int x = 1; x < d->global.width-1; x++, in += 4)
        {
          if((fabsf(fmaxf(in[0], 0.001f) / fmaxf(in[1], 0.001f)) > 1.01f) ||
             (fabsf(fmaxf(in[0], 0.001f) / fmaxf(in[2], 0.001f)) > 1.01f) ||
             (fabsf(fmaxf(in[1], 0.001f) / fmaxf(in[2], 0.001f)) > 1.01f))
          {
            layers = 3;
            goto checkdone;
          }
        }
      }
    }
    else if(d->bpp == 16 && !d->pixelformat)
    {
      for(int y = 1; y < d->global.height-1; y++)
      {
        uint16_t *in = (uint16_t *)in_void + (size_t)4 * y * d->global.width;
        for(int x = 1; x < d->global.width-1; x++, in += 4)
        {
          if((abs(in[0] - in[1]) > 100) ||
             (abs(in[0] - in[2]) > 100) ||
             (abs(in[1] - in[2]) > 100))
          {
            layers = 3;
            goto checkdone;
          }
        }
      }
    }
    else // 8bpp
    {
      for(int y = 1; y < d->global.height-1; y++)
      {
        uint8_t *in = (uint8_t *)in_void + (size_t)4 * y * d->global.width;
        for(int x = 1; x < d->global.width-1; x++, in += 4)
        {
          if((abs(in[0] - in[1]) > 5) ||
             (abs(in[0] - in[2]) > 5) ||
             (abs(in[1] - in[2]) > 5))
          {
            layers = 3;
            goto checkdone;
          }
        }
      }
    }
  }

  checkdone:
  if(layers == 1)
    dt_control_log(_("will export as a grayscale image"));

  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, layers);
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, (uint16_t)d->bpp);
  TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT,
               d->bpp == 32 || (d->bpp == 16 && d->pixelformat) ? SAMPLEFORMAT_IEEEFP : SAMPLEFORMAT_UINT);
  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, (uint32_t)d->global.width);
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, (uint32_t)d->global.height);
  if(layers == 3)
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
  else
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);

  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tif, 0));

  const int resolution = dt_conf_get_int("metadata/resolution");
  TIFFSetField(tif, TIFFTAG_XRESOLUTION, (float)resolution);
  TIFFSetField(tif, TIFFTAG_YRESOLUTION, (float)resolution);
  TIFFSetField(tif, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);

  const size_t rowsize = (d->global.width * layers) * d->bpp / 8;
  if((rowdata = malloc(rowsize)) == NULL)
  {
    rc = 1;
    goto exit;
  }

  if(d->bpp == 32)
  {
    for(int y = 0; y < d->global.height; y++)
    {
      float *in = (float *)in_void + (size_t)4 * y * d->global.width;
      float *out = (float *)rowdata;

      for(int x = 0; x < d->global.width; x++, in += 4, out += layers)
      {
        memcpy(out, in, sizeof(float) * layers);
      }

      if(TIFFWriteScanline(tif, rowdata, y, 0) == -1)
      {
        rc = 1;
        goto exit;
      }
    }
  }
#ifdef HAVE_IMATH
  else if(d->bpp == 16 && d->pixelformat)
  {
    for(int y = 0; y < d->global.height; y++)
    {
      float *in = (float *)in_void + (size_t)4 * y * d->global.width;
      uint16_t *out = (uint16_t *)rowdata;

      for(int x = 0; x < d->global.width; x++, in += 4, out += layers)
      {
        for(int l = 0; l < layers; ++l) out[l] = imath_float_to_half(in[l]);
      }

      if(TIFFWriteScanline(tif, rowdata, y, 0) == -1)
      {
        rc = 1;
        goto exit;
      }
    }
  }
#endif
  else if(d->bpp == 16 && !d->pixelformat)
  {
    for(int y = 0; y < d->global.height; y++)
    {
      uint16_t *in = (uint16_t *)in_void + (size_t)4 * y * d->global.width;
      uint16_t *out = (uint16_t *)rowdata;

      for(int x = 0; x < d->global.width; x++, in += 4, out += layers)
      {
        memcpy(out, in, sizeof(uint16_t) * layers);
      }

      if(TIFFWriteScanline(tif, rowdata, y, 0) == -1)
      {
        rc = 1;
        goto exit;
      }
    }
  }
  else // 8bpp
  {
    for(int y = 0; y < d->global.height; y++)
    {
      uint8_t *in = (uint8_t *)in_void + (size_t)4 * y * d->global.width;
      uint8_t *out = (uint8_t *)rowdata;

      for(int x = 0; x < d->global.width; x++, in += 4, out += layers)
      {
        memcpy(out, in, sizeof(uint8_t) * layers);
      }

      if(TIFFWriteScanline(tif, rowdata, y, 0) == -1)
      {
        rc = 1;
        goto exit;
      }
    }
  }

  rc = 0;

  // close the file before adding exif data
  if(tif)
  {
    TIFFClose(tif);
    tif = NULL;
  }
  if(exif)
  {
    rc = dt_exif_write_blob(exif, exif_len, filename, d->compress > 0);
    // Until we get symbolic error status codes, if rc is 1, return 0
    rc = (rc == 1) ? 0 : 1;
  }

  // exiv2 doesn't support multi page tiffs. so we have to write in two steps. :-(

  if(rc == 0 && n_pages > 1)
  {
#ifdef _WIN32
    tif = TIFFOpenW(wfilename, "al");
#else
    tif = TIFFOpen(filename, "al");
#endif

    if(!tif)
    {
      rc = 1;
      goto exit;
    }

    // add masks
    float missing_raster_mask[8 * 8] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                                         0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.0, 0.0,
                                         0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0, 0.0,
                                         0.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 0.0,
                                         0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0,
                                         0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                                         0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.0, 0.0,
                                         0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    static const size_t missing_raster_mask_w = 8, missing_raster_mask_h = 8;
    uint16_t page = 1;
    for(GList *iter = pipe->nodes; iter; iter = g_list_next(iter))
    {
      dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)iter->data;

      GHashTableIter rm_iter;
      gpointer key, value;

      g_hash_table_iter_init(&rm_iter, piece->raster_masks);
      while(g_hash_table_iter_next(&rm_iter, &key, &value))
      {
        if(free_mask) dt_free_align(raster_mask);
        raster_mask = dt_dev_get_raster_mask(piece, piece->module, GPOINTER_TO_INT(key), NULL, &free_mask);


        size_t w = d->global.width, h = d->global.height;
        if(!raster_mask)
        {
          // this should never happen
          w = missing_raster_mask_w;
          h = missing_raster_mask_h;
          raster_mask = missing_raster_mask;
          free_mask = FALSE;
        }

        TIFFSetField(tif, TIFFTAG_SUBFILETYPE, FILETYPE_PAGE);
        TIFFSetField(tif, TIFFTAG_PAGENUMBER, page, n_pages);

        const char *pagename = g_hash_table_lookup(piece->module->raster_mask.source.masks, key);
        if(pagename)
          TIFFSetField(tif, TIFFTAG_PAGENAME, pagename);
        else
          TIFFSetField(tif, TIFFTAG_PAGENAME, piece->module->name());

        if(d->compress == 1)
        {
          TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
          TIFFSetField(tif, TIFFTAG_PREDICTOR, PREDICTOR_NONE);
          TIFFSetField(tif, TIFFTAG_ZIPQUALITY, (uint16_t)d->compresslevel);
        }
        else if(d->compress == 2)
        {
          TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
          if(d->bpp == 32 || (d->bpp == 16 && d->pixelformat))
            TIFFSetField(tif, TIFFTAG_PREDICTOR, PREDICTOR_FLOATINGPOINT);
          else
            TIFFSetField(tif, TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL);
          TIFFSetField(tif, TIFFTAG_ZIPQUALITY, (uint16_t)d->compresslevel);
        }

        TIFFSetField(tif, TIFFTAG_XRESOLUTION, (float)resolution);
        TIFFSetField(tif, TIFFTAG_YRESOLUTION, (float)resolution);
        TIFFSetField(tif, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);

        TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, (uint32_t)w);
        TIFFSetField(tif, TIFFTAG_IMAGELENGTH, (uint32_t)h);
        TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);

#ifdef MASKS_USE_SAME_FORMAT
        TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, layers);
        TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, (uint16_t)d->bpp);
        TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT,
                     d->bpp == 32 || (d->bpp == 16 && d->pixelformat) ? SAMPLEFORMAT_IEEEFP : SAMPLEFORMAT_UINT);
        if(layers == 3)
          TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
        else
          TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
        TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tif, 0));

        if(w != d->global.width)
        {
          free(rowdata);
          const size_t _rowsize = (w * layers) * d->bpp / 8;
          rowdata = malloc(_rowsize);
        }

        if(d->bpp == 32)
        {
          for(int y = 0; y < h; y++)
          {
            const float *in = raster_mask + (size_t)y * w;
            float *out = (float *)rowdata;

            for(int x = 0; x < w; x++, out += layers)
            {
              for(int c = 0; c < layers; c++)
                out[c] = in[x];
            }

            if(TIFFWriteScanline(tif, rowdata, y, 0) == -1)
            {
              rc = 1;
              goto exit;
            }
          }
        }
#ifdef HAVE_IMATH
        else if(d->bpp == 16 && d->pixelformat)
        {
          for(int y = 0; y < h; y++)
          {
            const float *in = raster_mask + (size_t)y * w;
            uint16_t *out = (uint16_t *)rowdata;

            for(int x = 0; x < w; x++, out += layers)
            {
              for(int c = 0; c < layers; c++)
                out[c] = imath_float_to_half(in[x]);
            }

            if(TIFFWriteScanline(tif, rowdata, y, 0) == -1)
            {
              rc = 1;
              goto exit;
            }
          }
        }
#endif
        else if(d->bpp == 16 && !d->pixelformat)
        {
          for(int y = 0; y < h; y++)
          {
            const float *in = raster_mask + (size_t)y * w;
            uint16_t *out = (uint16_t *)rowdata;

            for(int x = 0; x < w; x++, out += layers)
            {
              for(int c = 0; c < layers; c++)
                out[c] = (uint16_t)roundf(CLIP(in[x]) * 65535.0f);
            }

            if(TIFFWriteScanline(tif, rowdata, y, 0) == -1)
            {
              rc = 1;
              goto exit;
            }
          }
        }
        else // 8 bpp
        {
          for(int y = 0; y < h; y++)
          {
            const float *in = raster_mask + (size_t)y * w;
            uint8_t *out = (uint8_t *)rowdata;

            for(int x = 0; x < w; x++, out += layers)
            {
              for(int c = 0; c < layers; c++)
                out[c] = (uint8_t)roundf(CLIP(in[x]) * 255.0f);
            }

            if(TIFFWriteScanline(tif, rowdata, y, 0) == -1)
            {
              rc = 1;
              goto exit;
            }
          }
        }
#else // MASKS_USE_SAME_FORMAT
        TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
        TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 32);
        TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP);
        if(d->compress == 2) // override predictor set above assuming MASKS_USE_SAME_FORMAT
            TIFFSetField(tif, TIFFTAG_PREDICTOR, PREDICTOR_FLOATINGPOINT);
        TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
        TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tif, 0));

        for(int y = 0; y < h; y++)
        {
          const float *in = raster_mask + (size_t)y * w;
          if(TIFFWriteScanline(tif, (void *)in, y, 0) == -1)
          {
            rc = 1;
            goto exit;
          }
        }
#endif // MASKS_USE_SAME_FORMAT

        page++;

        if(page < n_pages)
        {
          TIFFWriteDirectory(tif);
        }
      } // for all raster masks
    } // for all pipe nodes

    // success
    rc = 0;
  } // if more than 1 page, i.e., there are masks

exit:
  if(tif)
  {
    TIFFClose(tif);
    tif = NULL;
  }
  free(profile);
  profile = NULL;
  free(rowdata);
  rowdata = NULL;
#ifdef _WIN32
  g_free(wfilename);
#endif
  if(free_mask)
    dt_free_align(raster_mask);

  return rc;
}

size_t params_size(dt_imageio_module_format_t *self)
{
  return sizeof(dt_imageio_tiff_t) - sizeof(TIFF *);
}

void *legacy_params(dt_imageio_module_format_t *self, const void *const old_params, const size_t old_params_size,
                    const int old_version, const int new_version, size_t *new_size)
{
  if(old_version == 1 && new_version == 4)
  {
    typedef struct dt_imageio_tiff_v1_t
    {
      int max_width, max_height;
      int width, height;
      char style[128];
      int bpp;
      int compress;
      TIFF *handle;
    } dt_imageio_tiff_v1_t;

    const dt_imageio_tiff_v1_t *o = (dt_imageio_tiff_v1_t *)old_params;
    dt_imageio_tiff_t *n = (dt_imageio_tiff_t *)calloc(1, sizeof(dt_imageio_tiff_t));

    n->global.max_width = o->max_width;
    n->global.max_height = o->max_height;
    n->global.width = o->width;
    n->global.height = o->height;
    g_strlcpy(n->global.style, o->style, sizeof(o->style));
    n->global.style_append = FALSE;
    n->bpp = o->bpp;
    n->pixelformat = 0;
    n->compress = o->compress == 3 ? 2 : o->compress; // drop redundant float case
    n->compresslevel = 6;
    n->shortfile = 0;
    n->handle = o->handle;
    *new_size = self->params_size(self);
    return n;
  }
  else if(old_version == 2 && new_version == 4)
  {
    typedef struct dt_imageio_tiff_v2_t
    {
      int max_width, max_height;
      int width, height;
      char style[128];
      gboolean style_append;
      int bpp;
      int compress;
      TIFF *handle;
    } dt_imageio_tiff_v2_t;

    const dt_imageio_tiff_v2_t *o = (dt_imageio_tiff_v2_t *)old_params;
    dt_imageio_tiff_t *n = (dt_imageio_tiff_t *)calloc(1, sizeof(dt_imageio_tiff_t));

    n->global.max_width = o->max_width;
    n->global.max_height = o->max_height;
    n->global.width = o->width;
    n->global.height = o->height;
    g_strlcpy(n->global.style, o->style, sizeof(o->style));
    n->global.style_append = o->style_append;
    n->bpp = o->bpp;
    n->pixelformat = 0;
    n->compress = o->compress == 3 ? 2 : o->compress; // drop redundant float case
    n->compresslevel = 6;
    n->shortfile = 0;
    n->handle = o->handle;
    *new_size = self->params_size(self);
    return n;
  }
  else if(old_version == 3 && new_version == 4)
  {
    typedef struct dt_imageio_tiff_v3_t
    {
      dt_imageio_module_data_t global;
      int bpp;
      int compress;
      int compresslevel;
      int shortfile;
      TIFF *handle;
    } dt_imageio_tiff_v3_t;

    const dt_imageio_tiff_v3_t *o = (dt_imageio_tiff_v3_t *)old_params;
    dt_imageio_tiff_t *n = (dt_imageio_tiff_t *)calloc(1, sizeof(dt_imageio_tiff_t));

    n->global.max_width = o->global.max_width;
    n->global.max_height = o->global.max_height;
    n->global.width = o->global.width;
    n->global.height = o->global.height;
    g_strlcpy(n->global.style, o->global.style, sizeof(o->global.style));
    n->global.style_append = o->global.style_append;
    n->bpp = o->bpp;
    n->pixelformat = 0;
    if(o->compresslevel)
    {
      n->compress = o->compress == 3 ? 2 : o->compress; // drop redundant float case
      n->compresslevel = o->compresslevel;
    }
    else
    {
      n->compress = 0;
      n->compresslevel = 6;
    }
    n->shortfile = o->shortfile;
    n->handle = o->handle;
    *new_size = self->params_size(self);
    return n;
  }
  return NULL;
}

void *get_params(dt_imageio_module_format_t *self)
{
  dt_imageio_tiff_t *d = (dt_imageio_tiff_t *)calloc(1, sizeof(dt_imageio_tiff_t));

  if(!d) return NULL;

  d->bpp = dt_conf_get_int("plugins/imageio/format/tiff/bpp");
#ifdef HAVE_IMATH
  d->pixelformat = dt_conf_get_bool("plugins/imageio/format/tiff/pixelformat");
#else
  d->pixelformat = 0;
#endif
  d->compress = dt_conf_get_int("plugins/imageio/format/tiff/compress");
  d->compresslevel = dt_conf_get_int("plugins/imageio/format/tiff/compresslevel");
  d->shortfile = dt_conf_get_int("plugins/imageio/format/tiff/shortfile");

  return d;
}

void free_params(dt_imageio_module_format_t *self, dt_imageio_module_data_t *params)
{
  free(params);
}

int set_params(dt_imageio_module_format_t *self, const void *params, const int size)
{
  if(size != self->params_size(self)) return 1;
  const dt_imageio_tiff_t *d = (dt_imageio_tiff_t *)params;
  const dt_imageio_tiff_gui_t *g = (dt_imageio_tiff_gui_t *)self->gui_data;

  dt_bauhaus_combobox_set(g->bpp, d->bpp == 16 ? 1 : d->bpp == 32 ? 2 : 0);
  dt_bauhaus_combobox_set(g->pixelformat, d->pixelformat & 1);
  dt_bauhaus_combobox_set(g->compress, d->compress);
  dt_bauhaus_slider_set(g->compresslevel, d->compresslevel);
  dt_bauhaus_combobox_set(g->shortfiles, d->shortfile);

  return 0;
}

int bpp(dt_imageio_module_data_t *p)
{
  const dt_imageio_tiff_t *d = (dt_imageio_tiff_t *)p;

  if(d->bpp == 32 || (d->bpp == 16 && d->pixelformat)) return 32;

  return d->bpp;
}

int levels(dt_imageio_module_data_t *p)
{
  const dt_imageio_tiff_t *d = (dt_imageio_tiff_t *)p;

  int ret = IMAGEIO_RGB;

  if(d->bpp == 8)
    ret |= IMAGEIO_INT8;
  else if(d->bpp == 16 && !d->pixelformat)
    ret |= IMAGEIO_INT16;
  else
    ret |= IMAGEIO_FLOAT;

  return ret;
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
  return _("TIFF");
}

static void bpp_combobox_changed(GtkWidget *widget, dt_imageio_tiff_gui_t *gui)
{
  const int bpp_enum = dt_bauhaus_combobox_get(widget);
  dt_conf_set_int("plugins/imageio/format/tiff/bpp", bpp_enum == 1 ? 16 : bpp_enum == 2 ? 32 : 8);

#ifdef HAVE_IMATH
  gtk_widget_set_visible(gui->pixelformat, bpp_enum == 1);
#endif
}

static void pixelformat_combobox_changed(GtkWidget *widget, gpointer user_data)
{
  const int pixelformat = dt_bauhaus_combobox_get(widget);
  dt_conf_set_bool("plugins/imageio/format/tiff/pixelformat", pixelformat);
}

static void shortfile_combobox_changed(GtkWidget *widget, gpointer user_data)
{
  const int mode = dt_bauhaus_combobox_get(widget);
  dt_conf_set_int("plugins/imageio/format/tiff/shortfile", mode);
}

static void compress_combobox_changed(GtkWidget *widget, dt_imageio_tiff_gui_t *gui)
{
  const int compress = dt_bauhaus_combobox_get(widget);
  dt_conf_set_int("plugins/imageio/format/tiff/compress", compress);

  gtk_widget_set_visible(gui->compresslevel, compress != 0);
}

static void compress_level_changed(GtkWidget *slider, gpointer user_data)
{
  const int compresslevel = (int)dt_bauhaus_slider_get(slider);
  dt_conf_set_int("plugins/imageio/format/tiff/compresslevel", compresslevel);
}

void init(dt_imageio_module_format_t *self)
{
#ifdef USE_LUA
  dt_lua_register_module_member(darktable.lua_state.state, self, dt_imageio_tiff_t, bpp, int);
#endif
}

void cleanup(dt_imageio_module_format_t *self)
{
}

void gui_init(dt_imageio_module_format_t *self)
{
  dt_imageio_tiff_gui_t *gui = (dt_imageio_tiff_gui_t *)malloc(sizeof(dt_imageio_tiff_gui_t));
  if(!gui) return;
  self->gui_data = (void *)gui;

  const int bpp = dt_conf_get_int("plugins/imageio/format/tiff/bpp");
#ifdef HAVE_IMATH
  const int pixelformat = dt_conf_get_bool("plugins/imageio/format/tiff/pixelformat") & 1;
#else
  const int pixelformat = 0;
#endif
  const int compress = dt_conf_get_int("plugins/imageio/format/tiff/compress");
  const int compresslevel = dt_conf_get_int("plugins/imageio/format/tiff/compresslevel");
  const int shortmode = dt_conf_get_int("plugins/imageio/format/tiff/shortfile");

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  // Bit depth combo box
  DT_BAUHAUS_COMBOBOX_NEW_FULL(gui->bpp, self, NULL, N_("bit depth"), NULL,
                               bpp == 16   ? 1
                               : bpp == 32 ? 2
                                           : 0,
                               bpp_combobox_changed, gui, N_("8 bit"), N_("16 bit"), N_("32 bit (float)"));
  gtk_box_pack_start(GTK_BOX(self->widget), gui->bpp, TRUE, TRUE, 0);


  // Pixel format combo box
  DT_BAUHAUS_COMBOBOX_NEW_FULL(gui->pixelformat, self, NULL, N_("pixel type"), NULL, pixelformat,
                               pixelformat_combobox_changed, NULL, N_("unsigned integer"), N_("floating point"));
#ifdef HAVE_IMATH
  dt_bauhaus_combobox_set_default(gui->pixelformat,
                                  dt_confgen_get_bool("plugins/imageio/format/tiff/pixelformat", DT_DEFAULT) & 1);
  gtk_widget_set_visible(gui->pixelformat, bpp == 16);
#else
  gtk_widget_set_visible(gui->pixelformat, FALSE);
#endif
  gtk_box_pack_start(GTK_BOX(self->widget), gui->pixelformat, TRUE, TRUE, 0);

  gtk_widget_set_no_show_all(gui->pixelformat, TRUE);

  // Compression method combo box
  DT_BAUHAUS_COMBOBOX_NEW_FULL(gui->compress, self, NULL, N_("compression"), NULL, compress,
                               compress_combobox_changed, gui, N_("uncompressed"), N_("deflate"),
                               N_("deflate with predictor"));
  dt_bauhaus_combobox_set_default(gui->compress,
                                  dt_confgen_get_int("plugins/imageio/format/tiff/compress", DT_DEFAULT));
  gtk_box_pack_start(GTK_BOX(self->widget), gui->compress, TRUE, TRUE, 0);

  // Compression level slider
  gui->compresslevel = dt_bauhaus_slider_new_with_range(
      (dt_iop_module_t *)self, dt_confgen_get_int("plugins/imageio/format/tiff/compresslevel", DT_MIN),
      dt_confgen_get_int("plugins/imageio/format/tiff/compresslevel", DT_MAX), 1,
      dt_confgen_get_int("plugins/imageio/format/tiff/compresslevel", DT_DEFAULT), 0);
  dt_bauhaus_widget_set_label(gui->compresslevel, NULL, N_("compression level"));
  dt_bauhaus_slider_set(gui->compresslevel, compresslevel);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(gui->compresslevel), TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(gui->compresslevel), "value-changed", G_CALLBACK(compress_level_changed), NULL);

  gtk_widget_set_visible(gui->compresslevel, compress != 0);
  gtk_widget_set_no_show_all(gui->compresslevel, TRUE);

  // shortfile option combo box
  DT_BAUHAUS_COMBOBOX_NEW_FULL(gui->shortfiles, self, NULL, N_("b&w image"), NULL, shortmode,
                               shortfile_combobox_changed, self, N_("write rgb colors"), N_("write grayscale"));
  dt_bauhaus_combobox_set_default(gui->shortfiles,
                                  dt_confgen_get_int("plugins/imageio/format/tiff/shortfile", DT_DEFAULT));
  gtk_box_pack_start(GTK_BOX(self->widget), gui->shortfiles, TRUE, TRUE, 0);
}

void gui_cleanup(dt_imageio_module_format_t *self)
{
  free(self->gui_data);
}

void gui_reset(dt_imageio_module_format_t *self)
{
  dt_imageio_tiff_gui_t *gui = (dt_imageio_tiff_gui_t *)self->gui_data;

  const int bpp = dt_confgen_get_int("plugins/imageio/format/tiff/bpp", DT_DEFAULT);
  dt_bauhaus_combobox_set(gui->bpp, bpp == 16 ? 1 : bpp == 32 ? 2 : 0);
#ifdef HAVE_IMATH
  dt_bauhaus_combobox_set(gui->pixelformat,
                          dt_confgen_get_bool("plugins/imageio/format/tiff/pixelformat", DT_DEFAULT) & 1);
#else
  dt_bauhaus_combobox_set(gui->pixelformat, 0);
#endif
  dt_bauhaus_combobox_set(gui->compress, dt_confgen_get_int("plugins/imageio/format/tiff/compress", DT_DEFAULT));
  dt_bauhaus_slider_set(gui->compresslevel,
                        dt_confgen_get_int("plugins/imageio/format/tiff/compresslevel", DT_DEFAULT));
  dt_bauhaus_combobox_set(gui->shortfiles, dt_confgen_get_int("plugins/imageio/format/tiff/shortfile", DT_DEFAULT));
}

int flags(dt_imageio_module_data_t *data)
{
  return FORMAT_FLAGS_SUPPORT_XMP | FORMAT_FLAGS_SUPPORT_LAYERS;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
