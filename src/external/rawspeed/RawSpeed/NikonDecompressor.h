#ifndef NIKON_DECOMPRESSOR_H
#define NIKON_DECOMPRESSOR_H

#include "LJpegDecompressor.h"
#include "BitPumpMSB.h"
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

namespace RawSpeed {

class NikonDecompressor :
  public LJpegDecompressor
{
public:
  NikonDecompressor(FileMap* file, RawImage img );
public:
  virtual ~NikonDecompressor(void);
  void DecompressNikon(ByteStream *meta, uint32 w, uint32 h, uint32 bitsPS, uint32 offset, uint32 size);
private:
  void initTable(uint32 huffSelect);
  int HuffDecodeNikon();
  uint32 curve[0xffff];
  BitPumpMSB *bits;
};

static const uchar8 nikon_tree[][32] = {
  { 0,1,5,1,1,1,1,1,1,2,0,0,0,0,0,0,	/* 12-bit lossy */
  5,4,3,6,2,7,1,0,8,9,11,10,12 },
  { 0,1,5,1,1,1,1,1,1,2,0,0,0,0,0,0,	/* 12-bit lossy after split */
  0x39,0x5a,0x38,0x27,0x16,5,4,3,2,1,0,11,12,12 },
  { 0,1,4,2,3,1,2,0,0,0,0,0,0,0,0,0,  /* 12-bit lossless */
  5,4,6,3,7,2,8,1,9,0,10,11,12 },
  { 0,1,4,3,1,1,1,1,1,2,0,0,0,0,0,0,	/* 14-bit lossy */
  5,6,4,7,8,3,9,2,1,0,10,11,12,13,14 },
  { 0,1,5,1,1,1,1,1,1,1,2,0,0,0,0,0,	/* 14-bit lossy after split */
  8,0x5c,0x4b,0x3a,0x29,7,6,5,4,3,2,1,0,13,14 },
  { 0,1,4,2,2,3,1,2,0,0,0,0,0,0,0,0,	/* 14-bit lossless */
  7,6,8,5,9,4,10,3,11,12,2,0,1,13,14 } };
  
} // namespace RawSpeed

#endif
