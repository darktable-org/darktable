/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#include "rawspeed/RawSpeed/StdAfx.h"
#include "rawspeed/RawSpeed/FileReader.h"
#include "rawspeed/RawSpeed/TiffParser.h"
#include "rawspeed/RawSpeed/RawDecoder.h"
#include "rawspeed/RawSpeed/CameraMetaData.h"
#include "rawspeed/RawSpeed/ColorFilterArray.h"

extern "C"
{
#include "imageio.h"
#include "common/imageio_rawspeed.h"
#include "common/exif.h"
#include "common/darktable.h"
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

static CameraMetaData *meta = NULL;

dt_imageio_retval_t
dt_imageio_open_rawspeed(dt_image_t *img, const char *filename)
{
  if(!img->exif_inited)
    (void) dt_exif_read(img, filename);

  char filen[1024];
  snprintf(filen, 1024, "%s", filename);
  FileReader f(filen);

  RawDecoder *d = NULL;
  FileMap* m = NULL;
  try
  {
    if(meta == NULL)
    {
      dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
      if(meta == NULL)
      {
        char datadir[1024], camfile[1024];
        dt_get_datadir(datadir, 1024);
        snprintf(camfile, 1024, "%s/rawspeed/cameras.xml", datadir);
        // never cleaned up (only when dt closes)
        meta = new CameraMetaData(camfile);
      }
      dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
    }
    try
    {
      m = f.readFile();
    }
    catch (FileIOException e)
    {
      return DT_IMAGEIO_FILE_CORRUPTED;
    }
    TiffParser t(m);
    t.parseData();
    d = t.getDecoder();
    try
    {
      d->checkSupport(meta);

      d->decodeRaw();
      d->decodeMetaData(meta);
      RawImage r = d->mRaw;
      r->scaleBlackWhite();
      img->filters = r->cfa.getDcrawFilter();
      if(img->filters)
      {
        img->flags &= ~DT_IMAGE_LDR;
        img->flags |= DT_IMAGE_RAW;
      }

      // also include used override in orient:
      const int orientation = dt_image_orientation(img);
      img->width  = (orientation & 4) ? r->dim.y : r->dim.x;
      img->height = (orientation & 4) ? r->dim.x : r->dim.y;

      if(dt_image_alloc(img, DT_IMAGE_FULL))
      {
        if (d) delete d;
        if (m) delete m;
        return DT_IMAGEIO_CACHE_FULL;
      }
      dt_image_check_buffer(img, DT_IMAGE_FULL, r->dim.x*r->dim.y*r->bpp);
      dt_imageio_flip_buffers((char *)img->pixels, (char *)r->getData(), r->bpp, r->dim.x, r->dim.y, r->dim.x, r->dim.y, r->pitch, orientation);
    }
    catch (RawDecoderException e)
    {
      if (d) delete d;
      if (m) delete m;
      return DT_IMAGEIO_FILE_CORRUPTED;
    }
  }
  catch (CameraMetadataException e)
  {
    if (d) delete d;
    if (m) delete m;
    return DT_IMAGEIO_FILE_CORRUPTED;
  }
  catch (TiffParserException e)
  {
    if (d) delete d;
    if (m) delete m;
    return DT_IMAGEIO_FILE_CORRUPTED;
  }

  // clean up raw stuff.
  if (d) delete d;
  if (m) delete m;
  dt_image_release(img, DT_IMAGE_FULL, 'w');
  return DT_IMAGEIO_OK;
}

dt_imageio_retval_t
dt_imageio_open_rawspeed_preview(dt_image_t *img, const char *filename)
{
  if(!img->exif_inited)
    (void) dt_exif_read(img, filename);

  char filen[1024];
  snprintf(filen, 1024, "%s", filename);
  FileReader f(filen);

  uint16_t *buf = NULL;
  RawDecoder *d = NULL;
  FileMap* m = NULL;
  try
  {
    if(meta == NULL)
    {
      dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
      if(meta == NULL)
      {
        char datadir[1024], camfile[1024];
        dt_get_datadir(datadir, 1024);
        snprintf(camfile, 1024, "%s/rawspeed/cameras.xml", datadir);
        // never cleaned up (only when dt closes)
        meta = new CameraMetaData(camfile);
      }
      dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
    }
    try
    {
      m = f.readFile();
    }
    catch (FileIOException e)
    {
      return DT_IMAGEIO_FILE_CORRUPTED;
    }
    TiffParser t(m);
    t.parseData();
    d = t.getDecoder();
    try
    {
      d->checkSupport(meta);

      d->decodeRaw();
      d->decodeMetaData(meta);
      RawImage r = d->mRaw;
      r->scaleBlackWhite();
      img->filters = r->cfa.getDcrawFilter();

      // also include used override in orient:
      const int orientation = dt_image_orientation(img);
      img->width  = (orientation & 4) ? r->dim.y : r->dim.x;
      img->height = (orientation & 4) ? r->dim.x : r->dim.y;

      buf = (uint16_t *)dt_alloc_align(16, r->dim.x*r->dim.y*r->bpp);
      if(!buf)
      {
        if (d) delete d;
        if (m) delete m;
        return DT_IMAGEIO_CACHE_FULL;
      }
      dt_imageio_flip_buffers((char *)buf, (char *)r->getData(), r->bpp, r->dim.x, r->dim.y, r->dim.x, r->dim.y, r->pitch, orientation);
    }
    catch (RawDecoderException e)
    {
      if (d) delete d;
      if (m) delete m;
      return DT_IMAGEIO_FILE_CORRUPTED;
    }
  }
  catch (CameraMetadataException e)
  {
    if (d) delete d;
    if (m) delete m;
    return DT_IMAGEIO_FILE_CORRUPTED;
  }
  catch (TiffParserException e)
  {
    if (d) delete d;
    if (m) delete m;
    return DT_IMAGEIO_FILE_CORRUPTED;
  }

  dt_imageio_retval_t retv = DT_IMAGEIO_OK;
  if(img->filters)
  {
    img->flags &= ~DT_IMAGE_LDR;
    img->flags |= DT_IMAGE_RAW;
  }
  dt_image_raw_to_preview(img, (float *)buf);

  // clean up raw stuff.
  if (buf) free(buf);
  if (d) delete d;
  if (m) delete m;
  dt_image_release(img, DT_IMAGE_FULL, 'w');
  return retv;
}
