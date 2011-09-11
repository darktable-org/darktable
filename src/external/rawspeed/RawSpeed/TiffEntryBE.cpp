#include "StdAfx.h"
#include "TiffEntryBE.h"
/*
    RawSpeed - RAW file decoder.

    Copyright (C) 2009 Klaus Post

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

    http://www.klauspost.com
*/

namespace RawSpeed {

TiffEntryBE::TiffEntryBE(FileMap* f, uint32 offset) : mDataSwapped(false) {
  type = TIFF_UNDEFINED;  // We set type to undefined to avoid debug assertion errors.
  data = f->getDataWrt(offset);
  tag = (TiffTag)getShort();
  data += 2;
  TiffDataType _type = (TiffDataType)getShort();
  data += 2;
  count = getInt();
  type = _type;         //Now we can set it to the proper type

  if (type > 13)
    ThrowTPE("Error reading TIFF structure. Unknown Type 0x%x encountered.", type);
  uint32 bytesize = count << datashifts[type];
  if (bytesize <= 4) {
    data = f->getDataWrt(offset + 8);
  } else { // offset
    data = f->getDataWrt(offset + 8);
    data_offset = (unsigned int)data[0] << 24 | (unsigned int)data[1] << 16 | (unsigned int)data[2] << 8 | (unsigned int)data[3];
    CHECKSIZE(data_offset + bytesize);
    data = f->getDataWrt(data_offset);
  }
#ifdef _DEBUG
  debug_intVal = 0xC0CAC01A;
  debug_floatVal = sqrtf(-1);

  if (type == TIFF_LONG || type == TIFF_SHORT)
    debug_intVal = getInt();
  if (type == TIFF_FLOAT || type == TIFF_DOUBLE)
    debug_floatVal = getFloat();
#endif
}

TiffEntryBE::~TiffEntryBE(void) {
}

unsigned int TiffEntryBE::getInt() {
  if (!(type == TIFF_LONG || type == TIFF_SHORT || type == TIFF_UNDEFINED))
    ThrowTPE("TIFF, getInt: Wrong type 0x%x encountered. Expected Int", type);
  if (type == TIFF_SHORT)
    return getShort();
  return (unsigned int)data[0] << 24 | (unsigned int)data[1] << 16 | (unsigned int)data[2] << 8 | (unsigned int)data[3];
}

unsigned short TiffEntryBE::getShort() {
  if (!(type == TIFF_SHORT || type == TIFF_UNDEFINED))
    ThrowTPE("TIFF, getShort: Wrong type 0x%x encountered. Expected Short", type);
  return (unsigned short)data[0] << 8 | (unsigned short)data[1];
}

const unsigned int* TiffEntryBE::getIntArray() {
  //TODO: Make critical section to avoid clashes.
  if (!(type == TIFF_LONG || type == TIFF_UNDEFINED || type == TIFF_RATIONAL ||  type == TIFF_SRATIONAL))
    ThrowTPE("TIFF, getIntArray: Wrong type 0x%x encountered. Expected Int", type);
  if (mDataSwapped)
    return (unsigned int*)&data[0];

  unsigned int* d = (unsigned int*) & data[0];
  for (uint32 i = 0; i < count; i++) {
    d[i] = (unsigned int)data[i*4+0] << 24 | (unsigned int)data[i*4+1] << 16 | (unsigned int)data[i*4+2] << 8 | (unsigned int)data[i*4+3];
  }
  mDataSwapped = true;
  return d;
}

const unsigned short* TiffEntryBE::getShortArray() {
  //TODO: Make critical section to avoid clashes.
  if (!(type == TIFF_SHORT || type == TIFF_UNDEFINED))
    ThrowTPE("TIFF, getShortArray: Wrong type 0x%x encountered. Expected Short", type);

  if (mDataSwapped)
    return (unsigned short*)&data[0];

  unsigned short* d = (unsigned short*) & data[0];
  for (uint32 i = 0; i < count; i++) {
    d[i] = (unsigned short)data[i*2+0] << 8 | (unsigned short)data[i*2+1];
  }
  mDataSwapped = true;
  return d;
}

} // namespace RawSpeed
