#include "StdAfx.h"
#include "RafDecoder.h"

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

RafDecoder::RafDecoder(TiffIFD *rootIFD, FileMap* file) :
    RawDecoder(file), mRootIFD(rootIFD) {
      decoderVersion = 1;
}
RafDecoder::~RafDecoder(void) {
  if (mRootIFD)
    delete mRootIFD;
  mRootIFD = NULL;
}

RawImage RafDecoder::decodeRawInternal() {

  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(FUJI_STRIPOFFSETS);

  if (data.empty())
    ThrowRDE("Fuji decoder: Unable to locate raw IFD");

  TiffIFD* raw = data[0];
  mFile = raw->getFileMap();
  uint32 height = 0;
  uint32 width = 0;

  alt_layout = hints.find("set_alt_layout") == hints.end();
  fuji_width = hints.find("set_fuji_width") == hints.end();

  if (raw->hasEntry(FUJI_RAWIMAGEFULLHEIGHT)) {
    height = raw->getEntry(FUJI_RAWIMAGEFULLHEIGHT)->getInt();
    width = raw->getEntry(FUJI_RAWIMAGEFULLWIDTH)->getInt();
  } else if (raw->hasEntry((TiffTag)0x100)) {
    TiffEntry *e = raw->getEntry((TiffTag)0x100);
    if (e->count < 2)
      ThrowRDE("Fuji decoder: Size array too small");
    const ushort16 *size = e->getShortArray();
    height = size[0];
    width = size[1];
  } 
  if (raw->hasEntry((TiffTag)0x130)) {
    TiffEntry *e = raw->getEntry((TiffTag)0x130);
    if (e->count < 2)
      ThrowRDE("Fuji decoder: Layout array too small");
    const uchar8 *layout = e->getData();
    alt_layout = layout[0] >> 7;
    fuji_width = !(layout[1] & 8);
  }
  if (raw->hasEntry((TiffTag)0x121)) {
    TiffEntry *e = raw->getEntry((TiffTag)0x121);
    if (e->count < 2)
      ThrowRDE("Fuji decoder: Size array too small");

    const ushort16 *size = e->getShortArray();

    final_size = iPoint2D(size[1], size[0]);
    if (final_size.x == 4284)
      final_size.x += 3;
  }
  if (raw->hasEntry((TiffTag)0xc000)) {
    TiffEntry *e = raw->getEntry((TiffTag)0xc000);
    if (e->count < 2)
      ThrowRDE("Fuji decoder: Size array too small");
    const uint32 *size = e->getIntArray();

    int index = 0;
    final_size.x = size[index++];
    if (final_size.x > 10000  && e->count > 2)
      final_size.x  = size[index++];
    final_size.y = size[index++];
  }

  final_size.x >>= alt_layout;
  final_size.y <<= alt_layout;

  fuji_width = final_size.x >> (alt_layout ? 0 : 1);
  final_size.x = (final_size.y >> alt_layout) + fuji_width;
  final_size.y = final_size.x - 1;

  if (width <= 0 ||  height <= 0)
    ThrowRDE("RAF decoder: Unable to locate image size");

  TiffEntry *offsets = raw->getEntry(FUJI_STRIPOFFSETS);
  //TiffEntry *counts = raw->getEntry(FUJI_STRIPBYTECOUNTS);

  int bps = 16;
  if (raw->hasEntry(FUJI_BITSPERSAMPLE))    
    bps = raw->getEntry(FUJI_BITSPERSAMPLE)->getInt();

  if (offsets->count != 1)
    ThrowRDE("RAF Decoder: Multiple Strips found: %u", offsets->count);

  int off = offsets->getInt();
  if (!mFile->isValid(off))
    ThrowRDE("RAF RAW Decoder: Invalid image data offset, cannot decode.");

  mRaw->dim = iPoint2D(width, height);
  mRaw->createData();
  ByteStream input_start(mFile->getData(off), mFile->getSize() - off);
  iPoint2D pos(0, 0);
  readUncompressedRaw(input_start, mRaw->dim,pos, width*bps/8, bps, BitOrder_Plain);

  return mRaw;
}


void RafDecoder::decodeThreaded(RawDecoderThread * t) {
}

void RafDecoder::checkSupportInternal(CameraMetaData *meta) {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);
  if (data.empty())
    ThrowRDE("RAF Support check: Model name found");

  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();
  if (!this->checkCameraSupported(meta, make, model, ""))
     ThrowRDE("RAFDecoder: Unknown camera. Will not guess.");
}

void RafDecoder::decodeMetaDataInternal(CameraMetaData *meta) {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);

  if (data.empty())
    ThrowRDE("RAF Meta Decoder: Model name not found");
  if (!data[0]->hasEntry(MAKE))
    ThrowRDE("RAF Support: Make name not found");

  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();

  int iso = 0;
  if (mRootIFD->hasEntryRecursive(ISOSPEEDRATINGS))
    iso = mRootIFD->getEntryRecursive(ISOSPEEDRATINGS)->getInt();
  mRaw->isoSpeed = iso;
  /* We fetch data ourselves */
  TrimSpaces(make);
  TrimSpaces(model);
  Camera *cam = meta->getCamera(make, model, "");

  iPoint2D new_size(mRaw->dim);
  iPoint2D crop_offset = iPoint2D(0,0);
  if (cam && applyCrop) {
    new_size = cam->cropSize;
    crop_offset = cam->cropPos;
    // If crop size is negative, use relative cropping
    if (new_size.x <= 0)
      new_size.x = mRaw->dim.x - cam->cropPos.x + new_size.x;

    if (new_size.y <= 0)
      new_size.y = mRaw->dim.y - cam->cropPos.y + new_size.y;
  }

  bool rotate = hints.find("fuji_rotate") != hints.end();
  rotate = rotate & fujiRotate;

  // Rotate 45 degrees - could be multithreaded.
  if (rotate && !this->uncorrectedRawValues) {
    RawImage rotated = RawImage::create(final_size, TYPE_USHORT16, 1);
    rotated->clearArea(iRectangle2D(iPoint2D(0,0), rotated->dim));
    uint32 max_x = fuji_width << (alt_layout ? 0 : 1);
    int r,c;
    int dest_pitch = (int)rotated->pitch / 2;
    ushort16 *dst = (ushort16*)rotated->getData(0,0);
    for (int y = 0; y < new_size.y; y++) {
      ushort16 *src = (ushort16*)mRaw->getData(crop_offset.x, crop_offset.y + y);
      for (uint32 x = 0; x < max_x; x++) {
        if (alt_layout) {
          r = fuji_width - 1 - x + (y >> 1);
          c = x + ((y+1) >> 1);
        } else {
          r = fuji_width - 1 + y - (x >> 1);
          c = y + ((x+1) >> 1);
        }
        if (r < rotated->dim.y && c < rotated->dim.y)
          dst[c + r * dest_pitch] = src[x];
      }
    }
    mRaw = rotated;
  } else if (applyCrop) {
    mRaw->subFrame(iRectangle2D(crop_offset, new_size));
  }

  mRaw->cfa.setCFA(iPoint2D(2,2), CFA_GREEN, CFA_BLUE, CFA_RED, CFA_GREEN);
  mRaw->isCFA = true;
  if (cam) {
    const CameraSensorInfo *sensor = cam->getSensorInfo(iso);
    mRaw->blackLevel = sensor->mBlackLevel;
    mRaw->whitePoint = sensor->mWhiteLevel;
    mRaw->blackAreas = cam->blackAreas;
    mRaw->cfa = cam->cfa;
  }
  if (rotate)
    mRaw->fujiWidth = fuji_width;
}


} // namespace RawSpeed
