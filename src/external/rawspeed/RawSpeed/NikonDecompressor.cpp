#include "StdAfx.h"
#include "NikonDecompressor.h"
#include "BitPumpMSB.h"

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

NikonDecompressor::NikonDecompressor(FileMap* file, RawImage img) :
    LJpegDecompressor(file, img) {
  for (uint32 i = 0; i < 0x8000 ; i++) {
    curve[i]  = i;
  }
}

void NikonDecompressor::initTable(uint32 huffSelect) {
  HuffmanTable *dctbl1 = &huff[0];
  uint32 acc = 0;
  for (uint32 i = 0; i < 16 ;i++) {
    dctbl1->bits[i+1] = nikon_tree[huffSelect][i];
    acc += dctbl1->bits[i+1];
  }
  dctbl1->bits[0] = 0;

  for (uint32 i = 0 ; i < acc; i++) {
    dctbl1->huffval[i] = nikon_tree[huffSelect][i+16];
  }
  createHuffmanTable(dctbl1);
}

void NikonDecompressor::DecompressNikon(ByteStream *metadata, uint32 w, uint32 h, uint32 bitsPS, uint32 offset, uint32 size) {
  uint32 v0 = metadata->getByte();
  uint32 v1 = metadata->getByte();
  uint32 huffSelect = 0;
  uint32 split = 0;
  int pUp1[2];
  int pUp2[2];
  mUseBigtable = true;

  _RPT2(0, "Nef version v0:%u, v1:%u\n", v0, v1);

  if (v0 == 73 || v1 == 88)
    metadata->skipBytes(2110);

  if (v0 == 70) huffSelect = 2;
  if (bitsPS == 14) huffSelect += 3;

  pUp1[0] = metadata->getShort();
  pUp1[1] = metadata->getShort();
  pUp2[0] = metadata->getShort();
  pUp2[1] = metadata->getShort();

  int _max = 1 << bitsPS & 0x7fff;
  uint32 step = 0;
  uint32 csize = metadata->getShort();
  if (csize  > 1)
    step = _max / (csize - 1);
  if (v0 == 68 && v1 == 32 && step > 0) {
    for (uint32 i = 0; i < csize; i++)
      curve[i*step] = metadata->getShort();
    for (int i = 0; i < _max; i++)
      curve[i] = (curve[i-i%step] * (step - i % step) +
                  curve[i-i%step+step] * (i % step)) / step;
    metadata->setAbsoluteOffset(562);
    split = metadata->getShort();
  } else if (v0 != 70 && csize <= 0x4001) {
    for (uint32 i = 0; i < csize; i++) {
      curve[i] = metadata->getShort();
    }
    _max = csize;
  }
  initTable(huffSelect);

  mRaw->whitePoint = curve[_max-1];
  mRaw->blackLevel = curve[0];
  if (!uncorrectedRawValues) {
    mRaw->setTable(curve, _max, true);
  }

  uint32 x, y;
  BitPumpMSB bits(mFile->getData(offset), size);
  uchar8 *draw = mRaw->getData();
  ushort16 *dest;
  uint32 pitch = mRaw->pitch;

  int pLeft1 = 0;
  int pLeft2 = 0;
  uint32 cw = w / 2;
  uint32 random = bits.peekBits(24);
  for (y = 0; y < h; y++) {
    if (split && y == split) {
      initTable(huffSelect + 1);
    }
    dest = (ushort16*) & draw[y*pitch];  // Adjust destination
    pUp1[y&1] += HuffDecodeNikon(bits);
    pUp2[y&1] += HuffDecodeNikon(bits);
    pLeft1 = pUp1[y&1];
    pLeft2 = pUp2[y&1];
    mRaw->setWithLookUp(clampbits(pLeft1,15), (uchar8*)dest++, &random);
    mRaw->setWithLookUp(clampbits(pLeft2,15), (uchar8*)dest++, &random);
    for (x = 1; x < cw; x++) {
      bits.checkPos();
      pLeft1 += HuffDecodeNikon(bits);
      pLeft2 += HuffDecodeNikon(bits);
      mRaw->setWithLookUp(clampbits(pLeft1,15), (uchar8*)dest++, &random);
      mRaw->setWithLookUp(clampbits(pLeft2,15), (uchar8*)dest++, &random);
    }
  }

  if (uncorrectedRawValues) {
    mRaw->setTable(curve, _max, false);
  } else {
    mRaw->setTable(NULL);
  }

}

/*
*--------------------------------------------------------------
*
* HuffDecode --
*
* Taken from Figure F.16: extract next coded symbol from
* input stream.  This should becode a macro.
*
* Results:
* Next coded symbol
*
* Side effects:
* Bitstream is parsed.
*
*--------------------------------------------------------------
*/
int NikonDecompressor::HuffDecodeNikon(BitPumpMSB& bits) {
  int rv;
  int l, temp;
  int code, val ;

  HuffmanTable *dctbl1 = &huff[0];

  bits.fill();
  code = bits.peekBitsNoFill(14);
  val = dctbl1->bigTable[code];
  if ((val&0xff) !=  0xff) {
    bits.skipBitsNoFill(val&0xff);
    return val >> 8;
  }

  rv = 0;
  code = bits.peekByteNoFill();
  val = dctbl1->numbits[code];
  l = val & 15;
  if (l) {
    bits.skipBitsNoFill(l);
    rv = val >> 4;
  }  else {
    bits.skipBits(8);
    l = 8;
    while (code > dctbl1->maxcode[l]) {
      temp = bits.getBitNoFill();
      code = (code << 1) | temp;
      l++;
    }

    if (l > 16) {
      ThrowRDE("Corrupt JPEG data: bad Huffman code:%u\n", l);
    } else {
      rv = dctbl1->huffval[dctbl1->valptr[l] +
                           ((int)(code - dctbl1->mincode[l]))];
    }
  }

  if (rv == 16)
    return -32768;

  /*
  * Section F.2.2.1: decode the difference and
  * Figure F.12: extend sign bit
  */
  uint32 len = rv & 15;
  uint32 shl = rv >> 4;
  int diff = ((bits.getBits(len - shl) << 1) + 1) << shl >> 1;
  if ((diff & (1 << (len - 1))) == 0)
    diff -= (1 << len) - !shl;
  return diff;
}

} // namespace RawSpeed
