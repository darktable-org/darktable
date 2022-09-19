/*
    This file is part of darktable,
    Copyright (C) 2012-2020 darktable developers.

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

#ifdef HAVE_GRAPHICSMAGICK
#include "imageio_gm.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/exif.h"
#include "control/conf.h"
#include "develop/develop.h"
#include "imageio.h"

#include <assert.h>
#include <inttypes.h>
#include <magick/api.h>
#include <memory.h>
#include <stdio.h>
#include <strings.h>


// we only support images with certain filename extensions via GraphicsMagick;
// RAWs are excluded as GraphicsMagick would render them with third party
// libraries in reduced quality - slow and only 8-bit
static gboolean _supported_image(const gchar *filename)
{
  const char *extensions_whitelist[] = { "tif",  "tiff", "gif", "jpc", "jp2", "bmp", "dcm", "jng",
                                         "miff", "mng",  "pbm", "pnm", "ppm", "pgm", "webp", NULL };
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


dt_imageio_retval_t dt_imageio_open_gm(dt_image_t *img, const char *filename, dt_mipmap_buffer_t *mbuf)
{
  int err = DT_IMAGEIO_FILE_CORRUPTED;
  ExceptionInfo exception;
  Image *image = NULL;
  ImageInfo *image_info = NULL;
  uint32_t width, height;

  if(!_supported_image(filename)) return DT_IMAGEIO_FILE_CORRUPTED;

  if(!img->exif_inited) (void)dt_exif_read(img, filename);

  GetExceptionInfo(&exception);
  image_info = CloneImageInfo((ImageInfo *)NULL);

  g_strlcpy(image_info->filename, filename, sizeof(image_info->filename));

  image = ReadImage(image_info, &exception);
  if(exception.severity != UndefinedException) CatchException(&exception);
  if(!image)
  {
    fprintf(stderr, "[GraphicsMagick_open] image `%s' not found\n", img->filename);
    err = DT_IMAGEIO_FILE_NOT_FOUND;
    goto error;
  }

  dt_print(DT_DEBUG_IMAGEIO, "[GraphicsMagick_open] image `%s' loading\n", img->filename);

  if(IsCMYKColorspace(image->colorspace))
  {
    fprintf(stderr, "[GraphicsMagick_open] error: CMYK images are not supported.\n");
    err =  DT_IMAGEIO_FILE_CORRUPTED;
    goto error;
  }

  width = image->columns;
  height = image->rows;

  img->width = width;
  img->height = height;

  img->buf_dsc.channels = 4;
  img->buf_dsc.datatype = TYPE_FLOAT;

  float *mipbuf = (float *)dt_mipmap_cache_alloc(mbuf, img);
  if(!mipbuf)
  {
    fprintf(stderr, "[GraphicsMagick_open] could not alloc full buffer for image `%s'\n", img->filename);
    err = DT_IMAGEIO_CACHE_FULL;
    goto error;
  }

  for(uint32_t row = 0; row < height; row++)
  {
    float *bufprt = mipbuf + (size_t)4 * row * img->width;
    int ret = DispatchImage(image, 0, row, width, 1, "RGBP", FloatPixel, bufprt, &exception);
    if(exception.severity != UndefinedException) CatchException(&exception);
    if(ret != MagickPass)
    {
      fprintf(stderr, "[GraphicsMagick_open] error reading image `%s'\n", img->filename);
      err = DT_IMAGEIO_FILE_CORRUPTED;
      goto error;
    }
  }

  size_t profile_length;
  const gchar* profile_data = (gchar*) GetImageProfile(image, "ICM", &profile_length);
  if(profile_data)
  {
    img->profile_size = profile_length;
    img->profile = (uint8_t *)g_malloc0(profile_length);
    memcpy(img->profile, profile_data, profile_length);
  }

  if(image) DestroyImage(image);
  if(image_info) DestroyImageInfo(image_info);
  DestroyExceptionInfo(&exception);

  img->buf_dsc.filters = 0u;
  img->flags &= ~DT_IMAGE_RAW;
  img->flags &= ~DT_IMAGE_HDR;
  img->flags &= ~DT_IMAGE_S_RAW;
  img->flags |= DT_IMAGE_LDR;

  img->loader = LOADER_GM;
  return DT_IMAGEIO_OK;

error:
  if(image) DestroyImage(image);
  if(image_info) DestroyImageInfo(image_info);
  DestroyExceptionInfo(&exception);
  return err;
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

