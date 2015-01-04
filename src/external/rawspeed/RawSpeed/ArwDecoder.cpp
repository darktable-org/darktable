#include "StdAfx.h"
#include "ArwDecoder.h"
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

ArwDecoder::ArwDecoder(TiffIFD *rootIFD, FileMap* file) :
    RawDecoder(file), mRootIFD(rootIFD) {
  mShiftDownScale = 0;
  decoderVersion = 1;
}

ArwDecoder::~ArwDecoder(void) {
  if (mRootIFD)
    delete mRootIFD;
  mRootIFD = NULL;
}

RawImage ArwDecoder::decodeRawInternal() {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(STRIPOFFSETS);

  if (data.empty()) {
    TiffEntry *model = mRootIFD->getEntryRecursive(MODEL);

    if (model && model->getString() == "DSLR-A100") {
      // We've caught the elusive A100 in the wild, a transitional format
      // between the simple sanity of the MRW custom format and the wordly
      // wonderfullness of the Tiff-based ARW format, let's shoot from the hip
      uint32 off = mRootIFD->getEntryRecursive(SUBIFDS)->getInt();
      uint32 width = 3881;
      uint32 height = 2608;

      mRaw->dim = iPoint2D(width, height);
      mRaw->createData();
      // FIXME: there may be a better way to set the total max size;
      ByteStream input(mFile->getData(off),mFile->getSize()-off);

      try {
        DecodeARW(input, width, height);
      } catch (IOException &e) {
        mRaw->setError(e.what());
        // Let's ignore it, it may have delivered somewhat useful data.
      }

      return mRaw;
    } else {
      ThrowRDE("ARW Decoder: No image data found");
    }
  }

  TiffIFD* raw = data[0];
  int compression = raw->getEntry(COMPRESSION)->getInt();
  if (1 == compression) {
    // This is probably the SR2 format, let's pass it on
    try {
      DecodeSR2(raw);
    } catch (IOException &e) {
      mRaw->setError(e.what());
    }

    return mRaw;
  }
  if (32767 != compression)
    ThrowRDE("ARW Decoder: Unsupported compression");

  TiffEntry *offsets = raw->getEntry(STRIPOFFSETS);
  TiffEntry *counts = raw->getEntry(STRIPBYTECOUNTS);

  if (offsets->count != 1) {
    ThrowRDE("ARW Decoder: Multiple Strips found: %u", offsets->count);
  }
  if (counts->count != offsets->count) {
    ThrowRDE("ARW Decoder: Byte count number does not match strip size: count:%u, strips:%u ", counts->count, offsets->count);
  }
  uint32 width = raw->getEntry(IMAGEWIDTH)->getInt();
  uint32 height = raw->getEntry(IMAGELENGTH)->getInt();
  uint32 bitPerPixel = raw->getEntry(BITSPERSAMPLE)->getInt();

  // Sony E-550 marks compressed 8bpp ARW with 12 bit per pixel
  // this makes the compression detect it as a ARW v1.
  // This camera has however another MAKER entry, so we MAY be able
  // to detect it this way in the future.
  data = mRootIFD->getIFDsWithTag(MAKE);
  if (data.size() > 1) {
    for (uint32 i = 0; i < data.size(); i++) {
      string make = data[i]->getEntry(MAKE)->getString();
      /* Check for maker "SONY" without spaces */
      if (!make.compare("SONY"))
        bitPerPixel = 8;
    }
  }

  bool arw1 = counts->getInt() * 8 != width * height * bitPerPixel;
  if (arw1)
    height += 8;

  mRaw->dim = iPoint2D(width, height);
  mRaw->createData();

  ushort16 curve[0x4001];
  const ushort16* c = raw->getEntry(SONY_CURVE)->getShortArray();
  uint32 sony_curve[] = { 0, 0, 0, 0, 0, 4095 };

  for (uint32 i = 0; i < 4; i++)
    sony_curve[i+1] = (c[i] >> 2) & 0xfff;

  for (uint32 i = 0; i < 0x4001; i++)
    curve[i] = i;

  for (uint32 i = 0; i < 5; i++)
    for (uint32 j = sony_curve[i] + 1; j <= sony_curve[i+1]; j++)
      curve[j] = curve[j-1] + (1 << i);

  if (!uncorrectedRawValues)
    mRaw->setTable(curve, 0x4000, true);

  uint32 c2 = counts->getInt();
  uint32 off = offsets->getInt();

  if (!mFile->isValid(off))
    ThrowRDE("Sony ARW decoder: Data offset after EOF, file probably truncated");

  if (!mFile->isValid(off + c2))
    c2 = mFile->getSize() - off;


  ByteStream input(mFile->getData(off), c2);
 
  try {
    if (arw1)
      DecodeARW(input, width, height);
    else
      DecodeARW2(input, width, height, bitPerPixel);
  } catch (IOException &e) {
    mRaw->setError(e.what());
    // Let's ignore it, it may have delivered somewhat useful data.
  }

  // Set the table, if it should be needed later.
  if (uncorrectedRawValues) {
    mRaw->setTable(curve, 0x4000, false);
  } else {
    mRaw->setTable(NULL);
  }

  return mRaw;
}

void ArwDecoder::DecodeSR2(TiffIFD* raw) {
  uint32 width = raw->getEntry(IMAGEWIDTH)->getInt();
  uint32 height = raw->getEntry(IMAGELENGTH)->getInt();
  uint32 off = raw->getEntry(STRIPOFFSETS)->getInt();
  uint32 c2 = raw->getEntry(STRIPBYTECOUNTS)->getInt();

  mRaw->dim = iPoint2D(width, height);
  mRaw->createData();
  ByteStream input(mFile->getData(off), c2);

  Decode14BitRawBEunpacked(input, width, height);
}

void ArwDecoder::DecodeARW(ByteStream &input, uint32 w, uint32 h) {
  BitPumpMSB bits(&input);
  uchar8* data = mRaw->getData();
  ushort16* dest = (ushort16*) & data[0];
  uint32 pitch = mRaw->pitch / sizeof(ushort16);
  int sum = 0;
  for (uint32 x = w; x--;)
    for (uint32 y = 0; y < h + 1; y += 2) {
      bits.checkPos();
      bits.fill();
      if (y == h) y = 1;
      uint32 len = 4 - bits.getBitsNoFill(2);
      if (len == 3 && bits.getBitNoFill()) len = 0;
      if (len == 4)
        while (len < 17 && !bits.getBitNoFill()) len++;
      int diff = bits.getBits(len);
      if (len && (diff & (1 << (len - 1))) == 0)
        diff -= (1 << len) - 1;
      sum += diff;
      _ASSERTE(!(sum >> 12));
      if (y < h) dest[x+y*pitch] = sum;
    }
}

void ArwDecoder::DecodeARW2(ByteStream &input, uint32 w, uint32 h, uint32 bpp) {

  if (bpp == 8) {
    in = &input;
    this->startThreads();
    return;
  } // End bpp = 8

  if (bpp == 12) {
    uchar8* data = mRaw->getData();
    uint32 pitch = mRaw->pitch;
    const uchar8 *in = input.getData();

    if (input.getRemainSize() < (w * 3 / 2))
      ThrowRDE("Sony Decoder: Image data section too small, file probably truncated");

    if (input.getRemainSize() < (w*h*3 / 2))
      h = input.getRemainSize() / (w * 3 / 2) - 1;

    for (uint32 y = 0; y < h; y++) {
      ushort16* dest = (ushort16*) & data[y*pitch];
      for (uint32 x = 0 ; x < w; x += 2) {
        uint32 g1 = *in++;
        uint32 g2 = *in++;
        dest[x] = (g1 | ((g2 & 0xf) << 8));
        uint32 g3 = *in++;
        dest[x+1] = ((g2 >> 4) | (g3 << 4));
      }
    }
    // Shift scales, since black and white are the same as compressed precision
    mShiftDownScale = 2;
    return;
  }
  ThrowRDE("Unsupported bit depth");
}

void ArwDecoder::checkSupportInternal(CameraMetaData *meta) {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);
  if (data.empty())
    ThrowRDE("ARW Support check: Model name found");
  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();
  this->checkCameraSupported(meta, make, model, "");
}

void ArwDecoder::decodeMetaDataInternal(CameraMetaData *meta) {
  //Default
  int iso = 0;

  mRaw->cfa.setCFA(iPoint2D(2,2), CFA_RED, CFA_GREEN, CFA_GREEN2, CFA_BLUE);
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);

  if (data.empty())
    ThrowRDE("ARW Meta Decoder: Model name found");
  if (!data[0]->hasEntry(MAKE))
    ThrowRDE("ARW Decoder: Make name not found");

  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();

  if (mRootIFD->hasEntryRecursive(ISOSPEEDRATINGS))
    iso = mRootIFD->getEntryRecursive(ISOSPEEDRATINGS)->getInt();

  setMetaData(meta, make, model, "", iso);
  mRaw->whitePoint >>= mShiftDownScale;
  mRaw->blackLevel >>= mShiftDownScale;
}

/* Since ARW2 compressed images have predictable offsets, we decode them threaded */

void ArwDecoder::decodeThreaded(RawDecoderThread * t) {
  uchar8* data = mRaw->getData();
  uint32 pitch = mRaw->pitch;
  uint32 w = mRaw->dim.x;

  BitPumpPlain bits(in);
  for (uint32 y = t->start_y; y < t->end_y; y++) {
    ushort16* dest = (ushort16*) & data[y*pitch];
    // Realign
    bits.setAbsoluteOffset((w*8*y) >> 3);
    uint32 random = bits.peekBits(24);

    // Process 32 pixels (16x2) per loop.
    for (uint32 x = 0; x < w - 30;) {
      bits.checkPos();
      int _max = bits.getBits(11);
      int _min = bits.getBits(11);
      int _imax = bits.getBits(4);
      int _imin = bits.getBits(4);
      int sh;
      for (sh = 0; sh < 4 && 0x80 << sh <= _max - _min; sh++);
      for (int i = 0; i < 16; i++) {
        int p;
        if (i == _imax) p = _max;
        else if (i == _imin) p = _min;
        else {
          p = (bits.getBits(7) << sh) + _min;
          if (p > 0x7ff)
            p = 0x7ff;
        }
        mRaw->setWithLookUp(p << 1, (uchar8*)&dest[x+i*2], &random);
      }
      x += x & 1 ? 31 : 1;  // Skip to next 32 pixels
    }
  }
}

} // namespace RawSpeed
