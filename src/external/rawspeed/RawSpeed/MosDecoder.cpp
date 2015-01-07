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

  const uchar8 *insideTiff = mFile->getData(8);
  uint32 insideTiffHeader = *((uint32 *) insideTiff);
  if (insideTiffHeader == 0x49494949) {
    // We're inside a wacky phase_one tiff
    //    0x108:  raw_width     = data;
    //    0x109:  raw_height    = data;
    //    0x10a:  left_margin   = data;
    //    0x10b:  top_margin    = data;
    //    0x10c:  width         = data;
    //    0x10d:  height        = data;
    //    0x10e:  ph1.format    = data;
    //    0x10f:  data_offset   = data+base;
    //    0x110:  meta_offset   = data+base;
    //            meta_length   = len;
    //    0x112:  ph1.key_off   = save - 4;
    //    0x210:  ph1.tag_210   = int_to_float(data);
    //    0x21a:  ph1.tag_21a   = data;
    //    0x21c:  strip_offset  = data+base;
    //    0x21d:  ph1.black     = data;
    //    0x222:  ph1.split_col = data;
    //    0x223:  ph1.black_col = data+base;
    //    0x224:  ph1.split_row = data;
    //    0x225:  ph1.black_row = data+base;
    ThrowRDE("MOS Decoder: unfinished support for PhaseOneC encoding");
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

void MosDecoder::DecodePhaseOneC(ByteStream &input, uint32 width, uint32 height)
{
//  static const int length[] = { 8,7,6,9,11,10,5,12,14,13 };
//  int *offset, len[2], pred[2], row, col, i, j;
//  ushort *pixel;
//  short (*cblack)[2], (*rblack)[2];

//  pixel = (ushort *) calloc (width*3 + height*4, 2);
//  //merror (pixel, "phase_one_load_raw_c()");
//  offset = (int *) (pixel + width);

//  fseek (ifp, strip_offset, SEEK_SET);

//  for (row=0; row < raw_height; row++)
//    offset[row] = get4();
//  cblack = (short (*)[2]) (offset + raw_height);
//  fseek (ifp, ph1.black_col, SEEK_SET);
//  if (ph1.black_col)
//    read_shorts ((ushort *) cblack[0], raw_height*2);
//  rblack = cblack + raw_height;
//  fseek (ifp, ph1.black_row, SEEK_SET);
//  if (ph1.black_row)
//    read_shorts ((ushort *) rblack[0], raw_width*2);
//  for (i=0; i < 256; i++)
//    curve[i] = i*i / 3.969 + 0.5;
//  for (row=0; row < raw_height; row++) {
//    fseek (ifp, data_offset + offset[row], SEEK_SET);
//    ph1_bits(-1);
//    pred[0] = pred[1] = 0;
//    for (col=0; col < raw_width; col++) {
//      if (col >= (raw_width & -8))
//        len[0] = len[1] = 14;
//      else if ((col & 7) == 0)
//        for (i=0; i < 2; i++) {
//          for (j=0; j < 5 && !ph1_bits(1); j++);
//          if (j--) len[i] = length[j*2 + ph1_bits(1)];
//        }
//      if ((i = len[col & 1]) == 14)
//        pixel[col] = pred[col & 1] = ph1_bits(16);
//      else
//        pixel[col] = pred[col & 1] += ph1_bits(i) + 1 - (1 << (i - 1));
//      if (pred[col & 1] >> 16) derror();
//      if (ph1.format == 5 && pixel[col] < 256)
//        pixel[col] = curve[pixel[col]];
//    }
//    for (col=0; col < raw_width; col++) {
//      i = (pixel[col] << 2) - ph1.black
//        + cblack[row][col >= ph1.split_col]
//        + rblack[col][row >= ph1.split_row];
//      if (i > 0) RAW(row,col) = i;
//    }
//  }
//  free (pixel);
//  maximum = 0xfffc - ph1.black;
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
}

} // namespace RawSpeed
