#ifndef DNG_DECODER_SLICES_H
#define DNG_DECODER_SLICES_H

#include "RawDecoder.h"
#include <queue>
#include "LJpegPlain.h"
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

class DngSliceElement
{
public:
  DngSliceElement(uint32 off, uint32 count, uint32 offsetX, uint32 offsetY) : 
      byteOffset(off), byteCount(count), offX(offsetX), offY(offsetY), mUseBigtable(false) {};
  ~DngSliceElement(void) {};
  const uint32 byteOffset;
  const uint32 byteCount;
  const uint32 offX;
  const uint32 offY;
  bool mUseBigtable;
};
class DngDecoderSlices;

class DngDecoderThread
{
public:
  DngDecoderThread(void) {}
  ~DngDecoderThread(void) {}
  pthread_t threadid;
  queue<DngSliceElement> slices;
  DngDecoderSlices* parent;
};


class DngDecoderSlices
{
public:
  DngDecoderSlices(FileMap* file, RawImage img );
  ~DngDecoderSlices(void);
  void addSlice(DngSliceElement slice);
  void startDecoding();
  void decodeSlice(DngDecoderThread* t);
  void setError(const char* err);
  int size();
  queue<DngSliceElement> slices;
  vector<DngDecoderThread*> threads;
  FileMap *mFile; 
  RawImage mRaw;
  vector<const char*> errors;
  pthread_mutex_t errMutex;   // Mutex for above
  bool mFixLjpeg;
  uint32 nThreads;
};

} // namespace RawSpeed

#endif
