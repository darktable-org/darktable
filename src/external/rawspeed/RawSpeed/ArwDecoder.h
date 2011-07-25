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
#ifndef ARW_DECODER_H
#define ARW_DECODER_H

#include "RawDecoder.h"
#include "LJpegPlain.h"
#include "TiffIFD.h"
#include "BitPumpPlain.h"

namespace RawSpeed {

class ArwDecoder :
  public RawDecoder
{
public:
  ArwDecoder(TiffIFD *rootIFD, FileMap* file);
  virtual ~ArwDecoder(void);
  virtual RawImage decodeRaw();
  virtual void checkSupport(CameraMetaData *meta);
  virtual void decodeMetaData(CameraMetaData *meta);
  virtual void decodeThreaded(RawDecoderThread* t);
protected:
  void DecodeARW(ByteStream &input, uint32 w, uint32 h);
  void DecodeARW2(ByteStream &input, uint32 w, uint32 h, uint32 bpp);
  TiffIFD *mRootIFD;
  uint32 curve[0x4001];
  ByteStream *in;
};

} // namespace RawSpeed

#endif
