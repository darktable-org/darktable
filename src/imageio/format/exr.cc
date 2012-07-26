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

#include <stdlib.h>
#include <stdio.h>
#include <memory.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfTiledOutputFile.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfStandardAttributes.h>
#include "common/darktable.h"
#include "common/exif.h"
#include "common/colorspaces.h"
#include "common/imageio_module.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/** Special BLOB attribute implementation.*/
namespace Imf
{
typedef struct Blob
{
  uint32_t size;
  uint8_t *data;
} Blob;
typedef Imf::TypedAttribute<Imf::Blob> BlobAttribute;
template <> const char *BlobAttribute::staticTypeName()
{
  return "blob";
}
template <> void BlobAttribute::writeValueTo (OStream &os, int version) const
{
  Xdr::write <StreamIO> (os, _value.size);
  Xdr::write <StreamIO> (os, (char *)_value.data,_value.size);
}

template <> void BlobAttribute::readValueFrom (IStream &is, int size, int version)
{
  Xdr::read <StreamIO> (is, _value.size);
  Xdr::read <StreamIO> (is, (char *)_value.data, _value.size);
}
}

#ifdef __cplusplus
extern "C"
{
#endif

  DT_MODULE(1)

  typedef struct dt_imageio_exr_t
  {
    int max_width, max_height;
    int width, height;
  }
  dt_imageio_exr_t;
 
  void init(dt_imageio_module_format_t *self) 
  {
    Imf::BlobAttribute::registerAttributeType();
  }

  void cleanup(dt_imageio_module_format_t *self) {}

  int write_image (dt_imageio_exr_t *exr, const char *filename, const float *in, void *exif, int exif_len, int imgid)
  {
    Imf::Blob exif_blob= {0};
    exif_blob.size=exif_len;
    exif_blob.data=(uint8_t *)exif;
    Imf::Header header(exr->width,exr->height,1,Imath::V2f (0, 0),1,Imf::INCREASING_Y,Imf::PIZ_COMPRESSION);
    header.insert("comment",Imf::StringAttribute("Developed using Darktable "PACKAGE_VERSION));
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

  void*
  get_params(dt_imageio_module_format_t *self, int *size)
  {
    *size = sizeof(dt_imageio_module_data_t);
    dt_imageio_exr_t *d = (dt_imageio_exr_t *)malloc(sizeof(dt_imageio_exr_t));
    return d;
  }

  void
  free_params(dt_imageio_module_format_t *self, void *params)
  {
    free(params);
  }

  int
  set_params(dt_imageio_module_format_t *self, void *params, int size)
  {
    if(size != sizeof(dt_imageio_module_data_t)) return 1;
    return 0;
  }

  int bpp(dt_imageio_module_data_t *p)
  {
    return 32;
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
    return _("openexr");
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
