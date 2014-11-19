#ifndef HASSELBLAD_DECOMPRESSOR_H
#define HASSELBLAD_DECOMPRESSOR_H

#include "LJpegDecompressor.h"
#include "BitPumpMSB.h"
#include "TiffIFD.h"

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

class HasselbladDecompressor :
  public LJpegDecompressor
{
public:
  HasselbladDecompressor(FileMap* file, RawImage img);
  virtual ~HasselbladDecompressor(void);
  int HuffDecodeHasselblad();
  void decodeHasselblad(TiffIFD *root, uint32 offset, uint32 size);
  int pixelBaseOffset;
protected:
  int HuffGetLength();
  virtual void parseSOS();
  void decodeScanHasselblad();
  inline int getBits(int len);
  BitPumpMSB32* ph1_bits;  // Phase One has unescaped bits.
};

} // namespace RawSpeed

#endif
