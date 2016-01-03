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
#ifndef BYTE_STREAM_H
#define BYTE_STREAM_H

#include "IOException.h"
#include "FileMap.h"
#include <stack>

namespace RawSpeed {

class ByteStream
{
public:
  ByteStream(const uchar8* _buffer, uint32 _size);
  ByteStream(const ByteStream* b);
  ByteStream(FileMap *f, uint32 offset, uint32 count);
  ByteStream(FileMap *f, uint32 offset);
  virtual ~ByteStream(void);
  uint32 peekByte();
  uint32 getOffset() {return off;}
  void skipBytes(uint32 nbytes);
  uchar8 getByte();
  void setAbsoluteOffset(uint32 offset);
  void skipToMarker();
  uint32 getRemainSize() { return size-off;}
  const uchar8* getData() {return &buffer[off];}
  virtual ushort16 getShort();
  virtual int getInt();
  virtual uint32 getUInt();
  virtual float getFloat();
  // Increments the stream to after the next zero byte and returns the bytes in between (not a copy).
  // If the first byte is zero, stream is incremented one.
  const char* getString();  
  void pushOffset() { offset_stack.push(off);}
  void popOffset();
protected:
  const uchar8* buffer;
  uint32 size;            // This if the end of buffer.
  uint32 off;                  // Offset in bytes (this is next byte to deliver)
  stack<uint32> offset_stack;
};

} // namespace RawSpeed

#endif
