#include "StdAfx.h"
#include "TiffEntryBE.h"
/*
    RawSpeed - RAW file decoder.

    Copyright (C) 2009-2014 Klaus Post

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

TiffEntryBE::TiffEntryBE(FileMap* f, uint32 offset, uint32 up_offset) {
  own_data = NULL;
  file = f;
  parent_offset = up_offset;
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

TiffEntryBE::TiffEntryBE( TiffTag tag, TiffDataType type, uint32 count, const uchar8* data /*= NULL*/ )
: TiffEntry(tag, type,count, data)
{
  file = NULL;
  parent_offset = 0;
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

const uint32* TiffEntryBE::getIntArray() {
  if (!(type == TIFF_LONG || type == TIFF_SLONG || type == TIFF_UNDEFINED || type == TIFF_RATIONAL ||  type == TIFF_SRATIONAL))
    ThrowTPE("TIFF, getIntArray: Wrong type 0x%x encountered. Expected Int", type);
  if (own_data)
    return (uint32*)own_data;

  uint32 ncount = count * ((type == TIFF_RATIONAL ||  type == TIFF_SRATIONAL) ? 2 : 1);
  own_data = new uchar8[ncount*4];
  uint32* d = (uint32*)own_data;
  for (uint32 i = 0; i < ncount; i++) {
#ifdef LE_PLATFORM_HAS_BSWAP
      d[i] = PLATFORM_BSWAP32(*(uint32*)&data[i*4]);
#else
      d[i] = (unsigned int)data[i*4+0] << 24 | (unsigned int)data[i*4+1] << 16 | (unsigned int)data[i*4+2] << 8 | (unsigned int)data[i*4+3];
#endif
  }
  return (uint32*)own_data;
}

const ushort16* TiffEntryBE::getShortArray() {
  if (!(type == TIFF_SHORT || type == TIFF_UNDEFINED))
    ThrowTPE("TIFF, getShortArray: Wrong type 0x%x encountered. Expected Short", type);

  if (own_data)
    return (unsigned short*)own_data;

  own_data = new uchar8[count*2];
  ushort16* d = (ushort16*)own_data;
  for (uint32 i = 0; i < count; i++) {
    d[i] = (ushort16)data[i*2+0] << 8 | (ushort16)data[i*2+1];
  }
  return d;
}

const short16* TiffEntryBE::getSignedShortArray() {
  if (!(type == TIFF_SSHORT))
    ThrowTPE("TIFF, getShortArray: Wrong type 0x%x encountered. Expected SShort", type);

  if (own_data)
    return (short16 *)own_data;

  own_data = new uchar8[count*2];
  ushort16* d = (ushort16*)own_data;
  for (uint32 i = 0; i < count; i++) {
    d[i] = (ushort16)data[i*2+0] << 8 | (ushort16)data[i*2+1];
  }
  return (short16 *)d;
}

void TiffEntryBE::setData( const void *in_data, uint32 byte_count )
{
  if (datashifts[type] != 0)
    ThrowTPE("TIFF, Unable to set data on byteswapped platforms (unsupported)");
  TiffEntry::setData(in_data, byte_count);
}
} // namespace RawSpeed
