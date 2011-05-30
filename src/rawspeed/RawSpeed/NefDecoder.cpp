#include "StdAfx.h"
#include "NefDecoder.h"
#include "ByteStreamSwap.h"
#include "BitPumpMSB32.h"
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

NefDecoder::NefDecoder(TiffIFD *rootIFD, FileMap* file) :
    RawDecoder(file), mRootIFD(rootIFD) {
  decoderVersion = 3;
}

NefDecoder::~NefDecoder(void) {
}

RawImage NefDecoder::decodeRaw() {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(CFAPATTERN);

  if (data.empty())
    ThrowRDE("NEF Decoder: No image data found");

  TiffIFD* raw = data[0];
  int compression = raw->getEntry(COMPRESSION)->getInt();

  data = mRootIFD->getIFDsWithTag(MODEL);

  if (data.empty())
    ThrowRDE("NEF Decoder: No model data found");

  TiffEntry *offsets = raw->getEntry(STRIPOFFSETS);
  TiffEntry *counts = raw->getEntry(STRIPBYTECOUNTS);

  if (!data[0]->getEntry(MODEL)->getString().compare("NIKON D100 ")) {  /**Sigh**/
    if (!mFile->isValid(offsets->getInt()))
      ThrowRDE("NEF Decoder: Image data outside of file.");
    if (!D100IsCompressed(offsets->getInt())) {
      DecodeD100Uncompressed();
      return mRaw;
    }
  }

  if (compression == 1) {
    DecodeUncompressed();
    return mRaw;
  }

  if (offsets->count != 1) {
    ThrowRDE("NEF Decoder: Multiple Strips found: %u", offsets->count);
  }
  if (counts->count != offsets->count) {
    ThrowRDE("NEF Decoder: Byte count number does not match strip size: count:%u, strips:%u ", counts->count, offsets->count);
  }
  if (!mFile->isValid(offsets->getInt() + counts->getInt()))
    ThrowRDE("NEF Decoder: Invalid strip byte count. File probably truncated.");


  if (34713 != compression)
    ThrowRDE("NEF Decoder: Unsupported compression");

  uint32 width = raw->getEntry(IMAGEWIDTH)->getInt();
  uint32 height = raw->getEntry(IMAGELENGTH)->getInt();
  uint32 bitPerPixel = raw->getEntry(BITSPERSAMPLE)->getInt();

  mRaw->dim = iPoint2D(width, height);
  mRaw->createData();

  data = mRootIFD->getIFDsWithTag(MAKERNOTE);
  if (data.empty())
    ThrowRDE("NEF Decoder: No EXIF data found");

  TiffIFD* exif = data[0];
  TiffEntry *makernoteEntry = exif->getEntry(MAKERNOTE);
  const uchar8* makernote = makernoteEntry->getData();
  FileMap makermap((uchar8*)&makernote[10], makernoteEntry->count - 10);
  TiffParser makertiff(&makermap);
  makertiff.parseData();

  data = makertiff.RootIFD()->getIFDsWithTag((TiffTag)0x8c);

  if (data.empty())
    ThrowRDE("NEF Decoder: Decompression info tag not found");

  TiffEntry *meta;
  try {
    meta = data[0]->getEntry((TiffTag)0x96);
  } catch (TiffParserException) {
    meta = data[0]->getEntry((TiffTag)0x8c);  // Fall back
  }

  try {
    NikonDecompressor decompressor(mFile, mRaw);

    // Nikon is JPEG (Big Endian) byte order
    ByteStream* metastream;
    if (getHostEndianness() == big)
      metastream = new ByteStream(meta->getData(), meta->count);
    else
      metastream = new ByteStreamSwap(meta->getData(), meta->count);

    decompressor.DecompressNikon(metastream, width, height, bitPerPixel, offsets->getInt(), counts->getInt());

    delete metastream;
  } catch (IOException e) {
    errors.push_back(_strdup(e.what()));
    // Let's ignore it, it may have delivered somewhat useful data.
  }

  return mRaw;
}

/*
Figure out if a NEF file is compressed.  These fancy heuristics
are only needed for the D100, thanks to a bug in some cameras
that tags all images as "compressed".
*/
bool NefDecoder::D100IsCompressed(uint32 offset) {
  const uchar8 *test = mFile->getData(offset);
  int i;

  for (i = 15; i < 256; i += 16)
    if (test[i]) return true;
  return false;
}

TiffIFD* NefDecoder::FindBestImage(vector<TiffIFD*>* data) {
  int largest_width = 0;
  TiffIFD* best_ifd = NULL;
  for (int i = 0; i < (int)data->size(); i++) {
    TiffIFD* raw = (*data)[i];
    int width = raw->getEntry(IMAGEWIDTH)->getInt();
    if (width > largest_width)
      best_ifd = raw;
  }
  if (NULL == best_ifd)
    ThrowRDE("NEF Decoder: Unable to locate image");
  return best_ifd;
}

void NefDecoder::DecodeUncompressed() {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(CFAPATTERN);
  TiffIFD* raw = FindBestImage(&data);
  uint32 nslices = raw->getEntry(STRIPOFFSETS)->count;
  const uint32 *offsets = raw->getEntry(STRIPOFFSETS)->getIntArray();
  const uint32 *counts = raw->getEntry(STRIPBYTECOUNTS)->getIntArray();
  uint32 yPerSlice = raw->getEntry(ROWSPERSTRIP)->getInt();
  uint32 width = raw->getEntry(IMAGEWIDTH)->getInt();
  uint32 height = raw->getEntry(IMAGELENGTH)->getInt();
  uint32 bitPerPixel = raw->getEntry(BITSPERSAMPLE)->getInt();

  vector<NefSlice> slices;
  uint32 offY = 0;

  for (uint32 s = 0; s < nslices; s++) {
    NefSlice slice;
    slice.offset = offsets[s];
    slice.count = counts[s];
    if (offY + yPerSlice > height)
      slice.h = height - offY;
    else
      slice.h = yPerSlice;

    offY += yPerSlice;

    if (mFile->isValid(slice.offset + slice.count)) // Only decode if size is valid
      slices.push_back(slice);
  }

  if (0 == slices.size())
    ThrowRDE("NEF Decoder: No valid slices found. File probably truncated.");

  mRaw->dim = iPoint2D(width, offY);
  mRaw->createData();
  if (bitPerPixel == 14 && width*slices[0].h*2 == slices[0].count)
    bitPerPixel = 16; // D3

  offY = 0;
  for (uint32 i = 0; i < slices.size(); i++) {
    NefSlice slice = slices[i];
    ByteStream in(mFile->getData(slice.offset), slice.count);
    iPoint2D size(width, slice.h);
    iPoint2D pos(0, offY);
    try {
      if (hints.find(string("coolpixmangled")) != hints.end())
        readCoolpixMangledRaw(in, size, pos, width*bitPerPixel / 8);
      else if (hints.find(string("coolpixsplit")) != hints.end())
        readCoolpixSplitRaw(in, size, pos, width*bitPerPixel / 8);
      else
        readUncompressedRaw(in, size, pos, width*bitPerPixel / 8, bitPerPixel, true);
    } catch (RawDecoderException e) {
      if (i>0)
        errors.push_back(_strdup(e.what()));
      else
        throw;
    } catch (IOException e) {
      if (i>0)
        errors.push_back(_strdup(e.what()));
      else
        ThrowRDE("NEF decoder: IO error occurred in first slice, unable to decode more. Error is: %s", e.what());
    }
    offY += slice.h;
  }
}

void NefDecoder::readCoolpixMangledRaw(ByteStream &input, iPoint2D& size, iPoint2D& offset, int inputPitch) {
  uchar8* data = mRaw->getData();
  uint32 outPitch = mRaw->pitch;
  uint32 w = size.x;
  uint32 h = size.y;
  uint32 cpp = mRaw->getCpp();
  if (input.getRemainSize() < (inputPitch*h)) {
    if ((int)input.getRemainSize() > inputPitch)
      h = input.getRemainSize() / inputPitch - 1;
    else
      ThrowIOE("readUncompressedRaw: Not enough data to decode a single line. Image file truncated.");
  }

  if (offset.y > mRaw->dim.y)
    ThrowRDE("readUncompressedRaw: Invalid y offset");
  if (offset.x + size.x > mRaw->dim.x)
    ThrowRDE("readUncompressedRaw: Invalid x offset");

  uint32 y = offset.y;
  h = MIN(h + (uint32)offset.y, (uint32)mRaw->dim.y);
  w *= cpp;
  BitPumpMSB32 *in = new BitPumpMSB32(&input);
  for (; y < h; y++) {
    ushort16* dest = (ushort16*) & data[offset.x*sizeof(ushort16)*cpp+y*outPitch];
    for (uint32 x = 0 ; x < w; x++) {
      dest[x] =  in->getBits(12);
    }
  }
}


void NefDecoder::readCoolpixSplitRaw(ByteStream &input, iPoint2D& size, iPoint2D& offset, int inputPitch) {
  uchar8* data = mRaw->getData();
  uint32 outPitch = mRaw->pitch;
  uint32 w = size.x;
  uint32 h = size.y;
  uint32 cpp = mRaw->getCpp();
  if (input.getRemainSize() < (inputPitch*h)) {
    if ((int)input.getRemainSize() > inputPitch)
      h = input.getRemainSize() / inputPitch - 1;
    else
      ThrowIOE("readUncompressedRaw: Not enough data to decode a single line. Image file truncated.");
  }

  if (offset.y > mRaw->dim.y)
    ThrowRDE("readCoolpixSplitRaw: Invalid y offset");
  if (offset.x + size.x > mRaw->dim.x)
    ThrowRDE("readCoolpixSplitRaw: Invalid x offset");

  uint32 y = offset.y;
  h = MIN(h + (uint32)offset.y, (uint32)mRaw->dim.y);
  w *= cpp;
  h /= 2;
  BitPumpMSB *in = new BitPumpMSB(&input);
  for (; y < h; y++) {
    ushort16* dest = (ushort16*) & data[offset.x*sizeof(ushort16)*cpp+y*2*outPitch];
    for (uint32 x = 0 ; x < w; x++) {
      dest[x] =  in->getBits(12);
    }
  }
  for (y = offset.y; y < h; y++) {
    ushort16* dest = (ushort16*) & data[offset.x*sizeof(ushort16)*cpp+(y*2+1)*outPitch];
    for (uint32 x = 0 ; x < w; x++) {
      dest[x] =  in->getBits(12);
    }
  }
}

void NefDecoder::DecodeD100Uncompressed() {

  ThrowRDE("NEF DEcoder: D100 uncompressed not supported");
  /*
    TiffIFD* raw = mRootIFD->getIFDsWithTag(CFAPATTERN)[0];

    uint32 nslices = raw->getEntry(STRIPOFFSETS)->count;
    uint32 offset = raw->getEntry(STRIPOFFSETS)->getInt();
    uint32 count = raw->getEntry(STRIPBYTECOUNTS)->getInt();
    if (!mFile->isValid(offset+count))
      ThrowRDE("DecodeD100Uncompressed: Truncated file");

    const uchar8 *in =  mFile->getData(offset);

    uint32 w = raw->getEntry(IMAGEWIDTH)->getInt();
    uint32 h = raw->getEntry(IMAGELENGTH)->getInt();

    mRaw->dim = iPoint2D(w, h);
    mRaw->bpp = 2;
    mRaw->createData();

    uchar8* data = mRaw->getData();
    uint32 outPitch = mRaw->pitch;

    BitPumpMSB bits(mFile->getData(offset),count);
    for (uint32 y = 0; y < h; y++) {
      ushort16* dest = (ushort16*)&data[y*outPitch];
      for(uint32 x =0 ; x < w; x++) {
        uint32 b = bits.getBits(12);
        dest[x] = b;
        if ((x % 10) == 9)
          bits.skipBits(8);
      }
    }*/
}

void NefDecoder::checkSupport(CameraMetaData *meta) {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);
  if (data.empty())
    ThrowRDE("NEF Support check: Model name found");
  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();
  this->checkCameraSupported(meta, make, model, "");
}

void NefDecoder::decodeMetaData(CameraMetaData *meta) {
  mRaw->cfa.setCFA(CFA_RED, CFA_GREEN, CFA_GREEN2, CFA_BLUE);

  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);

  if (data.empty())
    ThrowRDE("NEF Meta Decoder: Model name found");

  int white = mRaw->whitePoint;
  int black = mRaw->blackLevel;

  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();

  setMetaData(meta, make, model, "");

  if (white != 65536)
    mRaw->whitePoint = white;
  if (black >= 0)
    mRaw->blackLevel = black;
}

} // namespace RawSpeed
