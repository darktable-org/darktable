#include "StdAfx.h"
#include "ByteStream.h"
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

namespace RawSpeed {

ByteStream::ByteStream(const uchar8* _buffer, uint32 _size) :
    buffer(_buffer), size(_size), off(0) {

}

ByteStream::ByteStream(const ByteStream *b) :
    buffer(b->buffer), size(b->size), off(b->off) {

}

ByteStream::ByteStream(FileMap *f, uint32 offset, uint32 _size) :
    size(_size) {
  buffer = f->getData(offset, size);
  off = 0;
}

ByteStream::ByteStream(FileMap *f, uint32 offset)
{
  size = f->getSize() - offset;
  buffer = f->getData(offset, size);
  off = 0;
}

ByteStream::~ByteStream(void) {

}

uint32 ByteStream::peekByte() {
  return buffer[off];
}

void ByteStream::skipBytes(uint32 nbytes) {
  off += nbytes;
  if (off > size)
    ThrowIOE("Skipped out of buffer");
}

uchar8 ByteStream::getByte() {
  if (off >= size)
    throw IOException("getByte:Out of buffer read");
  off++;
  return buffer[off-1];
}

ushort16 ByteStream::getShort() {
  if (off + 1 > size)
    ThrowIOE("getShort: Out of buffer read");
  off +=2;
  return ((ushort16)buffer[off-1] << 8) | (ushort16)buffer[off-2];
}

uint32 ByteStream::getUInt() {
  if (off + 4 > size)
    ThrowIOE("getInt:Out of buffer read");
  uint32 r = (uint32)buffer[off+3] << 24 | (uint32)buffer[off+2] << 16 | (uint32)buffer[off+1] << 8 | (uint32)buffer[off];
  off+=4;
  return r;
}

int ByteStream::getInt() {
  if (off + 4 > size)
    ThrowIOE("getInt:Out of buffer read");
  int r = (int)buffer[off+3] << 24 | (int)buffer[off+2] << 16 | (int)buffer[off+1] << 8 | (int)buffer[off];
  off+=4;
  return r;
}

void ByteStream::setAbsoluteOffset(uint32 offset) {
  if (offset >= size)
    ThrowIOE("setAbsoluteOffset:Offset set out of buffer");
  off = offset;
}

void ByteStream::skipToMarker() {
  int c = 0;
  while (!(buffer[off] == 0xFF && buffer[off+1] != 0 && buffer[off+1] != 0xFF)) {
    off++;
    c++;
    if (off >= size)
      ThrowIOE("No marker found inside rest of buffer");
  }
//  _RPT1(0,"Skipped %u bytes.\n", c);
}

const char* ByteStream::getString() {
  int start = off;
  while (buffer[off] != 0x00) {
    off++;
    if (off >= size)
      ThrowIOE("String not terminated inside rest of buffer");
  }
  off++;
  return (const char*)&buffer[start];
}

float ByteStream::getFloat()
{
  if (off + 4 > size)
    ThrowIOE("getFloat: Out of buffer read");
  float temp_f;
  uchar8 *temp = (uchar8 *)&temp_f;
  for (int i = 0; i < 4; i++)
    temp[i] = buffer[off+i];
  off+=4;
  return temp_f;
}

void ByteStream::popOffset()
{
 if (offset_stack.empty())
   ThrowIOE("Pop Offset: Stack empty");
 off = offset_stack.top();
 offset_stack.pop();
}
} // namespace RawSpeed
