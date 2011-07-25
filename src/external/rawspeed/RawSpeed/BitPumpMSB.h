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
#ifndef BIT_PUMP_MSB_H
#define BIT_PUMP_MSB_H

#include "ByteStream.h"

#define BITS_PER_LONG (8*sizeof(uint32))
#define MIN_GET_BITS  (BITS_PER_LONG-7)    /* max value for long getBuffer */

namespace RawSpeed {

// Note: Allocated buffer MUST be at least size+sizeof(uint32) large.

class BitPumpMSB
{
public:
  BitPumpMSB(ByteStream *s);
  BitPumpMSB(const uchar8* _buffer, uint32 _size );
	uint32 getBitsSafe(uint32 nbits);
	uint32 getBitSafe();
	uchar8 getByteSafe();
	void setAbsoluteOffset(uint32 offset);     // Set offset in bytes
  __inline uint32 getOffset() { return off-(mLeft>>3);}
  __inline void checkPos()  { if (off>size) throw IOException("Out of buffer read");};        // Check if we have a valid position
  __inline uint32 getBitNoFill() {return (mCurr >> (--mLeft)) & 1;}
  __inline uint32 peekByteNoFill() {return ((mCurr >> (mLeft-8))) & 0xff; }
  __inline uint32 getBitsNoFill(uint32 nbits) {return ((mCurr >> (mLeft -= (nbits)))) & ((1 << nbits) - 1);}
  __inline uint32 peekBitsNoFill(uint32 nbits) {return ((mCurr >> (mLeft-nbits))) & ((1 << nbits) - 1); }

  // Fill the buffer with at least 24 bits
__inline void fill() {
  unsigned char c, c2, c3;
  int m = mLeft >> 3;

  if (mLeft > 23)
    return;

  if (m == 2) {
     // 16 to 23 bits left, we can add 1 byte
     c = buffer[off++];
     mCurr = (mCurr << 8) | c;
     mLeft += 8;
     return;
  }

  if (m == 1) {
    // 8 to 15 bits left, we can add 2 bytes
      c = buffer[off++];
	  c2 = buffer[off++];
	  mCurr = (mCurr << 16) | (c<<8) | c2;
	  mLeft += 16;
	  return;
  }

  // 0 to 7 bits left, we can add 3 bytes
  c = buffer[off++];
  c2 = buffer[off++];
  c3 = buffer[off++];
  mCurr = (mCurr << 24) | (c<<16) | (c2<<8) | c3;
  mLeft += 24;

}

  __inline uint32 getBit() {
    if (!mLeft) fill();

    return (mCurr >> (--mLeft)) & 1;
  }

  __inline uint32 getBits(uint32 nbits) {
    if (mLeft < nbits) {
      fill();
    }

    return ((mCurr >> (mLeft -= (nbits)))) & ((1 << nbits) - 1);
  }

  __inline uint32 peekBit() {
    if (!mLeft) fill();

    return (mCurr >> (mLeft - 1)) & 1;
  }

  __inline uint32 peekBits(uint32 nbits) {
    if (mLeft < nbits) {
      fill();
    }

    return ((mCurr >> (mLeft - nbits))) & ((1 << nbits) - 1);
  }

  __inline uint32 peekByte() {
    if (mLeft < 8) {
      fill();
    }

    if (off > size)
      throw IOException("Out of buffer read");

    return ((mCurr >> (mLeft - 8))) & 0xff;
  }

  __inline void skipBits(unsigned int nbits) {
    while (nbits) {
      fill();
      checkPos();
      int n = MIN(nbits, mLeft);
      mLeft -= n;
      nbits -= n;
    }
  }

  __inline void skipBitsNoFill(unsigned int nbits) {
    mLeft -= nbits;
  }

  __inline unsigned char getByte() {
    if (mLeft < 8) {
      fill();
    }

    return ((mCurr >> (mLeft -= 8))) & 0xff;
  }

  virtual ~BitPumpMSB(void);
protected:
  void __inline init();
  const uchar8* buffer;
  const uint32 size;            // This if the end of buffer.
  uint32 mLeft;
  uint32 mCurr;
  uint32 off;                  // Offset in bytes
private:
};

} // namespace RawSpeed

#endif
