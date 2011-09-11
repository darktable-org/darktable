#ifndef COLOR_FILTER_ARRAY_H
#define COLOR_FILTER_ARRAY_H

#include "RawDecoderException.h"
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

typedef enum {
  CFA_COLOR_MIN = 0,
  CFA_RED = 0,
  CFA_GREEN = 1,
  CFA_BLUE = 2,
  CFA_GREEN2 = 3,
  CFA_CYAN = 4,
  CFA_MAGENTA = 5,
  CFA_YELLOW = 6,
  CFA_WHITE = 7,
  CFA_COLOR_MAX = 8,
  CFA_UNKNOWN = 255
} CFAColor;

typedef enum {
  CFA_POS_UPPERLEFT,
  CFA_POS_UPPERRIGHT,
  CFA_POS_LOWERLEFT,
  CFA_POS_LOWERRIGHT
} CFAPos;

class ColorFilterArray
{
public:
  ColorFilterArray(void);
  ColorFilterArray(CFAColor up_left, CFAColor up_right, CFAColor down_left, CFAColor down_right);
  virtual ~ColorFilterArray(void);
  void setCFA(CFAColor up_left, CFAColor up_right, CFAColor down_left, CFAColor down_right);
  void setColorAt(iPoint2D pos, CFAColor c);
  void setCFA(uchar8 dcrawCode);
  __inline CFAColor getColorAt(uint32 x, uint32 y) {return cfa[(x&1)+((y&1)<<1)];}
  uint32 toDcrawColor(CFAColor c);
  uint32 getDcrawFilter();
  void shiftLeft();
  void shiftDown();
  std::string asString();
  static std::string colorToString(CFAColor c);
private:
  CFAColor cfa[4];
};

} // namespace RawSpeed

#endif
