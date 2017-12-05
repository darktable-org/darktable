/*
    This file is part of darktable,
    copyright (c) 2010--2014 Henrik Andersson.

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

#define __STDC_FORMAT_MACROS

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

extern "C" {
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/exif.h"
#include "common/imageio.h"
#include "common/imageio_exr.h"
#include "control/conf.h"
#include "develop/develop.h"
}
#include "common/imageio_exr.hh"

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
  if(!Imf::isOpenExrFile((const char *)filename, isTiled)) return DT_IMAGEIO_FILE_CORRUPTED;

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
    return DT_IMAGEIO_FILE_CORRUPTED;
  }

  const Imf::Header &header = isTiled ? fileTiled->header() : file->header();

  /* check that channels available is any of supported RGB(a) */
  uint32_t cnt = 0;
  for(Imf::ChannelList::ConstIterator i = header.channels().begin(); i != header.channels().end(); ++i)
  {
    cnt++;
    if(i.name()[0] != 'R' && i.name()[0] != 'G' && i.name()[0] != 'B' && i.name()[0] != 'A')
    {
      fprintf(stderr, "[exr_read] Warning, only files with RGB(A) channels are supported.\n");
      return DT_IMAGEIO_FILE_CORRUPTED;
    }
  }

  /* we only support 3 and 4 channels */
  if(cnt < 3 || cnt > 4)
  {
    fprintf(stderr, "[exr_read] Warning, only files with 3 or 4 channels are supported.\n");
    return DT_IMAGEIO_FILE_CORRUPTED;
  }

  // read back exif data
  const Imf::BlobAttribute *exif = header.findTypedAttribute<Imf::BlobAttribute>("exif");
  // we append a jpg-compatible exif00 string, so get rid of that again:
  if(exif && exif->value().size > 6)
    dt_exif_read_from_blob(img, ((uint8_t *)(exif->value().data.get())) + 6, exif->value().size - 6);

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
    fprintf(stderr, "[exr_read] could not alloc full buffer for image `%s'\n", img->filename);
    /// \todo open exr cleanup...
    return DT_IMAGEIO_CACHE_FULL;
  }

  // FIXME: is this really needed?
  memset(buf, 0, 4 * img->width * img->height * sizeof(float));

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
    chromaticities = Imf::chromaticities(header);

  if(Imf::hasWhiteLuminance(header))
    whiteLuminance = Imf::whiteLuminance(header);

//   printf("hasChromaticities: %d\n", Imf::hasChromaticities(header));
//   printf("hasWhiteLuminance: %d\n", Imf::hasWhiteLuminance(header));
//   std::cout << chromaticities.red << std::endl;
//   std::cout << chromaticities.green << std::endl;
//   std::cout << chromaticities.blue << std::endl;
//   std::cout << chromaticities.white << std::endl;

  Imath::M44f m = Imf::RGBtoXYZ(chromaticities, whiteLuminance);
  float mat[3][3];

  for(int i = 0; i < 3; i++)
    for(int j = 0; j < 3; j++)
    {
      mat[i][j] = m[j][i];
    }

  mat3inv((float *)img->d65_color_matrix, (float *)mat);


  /* cleanup and return... */
  img->flags |= DT_IMAGE_HDR;

  return DT_IMAGEIO_OK;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
