#include "StdAfx.h"
#include "BitPumpJPEG.h"
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

    This is based on code by Hubert Figuiere.
    Copyright (C) 2007 Hubert Figuiere, released under LGPL
*/

namespace RawSpeed {

/*** Used for entropy encoded sections ***/

#define BITS_PER_LONG (8*sizeof(uint32))
#define MIN_GET_BITS  (BITS_PER_LONG-7)    /* max value for long getBuffer */


BitPumpJPEG::BitPumpJPEG(ByteStream *s):
    buffer(s->getData()), size(s->getRemainSize() + sizeof(uint32)), mLeft(0), mCurr(0), off(0) {
  init();
}


BitPumpJPEG::BitPumpJPEG(const uchar8* _buffer, uint32 _size) :
    buffer(_buffer), size(_size + sizeof(uint32)), mLeft(0), mCurr(0), off(0) {
  init();
}


void __inline BitPumpJPEG::init() {
  stuffed = 0;
  fill();
}

uint32 BitPumpJPEG::getBit() {
  if (!mLeft) fill();

  return (mCurr >> (--mLeft)) & 1;
}


uint32 BitPumpJPEG::getBits(uint32 nbits) {
  _ASSERTE(nbits < 24);

  if (mLeft < nbits) {
    fill();
  }
  return ((mCurr >> (mLeft -= (nbits)))) & ((1 << nbits) - 1);
}


uint32 BitPumpJPEG::peekBit() {
  if (!mLeft) fill();
  return (mCurr >> (mLeft - 1)) & 1;
}


uint32 BitPumpJPEG::peekBits(uint32 nbits) {
  if (mLeft < nbits) {
    fill();
  }
  return ((mCurr >> (mLeft - nbits))) & ((1 << nbits) - 1);
}


uint32 BitPumpJPEG::peekByte() {
  if (mLeft < 8) {
    fill();
  }
  if (off > size)
    throw IOException("Out of buffer read");

  return ((mCurr >> (mLeft - 8))) & 0xff;
}


uint32 BitPumpJPEG::getBitSafe() {
  if (!mLeft) {
    fill();
    if (off > size)
      throw IOException("Out of buffer read");
  }

  return (mCurr >> (--mLeft)) & 1;
}


uint32 BitPumpJPEG::getBitsSafe(unsigned int nbits) {
  if (nbits > MIN_GET_BITS)
    throw IOException("Too many bits requested");

  if (mLeft < nbits) {
    fill();
    checkPos();
  }
  return ((mCurr >> (mLeft -= (nbits)))) & ((1 << nbits) - 1);
}


void BitPumpJPEG::skipBits(unsigned int nbits) {
  _ASSERTE(nbits < 24);

  if (mLeft < nbits) {
    fill();
    checkPos();
  }

  mLeft -= nbits;
}


uchar8 BitPumpJPEG::getByte() {
  if (mLeft < 8) {
    fill();
  }

  return ((mCurr >> (mLeft -= 8))) & 0xff;
}


uchar8 BitPumpJPEG::getByteSafe() {
  if (mLeft < 8) {
    fill();
    checkPos();
  }

  return ((mCurr >> (mLeft -= 8))) & 0xff;
}


void BitPumpJPEG::setAbsoluteOffset(unsigned int offset) {
  if (offset >= size)
    throw IOException("Offset set out of buffer");

  mLeft = 0;
  off = offset;
  fill();
}


BitPumpJPEG::~BitPumpJPEG(void) {
}

} // namespace RawSpeed
