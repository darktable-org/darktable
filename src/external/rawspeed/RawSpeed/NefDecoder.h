#ifndef NEF_DECODER_H
#define NEF_DECODER_H

#include "RawDecoder.h"
#include "LJpegPlain.h"
#include "TiffIFD.h"
#include "BitPumpPlain.h"
#include "TiffParser.h"
#include "NikonDecompressor.h"
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

class NefDecoder :
  public RawDecoder
{
public:
  NefDecoder(TiffIFD *rootIFD, FileMap* file);
  virtual ~NefDecoder(void);
  virtual RawImage decodeRawInternal();
  virtual void decodeMetaDataInternal(CameraMetaData *meta);
  virtual void checkSupportInternal(CameraMetaData *meta);
  TiffIFD *mRootIFD;
  virtual TiffIFD* getRootIFD() {return mRootIFD;}
private:
  bool D100IsCompressed(uint32 offset);
  bool NEFIsUncompressed(TiffIFD *raw);
  bool NEFIsUncompressedRGB(TiffIFD *raw);
  void DecodeUncompressed();
  void DecodeD100Uncompressed();
  void DecodeSNefUncompressed();
  void readCoolpixMangledRaw(ByteStream &input, iPoint2D& size, iPoint2D& offset, int inputPitch);
  void readCoolpixSplitRaw(ByteStream &input, iPoint2D& size, iPoint2D& offset, int inputPitch);
  void DecodeNikonSNef(ByteStream &input, uint32 w, uint32 h);
  TiffIFD* FindBestImage(vector<TiffIFD*>* data);
  string getMode();
  string getExtendedMode(string mode);
};

class NefSlice {
public:
  NefSlice() { h = offset = count = 0;};
  ~NefSlice() {};
  uint32 h;
  uint32 offset;
  uint32 count;
};

} // namespace RawSpeed
#endif
