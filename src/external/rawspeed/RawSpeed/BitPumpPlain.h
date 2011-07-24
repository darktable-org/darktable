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
#ifndef BIT_PUMP_PLAIN_H
#define BIT_PUMP_PLAIN_H

#include "ByteStream.h"

namespace RawSpeed {

// Note: Allocated buffer MUST be at least size+sizeof(uint32) large.

class BitPumpPlain
{
public:
  BitPumpPlain(ByteStream *s);
  BitPumpPlain(const uchar8* _buffer, uint32 _size );
	uint32 getBits(uint32 nbits);
	uint32 getBit();
	uint32 getBitsSafe(uint32 nbits);
	uint32 getBitSafe();
	uint32 peekBits(uint32 nbits);
	uint32 peekBit();
  uint32 peekByte();
  void skipBits(uint32 nbits);
	uchar8 getByte();
	uchar8 getByteSafe();
	void setAbsoluteOffset(uint32 offset);
  uint32 getOffset() { return off>>3;}
  __inline void checkPos()  { if (off>size) throw IOException("Out of buffer read");};        // Check if we have a valid position

  virtual ~BitPumpPlain(void);
protected:
  const uchar8* buffer;
  const uint32 size;            // This if the end of buffer.
  uint32 off;                  // Offset in bytes
private:
};

} // namespace RawSpeed

#endif
