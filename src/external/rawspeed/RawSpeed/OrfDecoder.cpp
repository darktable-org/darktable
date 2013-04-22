#include "StdAfx.h"
#include "OrfDecoder.h"
#include "TiffParserOlympus.h"
#if defined(__unix__) || defined(__APPLE__) 
#include <stdlib.h>
#endif
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

OrfDecoder::OrfDecoder(TiffIFD *rootIFD, FileMap* file):
    RawDecoder(file), mRootIFD(rootIFD) {
      decoderVersion = 2;
}

OrfDecoder::~OrfDecoder(void) {
  if (mRootIFD)
    delete mRootIFD;
  mRootIFD = NULL;
}

RawImage OrfDecoder::decodeRawInternal() {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(STRIPOFFSETS);

  if (data.empty())
    ThrowRDE("ORF Decoder: No image data found");

  TiffIFD* raw = data[0];

  int compression = raw->getEntry(COMPRESSION)->getInt();
  if (1 != compression)
    ThrowRDE("ORF Decoder: Unsupported compression");

  TiffEntry *offsets = raw->getEntry(STRIPOFFSETS);
  TiffEntry *counts = raw->getEntry(STRIPBYTECOUNTS);

  if (offsets->count != 1) {
    ThrowRDE("ORF Decoder: Multiple Strips found: %u", offsets->count);
  }
  if (counts->count != offsets->count) {
    ThrowRDE("ORF Decoder: Byte count number does not match strip size: count:%u, strips:%u ", counts->count, offsets->count);
  }
  uint32 width = raw->getEntry(IMAGEWIDTH)->getInt();
  uint32 height = raw->getEntry(IMAGELENGTH)->getInt();
  uint32 bps = raw->getEntry(BITSPERSAMPLE)->getInt();

  if (!mFile->isValid(offsets->getInt() + counts->getInt()))
    ThrowRDE("ORF Decoder: Truncated file");

  mRaw->dim = iPoint2D(width, height);
  mRaw->createData();

  data = mRootIFD->getIFDsWithTag(MAKERNOTE);
  if (data.empty())
    ThrowRDE("ORF Decoder: No Makernote found");

  TiffIFD* exif = data[0];
  TiffEntry *makernoteEntry = exif->getEntry(MAKERNOTE);
  const uchar8* makernote = makernoteEntry->getData();
  FileMap makermap((uchar8*)&makernote[8], makernoteEntry->count - 8);
  try {
    TiffParserOlympus makertiff(&makermap);
    makertiff.parseData();
    data = makertiff.RootIFD()->getIFDsWithTag((TiffTag)0x2010);
    if (data.empty())
      ThrowRDE("ORF Decoder: Unsupported compression");
    TiffEntry *oly = data[0]->getEntry((TiffTag)0x2010);
    if (oly->type == TIFF_UNDEFINED)
      ThrowRDE("ORF Decoder: Unsupported compression");
  } catch (TiffParserException) {
    ThrowRDE("ORF Decoder: Unable to parse makernote");
  }

  // We add 3 bytes slack, since the bitpump might be a few bytes ahead.
  ByteStream s(mFile->getData(offsets->getInt()), counts->getInt() + 3);

  if ((hints.find(string("force_uncompressed")) != hints.end())) {
    ByteStream in(mFile->getData(offsets->getInt()), counts->getInt() + 3);
    iPoint2D size(width, height),pos(0,0);
    readUncompressedRaw(in, size, pos, width*bps/8,bps, BitOrder_Jpeg32);
    return mRaw;
  }

  try {
    decodeCompressed(s, width, height);
  } catch (IOException &e) {
    mRaw->setError(e.what());
    // Let's ignore it, it may have delivered somewhat useful data.
  }

  return mRaw;
}
/* This is probably the slowest decoder of them all.
 * I cannot see any way to effectively speed up the prediction
 * phase, which is by far the slowest part of this algorithm.
 * Also there is no way to multithread this code, since prediction
 * is based on the output of all previous pixel (bar the first four)
 */

void OrfDecoder::decodeCompressed(ByteStream& s, uint32 w, uint32 h) {
  int nbits, sign, low, high, i, left0, up, nw0, left1, nw1;
  int acarry0[3], acarry1[3], pred, diff;

  uchar8* data = mRaw->getData();
  int pitch = mRaw->pitch;

  /* Build a table to quickly look up "high" value */
  char bittable[4096];
  for (i = 0; i < 4096; i++) {
    int b = i;
    for (high = 0; high < 12; high++)
      if ((b>>(11-high))&1)
        break;
      bittable[i] = high;
  }
  left0 = nw0 = left1 = nw1 = 0;

  s.skipBytes(7);
  BitPumpMSB bits(&s);

  for (uint32 y = 0; y < h; y++) {
    memset(acarry0, 0, sizeof acarry0);
    memset(acarry1, 0, sizeof acarry1);
    ushort16* dest = (ushort16*) & data[y*pitch];
    bool y_border = y < 2;
    bool border = TRUE;
    for (uint32 x = 0; x < w; x++) {
      bits.checkPos();
      bits.fill();
      i = 2 * (acarry0[2] < 3);
      for (nbits = 2 + i; (ushort16) acarry0[0] >> (nbits + i); nbits++);

      int b = bits.peekBitsNoFill(15);
      sign = (b >> 14) * -1;
      low  = (b >> 12) & 3;
      high = bittable[b&4095];
      // Skip bits used above.
      bits.skipBitsNoFill(min(12+3, high + 1 + 3));

      if (high == 12)
        high = bits.getBits(16 - nbits) >> 1;

      acarry0[0] = (high << nbits) | bits.getBits(nbits);
      diff = (acarry0[0] ^ sign) + acarry0[1];
      acarry0[1] = (diff * 3 + acarry0[1]) >> 5;
      acarry0[2] = acarry0[0] > 16 ? 0 : acarry0[2] + 1;

      if (border) {
        if (y_border && x < 2)  
          pred = 0;
        else if (y_border) 
          pred = left0;
        else { 
          pred = dest[-pitch+((int)x)];
          nw0 = pred;
        }
        dest[x] = pred + ((diff << 2) | low);
        // Set predictor
        left0 = dest[x];
      } else {
        up  = dest[-pitch+((int)x)];
        int leftMinusNw = left0 - nw0;
        int upMinusNw = up - nw0;
        // Check if sign is different, and one is not zero
        if (leftMinusNw * upMinusNw < 0) {
          if (other_abs(leftMinusNw) > 32 || other_abs(upMinusNw) > 32)
            pred = left0 + upMinusNw;
          else 
            pred = (left0 + up) >> 1;
        } else 
          pred = other_abs(leftMinusNw) > other_abs(upMinusNw) ? left0 : up;

        dest[x] = pred + ((diff << 2) | low);
        // Set predictors
        left0 = dest[x];
        nw0 = up;
      }
      
      // ODD PIXELS
      x += 1;
      bits.checkPos();
      bits.fill();
      i = 2 * (acarry1[2] < 3);
      for (nbits = 2 + i; (ushort16) acarry1[0] >> (nbits + i); nbits++);
      b = bits.peekBitsNoFill(15);
      sign = (b >> 14) * -1;
      low  = (b >> 12) & 3;
      high = bittable[b&4095];
      // Skip bits used above.
      bits.skipBitsNoFill(min(12+3, high + 1 + 3));

      if (high == 12)
        high = bits.getBits(16 - nbits) >> 1;

      acarry1[0] = (high << nbits) | bits.getBits(nbits);
      diff = (acarry1[0] ^ sign) + acarry1[1];
      acarry1[1] = (diff * 3 + acarry1[1]) >> 5;
      acarry1[2] = acarry1[0] > 16 ? 0 : acarry1[2] + 1;

      if (border) {
        if (y_border && x < 2)  
          pred = 0;
        else if (y_border) 
          pred = left1;
        else { 
          pred = dest[-pitch+((int)x)];
          nw1 = pred;
        }
        dest[x] = pred + ((diff << 2) | low);
        // Set predictor
        left1 = dest[x];
      } else {
        up  = dest[-pitch+((int)x)];
        int leftMinusNw = left1 - nw1;
        int upMinusNw = up - nw1;

        // Check if sign is different, and one is not zero
        if (leftMinusNw * upMinusNw < 0) {
          if (other_abs(leftMinusNw) > 32 || other_abs(upMinusNw) > 32)
            pred = left1 + upMinusNw;
          else 
            pred = (left1 + up) >> 1;
        } else 
          pred = other_abs(leftMinusNw) > other_abs(upMinusNw) ? left1 : up;

        dest[x] = pred + ((diff << 2) | low);

        // Set predictors
        left1 = dest[x];
        nw1 = up;
      }
	  border = y_border;
    }
  }
}

void OrfDecoder::checkSupportInternal(CameraMetaData *meta) {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);
  if (data.empty())
    ThrowRDE("ORF Support check: Model name found");
  if (!data[0]->hasEntry(MAKE))
    ThrowRDE("ORF Support: Make name not found");
  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();
  this->checkCameraSupported(meta, make, model, "");
}

void OrfDecoder::decodeMetaDataInternal(CameraMetaData *meta) {
  int iso = 0;
  mRaw->cfa.setCFA(CFA_RED, CFA_GREEN, CFA_GREEN, CFA_BLUE);
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);

  if (data.empty())
    ThrowRDE("ORF Meta Decoder: Model name found");

  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();

  if (mRootIFD->hasEntryRecursive(ISOSPEEDRATINGS))
    iso = mRootIFD->getEntryRecursive(ISOSPEEDRATINGS)->getInt();

  setMetaData(meta, make, model, "", iso);

  TiffIFD *ImageProcessing = 0;
  try {
    data = mRootIFD->getIFDsWithTag(MAKERNOTE);

    if (!data.empty()) {
      TiffIFD* exif = data[0];
      TiffEntry *makernoteEntry = exif->getEntry(MAKERNOTE);
      const uchar8* makernote = makernoteEntry->getData();
      FileMap makermap((uchar8*)&makernote[8], makernoteEntry->count - 8);
      TiffParserOlympus makertiff(&makermap);
      makertiff.parseData();
      TiffEntry *blackEntry = 0;

      // Try reading black level from tag 0x2040 (Olympus Imageprocessing)
      if (makertiff.RootIFD()->hasEntryRecursive((TiffTag)0x2040)) {
        try {
          TiffEntry *imagep = makertiff.RootIFD()->getEntryRecursive((TiffTag)0x2040);

          int32 offset;
          const uchar8* data = imagep->getData();
          if (makertiff.tiff_endian == makertiff.getHostEndian())
            offset = *(int32*)data;
          else
            offset = (unsigned int)data[0] << 24 | (unsigned int)data[1] << 16 | (unsigned int)data[2] << 8 | (unsigned int)data[3];

          // It seems like Olympus doesn't mind data pointing out of the makernote, 
          // so we give it the entire remaining file
          FileMap makermap2((uchar8*)&makernote[0], mFile->getSize()-makernoteEntry->getDataOffset());
          if (makertiff.getHostEndian() == makertiff.tiff_endian)
            ImageProcessing = new TiffIFD(&makermap2, offset);
          else
            ImageProcessing = new TiffIFDBE(&makermap2, offset);
          blackEntry = ImageProcessing->getEntry((TiffTag)0x600);
        } catch (TiffParserException) {
        }
      }

      // Otherwise try 0x1012
      if (!blackEntry && makertiff.RootIFD()->hasEntryRecursive((TiffTag)0x1012)) {
        blackEntry = makertiff.RootIFD()->getEntryRecursive((TiffTag)0x1012);
      }

      // Order is assumed to be RGGB
      if (blackEntry && blackEntry->count == 4) {
        const ushort16* black = blackEntry->getShortArray();
        for (int i = 0; i < 4; i++) {
          if (mRaw->cfa.getColorAt(i&1, i>>1) == CFA_RED)
            mRaw->blackLevelSeparate[i] = black[0];
          else if (mRaw->cfa.getColorAt(i&1, i>>1) == CFA_BLUE)
            mRaw->blackLevelSeparate[i] = black[3];
          else if (mRaw->cfa.getColorAt(i&1, i>>1) == CFA_GREEN && i<2)
            mRaw->blackLevelSeparate[i] = black[1];
          else if (mRaw->cfa.getColorAt(i&1, i>>1) == CFA_GREEN)
            mRaw->blackLevelSeparate[i] = black[2];
        }
        // Adjust whitelevel based on the read black (we assume the dynamic range is the same)
        mRaw->whitePoint -= (mRaw->blackLevel - mRaw->blackLevelSeparate[0]);
      }
    }
  } catch (TiffParserException) {
  }
  if (ImageProcessing)
    delete ImageProcessing;

}

} // namespace RawSpeed
