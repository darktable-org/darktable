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

#ifndef SS_Point_H
#define SS_Point_H

#ifdef __unix
#include <stdlib.h>
#endif

class iPoint2D {
public:
	iPoint2D() {x = y = 0;  }
	iPoint2D( int a, int b) {x=a; y=b;}
  iPoint2D( const iPoint2D& pt) {x=pt.x; y=pt.y;}
  iPoint2D operator += (const iPoint2D& other) { x += other.x; y += other.y; return *this;}
  iPoint2D operator -= (const iPoint2D& other) { x -= other.x; y -= other.y; return *this;}
  iPoint2D operator - (const iPoint2D& b) { return iPoint2D(x-b.x,y-b.y); }
  iPoint2D operator + (const iPoint2D& b) { return iPoint2D(x+b.x,y+b.y); }
  iPoint2D operator = (const iPoint2D& b) { x = b.x; y = b.y; return *this;}
	~iPoint2D() {};
  uint32 area() {return abs(x*y);}
  bool isThisInside(const iPoint2D &otherPoint) {return (x<=otherPoint.x && y<=otherPoint.y); };
  int x,y;
};

} // namespace RawSpeed

#endif // SS_Point_H
