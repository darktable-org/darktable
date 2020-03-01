/*
    This file is part of darktable,
    Copyright (C) 2012-2020 darktable developers.

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

#pragma once

#include "common/image.h"
#include "common/mipmap_cache.h"

#include <ciso646>

#if defined(_LIBCPP_VERSION)
#include <memory>
#else
#include <tr1/memory>
#endif

#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfInputFile.h>
#include <OpenEXR/ImfStandardAttributes.h>
#include <OpenEXR/ImfTestFile.h>
#include <OpenEXR/ImfTiledInputFile.h>

#ifdef OPENEXR_IMF_INTERNAL_NAMESPACE
#define IMF_NS OPENEXR_IMF_INTERNAL_NAMESPACE
#else
#define IMF_NS Imf
#endif

// this stores our exif data as a blob.

template <typename T> struct array_deleter
{
  void operator()(T const *p)
  {
    delete[] p;
  }
};

namespace IMF_NS
{
class Blob
{
public:
  Blob() : size(0), data((uint8_t *)NULL)
  {
  }

  Blob(uint32_t _size, uint8_t *_data) : size(_size)
  {
    uint8_t *tmp_ptr = new uint8_t[_size];
    memcpy(tmp_ptr, _data, _size);
    data.reset(tmp_ptr, array_deleter<uint8_t>());
  }

  uint32_t size;
#if defined(_LIBCPP_VERSION)
  std::shared_ptr<uint8_t> data;
#else
  std::tr1::shared_ptr<uint8_t> data;
#endif
};


typedef IMF_NS::TypedAttribute<IMF_NS::Blob> BlobAttribute;
template <> const char *BlobAttribute::staticTypeName()
{
  return "blob";
}
template <> void BlobAttribute::writeValueTo(OStream &os, int version) const
{
  Xdr::write<StreamIO>(os, _value.size);
  Xdr::write<StreamIO>(os, (char *)(_value.data.get()), _value.size);
}

template <> void BlobAttribute::readValueFrom(IStream &is, int size, int version)
{
  Xdr::read<StreamIO>(is, _value.size);
  _value.data.reset(new uint8_t[_value.size], array_deleter<uint8_t>());
  Xdr::read<StreamIO>(is, (char *)(_value.data.get()), _value.size);
}
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
