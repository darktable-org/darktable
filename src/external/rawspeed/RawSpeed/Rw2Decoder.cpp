#include "StdAfx.h"
#include "Rw2Decoder.h"

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

Rw2Decoder::Rw2Decoder(TiffIFD *rootIFD, FileMap* file) :
    RawDecoder(file), mRootIFD(rootIFD), input_start(0) {
      decoderVersion = 1;
}
Rw2Decoder::~Rw2Decoder(void) {
  if (input_start)
    delete input_start;
  input_start = 0;
}

RawImage Rw2Decoder::decodeRaw() {

  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(PANASONIC_STRIPOFFSET);

  bool isOldPanasonic = FALSE;

  if (data.empty()) {
    if (!mRootIFD->hasEntryRecursive(STRIPOFFSETS))
      ThrowRDE("RW2 Decoder: No image data found");
    isOldPanasonic = TRUE;
    data = mRootIFD->getIFDsWithTag(STRIPOFFSETS);
  }

  TiffIFD* raw = data[0];
  uint32 height = raw->getEntry((TiffTag)3)->getShort();
  uint32 width = raw->getEntry((TiffTag)2)->getShort();

  if (isOldPanasonic) {
    ThrowRDE("Cannot decoder old-style Panasonic RAW files");
    TiffEntry *offsets = raw->getEntry(STRIPOFFSETS);
    TiffEntry *counts = raw->getEntry(STRIPBYTECOUNTS);

    if (offsets->count != 1) {
      ThrowRDE("RW2 Decoder: Multiple Strips found: %u", offsets->count);
    }
    int off = offsets->getInt();
    if (!mFile->isValid(off))
      ThrowRDE("Panasonic RAW Decoder: Invalid image data offset, cannot decode.");

    int count = counts->getInt();
    if (count != (int)(width*height*2))
      ThrowRDE("Panasonic RAW Decoder: Byte count is wrong.");

    if (!mFile->isValid(off+count))
      ThrowRDE("Panasonic RAW Decoder: Invalid image data offset, cannot decode.");
      
    mRaw->dim = iPoint2D(width, height);
    mRaw->createData();
    ByteStream input_start(mFile->getData(off), mFile->getSize() - off);
    iPoint2D pos(0, 0);
    readUncompressedRaw(input_start, mRaw->dim,pos, width*2, 16, FALSE);

  } else {

    mRaw->dim = iPoint2D(width, height);
    mRaw->createData();
    TiffEntry *offsets = raw->getEntry(PANASONIC_STRIPOFFSET);

    if (offsets->count != 1) {
      ThrowRDE("RW2 Decoder: Multiple Strips found: %u", offsets->count);
    }

    load_flags = 0x2008;
    int off = offsets->getInt();

    if (!mFile->isValid(off))
      ThrowRDE("RW2 Decoder: Invalid image data offset, cannot decode.");

    input_start = new ByteStream(mFile->getData(off), mFile->getSize() - off);
    DecodeRw2();
  }
  return mRaw;
}

void Rw2Decoder::DecodeRw2() {
  startThreads();
}

void Rw2Decoder::decodeThreaded(RawDecoderThread * t) {
  int x, i, j, sh = 0, pred[2], nonz[2];
  int w = mRaw->dim.x / 14;
  uint32 y;

  /* 9 + 1/7 bits per pixel */
  int skip = w * 14 * t->start_y * 9;
  skip += w * 2 * t->start_y;
  skip /= 8;

  PanaBitpump bits(new ByteStream(input_start));
  bits.load_flags = load_flags;
  bits.skipBytes(skip);

  for (y = t->start_y; y < t->end_y; y++) {
    ushort16* dest = (ushort16*)mRaw->getData(0, y);
    for (x = 0; x < w; x++) {
      pred[0] = pred[1] = nonz[0] = nonz[1] = 0;
      int u = 0;
      for (i = 0; i < 14; i++) {
        // Even pixels
        if (u == 2)
        {
          sh = 4 >> (3 - bits.getBits(2));
          u = -1;
        }
        if (nonz[0]) {
          if ((j = bits.getBits(8))) {
            if ((pred[0] -= 0x80 << sh) < 0 || sh == 4)
              pred[0] &= ~(-1 << sh);
            pred[0] += j << sh;
          }
        } else if ((nonz[0] = bits.getBits(8)) || i > 11)
          pred[0] = nonz[0] << 4 | bits.getBits(4);
        *dest++ = pred[0];

        // Odd pixels
        i++;
        u++;
        if (u == 2)
        {
          sh = 4 >> (3 - bits.getBits(2));
          u = -1;
        }
        if (nonz[1]) {
          if ((j = bits.getBits(8))) {
            if ((pred[1] -= 0x80 << sh) < 0 || sh == 4)
              pred[1] &= ~(-1 << sh);
            pred[1] += j << sh;
          }
        } else if ((nonz[1] = bits.getBits(8)) || i > 11)
          pred[1] = nonz[1] << 4 | bits.getBits(4);
        *dest++ = pred[1];
        u++;
      }
    }
  }
}

void Rw2Decoder::checkSupport(CameraMetaData *meta) {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);
  if (data.empty())
    ThrowRDE("RW2 Support check: Model name found");

  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();
  if (!this->checkCameraSupported(meta, make, model, getMode(model)))
    this->checkCameraSupported(meta, make, model, "");
}

void Rw2Decoder::decodeMetaData(CameraMetaData *meta) {
  mRaw->cfa.setCFA(CFA_BLUE, CFA_GREEN, CFA_GREEN2, CFA_RED);
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);

  if (data.empty())
    ThrowRDE("CR2 Meta Decoder: Model name not found");

  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();
  string mode = getMode(model);
  if (this->checkCameraSupported(meta, make, model, getMode(model)))
    setMetaData(meta, make, model, mode);
  else
    setMetaData(meta, make, model, "");
}

bool Rw2Decoder::almostEqualRelative(float A, float B, float maxRelativeError) {
  if (A == B)
    return true;

  float relativeError = fabs((A - B) / B);
  if (relativeError <= maxRelativeError)
    return true;
  return false;
}

std::string Rw2Decoder::getMode(const string model) {
  float ratio = 3.0f / 2.0f;  // Default
  if (mRaw->isAllocated()) {
    ratio = (float)mRaw->dim.x / (float)mRaw->dim.y;
  }

  if (almostEqualRelative(ratio, 16.0f / 9.0f, 0.02f))
    return "16:9";
  if (almostEqualRelative(ratio, 3.0f / 2.0f, 0.02f))
    return "3:2";
  if (almostEqualRelative(ratio, 4.0f / 3.0f, 0.02f))
    return "4:3";
  if (almostEqualRelative(ratio, 1.0f, 0.02f))
    return "1:1";

  return "";
}

PanaBitpump::PanaBitpump(ByteStream* _input) : input(_input), vbits(0) {
}

PanaBitpump::~PanaBitpump() {
  if (input)
    delete input;
  input = 0;
}

void PanaBitpump::skipBytes(int bytes) {
  int blocks = (bytes / 0x4000) * 0x4000;
  input->skipBytes(blocks);
  for (int i = blocks; i < bytes; i++)
    getBits(8);
}

uint32 PanaBitpump::getBits(int nbits) {
  int byte;

  if (!vbits) {
    /* On truncated files this routine will just return just for the truncated
    * part of the file. Since there is no chance of affecting output buffer
    * size we allow the decoder to decode this
    */
    if (input->getRemainSize() < 0x4000 - load_flags) {
      memcpy(buf + load_flags, input->getData(), input->getRemainSize());
      input->skipBytes(input->getRemainSize());
    } else {
      memcpy(buf + load_flags, input->getData(), 0x4000 - load_flags);
      input->skipBytes(0x4000 - load_flags);
      if (input->getRemainSize() < load_flags) {
        memcpy(buf, input->getData(), input->getRemainSize());
        input->skipBytes(input->getRemainSize());
      } else {
        memcpy(buf, input->getData(), load_flags);
        input->skipBytes(load_flags);
      }
    }
  }
  vbits = (vbits - nbits) & 0x1ffff;
  byte = vbits >> 3 ^ 0x3ff0;
  return (buf[byte] | buf[byte+1] << 8) >> (vbits & 7) & ~(-1 << nbits);
}

} // namespace RawSpeed
