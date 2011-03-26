#ifndef PENTAX_DECOMPRESSOR_H
#define PENTAX_DECOMPRESSOR_H

#include "LJpegDecompressor.h"
#include "BitPumpMSB.h"
#include "TiffIFD.h"

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

class PentaxDecompressor :
  public LJpegDecompressor
{
public:
  PentaxDecompressor(FileMap* file, RawImage img);
  virtual ~PentaxDecompressor(void);
  int HuffDecodePentax();
  void decodePentax(TiffIFD *root, uint32 offset, uint32 size);
  BitPumpMSB *pentaxBits;
};

} // namespace RawSpeed

#endif
