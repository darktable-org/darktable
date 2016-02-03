#include "StdAfx.h"
#include "ColorFilterArray.h"
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

ColorFilterArray::ColorFilterArray( iPoint2D _size)
{
  cfa = NULL;
  setSize(_size);
}

ColorFilterArray::ColorFilterArray() :
size(0,0) 
{
  cfa = NULL; 
}


ColorFilterArray::ColorFilterArray( const ColorFilterArray& other )
{
  cfa = NULL;
  setSize(other.size);
  if (cfa)
    memcpy(cfa, other.cfa, size.area()*sizeof(CFAColor));
}

// FC macro from dcraw outputs, given the filters definition, the dcraw color
// number for that given position in the CFA pattern
#define FC(filters,row,col) ((filters) >> ((((row) << 1 & 14) + ((col) & 1)) << 1) & 3)
ColorFilterArray::ColorFilterArray( const uint32 filters) :
size(8,2)
{
  cfa = NULL;
  setSize(size);

  for (int x = 0; x < 8; x++) {
    for (int y = 0; y < 2; y++) {
      CFAColor c = toRawspeedColor(FC(filters,y,x));
      setColorAt(iPoint2D(x,y), c);
    }
  }
}

ColorFilterArray& ColorFilterArray::operator=(const ColorFilterArray& other ) 
{
  setSize(other.size);
  if (cfa)
    memcpy(cfa, other.cfa, size.area()*sizeof(CFAColor));
  return *this;
}

void ColorFilterArray::setSize( iPoint2D _size )
{
  size = _size;
  if (cfa)
    delete[] cfa;
  cfa = NULL;
  if (size.area() <= 0)
    return;
  cfa = new CFAColor[size.area()];
  if (!cfa)
    ThrowRDE("ColorFilterArray:setSize Unable to allocate memory");
  memset(cfa, CFA_UNKNOWN, size.area()*sizeof(CFAColor));
}

ColorFilterArray::~ColorFilterArray( void )
{
  if (cfa)
    delete[] cfa;
  cfa = NULL;
}

CFAColor ColorFilterArray::getColorAt( uint32 x, uint32 y )
{
  if (!cfa)
    ThrowRDE("ColorFilterArray:getColorAt: No CFA size set");
  if (x >= (uint32)size.x || y >= (uint32)size.y) {
    x = x%size.x;
    y = y%size.y;
  }
  return cfa[x+y*size.x];
}

void ColorFilterArray::setCFA( iPoint2D in_size, ... )
{
  if (in_size != size) {
    setSize(in_size);
  }
  va_list arguments;
  va_start(arguments, in_size);
  for (uint32 i = 0; i <  size.area(); i++ ) {
    cfa[i] = (CFAColor)va_arg(arguments, int);
  }
  va_end (arguments);   
}

void ColorFilterArray::shiftLeft(int n) {
  if (!size.x) {
    ThrowRDE("ColorFilterArray:shiftLeft: No CFA size set (or set to zero)");
  }
  writeLog(DEBUG_PRIO_EXTRA, "Shift left:%d\n", n);
  int shift = n % size.x;
  if (0 == shift)
    return;
  CFAColor* tmp = new CFAColor[size.x];
  for (int y = 0; y < size.y; y++) {
    CFAColor *old = &cfa[y*size.x];
    memcpy(tmp, &old[shift], (size.x-shift)*sizeof(CFAColor));
    memcpy(&tmp[size.x-shift], old, shift*sizeof(CFAColor));
    memcpy(old, tmp, size.x * sizeof(CFAColor));
  }
  delete[] tmp;
}

void ColorFilterArray::shiftDown(int n) {
  if (!size.y) {
    ThrowRDE("ColorFilterArray:shiftDown: No CFA size set (or set to zero)");
  }
  writeLog(DEBUG_PRIO_EXTRA, "Shift down:%d\n", n);
  int shift = n % size.y;
  if (0 == shift)
    return;
  CFAColor* tmp = new CFAColor[size.y];
  for (int x = 0; x < size.x; x++) {
    CFAColor *old = &cfa[x];
    for (int y = 0; y < size.y; y++)
      tmp[y] = old[((y+shift)%size.y)*size.x];
    for (int y = 0; y < size.y; y++)
      old[y*size.x] = tmp[y];
  }
  delete[] tmp;
}

std::string ColorFilterArray::asString() {
  string dst = string("");
  for (int y = 0; y < size.y; y++) {
    for (int x = 0; x < size.x; x++) {
      dst += colorToString(getColorAt(x,y));
      dst += (x == size.x - 1) ? "\n" : ",";
    }
  }
  return dst;
}


std::string ColorFilterArray::colorToString(CFAColor c) {
  switch (c) {
    case CFA_RED:
      return string("RED");
    case CFA_GREEN:
      return string("GREEN");
    case CFA_BLUE:
      return string("BLUE");
    case CFA_GREEN2:
      return string("GREEN2");
    case CFA_CYAN:
      return string("CYAN");
    case CFA_MAGENTA:
      return string("MAGENTA");
    case CFA_YELLOW:
      return string("YELLOW");
    case CFA_WHITE:
      return string("WHITE");
    case CFA_FUJI_GREEN:
      return string("FUJIGREEN");
    default:
      return string("UNKNOWN");
  }
}


void ColorFilterArray::setColorAt(iPoint2D pos, CFAColor c) {
  if (pos.x >= size.x || pos.x < 0)
    ThrowRDE("ColorFilterArray::SetColor: position out of CFA pattern");
  if (pos.y >= size.y || pos.y < 0)
    ThrowRDE("ColorFilterArray::SetColor: position out of CFA pattern");
  cfa[pos.x+pos.y*size.x] = c;
}

RawSpeed::uint32 ColorFilterArray::getDcrawFilter()
{
  //dcraw magic
  if (size.x == 6 && size.y == 6) 
    return 9;

  if (size.x > 8 || size.y > 2 || 0 == cfa)
    return 1;

  if (!isPowerOfTwo(size.x))
    return 1;

  uint32 ret = 0;
  for (int x = 0; x < 8; x++) {
    for (int y = 0; y < 2; y++) {
      uint32 c = toDcrawColor(getColorAt(x,y));
      int g = (x >> 1) * 8;
      ret |= c << ((x&1)*2 + y*4 + g);
    }
  }
  for (int y = 0; y < size.y; y++) {
    for (int x = 0; x < size.x; x++) {
      writeLog(DEBUG_PRIO_EXTRA, "%s,", colorToString((CFAColor)toDcrawColor(getColorAt(x,y))).c_str());
    }
    writeLog(DEBUG_PRIO_EXTRA, "\n");
  }
  writeLog(DEBUG_PRIO_EXTRA, "DCRAW filter:%x\n",ret);
  return ret;
}

CFAColor ColorFilterArray::toRawspeedColor( uint32 dcrawColor )
{
  switch (dcrawColor) {
    case 0: return CFA_RED;
    case 1: return CFA_GREEN;
    case 2: return CFA_BLUE;
    case 3: return CFA_GREEN2;
  }
  return CFA_UNKNOWN;
}

RawSpeed::uint32 ColorFilterArray::toDcrawColor( CFAColor c )
{
  switch (c) {
    case CFA_FUJI_GREEN:
    case CFA_RED: return 0;
    case CFA_MAGENTA:
    case CFA_GREEN: return 1;
    case CFA_CYAN:
    case CFA_BLUE: return 2;
    case CFA_YELLOW:
    case CFA_GREEN2: return 3;
    default:
      break;
  }
  return 0;
}


} // namespace RawSpeed
