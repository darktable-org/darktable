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
#include "rawspeed/RawSpeed/StdAfx.h"
#include "rawspeed/RawSpeed/FileReader.h"
#include "rawspeed/RawSpeed/TiffParser.h"
#include "rawspeed/RawSpeed/RawDecoder.h"
#include "rawspeed/RawSpeed/CameraMetaData.h"
#include "rawspeed/RawSpeed/ColorFilterArray.h"

extern "C"
{
#include "common/imageio_rawspeed.h"
#include "common/exif.h"
#include "common/darktable.h"
}

using namespace RawSpeed;

dt_imageio_retval_t
dt_imageio_open_rawspeed(dt_image_t *img, const char *filename)
{
  if(!img->exif_inited)
    (void) dt_exif_read(img, filename);

  char datadir[1024], camfile[1024];
  snprintf(datadir, 1024, "%s", filename);
  FileReader f(datadir);
  dt_get_datadir(datadir, 1024);
  snprintf(camfile, 1024, "%s/rawspeed/cameras.xml", datadir);

  RawDecoder *d = NULL;
  FileMap* m = NULL;
  try
  {
    printf("[rawspeed] getting metadata...\n");
    CameraMetaData meta(camfile);
    try
    {
      printf("[rawspeed] reading file...\n");
      m = f.readFile();
      printf("[rawspeed] read file.\n");
    }
    catch (FileIOException e)
    {
      fprintf(stderr, "[rawspeed] could not open image:%s\n", e.what());
      return DT_IMAGEIO_FILE_CORRUPTED;
    }
    printf("[rawspeed] parsing tiff...\n");
    TiffParser t(m);
    t.parseData();
    d = t.getDecoder();
    try
    {
      printf("[rawspeed] loading raw...\n");
      d->checkSupport(&meta);

      d->decodeRaw();
      d->decodeMetaData(&meta);
      printf("[rawspeed] loading raw...\n");
      RawImage r = d->mRaw;
      r->scaleBlackWhite();
      img->filters = r->cfa.getDcrawFilter();
      printf("[rawspeed] loading raw...\n");

      for (uint32 i = 0; i < d->errors.size(); i++)
      {
        printf("Error Encountered:%s", d->errors[i]);
      }
      // TODO get orientation from exif instead?
      // img->orientation = raw->sizes.flip;

      img->width  = (img->orientation & 4) ? r->dim.y : r->dim.x;
      img->height = (img->orientation & 4) ? r->dim.x : r->dim.y;
      // img->exif_iso = raw->other.iso_speed;
      // img->exif_exposure = raw->other.shutter;
      // img->exif_aperture = raw->other.aperture;
      // img->exif_focal_length = raw->other.focal_len;
      // strncpy(img->exif_maker, raw->idata.make, sizeof(img->exif_maker));
      // img->exif_maker[sizeof(img->exif_maker) - 1] = 0x0;
      // strncpy(img->exif_model, raw->idata.model, sizeof(img->exif_model));
      // img->exif_model[sizeof(img->exif_model) - 1] = 0x0;
      // dt_gettime_t(img->exif_datetime_taken, raw->other.timestamp);

      printf("[rawspeed] loaded raw\n");
      if(dt_image_alloc(img, DT_IMAGE_FULL))
      {
        if (d) delete d;
        if (m) delete m;
        return DT_IMAGEIO_CACHE_FULL;
      }
      // dt_image_check_buffer(img, DT_IMAGE_FULL, (img->width)*(img->height)*sizeof(uint16_t));
      // memcpy(img->pixels, image->data, img->width*img->height*sizeof(uint16_t));
      dt_image_check_buffer(img, DT_IMAGE_FULL, r->dim.x*r->dim.y*r->bpp);
      memcpy(img->pixels, r->getData(), r->dim.x*r->dim.y*r->bpp);
      printf("[rawspeed] copied raw.\n");
    }
    catch (RawDecoderException e)
    {
      // wchar_t uni[1024];
      // MultiByteToWideChar(CP_ACP, 0, e.what(), -1, uni, 1024);
      // //    MessageBox(0,uni, L"RawDecoder Exception",0);
      // wprintf(L"Raw Decoder Exception:%s\n",uni);
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
    // wchar_t uni[1024];
    // MultiByteToWideChar(CP_ACP, 0, e.what(), -1, uni, 1024);
    // //    MessageBox(0,uni, L"Tiff Parser error",0);
    // wprintf(L"Tiff Exception:%s\n",uni);
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

