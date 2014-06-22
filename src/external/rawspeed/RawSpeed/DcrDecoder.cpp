#include "StdAfx.h"
#include "DcrDecoder.h"
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

DcrDecoder::DcrDecoder(TiffIFD *rootIFD, FileMap* file)  :
    RawDecoder(file), mRootIFD(rootIFD) {
  decoderVersion = 0;
}

DcrDecoder::~DcrDecoder(void) {
}

RawImage DcrDecoder::decodeRawInternal() {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(STRIPOFFSETS);

  if (data.size() < 2)
    ThrowRDE("DCR Decoder: No image data found");
    
  TiffIFD* raw = data[2];
  uint32 width = raw->getEntry(IMAGEWIDTH)->getInt();
  uint32 height = raw->getEntry(IMAGELENGTH)->getInt();
  uint32 off = raw->getEntry(STRIPOFFSETS)->getInt();
  uint32 c2 = raw->getEntry(STRIPBYTECOUNTS)->getInt();

  mRaw->dim = iPoint2D(width, height);
  mRaw->createData();
  ByteStream input(mFile->getData(off), c2);

  int compression = raw->getEntry(COMPRESSION)->getInt();
  if (65000 == compression)
    parseKodak65000(input, width, height);
  else
    ThrowRDE("DCR Decoder: Unsupported compression %d", compression);

  return mRaw;
}

void DcrDecoder::parseKodak65000(ByteStream &input, uint32 w, uint32 h) {
  ushort16 buf[256];
  uint32 pred[2];
  uchar8* data = mRaw->getData();
  uint32 pitch = mRaw->pitch;
  in = input.getData();

  for (uint32 y = 0; y < h; y++) {
    ushort16* dest = (ushort16*) & data[y*pitch];
    for (uint32 x = 0 ; x < w; x += 256) {
      uint32 len = MIN(256, w-x);
      bool ret = decodeKodak65000(buf, len);
      for (uint32 i = 0; i < len; i++) {
        dest[x+i] = ret ? buf[i] : (pred[i & 1] += buf[i]); // FIXME: dcraw applies a curve here
      }
    }
  }
}

bool DcrDecoder::decodeKodak65000(ushort16 *out, uint32 bsize) {
  uchar8 blen[768];
  uint64 bitbuf=0;
  uint32 bits=0, i, j, len, diff;

  bsize = (bsize + 3) & -4;
  for (i=0; i < bsize; i+=2) {
    if ((blen[i  ] = ((uint32) *in) & 15) > 12 ||
        (blen[i+1] = ((uint32) *in) >> 4) > 12 ) {
      for (i=0; i < bsize; i+=8) {
        ushort16 *raw = (ushort16 *) in;
        out[i  ] = *(raw+1) >> 12 << 8 | *(raw+3) >> 12 << 4 | *(raw+5) >> 12;
        out[i+1] = *(raw+0) >> 12 << 8 | *(raw+2) >> 12 << 4 | *(raw+4) >> 12;
        for (j=0; j < 6; j++)
          out[i+2+j] = ((uint32) *in+j) & 0xfff;
        in += 12;
      }
      return TRUE;
    }
  }
  if ((bsize & 7) == 4) {
    bitbuf  = ((uint32) *in++) << 8;
    bitbuf += ((uint32) *in++);
    bits = 16;
  }
  for (i=0; i < bsize; i++) {
    len = blen[i];
    if (bits < len) {
      for (j=0; j < 32; j+=8)
        bitbuf += (uint64) ((uint32) *in++) << (bits+(j^8));
      bits += 32;
    }
    diff = bitbuf & (0xffff >> (16-len));
    bitbuf >>= len;
    bits -= len;
    if ((diff & (1 << (len-1))) == 0)
      diff -= (1 << len) - 1;
    out[i] = diff;
  }
  return FALSE;
}

void DcrDecoder::checkSupportInternal(CameraMetaData *meta) {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);
  if (data.empty())
    ThrowRDE("DCR Support check: Model name not found");
  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();
  this->checkCameraSupported(meta, make, model, "");
}

void DcrDecoder::decodeMetaDataInternal(CameraMetaData *meta) {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);

  if (data.empty())
    ThrowRDE("DCR Decoder: Model name found");
  if (!data[0]->hasEntry(MAKE))
    ThrowRDE("DCR Decoder: Make name not found");

  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();
  setMetaData(meta, make, model, "", 0);
}

} // namespace RawSpeed
