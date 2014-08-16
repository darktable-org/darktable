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
#ifndef BIT_PUMP_MSB16_H
#define BIT_PUMP_MSB16_H

#include "ByteStream.h"

#ifdef MIN_GET_BITS
#undef MIN_GET_BITS
#endif

#define BITS_PER_LONG_LONG (8*sizeof(uint64))
#define MIN_GET_BITS  (BITS_PER_LONG_LONG-33)    /* max value for long getBuffer */

namespace RawSpeed {

// Note: Allocated buffer MUST be at least size+sizeof(uint32) large.

class BitPumpMSB16
{
public:
  BitPumpMSB16(ByteStream *s);
  BitPumpMSB16(const uchar8* _buffer, uint32 _size );
	uint32 getBitsSafe(uint32 nbits);
	uint32 getBitSafe();
	uchar8 getByteSafe();
	void setAbsoluteOffset(uint32 offset);     // Set offset in bytes
  __inline uint32 getOffset() { return off-(mLeft>>3);}
  __inline void checkPos()  { if (mStuffed > 3) throw IOException("Out of buffer read");};        // Check if we have a valid position

  // Fill the buffer with at least 24 bits
  __inline void fill() {  if (mLeft < MIN_GET_BITS) _fill();};
  void _fill();

  __inline uint32 getBit() {
    if (!mLeft) _fill();

    return (uint32)((mCurr >> (--mLeft)) & 1);
  }

  __inline uint32 getBitNoFill() {
    return (uint32)((mCurr >> (--mLeft)) & 1);
  }

  __inline uint32 getBits(uint32 nbits) {
    if (mLeft < nbits) {
      _fill();
    }

    return (uint32)((mCurr >> (mLeft -= (nbits))) & ((1 << nbits) - 1));
  }

  __inline uint32 getBitsNoFill(uint32 nbits) {
    return (uint32)((mCurr >> (mLeft -= (nbits))) & ((1 << nbits) - 1));
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
  __inline uint32 peekByteNoFill() {
    return (uint32)((mCurr >> (mLeft-8)) & 0xff);
  }

  __inline void skipBitsNoFill(uint32 nbits) {
    mLeft -= nbits;
  }

  virtual ~BitPumpMSB16(void);
protected:
  void __inline init();
  const uchar8* buffer;
  const uint32 size;            // This if the end of buffer.
  uint32 mLeft;
  uint64 mCurr;
  uint32 off;                  // Offset in bytes
  uint32 mStuffed;
private:
};

} // namespace RawSpeed

#endif
