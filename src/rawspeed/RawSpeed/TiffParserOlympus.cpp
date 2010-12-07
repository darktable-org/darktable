#include "StdAfx.h"
#include "TiffParserOlympus.h"
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

// More relaxed Tiff parser for olympus makernote

namespace RawSpeed {

TiffParserOlympus::TiffParserOlympus(FileMap* input) :
    TiffParser(input) {
}

TiffParserOlympus::~TiffParserOlympus(void) {
}

#ifdef CHECKSIZE
#undef CHECKSIZE
#endif
#ifdef CHECKPTR
#undef CHECKPTR
#endif

#define CHECKSIZE(A) if (A >= mInput->getSize()) throw TiffParserException("Error reading Olympus Metadata TIFF structure. File Corrupt")
#define CHECKPTR(A) if ((int)A >= ((int)(mInput->data) + size))) throw TiffParserException("Error reading Olympus Metadata TIFF structure. File Corrupt")


void TiffParserOlympus::parseData() {
  const unsigned char* data = mInput->getData(0);
  if (mInput->getSize() < 16)
    throw TiffParserException("Not a TIFF file (size too small)");
  if (data[0] != 0x49 || data[1] != 0x49) {
    tiff_endian = big;
    if (data[0] != 0x4D || data[1] != 0x4D)
      throw TiffParserException("Not a TIFF file (ID)");

  } else {
    tiff_endian = little;
  }

  if (tiff_endian == host_endian)
    mRootIFD = new TiffIFD();
  else
    mRootIFD = new TiffIFDBE();

  uint32 nextIFD = 4;  // Skip Endian and magic
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
