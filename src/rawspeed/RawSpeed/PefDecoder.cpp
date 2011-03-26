#include "StdAfx.h"
#include "PefDecoder.h"
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

PefDecoder::PefDecoder(TiffIFD *rootIFD, FileMap* file) :
    RawDecoder(file), mRootIFD(rootIFD) {
      decoderVersion = 2;
}

PefDecoder::~PefDecoder(void) {
}

RawImage PefDecoder::decodeRaw() {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(STRIPOFFSETS);

  if (data.empty())
    ThrowRDE("PEF Decoder: No image data found");

  TiffIFD* raw = data[0];

  int compression = raw->getEntry(COMPRESSION)->getInt();

  if (1 == compression) {
    decodeUncompressed(raw, true);
    return mRaw;
  }

  if (65535 != compression)
    ThrowRDE("PEF Decoder: Unsupported compression");

  TiffEntry *offsets = raw->getEntry(STRIPOFFSETS);
  TiffEntry *counts = raw->getEntry(STRIPBYTECOUNTS);

  if (offsets->count != 1) {
    ThrowRDE("PEF Decoder: Multiple Strips found: %u", offsets->count);
  }
  if (counts->count != offsets->count) {
    ThrowRDE("PEF Decoder: Byte count number does not match strip size: count:%u, strips:%u ", counts->count, offsets->count);
  }
  if (!mFile->isValid(offsets->getInt() + counts->getInt()))
    ThrowRDE("PEF Decoder: Truncated file.");

  uint32 width = raw->getEntry(IMAGEWIDTH)->getInt();
  uint32 height = raw->getEntry(IMAGELENGTH)->getInt();

  mRaw->dim = iPoint2D(width, height);
  mRaw->createData();
  try {
    PentaxDecompressor l(mFile, mRaw);
    l.decodePentax(mRootIFD, offsets->getInt(), counts->getInt());
  } catch (IOException e) {
    errors.push_back(_strdup(e.what()));
    // Let's ignore it, it may have delivered somewhat useful data.
  }

  return mRaw;
}

void PefDecoder::checkSupport(CameraMetaData *meta) {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);
  if (data.empty())
    ThrowRDE("PEF Support check: Model name found");

  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();
  this->checkCameraSupported(meta, make, model, "");
}

void PefDecoder::decodeMetaData(CameraMetaData *meta) {
  mRaw->cfa.setCFA(CFA_RED, CFA_GREEN, CFA_GREEN2, CFA_BLUE);
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);

  if (data.empty())
    ThrowRDE("PEF Meta Decoder: Model name found");

  TiffIFD* raw = data[0];

  string make = raw->getEntry(MAKE)->getString();
  string model = raw->getEntry(MODEL)->getString();

  setMetaData(meta, make, model, "");
}

} // namespace RawSpeed
