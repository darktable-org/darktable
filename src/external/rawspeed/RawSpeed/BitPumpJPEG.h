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
#ifndef BIT_PUMP_JPEG_H
#define BIT_PUMP_JPEG_H

#include "ByteStream.h"

#define BITS_PER_LONG (8*sizeof(uint32))
#define MIN_GET_BITS  (BITS_PER_LONG-7)    /* max value for long getBuffer */

namespace RawSpeed {

// Note: Allocated buffer MUST be at least size+sizeof(uint32) large.

class BitPumpJPEG
{
public:
  BitPumpJPEG(ByteStream *s);
  BitPumpJPEG(const uchar8* _buffer, uint32 _size );
	uint32 getBitsSafe(uint32 nbits);
	uint32 getBitSafe();
	uchar8 getByteSafe();
	void setAbsoluteOffset(uint32 offset);     // Set offset in bytes
  __inline uint32 getOffset() { return off-(mLeft>>3)+stuffed;}
  __inline void checkPos()  { if (off>=size || stuffed > (mLeft>>3)) ThrowIOE("Out of buffer read");};        // Check if we have a valid position

  // Fill the buffer with at least 24 bits
  void fill() {if (mLeft<25) _fill();}
 __inline uint32 peekBitsNoFill( uint32 nbits )
 {
   int shift = mLeft-nbits;
   uint32 ret = *(uint32*)&current_buffer[shift>>3];
   ret >>= shift & 7;
   return ret & ((1 << nbits) - 1);
 }


__inline uint32 getBit() {
  if (!mLeft) _fill();
  mLeft--;
  uint32 _byte = mLeft >> 3;
  return (current_buffer[_byte] >> (mLeft & 0x7)) & 1;
}

__inline uint32 getBitsNoFill(uint32 nbits) {
	uint32 ret = peekBitsNoFill(nbits);
	mLeft -= nbits;
	return ret;
}
__inline uint32 getBits(uint32 nbits) {
	fill();
	return getBitsNoFill(nbits);
}

__inline uint32 peekBit() {
  if (!mLeft) _fill();
  return (current_buffer[(mLeft-1) >> 3] >> ((mLeft-1) & 0x7)) & 1;
}
__inline uint32 getBitNoFill() {
  mLeft--;
  uint32 ret = (current_buffer[mLeft >> 3] >> (mLeft & 0x7)) & 1;
  return ret;
}

__inline uint32 peekByteNoFill() {
  int shift = mLeft-8;
  uint32 ret = *(uint32*)&current_buffer[shift>>3];
  ret >>= shift & 7;
  return ret & 0xff;
}

__inline uint32 peekBits(uint32 nbits) {
  fill();
  return peekBitsNoFill(nbits);
}

__inline uint32 peekByte() {
   fill();
 
  if (off > size)
    throw IOException("Out of buffer read");

  return peekByteNoFill();
} 

  __inline void skipBits(unsigned int nbits) {
    int skipn = nbits;
    while (skipn) {
      fill();
      checkPos();
      int n = MIN(skipn, mLeft);
      mLeft -= n;
      skipn -= n;
    }
  }

  __inline void skipBitsNoFill(unsigned int nbits) {
    mLeft -= nbits;
  }

  __inline unsigned char getByte() {
    fill();
    mLeft-=8;
    int shift = mLeft;
    uint32 ret = *(uint32*)&current_buffer[shift>>3];
    ret >>= shift & 7;
    return ret & 0xff;
  }

  virtual ~BitPumpJPEG(void);
protected:
  void __inline init();
  void _fill();
  const uchar8* buffer;
  uchar8 current_buffer[16];
  const uint32 size;            // This if the end of buffer.
  int mLeft;
  uint32 off;                  // Offset in bytes
  int stuffed;              // How many bytes has been stuffed?
private:
};

} // namespace RawSpeed

#endif//BIT_PUMP_JPEG_H
