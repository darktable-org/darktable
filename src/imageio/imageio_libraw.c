/*
    This file is part of darktable,
    Copyright (C) 2021-2026 darktable developers.

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

#ifdef HAVE_LIBRAW
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/exif.h"
#include "common/math.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "imageio_common.h"
#include "imageio_libraw.h"
#include "imageio_rawspeed.h" // for dt_rawspeed_crop_dcraw_filters

#include <libraw/libraw.h>

#include <inttypes.h>
#include <memory.h>
#include <stdio.h>
#include <strings.h>


typedef struct model_map
{
  const gchar *exif_make;
  const gchar *exif_model;
  const gchar *clean_make;
  const gchar *clean_model;
  const gchar *clean_alias;

} model_map_t;


const model_map_t modelMap[] = {
  {
    .exif_make = "Canon",
    .exif_model = "Canon EOS R",
    .clean_make = "Canon",
    .clean_model = "EOS R",
    .clean_alias = "EOS R"
  },
  {
    .exif_make = "Canon",
    .exif_model = "Canon EOS RP",
    .clean_make = "Canon",
    .clean_model = "EOS RP",
    .clean_alias = "EOS RP"
  },
  {
    .exif_make = "Canon",
    .exif_model = "Canon EOS R5",
    .clean_make = "Canon",
    .clean_model = "EOS R5",
    .clean_alias = "EOS R5"
  },
  {
    .exif_make = "Canon",
    .exif_model = "Canon EOS R6",
    .clean_make = "Canon",
    .clean_model = "EOS R6",
    .clean_alias = "EOS R6"
  },
  {
    .exif_make = "Canon",
    .exif_model = "Canon EOS R3",
    .clean_make = "Canon",
    .clean_model = "EOS R3",
    .clean_alias = "EOS R3"
  },
  {
    .exif_make = "Canon",
    .exif_model = "Canon EOS R7",
    .clean_make = "Canon",
    .clean_model = "EOS R7",
    .clean_alias = "EOS R7"
  },
  {
    .exif_make = "Canon",
    .exif_model = "Canon EOS R10",
    .clean_make = "Canon",
    .clean_model = "EOS R10",
    .clean_alias = "EOS R10"
  },
  {
    .exif_make = "Canon",
    .exif_model = "Canon EOS M50",
    .clean_make = "Canon",
    .clean_model = "EOS M50",
    .clean_alias = "EOS M50"
  },
  {
    .exif_make = "Canon",
    .exif_model = "Canon EOS KISS M",
    .clean_make = "Canon",
    .clean_model = "EOS M50",
    .clean_alias = "EOS KISS M"
  },
  {
    .exif_make = "Canon",
    .exif_model = "Canon EOS M50m2",
    .clean_make = "Canon",
    .clean_model = "EOS M50 Mark II",
    .clean_alias = "EOS M50 Mark II"
  },
  {
    .exif_make = "Canon",
    .exif_model = "Canon EOS KISS M2",
    .clean_make = "Canon",
    .clean_model = "EOS M50 Mark II",
    .clean_alias = "EOS KISS M2"
  },
  {
    .exif_make = "Canon",
    .exif_model = "Canon EOS M6 Mark II",
    .clean_make = "Canon",
    .clean_model = "EOS M6 Mark II",
    .clean_alias = "EOS M6 Mark II"
  },
  {
    .exif_make = "Canon",
    .exif_model = "Canon EOS M200",
    .clean_make = "Canon",
    .clean_model = "EOS M200",
    .clean_alias = "EOS M200"
  },
  {
    .exif_make = "Canon",
    .exif_model = "Canon EOS 250D",
    .clean_make = "Canon",
    .clean_model = "EOS 250D",
    .clean_alias = "EOS 250D"
  },
  {
    .exif_make = "Canon",
    .exif_model = "Canon EOS Kiss X10",
    .clean_make = "Canon",
    .clean_model = "EOS 250D",
    .clean_alias = "EOS Kiss X10"
  },
  {
    .exif_make = "Canon",
    .exif_model = "Canon EOS Rebel SL3",
    .clean_make = "Canon",
    .clean_model = "EOS 250D",
    .clean_alias = "EOS Rebel SL3"
  },
  {
    .exif_make = "Canon",
    .exif_model = "Canon EOS 200D II",
    .clean_make = "Canon",
    .clean_model = "EOS 250D",
    .clean_alias = "EOS 200D Mark II"
  },
  {
    .exif_make = "Canon",
    .exif_model = "Canon EOS 850D",
    .clean_make = "Canon",
    .clean_model = "EOS 850D",
    .clean_alias = "EOS 850D"
  },
  {
    .exif_make = "Canon",
    .exif_model = "Canon EOS Kiss X10i",
    .clean_make = "Canon",
    .clean_model = "EOS 850D",
    .clean_alias = "EOS Kiss X10i"
  },
  {
    .exif_make = "Canon",
    .exif_model = "Canon EOS Rebel T8i",
    .clean_make = "Canon",
    .clean_model = "EOS 850D",
    .clean_alias = "EOS Rebel T8i"
  },
  {
    .exif_make = "Canon",
    .exif_model = "Canon EOS 90D",
    .clean_make = "Canon",
    .clean_model = "EOS 90D",
    .clean_alias = "EOS 90D"
  },
  {
    .exif_make = "Canon",
    .exif_model = "Canon EOS-1D X Mark III",
    .clean_make = "Canon",
    .clean_model = "EOS-1D X Mark III",
    .clean_alias = "EOS-1D X Mark III"
  },
  {
    .exif_make = "Canon",
    .exif_model = "Canon PowerShot G7 X Mark III",
    .clean_make = "Canon",
    .clean_model = "PowerShot G7 X Mark III",
    .clean_alias = "PowerShot G7 X Mark III"
  },
  {
    .exif_make = "Canon",
    .exif_model = "Canon PowerShot G5 X Mark II",
    .clean_make = "Canon",
    .clean_model = "PowerShot G5 X Mark II",
    .clean_alias = "PowerShot G5 X Mark II"
  },
  {
    .exif_make = "Canon",
    .exif_model = "Canon PowerShot SX70 HS",
    .clean_make = "Canon",
    .clean_model = "PowerShot SX70 HS",
    .clean_alias = "PowerShot SX70 HS"
  },
#if LIBRAW_COMPILE_CHECK_VERSION_NOTLESS(0, 22)
  {
    .exif_make = "Canon",
    .exif_model = "Canon EOS Ra",
    .clean_make = "Canon",
    .clean_model = "EOS Ra",
    .clean_alias = "EOS Ra"
  },
  {
    .exif_make = "Canon",
    .exif_model = "Canon EOS R5 C",
    .clean_make = "Canon",
    .clean_model = "EOS R5 C",
    .clean_alias = "EOS R5 C"
  },
  {
    .exif_make = "Canon",
    .exif_model = "Canon EOS R6m2",
    .clean_make = "Canon",
    .clean_model = "EOS R6 Mark II",
    .clean_alias = "EOS R6 Mark II"
  },
  {
    .exif_make = "Canon",
    .exif_model = "Canon EOS R8",
    .clean_make = "Canon",
    .clean_model = "EOS R8",
    .clean_alias = "EOS R8"
  },
  {
    .exif_make = "Canon",
    .exif_model = "Canon EOS R50",
    .clean_make = "Canon",
    .clean_model = "EOS R50",
    .clean_alias = "EOS R50"
  },
  {
    .exif_make = "Canon",
    .exif_model = "Canon EOS R100",
    .clean_make = "Canon",
    .clean_model = "EOS R100",
    .clean_alias = "EOS R100"
  },
  {
    .exif_make = "Canon",
    .exif_model = "Canon EOS R5m2",
    .clean_make = "Canon",
    .clean_model = "EOS R5 Mark II",
    .clean_alias = "EOS R5 Mark II"
  },
  {
    .exif_make = "Canon",
    .exif_model = "Canon EOS R1",
    .clean_make = "Canon",
    .clean_model = "EOS R1",
    .clean_alias = "EOS R1"
  },
#endif
};


static gboolean is_in_glist(GList *list, const gchar *exif_model)
{
  for(GList *l = list; l; l = l->next)
  {
    if(!g_strcmp0((char *)l->data, exif_model))
      return TRUE;
  }
  return FALSE;
}

GList *warning_missing_support_seen = NULL;

/* LibRaw is expected to read only new Canon CR3 files */

static gboolean _supported_image(const gchar *filename)
{
  // At the moment of writing this code CR3 files are not supported by RawSpeed,
  // so they are always processed by LibRaw.
  gchar *extensions_whitelist;
  const gchar *always_by_libraw = "cr3 x3f";

  gchar *ext = g_strrstr(filename, ".");
  if(!ext)
    return FALSE;
  ext++;

  if(dt_conf_key_not_empty("libraw_extensions"))
    extensions_whitelist = g_strjoin(" ", always_by_libraw,
                                     dt_conf_get_string_const("libraw_extensions"), NULL);
  else
    extensions_whitelist = g_strdup(always_by_libraw);

  dt_print(DT_DEBUG_IMAGEIO,
           "[libraw_open] extensions whitelist: '%s'", extensions_whitelist);

  gchar *ext_lowercased = g_ascii_strdown(ext, -1);
  if(g_strstr_len(extensions_whitelist, -1, ext_lowercased))
  {
    g_free(ext_lowercased);
    g_free(extensions_whitelist);
    return TRUE;
  }
  g_free(ext_lowercased);
  g_free(extensions_whitelist);
  return FALSE;
}

// Raw formats (compression modes) RawSpeed can't decode but LibRaw can.
// When RawSpeed fails on a file, LibRaw opens it and the file is loaded
// via LibRaw if LibRaw picked one of these decoders for it. LibRaw picks
// its decoder while parsing the file, so the decoder name reliably tells
// what the file is and whether the LibRaw in use can decode it.

#if LIBRAW_COMPILE_CHECK_VERSION_NOTLESS(0, 23)
// Nikon HE/HE*: only the cameras listed by LibRaw (snapshot 202611).
// Values are the unmodified Exif.Image.Model strings.
static const char *const _nikon_he_models[] =
{
  "NIKON Z 8",
  "NIKON Z 9",
  "NIKON Z f",
  "NIKON Z6_3",
  NULL
};
#endif

typedef struct dt_libraw_fallback_t
{
  const char *decoder;        // as returned by libraw_unpack_function_name()
  int min_version;            // first LibRaw version decoding this format
  gboolean decoder_shared;    // LibRaw uses this decoder for formats RawSpeed
                              // supports too: only fall back for cameras
                              // RawSpeed knows (failing on the mode, not the
                              // camera), don't take over new cameras
  const char *const *models;  // NULL-terminated Exif models, NULL = all
  const char *description;
} dt_libraw_fallback_t;

static const dt_libraw_fallback_t _fallbacks[] =
{
#if LIBRAW_COMPILE_CHECK_VERSION_NOTLESS(0, 23)
  { "nikon_he_load_raw()",
    LIBRAW_MAKE_VERSION(0, 23, 0), FALSE, _nikon_he_models,
    "Nikon HE/HE*" },
  { "sony_arw6_load_raw()",
    LIBRAW_MAKE_VERSION(0, 23, 0), FALSE, NULL,
    "Sony ARW 6 compressed / compressed HQ" },
#endif
#if LIBRAW_COMPILE_CHECK_VERSION_NOTLESS(0, 21)
  { "fuji_compressed_load_raw()",
    LIBRAW_MAKE_VERSION(0, 21, 0), TRUE, NULL,
    "Fujifilm compressed (lossy)" },
  // DNGs RawSpeed fails on; regular DNGs stay with RawSpeed:
  // lossless JPEG with predictors RawSpeed doesn't support (e.g. 7)
  { "lossless_dng_load_raw()",
    LIBRAW_MAKE_VERSION(0, 21, 0), TRUE, NULL,
    "DNG lossless JPEG" },
  // uncompressed with inconsistent strip sizes (e.g. DJI Mavic 3)
  { "packed_dng_load_raw()",
    LIBRAW_MAKE_VERSION(0, 21, 0), TRUE, NULL,
    "DNG uncompressed" },
#endif
  { NULL, 0, FALSE, NULL, NULL }
};

static gboolean _in_list(const char *const *list, const char *value)
{
  for(int i = 0; list[i]; i++)
    if(!g_strcmp0(value, list[i]))
      return TRUE;
  return FALSE;
}

static const dt_libraw_fallback_t *_find_fallback(libraw_data_t *raw,
                                                  const dt_image_t *img,
                                                  const dt_imageio_retval_t rawspeed_ret)
{
  const char *decoder = libraw_unpack_function_name(raw);
  if(!decoder)
    return NULL;

  // the loader handles CFA data (Bayer, X-Trans) only, not e.g. LinearRaw
  // DNGs: leave those to the regular loader chain
  if(!raw->idata.filters)
    return NULL;

  for(const dt_libraw_fallback_t *f = _fallbacks; f->decoder; f++)
  {
    if(strcmp(decoder, f->decoder))
      continue;
    // the LibRaw found at runtime may be older than its headers
    if(libraw_versionNumber() < f->min_version)
    {
      dt_print(DT_DEBUG_IMAGEIO,
               "[libraw_open] LibRaw %s can't decode %s, skipping `%s'",
               libraw_version(), f->description, img->filename);
      return NULL;
    }
    if(f->decoder_shared && rawspeed_ret == DT_IMAGEIO_UNSUPPORTED_CAMERA)
      return NULL;
    if(f->models && !_in_list(f->models, img->exif_model))
      return NULL;
    return f;
  }
  return NULL;
}

// Issue a warning, but only if we haven't done so
// for that particular model in this darktable session
static void _check_libraw_missing_support(const struct dt_image_t *img)
{
  char lr_mk[64], lr_md[64], lr_al[64];
  if(!is_in_glist(warning_missing_support_seen, img->exif_model) &&
     !dt_libraw_lookup_makermodel(img->exif_maker, img->exif_model,
                                  lr_mk, sizeof(lr_mk),
                                  lr_md, sizeof(lr_md),
                                  lr_al, sizeof(lr_al)))
  {
    gchar *model_copy = g_strdup(img->exif_model);
    warning_missing_support_seen = g_list_append(warning_missing_support_seen, model_copy);

    const char *T1 = _("<span foreground='red'><b>WARNING</b></span>:"
                       " camera is not fully supported!");
    char *T2 = g_strdup_printf(_("colors for `%s' could be misrepresented,\n"
                                 "and edits might not be compatible with future versions."),
                               img->exif_model);

    char *msg = g_strconcat("<big>", T1, "\n\n", T2, "</big>", NULL);
    g_free(T2);

    dt_control_log(msg, (char *)NULL);
    g_free(msg);
  }
}

// Return the normalized maker, model and alias if the camera is found
gboolean dt_libraw_lookup_makermodel(const char *maker, const char *model,
                                     char *mk, int mk_len, char *md, int md_len,
                                     char *al, int al_len)
{
  for(int i = 0; i < sizeof(modelMap) / sizeof(modelMap[0]); ++i)
  {
    if(!g_strcmp0(maker, modelMap[i].exif_make)
       && !g_strcmp0(model, modelMap[i].exif_model))
    {
      g_strlcpy(mk, modelMap[i].clean_make, mk_len);
      g_strlcpy(md, modelMap[i].clean_model, md_len);
      g_strlcpy(al, modelMap[i].clean_alias, al_len);
      return TRUE;
    }
  }
  return FALSE;
}

// Black levels for darktable: raw_black_level_separate[] is per 2x2
// position, relative to the top left of the full raw buffer. LibRaw has
// a common black, per-color cblack[0..3] and, for some formats, a
// cblack[4] x cblack[5] pattern in cblack[6+] (e.g. Fujifilm stores its
// black levels only there, with black and cblack[0..3] being 0). LibRaw
// folds these together only in adjust_bl() during its own processing,
// which we don't run, so do it here. LibRaw's filters and black pattern
// are relative to the visible area (after the margins).
static unsigned _mod(const int a, const unsigned m)
{
  const int r = a % (int)m;
  return r < 0 ? (unsigned)(r + (int)m) : (unsigned)r;
}

static void _set_black_levels(dt_image_t *img, const libraw_data_t *raw)
{
  const unsigned *cblack = raw->rawdata.color.cblack;
  const unsigned black = raw->rawdata.color.black;
  const unsigned filters = raw->rawdata.iparams.filters;
  const unsigned prows = cblack[4];
  const unsigned pcols = cblack[5];
  const gboolean pattern = prows && pcols && prows * pcols <= LIBRAW_CBLACK_SIZE - 6;

  if(filters == 9u || filters == 0)
  {
    // X-Trans (or no CFA): no meaningful 2x2 positions, use the mean
    double level = black;
    for(int c = 0; c < 3; c++)
      level += cblack[c] / 3.0;
    if(pattern)
      for(unsigned k = 0; k < prows * pcols; k++)
        level += (double)cblack[6 + k] / (prows * pcols);
    for(int i = 0; i < 4; i++)
      img->raw_black_level_separate[i] = (uint16_t)(level + 0.5);
  }
  else
  {
    const int top = raw->rawdata.sizes.top_margin;
    const int left = raw->rawdata.sizes.left_margin;
    for(int i = 0; i < 4; i++)
    {
      // 2x2 position i in visible area coordinates
      const int row = (i >> 1) - top;
      const int col = (i & 1) - left;
      const unsigned color =
        filters >> ((((_mod(row, 8) << 1) & 14) | _mod(col, 2)) << 1) & 3;
      unsigned level = black + cblack[color];
      if(pattern)
        level += cblack[6 + _mod(row, prows) * pcols + _mod(col, pcols)];
      img->raw_black_level_separate[i] = (uint16_t)level;
    }
  }

  unsigned sum = 0;
  for(int i = 0; i < 4; i++)
    sum += img->raw_black_level_separate[i];
  img->raw_black_level = (uint16_t)((sum + 2) / 4);
}

static gboolean _matrix_is_empty(const float m[4][3])
{
  for(int k = 0; k < 4; k++)
    for(int i = 0; i < 3; i++)
      if(m[k][i] != 0.0f)
        return FALSE;
  return TRUE;
}

static void _apply_vendor_crop(dt_image_t *img, const libraw_data_t *raw)
{
  const libraw_raw_inset_crop_t *crop = &raw->rawdata.sizes.raw_inset_crops[0];
  const unsigned raw_width = raw->rawdata.sizes.raw_width;
  const unsigned raw_height = raw->rawdata.sizes.raw_height;

  // LibRaw marks a missing crop with 0xffff offsets / zero size
  if(crop->cleft == 0xffff || crop->ctop == 0xffff
     || crop->cwidth == 0 || crop->cheight == 0
     || (unsigned)crop->cleft + crop->cwidth > raw_width
     || (unsigned)crop->ctop + crop->cheight > raw_height)
    return;

  img->crop_x = crop->cleft;
  img->crop_y = crop->ctop;
  img->crop_right = raw_width - crop->cwidth - crop->cleft;
  img->crop_bottom = raw_height - crop->cheight - crop->ctop;
  img->p_width = crop->cwidth;
  img->p_height = crop->cheight;
}

static dt_imageio_retval_t _open_libraw(dt_image_t *img,
                                        const char *filename,
                                        dt_mipmap_buffer_t *mbuf,
                                        const gboolean fallback,
                                        const dt_imageio_retval_t rawspeed_ret)
{
  int err = DT_IMAGEIO_LOAD_FAILED;
  int libraw_err = LIBRAW_SUCCESS;

  if(!fallback && !_supported_image(filename))
    return DT_IMAGEIO_LOAD_FAILED;
  if(!img->exif_inited)
    (void)dt_exif_read(img, filename);

  libraw_data_t *raw = libraw_init(0);
  if(!raw)
    return fallback ? DT_IMAGEIO_UNSUPPORTED_FORMAT : DT_IMAGEIO_LOAD_FAILED;

#if defined(_WIN32) && (defined(UNICODE) || defined(_UNICODE))
  wchar_t *wfilename = g_utf8_to_utf16(filename, -1, NULL, NULL, NULL);
  libraw_err = libraw_open_wfile(raw, wfilename);
  g_free(wfilename);
#else
  libraw_err = libraw_open_file(raw, filename);
#endif

  if(fallback)
  {
    const dt_libraw_fallback_t *f = libraw_err == LIBRAW_SUCCESS
      ? _find_fallback(raw, img, rawspeed_ret)
      : NULL;
    if(!f)
    {
      // not a format we take over from RawSpeed: leave it to the
      // regular loader chain without any error output
      libraw_close(raw);
      return DT_IMAGEIO_UNSUPPORTED_FORMAT;
    }
    dt_print(DT_DEBUG_IMAGEIO,
             "[libraw_open] `%s': %s, loading via LibRaw", img->filename, f->description);
  }

  if(libraw_err != LIBRAW_SUCCESS)
    goto error;

  libraw_err = libraw_unpack(raw);
  if(libraw_err != LIBRAW_SUCCESS)
    goto error;

  // Bad method to detect if camera is fully supported by LibRaw,
  // but seems to be the best available. LibRaw crx decoder can actually
  // decode the raw data, but internal metadata like wb_coeffs, crops etc.
  // are not populated into libraw structure, or image is not of CFA type.
  if(raw->rawdata.color.cam_mul[0] == 0.0f
     || dt_isnan(raw->rawdata.color.cam_mul[0])
     || !raw->rawdata.raw_image)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[libraw_open] detected unsupported image `%s'", img->filename);
    err = DT_IMAGEIO_UNSUPPORTED_FEATURE;
    goto error;
  }

  // If the support detection method above failed (e.g. LibRaw matches color
  // matrix on a model substring), let's also check our internal LibRaw lookup
  // table for Canon CR3s only.
  gchar *ext = g_strrstr(filename, ".");
  if(!ext)
  {
    err = DT_IMAGEIO_LOAD_FAILED;
    goto error;
  }
  ext++;
  if(!g_ascii_strncasecmp("cr3", ext, 3))
    _check_libraw_missing_support(img);

  // Copy white level (all linear_max[] equal single
  // SpecularWhiteLevel for CR3, we can skip min or mean)
  img->raw_white_point = raw->rawdata.color.linear_max[0]
    ? raw->rawdata.color.linear_max[0]
    :raw->rawdata.color.maximum;

  // Without a white level the data can't be scaled; LibRaw leaves it at 0
  // for cameras it decodes but has no level data for (e.g. Nikon Z5 II HE
  // in LibRaw 0.23). Refuse such files instead of showing a broken image.
  if(img->raw_white_point == 0)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[libraw_open] no white level for `%s', unsupported", img->filename);
    err = DT_IMAGEIO_UNSUPPORTED_FEATURE;
    goto error;
  }

  _set_black_levels(img, raw);

  // AsShot WB coeffs
  for(size_t c = 0; c < 4; ++c)
    img->wb_coeffs[c] = raw->rawdata.color.cam_mul[c];

  // Grab the Adobe coeffs
  for(int k = 0; k < 4; k++)
    for(int i = 0; i < 3; i++)
      img->adobe_XYZ_to_CAM[k][i] = raw->rawdata.color.cam_xyz[k][i];

  // LibRaw leaves cam_xyz empty for DNGs and keeps the DNG ColorMatrix
  // tags in dng_color[] instead. Without a matrix darktable has no camera
  // reference (D65) white balance. Use the D65 matrix like the RawSpeed
  // loader does (CalibrationIlluminant 21, ColorMatrix1 or 2).
  if(_matrix_is_empty(img->adobe_XYZ_to_CAM))
  {
    for(int n = 0; n < 2; n++)
    {
      const libraw_dng_color_t *dng = &raw->rawdata.color.dng_color[n];
      if(dng->illuminant != 21) // D65
        continue;
      float candidate[4][3] = { { 0.0f } };
      for(int k = 0; k < 4; k++)
        for(int i = 0; i < 3; i++)
          candidate[k][i] = dng->colormatrix[k][i];
      if(_matrix_is_empty(candidate))
        continue;
      for(int k = 0; k < 4; k++)
        for(int i = 0; i < 3; i++)
          img->adobe_XYZ_to_CAM[k][i] = candidate[k][i];
      break;
    }
  }

  // Raw dimensions. This is the full sensor range.
  img->width = raw->rawdata.sizes.raw_width;
  img->height = raw->rawdata.sizes.raw_height;

  // Apply crop parameters
  img->crop_x = raw->rawdata.sizes.left_margin;
  img->crop_y = raw->rawdata.sizes.top_margin;
  img->crop_right = raw->rawdata.sizes.raw_width - raw->rawdata.sizes.width - raw->rawdata.sizes.left_margin;
  img->crop_bottom = raw->rawdata.sizes.raw_height - raw->rawdata.sizes.height - raw->rawdata.sizes.top_margin;

  img->p_width = img->width - img->crop_x - img->crop_right;
  img->p_height = img->height - img->crop_y - img->crop_bottom;

  // Fujifilm and DNG: use the crop stored in the file, like the RawSpeed
  // loader does, so files get the same crop whichever loader opens them.
  // LibRaw provides it in raw_inset_crops[0]: for Fujifilm from
  // RawImageCropTopLeft / RawImageCroppedSize, for DNG from
  // DefaultCropOrigin / DefaultCropSize within the ActiveArea. LibRaw's
  // own visible area for these files can be slightly larger. LibRaw moves
  // an odd crop origin to the next even row/column (shrinking the size
  // accordingly), so the crop can be one row/column smaller than RawSpeed's.
  if(raw->rawdata.iparams.maker_index == LIBRAW_CAMERAMAKER_Fujifilm
     || raw->rawdata.iparams.dng_version)
    _apply_vendor_crop(img, raw);
  // We can reuse the libraw filters property, it's already well-handled in dt.
  // It contains (for CR3) the Bayer pattern, but we have to undo some LibRaw logic.
  if(raw->rawdata.iparams.colors == 3)
  {
    // Workaround for 3 color filters (ok for CR3) from LibRaw::pre_interpolate()
    img->buf_dsc.filters = raw->rawdata.iparams.filters & ~((raw->rawdata.iparams.filters & 0x55555555U) << 1);
  }
  else
  {
    // In general we should run through entire post-processing to get corrected filters.
    // This incurs a significant performance penalty.
    libraw_err = libraw_dcraw_process(raw);
    if(libraw_err != LIBRAW_SUCCESS)
      goto error;

    img->buf_dsc.filters = raw->idata.filters;
  }

  // LibRaw's Bayer filters are relative to its visible area, darktable
  // expects them relative to the top left of the full raw buffer (like
  // the RawSpeed loader), independent of the crop. No-op for even margins.
  img->buf_dsc.filters = dt_rawspeed_crop_dcraw_filters(img->buf_dsc.filters,
                                                        raw->rawdata.sizes.left_margin,
                                                        raw->rawdata.sizes.top_margin);

  // X-Trans: darktable expects the 6x6 CFA aligned to the top left of
  // the full (uncropped) raw buffer, like the RawSpeed loader provides
  // it. That is LibRaw's xtrans_abs (xtrans is relative to the visible
  // area). Without this the pattern stays all zero = all red pixels.
  if(img->buf_dsc.filters == 9u)
  {
    for(int j = 0; j < 6; ++j)
      for(int i = 0; i < 6; ++i)
        img->buf_dsc.xtrans[j][i] = (uint8_t)raw->rawdata.iparams.xtrans_abs[j][i];
  }

  dt_exif_img_check_additional_tags(img, filename);

  // Single channel CFA data (Bayer or X-Trans)
  img->buf_dsc.channels = 1;

  img->buf_dsc.datatype = TYPE_UINT16;
  img->buf_dsc.cst = IOP_CS_RAW;

  // Taken from the old dt libraw integration
  if(FILTERS_ARE_4BAYER(img->buf_dsc.filters))
  {
    img->flags |= DT_IMAGE_4BAYER;
  }
  else
  {
    img->flags &= ~DT_IMAGE_4BAYER;
  }

  if(img->buf_dsc.filters)
  {
    img->flags &= ~DT_IMAGE_LDR;
    img->flags &= ~DT_IMAGE_HDR;
    img->flags |= DT_IMAGE_RAW;
  }
  else
  {
    // LDR DNG. It exists :(
    img->flags &= ~DT_IMAGE_RAW;
    img->flags &= ~DT_IMAGE_HDR;
    img->flags |= DT_IMAGE_LDR;
  }

  // Metadata only, like the RawSpeed loader
  if(!mbuf)
  {
    img->loader = LOADER_LIBRAW;
    err = DT_IMAGEIO_OK;
    goto error;
  }

  // Allocate and copy image from libraw buffer to dt
  void *buf = dt_mipmap_cache_alloc(mbuf, img);
  if(!buf)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[libraw_open] could not alloc full buffer for image `%s'",
             img->filename);
    err = DT_IMAGEIO_CACHE_FULL;
    goto error;
  }

  // Use faster memcpy if buffer sizes are equal
  const size_t bufSize_mipmap = (size_t)img->width * img->height * sizeof(uint16_t);
  const size_t bufSize_libraw = (size_t)raw->rawdata.sizes.raw_pitch * raw->rawdata.sizes.raw_height;
  if(bufSize_mipmap == bufSize_libraw)
  {
    memcpy(buf, raw->rawdata.raw_image, bufSize_mipmap);
  }
  else
  {
    dt_imageio_flip_buffers((char *)buf, (char *)raw->rawdata.raw_image, sizeof(uint16_t),
                            raw->rawdata.sizes.raw_width, raw->rawdata.sizes.raw_height,
                            raw->rawdata.sizes.raw_width, raw->rawdata.sizes.raw_height,
                            raw->rawdata.sizes.raw_pitch, ORIENTATION_NONE);
  }

  img->loader = LOADER_LIBRAW;

  err = DT_IMAGEIO_OK;

error:
  if(libraw_err != LIBRAW_SUCCESS)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[libraw_open] `%s': %s",
             img->filename, libraw_strerror(libraw_err));
    switch(libraw_err)
    {
    case LIBRAW_FILE_UNSUPPORTED:
      err = DT_IMAGEIO_UNSUPPORTED_FORMAT;
      break;
    case LIBRAW_NOT_IMPLEMENTED:
      err = DT_IMAGEIO_UNSUPPORTED_FEATURE;
      break;
    case LIBRAW_DATA_ERROR:
      err = DT_IMAGEIO_FILE_CORRUPTED;
      break;
    case LIBRAW_IO_ERROR:
      err = DT_IMAGEIO_IOERROR;
      break;
    default:
      err = DT_IMAGEIO_LOAD_FAILED;
      break;
    }
  }
  libraw_close(raw);
  return err;
}

dt_imageio_retval_t dt_imageio_open_libraw(dt_image_t *img,
                                           const char *filename,
                                           dt_mipmap_buffer_t *mbuf)
{
  return _open_libraw(img, filename, mbuf, FALSE, DT_IMAGEIO_OK);
}

dt_imageio_retval_t dt_imageio_open_libraw_fallback(dt_image_t *img,
                                                    const char *filename,
                                                    dt_mipmap_buffer_t *mbuf,
                                                    const dt_imageio_retval_t rawspeed_ret)
{
  // files routed to LibRaw by extension are loaded by the regular path
  if(!img || _supported_image(filename))
    return DT_IMAGEIO_UNSUPPORTED_FORMAT;
  return _open_libraw(img, filename, mbuf, TRUE, rawspeed_ret);
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
