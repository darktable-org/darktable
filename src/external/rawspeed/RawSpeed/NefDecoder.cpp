#include "StdAfx.h"
#include "NefDecoder.h"
#include "ByteStreamSwap.h"
#include "BitPumpMSB32.h"
/*
    RawSpeed - RAW file decoder.

    Copyright (C) 2009-2014 Klaus Post
    Copyright (C) 2015 Pedro Côrte-Real

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
  decoderVersion = 5;
}

NefDecoder::~NefDecoder(void) {
  if (mRootIFD)
    delete mRootIFD;
  mRootIFD = NULL;
}

RawImage NefDecoder::decodeRawInternal() {
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

  if (compression == 1 || (hints.find(string("force_uncompressed")) != hints.end()) ||
      NEFIsUncompressed(raw)) {
    DecodeUncompressed();
    return mRaw;
  }

  if (NEFIsUncompressedRGB(raw)) {
    DecodeSNefUncompressed();
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

  data = mRootIFD->getIFDsWithTag((TiffTag)0x8c);

  if (data.empty())
    ThrowRDE("NEF Decoder: Decompression info tag not found");

  TiffEntry *meta;
  if (data[0]->hasEntry((TiffTag)0x96)) {
    meta = data[0]->getEntry((TiffTag)0x96);
  } else {
    meta = data[0]->getEntry((TiffTag)0x8c);  // Fall back
  }

  try {
    NikonDecompressor decompressor(mFile, mRaw);
    decompressor.uncorrectedRawValues = uncorrectedRawValues;
    ByteStream* metastream;
    if (getHostEndianness() == data[0]->endian)
      metastream = new ByteStream(meta->getData(), meta->count);
    else
      metastream = new ByteStreamSwap(meta->getData(), meta->count);

    decompressor.DecompressNikon(metastream, width, height, bitPerPixel, offsets->getInt(), counts->getInt());

    delete metastream;
  } catch (IOException &e) {
    mRaw->setError(e.what());
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

/* At least the D810 has a broken firmware that tags uncompressed images
   as if they were compressed. For those cases we set uncompressed mode
   by figuring out that the image is the size of uncompressed packing */
bool NefDecoder::NEFIsUncompressed(TiffIFD *raw) {
  const uint32 *counts = raw->getEntry(STRIPBYTECOUNTS)->getIntArray();
  uint32 width = raw->getEntry(IMAGEWIDTH)->getInt();
  uint32 height = raw->getEntry(IMAGELENGTH)->getInt();
  uint32 bitPerPixel = raw->getEntry(BITSPERSAMPLE)->getInt();

  return counts[0] == width*height*bitPerPixel/8;
}

/* At least the D810 has a broken firmware that tags uncompressed images
   as if they were compressed. For those cases we set uncompressed mode
   by figuring out that the image is the size of uncompressed packing */
bool NefDecoder::NEFIsUncompressedRGB(TiffIFD *raw) {
  const uint32 *counts = raw->getEntry(STRIPBYTECOUNTS)->getIntArray();
  uint32 width = raw->getEntry(IMAGEWIDTH)->getInt();
  uint32 height = raw->getEntry(IMAGELENGTH)->getInt();

  return counts[0] == width*height*3;
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

    offY = MIN(height, offY + yPerSlice);

    if (mFile->isValid(slice.offset + slice.count)) // Only decode if size is valid
      slices.push_back(slice);
  }

  if (0 == slices.size())
    ThrowRDE("NEF Decoder: No valid slices found. File probably truncated.");

  mRaw->dim = iPoint2D(width, offY);

  mRaw->createData();
  if (bitPerPixel == 14 && width*slices[0].h*2 == slices[0].count)
    bitPerPixel = 16; // D3 & D810

  if(hints.find("real_bpp") != hints.end()) {
    stringstream convert(hints.find("real_bpp")->second);
    convert >> bitPerPixel;
  }

  bool bitorder = true;
  map<string,string>::iterator msb_hint = hints.find("msb_override");
  if (msb_hint != hints.end())
    bitorder = (0 == (msb_hint->second).compare("true"));

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
        readUncompressedRaw(in, size, pos, width*bitPerPixel / 8, bitPerPixel, bitorder ? BitOrder_Jpeg : BitOrder_Plain);
    } catch (RawDecoderException e) {
      if (i>0)
        mRaw->setError(e.what());
      else
        throw;
    } catch (IOException e) {
      if (i>0)
        mRaw->setError(e.what());
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
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(STRIPOFFSETS);

  if (data.size() < 2)
    ThrowRDE("DecodeD100Uncompressed: No image data found");

  TiffIFD* raw = data[1];

  uint32 offset = raw->getEntry(STRIPOFFSETS)->getInt();
  // Hardcode the sizes as at least the width is not correctly reported
  uint32 width = 3040;
  uint32 height = 2024;

  mRaw->dim = iPoint2D(width, height);
  mRaw->createData();
  ByteStream input(mFile->getData(offset), mFile->getSize()-offset);

  Decode12BitRawBEWithControl(input, width, height);
}

void NefDecoder::DecodeSNefUncompressed() {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(CFAPATTERN);
  TiffIFD* raw = FindBestImage(&data);
  uint32 offset = raw->getEntry(STRIPOFFSETS)->getInt();
  uint32 width = raw->getEntry(IMAGEWIDTH)->getInt();
  uint32 height = raw->getEntry(IMAGELENGTH)->getInt();

  mRaw->dim = iPoint2D(width, height);
  mRaw->setCpp(3);
  mRaw->isCFA = false;
  mRaw->createData();

  ByteStream in(mFile->getData(offset), mFile->getSize()-offset);

  DecodeNikonSNef(in, width, height);
}

void NefDecoder::checkSupportInternal(CameraMetaData *meta) {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);
  if (data.empty())
    ThrowRDE("NEF Support check: Model name not found");
  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();

  string mode = getMode();
  string extended_mode = getExtendedMode(mode);

  if (meta->hasCamera(make, model, extended_mode))
    this->checkCameraSupported(meta, make, model, extended_mode);
  else if (meta->hasCamera(make, model, mode))
    this->checkCameraSupported(meta, make, model, mode);
  else
    this->checkCameraSupported(meta, make, model, "");
}

string NefDecoder::getMode() {
  ostringstream mode;
  vector<TiffIFD*>  data = mRootIFD->getIFDsWithTag(CFAPATTERN);
  TiffIFD* raw = FindBestImage(&data);
  int compression = raw->getEntry(COMPRESSION)->getInt();
  uint32 bitPerPixel = raw->getEntry(BITSPERSAMPLE)->getInt();

  if (NEFIsUncompressedRGB(raw))
    mode << "sNEF-uncompressed";
  else {
    if (1 == compression || NEFIsUncompressed(raw))
      mode << bitPerPixel << "bit-uncompressed";
    else
      mode << bitPerPixel << "bit-compressed";
  }
  return mode.str();
}

string NefDecoder::getExtendedMode(string mode) {
  ostringstream extended_mode;

  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(CFAPATTERN);
  if (data.empty())
    ThrowRDE("NEF Support check: Image size not found");
  if (!data[0]->hasEntry(IMAGEWIDTH) || !data[0]->hasEntry(IMAGELENGTH))
    ThrowRDE("NEF Support: Image size not found");
  uint32 width = data[0]->getEntry(IMAGEWIDTH)->getInt();
  uint32 height = data[0]->getEntry(IMAGELENGTH)->getInt();

  extended_mode << width << "x" << height << "-" << mode;
  return extended_mode.str();
}

void NefDecoder::decodeMetaDataInternal(CameraMetaData *meta) {
  int iso = 0;
  mRaw->cfa.setCFA(iPoint2D(2,2), CFA_RED, CFA_GREEN, CFA_GREEN2, CFA_BLUE);

  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);

  if (data.empty())
    ThrowRDE("NEF Meta Decoder: Model name not found");
  if (!data[0]->hasEntry(MAKE))
    ThrowRDE("NEF Support: Make name not found");

  int white = mRaw->whitePoint;
  int black = mRaw->blackLevel;

  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();

  if (mRootIFD->hasEntryRecursive(ISOSPEEDRATINGS))
    iso = mRootIFD->getEntryRecursive(ISOSPEEDRATINGS)->getInt();

  // Read the whitebalance

  // We use this for the D50 and D2X whacky WB "encryption"
  static const uchar8 serialmap[256] = {
  0xc1,0xbf,0x6d,0x0d,0x59,0xc5,0x13,0x9d,0x83,0x61,0x6b,0x4f,0xc7,0x7f,0x3d,0x3d,
  0x53,0x59,0xe3,0xc7,0xe9,0x2f,0x95,0xa7,0x95,0x1f,0xdf,0x7f,0x2b,0x29,0xc7,0x0d,
  0xdf,0x07,0xef,0x71,0x89,0x3d,0x13,0x3d,0x3b,0x13,0xfb,0x0d,0x89,0xc1,0x65,0x1f,
  0xb3,0x0d,0x6b,0x29,0xe3,0xfb,0xef,0xa3,0x6b,0x47,0x7f,0x95,0x35,0xa7,0x47,0x4f,
  0xc7,0xf1,0x59,0x95,0x35,0x11,0x29,0x61,0xf1,0x3d,0xb3,0x2b,0x0d,0x43,0x89,0xc1,
  0x9d,0x9d,0x89,0x65,0xf1,0xe9,0xdf,0xbf,0x3d,0x7f,0x53,0x97,0xe5,0xe9,0x95,0x17,
  0x1d,0x3d,0x8b,0xfb,0xc7,0xe3,0x67,0xa7,0x07,0xf1,0x71,0xa7,0x53,0xb5,0x29,0x89,
  0xe5,0x2b,0xa7,0x17,0x29,0xe9,0x4f,0xc5,0x65,0x6d,0x6b,0xef,0x0d,0x89,0x49,0x2f,
  0xb3,0x43,0x53,0x65,0x1d,0x49,0xa3,0x13,0x89,0x59,0xef,0x6b,0xef,0x65,0x1d,0x0b,
  0x59,0x13,0xe3,0x4f,0x9d,0xb3,0x29,0x43,0x2b,0x07,0x1d,0x95,0x59,0x59,0x47,0xfb,
  0xe5,0xe9,0x61,0x47,0x2f,0x35,0x7f,0x17,0x7f,0xef,0x7f,0x95,0x95,0x71,0xd3,0xa3,
  0x0b,0x71,0xa3,0xad,0x0b,0x3b,0xb5,0xfb,0xa3,0xbf,0x4f,0x83,0x1d,0xad,0xe9,0x2f,
  0x71,0x65,0xa3,0xe5,0x07,0x35,0x3d,0x0d,0xb5,0xe9,0xe5,0x47,0x3b,0x9d,0xef,0x35,
  0xa3,0xbf,0xb3,0xdf,0x53,0xd3,0x97,0x53,0x49,0x71,0x07,0x35,0x61,0x71,0x2f,0x43,
  0x2f,0x11,0xdf,0x17,0x97,0xfb,0x95,0x3b,0x7f,0x6b,0xd3,0x25,0xbf,0xad,0xc7,0xc5,
  0xc5,0xb5,0x8b,0xef,0x2f,0xd3,0x07,0x6b,0x25,0x49,0x95,0x25,0x49,0x6d,0x71,0xc7};
  static const uchar8 keymap[256] = {
  0xa7,0xbc,0xc9,0xad,0x91,0xdf,0x85,0xe5,0xd4,0x78,0xd5,0x17,0x46,0x7c,0x29,0x4c,
  0x4d,0x03,0xe9,0x25,0x68,0x11,0x86,0xb3,0xbd,0xf7,0x6f,0x61,0x22,0xa2,0x26,0x34,
  0x2a,0xbe,0x1e,0x46,0x14,0x68,0x9d,0x44,0x18,0xc2,0x40,0xf4,0x7e,0x5f,0x1b,0xad,
  0x0b,0x94,0xb6,0x67,0xb4,0x0b,0xe1,0xea,0x95,0x9c,0x66,0xdc,0xe7,0x5d,0x6c,0x05,
  0xda,0xd5,0xdf,0x7a,0xef,0xf6,0xdb,0x1f,0x82,0x4c,0xc0,0x68,0x47,0xa1,0xbd,0xee,
  0x39,0x50,0x56,0x4a,0xdd,0xdf,0xa5,0xf8,0xc6,0xda,0xca,0x90,0xca,0x01,0x42,0x9d,
  0x8b,0x0c,0x73,0x43,0x75,0x05,0x94,0xde,0x24,0xb3,0x80,0x34,0xe5,0x2c,0xdc,0x9b,
  0x3f,0xca,0x33,0x45,0xd0,0xdb,0x5f,0xf5,0x52,0xc3,0x21,0xda,0xe2,0x22,0x72,0x6b,
  0x3e,0xd0,0x5b,0xa8,0x87,0x8c,0x06,0x5d,0x0f,0xdd,0x09,0x19,0x93,0xd0,0xb9,0xfc,
  0x8b,0x0f,0x84,0x60,0x33,0x1c,0x9b,0x45,0xf1,0xf0,0xa3,0x94,0x3a,0x12,0x77,0x33,
  0x4d,0x44,0x78,0x28,0x3c,0x9e,0xfd,0x65,0x57,0x16,0x94,0x6b,0xfb,0x59,0xd0,0xc8,
  0x22,0x36,0xdb,0xd2,0x63,0x98,0x43,0xa1,0x04,0x87,0x86,0xf7,0xa6,0x26,0xbb,0xd6,
  0x59,0x4d,0xbf,0x6a,0x2e,0xaa,0x2b,0xef,0xe6,0x78,0xb6,0x4e,0xe0,0x2f,0xdc,0x7c,
  0xbe,0x57,0x19,0x32,0x7e,0x2a,0xd0,0xb8,0xba,0x29,0x00,0x3c,0x52,0x7d,0xa8,0x49,
  0x3b,0x2d,0xeb,0x25,0x49,0xfa,0xa3,0xaa,0x39,0xa7,0xc5,0xa7,0x50,0x11,0x36,0xfb,
  0xc6,0x67,0x4a,0xf5,0xa5,0x12,0x65,0x7e,0xb0,0xdf,0xaf,0x4e,0xb3,0x61,0x7f,0x2f};

  vector<TiffIFD*> note = mRootIFD->getIFDsWithTag((TiffTag)12);
  if (!note.empty()) {
    TiffEntry *wb = note[0]->getEntry((TiffTag)12);
    if (wb->count == 4 && wb->type == TIFF_RATIONAL) {
      const uint32* wba = wb->getIntArray();
      mRaw->metadata.wbCoeffs[0] = wba[0]*1.0f / wba[1];
      mRaw->metadata.wbCoeffs[1] = wba[4]*1.0f / wba[5];
      mRaw->metadata.wbCoeffs[2] = wba[2]*1.0f / wba[3];
      if (mRaw->metadata.wbCoeffs[1] == 0)
        mRaw->metadata.wbCoeffs[1] = 1.0f;
    }
  } else if (mRootIFD->hasEntryRecursive((TiffTag)0x0097)) {
    TiffEntry *wb = mRootIFD->getEntryRecursive((TiffTag)0x0097);
    if (wb->count > 4) {
      const uchar8 *data = wb->getData();
      uint32 version = 0;
      for (uint32 i=0; i<4; i++)
        version = (version << 4) + data[i]-'0';
      if (version == 0x100 && wb->count >= 80 && wb->type == TIFF_UNDEFINED) {
        const ushort16 *tmp = wb->getShortArray();
        mRaw->metadata.wbCoeffs[0] = (float) tmp[36];
        mRaw->metadata.wbCoeffs[2] = (float) tmp[37];
        mRaw->metadata.wbCoeffs[1] = (float) tmp[38];
      } else if (version == 0x103 && wb->count >= 26 && wb->type == TIFF_UNDEFINED) {
        const ushort16 *tmp = wb->getShortArray();
        mRaw->metadata.wbCoeffs[0] = (float) tmp[10];
        mRaw->metadata.wbCoeffs[1] = (float) tmp[11];
        mRaw->metadata.wbCoeffs[2] = (float) tmp[12];
      } else if (((version == 0x204 && wb->count >= 564) ||
                  (version == 0x205 && wb->count >= 284)) &&
                 mRootIFD->hasEntryRecursive((TiffTag)0x001d) &&
                 mRootIFD->hasEntryRecursive((TiffTag)0x00a7)) {
        // Get the serial number
        TiffEntry *serial = mRootIFD->getEntryRecursive((TiffTag)0x001d);
        const char *data = (const char*) serial->getData();
        uint32 serialno = 0;
        for (uint32 i=0; i<serial->count; i++) {
          if (!data[i]) break;
          if (data[i] >= '0' && data[i] <= '9')
            serialno = serialno*10 + data[i]-'0';
          else
            serialno = serialno*10 + data[i]%10;
        }

        // Get the decryption key
        TiffEntry *key = mRootIFD->getEntryRecursive((TiffTag)0x00a7);
        const uchar8 *keydata = key->getData();
        uint32 keyno = keydata[0]^keydata[1]^keydata[2]^keydata[3];

        // "Decrypt" the block using the serial and key
        uchar8 *buf = (uchar8 *)wb->getData()+4;
        if (version == 0x204)
          buf += 280;
        uchar8 ci = serialmap[serialno & 0xff];
        uchar8 cj = keymap[keyno & 0xff];
        uchar8 ck = 0x60;

        for (uint32 i=0; i < 280; i++)
          buf[i] ^= (cj += ci * ck++);

        // Finally set the WB coeffs
        uint32 off = (version == 0x204) ? 6 : 14;
        mRaw->metadata.wbCoeffs[0] = get2BE(buf, off);
        mRaw->metadata.wbCoeffs[1] = get2BE(buf, off+2);
        mRaw->metadata.wbCoeffs[2] = get2BE(buf, off+6);
      }
    }
  } else if (mRootIFD->hasEntryRecursive((TiffTag)0x0014)) {
    TiffEntry *wb = mRootIFD->getEntryRecursive((TiffTag)0x0014);
    uchar8 *tmp = (uchar8 *) wb->getData();
    if (wb->count == 2560 && wb->type == TIFF_UNDEFINED) {
      uint32 red = ((uint32) tmp[1249]) | (((uint32) tmp[1248]) <<8);
      uint32 blue = ((uint32) tmp[1251]) | (((uint32) tmp[1250]) <<8);
      mRaw->metadata.wbCoeffs[0] = (float) red / 256.0f;
      mRaw->metadata.wbCoeffs[1] = 1.0f;
      mRaw->metadata.wbCoeffs[2] = (float) blue / 256.0f;
    } else if (!strncmp((char *)tmp,"NRW ",4)) {
      uint32 offset = 0;
      if (strncmp((char *)tmp+4,"0100",4) && wb->count > 72)
        offset = 56;
      else if (wb->count > 1572)
        offset = 1556;

      if (offset) {
        tmp += offset;
        mRaw->metadata.wbCoeffs[0] = (float) (get4LE(tmp,0) << 2);
        mRaw->metadata.wbCoeffs[1] = (float) (get4LE(tmp,4) + get4LE(tmp,8));
        mRaw->metadata.wbCoeffs[2] = (float) (get4LE(tmp,12) << 2);
      }
    }
  }

  string mode = getMode();
  string extended_mode = getExtendedMode(mode);
  if (meta->hasCamera(make, model, extended_mode)) {
    setMetaData(meta, make, model, extended_mode, iso);
  } else if (meta->hasCamera(make, model, mode)) {
    setMetaData(meta, make, model, mode, iso);
  } else {
    setMetaData(meta, make, model, "", iso);
  }

  if (white != 65536)
    mRaw->whitePoint = white;
  if (black >= 0 && hints.find(string("nikon_override_auto_black")) == hints.end())
    mRaw->blackLevel = black;
}

// Curve measured by libraw: https://github.com/LibRaw/LibRaw/blob/master/src/libraw_cxx.cpp#L1092-L1118
__inline float curveValue(float v) {
  float beta_1 = 5.79342238397656E-02f;
  float beta_2 = 3.28163551282665f;
  float beta_3 = -8.43136004842678f;
  float beta_4 = 1.03533181861023E+01f;
  float x = v* (1.0f/4096.f);
  float y = (1.f-expf(beta_1*x-beta_2*x*x-beta_3*x*x*x-beta_4*x*x*x*x));
  return y*16383.f;
}

// DecodeNikonYUY2 decodes 12 bit data in an YUY2-like pattern (2 Luma, 1 Chroma per 2 pixels).
// We un-apply the whitebalance, so output matches lossless.
// Note that values are scaled. See comment below on details.
// OPTME: It would be trivial to run this multithreaded.
void NefDecoder::DecodeNikonSNef(ByteStream &input, uint32 w, uint32 h) {
  uchar8* data = mRaw->getData();
  uint32 pitch = mRaw->pitch;
  const uchar8 *in = input.getData();
  if (input.getRemainSize() < (w*h*3)) {
    if ((uint32)input.getRemainSize() > w*3) {
      h = input.getRemainSize() / (w*3) - 1;
      mRaw->setError("Image truncated (file is too short)");
    } else
      ThrowIOE("DecodeNikonSNef: Not enough data to decode a single line. Image file truncated.");
  }

  // We need to read the applied whitebalance, since we should return
  // data before whitebalance, so we "unapply" it.
  vector<TiffIFD*> note = mRootIFD->getIFDsWithTag((TiffTag)12);

  if (note.empty())
    ThrowRDE("NEF Decoder: Unable to locate whitebalance needed for decompression");

  TiffEntry* wb = note[0]->getEntry((TiffTag)12);
  if (wb->count != 4 || wb->type != TIFF_RATIONAL)
    ThrowRDE("NEF Decoder: Whitebalance has unknown count or type");

  const uint32* wba = wb->getIntArray();
  if (!(wba[1] && wba[3] && wba[5] && wba[7]))
    ThrowRDE("NEF Decoder: Whitebalance has zero value");

  float wb_r = (float)wba[0] / (float)wba[1];
  float wb_b = (float)wba[2] / (float)wba[3];
  //float wb_g1 = (float)wba[4] / (float)wba[5];
  //float wb_g2 = (float)wba[6] / (float)wba[7];

  float inv_wb_r = 1.0f / wb_r;
  float inv_wb_b = 1.0f / wb_b;

  for (uint32 y = 0; y < h; y++) {
    ushort16* dest = (ushort16*) & data[y*pitch];
    for (uint32 x = 0 ; x < w*3; x += 6) {
      /* Decoding method and coefficients taken from
      http://www.rawdigger.com/howtouse/nikon-small-raw-internals */

      uint32 g1 = *in++;
      uint32 g2 = *in++;
      uint32 g3 = *in++;
      uint32 g4 = *in++;
      uint32 g5 = *in++;
      uint32 g6 = *in++;

      float y1 = (float)(g1 | ((g2 & 0x0f) << 8));
      float y2 = (float)((g2 >> 4) | (g3 << 4));
      float cb = (float)(g4 | ((g5 & 0x0f) << 8));
      float cr = (float)((g5 >> 4) | (g6 << 4));

      float cb2 = cb;
      float cr2 = cr;
      // Interpolate right pixel. We assume the sample is aligned with left pixel.
      if ((x+6) < w*3) {
        g5 = in[4];
        g6 = in[5];
        cb2 = ((float)(g4 | ((g5 & 0x0f) << 8)) + cb)*0.5f;
        cr2 = ((float)((g5 >> 4) | (g6 << 4)) + cr)* 0.5f;
      }

      // Scale Y to 2549 (maximum value determined by rawdigger)
      y1 = y1 * (4096.0f/2549.0f);
      y2 = y2 * (4096.0f/2549.0f);

      // Center cb/cr on 0. cb/cr has maximum of +- 1280 (recommended  by rawdigger)
      cb = (cb - 2048.0f)*(2048.0f/1280.0f);
      cr = (cr - 2048.0f)*(2048.0f/1280.0f);
      cb2 = (cb2 - 2048.0f)*(2048.0f/1280.0f);
      cr2 = (cr2 - 2048.0f)*(2048.0f/1280.0f);

      dest[x]   = clampbits((int)(inv_wb_r * curveValue(y1 + 1.40200f * cr)), 16);
      dest[x+1] = clampbits((int)(curveValue(y1 - 0.34414f * cb - 0.71414f * cr)), 16);
      dest[x+2] = clampbits((int)(inv_wb_b * curveValue(y1 + 1.77200f * cb)), 16);
      dest[x+3] = clampbits((int)(inv_wb_r * curveValue(y2 + 1.40200f * cr2)), 16);
      dest[x+4] = clampbits((int)(curveValue(y2 - 0.34414f * cb2 - 0.71414f * cr2)), 16);
      dest[x+5] = clampbits((int)(inv_wb_b * curveValue(y2 + 1.77200f * cb2)), 16);
    }
  }
}

} // namespace RawSpeed
