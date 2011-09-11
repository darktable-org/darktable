#include "StdAfx.h"
#include "RawImage.h"
#include "RawDecoder.h"  // For exceptions
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
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace RawSpeed {

RawImageData::RawImageData(void):
    dim(0, 0), isCFA(true),
    blackLevel(-1), whitePoint(65536),
    dataRefCount(0), data(0), cpp(1), bpp(0),
    uncropped_dim(0, 0) {
  blackLevelSeparate[0] = blackLevelSeparate[1] = blackLevelSeparate[2] = blackLevelSeparate[3] = -1;
  pthread_mutex_init(&mymutex, NULL);
  subsampling.x = subsampling.y = 1;
}

RawImageData::RawImageData(iPoint2D _dim, uint32 _bpc, uint32 _cpp) :
    dim(_dim),
    blackLevel(-1), whitePoint(65536),
    dataRefCount(0), data(0), cpp(cpp), bpp(_bpc),
    uncropped_dim(0, 0) {
  blackLevelSeparate[0] = blackLevelSeparate[1] = blackLevelSeparate[2] = blackLevelSeparate[3] = -1;
  subsampling.x = subsampling.y = 1;
  createData();
  pthread_mutex_init(&mymutex, NULL);
}

RawImageData::~RawImageData(void) {
  _ASSERTE(dataRefCount == 0);
  if (data)
    _aligned_free(data);
  data = 0;
  mOffset = iPoint2D(0, 0);
  pthread_mutex_destroy(&mymutex);
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
  data = 0;
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

void RawImageData::subFrame(iPoint2D offset, iPoint2D new_size) {
  if (!new_size.isThisInside(dim - offset)) {
    printf("WARNING: RawImageData::subFrame - Attempted to create new subframe larger than original size. Crop skipped.\n");
    return;
  }
  if (offset.x < 0 || offset.y < 0) {
    printf("WARNING: RawImageData::subFrame - Negative crop offset. Crop skipped.\n");
    return;
  }

  mOffset += offset;
  dim = new_size;
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

RawImageData* RawImage::operator->() {
  return p_;
}

RawImageData& RawImage::operator*() {
  return *p_;
}

RawImage& RawImage::operator=(const RawImage & p) {
  RawImageData* const old = p_;
  p_ = p.p_;
  ++p_->dataRefCount;
  if (--old->dataRefCount == 0) delete old;
  return *this;
}

void *RawImageWorkerThread(void *_this) {
  RawImageWorker* me = (RawImageWorker*)_this;
  me->_performTask();
  pthread_exit(NULL);
  return 0;
}

RawImageWorker::RawImageWorker( RawImageData *_img, RawImageWorkerTask _task, int _start_y, int _end_y )
{
  data = _img;
  start_y = _start_y;
  end_y = _end_y;
  task = _task;
  startThread();
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

void RawImageWorker::_performTask()
{
  switch(task)
  {
    case TASK_SCALE_VALUES:
      data->scaleValues(start_y, end_y);
      break;
    default:
      _ASSERTE(false);
  }
}


} // namespace RawSpeed
