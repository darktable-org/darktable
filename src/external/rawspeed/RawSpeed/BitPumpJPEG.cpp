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
*/

namespace RawSpeed {

/*** Used for entropy encoded sections ***/


BitPumpJPEG::BitPumpJPEG(ByteStream *s):
    buffer(s->getData()), size(s->getRemainSize() + sizeof(uint32)), mLeft(0), off(0), stuffed(0) {
  init();
}

BitPumpJPEG::BitPumpJPEG(const uchar8* _buffer, uint32 _size) :
    buffer(_buffer), size(_size + sizeof(uint32)), mLeft(0), off(0), stuffed(0) {
  init();
}

__inline void BitPumpJPEG::init() {
  current_buffer = (uchar8*)_aligned_malloc(16, 16);
  if (!current_buffer)
    ThrowRDE("BitPumpJPEG::init(): Unable to allocate memory");
  memset(current_buffer,0,16);
  fill();
}

void BitPumpJPEG::fill()
{
  if (mLeft >=24)
    return;
  // Fill in 96 bits
  int* b = (int*)current_buffer;
  if ((off + 12) >= size) {
    while(mLeft <= 64 && off < size) {
      for (int i = (mLeft>>3); i >= 0; i--)
        current_buffer[i+1] = current_buffer[i];
      uchar8 val = buffer[off++];
      if (val == 0xff) {
        if (buffer[off] == 0)
          off++;
        else {
          // We hit another marker - don't forward bitpump anymore
          val = 0;
          off--;
          stuffed++;
        }
      }
      current_buffer[0] = val;
      mLeft+=8;
    }
    while (mLeft < 64) {
      b[2] = b[1];
      b[1] = b[0];
      b[0] = 0;
      mLeft +=32;
      stuffed +=4;  //We are adding to mLeft without incrementing offset
    }
    return;
  }
  b[3] = b[0];
  for (int i = 0; i < 12; i++) {
    uchar8 val = buffer[off++];
    if (val == 0xff) {
      if (buffer[off] == 0)
        off++;
      else {
        val = 0;
        off--;
        stuffed++;
      }
    }
    current_buffer[11-i] = val;
  } 
  mLeft+=96;
}


uint32 BitPumpJPEG::getBitSafe() {
  fill();
  checkPos();

  return getBitNoFill();
}

uint32 BitPumpJPEG::getBitsSafe(unsigned int nbits) {
  if (nbits > MIN_GET_BITS)
    throw IOException("Too many bits requested");

  fill();
  checkPos();
  return getBitsNoFill(nbits);
}


uchar8 BitPumpJPEG::getByteSafe() {
  fill();
  checkPos();
  return getBitsNoFill(8);
}

void BitPumpJPEG::setAbsoluteOffset(unsigned int offset) {
  if (offset >= size)
    throw IOException("Offset set out of buffer");

  mLeft = 0;
  off = offset;
  fill();
}



BitPumpJPEG::~BitPumpJPEG(void) {
	_aligned_free(current_buffer);
}

} // namespace RawSpeed

