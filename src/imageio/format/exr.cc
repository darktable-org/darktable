/*
   This file is part of darktable,
   copyright (c) 2010-2011 Henrik Andersson.

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

#include <cstdlib>
#include <cstdio>
#include <memory>

#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfTiledOutputFile.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfStandardAttributes.h>

extern "C"
{
#include "common/darktable.h"
#include "common/exif.h"
#include "common/colorspaces.h"
#include "common/imageio.h"
#include "common/imageio_module.h"
#include "common/imageio_exr.h"
#include "common/imageio_format.h"
}
#include "common/imageio_exr.hh"

#ifdef __cplusplus
extern "C"
{
#endif

  DT_MODULE(1)

    typedef struct dt_imageio_exr_t
    {
      int max_width, max_height;
      int width, height;
      char style[128];
    }
  dt_imageio_exr_t;

  void init(dt_imageio_module_format_t *self)
  {
    Imf::BlobAttribute::registerAttributeType();
  }

  void cleanup(dt_imageio_module_format_t *self) {}

  int write_image (dt_imageio_module_data_t *tmp, const char *filename, const void *in_tmp, void *exif, int exif_len, int imgid)
  {
    dt_imageio_exr_t * exr = (dt_imageio_exr_t*) tmp;
    const float * in = (const float *) in_tmp;
    Imf::Blob exif_blob(exif_len, (uint8_t*)exif);
    Imf::Header header(exr->width,exr->height,1,Imath::V2f (0, 0),1,Imf::INCREASING_Y,Imf::PIZ_COMPRESSION);
    header.insert("comment",Imf::StringAttribute("Developed using Darktable " PACKAGE_VERSION));
    header.insert("exif", Imf::BlobAttribute(exif_blob));
    header.channels().insert("R",Imf::Channel(Imf::FLOAT));
    header.channels().insert("B",Imf::Channel(Imf::FLOAT));
    header.channels().insert("G",Imf::Channel(Imf::FLOAT));
    header.setTileDescription(Imf::TileDescription(100, 100, Imf::ONE_LEVEL));
    Imf::TiledOutputFile file(filename, header);
    Imf::FrameBuffer data;

    uint32_t channelsize=(exr->width*exr->height);
    float *red=(float *)malloc(channelsize*sizeof(float));
    float *green=(float *)malloc(channelsize*sizeof(float));
    float *blue=(float *)malloc(channelsize*sizeof(float));

    for(uint32_t j=0; j<channelsize; j++)
    {
      red[j]=in[j*4+0];
    }
    data.insert("R",Imf::Slice(Imf::FLOAT,(char *)red,sizeof(float)*1,sizeof(float)*exr->width));

    for(uint32_t j=0; j<channelsize; j++)
    {
      blue[j]=in[j*4+2];
    }
    data.insert("B",Imf::Slice(Imf::FLOAT,(char *)blue,sizeof(float)*1,sizeof(float)*exr->width));

    for(uint32_t j=0; j<channelsize; j++)
    {
      green[j]=in[j*4+1];
    }
    data.insert("G",Imf::Slice(Imf::FLOAT,(char *)green,sizeof(float)*1,sizeof(float)*exr->width));

    file.setFrameBuffer(data);
    file.writeTiles (0, file.numXTiles() - 1, 0, file.numYTiles() - 1);
    free(red);
    free(green);
    free(blue);
    return 0;
  }

  size_t
    params_size(dt_imageio_module_format_t *self)
    {
      return sizeof(dt_imageio_module_data_t);
    }

  void*
    get_params(dt_imageio_module_format_t *self)
    {
      dt_imageio_exr_t *d = (dt_imageio_exr_t *)malloc(sizeof(dt_imageio_exr_t));
      memset(d,0,sizeof(dt_imageio_exr_t));
      return d;
    }

  void
    free_params(dt_imageio_module_format_t *self, dt_imageio_module_data_t *params)
    {
      free(params);
    }

  int
    set_params(dt_imageio_module_format_t *self, const void *params, const int size)
    {
      if(size != (int)self->params_size(self)) return 1;
      return 0;
    }

  int bpp(dt_imageio_module_data_t *p)
  {
    return 32;
  }

  int levels(dt_imageio_module_data_t *p)
  {
    return IMAGEIO_RGB|IMAGEIO_FLOAT;
  }

  const char*
    mime(dt_imageio_module_data_t *data)
    {
      return "image/openexr";
    }

  const char*
    extension(dt_imageio_module_data_t *data)
    {
      return "exr";
    }

  const char*
    name ()
    {
      return _("OpenEXR (float)");
    }

  // TODO: some quality/compression stuff?
  void gui_init    (dt_imageio_module_format_t *self) {}
  void gui_cleanup (dt_imageio_module_format_t *self) {}
  void gui_reset   (dt_imageio_module_format_t *self) {}



#ifdef __cplusplus
}
#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
