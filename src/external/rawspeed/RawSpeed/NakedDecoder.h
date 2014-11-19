/*
    RawSpeed - RAW file decoder.

    Copyright (C) 2009-2014 Klaus Post
    Copyright (C) 2014 Pedro Côrte-Real

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
#ifndef NAKED_DECODER_H
#define NAKED_DECODER_H

#include "RawDecoder.h"

namespace RawSpeed {

typedef struct {
  uint32 fsize;
  ushort16 width, height;
  uchar8 lm, tm, rm, bm, lf, cf, max, flags;
  char make[10], model[20];
  ushort16 offset;
} naked_camera_t;

class NakedDecoder :
  public RawDecoder
{
public:
  NakedDecoder(FileMap* file);
  virtual ~NakedDecoder(void);
  virtual RawImage decodeRawInternal();
  virtual void checkSupportInternal(CameraMetaData *meta);
  virtual void decodeMetaDataInternal(CameraMetaData *meta);
  static int couldBeNakedRaw(FileMap* input);
protected:
  virtual void identifyFile();
  char *make;
  char *model;
  uint32 width;
  uint32 height;
  uint32 offset;
  uint32 bits;
};

} // namespace RawSpeed

#endif
