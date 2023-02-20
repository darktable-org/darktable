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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <inttypes.h>
#include <memory>
#include <stdio.h>
#include <string.h>

#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfInputFile.h>
#include <OpenEXR/ImfStandardAttributes.h>
#include <OpenEXR/ImfTestFile.h>
#include <OpenEXR/ImfThreading.h>
#include <OpenEXR/ImfTiledInputFile.h>

#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/exif.h"
#include "common/metadata.h"
#include "common/datetime.h"
#include "control/conf.h"
#include "develop/develop.h"
#include "imageio/imageio_common.h"
#include "imageio/imageio_exr.h"
#include "imageio/imageio_exr.hh"

dt_imageio_retval_t dt_imageio_open_exr(dt_image_t *img, const char *filename, dt_mipmap_buffer_t *mbuf)
{
  bool isTiled = false;

  Imf::setGlobalThreadCount(dt_get_num_threads());

  std::unique_ptr<Imf::TiledInputFile> fileTiled;
  std::unique_ptr<Imf::InputFile> file;

  Imath::Box2i dw;
  Imf::FrameBuffer frameBuffer;
  uint32_t xstride, ystride;


  /* verify openexr image */
  if(!Imf::isOpenExrFile((const char *)filename, isTiled)) return DT_IMAGEIO_LOAD_FAILED;

  /* open exr file */
  try
  {
    if(isTiled)
    {
      std::unique_ptr<Imf::TiledInputFile> temp(new Imf::TiledInputFile(filename));
      fileTiled = std::move(temp);
    }
    else
    {
      std::unique_ptr<Imf::InputFile> temp(new Imf::InputFile(filename));
      file = std::move(temp);
    }
  }
  catch(const std::exception &e)
  {
    return DT_IMAGEIO_LOAD_FAILED;
  }

  const Imf::Header &header = isTiled ? fileTiled->header() : file->header();

  /* check that channels available is any of supported RGB(a) */
  bool hasR = false, hasG = false, hasB = false;
  for(Imf::ChannelList::ConstIterator i = header.channels().begin(); i != header.channels().end(); ++i)
  {
    std::string name(i.name());
    if(name == "R") hasR = true;
    if(name == "G") hasG = true;
    if(name == "B") hasB = true;
  }
  if(!(hasR && hasG && hasB))
  {
    dt_print(DT_DEBUG_ALWAYS, "[exr_open] error: only images with RGB(A) channels are supported, skipping `%s'\n", img->filename);
    return DT_IMAGEIO_LOAD_FAILED;
  }

  if(!img->exif_inited)
  {
    // read back exif data, either as embedded blob or "standard" attributes
    // if another software is able to update these exif data, the former test
    // should be removed to take the potential changes in account (not done
    // by normal import image flow)
    const Imf::BlobAttribute *exif = header.findTypedAttribute<Imf::BlobAttribute>("exif");
    if(exif)
    {
      uint8_t *exif_blob = exif->value().data.get();
      uint32_t exif_size = exif->value().size;
      /* skip any superfluous "Exif\0\0" APP1 prefix written by dt 4.0.0 and earlier */
      if(exif_size >= 6 && !memcmp(exif_blob, "Exif\0\0", 6))
      {
        exif_blob += 6;
        exif_size -= 6;
      }
      if(exif_size > 0) dt_exif_read_from_blob(img, exif_blob, exif_size);
    }
    else
    {
      if(Imf::hasOwner(header)) dt_metadata_set_import(img->id, "Xmp.dc.rights", Imf::owner(header).c_str());
      if(Imf::hasComments(header))
        dt_metadata_set_import(img->id, "Xmp.dc.description", Imf::comments(header).c_str());
      if(Imf::hasCapDate(header))
      {
        // utcOffset can be ignored for now, see dt_datetime_exif_to_numbers()
        char *datetime = strdup(Imf::capDate(header).c_str());
        dt_exif_sanitize_datetime(datetime);
        dt_datetime_exif_to_img(img, datetime);
        free(datetime);
      }
      if(Imf::hasLongitude(header) && Imf::hasLatitude(header))
      {
        img->geoloc.longitude = Imf::longitude(header);
        img->geoloc.latitude = Imf::latitude(header);
      }
      if(Imf::hasAltitude(header)) img->geoloc.elevation = Imf::altitude(header);
      if(Imf::hasFocus(header)) img->exif_focus_distance = Imf::focus(header);
      if(Imf::hasExpTime(header)) img->exif_exposure = Imf::expTime(header);
      if(Imf::hasAperture(header)) img->exif_aperture = Imf::aperture(header);
      if(Imf::hasIsoSpeed(header)) img->exif_iso = Imf::isoSpeed(header);
    }
  }

  /* Get image width and height from displayWindow */
  dw = header.displayWindow();
  img->width = dw.max.x - dw.min.x + 1;
  img->height = dw.max.y - dw.min.y + 1;

  // Try to allocate image data
  img->buf_dsc.channels = 4;
  img->buf_dsc.datatype = TYPE_FLOAT;
  float *buf = (float *)dt_mipmap_cache_alloc(mbuf, img);
  if(!buf)
  {
    dt_print(DT_DEBUG_ALWAYS, "[exr_open] error: could not alloc full buffer for image `%s'\n", img->filename);
    return DT_IMAGEIO_CACHE_FULL;
  }

  /* setup framebuffer */
  xstride = sizeof(float) * 4;
  ystride = sizeof(float) * img->width * 4;
  frameBuffer.insert("R", Imf::Slice(Imf::FLOAT, (char *)(buf + 0), xstride, ystride, 1, 1, 0.0));
  frameBuffer.insert("G", Imf::Slice(Imf::FLOAT, (char *)(buf + 1), xstride, ystride, 1, 1, 0.0));
  frameBuffer.insert("B", Imf::Slice(Imf::FLOAT, (char *)(buf + 2), xstride, ystride, 1, 1, 0.0));
  frameBuffer.insert("A", Imf::Slice(Imf::FLOAT, (char *)(buf + 3), xstride, ystride, 1, 1, 0.0));

  if(isTiled)
  {
    fileTiled->setFrameBuffer(frameBuffer);
    fileTiled->readTiles(0, fileTiled->numXTiles() - 1, 0, fileTiled->numYTiles() - 1);
  }
  else
  {
    /* read pixels from dataWindow */
    dw = header.dataWindow();
    file->setFrameBuffer(frameBuffer);
    file->readPixels(dw.min.y, dw.max.y);
  }

  /* try to get the chromaticities and whitepoint. this will add the default linear rec709 profile when nothing
   * was embedded and look as if it was embedded in colorin. better than defaulting to something wrong there. */
  Imf::Chromaticities chromaticities;
  float whiteLuminance = 1.0;

  if(Imf::hasChromaticities(header))
  {
    chromaticities = Imf::chromaticities(header);

    /* adapt chromaticities to D65 expected by colorin */
    cmsCIExyY red_xy = { chromaticities.red[0], chromaticities.red[1], 1.0 };
    cmsCIEXYZ srcRed;
    cmsxyY2XYZ(&srcRed, &red_xy);

    cmsCIExyY green_xy = { chromaticities.green[0], chromaticities.green[1], 1.0 };
    cmsCIEXYZ srcGreen;
    cmsxyY2XYZ(&srcGreen, &green_xy);

    cmsCIExyY blue_xy = { chromaticities.blue[0], chromaticities.blue[1], 1.0 };
    cmsCIEXYZ srcBlue;
    cmsxyY2XYZ(&srcBlue, &blue_xy);

    const cmsCIExyY srcWhite_xy = { chromaticities.white[0], chromaticities.white[1], 1.0 };
    cmsCIEXYZ srcWhite;
    cmsxyY2XYZ(&srcWhite, &srcWhite_xy);

    /* use Imf::Chromaticities definition */
    const cmsCIExyY d65_xy = {0.3127f, 0.3290f, 1.0};
    cmsCIEXYZ d65;
    cmsxyY2XYZ(&d65, &d65_xy);

    cmsCIEXYZ dstRed;
    cmsAdaptToIlluminant(&dstRed, &srcWhite, &d65, &srcRed);

    cmsCIEXYZ dstGreen;
    cmsAdaptToIlluminant(&dstGreen, &srcWhite, &d65, &srcGreen);

    cmsCIEXYZ dstBlue;
    cmsAdaptToIlluminant(&dstBlue, &srcWhite, &d65, &srcBlue);

    cmsXYZ2xyY(&red_xy, &dstRed);
    chromaticities.red[0] = (float)red_xy.x;
    chromaticities.red[1] = (float)red_xy.y;

    cmsXYZ2xyY(&green_xy, &dstGreen);
    chromaticities.green[0] = (float)green_xy.x;
    chromaticities.green[1] = (float)green_xy.y;

    cmsXYZ2xyY(&blue_xy, &dstBlue);
    chromaticities.blue[0] = (float)blue_xy.x;
    chromaticities.blue[1] = (float)blue_xy.y;

    chromaticities.white[0] = 0.3127f;
    chromaticities.white[1] = 0.3290f;
  }

  if(Imf::hasWhiteLuminance(header))
    whiteLuminance = Imf::whiteLuminance(header);

  Imath::M44f m = Imf::XYZtoRGB(chromaticities, whiteLuminance);

  for(int i = 0; i < 3; i++)
    for(int j = 0; j < 3; j++)
    {
      img->d65_color_matrix[3 * i + j] = m[j][i];
    }

  img->buf_dsc.cst = IOP_CS_RGB;
  img->buf_dsc.filters = 0u;
  img->flags &= ~DT_IMAGE_RAW;
  img->flags &= ~DT_IMAGE_S_RAW;
  img->flags &= ~DT_IMAGE_LDR;
  img->flags |= DT_IMAGE_HDR;

  img->loader = LOADER_EXR;
  return DT_IMAGEIO_OK;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
