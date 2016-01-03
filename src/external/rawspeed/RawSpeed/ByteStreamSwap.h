#ifndef BYTE_STREAM_SWAP_H
#define BYTE_STREAM_SWAP_H

#include "ByteStream.h"

#include "IOException.h"

namespace RawSpeed {

class ByteStreamSwap :
  public ByteStream
{
public:
  ByteStreamSwap(const uchar8* _buffer, uint32 _size);
  ByteStreamSwap(const ByteStreamSwap* b);
  ByteStreamSwap(FileMap *f, uint32 offset, uint32 count);
  ByteStreamSwap(FileMap *f, uint32 offset);
  virtual ushort16 getShort();
  virtual int getInt();
  virtual ~ByteStreamSwap(void);
  virtual uint32 getUInt();
};

} // namespace RawSpeed

#endif
