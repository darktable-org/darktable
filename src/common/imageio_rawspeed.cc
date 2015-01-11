/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.

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
#ifdef _OPENMP
#include <omp.h>
#endif

#include <memory>

#include "rawspeed/RawSpeed/StdAfx.h"
#include "rawspeed/RawSpeed/FileReader.h"
#include "rawspeed/RawSpeed/RawDecoder.h"
#include "rawspeed/RawSpeed/RawParser.h"
#include "rawspeed/RawSpeed/CameraMetaData.h"
#include "rawspeed/RawSpeed/ColorFilterArray.h"

extern "C" {
#include "imageio.h"
#include "common/imageio_rawspeed.h"
#include "common/exif.h"
#include "common/darktable.h"
#include "common/colorspaces.h"
#include "common/file_location.h"
}

// define this function, it is only declared in rawspeed:
int rawspeed_get_number_of_processor_cores()
{
#ifdef _OPENMP
  return omp_get_num_procs();
#else
  return 1;
#endif
}

using namespace RawSpeed;

dt_imageio_retval_t dt_imageio_open_rawspeed_sraw(dt_image_t *img, RawImage r, dt_mipmap_buffer_t *buf);
static CameraMetaData *meta = NULL;

#if 0
static void
scale_black_white(uint16_t *const buf, const uint16_t black, const uint16_t white, const int width, const int height, const int stride)
{
  const float scale = 65535.0f/(white-black);
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j=0; j<height; j++)
  {
    uint16_t *b = buf + j*stride;
    for(int i=0; i<width; i++)
    {
      b[0] = CLAMPS((b[0] - black)*scale, 0, 0xffff);
      b++;
    }
  }
}
#endif

dt_imageio_retval_t dt_imageio_open_rawspeed(dt_image_t *img, const char *filename,
                                             dt_mipmap_buffer_t *mbuf)
{
  if(!img->exif_inited) (void)dt_exif_read(img, filename);

#ifdef __WIN32__
  const size_t len = strlen(filename) + 1;
  wchar_t filen[len];
  mbstowcs(filen, filename, len);
  FileReader f(filen);
#else
  char filen[PATH_MAX] = { 0 };
  snprintf(filen, sizeof(filen), "%s", filename);
  FileReader f(filen);
#endif

#ifdef __APPLE__
  std::auto_ptr<RawDecoder> d;
  std::auto_ptr<FileMap> m;
#else
  std::unique_ptr<RawDecoder> d;
  std::unique_ptr<FileMap> m;
#endif

  try
  {
    /* Load rawspeed cameras.xml meta file once */
    if(meta == NULL)
    {
      dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
      if(meta == NULL)
      {
        char datadir[PATH_MAX] = { 0 }, camfile[PATH_MAX] = { 0 };
        dt_loc_get_datadir(datadir, sizeof(datadir));
        snprintf(camfile, sizeof(camfile), "%s/rawspeed/cameras.xml", datadir);
        // never cleaned up (only when dt closes)
        meta = new CameraMetaData(camfile);
      }
      dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
    }

#ifdef __APPLE__
    m = auto_ptr<FileMap>(f.readFile());
#else
    m = unique_ptr<FileMap>(f.readFile());
#endif

    RawParser t(m.get());
#ifdef __APPLE__
    d = auto_ptr<RawDecoder>(t.getDecoder());
#else
    d = unique_ptr<RawDecoder>(t.getDecoder());
#endif

    if(!d.get()) return DT_IMAGEIO_FILE_CORRUPTED;

    d->failOnUnknown = true;
    d->checkSupport(meta);
    d->decodeRaw();
    d->decodeMetaData(meta);
    RawImage r = d->mRaw;

    /* free auto pointers on spot */
    d.reset();
    m.reset();

    img->filters = 0u;
    if(!r->isCFA)
    {
      dt_imageio_retval_t ret = dt_imageio_open_rawspeed_sraw(img, r, mbuf);
      return ret;
    }

    // only scale colors for sizeof(uint16_t) per pixel, not sizeof(float)
    // if(r->getDataType() != TYPE_FLOAT32) scale_black_white((uint16_t *)r->getData(), r->blackLevel,
    // r->whitePoint, r->dim.x, r->dim.y, r->pitch/r->getBpp());
    if(r->getDataType() != TYPE_FLOAT32) r->scaleBlackWhite();
    img->bpp = r->getBpp();
    img->filters = r->cfa.getDcrawFilter();
    if(img->filters)
    {
      img->flags &= ~DT_IMAGE_LDR;
      img->flags |= DT_IMAGE_RAW;
      if(r->getDataType() == TYPE_FLOAT32) img->flags |= DT_IMAGE_HDR;
      // special handling for x-trans sensors
      if(img->filters == 9u)
      {
        // get 6x6 CFA offset from top left of cropped image
        // NOTE: This is different from how things are done with Bayer
        // sensors. For these, the CFA in cameras.xml is pre-offset
        // depending on the distance modulo 2 between raw and usable
        // image data. For X-Trans, the CFA in cameras.xml is
        // (currently) aligned with the top left of the raw data, and
        // hence it is shifted here to align with the top left of the
        // cropped image.
        iPoint2D tl_margin = r->getCropOffset();
        for(int i = 0; i < 6; ++i)
          for(int j = 0; j < 6; ++j)
            img->xtrans[j][i] = r->cfa.getColorAt((i + tl_margin.x) % 6, (j + tl_margin.y) % 6);
      }
    }

    img->width = r->dim.x;
    img->height = r->dim.y;

    /* needed in exposure iop for Deflicker */
    img->raw_black_level = r->blackLevel;
    img->raw_white_point = r->whitePoint;

    img->fuji_rotation_pos = r->metadata.fujiRotationPos;
    img->pixel_aspect_ratio = (float)r->metadata.pixelAspectRatio;

    for (int i=0; i<3; i++)
      img->wb_coeffs[i] = r->metadata.wbCoeffs[i];

    void *buf = dt_mipmap_cache_alloc(mbuf, img);
    if(!buf) return DT_IMAGEIO_CACHE_FULL;

    dt_imageio_flip_buffers((char *)buf, (char *)r->getData(), r->getBpp(), r->dim.x, r->dim.y, r->dim.x,
                            r->dim.y, r->pitch, ORIENTATION_NONE);
  }
  catch(const std::exception &exc)
  {
    printf("[rawspeed] %s\n", exc.what());

    /* if an exception is raised lets not retry or handle the
     specific ones, consider the file as corrupted */
    return DT_IMAGEIO_FILE_CORRUPTED;
  }
  catch(...)
  {
    printf("Unhandled exception in imageio_rawspeed\n");
    return DT_IMAGEIO_FILE_CORRUPTED;
  }

  return DT_IMAGEIO_OK;
}

dt_imageio_retval_t dt_imageio_open_rawspeed_sraw(dt_image_t *img, RawImage r, dt_mipmap_buffer_t *mbuf)
{
  // sraw aren't real raw, but not ldr either (need white balance and stuff)
  img->flags &= ~DT_IMAGE_LDR;
  img->flags &= ~DT_IMAGE_RAW;

  img->width = r->dim.x;
  img->height = r->dim.y;

  // Grab the WB
  for (int i=0; i<3; i++)
    img->wb_coeffs[i] = r->metadata.wbCoeffs[i];

  /* needed by Deflicker */
  img->raw_black_level = r->blackLevel;
  img->raw_white_point = r->whitePoint;

  size_t raw_width = r->dim.x;
  size_t raw_height = r->dim.y;

  iPoint2D dimUncropped = r->getUncroppedDim();
  iPoint2D cropTL = r->getCropOffset();

  // work around 50D bug
  char makermodel[1024];
  dt_colorspaces_get_makermodel(makermodel, sizeof(makermodel), img->exif_maker, img->exif_model);

  // actually we want to store full floats here:
  img->bpp = 4 * sizeof(float);
  img->cpp = r->getCpp();
  void *buf = dt_mipmap_cache_alloc(mbuf, img);
  if(!buf) return DT_IMAGEIO_CACHE_FULL;

  int black = r->blackLevel;
  int white = r->whitePoint;

  uint16_t *raw_img = (uint16_t *)r->getDataUncropped(0, 0);

  const float scale = (float)(white - black);

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) shared(raw_width, raw_height, raw_img, img,          \
                                                               dimUncropped, cropTL, buf, black)
#endif
  for(size_t row = 0; row < raw_height; row++)
  {
    const uint16_t *in = ((uint16_t *)raw_img)
                         + (size_t)(img->cpp * (dimUncropped.x * (row + cropTL.y) + cropTL.x));
    float *out = ((float *)buf) + (size_t)4 * row * raw_width;

    for(size_t col = 0; col < raw_width; col++, in += img->cpp, out += 4)
    {
      for(int k = 0; k < 3; k++)
      {
        if(img->cpp == 1)
        {
          /*
           * monochrome image (e.g. Leica M9 monochrom),
           * we need to copy data from only channel to each of 3 channels
           */

          out[k] = MAX(0.0f, (((float)(*in)) - black) / scale);
        }
        else
        {
          /*
           * standard 3-ch image
           * just copy 3 ch to 3 ch
           */

          out[k] = MAX(0.0f, (((float)(in[k])) - black) / scale);
        }
      }
    }
  }

  return DT_IMAGEIO_OK;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
