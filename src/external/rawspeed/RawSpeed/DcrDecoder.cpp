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
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(CFAPATTERN);

  if (data.size() < 1)
    ThrowRDE("DCR Decoder: No image data found");
    
  TiffIFD* raw = data[0];
  uint32 width = raw->getEntry(IMAGEWIDTH)->getInt();
  uint32 height = raw->getEntry(IMAGELENGTH)->getInt();
  uint32 off = raw->getEntry(STRIPOFFSETS)->getInt();
  uint32 c2 = raw->getEntry(STRIPBYTECOUNTS)->getInt();

  if (c2 > mFile->getSize() - off) {
    mRaw->setError("Warning: byte count larger than file size, file probably truncated.");
  }

  mRaw->dim = iPoint2D(width, height);
  mRaw->createData();
  ByteStream input(mFile->getData(off), mFile->getSize() - off);

  int compression = raw->getEntry(COMPRESSION)->getInt();
  if (65000 == compression) {
    TiffEntry *ifdoffset = mRootIFD->getEntryRecursive(KODAK_IFD);
    if (!ifdoffset)
      ThrowRDE("DCR Decoder: Couldn't find the Kodak IFD offset");
    TiffIFD *kodakifd;
    if (mRootIFD->endian == getHostEndianness())
      kodakifd = new TiffIFD(mFile, ifdoffset->getInt());
    else
      kodakifd = new TiffIFDBE(mFile, ifdoffset->getInt());
    TiffEntry *linearization = kodakifd->getEntryRecursive(KODAK_LINEARIZATION);
    if (!linearization || linearization->count != 1024 || linearization->type != TIFF_SHORT) {
      delete kodakifd;
      ThrowRDE("DCR Decoder: Couldn't find the linearization table");
    }

    // FIXME: dcraw does all sorts of crazy things besides this to fetch
    //        WB from what appear to be presets and calculate it in weird ways
    //        The only file I have only uses this method, if anybody careas look
    //        in dcraw.c parse_kodak_ifd() for all that weirdness
    TiffEntry *blob = kodakifd->getEntryRecursive((TiffTag) 0x03fd);
    if (blob && blob->count == 72) {
      ushort16 *data = (ushort16 *) blob->getShortArray();
      mRaw->metadata.wbCoeffs[0] = (float) 2048.0f / data[20];
      mRaw->metadata.wbCoeffs[1] = (float) 2048.0f / data[21];
      mRaw->metadata.wbCoeffs[2] = (float) 2048.0f / data[22];
    }

    // Get create passthrough curve if user has requested that.
    if (uncorrectedRawValues) {
      for (int i = 0; i < 1024; i++)
        linear[i] = i;
    }

    try {
      if (uncorrectedRawValues)
        decodeKodak65000(input, width, height, linear);
      else
        decodeKodak65000(input, width, height, linearization->getShortArray());
    } catch (IOException) {
      mRaw->setError("IO error occurred while reading image. Returning partial result.");
    }
    delete kodakifd;
  } else
    ThrowRDE("DCR Decoder: Unsupported compression %d", compression);

  return mRaw;
}

void DcrDecoder::decodeKodak65000(ByteStream &input, uint32 w, uint32 h, const ushort16 *curve) {
  ushort16 buf[256];
  uint32 pred[2];
  uchar8* data = mRaw->getData();
  uint32 pitch = mRaw->pitch;

  for (uint32 y = 0; y < h; y++) {
    ushort16* dest = (ushort16*) & data[y*pitch];
    for (uint32 x = 0 ; x < w; x += 256) {
      pred[0] = pred[1] = 0;
      uint32 len = MIN(256, w-x);
      decodeKodak65000Segment(input, buf, len);
      for (uint32 i = 0; i < len; i++) {
        ushort16 value = pred[i & 1] += buf[i];
        if (value > 1023)
          ThrowRDE("DCR Decoder: Value out of bounds %d", value);
        dest[x+i] = curve[value];
      }
    }
  }
}

void DcrDecoder::decodeKodak65000Segment(ByteStream &input, ushort16 *out, uint32 bsize) {
  uchar8 blen[768];
  uint64 bitbuf=0;
  uint32 bits=0;
  
  bsize = (bsize + 3) & -4;
  for (uint32 i=0; i < bsize; i+=2) {
    blen[i] = input.peekByte() & 15;
    blen[i+1] = input.getByte() >> 4;
  }
  if ((bsize & 7) == 4) {
    bitbuf  = ((int) input.getByte()) << 8;
    bitbuf += ((int) input.getByte());
    bits = 16;
  }
  for (uint32 i=0; i < bsize; i++) {
    uint32 len = blen[i];
    if (bits < len) {
      for (uint32 j=0; j < 32; j+=8) {
        bitbuf += (long long) ((int) input.getByte()) << (bits+(j^8));
      }
      bits += 32;
    }
    uint32 diff = (uint32)bitbuf & (0xffff >> (16-len));
    bitbuf >>= len;
    bits -= len;
    if (len && (diff & (1 << (len-1))) == 0)
      diff -= (1 << len) - 1;
    out[i] = diff;
  }
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
