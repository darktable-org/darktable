#include "StdAfx.h"
#include "BitPumpMSB16.h"
/*
    RawSpeed - RAW file decoder.

    Copyright (C) 2009-2014 Klaus Post

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

/*** Used for entropy encoded sections, for now only Nikon Coolpix ***/


BitPumpMSB16::BitPumpMSB16(ByteStream *s):
    buffer(s->getData()), size(s->getRemainSize() + sizeof(uint32)), mLeft(0), mCurr(0), off(0) {
  init();
}

BitPumpMSB16::BitPumpMSB16(const uchar8* _buffer, uint32 _size) :
    buffer(_buffer), size(_size + sizeof(uint32)), mLeft(0), mCurr(0), off(0) {
  init();
}

__inline void BitPumpMSB16::init() {
  mStuffed = 0;
  _fill();
}

void BitPumpMSB16::_fill()
{
  uint32 c, c2;
  if ((off + 4) > size) {
    while (off < size) {
      mCurr <<= 8;
      c = buffer[off++];
      mCurr |= c;
      mLeft += 8;
    }
    while (mLeft < MIN_GET_BITS) {
      mCurr <<= 8;
      mLeft += 8;
      mStuffed++;
    }
    return;
  }
  c = buffer[off++];
  c2 = buffer[off++];
  mCurr <<= 16;
  mCurr |= (c2<<8) | c;
  mLeft += 16;
}

uint32 BitPumpMSB16::getBitsSafe(unsigned int nbits) {
  if (nbits > MIN_GET_BITS)
    throw IOException("Too many bits requested");

  if (mLeft < nbits) {
    _fill();
    checkPos();
  }

  return (uint32)((mCurr >> (mLeft -= (nbits))) & ((1 << nbits) - 1));
}


void BitPumpMSB16::setAbsoluteOffset(unsigned int offset) {
  if (offset >= size)
    throw IOException("Offset set out of buffer");

  mLeft = 0;
  mCurr = 0;
  off = offset;
  mStuffed = 0;
  _fill();
}

BitPumpMSB16::~BitPumpMSB16(void) {
}


} // namespace RawSpeed
