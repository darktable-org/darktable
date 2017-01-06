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
    make = data[0]->getEntry(MAKE)->getString();
    model = data[0]->getEntry(MODEL)->getString();
  } else {
    TiffEntry *xmp = mRootIFD->getEntryRecursive(XMP);
    if (!xmp)
      ThrowRDE("MOS Decoder: Couldn't find the XMP");
    string xmpText = xmp->getString();
    make = getXMPTag(xmpText, "Make");
    model = getXMPTag(xmpText, "Model");
  }
}

MosDecoder::~MosDecoder(void) {
  delete mRootIFD;
}

string MosDecoder::getXMPTag(string xmp, string tag) {
  string::size_type start = xmp.find("<tiff:"+tag+">");
  string::size_type end = xmp.find("</tiff:"+tag+">");
  if (start == string::npos || end == string::npos || end <= start)
    ThrowRDE("MOS Decoder: Couldn't find tag '%s' in the XMP", tag.c_str());
  int startlen = tag.size()+7;
  return xmp.substr(start+startlen, end-start-startlen);
}

RawImage MosDecoder::decodeRawInternal() {
  vector<TiffIFD*> data;
  TiffIFD* raw = NULL;
  uint32 off = 0;

  uint32 base = 8;
  // We get a pointer up to the end of the file as we check offset bounds later
  const uchar8 *insideTiff = mFile->getData(base, mFile->getSize()-base);
  if (get4LE(insideTiff, 0) == 0x49494949) {
    uint32 offset = get4LE(insideTiff, 8);
    if (offset+base+4 > mFile->getSize())
      ThrowRDE("MOS: PhaseOneC offset out of bounds");

    uint32 entries = get4LE(insideTiff, offset);
    uint32 pos = 8; // Skip another 4 bytes

    uint32 width=0, height=0, strip_offset=0, data_offset=0, wb_offset=0;
    while (entries--) {
      if (offset+base+pos+16 > mFile->getSize())
        ThrowRDE("MOS: PhaseOneC offset out of bounds");

      uint32 tag  = get4LE(insideTiff, offset+pos);
      //uint32 type = get4LE(insideTiff, offset+pos+4);
      //uint32 len  = get4LE(insideTiff, offset+pos+8);
      uint32 data = get4LE(insideTiff, offset+pos+12);
      pos += 16;
      switch(tag) {
      case 0x107: wb_offset    = data+base;      break;
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

    const uchar8 *data = mFile->getData(wb_offset, 12);
    for(int i=0; i<3; i++) {
      // Use get4LE instead of going straight to float so this is endian clean
      uint32 value = get4LE(data, i*4);
      mRaw->metadata.wbCoeffs[i] = *((float *) &value);
    }

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

  ByteStream input(mFile, off);
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
    uint32 off = data_offset + get4LE(mFile->getData(strip_offset, 4), row*4);

    BitPumpMSB32 pump(mFile, off);
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

    uchar8 *buffer = meta->getDataWrt();
    uint32 size = meta->count;

    // We need at least 17+44 bytes for the NeutObj_neutrals section itself
    if(size < 1)
      ThrowRDE("Can't parse a zero sized meta entry");

    //Make sure the data is NUL terminated so that scanf never reads beyond limits
    //This is not a string though, it will have other NUL's in the middle
    buffer[size-1] = 0;

    // dcraw does actual parsing, since we just want one field we bruteforce it
    uchar8 *neutobj = NULL;
    // We need at least 17+44 bytes for the NeutObj_neutrals section itself
    for(uint32 i=0; (int32)i < (int32)size-17-44; i++) {
      if (!strncmp("NeutObj_neutrals", (const char *) buffer+i, 16)) {
        neutobj = buffer+i;
        break;
      }
    }
    if (neutobj) {
      uint32 tmp[4] = {0};
      sscanf((const char *)neutobj+44, "%u %u %u %u", &tmp[0], &tmp[1], &tmp[2], &tmp[3]);
      if (tmp[0] > 0 && tmp[1] > 0 && tmp[2] > 0 && tmp[3] > 0) {
        mRaw->metadata.wbCoeffs[0] = (float) tmp[0]/tmp[1];
        mRaw->metadata.wbCoeffs[1] = (float) tmp[0]/tmp[2];
        mRaw->metadata.wbCoeffs[2] = (float) tmp[0]/tmp[3];
      }
    }
  }

  if (black_level)
    mRaw->blackLevel = black_level;
}

} // namespace RawSpeed
