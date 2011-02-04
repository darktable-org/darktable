#pragma once
#include "ColorFilterArray.h"
#include "BlackArea.h"

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

class RawImage;
class RawImageData
{

public:
  virtual ~RawImageData(void);
  iPoint2D dim;
  uint32 bpp;      // Bytes per pixel.
  uint32 getCpp() const { return cpp; }
  void setCpp(uint32 val);
  uint32 pitch;
  virtual void createData();
  virtual void destroyData();
  uchar8* getData();
  uchar8* getData(uint32 x, uint32 y);    // Not super fast, but safe. Don't use per pixel.
  uchar8* getDataUncropped(uint32 x, uint32 y);
  virtual void subFrame( iPoint2D offset, iPoint2D new_size );
  void scaleBlackWhite();
  bool isCFA;
  ColorFilterArray cfa;
  int blackLevel;
  int blackLevelSeparate[4];
  int whitePoint;
  vector<BlackArea> blackAreas;
  iPoint2D subsampling;
  bool isAllocated() {return !!data;}
  void scaleValues();
protected:
  RawImageData(void);
  RawImageData(iPoint2D dim, uint32 bpp, uint32 cpp=1);
  void calculateBlackAreas();
  uint32 dataRefCount;
  uchar8* data;
  uint32 cpp;      // Components per pixel
  friend class RawImage;
  pthread_mutex_t mymutex;
  iPoint2D mOffset;
};


 class RawImage {
 public:
   static RawImage create();
   static RawImage create(iPoint2D dim, uint32 bytesPerComponent, uint32 componentsPerPixel = 1);
   RawImageData* operator-> ();
   RawImageData& operator* ();
   RawImage(RawImageData* p);  // p must not be NULL
  ~RawImage();
   RawImage(const RawImage& p);
   RawImage& operator= (const RawImage& p);

 private:
   RawImageData* p_;    // p_ is never NULL
 };

inline RawImage RawImage::create()  { return new RawImageData(); }
inline RawImage RawImage::create(iPoint2D dim, uint32 bytesPerPixel, uint32 componentsPerPixel)
{ return new RawImageData(dim, bytesPerPixel, componentsPerPixel); }

} // namespace RawSpeed
