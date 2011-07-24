#include "StdAfx.h"
#include "SrwDecoder.h"
#include "TiffParserOlympus.h"
#ifdef __unix__
#include <stdlib.h>
#endif
/*
    RawSpeed - RAW file decoder.

    Copyright (C) 2009-2010 Klaus Post

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

SrwDecoder::SrwDecoder(TiffIFD *rootIFD, FileMap* file):
    RawDecoder(file), mRootIFD(rootIFD) {
}

SrwDecoder::~SrwDecoder(void) {
}

RawImage SrwDecoder::decodeRaw() {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(STRIPOFFSETS);

  if (data.empty())
    ThrowRDE("Srw Decoder: No image data found");

  TiffIFD* raw = data[0];

  int compression = raw->getEntry(COMPRESSION)->getInt();
  if (32769 != compression && 32770 != compression )
    ThrowRDE("Srw Decoder: Unsupported compression");

  if (32769 == compression)
  {
    this->decodeUncompressed(raw, false);
    return mRaw;
  }

  if (32770 == compression)
  {
    this->decodeUncompressed(raw, true);
    return mRaw;
  }
  ThrowRDE("Srw Decoder: Unsupported compression");
  return mRaw;
}

void SrwDecoder::checkSupport(CameraMetaData *meta) {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);
  if (data.empty())
    ThrowRDE("Srw Support check: Model name found");
  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();
  this->checkCameraSupported(meta, make, model, "");
}

void SrwDecoder::decodeMetaData(CameraMetaData *meta) {
  mRaw->cfa.setCFA(CFA_RED, CFA_GREEN, CFA_GREEN2, CFA_BLUE);
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);

  if (data.empty())
    ThrowRDE("SRW Meta Decoder: Model name found");

  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();

  data = mRootIFD->getIFDsWithTag(CFAPATTERN);
  if (!this->checkCameraSupported(meta, make, model, "") && !data.empty() && data[0]->hasEntry(CFAREPEATPATTERNDIM)) {
    const unsigned short* pDim = data[0]->getEntry(CFAREPEATPATTERNDIM)->getShortArray();
    iPoint2D cfaSize(pDim[1], pDim[0]);
    if (cfaSize.x != 2 && cfaSize.y != 2)
      ThrowRDE("SRW Decoder: Unsupported CFA pattern size");

    const uchar8* cPat = data[0]->getEntry(CFAPATTERN)->getData();
    if (cfaSize.area() != data[0]->getEntry(CFAPATTERN)->count)
      ThrowRDE("SRW Decoder: CFA pattern dimension and pattern count does not match: %d.");

    for (int y = 0; y < cfaSize.y; y++) {
      for (int x = 0; x < cfaSize.x; x++) {
        uint32 c1 = cPat[x+y*cfaSize.x];
        CFAColor c2;
        switch (c1) {
            case 0:
              c2 = CFA_RED; break;
            case 1:
              c2 = CFA_GREEN; break;
            case 2:
              c2 = CFA_BLUE; break;
            default:
              c2 = CFA_UNKNOWN;
              ThrowRDE("SRW Decoder: Unsupported CFA Color.");
        }
        mRaw->cfa.setColorAt(iPoint2D(x, y), c2);
      }
    }
    printf("Camera CFA: %s\n", mRaw->cfa.asString().c_str());
  }
  setMetaData(meta, make, model, "");
}

} // namespace RawSpeed
