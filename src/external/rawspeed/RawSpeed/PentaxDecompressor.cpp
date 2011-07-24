#include "StdAfx.h"
#include "PentaxDecompressor.h"
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

PentaxDecompressor::PentaxDecompressor(FileMap* file, RawImage img) :
    LJpegDecompressor(file, img) {
  pentaxBits = 0;
}

PentaxDecompressor::~PentaxDecompressor(void) {
  if (pentaxBits)
    delete(pentaxBits);
  pentaxBits = 0;
}


void PentaxDecompressor::decodePentax(TiffIFD *root, uint32 offset, uint32 size) {
  // Prepare huffmann table              0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 = 16 entries
  static const uchar8 pentax_tree[] =  { 0, 2, 3, 1, 1, 1, 1, 1, 1, 2, 0, 0, 0, 0, 0, 0,
                                         3, 4, 2, 5, 1, 6, 0, 7, 8, 9, 10, 11, 12
                                       };
  //                                     0 1 2 3 4 5 6 7 8 9  0  1  2 = 13 entries
  HuffmanTable *dctbl1 = &huff[0];

  /* Attempt to read huffman table, if found in makernote */
  if (root->hasEntryRecursive((TiffTag)0x220)) {
    TiffEntry *t = root->getEntryRecursive((TiffTag)0x220);
    if (t->type == TIFF_UNDEFINED) {
      const uchar8* data = t->getData();
      uint32 depth = (data[1]+12)&0xf;
      data +=14;
      uint32 v0[16];
      uint32 v1[16];
      uint32 v2[16];
      for (uint32 i = 0; i < depth; i++)
         v0[i] = (uint32)(data[i*2])<<8 | (uint32)(data[i*2+1]);
      data+=depth*2;

      for (uint32 i = 0; i < depth; i++)
        v1[i] = data[i];

      /* Reset bits */
      for (uint32 i = 0; i < 17; i++)
        dctbl1->bits[i] = 0;

      /* Calculate codes and store bitcounts */
      for (uint32 c = 0; c < depth; c++)
      {
        v2[c] = v0[c]>>(12-v1[c]);
        dctbl1->bits[v1[c]]++;
      }
      /* Find smallest */
      for (uint32 i = 0; i < depth; i++)
      {
        uint32 sm_val = 0xfffffff;
        uint32 sm_num = 0xff;
        for (uint32 j = 0; j < depth; j++)
        {
          if(v2[j]<=sm_val)
          {
            sm_num = j;
            sm_val = v2[j];
          }
        }
        dctbl1->huffval[i] = sm_num;
        v2[sm_num]=0xffffffff;
      }
    }
  } else {
    /* Initialize with legacy data */
    uint32 acc = 0;
    for (uint32 i = 0; i < 16 ;i++) {
      dctbl1->bits[i+1] = pentax_tree[i];
      acc += dctbl1->bits[i+1];
    }
    dctbl1->bits[0] = 0;
    for (uint32 i = 0 ; i < acc; i++) {
      dctbl1->huffval[i] = pentax_tree[i+16];
    }
  }
  mUseBigtable = true;
  createHuffmanTable(dctbl1);

  pentaxBits = new BitPumpMSB(mFile->getData(offset), size);
  uchar8 *draw = mRaw->getData();
  ushort16 *dest;
  uint32 w = mRaw->dim.x;
  uint32 h = mRaw->dim.y;
  int pUp1[2] = {0, 0};
  int pUp2[2] = {0, 0};
  int pLeft1 = 0;
  int pLeft2 = 0;

  for (uint32 y = 0;y < h;y++) {
    pentaxBits->checkPos();
    dest = (ushort16*) & draw[y*mRaw->pitch];  // Adjust destination
    pUp1[y&1] += HuffDecodePentax();
    pUp2[y&1] += HuffDecodePentax();
    dest[0] = pLeft1 = pUp1[y&1];
    dest[1] = pLeft2 = pUp2[y&1];
    for (uint32 x = 2; x < w ; x += 2) {
      pLeft1 += HuffDecodePentax();
      pLeft2 += HuffDecodePentax();
      dest[x] =  pLeft1;
      dest[x+1] =  pLeft2;
      _ASSERTE(pLeft1 >= 0 && pLeft1 <= (65536));
      _ASSERTE(pLeft2 >= 0 && pLeft2 <= (65536));
    }
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
int PentaxDecompressor::HuffDecodePentax() {
  int rv;
  int l, temp;
  int code, val;

  HuffmanTable *dctbl1 = &huff[0];
  /*
  * If the huffman code is less than 8 bits, we can use the fast
  * table lookup to get its value.  It's more than 8 bits about
  * 3-4% of the time.
  */
  pentaxBits->fill();
  code = pentaxBits->peekBitsNoFill(14);
  val = dctbl1->bigTable[code];
  if ((val&0xff) !=  0xff) {
    pentaxBits->skipBitsNoFill(val&0xff);
    return val >> 8;
  }

  rv = 0;
  code = pentaxBits->peekByteNoFill();
  val = dctbl1->numbits[code];
  l = val & 15;
  if (l) {
    pentaxBits->skipBitsNoFill(l);
    rv = val >> 4;
  }  else {
    pentaxBits->skipBits(8);
    l = 8;
    while (code > dctbl1->maxcode[l]) {
      temp = pentaxBits->getBitNoFill();
      code = (code << 1) | temp;
      l++;
    }

    /*
    * With garbage input we may reach the sentinel value l = 17.
    */

    if (l > 12) {
      ThrowIOE("Corrupt JPEG data: bad Huffman code:%u\n", l);
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

  if (rv) {
    int x = pentaxBits->getBits(rv);
    if ((x & (1 << (rv - 1))) == 0)
      x -= (1 << rv) - 1;
    return x;
  }
  return 0;
}

} // namespace RawSpeed
