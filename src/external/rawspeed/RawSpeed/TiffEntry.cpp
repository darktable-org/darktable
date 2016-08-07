#include "StdAfx.h"
#include "TiffEntry.h"
#include <math.h>
/*
    RawSpeed - RAW file decoder.

    Copyright (C) 2009-2014 Klaus Post
    Copyright (C) 2015 Pedro Côrte-Real

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

TiffEntry::TiffEntry() {
  own_data = NULL;
  parent_offset = 0;
  file = NULL;
}

TiffEntry::TiffEntry(FileMap* f, uint32 offset, uint32 up_offset) {
  parent_offset = up_offset;
  own_data = NULL;
  empty_data = 0;
  file = f;
  type = TIFF_UNDEFINED;  // We set type to undefined to avoid debug assertion errors.

  const uchar8 *temp_data = (const uchar8 *)f->getData(offset, 8);
  tag = (TiffTag) get2LE(temp_data, 0);
  type = (TiffDataType) get2LE(temp_data, 2);
  count = get4LE(temp_data,4);

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
    data_offset = get4LE(f->getData(offset+8, 4),0);
    fetchData();
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

void TiffEntry::fetchData() {
  if(file) {
    data = file->getDataWrt(data_offset, bytesize);
  }
}


TiffEntry::TiffEntry(TiffTag _tag, TiffDataType _type, uint32 _count, const uchar8* _data )
{
  file = NULL;
  parent_offset = 0;
  tag = _tag;
  type = _type;
  count = _count;
  data_offset = -1; // Set nonsense value in case someone tries to use it
  bytesize = (size_t)_count << (size_t)datashifts[_type];
  if (NULL == _data) {
    own_data = new uchar8[bytesize];
    memset(own_data,0,bytesize);
    data = own_data;
  } else {
    data = _data; 
    own_data = NULL;
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

TiffEntry::~TiffEntry(void) {
  if (own_data)
    delete[] own_data;
}

bool TiffEntry::isInt() {
  return (type == TIFF_LONG || type == TIFF_SHORT || type ==  TIFF_BYTE);
}

uchar8 TiffEntry::getByte(uint32 num) {
  if (type != TIFF_BYTE)
    ThrowTPE("TIFF, getByte: Wrong type %u encountered. Expected Byte on 0x%x", type, tag);

  if (num >= bytesize)
    ThrowTPE("TIFF, getByte: Trying to read out of bounds");

  return data[num];
}

ushort16 TiffEntry::getShort(uint32 num) {
  if (type != TIFF_SHORT && type != TIFF_UNDEFINED)
    ThrowTPE("TIFF, getShort: Wrong type %u encountered. Expected Short or Undefined on 0x%x", type, tag);

  if (num*2+1 >= bytesize)
    ThrowTPE("TIFF, getShort: Trying to read out of bounds");

  return get2LE(data, num*2);
}

short16 TiffEntry::getSShort(uint32 num) {
  if (type != TIFF_SSHORT && type != TIFF_UNDEFINED)
    ThrowTPE("TIFF, getSShort: Wrong type %u encountered. Expected Short or Undefined on 0x%x", type, tag);

  if (num*2+1 >= bytesize)
    ThrowTPE("TIFF, getSShort: Trying to read out of bounds");

  return (short16) get2LE(data, num*2);
}

uint32 TiffEntry::getInt(uint32 num) {
  if (type == TIFF_SHORT) return getShort(num);
  if (!(type == TIFF_LONG || type == TIFF_OFFSET || type == TIFF_BYTE || type == TIFF_UNDEFINED || type == TIFF_RATIONAL || type == TIFF_SRATIONAL))
    ThrowTPE("TIFF, getInt: Wrong type %u encountered. Expected Long, Offset, Rational or Undefined on 0x%x", type, tag);

  if (num*4+3 >= bytesize)
    ThrowTPE("TIFF, getInt: Trying to read out of bounds");

  return get4LE(data, num*4);
}

int32 TiffEntry::getSInt(uint32 num) {
  if (type == TIFF_SSHORT) return getSShort(num);
  if (!(type == TIFF_SLONG || type == TIFF_UNDEFINED))
    ThrowTPE("TIFF, getSInt: Wrong type %u encountered. Expected SLong or Undefined on 0x%x", type, tag);

  if (num*4+3 >= bytesize)
    ThrowTPE("TIFF, getSInt: Trying to read out of bounds");

  return get4LE(data, num*4);
}

void TiffEntry::getShortArray(ushort16 *array, uint32 num) {
  for (uint32 i = 0; i < num; i++)
    array[i] = getShort(i);
}

void TiffEntry::getIntArray(uint32 *array, uint32 num) {
  for (uint32 i = 0; i < num; i++)
    array[i] = getInt(i);
}

void TiffEntry::getFloatArray(float *array, uint32 num) {
  for (uint32 i = 0; i < num; i++)
    array[i] = getFloat(i);
}

bool TiffEntry::isFloat() {
  return  (type == TIFF_FLOAT || type == TIFF_DOUBLE || type == TIFF_RATIONAL || 
           type == TIFF_SRATIONAL || type == TIFF_LONG || type == TIFF_SLONG || 
           type == TIFF_SHORT || type == TIFF_SSHORT);
}

float TiffEntry::getFloat(uint32 num) {
  if (!isFloat())
    ThrowTPE("TIFF, getFloat: Wrong type 0x%x encountered. Expected Float or something convertible on 0x%x", type, tag);

  if (type == TIFF_DOUBLE) {
    if (num*8+7 >= bytesize)
      ThrowTPE("TIFF, getFloat: Trying to read out of bounds");
    return (float) get8LE(data, num*8);
  } else if (type == TIFF_FLOAT) {
    if (num*4+3 >= bytesize)
      ThrowTPE("TIFF, getFloat: Trying to read out of bounds");
    return (float) get4LE(data, num*4);
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

string TiffEntry::getString() {
  if (type != TIFF_ASCII && type != TIFF_BYTE)
    ThrowTPE("TIFF, getString: Wrong type 0x%x encountered. Expected Ascii or Byte", type);

  if (count == 0)
    return string("");

  if (!own_data) {
    own_data = new uchar8[count];
    memcpy(own_data, data, count);
    own_data[count-1] = 0;  // Ensure string is not larger than count defines
  }
  return string((const char*)&own_data[0]);
}

bool TiffEntry::isString() {
  return (type == TIFF_ASCII);
}

int TiffEntry::getElementSize() {
  return datasizes[type];
}

int TiffEntry::getElementShift() {
  return datashifts[type];
}

void TiffEntry::setData( const void *in_data, uint32 byte_count )
{
  uint32 bytesize = count << datashifts[type];
  if (byte_count > bytesize)
    ThrowTPE("TIFF, data set larger than entry size given");

  if (!own_data) {
    own_data = new uchar8[bytesize];
    memcpy(own_data, data, bytesize);
  }
  memcpy(own_data, in_data, byte_count);
}

uchar8* TiffEntry::getDataWrt()
{
  if (!own_data) {
    own_data = new uchar8[bytesize];
    memcpy(own_data, data, bytesize);
  }
  return own_data;
}

#ifdef _MSC_VER
#pragma warning(disable: 4996) // this function or variable may be unsafe
#endif

std::string TiffEntry::getValueAsString()
{  
  if (type == TIFF_ASCII)
    return string((const char*)&data[0]);
  char *temp_string = new char[4096];
  if (count == 1) {
    switch (type) {
      case TIFF_LONG:
        sprintf(temp_string, "Long: %u (0x%x)", getInt(), getInt());
        break;
      case TIFF_SHORT:
        sprintf(temp_string, "Short: %u (0x%x)", getInt(), getInt());
        break;
      case TIFF_BYTE:
        sprintf(temp_string, "Byte: %u (0x%x)", getInt(), getInt());
        break;
      case TIFF_FLOAT:
        sprintf(temp_string, "Float: %f", getFloat());
        break;
      case TIFF_RATIONAL:
      case TIFF_SRATIONAL:
        sprintf(temp_string, "Rational Number: %f", getFloat());
        break;
      default:
        sprintf(temp_string, "Type: %x: ", type);
        for (uint32 i = 0; i < datasizes[type]; i++) {
          sprintf(&temp_string[strlen(temp_string-1)], "%x", data[i]);
        }
    }
  }
  string ret(temp_string);
  delete [] temp_string;
  return ret;
}

} // namespace RawSpeed
