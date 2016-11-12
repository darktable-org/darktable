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
  const ushort16 numType = (TiffDataType) get2BE(temp_data, 2);
  count = get4BE(temp_data,4);

  if (numType > 13)
    ThrowTPE("Error reading TIFF structure. Unknown Type 0x%x encountered.", numType);

  type = (TiffDataType) numType;

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
    ThrowTPE("TIFF, getShort: Wrong type %u encountered. Expected Short or Undefined on 0x%x", type, tag);

  if (num*2+1 >= bytesize)
    ThrowTPE("TIFF, getShort: Trying to read out of bounds");

  return get2BE(data, num*2);
}

short16 TiffEntryBE::getSShort(uint32 num) {
  if (type != TIFF_SSHORT && type != TIFF_UNDEFINED)
    ThrowTPE("TIFF, getSShort: Wrong type %u encountered. Expected Short or Undefined on 0x%x", type, tag);

  if (num*2+1 >= bytesize)
    ThrowTPE("TIFF, getSShort: Trying to read out of bounds");

  return (short16) get2LE(data, num*2);
}

uint32 TiffEntryBE::getInt(uint32 num) {
  if (type == TIFF_SHORT) return getShort(num);
  if (!(type == TIFF_LONG || type == TIFF_OFFSET || type == TIFF_BYTE || type == TIFF_UNDEFINED || type == TIFF_RATIONAL || type == TIFF_SRATIONAL))
    ThrowTPE("TIFF, getInt: Wrong type %u encountered. Expected Long, Offset or Undefined on 0x%x", type, tag);

  if (num*4+3 >= bytesize)
    ThrowTPE("TIFF, getInt: Trying to read out of bounds");

  return get4BE(data, num*4);
}

int32 TiffEntryBE::getSInt(uint32 num) {
  if (type == TIFF_SSHORT) return getSShort(num);
  if (!(type == TIFF_SLONG || type == TIFF_UNDEFINED))
    ThrowTPE("TIFF, getSInt: Wrong type %u encountered. Expected SLong or Undefined on 0x%x", type, tag);

  if (num*4+3 >= bytesize)
    ThrowTPE("TIFF, getSInt: Trying to read out of bounds");

  return get4BE(data, num*4);
}

float TiffEntryBE::getFloat(uint32 num) {
  if (!isFloat())
    ThrowTPE("TIFF, getFloat: Wrong type 0x%x encountered. Expected Float or something convertible on 0x%x", type, tag);

  if (type == TIFF_DOUBLE) {
    if (num*8+7 >= bytesize)
      ThrowTPE("TIFF, getFloat: Trying to read out of bounds");
    return (float) get8BE(data, num*8);
  } else if (type == TIFF_FLOAT) {
    if (num*4+3 >= bytesize)
      ThrowTPE("TIFF, getFloat: Trying to read out of bounds");
    return (float) get4BE(data, num*4);
  } else if (type == TIFF_LONG || type == TIFF_SHORT) {
    return (float)getInt(num);
  } else if (type == TIFF_SLONG || type == TIFF_SSHORT) {
    return (float)getSInt(num);
  } else if (type == TIFF_RATIONAL) {
    uint32 a = getInt(num*2);
    uint32 b = getInt(num*2+1);
    if (b)
      return (float) a/b;
  } else if (type == TIFF_SRATIONAL) {
    int a = (int) getInt(num*2);
    int b = (int) getInt(num*2+1);
    if (b)
      return (float) a/b;
  }
  return 0.0f;
}

void TiffEntryBE::setData( const void *in_data, uint32 byte_count )
{
  if (datashifts[type] != 0)
    ThrowTPE("TIFF, Unable to set data on byteswapped platforms (unsupported)");
  TiffEntry::setData(in_data, byte_count);
}
} // namespace RawSpeed
