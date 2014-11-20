#include "StdAfx.h"
#include "HasselbladDecompressor.h"
#include "ByteStreamSwap.h"

/*
    RawSpeed - RAW file decoder.

    Copyright (C) 2009-2014 Klaus Post

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

HasselbladDecompressor::HasselbladDecompressor(FileMap* file, RawImage img) :
    LJpegDecompressor(file, img) {
  ph1_bits = 0;
  pixelBaseOffset = 0;
}

HasselbladDecompressor::~HasselbladDecompressor(void) {
  if (ph1_bits)
    delete(ph1_bits);
  ph1_bits = 0;
}


void HasselbladDecompressor::decodeHasselblad(TiffIFD *root, uint32 offset, uint32 size) {
  // We cannot use bigtable, because values are packed two pixels at the time.
  mUseBigtable = false;
  startDecoder(offset,size, 0,0);
}

/* Since Phase One has it's own definition of a SOS header, we override it from LJPEG-Decompressor */
void HasselbladDecompressor::parseSOS() {
  if (!frame.initialized)
    ThrowRDE("LJpegDecompressor::parseSOS: Frame not yet initialized (SOF Marker not parsed)");

  uint32 headerLength = input->getShort();
  uint32 soscps = input->getByte();
  if (frame.cps != soscps)
    ThrowRDE("LJpegDecompressor::parseSOS: Component number mismatch.");

  for (uint32 i = 0;i < frame.cps;i++) {
    uint32 cs = input->getByte();

    uint32 count = 0;  // Find the correct component
    while (frame.compInfo[count].componentId != cs) {
      if (count >= frame.cps)
        ThrowRDE("LJpegDecompressor::parseSOS: Invalid Component Selector");
      count++;
    }

    uint32 b = input->getByte();
    uint32 td = b >> 4;
    if (td > 3)
      ThrowRDE("LJpegDecompressor::parseSOS: Invalid Huffman table selection");
    if (!huff[td].initialized)
      ThrowRDE("LJpegDecompressor::parseSOS: Invalid Huffman table selection, not defined.");

    if (count > 3)
      ThrowRDE("LJpegDecompressor::parseSOS: Component count out of range");

    frame.compInfo[count].dcTblNo = td;
  }

  // Get predictor
  pred = input->getByte();

  // Hasselblad files are tagged with predictor #8
  if (pred != 8)
    ThrowRDE("HasselbladDecompressor::parseSOS: Invalid predictor mode.");

  input->skipBytes(1);                    // Se + Ah Not used in LJPEG
  uint32 b = input->getByte();
  Pt = b & 0xf;        // Point Transform

  uint32 cheadersize = 3 + frame.cps * 2 + 3;
  _ASSERTE(cheadersize == headerLength);

  if (ph1_bits)
    delete(ph1_bits);

  ph1_bits = new BitPumpMSB32(input);

  try {
    decodeScanHasselblad();
  } catch (...) {
    throw;
  }
  input->skipBytes(ph1_bits->getOffset());
}

// Returns len bits as a signed value.
// Highest bit is a sign bit
inline int HasselbladDecompressor::getBits(int len) {
  int diff = ph1_bits->getBits(len);
  if ((diff & (1 << (len - 1))) == 0)
    diff -= (1 << len) - 1;
  if (diff == 65535)
    return -32768;
  return diff;
}

void HasselbladDecompressor::decodeScanHasselblad() {
  // Pixels are packed two at a time, not like LJPEG:
  // [p1_length_as_huffman][p2_length_as_huffman][p0_diff_with_length][p1_diff_with_length]|NEXT PIXELS
  for (uint32 y = 0; y < frame.h; y++) {
    ushort16 *dest = (ushort16*) mRaw->getData(0, y);
    int p1 = 0x8000 + pixelBaseOffset;
    int p2 = 0x8000 + pixelBaseOffset;
    ph1_bits->checkPos();
    for (uint32 x = 0; x < frame.w ; x+=2 ) {
      int len1 = HuffGetLength();
      int len2 = HuffGetLength();
      p1 += getBits(len1);
      p2 += getBits(len2);
      dest[x] = p1;
      dest[x+1] = p2;
    }
  }
}

int HasselbladDecompressor::HuffGetLength() {
  int rv = 0;
  int l, temp;
  int code, val;

  HuffmanTable *dctbl1 = &huff[0];
  /*
  * If the huffman code is less than 8 bits, we can use the fast
  * table lookup to get its value.  It's more than 8 bits about
  * 3-4% of the time.
  */
  ph1_bits->fill();

  code = ph1_bits->peekByteNoFill();
  val = dctbl1->numbits[code];
  l = val & 15;
  if (l) {
    ph1_bits->skipBitsNoFill(l);
    return val >> 4;
  }
  ph1_bits->skipBits(8);
  l = 8;

  while (code > dctbl1->maxcode[l]) {
    temp = ph1_bits->getBitNoFill();
    code = (code << 1) | temp;
    l++;
  }

  /*
  * With garbage input we may reach the sentinel value l = 17.
  */

  if (l > 16) {
    ThrowRDE("Hasselblad, Corrupt JPEG data: bad Huffman code:%u\n", l);
  } else {
    rv = dctbl1->huffval[dctbl1->valptr[l] +
                         ((int)(code - dctbl1->mincode[l]))];
  }
  return rv;
}


} // namespace RawSpeed
