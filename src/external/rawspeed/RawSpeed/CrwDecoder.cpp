#include "StdAfx.h"
#include "CrwDecoder.h"

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

namespace RawSpeed {

CrwDecoder::CrwDecoder(CiffIFD *rootIFD, FileMap* file) :
    RawDecoder(file), mRootIFD(rootIFD) {
  decoderVersion = 0;
}

CrwDecoder::~CrwDecoder(void) {
  if (mRootIFD)
    delete mRootIFD;
  mRootIFD = NULL;
}

RawImage CrwDecoder::decodeRawInternal() {
  return mRaw;
}

void CrwDecoder::checkSupportInternal(CameraMetaData *meta) {
  vector<CiffIFD*> data = mRootIFD->getIFDsWithTag(CIFF_MAKEMODEL);
  if (data.empty())
    ThrowRDE("CRW Support check: Model name not found");
  string make = data[0]->getEntry(CIFF_MAKEMODEL)->getString();
  string model = data[0]->getEntry(CIFF_MAKEMODEL)->getString();

  this->checkCameraSupported(meta, make, model, "");
}

void CrwDecoder::decodeMetaDataInternal(CameraMetaData *meta) {
  int iso = 0;
  mRaw->cfa.setCFA(iPoint2D(2,2), CFA_RED, CFA_GREEN, CFA_GREEN2, CFA_BLUE);
  vector<CiffIFD*> data = mRootIFD->getIFDsWithTag(CIFF_MAKEMODEL);
  if (data.empty())
    ThrowRDE("CRW Support check: Model name not found");
  string make = data[0]->getEntry(CIFF_MAKEMODEL)->getString();
  string model = data[0]->getEntry(CIFF_MAKEMODEL)->getString();
  string mode = "";

  setMetaData(meta, make, model, mode, iso);
}

} // namespace RawSpeed
