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
#ifndef ARW_DECODER_H
#define ARW_DECODER_H

#include "RawDecoder.h"
#include "LJpegPlain.h"
#include "TiffIFD.h"
#include "TiffIFDBE.h"
#include "BitPumpPlain.h"

namespace RawSpeed {

class ArwDecoder :
  public RawDecoder
{
public:
  ArwDecoder(TiffIFD *rootIFD, FileMap* file);
  virtual ~ArwDecoder(void);
  virtual RawImage decodeRawInternal();
  virtual void checkSupportInternal(CameraMetaData *meta);
  virtual void decodeMetaDataInternal(CameraMetaData *meta);
  virtual void decodeThreaded(RawDecoderThread* t);
  virtual TiffIFD* getRootIFD() {return mRootIFD;}
protected:
  void DecodeARW(ByteStream &input, uint32 w, uint32 h);
  void DecodeARW2(ByteStream &input, uint32 w, uint32 h, uint32 bpp);
  void DecodeSR2(TiffIFD* raw);
  void GetWB();
  TiffIFD *mRootIFD;
  ByteStream *in;
  int mShiftDownScale;
};

} // namespace RawSpeed

#endif
