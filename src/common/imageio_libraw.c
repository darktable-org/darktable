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


/* LibRAW is expected to read only new canon CR3 files */

static gboolean _supported_image(const gchar *filename)
{
  const char *extensions_whitelist[] = { "cr3", NULL };
  gboolean supported = FALSE;
  char *ext = g_strrstr(filename, ".");
  if(!ext) return FALSE;
  ext++;
  for(const char **i = extensions_whitelist; *i != NULL; i++)
    if(!g_ascii_strncasecmp(ext, *i, strlen(*i)))
    {
      supported = TRUE;
      break;
    }
  return supported;
}



gboolean dt_libraw_lookup_makermodel(const char *maker, const char *model,
                                     char *mk, int mk_len, char *md, int md_len,
                                     char *al, int al_len)
{
  gboolean got_it_done = FALSE;

  if(g_str_equal(maker, "Canon"))
  {
    g_strlcpy(mk, "Canon", mk_len);

    if(g_str_equal(model, "Canon EOS RP"))
    {
      g_strlcpy(md, "EOS RP", md_len);
      g_strlcpy(al, "EOS RP", al_len);
    }
    if(g_str_equal(model, "Canon EOS R"))
    {
      g_strlcpy(md, "EOS R", md_len);
      g_strlcpy(al, "EOS R", al_len);
    }
    got_it_done = TRUE;
  }

  return got_it_done;
}


dt_imageio_retval_t dt_imageio_open_libraw(dt_image_t *img, const char *filename, dt_mipmap_buffer_t *mbuf)
{
  int err;
  int libraw_err = 0;
  if(!_supported_image(filename)) return DT_IMAGEIO_FILE_CORRUPTED;
  if(!img->exif_inited) (void)dt_exif_read(img, filename);

  libraw_data_t *raw = libraw_init(0);
  if(!raw) return DT_IMAGEIO_FILE_CORRUPTED;

  libraw_err = libraw_open_file(raw, filename);
  if(libraw_err != LIBRAW_SUCCESS) goto error;

  libraw_err = libraw_unpack(raw);
  if(libraw_err != LIBRAW_SUCCESS) goto error;

  // Bad method to detect if camera is fully supported by libraw.
  // But seems to be the best available. libraw crx decoder can actually
  // decode the raw data, but internal metadata like wb_coeffs, crops etc.
  // are not populated into libraw structure.
  if(raw->color.cam_mul[0] == 0.0 || isnan(raw->color.cam_mul[0]))
  {
    libraw_close(raw);
    return DT_IMAGEIO_FILE_CORRUPTED;
  }

  libraw_err = libraw_dcraw_process(raw);
  if(libraw_err != LIBRAW_SUCCESS) goto error;

  // Copy white level
  img->raw_white_point = raw->color.maximum;

  // Copy black level, specific for Canon as the regular black level
  // info in libraw is set to 0. We need to look into makernotes!
  img->raw_black_level_separate[0] = raw->makernotes.canon.ChannelBlackLevel[0];
  img->raw_black_level_separate[1] = raw->makernotes.canon.ChannelBlackLevel[1];
  img->raw_black_level_separate[2] = raw->makernotes.canon.ChannelBlackLevel[2];
  img->raw_black_level_separate[3] = raw->makernotes.canon.ChannelBlackLevel[3];

  // AsShot WB coeffs, caution: different ordering!
  img->wb_coeffs[0] = raw->color.cam_mul[2];
  img->wb_coeffs[1] = raw->color.cam_mul[1];
  img->wb_coeffs[2] = raw->color.cam_mul[0];
  img->wb_coeffs[3] = raw->color.cam_mul[3];

  // Raw dimensions. This is the full sensor range.
  img->width = raw->sizes.raw_width;
  img->height = raw->sizes.raw_height;

  // Apply crop parameters
  img->crop_x = raw->sizes.raw_inset_crop.cleft;
  img->crop_y = raw->sizes.raw_inset_crop.ctop;
  img->crop_width = raw->sizes.raw_width - raw->sizes.raw_inset_crop.cwidth - raw->sizes.raw_inset_crop.cleft;
  img->crop_height = raw->sizes.raw_height - raw->sizes.raw_inset_crop.cheight - raw->sizes.raw_inset_crop.ctop;

  // We can reuse the libraw filters property, it's already well-handled in dt.
  // It contains the BAYER filter pattern.
  img->buf_dsc.filters = raw->idata.filters;

  // For CR3, we only have BAYER data and a single channel
  img->buf_dsc.channels = 1;

  img->buf_dsc.datatype = TYPE_UINT16;
  img->buf_dsc.cst = iop_cs_RAW;

  // Allocate and copy image from libraw buffer to dt
  void *buf = dt_mipmap_cache_alloc(mbuf, img);
  if(!buf)
  {
    err = DT_IMAGEIO_CACHE_FULL;
    goto error;
  }
  dt_imageio_flip_buffers((char *)buf, (char *)raw->rawdata.raw_image, sizeof(uint16_t), raw->sizes.raw_width,
                          raw->sizes.raw_height, raw->sizes.raw_width, raw->sizes.raw_height, raw->sizes.raw_pitch, ORIENTATION_NONE);


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
  libraw_close(raw);

  return DT_IMAGEIO_OK;

error:
  fprintf(stderr, "libraw error: %s\n", libraw_strerror(libraw_err));
  libraw_close(raw);
  return err;
}
#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
