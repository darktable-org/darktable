#include "StdAfx.h"
#include "MosDecoder.h"
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

MosDecoder::MosDecoder(TiffIFD *rootIFD, FileMap* file)  :
    RawDecoder(file), mRootIFD(rootIFD) {
  decoderVersion = 0;

  TiffEntry *xmp = mRootIFD->getEntryRecursive(XMP);
  if (!xmp)
    ThrowRDE("MOS Decoder: Couldn't find the XMP");

  parseXMP(xmp);
}

MosDecoder::~MosDecoder(void) {
}

void MosDecoder::parseXMP(TiffEntry *xmp) {
  uchar8 *text = xmp->getDataWrt();
  text[xmp->count - 1] = 0; // Make sure the string is NUL terminated

  char *makeEnd;
  make = strstr((char *) text, "<tiff:Make>");
  makeEnd = strstr((char *) text, "</tiff:Make>");
  if (!make || !makeEnd)
    ThrowRDE("MOS Decoder: Couldn't find the Make in the XMP");
  make += 11; // Advance to the end of the start tag

  char *modelEnd;
  model = strstr((char *) text, "<tiff:Model>");
  modelEnd = strstr((char *) text, "</tiff:Model>");
  if (!model || !modelEnd)
    ThrowRDE("MOS Decoder: Couldn't find the Model in the XMP");
  model += 12; // Advance to the end of the start tag

  // NUL terminate the strings in place
  *makeEnd = 0;
  *modelEnd = 0;
}

RawImage MosDecoder::decodeRawInternal() {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(TILEOFFSETS);

  if (data.empty())
    ThrowRDE("MOS Decoder: No image data found");

  int compression = data[0]->getEntry(COMPRESSION)->getInt();
  if (1 != compression)
    ThrowRDE("MOS Decoder: Unsupported compression");

  TiffIFD* raw = data[0];
  uint32 width = raw->getEntry(IMAGEWIDTH)->getInt();
  uint32 height = raw->getEntry(IMAGELENGTH)->getInt();
  uint32 off = raw->getEntry(TILEOFFSETS)->getInt();

  mRaw->dim = iPoint2D(width, height);
  mRaw->createData();
  ByteStream input(mFile->getData(off), mFile->getSize()-off);

  Decode16BitRawBEunpacked(input, width, height);
  return mRaw;
}

void MosDecoder::checkSupportInternal(CameraMetaData *meta) {
  this->checkCameraSupported(meta, make, model, "");
}

void MosDecoder::decodeMetaDataInternal(CameraMetaData *meta) {
  setMetaData(meta, make, model, "", 0);
}

} // namespace RawSpeed
