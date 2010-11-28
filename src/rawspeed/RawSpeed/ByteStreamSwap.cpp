#include "StdAfx.h"
#include "ByteStreamSwap.h"

namespace RawSpeed {


ByteStreamSwap::ByteStreamSwap( const uchar8* _buffer, uint32 _size ) : 
ByteStream(_buffer, _size)
{}

ByteStreamSwap::ByteStreamSwap( const ByteStreamSwap* b ) :
ByteStream(b)
{}

ByteStreamSwap::~ByteStreamSwap(void)
{
}

ushort16 ByteStreamSwap::getShort() {
  if (off + 1 >= size)
    throw IOException("getShort: Out of buffer read");
  uint32 a = buffer[off++];
  uint32 b = buffer[off++];
  return (ushort16)((a << 8) | b);
}

/* NOTE: Actually unused, so not tested */
int ByteStreamSwap::getInt() {
  if (off + 4 >= size)
    throw IOException("getInt: Out of buffer read");
  int r = (int)buffer[off] << 24 | (int)buffer[off] << 16 | (int)buffer[off] << 8 | (int)buffer[off];
  off+=4;
  return r;
}

} // namespace RawSpeed
