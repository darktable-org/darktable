/*
    This file is part of darktable,
    copyright (c) 2010 Henrik Andersson.

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

extern "C"
{
#include "common/imageio_exr.h"
#include "common/imageio.h"
#include "common/darktable.h"
#include "develop/develop.h"
#include "common/exif.h"
#include "common/colorspaces.h"
#include "control/conf.h"
}

#include "common/imageio_exr.hh"
#include <memory>
#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfTestFile.h>
#include <OpenEXR/ImfInputFile.h>
#include <OpenEXR/ImfTiledInputFile.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfStandardAttributes.h>


dt_imageio_retval_t dt_imageio_open_exr (dt_image_t *img, const char *filename, dt_mipmap_cache_allocator_t a)
{
  bool isTiled=false;
  std::auto_ptr<Imf::TiledInputFile> fileTiled;
  std::auto_ptr<Imf::InputFile> file;
  const Imf::Header *header=NULL;

  /* verify openexr image */
  if(!Imf::isOpenExrFile ((const char *)filename,isTiled))
    return DT_IMAGEIO_FILE_CORRUPTED;

  /* open exr file */
  try
  {
    if(isTiled)
    {
      std::auto_ptr<Imf::TiledInputFile> temp(new Imf::TiledInputFile(filename));
      fileTiled = temp;
      header = &(fileTiled->header());
    }
    else
    {
      std::auto_ptr<Imf::InputFile> temp(new Imf::InputFile(filename));
      file = temp;
      header = &(file->header());
    }
  }
  catch (const std::exception &e)
  {
    return DT_IMAGEIO_FILE_CORRUPTED;
  }

  // read back exif data
  const Imf::BlobAttribute *exif =
    header->findTypedAttribute <Imf::BlobAttribute> ("exif");
  // we append a jpg-compatible exif00 string, so get rid of that again:
  if(exif && exif->value().size > 6)
    dt_exif_read_from_blob(img, exif->value().data+6, exif->value().size-6);

  /* Get image width and height */
  Imath::Box2i dw = header->dataWindow();
  uint32_t width = dw.max.x - dw.min.x + 1;
  uint32_t height = dw.max.y - dw.min.y + 1;
  img->width = width;
  img->height = height;


  // Try to allocate image data
  float *buf = (float *)dt_mipmap_cache_alloc(img, DT_MIPMAP_FULL, a);
  if(!buf)
  {
    fprintf(stderr, "[exr_read] could not alloc full buffer for image `%s'\n", img->filename);
    /// \todo open exr cleanup...
    return DT_IMAGEIO_CACHE_FULL;
  }

  /* check channels in image, currently we only support R,G,B */
  const Imf::ChannelList &channels = header->channels();
  if(channels.findChannel("R") && channels.findChannel("G") && channels.findChannel("B"))
  {
    Imf::FrameBuffer frameBuffer;
    frameBuffer.insert ("R",Imf::Slice(Imf::FLOAT,(char *)(buf),sizeof(float)*4,sizeof(float)*width*4,1,1,0.0));
    frameBuffer.insert ("G",Imf::Slice(Imf::FLOAT,(char *)(buf+1),sizeof(float)*4,sizeof(float)*width*4,1,1,0.0));
    frameBuffer.insert ("B",Imf::Slice(Imf::FLOAT,(char *)(buf+2),sizeof(float)*4,sizeof(float)*width*4,1,1,0.0));
    frameBuffer.insert ("A",Imf::Slice(Imf::FLOAT,(char *)(buf+3),sizeof(float)*4,sizeof(float)*width*4,1,1,0.0));

    if(isTiled)
    {
      fileTiled->setFrameBuffer (frameBuffer);
      fileTiled->readTiles (0, fileTiled->numXTiles() - 1, 0, fileTiled->numYTiles() - 1);
    }
    else
    {
      file->setFrameBuffer (frameBuffer);
      file->readPixels(dw.min.y,dw.max.y);
    }
  }

  /* cleanup and return... */
  img->flags |= DT_IMAGE_HDR;

  return DT_IMAGEIO_OK;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
