/*
    This file is part of darktable,
    copyright (c) 2012 johannes hanika

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
#ifndef DT_IMAGEIO_EXR_HH
#define DT_IMAGEIO_EXR_HH

#include "common/image.h"
#include "common/mipmap_cache.h"

#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfTestFile.h>
#include <OpenEXR/ImfInputFile.h>
#include <OpenEXR/ImfTiledInputFile.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfStandardAttributes.h>

// this stores our exif data as a blob.
namespace Imf
{
class Blob
{
public:
  Blob()
  : size(0)
  , data(0)
  {}

  ~Blob()
  {
    if(data) free(data);
  }
  uint32_t size;
  uint8_t *data;
};


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
  _value.data = (uint8_t *)malloc(_value.size);
  Xdr::read <StreamIO> (is, (char *)_value.data, _value.size);
}
}

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
