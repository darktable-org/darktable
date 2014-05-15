#include "StdAfx.h"
#include "TiffParser.h"

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


TiffParser::TiffParser(FileMap* inputData): mInput(inputData), mRootIFD(0) {
  host_endian = getHostEndianness();
}


TiffParser::~TiffParser(void) {
  if (mRootIFD)
    delete mRootIFD;
  mRootIFD = NULL;
}

#ifdef CHECKSIZE
#undef CHECKSIZE
#endif
#ifdef CHECKPTR
#undef CHECKPTR
#endif

#define CHECKSIZE(A) if (A >= mInput->getSize()) throw TiffParserException("Error reading TIFF structure (size out of bounds). File Corrupt")
#define CHECKPTR(A) if ((int)A >= ((int)(mInput->data) + size))) throw TiffParserException("Error reading TIFF structure (size out of bounds). File Corrupt")

void TiffParser::parseData() {
  const unsigned char* data = mInput->getData(0);
  if (mInput->getSize() < 16)
    throw TiffParserException("Not a TIFF file (size too small)");
  if (data[0] != 0x49 || data[1] != 0x49) {
    tiff_endian = big;
    if (data[0] != 0x4D || data[1] != 0x4D)
      throw TiffParserException("Not a TIFF file (ID)");

    if (data[3] != 42)
      throw TiffParserException("Not a TIFF file (magic 42)");
  } else {
    tiff_endian = little;
    if (data[2] != 42 && data[2] != 0x52 && data[2] != 0x55) // ORF has 0x52, RW2 0x55 - Brillant!
      throw TiffParserException("Not a TIFF file (magic 42)");
  }

  if (mRootIFD)
    delete mRootIFD;

  if (tiff_endian == host_endian)
    mRootIFD = new TiffIFD();
  else
    mRootIFD = new TiffIFDBE();

  uint32 nextIFD;
  data = mInput->getData(4);
  if (tiff_endian == host_endian) {
    nextIFD = *(int*)data;
  } else {
    nextIFD = (unsigned int)data[0] << 24 | (unsigned int)data[1] << 16 | (unsigned int)data[2] << 8 | (unsigned int)data[3];
  }
  while (nextIFD) {
    CHECKSIZE(nextIFD);

    if (tiff_endian == host_endian)
      mRootIFD->mSubIFD.push_back(new TiffIFD(mInput, nextIFD));
    else
      mRootIFD->mSubIFD.push_back(new TiffIFDBE(mInput, nextIFD));

    nextIFD = mRootIFD->mSubIFD.back()->getNextIFD();
  }
}

RawDecoder* TiffParser::getDecoder() {
  if (!mRootIFD)
    parseData();

  vector<TiffIFD*> potentials;
  potentials = mRootIFD->getIFDsWithTag(DNGVERSION);

  /* Copy, so we can pass it on and not have it destroyed with ourselves */
  TiffIFD* root = mRootIFD;

  if (!potentials.empty()) {  // We have a dng image entry
    TiffIFD *t = potentials[0];
    const unsigned char* c = t->getEntry(DNGVERSION)->getData();
    if (c[0] > 1)
      throw TiffParserException("DNG version too new.");
    mRootIFD = NULL;
    return new DngDecoder(root, mInput);
  }

  potentials = mRootIFD->getIFDsWithTag(MAKE);

  if (!potentials.empty()) {  // We have make entry
    for (vector<TiffIFD*>::iterator i = potentials.begin(); i != potentials.end(); ++i) {
      string make = (*i)->getEntry(MAKE)->getString();
      TrimSpaces(make);
      if (!make.compare("Canon")) {
        mRootIFD = NULL;
        return new Cr2Decoder(root, mInput);
      }
      if (!make.compare("NIKON CORPORATION")) {
        mRootIFD = NULL;
        return new NefDecoder(root, mInput);
      }
      if (!make.compare("NIKON")) {
        mRootIFD = NULL;
        return new NefDecoder(root, mInput);
      }
      if (!make.compare("OLYMPUS IMAGING CORP.")) {
        mRootIFD = NULL;
        return new OrfDecoder(root, mInput);
      }
      if (!make.compare("SONY")) {
        mRootIFD = NULL;
        return new ArwDecoder(root, mInput);
      }
      if (!make.compare("PENTAX Corporation")) {
        mRootIFD = NULL;
        return new PefDecoder(root, mInput);
      }
      if (!make.compare("PENTAX")) {
        mRootIFD = NULL;
        return new PefDecoder(root, mInput);
      }
      if (!make.compare("Panasonic") || !make.compare("LEICA")) {
        mRootIFD = NULL;
        return new Rw2Decoder(root, mInput);
      }
      if (!make.compare("SAMSUNG")) {
        mRootIFD = NULL;
        return new SrwDecoder(root, mInput);
      }
    }
  }
  throw TiffParserException("No decoder found. Sorry.");
  return NULL;
}

} // namespace RawSpeed
