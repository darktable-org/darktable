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
  parent_offset = up_offset;
  own_data = NULL;
  empty_data = 0;
  file = f;
  type = TIFF_UNDEFINED;  // We set type to undefined to avoid debug assertion errors.

  const uchar8 *temp_data = f->getData(offset, 8);
  tag = (TiffTag) get2BE(temp_data, 0);
  type = (TiffDataType) get2BE(temp_data, 2);
  count = get4BE(temp_data,4);

  if (type > 13)
    ThrowTPE("Error reading TIFF structure. Unknown Type 0x%x encountered.", type);

  bytesize = (uint64)count << datashifts[type];
  if (bytesize > UINT32_MAX)
    ThrowTPE("TIFF entry is supposedly %llu bytes", bytesize);

  if (bytesize == 0) // Better return empty than NULL-dereference later
    data = (uchar8 *) &empty_data;
  else if (bytesize <= 4)
    data = f->getDataWrt(offset + 8, bytesize);
  else { // offset
    data_offset = get4BE(f->getData(offset+8, 4),0);
    data = f->getDataWrt(data_offset, bytesize);
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

ushort16 TiffEntryBE::getShort(uint32 num) {
  if (type == TIFF_BYTE) return getByte(num);
  if (type != TIFF_SHORT && type != TIFF_UNDEFINED)
    ThrowTPE("TIFF, getShort: Wrong type %u encountered. Expected Short or Undefined", type);

  if (num*2+1 >= bytesize)
    ThrowTPE("TIFF, getShort: Trying to read out of bounds");

  return get2BE(data, num*2);
}

uint32 TiffEntryBE::getInt(uint32 num) {
  if (type == TIFF_BYTE) return getByte(num);
  if (type == TIFF_SHORT) return getShort(num);
  if (!(type == TIFF_LONG || type == TIFF_OFFSET || type == TIFF_UNDEFINED))
    ThrowTPE("TIFF, getInt: Wrong type %u encountered. Expected Long, Offset or Undefined", type);

  if (num*4+3 >= bytesize)
    ThrowTPE("TIFF, getInt: Trying to read out of bounds");

  return get4BE(data, num*4);
}

const uint32* TiffEntryBE::getIntArray() {
  if (!(type == TIFF_LONG || type == TIFF_SLONG || type == TIFF_UNDEFINED || type == TIFF_RATIONAL || type == TIFF_SRATIONAL || type == TIFF_OFFSET))
    ThrowTPE("TIFF, getIntArray: Wrong type 0x%x encountered. Expected Int", type);
  if (own_data)
    return (uint32*)own_data;

  uint32 ncount = count * ((type == TIFF_RATIONAL ||  type == TIFF_SRATIONAL) ? 2 : 1);
  if (type == TIFF_UNDEFINED) ncount /= 4;
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

  uint32 ncount = count;
  if (type == TIFF_UNDEFINED) ncount /= 2;
  own_data = new uchar8[ncount*2];
  ushort16* d = (ushort16*)own_data;
  for (uint32 i = 0; i < ncount; i++) {
    d[i] = (ushort16)data[i*2+0] << 8 | (ushort16)data[i*2+1];
  }
  return d;
}

const short16* TiffEntryBE::getSignedShortArray() {
  if (!(type == TIFF_SSHORT))
    ThrowTPE("TIFF, getShortArray: Wrong type 0x%x encountered. Expected SShort", type);

  if (own_data)
    return (short16 *)own_data;

  uint32 ncount = count;
  if (type == TIFF_UNDEFINED) ncount /= 2;
  own_data = new uchar8[ncount*2];
  ushort16* d = (ushort16*)own_data;
  for (uint32 i = 0; i < ncount; i++) {
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
