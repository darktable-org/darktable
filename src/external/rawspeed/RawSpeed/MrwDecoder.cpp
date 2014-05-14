#include "StdAfx.h"
#include "MrwDecoder.h"
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

MrwDecoder::MrwDecoder(uint32 doff, uint32 w, uint32 h, FileMap* file) :
    RawDecoder(file) {
  data_offset = doff;
  raw_width = w;
  raw_height = h;
}

MrwDecoder::~MrwDecoder(void) {
}

RawImage MrwDecoder::decodeRawInternal() {
  mRaw->dim = iPoint2D(raw_width, raw_height);
  mRaw->createData();

  uint32 imgsize = raw_width * raw_height * 3 / 2;

  if (!mFile->isValid(data_offset))
    ThrowRDE("Sony MRW decoder: Data offset after EOF, file probably truncated");
  if (!mFile->isValid(data_offset+imgsize-1))
    ThrowRDE("Sony MRW decoder: Image end after EOF, file probably truncated");

  ByteStream input(mFile->getData(data_offset), imgsize);
 
  try {
    Decode12BitRawBE(input, raw_width, raw_height);
  } catch (IOException &e) {
    mRaw->setError(e.what());
    // Let's ignore it, it may have delivered somewhat useful data.
  }

  return mRaw;
}

void MrwDecoder::checkSupportInternal(CameraMetaData *meta) {
  //FIXME: Get the actual make and model from the TIFF section
  this->checkCameraSupported(meta, "KONICA MINOLTA", "DYNAX 5D", "");
}

void MrwDecoder::decodeMetaDataInternal(CameraMetaData *meta) {
  //Default
  int iso = 0;

  //FIXME: Get the actual make and model from the TIFF section
  setMetaData(meta, "KONICA MINOLTA", "DYNAX 5D", "", iso);
}

} // namespace RawSpeed
