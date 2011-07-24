#include "StdAfx.h"
#include "BitPumpMSB32.h"
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

/*** Used for entropy encoded sections, for now only Nikon Coolpix ***/


BitPumpMSB32::BitPumpMSB32(ByteStream *s):
    buffer(s->getData()), size(s->getRemainSize() + sizeof(uint32)), mLeft(0), mCurr(0), off(0) {
  init();
}

BitPumpMSB32::BitPumpMSB32(const uchar8* _buffer, uint32 _size) :
    buffer(_buffer), size(_size + sizeof(uint32)), mLeft(0), mCurr(0), off(0) {
  init();
}

__inline void BitPumpMSB32::init() {
  fill();
}

void BitPumpMSB32::fill()
{
  uint32 c, c2, c3, c4;

  if (mLeft >= MIN_GET_BITS)
    return;

  c = buffer[off++];
  c2 = buffer[off++];
  c3 = buffer[off++];
  c4 = buffer[off++];
  mCurr <<= 32;
  mCurr |= (c4 << 24) | (c3<<16) | (c2<<8) | c;
  mLeft += 32;
}

uint32 BitPumpMSB32::getBitsSafe(unsigned int nbits) {
  if (nbits > MIN_GET_BITS)
    throw IOException("Too many bits requested");

  if (mLeft < nbits) {
    fill();
    checkPos();
  }

  return (uint32)((mCurr >> (mLeft -= (nbits))) & ((1 << nbits) - 1));
}


void BitPumpMSB32::setAbsoluteOffset(unsigned int offset) {
  if (offset >= size)
    throw IOException("Offset set out of buffer");

  mLeft = 0;
  mCurr = 0;
  off = offset;
  fill();
}

BitPumpMSB32::~BitPumpMSB32(void) {
}


} // namespace RawSpeed
