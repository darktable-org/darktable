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

  ByteStream input(mFile->getData(data_offset), 0);
 
  try {
    DecodeMRW(input);
  } catch (IOException &e) {
    mRaw->setError(e.what());
    // Let's ignore it, it may have delivered somewhat useful data.
  }

  return mRaw;
}

void MrwDecoder::DecodeMRW(ByteStream &input) {
  uchar8* data = mRaw->getData();
  ushort16* dest = (ushort16*) & data[0];
  uint32 pitch = mRaw->pitch / sizeof(ushort16);

  ushort16 vbits=0;
  
  for (uint32 row=0; row < raw_height; row++) {
    for (uint32 col=0; col < raw_width; col++) {
      // Read enough bytes so we have a full sample, on the first run we'll read
      // 16bits and then on the second run we'll read 8bits and use the 4 left
      // over from the previous read, rinse repeat
      for (vbits -= 12; vbits < 0; vbits += 8)
        data++;
      // Then we need to ignore any extra bits to get a clean 12 bit value
      dest[col+row*pitch] = (*((ushort16 *) data) >> vbits) & 0x0fff;
    }
  }
}

void MrwDecoder::checkSupportInternal(CameraMetaData *meta) {
  //FIXME: NOOP for now
}

void MrwDecoder::decodeMetaDataInternal(CameraMetaData *meta) {
  //FIXME: NOOP for now
}

} // namespace RawSpeed
