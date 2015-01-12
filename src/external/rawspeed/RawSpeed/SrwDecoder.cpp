#include "StdAfx.h"
#include "SrwDecoder.h"
#include "ByteStreamSwap.h"

#if defined(__unix__) || defined(__APPLE__) 
#include <stdlib.h>
#endif
/*
    RawSpeed - RAW file decoder.

    Copyright (C) 2009-2010 Klaus Post
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

SrwDecoder::SrwDecoder(TiffIFD *rootIFD, FileMap* file):
RawDecoder(file), mRootIFD(rootIFD) {
  decoderVersion = 3;
  b = NULL;
}

SrwDecoder::~SrwDecoder(void) {
  if (mRootIFD)
    delete mRootIFD;
  mRootIFD = NULL;
  if (NULL != b)
    delete b;
  b = NULL;
}

RawImage SrwDecoder::decodeRawInternal() {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(STRIPOFFSETS);

  if (data.empty())
    ThrowRDE("Srw Decoder: No image data found");

  TiffIFD* raw = data[0];

  int compression = raw->getEntry(COMPRESSION)->getInt();
  int bits = raw->getEntry(BITSPERSAMPLE)->getInt();

  if (32769 != compression && 32770 != compression && 32772 != compression && 32773 != compression)
    ThrowRDE("Srw Decoder: Unsupported compression");

  if (32769 == compression)
  {
    bool bit_order = false;  // Default guess
    map<string,string>::iterator msb_hint = hints.find("msb_override");
    if (msb_hint != hints.end())
      bit_order = (0 == (msb_hint->second).compare("true"));
    this->decodeUncompressed(raw, bit_order ? BitOrder_Jpeg : BitOrder_Plain);
    return mRaw;
  }

  if (32770 == compression)
  {
    if (!raw->hasEntry ((TiffTag)40976)) {
      bool bit_order = (bits == 12);  // Default guess
      map<string,string>::iterator msb_hint = hints.find("msb_override");
      if (msb_hint != hints.end())
        bit_order = (0 == (msb_hint->second).compare("true"));
      this->decodeUncompressed(raw, bit_order ? BitOrder_Jpeg : BitOrder_Plain);
      return mRaw;
    } else {
      uint32 nslices = raw->getEntry(STRIPOFFSETS)->count;
      if (nslices != 1)
        ThrowRDE("Srw Decoder: Only one slice supported, found %u", nslices);
      try {
        decodeCompressed(raw);
      } catch (RawDecoderException& e) {
        mRaw->setError(e.what());
      }
      return mRaw;
    }
  }
  if (32772 == compression)
  {
    uint32 nslices = raw->getEntry(STRIPOFFSETS)->count;
    if (nslices != 1)
      ThrowRDE("Srw Decoder: Only one slice supported, found %u", nslices);
    try {
      decodeCompressed2(raw, bits);
    } catch (RawDecoderException& e) {
      mRaw->setError(e.what());
    }
    return mRaw;
  }
  if (32773 == compression)
  {
    try {
      decodeCompressed3(raw);
    } catch (RawDecoderException& e) {
      mRaw->setError(e.what());
    }
    return mRaw;
  }
  ThrowRDE("Srw Decoder: Unsupported compression");
  return mRaw;
}
// Decoder for compressed srw files (NX300 and later)
void SrwDecoder::decodeCompressed( TiffIFD* raw )
{
  uint32 width = raw->getEntry(IMAGEWIDTH)->getInt();
  uint32 height = raw->getEntry(IMAGELENGTH)->getInt();
  mRaw->dim = iPoint2D(width, height);
  mRaw->createData();
  const uint32 offset = raw->getEntry(STRIPOFFSETS)->getInt();
  uint32 compressed_offset = raw->getEntry((TiffTag)40976)->getInt();

  if (NULL != b)
    delete b;
  if (getHostEndianness() == little)
    b = new ByteStream(mFile->getData(0), mFile->getSize());
  else
    b = new ByteStreamSwap(mFile->getData(0), mFile->getSize());
  b->setAbsoluteOffset(compressed_offset);

  for (uint32 y = 0; y < height; y++) {
    uint32 line_offset = offset + b->getInt();
    if (line_offset >= mFile->getSize())
      ThrowRDE("Srw decoder: Offset outside image file, file probably truncated.");
    int len[4];
    for (int i = 0; i < 4; i++)
      len[i] = y < 2 ? 7 : 4;
    BitPumpMSB32 bits(mFile->getData(line_offset),mFile->getSize() - line_offset);
    int op[4];
    ushort16* img = (ushort16*)mRaw->getData(0, y);
    ushort16* img_up = (ushort16*)mRaw->getData(0, max(0, (int)y - 1));
    ushort16* img_up2 = (ushort16*)mRaw->getData(0, max(0, (int)y - 2));
    // Image is arranged in groups of 16 pixels horizontally
    for (uint32 x = 0; x < width; x += 16) {
      bits.fill();
      bool dir = !!bits.getBitNoFill();
      for (int i = 0; i < 4; i++)
        op[i] = bits.getBitsNoFill(2);
      for (int i = 0; i < 4; i++) {
        switch (op[i]) {
          case 3: len[i] = bits.getBits(4);
            break;
          case 2: len[i]--;
            break;
          case 1: len[i]++;
        }
        if (len[i] < 0)
          ThrowRDE("Srw Decompressor: Bit length less than 0.");
        if (len[i] > 16)
          ThrowRDE("Srw Decompressor: Bit Length more than 16.");
      }
      if (dir) {
        // Upward prediction
        // First we decode even pixels
        for (int c = 0; c < 16; c += 2) {
          int b = len[(c >> 3)];
          int32 adj = ((int32) bits.getBits(b) << (32-b) >> (32-b));
          img[c] = adj + img_up[c];
        }
        // Now we decode odd pixels
        // Why on earth upward prediction only looks up 1 line above
        // is beyond me, it will hurt compression a deal.
        for (int c = 1; c < 16; c += 2) {
          int b = len[2 | (c >> 3)];
          int32 adj = ((int32) bits.getBits(b) << (32-b) >> (32-b));
          img[c] = adj + img_up2[c];
        }
      } else {
        // Left to right prediction
        // First we decode even pixels
        int pred_left = x ? img[-2] : 128;
        for (int c = 0; c < 16; c += 2) {
          int b = len[(c >> 3)];
          int32 adj = ((int32) bits.getBits(b) << (32-b) >> (32-b));
          img[c] = adj + pred_left;
        }
        // Now we decode odd pixels
        pred_left = x ? img[-1] : 128;
        for (int c = 1; c < 16; c += 2) {
          int b = len[2 | (c >> 3)];
          int32 adj = ((int32) bits.getBits(b) << (32-b) >> (32-b));
          img[c] = adj + pred_left;
        }
      }
      bits.checkPos();
      img += 16;
      img_up += 16;
      img_up2 += 16;
    }
  }

  // Swap red and blue pixels to get the final CFA pattern
  for (uint32 y = 0; y < height-1; y+=2) {
    ushort16* topline = (ushort16*)mRaw->getData(0, y);
    ushort16* bottomline = (ushort16*)mRaw->getData(0, y+1);
    for (uint32 x = 0; x < width-1; x += 2) {
      ushort16 temp = topline[1];
      topline[1] = bottomline[0];
      bottomline[0] = temp;
      topline += 2;
      bottomline += 2;
    }
  }
}

// Decoder for compressed srw files (NX3000 and later)
void SrwDecoder::decodeCompressed2( TiffIFD* raw, int bits)
{
  uint32 width = raw->getEntry(IMAGEWIDTH)->getInt();
  uint32 height = raw->getEntry(IMAGELENGTH)->getInt();
  uint32 offset = raw->getEntry(STRIPOFFSETS)->getInt();

  mRaw->dim = iPoint2D(width, height);
  mRaw->createData();

  // This format has a variable length encoding of how many bits are needed
  // to encode the difference between pixels, we use a table to process it
  // that has two values, the first the number of bits that were used to 
  // encode, the second the number of bits that come after with the difference
  // The table has 14 entries because the difference can have between 0 (no 
  // difference) and 13 bits (differences between 12 bits numbers can need 13)
  const ushort16 tab[14][2] = {{3,4}, {3,7}, {2,6}, {2,5}, {4,3}, {6,0}, {7,9},
                               {8,10}, {9,11}, {10,12}, {10,13}, {5,1}, {4,8}, {4,2}};
  encTableItem tbl[1024];
  ushort16 vpred[2][2] = {{0,0},{0,0}}, hpred[2];

  // We generate a 1024 entry table (to be addressed by reading 10 bits) by 
  // consecutively filling in 2^(10-N) positions where N is the variable number of
  // bits of the encoding. So for example 4 is encoded with 3 bits so the first
  // 2^(10-3)=128 positions are set with 3,4 so that any time we read 000 we 
  // know the next 4 bits are the difference. We read 10 bits because that is
  // the maximum number of bits used in the variable encoding (for the 12 and 
  // 13 cases)
  uint32 n = 0;
  for (uint32 i=0; i < 14; i++) {
    for(int32 c = 0; c < (1024 >> tab[i][0]); c++) {
      tbl[n  ].encLen = tab[i][0];
      tbl[n++].diffLen = tab[i][1];
    }
  }

  BitPumpMSB pump(mFile->getData(offset),mFile->getSize() - offset);
  for (uint32 y = 0; y < height; y++) {
    ushort16* img = (ushort16*)mRaw->getData(0, y);
    for (uint32 x = 0; x < width; x++) {
      int32 diff = samsungDiff(pump, tbl);
      if (x < 2)
        hpred[x] = vpred[y & 1][x] += diff;
      else
        hpred[x & 1] += diff;
      img[x] = hpred[x & 1];
      if (img[x] >> bits)
        ThrowRDE("SRW: Error: decoded value out of bounds at %d:%d", x, y);
    }
  }
}

int32 SrwDecoder::samsungDiff (BitPumpMSB &pump, encTableItem *tbl)
{
  // We read 10 bits to index into our table
  uint32 c = pump.peekBits(10);
  // Skip the bits that were used to encode this case
  pump.getBitsSafe(tbl[c].encLen);
  // Read the number of bits the table tells me
  int32 len = tbl[c].diffLen;
  int32 diff = pump.getBitsSafe(len);

  // If the first bit is 0 we need to turn this into a negative number
  if (len && (diff & (1 << (len-1))) == 0)
    diff -= (1 << len) - 1;
  return diff;
}

// Decoder for third generation compressed SRW files (NX1)
// Seriously Samsung just use lossless jpeg already, it compresses better too :)

// Thanks to Michael Reichmann (Luminous Landscape) for putting me in contact
// and Loring von Palleske (Samsung) for pointing to the open-source code of
// Samsung's DNG converter at http://opensource.samsung.com/
void SrwDecoder::decodeCompressed3( TiffIFD* raw)
{
  uint32 offset = raw->getEntry(STRIPOFFSETS)->getInt();
  BitPumpMSB32 startpump(mFile->getData(offset),mFile->getSize() - offset);

  // Process the initial metadata bits, we only really use initVal, width and
  // height (the last two match the TIFF values anyway)
  startpump.getBitsSafe(16); // NLCVersion
  startpump.getBitsSafe(4);  // ImgFormat
  uint32 bitDepth = startpump.getBitsSafe(4)+1;
  startpump.getBitsSafe(4);  // NumBlkInRCUnit
  startpump.getBitsSafe(4);  // CompressionRatio
  uint32 width    = startpump.getBitsSafe(16);
  uint32 height    = startpump.getBitsSafe(16);
  startpump.getBitsSafe(16); // TileWidth
  startpump.getBitsSafe(4);  // reserved
  startpump.getBitsSafe(4);  // OptCode
  startpump.getBitsSafe(8);  // OverlapWidth
  startpump.getBitsSafe(8);  // reserved
  startpump.getBitsSafe(8);  // Inc
  startpump.getBitsSafe(2);  // reserved
  uint32 initVal  = startpump.getBitsSafe(14);

  mRaw->dim = iPoint2D(width, height);
  mRaw->createData();

  // The format is relatively straightforward. Each line gets encoded as a set 
  // of differences from pixels from another line. Pixels are grouped in blocks 
  // of 16 (8 green, 8 red or blue). Each block is encoded in three sections.
  // First 1 or 4 bits to specify which reference pixels to use, then a section
  // that specifies for each pixel the number of bits in the difference, then
  // the actual difference bits
  uint32 motion;
  uint32 diffBitsMode[3][2] = {0};
  uint32 line_offset = startpump.getOffset();
  for (uint32 row=0; row < height; row++) {
    // Align pump to 16byte boundary
    if ((line_offset & 0xf) != 0)
      line_offset += 16 - (line_offset & 0xf);
    BitPumpMSB32 pump(mFile->getData(offset+line_offset),mFile->getSize()-offset-line_offset);

    ushort16* img = (ushort16*)mRaw->getData(0, row);
    ushort16* img_up = (ushort16*)mRaw->getData(0, max(0, (int)row - 1));
    ushort16* img_up2 = (ushort16*)mRaw->getData(0, max(0, (int)row - 2));
    // Initialize the motion and diff modes at the start of the line
    motion = 7;
    for (uint32 i=0; i<3; i++)
      diffBitsMode[i][0] = diffBitsMode[i][1] = (row==0 || row==1) ? 7 : 4;

    for (uint32 col=0; col < width; col += 16) {
      // First we figure out which reference pixels mode we're in
      if (!pump.getBitsSafe(1))
        motion = pump.getBitsSafe(3);
      if ((row==0 || row==1) && (motion != 7))
        ThrowRDE("SRW Decoder: At start of image and motion isn't 7. File corrupted?");
      if (motion == 7) {
        // The base case, just set all pixels to the previous ones on the same line
        // If we're at the left edge we just start at the initial value
        for (uint32 i=0; i<16; i++) {
          img[i] = (col == 0) ? initVal : *(img+i-2);
        }
      } else {
        // The complex case, we now need to actually lookup one or two lines above
        if (row < 2)
          ThrowRDE("SRW: Got a previous line lookup on first two lines. File corrupted?");
        int32 motionOffset[7] =    {-4,-2,-2,0,0,2,4};
        int32 motionDoAverage[7] = { 0, 0, 1,0,1,0,0};

        int32 slideOffset = motionOffset[motion];
        int32 doAverage = motionDoAverage[motion];

        for (uint32 i=0; i<16; i++) {
          ushort16* refpixel;
          if ((row+i) & 0x1) // Red or blue pixels use same color two lines up
            refpixel = img_up2 + i + slideOffset;
          else // Green pixel N uses Green pixel N from row above (top left or top right)
            refpixel = img_up + i + slideOffset + ((i%2) ? -1 : 1);

          // In some cases we use as reference interpolation of this pixel and the next
          if (doAverage)
            img[i] = (*refpixel + *(refpixel+2) + 1) >> 1;
          else
            img[i] = *refpixel;
        }
      }

      // Figure out how many difference bits we have to read for each pixel
      uint32 diffBits[4] = {0};
      uint32 flags[4];
      for (uint32 i=0; i<4; i++)
        flags[i] = pump.getBitsSafe(2);
      for (uint32 i=0; i<4; i++) {
        // The color is 0-Green 1-Blue 2-Red
        uint32 colornum = (row % 2 != 0) ? i>>1 : ((i>>1)+2) % 3;
        switch(flags[i]) {
          case 0: diffBits[i] = diffBitsMode[colornum][0]; break;
          case 1: diffBits[i] = diffBitsMode[colornum][0]+1; break;
          case 2: diffBits[i] = diffBitsMode[colornum][0]-1; break;
          case 3: diffBits[i] = pump.getBitsSafe(4); break;
        }
        diffBitsMode[colornum][0] = diffBitsMode[colornum][1];
        diffBitsMode[colornum][1] = diffBits[i];
        if(diffBits[i] > bitDepth+1)
          ThrowRDE("SRW Decoder: Too many difference bits. File corrupted?");
      }

      // Actually read the differences and write them to the pixels
      for (uint32 i=0; i<16; i++) {
        uint32 len = diffBits[i>>2];
        int32 diff = pump.getBitsSafe(len);
        // If the first bit is 1 we need to turn this into a negative number
        if (diff >> (len-1))
          diff -= (1 << len);
        // Apply the diff to pixels 0 2 4 6 8 10 12 14 1 3 5 7 9 11 13 15
        if (row % 2)
          img[((i&0x7)<<1)+1-(i>>3)] += diff;
        else
          img[((i&0x7)<<1)+(i>>3)] += diff;
      }

      img += 16;
      img_up += 16;
      img_up2 += 16;
    }
    line_offset += pump.getOffset();
  }
}

void SrwDecoder::checkSupportInternal(CameraMetaData *meta) {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);
  if (data.empty())
    ThrowRDE("Srw Support check: Model name found");
  if (!data[0]->hasEntry(MAKE))
    ThrowRDE("SRW Support: Make name not found");
  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();
  this->checkCameraSupported(meta, make, model, "");
}

void SrwDecoder::decodeMetaDataInternal(CameraMetaData *meta) {
  mRaw->cfa.setCFA(iPoint2D(2,2), CFA_RED, CFA_GREEN, CFA_GREEN2, CFA_BLUE);
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);

  if (data.empty())
    ThrowRDE("SRW Meta Decoder: Model name found");

  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();

  data = mRootIFD->getIFDsWithTag(CFAPATTERN);
  if (!this->checkCameraSupported(meta, make, model, "") && !data.empty() && data[0]->hasEntry(CFAREPEATPATTERNDIM)) {
    const unsigned short* pDim = data[0]->getEntry(CFAREPEATPATTERNDIM)->getShortArray();
    iPoint2D cfaSize(pDim[1], pDim[0]);
    if (cfaSize.x != 2 && cfaSize.y != 2)
      ThrowRDE("SRW Decoder: Unsupported CFA pattern size");

    const uchar8* cPat = data[0]->getEntry(CFAPATTERN)->getData();
    if (cfaSize.area() != data[0]->getEntry(CFAPATTERN)->count)
      ThrowRDE("SRW Decoder: CFA pattern dimension and pattern count does not match: %d.");

    for (int y = 0; y < cfaSize.y; y++) {
      for (int x = 0; x < cfaSize.x; x++) {
        uint32 c1 = cPat[x+y*cfaSize.x];
        CFAColor c2;
        switch (c1) {
            case 0:
              c2 = CFA_RED; break;
            case 1:
              c2 = CFA_GREEN; break;
            case 2:
              c2 = CFA_BLUE; break;
            default:
              c2 = CFA_UNKNOWN;
              ThrowRDE("SRW Decoder: Unsupported CFA Color.");
        }
        mRaw->cfa.setColorAt(iPoint2D(x, y), c2);
      }
    }
    //printf("Camera CFA: %s\n", mRaw->cfa.asString().c_str());
  }

  int iso = 0;
  if (mRootIFD->hasEntryRecursive(ISOSPEEDRATINGS))
    iso = mRootIFD->getEntryRecursive(ISOSPEEDRATINGS)->getInt();

  setMetaData(meta, make, model, "", iso);

  // Set the whitebalance
  if (mRootIFD->hasEntryRecursive(SAMSUNG_WB_RGGBLEVELSUNCORRECTED) &&
      mRootIFD->hasEntryRecursive(SAMSUNG_WB_RGGBLEVELSBLACK)) {
    TiffEntry *wb_levels = mRootIFD->getEntryRecursive(SAMSUNG_WB_RGGBLEVELSUNCORRECTED);
    TiffEntry *wb_black = mRootIFD->getEntryRecursive(SAMSUNG_WB_RGGBLEVELSBLACK);
    if (wb_levels->count == 4 && wb_black->count == 4) {
      wb_levels->offsetFromParent();
      const uint32 *levels = wb_levels->getIntArray();
      wb_black->offsetFromParent();
      const uint32 *blacks = wb_black->getIntArray();

      mRaw->metadata.wbCoeffs[0] = levels[0] - blacks[0];
      mRaw->metadata.wbCoeffs[1] = levels[1] - blacks[1];
      mRaw->metadata.wbCoeffs[2] = levels[3] - blacks[3];
    }
  }
}


} // namespace RawSpeed
