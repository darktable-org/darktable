#ifndef RAW_IMAGE_H
#define RAW_IMAGE_H

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
class RawImageWorker;
typedef enum {TYPE_USHORT16, TYPE_FLOAT32} RawImageType;

class RawImageData
{
  friend class RawImageWorker;
public:
  virtual ~RawImageData(void);
  uint32 getCpp() const { return cpp; }
  uint32 getBpp() const { return bpp; }
  void setCpp(uint32 val);
  virtual void createData();
  virtual void destroyData();
  RawSpeed::RawImageType getDataType() const { return dataType; }
  uchar8* getData();
  uchar8* getData(uint32 x, uint32 y);    // Not super fast, but safe. Don't use per pixel.
  uchar8* getDataUncropped(uint32 x, uint32 y);
  virtual void subFrame( iPoint2D offset, iPoint2D new_size );
  iPoint2D getUncroppedDim();
  iPoint2D getCropOffset();
  virtual void scaleBlackWhite() = 0;
  bool isAllocated() {return !!data;}
  iPoint2D dim;
  uint32 pitch;
  bool isCFA;
  ColorFilterArray cfa;
  int blackLevel;
  int blackLevelSeparate[4];
  int whitePoint;
  vector<BlackArea> blackAreas;
  iPoint2D subsampling;
protected:
  RawImageType dataType;
  RawImageData(void);
  RawImageData(iPoint2D dim, uint32 bpp, uint32 cpp=1);
  virtual void calculateBlackAreas() = 0;
  virtual void scaleValues(int start_y, int end_y) = 0;
  uint32 dataRefCount;
  uchar8* data;
  uint32 cpp;      // Components per pixel
  uint32 bpp;      // Bytes per pixel.
  friend class RawImage;
  pthread_mutex_t mymutex;
  iPoint2D mOffset;
  iPoint2D uncropped_dim;
};

class RawImageDataU16 : public RawImageData
{
public:
  virtual void scaleBlackWhite();

protected:
  virtual void calculateBlackAreas();
  virtual void scaleValues(int start_y, int end_y);
  RawImageDataU16(void);
  RawImageDataU16(iPoint2D dim, uint32 cpp=1);
  friend class RawImage;
};

class RawImageDataFloat : public RawImageData
{
public:
  virtual void scaleBlackWhite();

protected:
  virtual void calculateBlackAreas();
  virtual void scaleValues(int start_y, int end_y);
  RawImageDataFloat(void);
  RawImageDataFloat(iPoint2D dim, uint32 cpp=1);
  friend class RawImage;
};

class RawImageWorker {
public:
  typedef enum {TASK_SCALE_VALUES} RawImageWorkerTask;
  RawImageWorker(RawImageData *img, RawImageWorkerTask task, int start_y, int end_y);
  void waitForThread();
  void _performTask();
protected:
  void startThread();
  pthread_t threadid;
  RawImageData* data;
  RawImageWorkerTask task;
  int start_y;
  int end_y;
};

 class RawImage {
 public:
   static RawImage create(RawImageType type = TYPE_USHORT16);
   static RawImage create(iPoint2D dim, RawImageType type = TYPE_USHORT16, uint32 componentsPerPixel = 1);
   RawImageData* operator-> ();
   RawImageData& operator* ();
   RawImage(RawImageData* p);  // p must not be NULL
  ~RawImage();
   RawImage(const RawImage& p);
   RawImage& operator= (const RawImage& p);

 private:
   RawImageData* p_;    // p_ is never NULL
 };

inline RawImage RawImage::create(RawImageType type)  { 
  switch (type)
  {
    case TYPE_USHORT16:
      return new RawImageDataU16();
    case TYPE_FLOAT32:
      return new RawImageDataFloat();
    default:
      printf("RawImage::create: Unknown Image type!\n");
  }
  return NULL; 
}

inline RawImage RawImage::create(iPoint2D dim, RawImageType type, uint32 componentsPerPixel)
{   
  switch (type) {
    case TYPE_USHORT16:
      return new RawImageDataU16(dim, componentsPerPixel);
    default:
      printf("RawImage::create: Unknown Image type!\n");
  }
  return NULL; 
}

} // namespace RawSpeed

#endif
