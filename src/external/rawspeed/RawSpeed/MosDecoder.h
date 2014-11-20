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
#ifndef MOS_DECODER_H
#define MOS_DECODER_H

#include "RawDecoder.h"
#include "string.h"
#include "LJpegPlain.h"

namespace RawSpeed {

class MosDecoder :
  public RawDecoder
{
public:
  MosDecoder(TiffIFD *rootIFD, FileMap* file);
  virtual ~MosDecoder(void);
  virtual RawImage decodeRawInternal();
  virtual void checkSupportInternal(CameraMetaData *meta);
  virtual void decodeMetaDataInternal(CameraMetaData *meta);
protected:
  TiffIFD *mRootIFD;
  const char *make, *model;
  void parseXMP(TiffEntry *xmp);
  void DecodePhaseOneC(ByteStream &input, uint32 width, uint32 height);
};

} // namespace RawSpeed

#endif
