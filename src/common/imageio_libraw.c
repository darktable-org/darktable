/*
    This file is part of darktable,
    Copyright (C) 2021 darktable developers.

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
#include "common/darktable.h"
#include "imageio.h"
#include "imageio_gm.h"
#include "develop/develop.h"
#include "common/exif.h"
#include "common/colorspaces.h"
#include "control/conf.h"

#include <memory.h>
#include <stdio.h>
#include <inttypes.h>
#include <strings.h>
#include <assert.h>

#include <libraw/libraw.h>


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
    .exif_model = "Canon EOS Rebel SL3",
    .clean_make = "Canon",
    .clean_model = "EOS 250D",
    .clean_alias = "EOS Rebel SL3"
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
  }
};



/* LibRAW is expected to read only new canon CR3 files */

static gboolean _supported_image(const gchar *filename)
{
  const char *extensions_whitelist[] = { "cr3", NULL };
  char *ext = g_strrstr(filename, ".");
  if(!ext) return FALSE;
  ext++;
  for(const char **i = extensions_whitelist; *i != NULL; i++)
    if(!g_ascii_strncasecmp(ext, *i, strlen(*i)))
    {
      return TRUE;
    }
  return FALSE;
}

gboolean dt_libraw_lookup_makermodel(const char *maker, const char *model,
                                     char *mk, int mk_len, char *md, int md_len,
                                     char *al, int al_len)
{
  for(int i = 0; i < sizeof(modelMap) / sizeof(modelMap[0]); ++i)
  {
    if(!g_strcmp0(maker, modelMap[i].exif_make) && !g_strcmp0(model, modelMap[i].exif_model))
    {
      //printf("input model: %s, exif model: %s\n", model, modelMap[i].exif_model);
      g_strlcpy(mk, modelMap[i].clean_make, mk_len);
      g_strlcpy(md, modelMap[i].clean_model, md_len);
      g_strlcpy(al, modelMap[i].clean_alias, al_len);
      return TRUE;
    }
  }
  return FALSE;
}


dt_imageio_retval_t dt_imageio_open_libraw(dt_image_t *img, const char *filename, dt_mipmap_buffer_t *mbuf)
{
  int err = DT_IMAGEIO_FILE_CORRUPTED;
  int libraw_err = LIBRAW_SUCCESS;
  if(!_supported_image(filename)) return DT_IMAGEIO_FILE_CORRUPTED;
  if(!img->exif_inited) (void)dt_exif_read(img, filename);

  libraw_data_t *raw = libraw_init(0);
  if(!raw) return DT_IMAGEIO_FILE_CORRUPTED;

#if defined(_WIN32) && (defined(UNICODE) || defined(_UNICODE))
  wchar_t *wfilename = g_utf8_to_utf16(filename, -1, NULL, NULL, NULL);
  libraw_err = libraw_open_wfile(raw, wfilename);
  g_free(wfilename);
#else
  libraw_err = libraw_open_file(raw, filename);
#endif
  if(libraw_err != LIBRAW_SUCCESS) goto error;

  libraw_err = libraw_unpack(raw);
  if(libraw_err != LIBRAW_SUCCESS) goto error;

  // Bad method to detect if camera is fully supported by LibRaw,
  // but seems to be the best available. LibRaw crx decoder can actually
  // decode the raw data, but internal metadata like wb_coeffs, crops etc.
  // are not populated into libraw structure, or image is not of CFA type.
  if(raw->rawdata.color.cam_mul[0] == 0.0f || isnan(raw->rawdata.color.cam_mul[0]) || !raw->rawdata.raw_image)
  {
    fprintf(stderr, "[libraw_open] detected unsupported image `%s'\n", img->filename);
    goto error;
  }

  // Copy white level (all linear_max[] equal single SpecularWhiteLevel for CR3, we can skip min or mean)
  img->raw_white_point = raw->rawdata.color.linear_max[0] ? raw->rawdata.color.linear_max[0] :raw->rawdata.color.maximum;

  // Copy black level
  for(size_t c = 0; c < 4; ++c)
    img->raw_black_level_separate[c] = raw->rawdata.color.black + raw->rawdata.color.cblack[c];

  // AsShot WB coeffs
  for(size_t c = 0; c < 4; ++c)
    img->wb_coeffs[c] = raw->rawdata.color.cam_mul[c];

  // Grab the adobe coeff
  for(int k = 0; k < 4; k++)
    for(int i = 0; i < 3; i++)
      img->adobe_XYZ_to_CAM[k][i] = raw->rawdata.color.cam_xyz[k][i];

  // Raw dimensions. This is the full sensor range.
  img->width = raw->rawdata.sizes.raw_width;
  img->height = raw->rawdata.sizes.raw_height;

  // Apply crop parameters
  libraw_raw_inset_crop_t *ric;
#if LIBRAW_COMPILE_CHECK_VERSION_NOTLESS(0, 21)
  ric = &raw->rawdata.sizes.raw_inset_crops[0];
#else
  ric = &raw->rawdata.sizes.raw_inset_crop;
#endif
  img->crop_x = ric->cleft;
  img->crop_y = ric->ctop;
  img->crop_width = raw->rawdata.sizes.raw_width - ric->cwidth - ric->cleft;
  img->crop_height = raw->rawdata.sizes.raw_height - ric->cheight - ric->ctop;

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
    if(libraw_err != LIBRAW_SUCCESS) goto error;

    img->buf_dsc.filters = raw->idata.filters;
  }

  // For CR3, we only have Bayer data and a single channel
  img->buf_dsc.channels = 1;

  img->buf_dsc.datatype = TYPE_UINT16;
  img->buf_dsc.cst = IOP_CS_RAW;

  // Allocate and copy image from libraw buffer to dt
  void *buf = dt_mipmap_cache_alloc(mbuf, img);
  if(!buf)
  {
    fprintf(stderr, "[libraw_open] could not alloc full buffer for image `%s'\n", img->filename);
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

  // Checks not really required for CR3 support, but it's taken from the old dt libraw integration.
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
    // ldr dng. it exists :(
    img->flags &= ~DT_IMAGE_RAW;
    img->flags &= ~DT_IMAGE_HDR;
    img->flags |= DT_IMAGE_LDR;
  }

  img->loader = LOADER_LIBRAW;

  err = DT_IMAGEIO_OK;

error:
  if(libraw_err != LIBRAW_SUCCESS)
    fprintf(stderr, "[libraw_open] `%s': %s\n", img->filename, libraw_strerror(libraw_err));
  libraw_close(raw);
  return err;
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

