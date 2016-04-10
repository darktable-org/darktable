#include "StdAfx.h"
#include "DcsDecoder.h"

/*
    RawSpeed - RAW file decoder.

    Copyright (C) 2009-2014 Klaus Post
    Copyright (C) 2015 Pedro Côrte-Real

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

DcsDecoder::DcsDecoder(TiffIFD *rootIFD, FileMap* file)  :
    RawDecoder(file), mRootIFD(rootIFD) {
  decoderVersion = 0;
}

DcsDecoder::~DcsDecoder(void) {
}

RawImage DcsDecoder::decodeRawInternal() {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(IMAGEWIDTH);

  if (data.size() < 1)
    ThrowRDE("DCS Decoder: No image data found");

  TiffIFD* raw = data[0];
  uint32 width = raw->getEntry(IMAGEWIDTH)->getInt();
  // Find the largest image in the file
  for(uint32 i=1; i<data.size(); i++)
    if(data[i]->getEntry(IMAGEWIDTH)->getInt() > width)
      raw = data[i];

  width = raw->getEntry(IMAGEWIDTH)->getInt();
  uint32 height = raw->getEntry(IMAGELENGTH)->getInt();
  uint32 off = raw->getEntry(STRIPOFFSETS)->getInt();
  uint32 c2 = raw->getEntry(STRIPBYTECOUNTS)->getInt();

  if (off > mFile->getSize())
    ThrowRDE("DCR Decoder: Offset is out of bounds");

  if (c2 > mFile->getSize() - off) {
    mRaw->setError("Warning: byte count larger than file size, file probably truncated.");
  }

  mRaw->dim = iPoint2D(width, height);
  mRaw->createData();
  ByteStream input(mFile, off);

  TiffEntry *linearization = mRootIFD->getEntryRecursive(GRAYRESPONSECURVE);
  if (!linearization || linearization->count != 256 || linearization->type != TIFF_SHORT)
    ThrowRDE("DCS Decoder: Couldn't find the linearization table");

  ushort16 *table = new ushort16[256];
  linearization->getShortArray(table, 256);

  if (!uncorrectedRawValues)
    mRaw->setTable(table, 256, true);

  Decode8BitRaw(input, width, height);

  // Set the table, if it should be needed later.
  if (uncorrectedRawValues) {
    mRaw->setTable(table, 256, false);
  } else {
    mRaw->setTable(NULL);
  }

  return mRaw;
}

void DcsDecoder::checkSupportInternal(CameraMetaData *meta) {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);
  if (data.empty())
    ThrowRDE("DCS Support check: Model name not found");
  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();
  this->checkCameraSupported(meta, make, model, "");
}

void DcsDecoder::decodeMetaDataInternal(CameraMetaData *meta) {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);

  if (data.empty())
    ThrowRDE("DCS Decoder: Model name found");
  if (!data[0]->hasEntry(MAKE))
    ThrowRDE("DCS Decoder: Make name not found");

  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();
  setMetaData(meta, make, model, "", 0);
}

} // namespace RawSpeed
