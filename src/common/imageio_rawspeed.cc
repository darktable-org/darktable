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
#ifdef HAVE_RAWSPEED
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

extern "C"
{
#include "imageio.h"
#include "common/imageio_rawspeed.h"
#include "common/exif.h"
#include "common/darktable.h"
#include "common/colorspaces.h"
#include "common/file_location.h"
}

// define this function, it is only declared in rawspeed:
int
rawspeed_get_number_of_processor_cores()
{
#ifdef _OPENMP
  return omp_get_num_procs();
#else
  return 1;
#endif
}

using namespace RawSpeed;

dt_imageio_retval_t dt_imageio_open_rawspeed_sraw(dt_image_t *img, RawImage r, dt_mipmap_cache_allocator_t a);
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

dt_imageio_retval_t
dt_imageio_open_rawspeed(
  dt_image_t  *img,
  const char  *filename,
  dt_mipmap_cache_allocator_t a)
{
  if(!img->exif_inited)
    (void) dt_exif_read(img, filename);

#ifdef __WIN32__
  const size_t len = strlen(filename) + 1;
  wchar_t filen[len];
  mbstowcs(filen, filename, len);
  FileReader f(filen);
#else
  char filen[1024];
  snprintf(filen, 1024, "%s", filename);
  FileReader f(filen);
#endif

  std::auto_ptr<RawDecoder> d;
  std::auto_ptr<FileMap> m;

  try
  {
    /* Load rawspeed cameras.xml meta file once */
    if(meta == NULL)
    {
      dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
      if(meta == NULL)
      {
        char datadir[1024], camfile[1024];
        dt_loc_get_datadir(datadir, 1024);
        snprintf(camfile, 1024, "%s/rawspeed/cameras.xml", datadir);
        // never cleaned up (only when dt closes)
        meta = new CameraMetaData(camfile);
      }
      dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
    }

    m = auto_ptr<FileMap>(f.readFile());

    RawParser t(m.get());
    d = auto_ptr<RawDecoder>(t.getDecoder());

    if(!d.get())
      return DT_IMAGEIO_FILE_CORRUPTED;

    d->failOnUnknown = true;
    d->checkSupport(meta);
    d->decodeRaw();
    d->decodeMetaData(meta);
    RawImage r = d->mRaw;

    /* free auto pointers on spot */
    d.reset();
    m.reset();

    img->filters = 0;
    if( !r->isCFA )
    {
      dt_imageio_retval_t ret = dt_imageio_open_rawspeed_sraw(img, r, a);
      return ret;
    }

    // only scale colors for sizeof(uint16_t) per pixel, not sizeof(float)
    // if(r->getDataType() != TYPE_FLOAT32) scale_black_white((uint16_t *)r->getData(), r->blackLevel, r->whitePoint, r->dim.x, r->dim.y, r->pitch/r->getBpp());
    if(r->getDataType() != TYPE_FLOAT32) r->scaleBlackWhite();
    img->bpp = r->getBpp();
    img->filters = r->cfa.getDcrawFilter();
    if(img->filters)
    {
      img->flags &= ~DT_IMAGE_LDR;
      img->flags |= DT_IMAGE_RAW;
      if(r->getDataType() == TYPE_FLOAT32) img->flags |= DT_IMAGE_HDR;
    }

    // also include used override in orient:
    const int orientation = dt_image_orientation(img);
    img->width  = (orientation & 4) ? r->dim.y : r->dim.x;
    img->height = (orientation & 4) ? r->dim.x : r->dim.y;

    /* needed in exposure iop for Deflicke */
    img->raw_black_level = r->blackLevel;
    img->raw_white_point = r->whitePoint;

    void *buf = dt_mipmap_cache_alloc(img, DT_MIPMAP_FULL, a);
    if(!buf)
      return DT_IMAGEIO_CACHE_FULL;

    dt_imageio_flip_buffers((char *)buf, (char *)r->getData(), r->getBpp(), r->dim.x, r->dim.y, r->dim.x, r->dim.y, r->pitch, orientation);
  }
  catch (const std::exception &exc)
  {
    printf("[rawspeed] %s\n", exc.what());

    /* if an exception is raised lets not retry or handle the
     specific ones, consider the file as corrupted */
    return DT_IMAGEIO_FILE_CORRUPTED;
  }
  catch (...)
  {
    printf("Unhandled exception in imageio_rawspeed\n");
    return DT_IMAGEIO_FILE_CORRUPTED;
  }

  return DT_IMAGEIO_OK;
}

dt_imageio_retval_t
dt_imageio_open_rawspeed_sraw(dt_image_t *img, RawImage r, dt_mipmap_cache_allocator_t a)
{
  // sraw aren't real raw, but not ldr either (need white balance and stuff)
  img->flags &= ~DT_IMAGE_LDR;
  img->flags &= ~DT_IMAGE_RAW;

  const int orientation = dt_image_orientation(img);
  img->width  = (orientation & 4) ? r->dim.y : r->dim.x;
  img->height = (orientation & 4) ? r->dim.x : r->dim.y;

  /* needed by Deflicker */
  img->raw_black_level = r->blackLevel;
  img->raw_white_point = r->whitePoint;

  size_t raw_width = r->dim.x;
  size_t raw_height = r->dim.y;

  // work around 50D bug
  char makermodel[1024];
  dt_colorspaces_get_makermodel(makermodel, 1024, img->exif_maker, img->exif_model);

  // actually we want to store full floats here:
  img->bpp = 4*sizeof(float);
  void *buf = dt_mipmap_cache_alloc(img, DT_MIPMAP_FULL, a);
  if(!buf)
    return DT_IMAGEIO_CACHE_FULL;

  int black = r->blackLevel;
  int white = r->whitePoint;

  ushort16* raw_img = (ushort16*)r->getData();

#if 0
  dt_imageio_flip_buffers_ui16_to_float(buf, raw_img, black, white, 3, raw_width, raw_height,
                                        raw_width, raw_height, raw_width + raw_width_extra, orientation);
#else

  // TODO - OMPize this.
  float scale = 1.0 / (white - black);
  for( size_t row = 0; row < raw_height; ++row )
    for( size_t col = 0; col < raw_width; ++col )
      for( int k = 0; k < 3; ++k )
        ((float *)buf)[4 * dt_imageio_write_pos(col, row, raw_width, raw_height, raw_width, raw_height, orientation) + k] =
          // ((float)raw_img[row*(raw_width + raw_width_extra)*3 + col*3 + k] - black) * scale;
          ((float)raw_img[row*(r->pitch/2) + col*3 + k] - black) * scale;
#endif

  return DT_IMAGEIO_OK;
}

#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
