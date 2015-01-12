#include "StdAfx.h"
#include "KdcDecoder.h"

/*
    RawSpeed - RAW file decoder.

    Copyright (C) 2009-2014 Klaus Post
    Copyright (C) 2014-2015 Pedro Côrte-Real

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

KdcDecoder::KdcDecoder(TiffIFD *rootIFD, FileMap* file)  :
    RawDecoder(file), mRootIFD(rootIFD) {
  decoderVersion = 0;
}

KdcDecoder::~KdcDecoder(void) {
}

RawImage KdcDecoder::decodeRawInternal() {
  int compression = mRootIFD->getEntryRecursive(COMPRESSION)->getInt();
  if (7 != compression)
    ThrowRDE("KDC Decoder: Unsupported compression %d", compression);

  TiffEntry *ex = mRootIFD->getEntryRecursive(PIXELXDIMENSION);
  TiffEntry *ey = mRootIFD->getEntryRecursive(PIXELYDIMENSION);

  if (NULL == ex || NULL == ey)
    ThrowRDE("KDC Decoder: Unable to retrieve image size");

  uint32 width = ex->getInt();
  uint32 height = ey->getInt();

  TiffEntry *offset = mRootIFD->getEntryRecursive(KODAK_KDC_OFFSET);
  if (!offset || offset->count < 13)
    ThrowRDE("KDC Decoder: Couldn't find the KDC offset");
  const uint32 *offsetarray = offset->getIntArray();
  uint32 off = offsetarray[4] + offsetarray[12];

  mRaw->dim = iPoint2D(width, height);
  mRaw->createData();
  ByteStream input(mFile->getData(off), mFile->getSize()-off);

  Decode12BitRawBE(input, width, height);

  return mRaw;
}

void KdcDecoder::checkSupportInternal(CameraMetaData *meta) {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);
  if (data.empty())
    ThrowRDE("KDC Support check: Model name not found");
  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();
  this->checkCameraSupported(meta, make, model, "");
}

void KdcDecoder::decodeMetaDataInternal(CameraMetaData *meta) {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);

  if (data.empty())
    ThrowRDE("KDC Decoder: Model name found");
  if (!data[0]->hasEntry(MAKE))
    ThrowRDE("KDC Decoder: Make name not found");

  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();
  setMetaData(meta, make, model, "", 0);

  if (mRootIFD->hasEntryRecursive(KODAKWB)) {
    TiffEntry *wb = mRootIFD->getEntryRecursive(KODAKWB);
    if (wb->count == 734 || wb->count == 1502) {
      const uchar8 *tmp = wb->getData();
      mRaw->metadata.wbCoeffs[0] = (float)((((ushort16) tmp[148])<<8)|tmp[149])/256.0f;
      mRaw->metadata.wbCoeffs[1] = 1.0f;
      mRaw->metadata.wbCoeffs[2] = (float)((((ushort16) tmp[150])<<8)|tmp[151])/256.0f;
    }
  }
}

} // namespace RawSpeed
