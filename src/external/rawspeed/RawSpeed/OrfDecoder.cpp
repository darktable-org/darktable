#include "StdAfx.h"
#include "OrfDecoder.h"
#if defined(__unix__) || defined(__APPLE__) 
#include <stdlib.h>
#endif
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
    // We're in an old-school ORF file, decode it separately
    try {
      decodeOldORF(raw);
    } catch (IOException &e) {
       mRaw->setError(e.what());
    }
    return mRaw;
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

  if ((hints.find(string("force_uncompressed")) != hints.end())) {
    // Old packed ORF, decode it like that
    uint32 off = offsets->getInt();
    uint32 size = mFile->getSize() - off;
    ByteStream input(mFile->getData(off), size);
    Decode12BitRawWithControl(input, width, height);
    return mRaw;
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

void OrfDecoder::decodeOldORF(TiffIFD* raw) {
  uint32 width = raw->getEntry(IMAGEWIDTH)->getInt();
  uint32 height = raw->getEntry(IMAGELENGTH)->getInt();
  uint32 off = raw->getEntry(STRIPOFFSETS)->getInt();

  if (!mFile->isValid(off))
      ThrowRDE("ORF Decoder: Invalid image data offset, cannot decode.");

  mRaw->dim = iPoint2D(width, height);
  mRaw->createData();
  uint32 size = mFile->getSize() - off;
  ByteStream input(mFile->getData(off), size);

  if (size >= width*height*2) { // We're in an unpacked raw
    if (raw->endian == little)
      Decode12BitRawUnpacked(input, width, height);
    else
      Decode12BitRawBEunpackedLeftAligned(input, width, height);
  } else if (size >= width*height*3/2) { // We're in one of those weird interlaced packed raws
      Decode12BitRawBEInterlaced(input, width, height);
  } else {
    ThrowRDE("ORF Decoder: Don't know how to handle the encoding in this file\n");
  }
}

/* This is probably the slowest decoder of them all.
 * I cannot see any way to effectively speed up the prediction
 * phase, which is by far the slowest part of this algorithm.
 * Also there is no way to multithread this code, since prediction
 * is based on the output of all previous pixel (bar the first four)
 */

void OrfDecoder::decodeCompressed(ByteStream& s, uint32 w, uint32 h) {
  int nbits, sign, low, high, i, left0, nw0, left1, nw1;
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
      bittable[i] = min(12,high);
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

      // Skip bytes used above or read bits
      if (high == 12) {
        bits.skipBitsNoFill(15);
        high = bits.getBits(16 - nbits) >> 1;
      } else {
        bits.skipBitsNoFill(high + 1 + 3);
      }

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
        // Have local variables for values used several tiles
        // (having a "ushort16 *dst_up" that caches dest[-pitch+((int)x)] is actually slower, probably stack spill or aliasing)
        int up  = dest[-pitch+((int)x)];
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
      bits.fill();
      i = 2 * (acarry1[2] < 3);
      for (nbits = 2 + i; (ushort16) acarry1[0] >> (nbits + i); nbits++);
      b = bits.peekBitsNoFill(15);
      sign = (b >> 14) * -1;
      low  = (b >> 12) & 3;
      high = bittable[b&4095];

      // Skip bytes used above or read bits
      if (high == 12) {
        bits.skipBitsNoFill(15);
        high = bits.getBits(16 - nbits) >> 1;
      } else {
        bits.skipBitsNoFill(high + 1 + 3);
      }

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
        dest[x] = left1 = pred + ((diff << 2) | low);
      } else {
        int up  = dest[-pitch+((int)x)];
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

        dest[x] = left1 = pred + ((diff << 2) | low);
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
  mRaw->cfa.setCFA(iPoint2D(2,2), CFA_RED, CFA_GREEN, CFA_GREEN, CFA_BLUE);
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);

  if (data.empty())
    ThrowRDE("ORF Meta Decoder: Model name found");

  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();

  if (mRootIFD->hasEntryRecursive(ISOSPEEDRATINGS))
    iso = mRootIFD->getEntryRecursive(ISOSPEEDRATINGS)->getInt();

  setMetaData(meta, make, model, "", iso);

  if (mRootIFD->hasEntryRecursive(OLYMPUSREDMULTIPLIER) &&
      mRootIFD->hasEntryRecursive(OLYMPUSBLUEMULTIPLIER)) {
    mRaw->metadata.wbCoeffs[0] = (float) mRootIFD->getEntryRecursive(OLYMPUSREDMULTIPLIER)->getShort();
    mRaw->metadata.wbCoeffs[1] = 256.0f;
    mRaw->metadata.wbCoeffs[2] = (float) mRootIFD->getEntryRecursive(OLYMPUSBLUEMULTIPLIER)->getShort();
  } else {
    // Newer cameras process the Image Processing SubIFD in the makernote
    if(mRootIFD->hasEntryRecursive(OLYMPUSIMAGEPROCESSING)) {
      TiffEntry *img_entry = mRootIFD->getEntryRecursive(OLYMPUSIMAGEPROCESSING);
      uint32 offset = *((ushort16 *) img_entry->getData()) + img_entry->parent_offset - 12;
      TiffIFD *image_processing;
      if (mRootIFD->endian == getHostEndianness())
        image_processing = new TiffIFD(mFile, offset);
      else
        image_processing = new TiffIFDBE(mFile, offset);

      // Get the WB
      if(image_processing->hasEntry((TiffTag) 0x0100)) {
        TiffEntry *wb = image_processing->getEntry((TiffTag) 0x0100);
        if (wb->count == 4) {
          wb->parent_offset = img_entry->parent_offset - 12;
          wb->offsetFromParent();
        }
        if (wb->count == 2 || wb->count == 4) {
          const ushort16 *tmp = wb->getShortArray();
          mRaw->metadata.wbCoeffs[0] = (float) tmp[0];
          mRaw->metadata.wbCoeffs[1] = 256.0f;
          mRaw->metadata.wbCoeffs[2] = (float) tmp[1];
        }
      }

      // Get the black levels
      if(image_processing->hasEntry((TiffTag) 0x0600)) {
        TiffEntry *blackEntry = image_processing->getEntry((TiffTag) 0x0600);
        // Order is assumed to be RGGB
        if (blackEntry->count == 4) {
          blackEntry->parent_offset = img_entry->parent_offset - 12;
          blackEntry->offsetFromParent();
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
      if (image_processing)
        delete image_processing;
    }
  }
}

} // namespace RawSpeed
