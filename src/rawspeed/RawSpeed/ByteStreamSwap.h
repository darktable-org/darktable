#pragma once
#include "ByteStream.h"

#include "IOException.h"

namespace RawSpeed {

class ByteStreamSwap :
  public ByteStream
{
public:
  ByteStreamSwap(const uchar8* _buffer, uint32 _size);
  ByteStreamSwap(const ByteStreamSwap* b);
  virtual ushort16 getShort();
  virtual int getInt();
  virtual ~ByteStreamSwap(void);
};

} // namespace RawSpeed
