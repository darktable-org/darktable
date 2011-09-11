#ifndef LJPEG_PLAIN_H
#define LJPEG_PLAIN_H

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

/******************
 * Decompresses Lossless non subsampled JPEGs, with 2-4 components
 *****************/

class LJpegPlain :
  public LJpegDecompressor
{
public:
  LJpegPlain(FileMap* file, RawImage img);
  virtual ~LJpegPlain(void);
protected:
  virtual void decodeScan();
private:
  void decodeScanLeft4Comps();
  void decodeScanLeft2Comps();
  void decodeScanLeft3Comps();
  void decodeScanLeftGeneric();
  void decodeScanLeft4_2_0();
  void decodeScanLeft4_2_2();
  uint32 *offset;
  int* slice_width;
};

} // namespace RawSpeed

#endif
