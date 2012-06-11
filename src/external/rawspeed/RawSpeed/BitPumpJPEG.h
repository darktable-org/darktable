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
#ifndef BIT_PUMP_JPEG_H
#define BIT_PUMP_JPEG_H

#include "ByteStream.h"
#include "IOException.h"

namespace RawSpeed {

// Note: Allocated buffer MUST be at least size+sizeof(uint32) large.

class BitPumpJPEG
{
public:
  BitPumpJPEG(ByteStream *s);
  BitPumpJPEG(const uchar8* _buffer, uint32 _size );
	uint32 getBits(uint32 nbits);
	uint32 getBit();
	uint32 getBitsSafe(uint32 nbits);
	uint32 getBitSafe();
	uint32 peekBits(uint32 nbits);
	uint32 peekBit();
  uint32 peekByte();
  void skipBits(uint32 nbits);
  __inline void skipBitsNoFill(uint32 nbits){ mLeft -= nbits; }
  __inline void checkPos()  { if (off>size) throw IOException("Out of buffer read");};        // Check if we have a valid position
	uchar8 getByte();
	uchar8 getByteSafe();
	void setAbsoluteOffset(uint32 offset);     // Set offset in bytes
  uint32 getOffset() { return off-(mLeft>>3)+stuffed;}
  __inline uint32 getBitNoFill() {return (mCurr >> (--mLeft)) & 1;}
  __inline uint32 peekByteNoFill() {return ((mCurr >> (mLeft-8))) & 0xff; }
  __inline uint32 peekBitsNoFill(uint32 nbits) {return ((mCurr >> (mLeft-nbits))) & ((1 << nbits) - 1); }
  __inline uint32 getBitsNoFill(uint32 nbits) { return ((mCurr >> (mLeft -= (nbits)))) & ((1 << nbits) - 1);}

#define TEST_IF_FF(VAL) if (VAL == 0xFF) {\
  if (buffer[off] == 0)\
  off++;\
  else  {\
  VAL = 0;off--;stuffed++;\
  }\
  }


  // Fill the buffer with at least 24 bits
  __inline void fill() {
    uchar8 c, c2, c3;

    int m = mLeft >> 3;

    if (mLeft > 23)
      return;

    if (m == 2)
    {
      // 16 to 23 bits left, we can add 1 byte
      c = buffer[off++];
      TEST_IF_FF(c);
      mCurr = (mCurr << 8) | c;
      mLeft += 8;
      return;
    }

    if (m == 1)
    {
      // 8 to 15 bits left, we can add 2 bytes
      c = buffer[off++];
      TEST_IF_FF(c);
      c2 = buffer[off++];
      TEST_IF_FF(c2);
      mCurr = (mCurr << 16) | (c<<8) | c2;
      mLeft += 16;
      return;
    }

    // 0 to 7 bits left, we can add 3 bytes
    c = buffer[off++];
    TEST_IF_FF(c);
    c2 = buffer[off++];
    TEST_IF_FF(c2);
    c3 = buffer[off++];
    TEST_IF_FF(c3);
    mCurr = (mCurr << 24) | (c<<16) | (c2<<8) | c3;
    mLeft += 24;
  }

#undef TEST_IF_FF

  virtual ~BitPumpJPEG(void);
protected:
  void __inline init();
  const uchar8* buffer;
  const uint32 size;            // This if the end of buffer.
  uint32 mLeft;
  uint32 mCurr;
  uint32 off;                  // Offset in bytes
  uint32 stuffed;              // How many bytes has been stuffed?
private:
};

} // namespace RawSpeed

#endif
