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

MrwDecoder::MrwDecoder(FileMap* file) :
    RawDecoder(file) {
  parseHeader();
}

MrwDecoder::~MrwDecoder(void) {
}

int MrwDecoder::isMRW(FileMap* input) {
  const uchar8* data = input->getData(0);
  return data[0] == 0x00 && data[1] == 0x4D && data[2] == 0x52 && data[3] == 0x4D;
}

void MrwDecoder::parseHeader() {
  const unsigned char* data = mFile->getData(0);
  
  if (!isMRW(mFile))
    ThrowRDE("This isn't actually a MRW file, why are you calling me?");
    
  // FIXME: We need to be more complete and parse the full PRD, WBG and TTW 
  //        entries (see dcraw parse_minolta code)
  data_offset = ((data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7])+8;
  raw_height = (data[24] << 8) | data[25];
  raw_width = (data[26] << 8) | data[27];
}


RawImage MrwDecoder::decodeRawInternal() {
  mRaw->dim = iPoint2D(raw_width, raw_height);
  mRaw->createData();

  uint32 imgsize = raw_width * raw_height * 3 / 2;

  if (!mFile->isValid(data_offset))
    ThrowRDE("MRW decoder: Data offset after EOF, file probably truncated");
  if (!mFile->isValid(data_offset+imgsize-1))
    ThrowRDE("MRW decoder: Image end after EOF, file probably truncated");

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
