#ifndef RAW_IMAGE_H
#define RAW_IMAGE_H

#include "ColorFilterArray.h"
#include "BlackArea.h"

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

class RawImage;
class RawImageData;
typedef enum {TYPE_USHORT16, TYPE_FLOAT32} RawImageType;

class RawImageWorker {
public:
  typedef enum {
    SCALE_VALUES = 1, FIX_BAD_PIXELS = 2, APPLY_LOOKUP = 3 | 0x1000, FULL_IMAGE = 0x1000
  } RawImageWorkerTask;
  RawImageWorker(RawImageData *img, RawImageWorkerTask task, int start_y, int end_y);
  void startThread();
  void waitForThread();
  void performTask();
protected:
  pthread_t threadid;
  RawImageData* data;
  RawImageWorkerTask task;
  int start_y;
  int end_y;
};

class TableLookUp {
public:
  TableLookUp(int ntables, bool dither);
  ~TableLookUp();
  void setTable(int ntable, const ushort16* table, int nfilled);
  ushort16* getTable(int n);
  const int ntables;
  ushort16* tables;
  const bool dither;
};


class ImageMetaData {
public:
  ImageMetaData(void);

  // Aspect ratio of the pixels, usually 1 but some cameras need scaling
  // <1 means the image needs to be stretched vertically, (0.5 means 2x)
  // >1 means the image needs to be stretched horizontally (2 mean 2x)
  double pixelAspectRatio;

  // White balance coefficients of the image
  float wbCoeffs[3];

  // How many pixels far down the left edge and far up the right edge the image 
  // corners are when the image is rotated 45 degrees in Fuji rotated sensors.
  uint32 fujiRotationPos;

  iPoint2D subsampling;
  string make;
  string model;
  string mode;

  // ISO speed. If known the value is set, otherwise it will be '0'.
  int isoSpeed;

private:
};

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
  void blitFrom(const RawImage src, iPoint2D srcPos, iPoint2D size, iPoint2D destPos);
  RawSpeed::RawImageType getDataType() const { return dataType; }
  uchar8* getData();
  uchar8* getData(uint32 x, uint32 y);    // Not super fast, but safe. Don't use per pixel.
  uchar8* getDataUncropped(uint32 x, uint32 y);
  virtual void subFrame(iRectangle2D cropped );
  virtual void clearArea(iRectangle2D area, uchar8 value = 0);
  iPoint2D getUncroppedDim();
  iPoint2D getCropOffset();
  virtual void scaleBlackWhite() = 0;
  virtual void calculateBlackAreas() = 0;
  virtual void setWithLookUp(ushort16 value, uchar8* dst, uint32* random) = 0;
  virtual void sixteenBitLookup();
  virtual void transferBadPixelsToMap();
  virtual void fixBadPixels();
  void expandBorder(iRectangle2D validData);
  void setTable(const ushort16* table, int nfilled, bool dither);
  void setTable(TableLookUp *t);

  bool isAllocated() {return !!data;}
  void createBadPixelMap();
  iPoint2D dim;
  uint32 pitch;
  bool isCFA;
  ColorFilterArray cfa;
  int blackLevel;
  int blackLevelSeparate[4];
  int whitePoint;
  vector<BlackArea> blackAreas;
  /* Vector containing silent errors that occurred doing decoding, that may have lead to */
  /* an incomplete image. */
  vector<const char*> errors;
  pthread_mutex_t errMutex;   // Mutex for above
  void setError(const char* err);
  /* Vector containing the positions of bad pixels */
  /* Format is x | (y << 16), so maximum pixel position is 65535 */
  vector<uint32> mBadPixelPositions;    // Positions of zeroes that must be interpolated
  pthread_mutex_t mBadPixelMutex;   // Mutex for above, must be used if more than 1 thread is accessing vector
  uchar8 *mBadPixelMap;
  uint32 mBadPixelMapPitch;
  bool mDitherScale;           // Should upscaling be done with dither to minimize banding?
  ImageMetaData metadata;

protected:
  RawImageType dataType;
  RawImageData(void);
  RawImageData(iPoint2D dim, uint32 bpp, uint32 cpp=1);
  virtual void scaleValues(int start_y, int end_y) = 0;
  virtual void doLookup(int start_y, int end_y) = 0;
  virtual void fixBadPixel( uint32 x, uint32 y, int component = 0) = 0;
  void fixBadPixelsThread(int start_y, int end_y);
  void startWorker(RawImageWorker::RawImageWorkerTask task, bool cropped );
  uint32 dataRefCount;
  uchar8* data;
  uint32 cpp;      // Components per pixel
  uint32 bpp;      // Bytes per pixel.
  friend class RawImage;
  pthread_mutex_t mymutex;
  iPoint2D mOffset;
  iPoint2D uncropped_dim;
  TableLookUp *table;
};


class RawImageDataU16 : public RawImageData
{
public:
  virtual void scaleBlackWhite();
  virtual void calculateBlackAreas();
  virtual void setWithLookUp(ushort16 value, uchar8* dst, uint32* random);

protected:
  virtual void scaleValues(int start_y, int end_y);
  virtual void fixBadPixel( uint32 x, uint32 y, int component = 0);
  virtual void doLookup(int start_y, int end_y);

  RawImageDataU16(void);
  RawImageDataU16(iPoint2D dim, uint32 cpp=1);
  friend class RawImage;
};

class RawImageDataFloat : public RawImageData
{
public:
  virtual void scaleBlackWhite();
  virtual void calculateBlackAreas();
  virtual void setWithLookUp(ushort16 value, uchar8* dst, uint32* random);

protected:
  virtual void scaleValues(int start_y, int end_y);
  virtual void fixBadPixel( uint32 x, uint32 y, int component = 0);
  virtual void doLookup(int start_y, int end_y);
  RawImageDataFloat(void);
  RawImageDataFloat(iPoint2D dim, uint32 cpp=1);
  friend class RawImage;
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
      writeLog(DEBUG_PRIO_ERROR, "RawImage::create: Unknown Image type!\n");
  }
  return NULL; 
}

inline RawImage RawImage::create(iPoint2D dim, RawImageType type, uint32 componentsPerPixel)
{   
  switch (type) {
    case TYPE_USHORT16:
      return new RawImageDataU16(dim, componentsPerPixel);
    default:
      writeLog(DEBUG_PRIO_ERROR, "RawImage::create: Unknown Image type!\n");
  }
  return NULL; 
}

} // namespace RawSpeed

#endif
