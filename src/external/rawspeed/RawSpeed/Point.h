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
#ifndef SS_Point_H
#define SS_Point_H

namespace RawSpeed {

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
  iPoint2D operator - (const iPoint2D& b) const { return iPoint2D(x-b.x,y-b.y); }
  iPoint2D operator + (const iPoint2D& b) const { return iPoint2D(x+b.x,y+b.y); }
  iPoint2D operator = (const iPoint2D& b) { x = b.x; y = b.y; return *this;}
	~iPoint2D() {};
  uint32 area() const {return abs(x*y);}
  bool isThisInside(const iPoint2D &otherPoint) const {return (x<=otherPoint.x && y<=otherPoint.y); };
  iPoint2D getSmallest(const iPoint2D &otherPoint) const { return iPoint2D(min(x, otherPoint.x), min(y, otherPoint.y)); };
  int x,y;
};

/* Helper class for managing a rectangle in 2D space. */
class iRectangle2D {
public:
  iRectangle2D() {}
  iRectangle2D( int w, int h) {dim = iPoint2D(w,h);}
  iRectangle2D( int x_pos, int y_pos, int w, int h) {dim = iPoint2D(w,h); pos=iPoint2D(x_pos, y_pos);}
  iRectangle2D( const iRectangle2D& r) {dim = iPoint2D(r.dim); pos = iPoint2D(r.pos);}
  iRectangle2D( const iPoint2D& _pos, const iPoint2D& size) {dim = size; pos=_pos;}
  iRectangle2D operator = (const iRectangle2D& b) {dim = iPoint2D(b.dim); pos = iPoint2D(b.pos); return *this;}
  ~iRectangle2D() {};
  uint32 area() const {return dim.area();}
  void offset(const iPoint2D& offset) {pos+=offset;}
  bool isThisInside(const iRectangle2D &otherPoint) const {
    iPoint2D br1 = getBottomRight();
    iPoint2D br2 = otherPoint.getBottomRight();
    return pos.x >= otherPoint.pos.x && pos.y >= otherPoint.pos.y && br1.x <= br2.x && br1.y <= br2.y;
  }
  bool isPointInside(const iPoint2D &checkPoint) const {
    iPoint2D br1 = getBottomRight();
    return pos.x <= checkPoint.x && pos.y <= checkPoint.y && br1.x >= checkPoint.x && br1.y >= checkPoint.y;
  }
  int getTop() const {return pos.y; }
  int getBottom() const {return pos.y+dim.y; }
  int getLeft() const {return pos.x; }
  int getRight() const {return pos.x+dim.x; }
  int getWidth() const {return dim.x; }
  int getHeight() const {return dim.y; }
  iPoint2D getTopLeft() const {return pos; }
  iPoint2D getBottomRight() const {return dim + pos;}
  /* Retains size */
  void setTopLeft(const iPoint2D& top_left) {pos = top_left;}
  /* Set BR  */
  void setBottomRightAbsolute(const iPoint2D& bottom_right) {dim = iPoint2D(bottom_right) - pos;};
  void setAbsolute(int x1, int y1, int x2, int y2) {pos = iPoint2D(x1,y1); dim = iPoint2D(x2-x1,y2-y1);};
  void setAbsolute(const iPoint2D& top_left, const iPoint2D& bottom_right) {pos = top_left; setBottomRightAbsolute(bottom_right);};
  void setSize(const iPoint2D& size) {dim = size;};
  bool hasPositiveArea() const {return (dim.x > 0) && (dim.y > 0);};
  /* Crop, so area is postitive, and return true, if there is any area left */
  /* This will ensure that bottomright is never on the left/top of the offset */
  bool cropArea(){ dim.x = max(0,dim.x); dim.y = max(0, dim.y); return hasPositiveArea();};
  /* This will make sure that offset is positive, and make the area smaller if needed */
  /* This will return true if there is any area left */
  bool cropOffsetToZero(){ 
    iPoint2D crop_pixels; 
    if (pos.x < 0) {
      crop_pixels.x = -(pos.x);
      pos.x = 0;
    }
    if (pos.y < 0) {
      crop_pixels.y = -pos.y;
      pos.y = 0;
    }
    dim -= crop_pixels;
    return cropArea();
  };
  iRectangle2D getOverlap(const iRectangle2D& other) const {
    iRectangle2D overlap;
    iPoint2D br1 = getBottomRight();
    iPoint2D br2 = other.getBottomRight();
    overlap.setAbsolute(max(pos.x, other.pos.x), max(pos.y, other.pos.y), min(br1.x, br2.x), min(br1.y, br2.y));
    return overlap;
  };
  iRectangle2D combine(const iRectangle2D& other) const {
    iRectangle2D combined;
		iPoint2D br1 = getBottomRight();
		iPoint2D br2 = other.getBottomRight();
		combined.setAbsolute(min(pos.x, other.pos.x), min(pos.y, other.pos.y), max(br1.x, br2.x), max(br2.y, br2.y));
		return combined;
  };
  iPoint2D pos;
  iPoint2D dim;
};

} // namespace RawSpeed

#endif // SS_Point_H
