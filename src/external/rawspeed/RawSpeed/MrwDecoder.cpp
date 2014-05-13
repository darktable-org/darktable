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

MrwDecoder::MrwDecoder(TiffIFD *rootIFD, FileMap* file) :
    RawDecoder(file), mRootIFD(rootIFD) {
  mShiftDownScale = 0;
}

MrwDecoder::~MrwDecoder(void) {
  if (mRootIFD)
    delete mRootIFD;
  mRootIFD = NULL;
}

RawImage MrwDecoder::decodeRawInternal() {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(STRIPOFFSETS);

  if (data.empty())
    ThrowRDE("MRW Decoder: No image data found");

  TiffIFD* raw = data[0];
  TiffEntry *offsets = raw->getEntry(STRIPOFFSETS);
  TiffEntry *counts = raw->getEntry(STRIPBYTECOUNTS);

  if (offsets->count != 1) {
    ThrowRDE("MRW Decoder: Multiple Strips found: %u", offsets->count);
  }
  if (counts->count != offsets->count) {
    ThrowRDE("MRW Decoder: Byte count number does not match strip size: count:%u, strips:%u ", counts->count, offsets->count);
  }
  uint32 width = raw->getEntry(IMAGEWIDTH)->getInt();
  uint32 height = raw->getEntry(IMAGELENGTH)->getInt();

  uint32 c2 = counts->getInt();
  uint32 off = offsets->getInt();

  ByteStream input(mFile->getData(off), c2);
 
  try {
    DecodeMRW(input, width, height);
  } catch (IOException &e) {
    mRaw->setError(e.what());
    // Let's ignore it, it may have delivered somewhat useful data.
  }

  return mRaw;
}

void MrwDecoder::DecodeMRW(ByteStream &input, uint32 w, uint32 h) {
  uchar8* data = mRaw->getData();
  ushort16* dest = (ushort16*) & data[0];
  uint32 pitch = mRaw->pitch / sizeof(ushort16);

  ushort16 vbits=0;
  
  for (uint32 row=0; row < h; row++) {
    for (uint32 col=0; col < w; col++) {
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
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);
  if (data.empty())
    ThrowRDE("MRW Support check: Model name found");
  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();
  this->checkCameraSupported(meta, make, model, "");
}

void MrwDecoder::decodeMetaDataInternal(CameraMetaData *meta) {
  //Default
  int iso = 0;

  mRaw->cfa.setCFA(CFA_RED, CFA_GREEN, CFA_GREEN2, CFA_BLUE);
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);

  if (data.empty())
    ThrowRDE("MRW Meta Decoder: Model name found");
  if (!data[0]->hasEntry(MAKE))
    ThrowRDE("MRW Decoder: Make name not found");

  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();

  if (mRootIFD->hasEntryRecursive(ISOSPEEDRATINGS))
    iso = mRootIFD->getEntryRecursive(ISOSPEEDRATINGS)->getInt();

  setMetaData(meta, make, model, "", iso);
  mRaw->whitePoint >>= mShiftDownScale;
  mRaw->blackLevel >>= mShiftDownScale;
}

/* MRW images have predictable offsets threaded decoding should be trivial
   FIXME: This is actually not implemented yet! */

/* void MrwDecoder::decodeThreaded(RawDecoderThread * t) {
  uchar8* data = mRaw->getData();
  uint32 pitch = mRaw->pitch;
  uint32 w = mRaw->dim.x;
} */

} // namespace RawSpeed
