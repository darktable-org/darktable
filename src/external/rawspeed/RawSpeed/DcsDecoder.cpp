#include "StdAfx.h"
#include "DcsDecoder.h"

/*
    RawSpeed - RAW file decoder.

    Copyright (C) 2009-2014 Klaus Post
    Copyright (C) 2015 Pedro Côrte-Real

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

DcsDecoder::DcsDecoder(TiffIFD *rootIFD, FileMap* file)  :
    RawDecoder(file), mRootIFD(rootIFD) {
  decoderVersion = 0;
}

DcsDecoder::~DcsDecoder(void) {
}

RawImage DcsDecoder::decodeRawInternal() {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(CFAPATTERN);

  if (data.size() < 1)
    ThrowRDE("DCS Decoder: No image data found");

  return mRaw;
}

void DcsDecoder::checkSupportInternal(CameraMetaData *meta) {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);
  if (data.empty())
    ThrowRDE("DCS Support check: Model name not found");
  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();
  this->checkCameraSupported(meta, make, model, "");
}

void DcsDecoder::decodeMetaDataInternal(CameraMetaData *meta) {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);

  if (data.empty())
    ThrowRDE("DCS Decoder: Model name found");
  if (!data[0]->hasEntry(MAKE))
    ThrowRDE("DCS Decoder: Make name not found");

  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();
  setMetaData(meta, make, model, "", 0);
}

} // namespace RawSpeed
