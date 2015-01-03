#include "StdAfx.h"
#include "RawImage.h"
#include "RawDecoder.h"  // For exceptions
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
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace RawSpeed {

RawImageData::RawImageData(void):
    dim(0, 0), isCFA(true), cfa(iPoint2D(0,0)),
    blackLevel(-1), whitePoint(65536),
    dataRefCount(0), data(0), cpp(1), bpp(0),
    uncropped_dim(0, 0), table(NULL) {
  blackLevelSeparate[0] = blackLevelSeparate[1] = blackLevelSeparate[2] = blackLevelSeparate[3] = -1;
  pthread_mutex_init(&mymutex, NULL);
  mBadPixelMap = NULL;
  pthread_mutex_init(&errMutex, NULL);
  pthread_mutex_init(&mBadPixelMutex, NULL);
  mDitherScale = TRUE;
}

RawImageData::RawImageData(iPoint2D _dim, uint32 _bpc, uint32 _cpp) :
    dim(_dim), isCFA(_cpp==1), cfa(iPoint2D(0,0)),
    blackLevel(-1), whitePoint(65536),
    dataRefCount(0), data(0), cpp(_cpp), bpp(_bpc * _cpp),
    uncropped_dim(0, 0), table(NULL) {
  blackLevelSeparate[0] = blackLevelSeparate[1] = blackLevelSeparate[2] = blackLevelSeparate[3] = -1;
  mBadPixelMap = NULL;
  mDitherScale = TRUE;
  createData();
  pthread_mutex_init(&mymutex, NULL);
  pthread_mutex_init(&errMutex, NULL);
  pthread_mutex_init(&mBadPixelMutex, NULL);
}

ImageMetaData::ImageMetaData(void) {
  subsampling.x = subsampling.y = 1;
  isoSpeed = 0;
  pixelAspectRatio = 1;
  fujiRotationPos = 0;
  wbCoeffs[0] = NAN;
  wbCoeffs[1] = NAN;
  wbCoeffs[2] = NAN;
}

RawImageData::~RawImageData(void) {
  _ASSERTE(dataRefCount == 0);
  mOffset = iPoint2D(0, 0);
  pthread_mutex_destroy(&mymutex);
  pthread_mutex_destroy(&errMutex);
  pthread_mutex_destroy(&mBadPixelMutex);
  for (uint32 i = 0 ; i < errors.size(); i++) {
    free((void*)errors[i]);
  }
  if (table != NULL) {
    delete table;
  }
  errors.clear();
  destroyData();
}


void RawImageData::createData() {
  if (dim.x > 65535 || dim.y > 65535)
    ThrowRDE("RawImageData: Dimensions too large for allocation.");
  if (dim.x <= 0 || dim.y <= 0)
    ThrowRDE("RawImageData: Dimension of one sides is less than 1 - cannot allocate image.");
  if (data)
    ThrowRDE("RawImageData: Duplicate data allocation in createData.");
  pitch = (((dim.x * bpp) + 15) / 16) * 16;
  data = (uchar8*)_aligned_malloc(pitch * dim.y, 16);
  if (!data)
    ThrowRDE("RawImageData::createData: Memory Allocation failed.");
  uncropped_dim = dim;
}

void RawImageData::destroyData() {
  if (data)
    _aligned_free(data);
  if (mBadPixelMap)
    _aligned_free(mBadPixelMap);
  data = 0;
  mBadPixelMap = 0;
}

void RawImageData::setCpp(uint32 val) {
  if (data)
    ThrowRDE("RawImageData: Attempted to set Components per pixel after data allocation");
  if (val > 4)
    ThrowRDE("RawImageData: Only up to 4 components per pixel is support - attempted to set: %d", val);
  bpp /= cpp;
  cpp = val;
  bpp *= val;
}

uchar8* RawImageData::getData() {
  if (!data)
    ThrowRDE("RawImageData::getData - Data not yet allocated.");
  return &data[mOffset.y*pitch+mOffset.x*bpp];
}

uchar8* RawImageData::getData(uint32 x, uint32 y) {
  if ((int)x >= dim.x)
    ThrowRDE("RawImageData::getData - X Position outside image requested.");
  if ((int)y >= dim.y) {
    ThrowRDE("RawImageData::getData - Y Position outside image requested.");
  }

  x += mOffset.x;
  y += mOffset.y;

  if (!data)
    ThrowRDE("RawImageData::getData - Data not yet allocated.");

  return &data[y*pitch+x*bpp];
}

uchar8* RawImageData::getDataUncropped(uint32 x, uint32 y) {
  if ((int)x >= uncropped_dim.x)
    ThrowRDE("RawImageData::getDataUncropped - X Position outside image requested.");
  if ((int)y >= uncropped_dim.y) {
    ThrowRDE("RawImageData::getDataUncropped - Y Position outside image requested.");
  }

  if (!data)
    ThrowRDE("RawImageData::getDataUncropped - Data not yet allocated.");

  return &data[y*pitch+x*bpp];
}

iPoint2D RawImageData::getUncroppedDim()
{
  return uncropped_dim;
}

iPoint2D RawImageData::getCropOffset()
{
  return mOffset;
}

void RawImageData::subFrame(iRectangle2D crop) {
  if (!crop.dim.isThisInside(dim - crop.pos)) {
    writeLog(DEBUG_PRIO_WARNING, "WARNING: RawImageData::subFrame - Attempted to create new subframe larger than original size. Crop skipped.\n");
    return;
  }
  if (crop.pos.x < 0 || crop.pos.y < 0 || !crop.hasPositiveArea()) {
    writeLog(DEBUG_PRIO_WARNING, "WARNING: RawImageData::subFrame - Negative crop offset. Crop skipped.\n");
    return;
  }

  mOffset += crop.pos;
  dim = crop.dim;
}

void RawImageData::setError( const char* err )
{
  pthread_mutex_lock(&errMutex);
  errors.push_back(_strdup(err));
  pthread_mutex_unlock(&errMutex);
}

void RawImageData::createBadPixelMap()
{
  if (!isAllocated())
    ThrowRDE("RawImageData::createBadPixelMap: (internal) Bad pixel map cannot be allocated before image.");
  mBadPixelMapPitch = (((uncropped_dim.x / 8) + 15) / 16) * 16;
  mBadPixelMap = (uchar8*)_aligned_malloc(mBadPixelMapPitch * uncropped_dim.y, 16);
  memset(mBadPixelMap, 0, mBadPixelMapPitch * uncropped_dim.y);
  if (!mBadPixelMap)
    ThrowRDE("RawImageData::createData: Memory Allocation failed.");
}

RawImage::RawImage(RawImageData* p) : p_(p) {
  pthread_mutex_lock(&p_->mymutex);
  ++p_->dataRefCount;
  pthread_mutex_unlock(&p_->mymutex);
}

RawImage::RawImage(const RawImage& p) : p_(p.p_) {
  pthread_mutex_lock(&p_->mymutex);
  ++p_->dataRefCount;
  pthread_mutex_unlock(&p_->mymutex);
}

RawImage::~RawImage() {
  pthread_mutex_lock(&p_->mymutex);
  if (--p_->dataRefCount == 0) {
    pthread_mutex_unlock(&p_->mymutex);
    delete p_;
    return;
  }
  pthread_mutex_unlock(&p_->mymutex);
}


void RawImageData::transferBadPixelsToMap()
{
  if (mBadPixelPositions.empty())
    return;

  if (!mBadPixelMap)
    createBadPixelMap();

  for (vector<uint32>::iterator i=mBadPixelPositions.begin(); i != mBadPixelPositions.end(); i++) {
    uint32 pos = *i;
    uint32 pos_x = pos&0xffff;
    uint32 pos_y = pos>>16;
    mBadPixelMap[mBadPixelMapPitch * pos_y + (pos_x >> 3)] |= 1 << (pos_x&7);
  }
  mBadPixelPositions.clear();
}

void RawImageData::fixBadPixels()
{
#if !defined (EMULATE_DCRAW_BAD_PIXELS)

  /* Transfer if not already done */
  transferBadPixelsToMap();

#if 0 // For testing purposes
  if (!mBadPixelMap)
    createBadPixelMap();
  for (int y = 400; y < 700; y++){
    for (int x = 1200; x < 1700; x++) {
      mBadPixelMap[mBadPixelMapPitch * y + (x >> 3)] |= 1 << (x&7);
    }
  }
#endif

  /* Process bad pixels, if any */
  if (mBadPixelMap)
    startWorker(RawImageWorker::FIX_BAD_PIXELS, false);

  return;

#else  // EMULATE_DCRAW_BAD_PIXELS - not recommended, testing purposes only

  for (vector<uint32>::iterator i=mBadPixelPositions.begin(); i != mBadPixelPositions.end(); i++) {
    uint32 pos = *i;
    uint32 pos_x = pos&0xffff;
    uint32 pos_y = pos>>16;
    uint32 total = 0;
    uint32 div = 0;
    // 0 side covered by unsignedness.
    for (uint32 r=pos_x-2; r<=pos_x+2 && r<(uint32)uncropped_dim.x; r+=2) {
      for (uint32 c=pos_y-2; c<=pos_y+2 && c<(uint32)uncropped_dim.y; c+=2) {
        ushort16* pix = (ushort16*)getDataUncropped(r,c);
        if (*pix) {
          total += *pix;
          div++;
        }
      }
    }
    ushort16* pix = (ushort16*)getDataUncropped(pos_x,pos_y);
    if (div) {
      pix[0] = total / div;
    }
  }
#endif

}

void RawImageData::startWorker(RawImageWorker::RawImageWorkerTask task, bool cropped )
{
  int height = (cropped) ? dim.y : uncropped_dim.y;
  if (task & RawImageWorker::FULL_IMAGE) {
    height = uncropped_dim.y;
  }

  int threads = getThreadCount();
  if (threads <= 1) {
    RawImageWorker worker(this, task, 0, height);
    worker.performTask();
    return;
  }

  RawImageWorker **workers = new RawImageWorker*[threads];
  int y_offset = 0;
  int y_per_thread = (height + threads - 1) / threads;

  for (int i = 0; i < threads; i++) {
    int y_end = MIN(y_offset + y_per_thread, height);
    workers[i] = new RawImageWorker(this, task, y_offset, y_end);
    workers[i]->startThread();
    y_offset = y_end;
  }
  for (int i = 0; i < threads; i++) {
    workers[i]->waitForThread();
    delete workers[i];
  }
  delete[] workers;
}

void RawImageData::fixBadPixelsThread( int start_y, int end_y )
{
  int gw = (uncropped_dim.x + 15) / 32;
  for (int y = start_y; y < end_y; y++) {
    uint32* bad_map = (uint32*)&mBadPixelMap[y*mBadPixelMapPitch];
    for (int x = 0 ; x < gw; x++) {
      // Test if there is a bad pixel within these 32 pixels
      if (bad_map[x] != 0) {
        uchar8 *bad = (uchar8*)&bad_map[x];
        // Go through each pixel
        for (int i = 0; i < 4; i++) {
          for (int j = 0; j < 8; j++) {
            if (1 == ((bad[i]>>j) & 1))
              fixBadPixel(x*32+i*8+j, y, 0);
          }
        }
      }
    }
  }
}

void RawImageData::blitFrom(RawImage src, iPoint2D srcPos, iPoint2D size, iPoint2D destPos )
{
  iRectangle2D src_rect(srcPos, size);
  iRectangle2D dest_rect(destPos, size);
  src_rect = src_rect.getOverlap(iRectangle2D(iPoint2D(0,0), src->dim));
  dest_rect = dest_rect.getOverlap(iRectangle2D(iPoint2D(0,0), dim));

  iPoint2D blitsize = src_rect.dim.getSmallest(dest_rect.dim);
  if (blitsize.area() <= 0)
    return;

  // TODO: Move offsets after crop.
  BitBlt(getData(dest_rect.pos.x, dest_rect.pos.y), pitch, src->getData(src_rect.pos.x, src_rect.pos.y), src->pitch, blitsize.x*bpp, blitsize.y);
}

/* Does not take cfa into consideration */
void RawImageData::expandBorder(iRectangle2D validData)
{
  validData = validData.getOverlap(iRectangle2D(0,0,dim.x, dim.y));
  if (validData.pos.x > 0) {
    for (int y = 0; y < dim.y; y++ ) {
      uchar8* src_pos = getData(validData.pos.x, y);
      uchar8* dst_pos = getData(validData.pos.x-1, y);
      for (int x = validData.pos.x; x >= 0; x--) {
        for (uint32 i = 0; i < bpp; i++) {
          dst_pos[i] = src_pos[i];
        }
        dst_pos -= bpp;
      }
    }
  }

  if (validData.getRight() < dim.x) {
    int pos = validData.getRight();
    for (int y = 0; y < dim.y; y++ ) {
      uchar8* src_pos = getData(pos-1, y);
      uchar8* dst_pos = getData(pos, y);
      for (int x = pos; x < dim.x; x++) {
        for (uint32 i = 0; i < bpp; i++) {
          dst_pos[i] = src_pos[i];
        }
        dst_pos += bpp;
      }
    }
  }

  if (validData.pos.y > 0) {
    uchar8* src_pos = getData(0, validData.pos.y);
    for (int y = 0; y < validData.pos.y; y++ ) {
      uchar8* dst_pos = getData(0, y);
      memcpy(dst_pos, src_pos, dim.x*bpp);
    }
  }
  if (validData.getBottom() < dim.y) {
    uchar8* src_pos = getData(0, validData.getBottom()-1);
    for (int y = validData.getBottom(); y < dim.y; y++ ) {
      uchar8* dst_pos = getData(0, y);
      memcpy(dst_pos, src_pos, dim.x*bpp);
    }
  }
}

void RawImageData::clearArea( iRectangle2D area, uchar8 val /*= 0*/ )
{
  area = area.getOverlap(iRectangle2D(iPoint2D(0,0), dim));

  if (area.area() <= 0)
    return;

  for (int y = area.getTop(); y < area.getBottom(); y++)
    memset(getData(area.getLeft(),y), val, area.getWidth() * bpp);
}

RawImageData* RawImage::operator->() {
  return p_;
}

RawImageData& RawImage::operator*() {
  return *p_;
}

RawImage& RawImage::operator=(const RawImage & p) {
  if (this == &p)      // Same object?
    return *this;      // Yes, so skip assignment, and just return *this.
  pthread_mutex_lock(&p_->mymutex);
  // Retain the old RawImageData before overwriting it
  RawImageData* const old = p_;
  p_ = p.p_;
  // Increment use on new data
  ++p_->dataRefCount;
  // If the RawImageData previously used by "this" is unused, delete it.
  if (--old->dataRefCount == 0) {
  	pthread_mutex_unlock(&(old->mymutex));
  	delete old;
  } else {
  	pthread_mutex_unlock(&(old->mymutex));
  }
  return *this;
}

void *RawImageWorkerThread(void *_this) {
  RawImageWorker* me = (RawImageWorker*)_this;
  me->performTask();
  pthread_exit(NULL);
  return 0;
}

RawImageWorker::RawImageWorker( RawImageData *_img, RawImageWorkerTask _task, int _start_y, int _end_y )
{
  data = _img;
  start_y = _start_y;
  end_y = _end_y;
  task = _task;
}

void RawImageWorker::startThread()
{
  /* Initialize and set thread detached attribute */
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
  pthread_create(&threadid, &attr, RawImageWorkerThread, this);
}

void RawImageWorker::waitForThread()
{
  void *status;
  pthread_join(threadid, &status);
}

void RawImageWorker::performTask()
{
  try {
    switch(task)
    {
    case SCALE_VALUES:
      data->scaleValues(start_y, end_y);
      break;
    case FIX_BAD_PIXELS:
      data->fixBadPixelsThread(start_y, end_y);
      break;
    case APPLY_LOOKUP:
      data->doLookup(start_y, end_y);
      break;
    default:
      _ASSERTE(false);
    }
  } catch (RawDecoderException &e) {
    data->setError(e.what());
  } catch (TiffParserException &e) {
    data->setError(e.what());
  } catch (IOException &e) {
    data->setError(e.what());
  }
}

void RawImageData::sixteenBitLookup()
{
  if (table == NULL) {
    return;
  }
  startWorker(RawImageWorker::APPLY_LOOKUP, true);
}

void RawImageData::setTable( TableLookUp *t )
{
  if (table != NULL) {
    delete table;
  }
  table = t;
}

void RawImageData::setTable(const ushort16* table, int nfilled, bool dither) {
  TableLookUp* t = new TableLookUp(1, dither);
  t->setTable(0, table, nfilled);
  this->setTable(t);
}

const int TABLE_SIZE = 65536 * 2;

// Creates n numre of tables.
TableLookUp::TableLookUp( int _ntables, bool _dither ) : ntables(_ntables), dither(_dither) {
  tables = NULL;
  if (ntables < 1) {
    ThrowRDE("Cannot construct 0 tables");
  }
  tables = new ushort16[ntables * TABLE_SIZE];
  memset(tables, 0, sizeof(ushort16) * ntables * TABLE_SIZE);
}

TableLookUp::~TableLookUp()
{
  if (tables != NULL) {
    delete[] tables;
    tables = NULL;
  }
}


void TableLookUp::setTable(int ntable, const ushort16 *table , int nfilled) {
  if (ntable > ntables) {
    ThrowRDE("Table lookup with number greater than number of tables.");
  }
  ushort16* t = &tables[ntable* TABLE_SIZE];
  if (!dither) {
    for (int i = 0; i < 65536; i++) {
      t[i] = (i < nfilled) ? table[i] : table[nfilled-1];
    }
    return;
  }
  for (int i = 0; i < nfilled; i++) {
    int center = table[i];
    int lower = i > 0 ? table[i-1] : center;
    int upper = i < (nfilled-1) ? table[i+1] : center;
    int delta = upper - lower;
    t[i*2] = center - ((upper - lower + 2) / 4);
    t[i*2+1] = delta;
  }

  for (int i = nfilled; i < 65536; i++) {
    t[i*2] = table[nfilled-1];
    t[i*2+1] = 0;
  }
  t[0] = t[1];
  t[TABLE_SIZE - 1] = t[TABLE_SIZE - 2];
}


ushort16* TableLookUp::getTable(int n) {
  if (n > ntables) {
    ThrowRDE("Table lookup with number greater than number of tables.");
  }
  return &tables[n * TABLE_SIZE];
}


} // namespace RawSpeed
