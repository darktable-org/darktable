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
#ifndef BIT_PUMP_MSB32_H
#define BIT_PUMP_MSB32_H

#include "ByteStream.h"

#ifdef MIN_GET_BITS
#undef MIN_GET_BITS
#endif

#define BITS_PER_LONG_LONG (8*sizeof(uint64))
#define MIN_GET_BITS  (BITS_PER_LONG_LONG-33)    /* max value for long getBuffer */

namespace RawSpeed {

// Note: Allocated buffer MUST be at least size+sizeof(uint32) large.

class BitPumpMSB32
{
public:
  BitPumpMSB32(ByteStream *s);
  BitPumpMSB32(const uchar8* _buffer, uint32 _size );
	uint32 getBitsSafe(uint32 nbits);
	uint32 getBitSafe();
	uchar8 getByteSafe();
	void setAbsoluteOffset(uint32 offset);     // Set offset in bytes
  __inline uint32 getOffset() { return off-(mLeft>>3);}
  __inline void checkPos()  { if (off>size) throw IOException("Out of buffer read");};        // Check if we have a valid position

  // Fill the buffer with at least 24 bits
void fill();

  __inline uint32 getBit() {
    if (!mLeft) fill();

    return (uint32)((mCurr >> (--mLeft)) & 1);
  }

  __inline uint32 getBits(uint32 nbits) {
    if (mLeft < nbits) {
      fill();
    }

    return (uint32)((mCurr >> (mLeft -= (nbits))) & ((1 << nbits) - 1));
  }

  virtual ~BitPumpMSB32(void);
protected:
  void __inline init();
  const uchar8* buffer;
  const uint32 size;            // This if the end of buffer.
  uint32 mLeft;
  uint64 mCurr;
  uint32 off;                  // Offset in bytes
private:
};

} // namespace RawSpeed

#endif
