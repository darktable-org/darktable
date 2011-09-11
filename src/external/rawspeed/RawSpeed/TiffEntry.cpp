#include "StdAfx.h"
#include "TiffEntry.h"
#include <math.h>
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

TiffEntry::TiffEntry() {
}

TiffEntry::TiffEntry(FileMap* f, uint32 offset) {
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

TiffEntry::~TiffEntry(void) {
}

bool TiffEntry::isInt() {
  return (type == TIFF_LONG || type == TIFF_SHORT);
}

unsigned int TiffEntry::getInt() {
  if (!(type == TIFF_LONG || type == TIFF_SHORT))
    ThrowTPE("TIFF, getInt: Wrong type 0x%x encountered. Expected Long", type);
  if (type == TIFF_SHORT)
    return getShort();
  return *(unsigned int*)&data[0];
}

unsigned short TiffEntry::getShort() {
  if (type != TIFF_SHORT)
    ThrowTPE("TIFF, getShort: Wrong type 0x%x encountered. Expected Short", type);
  return *(unsigned short*)&data[0];
}

const unsigned int* TiffEntry::getIntArray() {
  if (type != TIFF_LONG && type != TIFF_RATIONAL && type != TIFF_SRATIONAL && type != TIFF_UNDEFINED )
    ThrowTPE("TIFF, getIntArray: Wrong type 0x%x encountered. Expected Long", type);
  return (unsigned int*)&data[0];
}

const unsigned short* TiffEntry::getShortArray() {
  if (type != TIFF_SHORT)
    ThrowTPE("TIFF, getShortArray: Wrong type 0x%x encountered. Expected Short", type);
  return (unsigned short*)&data[0];
}

unsigned char TiffEntry::getByte() {
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
  data[count-1] = 0;  // Ensure string is not larger than count defines
  return string((char*)&data[0]);
}

int TiffEntry::getElementSize() {
  return datasizes[type];
}

int TiffEntry::getElementShift() {
  return datashifts[type];
}

} // namespace RawSpeed
