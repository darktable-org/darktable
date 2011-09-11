#include "StdAfx.h"
#include "TiffParserHeaderless.h"
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

TiffParserHeaderless::TiffParserHeaderless(FileMap* input, Endianness _end) :
    TiffParser(input) {
  tiff_endian = _end;
}

TiffParserHeaderless::~TiffParserHeaderless(void) {
}

#ifdef CHECKSIZE
#undef CHECKSIZE
#endif
#ifdef CHECKPTR
#undef CHECKPTR
#endif

#define CHECKSIZE(A) if (A >= mInput->getSize()) throw TiffParserException("Error reading Headerless TIFF structure. File Corrupt")
#define CHECKPTR(A) if ((int)A >= ((int)(mInput->data) + size))) throw TiffParserException("Error reading Headerless TIFF structure. File Corrupt")

void TiffParserHeaderless::parseData() {
  parseData(0);
}

void TiffParserHeaderless::parseData(uint32 firstIfdOffset) {
  if (mInput->getSize() < 12)
    throw TiffParserException("Not a TIFF file (size too small)");

  if (tiff_endian == host_endian)
    mRootIFD = new TiffIFD();
  else
    mRootIFD = new TiffIFDBE();

  uint32 nextIFD = firstIfdOffset;
  do {
    CHECKSIZE(nextIFD);

    if (tiff_endian == host_endian)
      mRootIFD->mSubIFD.push_back(new TiffIFD(mInput, nextIFD));
    else
      mRootIFD->mSubIFD.push_back(new TiffIFDBE(mInput, nextIFD));

    nextIFD = mRootIFD->mSubIFD.back()->getNextIFD();
  } while (nextIFD);
}

} // namespace RawSpeed
