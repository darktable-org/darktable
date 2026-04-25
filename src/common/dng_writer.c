/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

#include "common/dng_writer.h"
#include "common/darktable.h"
#include "common/exif.h"
#include "common/image.h"
#include "develop/imageop_math.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <tiffio.h>

#ifdef _WIN32
#include <wchar.h>
#endif

// DNG uses SRATIONAL / RATIONAL for matrix and WB tags. libtiff accepts
// these as float/double arrays and handles the conversion; we just pass
// the values as double

// map the dcraw 2x2 CFA filters word to 4 single-byte channel indices
// for the DNG CFAPattern tag: 0=R, 1=G, 2=B, following DNG spec §A.3.1
static void _cfa_bytes_from_filters(uint32_t filters, uint8_t out[4])
{
  out[0] = FC(0, 0, filters);
  out[1] = FC(0, 1, filters);
  out[2] = FC(1, 0, filters);
  out[3] = FC(1, 1, filters);
}

int dt_dng_write_cfa_bayer(const char *filename,
                           const uint16_t *cfa,
                           int width,
                           int height,
                           const dt_image_t *img,
                           const void *exif_blob,
                           int exif_len)
{
  if(!filename || !cfa || !img || width <= 0 || height <= 0)
    return 1;

#ifdef _WIN32
  wchar_t *wfilename = g_utf8_to_utf16(filename, -1, NULL, NULL, NULL);
  TIFF *tif = TIFFOpenW(wfilename, "wl");
  g_free(wfilename);
#else
  TIFF *tif = TIFFOpen(filename, "wl");
#endif
  if(!tif) return 1;

  // required baseline TIFF tags for a single-plane raw image
  TIFFSetField(tif, TIFFTAG_SUBFILETYPE, 0);
  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, (uint32_t)width);
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, (uint32_t)height);
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, (uint16_t)16);
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, (uint16_t)1);
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_CFA);
  TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);
  TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
  TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tif, 0));
  TIFFSetField(tif, TIFFTAG_XRESOLUTION, 300.0);
  TIFFSetField(tif, TIFFTAG_YRESOLUTION, 300.0);
  TIFFSetField(tif, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
  {
    gchar *software = g_strdup_printf("darktable %s",
                                       darktable_package_version);
    TIFFSetField(tif, TIFFTAG_SOFTWARE, software);
    g_free(software);
  }

  // camera identification
  if(img->camera_maker[0])
    TIFFSetField(tif, TIFFTAG_MAKE, img->camera_maker);
  if(img->camera_model[0])
    TIFFSetField(tif, TIFFTAG_MODEL, img->camera_model);
  if(img->camera_makermodel[0])
    TIFFSetField(tif, TIFFTAG_UNIQUECAMERAMODEL, img->camera_makermodel);

  // DNG identification
  const uint8_t dng_version[4] = { 1, 4, 0, 0 };
  const uint8_t dng_backward[4] = { 1, 2, 0, 0 };
  TIFFSetField(tif, TIFFTAG_DNGVERSION, dng_version);
  TIFFSetField(tif, TIFFTAG_DNGBACKWARDVERSION, dng_backward);

  // CFA description
  const uint16_t cfa_repeat_dim[2] = { 2, 2 };
  TIFFSetField(tif, TIFFTAG_CFAREPEATPATTERNDIM, cfa_repeat_dim);

  uint8_t cfa_pattern[4];
  _cfa_bytes_from_filters(img->buf_dsc.filters, cfa_pattern);
  TIFFSetField(tif, TIFFTAG_CFAPATTERN, 4, cfa_pattern);

  const uint8_t cfa_plane_color[3] = { 0, 1, 2 };   // R, G, B
  TIFFSetField(tif, TIFFTAG_CFAPLANECOLOR, 3, cfa_plane_color);
  TIFFSetField(tif, TIFFTAG_CFALAYOUT, (uint16_t)1); // rectangular

  // black/white levels
  // BlackLevel is declared as a 2x2 repeat over the CFA pattern. we
  // honor per-channel values when rawspeed provided them, otherwise
  // fall back to the single raw_black_level broadcast to all four
  const uint16_t bl_repeat_dim[2] = { 2, 2 };
  TIFFSetField(tif, TIFFTAG_BLACKLEVELREPEATDIM, bl_repeat_dim);

  float black_level[4];
  const gboolean have_separate
    = (img->raw_black_level_separate[0] != 0
       || img->raw_black_level_separate[1] != 0
       || img->raw_black_level_separate[2] != 0
       || img->raw_black_level_separate[3] != 0);
  for(int i = 0; i < 4; i++)
  {
    black_level[i] = have_separate
      ? (float)img->raw_black_level_separate[i]
      : (float)img->raw_black_level;
  }
  TIFFSetField(tif, TIFFTAG_BLACKLEVEL, 4, black_level);

  const uint32_t white = img->raw_white_point
    ? img->raw_white_point : 65535u;
  TIFFSetField(tif, TIFFTAG_WHITELEVEL, 1, &white);

  // AsShotNeutral (derived from wb_coeffs)
  // DNG AsShotNeutral encodes the neutral white balance as a
  // cameraRGB triple where smaller values mean more amplification.
  // darktable's wb_coeffs are raw-to-white multipliers; AsShotNeutral
  // is their inverse, normalized so the maximum element is 1
  if(img->wb_coeffs[0] > 0.0f
     && img->wb_coeffs[1] > 0.0f
     && img->wb_coeffs[2] > 0.0f)
  {
    float inv[3];
    for(int i = 0; i < 3; i++)
      inv[i] = 1.0f / img->wb_coeffs[i];
    const float m = fmaxf(inv[0], fmaxf(inv[1], inv[2]));
    if(m > 0.0f)
      for(int i = 0; i < 3; i++) inv[i] /= m;
    TIFFSetField(tif, TIFFTAG_ASSHOTNEUTRAL, 3, inv);
  }

  // ColorMatrix1 (XYZ D50 -> cameraRGB, 3x3 for trichromatic)
  // darktable's adobe_XYZ_to_CAM is populated from the rawspeed
  // cameras.xml matrix in row-major [camRGB][XYZ] layout, which
  // matches the DNG ColorMatrix1 layout exactly (row = camera axis,
  // column = XYZ axis)
  {
    float non_zero = 0.0f;
    for(int k = 0; k < 3; k++)
      for(int i = 0; i < 3; i++)
        non_zero += fabsf(img->adobe_XYZ_to_CAM[k][i]);

    if(non_zero > 0.0f)
    {
      float color_matrix[9];
      for(int k = 0; k < 3; k++)
        for(int i = 0; i < 3; i++)
          color_matrix[k * 3 + i] = img->adobe_XYZ_to_CAM[k][i];
      TIFFSetField(tif, TIFFTAG_COLORMATRIX1, 9, color_matrix);
    }
  }

  // advertise the visible region inside the full raw buffer; without
  // these tags the importer renders the optical-black margins too
  const int crop_x = (img->crop_x > 0) ? img->crop_x : 0;
  const int crop_y = (img->crop_y > 0) ? img->crop_y : 0;
  const int vis_w  = (img->p_width  > 0 && img->p_width  <= width  - crop_x)
                     ? img->p_width  : (width  - crop_x);
  const int vis_h  = (img->p_height > 0 && img->p_height <= height - crop_y)
                     ? img->p_height : (height - crop_y);

  const uint32_t active_area[4] = {
    (uint32_t)crop_y, (uint32_t)crop_x,
    (uint32_t)(crop_y + vis_h), (uint32_t)(crop_x + vis_w),
  };
  const float default_scale[2] = { 1.0f, 1.0f };
  const float default_crop_origin[2] = { 0.0f, 0.0f };
  const float default_crop_size[2] = { (float)vis_w, (float)vis_h };
  TIFFSetField(tif, TIFFTAG_ACTIVEAREA, active_area);
  TIFFSetField(tif, TIFFTAG_DEFAULTSCALE, default_scale);
  TIFFSetField(tif, TIFFTAG_DEFAULTCROPORIGIN, default_crop_origin);
  TIFFSetField(tif, TIFFTAG_DEFAULTCROPSIZE, default_crop_size);

  // scanline write
  int res = 0;
  for(int y = 0; y < height && res == 0; y++)
  {
    const uint16_t *row = cfa + (size_t)y * width;
    if(TIFFWriteScanline(tif, (void *)row, y, 0) < 0)
      res = 1;
  }

  TIFFClose(tif);

  // embed source EXIF (datetime, ISO, shutter, etc.)
  // dt_exif_write_blob takes a non-const pointer; we don't modify it
  if(res == 0 && exif_blob && exif_len > 0)
    dt_exif_write_blob((uint8_t *)exif_blob, (uint32_t)exif_len,
                       filename, 0);

  if(res != 0)
    g_unlink(filename);

  return res;
}

int dt_dng_write_linear(const char *filename,
                        const float *rgb,
                        int width,
                        int height,
                        const dt_image_t *img,
                        const void *exif_blob,
                        int exif_len)
{
  if(!filename || !rgb || !img || width <= 0 || height <= 0)
    return 1;

#ifdef _WIN32
  wchar_t *wfilename = g_utf8_to_utf16(filename, -1, NULL, NULL, NULL);
  TIFF *tif = TIFFOpenW(wfilename, "wl");
  g_free(wfilename);
#else
  TIFF *tif = TIFFOpen(filename, "wl");
#endif
  if(!tif) return 1;

  // baseline TIFF tags, 3 samples per pixel (demosaicked)
  TIFFSetField(tif, TIFFTAG_SUBFILETYPE, 0);
  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, (uint32_t)width);
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, (uint32_t)height);
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, (uint16_t)16);
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, (uint16_t)3);
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, 34892);  // LinearRaw
  TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);
  TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
  TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tif, 0));
  TIFFSetField(tif, TIFFTAG_XRESOLUTION, 300.0);
  TIFFSetField(tif, TIFFTAG_YRESOLUTION, 300.0);
  TIFFSetField(tif, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
  {
    gchar *software = g_strdup_printf("darktable %s",
                                       darktable_package_version);
    TIFFSetField(tif, TIFFTAG_SOFTWARE, software);
    g_free(software);
  }

  // camera identification
  if(img->camera_maker[0])
    TIFFSetField(tif, TIFFTAG_MAKE, img->camera_maker);
  if(img->camera_model[0])
    TIFFSetField(tif, TIFFTAG_MODEL, img->camera_model);
  if(img->camera_makermodel[0])
    TIFFSetField(tif, TIFFTAG_UNIQUECAMERAMODEL, img->camera_makermodel);

  // DNG identification
  const uint8_t dng_version[4] = { 1, 4, 0, 0 };
  const uint8_t dng_backward[4] = { 1, 2, 0, 0 };
  TIFFSetField(tif, TIFFTAG_DNGVERSION, dng_version);
  TIFFSetField(tif, TIFFTAG_DNGBACKWARDVERSION, dng_backward);

  // NO CFA tags: this is demosaicked data.
  //     encode as normalized: BlackLevel=0, WhiteLevel=65535. the
  //     pixel data is already un-WB'd camRGB in [0, 1] range (the
  //     raw_restore_linear pipeline does matrix + un-boost + un-WB
  //     before handing off). the consumer applies WB via
  //     AsShotNeutral, reads uint16 as [0, 65535] and normalizes to
  //     [0, 1] via black/white
  const uint32_t white_norm = 65535u;
  const float black3[3] = { 0.0f, 0.0f, 0.0f };
  TIFFSetField(tif, TIFFTAG_BLACKLEVEL, 3, black3);
  TIFFSetField(tif, TIFFTAG_WHITELEVEL, 1, &white_norm);

  // AsShotNeutral = inverse of WB multipliers, normalized so max=1.
  // on re-import, darktable reads this and derives WB coeffs via
  // wb[c] = 1/AsShotNeutral[c] / wb[G-normalized]. the temperature
  // iop then applies this WB to our un-WB'd data, giving the standard
  // raw-pipeline result
  if(img->wb_coeffs[0] > 0.0f
     && img->wb_coeffs[1] > 0.0f
     && img->wb_coeffs[2] > 0.0f)
  {
    float inv[3];
    for(int i = 0; i < 3; i++) inv[i] = 1.0f / img->wb_coeffs[i];
    const float m = fmaxf(inv[0], fmaxf(inv[1], inv[2]));
    if(m > 0.0f) for(int i = 0; i < 3; i++) inv[i] /= m;
    TIFFSetField(tif, TIFFTAG_ASSHOTNEUTRAL, 3, inv);
  }
  else
  {
    const float neutral[3] = { 1.0f, 1.0f, 1.0f };
    TIFFSetField(tif, TIFFTAG_ASSHOTNEUTRAL, 3, neutral);
  }

  // ColorMatrix1 from camera's XYZ->CAM (3x3 portion)
  {
    float non_zero = 0.0f;
    for(int k = 0; k < 3; k++)
      for(int i = 0; i < 3; i++)
        non_zero += fabsf(img->adobe_XYZ_to_CAM[k][i]);
    if(non_zero > 0.0f)
    {
      float color_matrix[9];
      for(int k = 0; k < 3; k++)
        for(int i = 0; i < 3; i++)
          color_matrix[k * 3 + i] = img->adobe_XYZ_to_CAM[k][i];
      TIFFSetField(tif, TIFFTAG_COLORMATRIX1, 9, color_matrix);
    }
  }

  // linear DNG: buffer is already at visible dims (post-demosaic);
  // ACTIVEAREA covers the full buffer, no margin to crop
  const uint32_t active_area[4] = {
    0, 0, (uint32_t)height, (uint32_t)width,
  };
  const float default_scale[2] = { 1.0f, 1.0f };
  const float default_crop_origin[2] = { 0.0f, 0.0f };
  const float default_crop_size[2] = { (float)width, (float)height };
  TIFFSetField(tif, TIFFTAG_ACTIVEAREA, active_area);
  TIFFSetField(tif, TIFFTAG_DEFAULTSCALE, default_scale);
  TIFFSetField(tif, TIFFTAG_DEFAULTCROPORIGIN, default_crop_origin);
  TIFFSetField(tif, TIFFTAG_DEFAULTCROPSIZE, default_crop_size);

  // scanline write: float [0, 1] normalized camRGB -> uint16
  //     [0, 65535]. BlackLevel=0 and WhiteLevel=65535 let the
  //     re-importer recover the [0, 1] range via the standard raw
  //     normalization (val - black) / (white - black)
  const float clip_hi = 65535.0f;
  uint16_t *scan = g_malloc((size_t)width * 3 * sizeof(uint16_t));
  int res = 0;
  if(!scan)
  {
    TIFFClose(tif);
    g_unlink(filename);
    return 1;
  }
  for(int y = 0; y < height && res == 0; y++)
  {
    const float *row = rgb + (size_t)y * width * 3;
    for(int x = 0; x < width; x++)
    {
      for(int c = 0; c < 3; c++)
      {
        float adc = row[x * 3 + c] * 65535.0f;
        if(adc < 0.0f) adc = 0.0f;
        if(adc > clip_hi) adc = clip_hi;
        scan[x * 3 + c] = (uint16_t)(adc + 0.5f);
      }
    }
    if(TIFFWriteScanline(tif, scan, y, 0) < 0) res = 1;
  }
  g_free(scan);

  TIFFClose(tif);

  if(res == 0 && exif_blob && exif_len > 0)
    dt_exif_write_blob((uint8_t *)exif_blob, (uint32_t)exif_len,
                       filename, 0);

  if(res != 0)
    g_unlink(filename);

  return res;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
