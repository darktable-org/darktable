#include "StdAfx.h"
#include "ColorFilterArray.h"
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

ColorFilterArray::ColorFilterArray(void) {
  setCFA(CFA_UNKNOWN, CFA_UNKNOWN, CFA_UNKNOWN, CFA_UNKNOWN);
}

ColorFilterArray::ColorFilterArray(CFAColor up_left, CFAColor up_right, CFAColor down_left, CFAColor down_right) {
  cfa[0] = up_left;
  cfa[1] = up_right;
  cfa[2] = down_left;
  cfa[3] = down_right;
}

ColorFilterArray::~ColorFilterArray(void) {
}

void ColorFilterArray::setCFA(CFAColor up_left, CFAColor up_right, CFAColor down_left, CFAColor down_right) {
  cfa[0] = up_left;
  cfa[1] = up_right;
  cfa[2] = down_left;
  cfa[3] = down_right;
}

void ColorFilterArray::setCFA(uchar8 dcrawCode) {
  cfa[0] = (CFAColor)(dcrawCode & 0x3);
  cfa[1] = (CFAColor)((dcrawCode >> 2) & 0x3);
  cfa[2] = (CFAColor)((dcrawCode >> 4) & 0x3);
  cfa[3] = (CFAColor)((dcrawCode >> 6) & 0x3);
}

uint32 ColorFilterArray::getDcrawFilter() {
  if (cfa[0] > 3 || cfa[1] > 3 || cfa[2] > 3 || cfa[3] > 3)
    ThrowRDE("getDcrawFilter: Invalid colors defined.");
  uint32 v =  cfa[0] | cfa[1] << 2 | cfa[2] << 4 | cfa[3] << 6;
  return v | (v << 8) | (v << 16) | (v << 24);
}

std::string ColorFilterArray::asString() {
  string s("Upper left:");
  s += colorToString(cfa[0]);
  s.append(" * Upper right:");
  s += colorToString(cfa[1]);
  s += ("\nLower left:");
  s += colorToString(cfa[2]);
  s.append(" * Lower right:");
  s += colorToString(cfa[3]);
  s.append("\n");

  s += string("CFA_") + colorToString(cfa[0]) + string(", CFA_") + colorToString(cfa[1]);
  s += string(", CFA_") + colorToString(cfa[2]) + string(", CFA_") + colorToString(cfa[3]) + string("\n");
  return s;
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
    default:
      return string("UNKNOWN");
  }
}

void ColorFilterArray::setColorAt(iPoint2D pos, CFAColor c) {
  if (pos.x > 1 || pos.x < 0)
    ThrowRDE("ColorFilterArray::SetColor: position out of CFA pattern");
  if (pos.y > 1 || pos.y < 0)
    ThrowRDE("ColorFilterArray::SetColor: position out of CFA pattern");
  cfa[pos.x+pos.y*2] = c;
//  _RPT2(0, "cfa[%u] = %u\n",pos.x+pos.y*2, c);
}

void ColorFilterArray::shiftLeft() {
  CFAColor tmp1 = cfa[0];
  CFAColor tmp2 = cfa[2];
  cfa[0] = cfa[1];
  cfa[2] = cfa[3];
  cfa[1] = tmp1;
  cfa[3] = tmp2;
}

void ColorFilterArray::shiftDown() {
  CFAColor tmp1 = cfa[0];
  CFAColor tmp2 = cfa[1];
  cfa[0] = cfa[2];
  cfa[1] = cfa[3];
  cfa[2] = tmp1;
  cfa[3] = tmp2;
}

} // namespace RawSpeed
