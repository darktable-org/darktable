#include "StdAfx.h"
#include "CiffParser.h"
#include "CrwDecoder.h"

/*
    RawSpeed - RAW file decoder.

    Copyright (C) 2009-2014 Klaus Post
    Copyright (C) 2014 Pedro Côrte-Real

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


CiffParser::CiffParser(FileMap* inputData): mInput(inputData), mRootIFD(0) {
}


CiffParser::~CiffParser(void) {
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

#define CHECKSIZE(A) if (A >= mInput->getSize()) throw CiffParserException("Error reading CIFF structure (size out of bounds). File Corrupt")
#define CHECKPTR(A) if ((int)A >= ((int)(mInput->data) + size))) throw CiffParserException("Error reading CIFF structure (size out of bounds). File Corrupt")

void CiffParser::parseData() {
  if (little != getHostEndianness())
    ThrowCPE("CIFF parsing not supported on big-endian architectures yet");

  const unsigned char* data = mInput->getData(0);
  if (mInput->getSize() < 16)
    ThrowCPE("Not a CIFF file (size too small)");

  if (data[0] != 0x49 || data[1] != 0x49)
    ThrowCPE("Not a CIFF file (ID)");

  if (mRootIFD)
    delete mRootIFD;

  mRootIFD = new CiffIFD(mInput, data[2], mInput->getSize());
}

RawDecoder* CiffParser::getDecoder() {
  if (!mRootIFD)
    parseData();

  /* Copy, so we can pass it on and not have it destroyed with ourselves */
  CiffIFD* root = mRootIFD;

  vector<CiffIFD*> potentials;
  potentials = mRootIFD->getIFDsWithTag(CIFF_MAKEMODEL);

  if (!potentials.empty()) {  // We have make entry
    for (vector<CiffIFD*>::iterator i = potentials.begin(); i != potentials.end(); ++i) {
      string make = (*i)->getEntry(CIFF_MAKEMODEL)->getString();
      TrimSpaces(make);
      if (!make.compare("Canon")) {
        mRootIFD = NULL;
        return new CrwDecoder(root, mInput);
      }
    }
  }

  throw CiffParserException("No decoder found. Sorry.");
  return NULL;
}

void CiffParser::MergeIFD( CiffParser* other_ciff)
{
  if (!other_ciff || !other_ciff->mRootIFD || other_ciff->mRootIFD->mSubIFD.empty())
    return;

  CiffIFD *other_root = other_ciff->mRootIFD;
  for (vector<CiffIFD*>::iterator i = other_root->mSubIFD.begin(); i != other_root->mSubIFD.end(); ++i) {
    mRootIFD->mSubIFD.push_back(*i);
  }

  for (map<CiffTag, CiffEntry*>::iterator i = other_root->mEntry.begin(); i != other_root->mEntry.end(); ++i) {    
    mRootIFD->mEntry[(*i).first] = (*i).second;
  }
  other_root->mSubIFD.clear();
  other_root->mEntry.clear();
}

} // namespace RawSpeed
