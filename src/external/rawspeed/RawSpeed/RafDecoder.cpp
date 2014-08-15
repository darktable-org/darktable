#include "StdAfx.h"
#include "RafDecoder.h"

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
  alt_layout = FALSE;

  if (raw->hasEntry(FUJI_RAWIMAGEFULLHEIGHT)) {
    height = raw->getEntry(FUJI_RAWIMAGEFULLHEIGHT)->getInt();
    width = raw->getEntry(FUJI_RAWIMAGEFULLWIDTH)->getInt();
  } else if (raw->hasEntry(IMAGEWIDTH)) {
    TiffEntry *e = raw->getEntry(IMAGEWIDTH);
    if (e->count < 2)
      ThrowRDE("Fuji decoder: Size array too small");
    const ushort16 *size = e->getShortArray();
    height = size[0];
    width = size[1];
  } 
  if (raw->hasEntry(FUJI_LAYOUT)) {
    TiffEntry *e = raw->getEntry(FUJI_LAYOUT);
    if (e->count < 2)
      ThrowRDE("Fuji decoder: Layout array too small");
    const uchar8 *layout = e->getData();
    alt_layout = !(layout[0] >> 7);
  }

  if (width <= 0 ||  height <= 0)
    ThrowRDE("RAF decoder: Unable to locate image size");

  TiffEntry *offsets = raw->getEntry(FUJI_STRIPOFFSETS);

  if (offsets->count != 1)
    ThrowRDE("RAF Decoder: Multiple Strips found: %u", offsets->count);

  int off = offsets->getInt();
  if (!mFile->isValid(off))
    ThrowRDE("RAF RAW Decoder: Invalid image data offset, cannot decode.");

  int bps = 16;
  if (raw->hasEntry(FUJI_BITSPERSAMPLE))
    bps = raw->getEntry(FUJI_BITSPERSAMPLE)->getInt();
  // x-trans sensors report 14bpp, but data isn't packed so read as 16bpp
  if (bps == 14) bps = 16;

  mRaw->dim = iPoint2D(width, height);
  mRaw->createData();
  ByteStream input(mFile->getData(off), mFile->getSize() - off);
  iPoint2D pos(0, 0);

  if (hints.find("double_width_unpacked") != hints.end()) {
    Decode16BitRawUnpacked(input, width*2, height);
  } else if (mRootIFD->endian == big) {
    Decode16BitRawBEunpacked(input, width, height);
  } else {
    if (hints.find("jpeg32_bitorder") != hints.end())
      readUncompressedRaw(input, mRaw->dim, pos, width*bps/8, bps, BitOrder_Jpeg32);
    else
      readUncompressedRaw(input, mRaw->dim, pos, width*bps/8, bps, BitOrder_Plain);
  }
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

  // This is where we'd normally call setMetaData but since we may still need
  // to rotate the image for SuperCCD cameras we do everything ourselves
  TrimSpaces(make);
  TrimSpaces(model);
  Camera *cam = meta->getCamera(make, model, "");
  if (!cam)
    ThrowRDE("RAF Meta Decoder: Couldn't find camera");

  iPoint2D new_size(mRaw->dim);
  iPoint2D crop_offset = iPoint2D(0,0);
  if (applyCrop) {
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
    // Calculate the 45 degree rotated size;
    uint32 rotatedsize;
    if (alt_layout)
      rotatedsize = new_size.y+new_size.x/2;
    else
      rotatedsize = new_size.x+new_size.y/2;

    iPoint2D final_size(rotatedsize, rotatedsize);
    RawImage rotated = RawImage::create(final_size, TYPE_USHORT16, 1);
    rotated->clearArea(iRectangle2D(iPoint2D(0,0), rotated->dim));
    int dest_pitch = (int)rotated->pitch / 2;
    ushort16 *dst = (ushort16*)rotated->getData(0,0);

    for (int y = 0; y < new_size.y; y++) {
      ushort16 *src = (ushort16*)mRaw->getData(crop_offset.x, crop_offset.y + y);
      for (int x = 0; x < new_size.x; x++) {
        int h, w;
        if (alt_layout) { // Swapped x and y
          h = rotatedsize - (new_size.y + 1 - y + (x >> 1));
          w = ((x+1) >> 1) + y;
        } else {
          h = new_size.x - 1 - x + (y >> 1);
          w = ((y+1) >> 1) + x;
        }
        if (h < rotated->dim.y && w < rotated->dim.x)
          dst[w + h * dest_pitch] = src[x];
        else
          ThrowRDE("RAF Decoder: Trying to write out of bounds");
      }
    }
    mRaw = rotated;
  } else if (applyCrop) {
    mRaw->subFrame(iRectangle2D(crop_offset, new_size));
  }

  const CameraSensorInfo *sensor = cam->getSensorInfo(iso);
  mRaw->blackLevel = sensor->mBlackLevel;
  mRaw->whitePoint = sensor->mWhiteLevel;
  mRaw->blackAreas = cam->blackAreas;
  mRaw->cfa = cam->cfa;
}


} // namespace RawSpeed
