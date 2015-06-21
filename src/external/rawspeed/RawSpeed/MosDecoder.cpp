#include "StdAfx.h"
#include "MosDecoder.h"
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

MosDecoder::MosDecoder(TiffIFD *rootIFD, FileMap* file)  :
    RawDecoder(file), mRootIFD(rootIFD) {
  decoderVersion = 0;
  black_level = 0;

  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MAKE);
  if (!data.empty()) {
    make = (const char *) data[0]->getEntry(MAKE)->getDataWrt();
    model = (const char *) data[0]->getEntry(MODEL)->getDataWrt();
  } else {
    TiffEntry *xmp = mRootIFD->getEntryRecursive(XMP);
    if (!xmp)
      ThrowRDE("MOS Decoder: Couldn't find the XMP");

    parseXMP(xmp);
  }
}

MosDecoder::~MosDecoder(void) {
}

void MosDecoder::parseXMP(TiffEntry *xmp) {
  if (xmp->count <= 0)
    ThrowRDE("MOS Decoder: Empty XMP");

  uchar8 *xmpText = xmp->getDataWrt();
  xmpText[xmp->count - 1] = 0; // Make sure the string is NUL terminated

  char *makeEnd;
  make = strstr((char *) xmpText, "<tiff:Make>");
  makeEnd = strstr((char *) xmpText, "</tiff:Make>");
  if (!make || !makeEnd)
    ThrowRDE("MOS Decoder: Couldn't find the Make in the XMP");
  make += 11; // Advance to the end of the start tag

  char *modelEnd;
  model = strstr((char *) xmpText, "<tiff:Model>");
  modelEnd = strstr((char *) xmpText, "</tiff:Model>");
  if (!model || !modelEnd)
    ThrowRDE("MOS Decoder: Couldn't find the Model in the XMP");
  model += 12; // Advance to the end of the start tag

  // NUL terminate the strings in place
  *makeEnd = 0;
  *modelEnd = 0;
}

RawImage MosDecoder::decodeRawInternal() {
  vector<TiffIFD*> data;
  TiffIFD* raw = NULL;
  uint32 off = 0;

  uint32 base = 8;
  const uchar8 *insideTiff = mFile->getData(base);
  if (get4LE(insideTiff, 0) == 0x49494949) {
    uint32 offset = get4LE(insideTiff, 8);
    if (offset+base+4 > mFile->getSize())
      ThrowRDE("MOS: PhaseOneC offset out of bounds");

    uint32 entries = get4LE(insideTiff, offset);
    uint32 pos = 8; // Skip another 4 bytes

    uint32 width=0, height=0, strip_offset=0, data_offset=0;
    while (entries--) {
      if (offset+base+pos+16 > mFile->getSize())
        ThrowRDE("MOS: PhaseOneC offset out of bounds");

      uint32 tag  = get4LE(insideTiff, offset+pos);
      //uint32 type = get4LE(insideTiff, offset+pos+4);
      //uint32 len  = get4LE(insideTiff, offset+pos+8);
      uint32 data = get4LE(insideTiff, offset+pos+12);
      pos += 16;
      switch(tag) {
      case 0x108: width        = data;      break;
      case 0x109: height       = data;      break;
      case 0x10f: data_offset  = data+base; break;
      case 0x21c: strip_offset = data+base; break;
      case 0x21d: black_level  = data>>2;   break;
      }
    }
    if (width <= 0 || height <= 0)
      ThrowRDE("MOS: PhaseOneC couldn't find width and height");
    if (strip_offset+height*4 > mFile->getSize())
      ThrowRDE("MOS: PhaseOneC strip offsets out of bounds");
    if (data_offset > mFile->getSize())
      ThrowRDE("MOS: PhaseOneC data offset out of bounds");

    mRaw->dim = iPoint2D(width, height);
    mRaw->createData();

    DecodePhaseOneC(data_offset, strip_offset, width, height);

    return mRaw;
  } else {
    data = mRootIFD->getIFDsWithTag(TILEOFFSETS);
    if (!data.empty()) {
      raw = data[0];
      off = raw->getEntry(TILEOFFSETS)->getInt();
    } else {
      data = mRootIFD->getIFDsWithTag(CFAPATTERN);
      if (!data.empty()) {
        raw = data[0];
        off = raw->getEntry(STRIPOFFSETS)->getInt();
      } else
        ThrowRDE("MOS Decoder: No image data found");
    }
  }
  
  uint32 width = raw->getEntry(IMAGEWIDTH)->getInt();
  uint32 height = raw->getEntry(IMAGELENGTH)->getInt();
  mRaw->dim = iPoint2D(width, height);
  mRaw->createData();

  ByteStream input(mFile->getData(off), mFile->getSize()-off);
  int compression = raw->getEntry(COMPRESSION)->getInt();
  if (1 == compression) {
    if (mRootIFD->endian == big)
      Decode16BitRawBEunpacked(input, width, height);
    else
      Decode16BitRawUnpacked(input, width, height);
  }
  else if (99 == compression || 7 == compression) {
    ThrowRDE("MOS Decoder: Leaf LJpeg not yet supported");
    //LJpegPlain l(mFile, mRaw);
    //l.startDecoder(off, mFile->getSize()-off, 0, 0);
  } else
    ThrowRDE("MOS Decoder: Unsupported compression: %d", compression);

  return mRaw;
}

void MosDecoder::DecodePhaseOneC(uint32 data_offset, uint32 strip_offset, uint32 width, uint32 height)
{
  const int length[] = { 8,7,6,9,11,10,5,12,14,13 };

  for (uint32 row=0; row < height; row++) {
    uint32 off = data_offset + get4LE(mFile->getData(strip_offset), row*4);

    BitPumpMSB32 pump(mFile->getData(off),mFile->getSize()-off);
    uint32 pred[2], len[2];
    pred[0] = pred[1] = 0;
    ushort16* img = (ushort16*)mRaw->getData(0, row);
    for (uint32 col=0; col < width; col++) {
      if (col >= (width & -8))
        len[0] = len[1] = 14;
      else if ((col & 7) == 0)
        for (uint32 i=0; i < 2; i++) {
          uint32 j = 0;
          for (; j < 5 && !pump.getBitsSafe(1); j++);
          if (j--) len[i] = length[j*2 + pump.getBitsSafe(1)];
        }
      int i = len[col & 1];
      if (i == 14)
        img[col] = pred[col & 1] = pump.getBitsSafe(16);
      else
        img[col] = pred[col & 1] += pump.getBitsSafe(i) + 1 - (1 << (i - 1));
    }
  }
}

void MosDecoder::checkSupportInternal(CameraMetaData *meta) {
  this->checkCameraSupported(meta, make, model, "");
}

void MosDecoder::decodeMetaDataInternal(CameraMetaData *meta) {
  setMetaData(meta, make, model, "", 0);

  // Fetch the white balance (see dcraw.c parse_mos for more metadata that can be gotten)
  if (mRootIFD->hasEntryRecursive(LEAFMETADATA)) {
    TiffEntry *meta = mRootIFD->getEntryRecursive(LEAFMETADATA);
    char *text = (char *) meta->getDataWrt();
    uint32 size = meta->count;
    text[size-1] = 0; //Make sure the data is NUL terminated so that scanf never reads beyond limits

    // dcraw does actual parsing, since we just want one field we bruteforce it
    char *neutobj = (char *) memmem(text, size, "NeutObj_neutrals", 16);
    if (neutobj) {
      uint32 tmp[4] = {0};
      sscanf((const char *)neutobj+44, "%u %u %u %u", &tmp[0], &tmp[1], &tmp[2], &tmp[3]);
      mRaw->metadata.wbCoeffs[0] = (float) tmp[0]/tmp[1];
      mRaw->metadata.wbCoeffs[1] = (float) tmp[0]/tmp[2];
      mRaw->metadata.wbCoeffs[2] = (float) tmp[0]/tmp[3];
    }
    if (text)
      delete text;
  }

  if (black_level)
    mRaw->blackLevel = black_level;
}

} // namespace RawSpeed
