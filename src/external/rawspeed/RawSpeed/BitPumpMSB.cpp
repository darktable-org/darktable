#include "StdAfx.h"
#include "BitPumpMSB.h"

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

/*** Used for entropy encoded sections ***/


BitPumpMSB::BitPumpMSB(ByteStream *s):
    buffer(s->getData()), size(s->getRemainSize() + sizeof(uint32)), mLeft(0), off(0) {
  init();
}

BitPumpMSB::BitPumpMSB(const uchar8* _buffer, uint32 _size) :
    buffer(_buffer), size(_size + sizeof(uint32)), mLeft(0), off(0) {
  init();
}

__inline void BitPumpMSB::init() {
  mStuffed = 0;
  current_buffer = (uchar8*)_aligned_malloc(16, 16);
  if (!current_buffer)
    ThrowRDE("BitPumpMSB::init(): Unable to allocate memory");
  memset(current_buffer,0,16);
  fill();
}

void BitPumpMSB::fill()
{
  if (mLeft >=24)
    return;
  // Fill in 96 bits
  int* b = (int*)current_buffer;
  if ((off + 12) > size) {
    while(mLeft <= 64 && off < size) {
      for (int i = (mLeft>>3); i >= 0; i--)
        current_buffer[i+1] = current_buffer[i];
      current_buffer[0] = buffer[off++];
      mLeft+=8;
    }
    while (mLeft <= 64) {
      b[3] = b[2];
      b[2] = b[1];
      b[1] = b[0];
      b[0] = 0;
      mLeft +=32;
      mStuffed += 4;
    }
    return;
  }
  b[3] = b[0];
#if defined(LE_PLATFORM_HAS_BSWAP)
  b[2] = PLATFORM_BSWAP32(*(int*)&buffer[off]);
  b[1] = PLATFORM_BSWAP32(*(int*)&buffer[off+4]);
  b[0] = PLATFORM_BSWAP32(*(int*)&buffer[off+8]);
  off+=12;
#else
  b[2] = (buffer[off] << 24) | (buffer[off+1] << 16)  | (buffer[off+2] << 8) | buffer[off+3];
  off+=4;
  b[1] = (buffer[off] << 24) | (buffer[off+1] << 16)  | (buffer[off+2] << 8) | buffer[off+3];
  off+=4;
  b[0] = (buffer[off] << 24) | (buffer[off+1] << 16)  | (buffer[off+2] << 8) | buffer[off+3];
  off+=4;
#endif
  mLeft+=96;
}


uint32 BitPumpMSB::getBitSafe() {
  fill();
  checkPos();

  return getBitNoFill();
}

uint32 BitPumpMSB::getBitsSafe(unsigned int nbits) {
  if (nbits > MIN_GET_BITS)
    ThrowIOE("Too many bits requested");

  fill();
  checkPos();
  return getBitsNoFill(nbits);
}


uchar8 BitPumpMSB::getByteSafe() {
  fill();
  checkPos();
  return getBitsNoFill(8);
}

void BitPumpMSB::setAbsoluteOffset(unsigned int offset) {
  if (offset >= size)
    ThrowIOE("Offset set out of buffer");

  mLeft = 0;
  mStuffed = 0;
  off = offset;
  fill();
}



BitPumpMSB::~BitPumpMSB(void) {
	_aligned_free(current_buffer);
}

} // namespace RawSpeed

