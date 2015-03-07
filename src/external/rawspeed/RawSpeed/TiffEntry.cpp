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
  file = f;
  unsigned short* p = (unsigned short*)f->getData(offset);
  tag = (TiffTag)p[0];
  type = (TiffDataType)p[1];
  count = *(int*)f->getData(offset + 4);
  if (type > 13)
    ThrowTPE("Error reading TIFF structure. Unknown Type 0x%x encountered.", type);
  uint32 bytesize = count << datashifts[type];
  if (bytesize <= 4) {
    data = f->getDataWrt(offset + 8);
  } else { // offset
    data_offset = *(uint32*)f->getData(offset + 8);
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
  FileMap *f = file; // CHECKSIZE uses f
  if(file) {
    uint32 bytesize = count << datashifts[type];
    CHECKSIZE(data_offset + bytesize);
    data = file->getDataWrt(data_offset);
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
  if (NULL == _data) {
    uint32 bytesize = _count << datashifts[_type];
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

unsigned int TiffEntry::getInt() {
  if (!(type == TIFF_LONG || type == TIFF_SHORT || type == TIFF_BYTE))
    ThrowTPE("TIFF, getInt: Wrong type 0x%x encountered. Expected Long, Short or Byte", type);
  if (type == TIFF_BYTE)
    return getByte();
  if (type == TIFF_SHORT)
    return getShort();
  return (uint32)data[3] << 24 | (uint32)data[2] << 16 | (uint32)data[1] << 8 | (uint32)data[0];
}

unsigned short TiffEntry::getShort() {
  if (type != TIFF_SHORT)
    ThrowTPE("TIFF, getShort: Wrong type 0x%x encountered. Expected Short", type);
  return ((ushort16)data[1] << 8) | (ushort16)data[0];
}

const uint32* TiffEntry::getIntArray() {
  if (type != TIFF_LONG && type != TIFF_SLONG && type != TIFF_RATIONAL && type != TIFF_SRATIONAL && type != TIFF_UNDEFINED )
    ThrowTPE("TIFF, getIntArray: Wrong type 0x%x encountered. Expected Long", type);
  return (uint32*)&data[0];
}

const ushort16* TiffEntry::getShortArray() {
  if (!(type == TIFF_SHORT || type == TIFF_UNDEFINED))
    ThrowTPE("TIFF, getShortArray: Wrong type 0x%x encountered. Expected Short", type);
  return (ushort16*)&data[0];
}

const short16* TiffEntry::getSignedShortArray() {
  if (!(type == TIFF_SSHORT))
    ThrowTPE("TIFF, getShortArray: Wrong type 0x%x encountered. Expected Signed Short", type);
  return (short16*)&data[0];
}

uchar8 TiffEntry::getByte() {
  if (type != TIFF_BYTE)
    ThrowTPE("TIFF, getByte: Wrong type 0x%x encountered. Expected Byte", type);
  return data[0];
}

bool TiffEntry::isFloat() {
  return (type == TIFF_FLOAT || type == TIFF_DOUBLE || type == TIFF_RATIONAL || type == TIFF_SRATIONAL || type == TIFF_LONG || type == TIFF_SHORT);
}

float TiffEntry::getFloat() {
  if (!(type == TIFF_FLOAT || type == TIFF_DOUBLE || type == TIFF_RATIONAL || type == TIFF_SRATIONAL || type == TIFF_LONG || type == TIFF_SHORT))
    ThrowTPE("TIFF, getFloat: Wrong type 0x%x encountered. Expected Float", type);
  if (type == TIFF_DOUBLE) {
    return (float)*(double*)&data[0];
  } else if (type == TIFF_FLOAT) {
    return *(float*)&data[0];
  } else if (type == TIFF_LONG || type == TIFF_SHORT) {
    return (float)getInt();
  } else if (type == TIFF_RATIONAL) {
    const unsigned int* t = getIntArray();
    if (t[1])
      return (float)t[0]/t[1];
  } else if (type == TIFF_SRATIONAL) {
    const int* t = (const int*)getIntArray();
    if (t[1])
      return (float)t[0]/t[1];
  }
  return 0.0f;
}

string TiffEntry::getString() {
  if (type != TIFF_ASCII)
    ThrowTPE("TIFF, getString: Wrong type 0x%x encountered. Expected Ascii", type);
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
    uint32 bytesize = count << datashifts[type];
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
