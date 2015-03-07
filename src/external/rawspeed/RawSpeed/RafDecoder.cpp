#include "StdAfx.h"
#include "RafDecoder.h"

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

RafDecoder::RafDecoder(TiffIFD *rootIFD, FileMap* file) :
    RawDecoder(file), mRootIFD(rootIFD) {
  decoderVersion = 1;
  alt_layout = FALSE;
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

  // Some fuji SuperCCD cameras include a second raw image next to the first one
  // that is identical but darker to the first. The two combined can produce
  // a higher dynamic range image. Right now we're ignoring it.
  bool double_width = hints.find("double_width_unpacked") != hints.end();

  mRaw->dim = iPoint2D(width*(double_width ? 2 : 1), height);
  mRaw->createData();
  ByteStream input(mFile->getData(off), mFile->getSize() - off);
  iPoint2D pos(0, 0);

  if (double_width) {
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
  mRaw->metadata.isoSpeed = iso;

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
    bool double_width = hints.find("double_width_unpacked") != hints.end();
    // If crop size is negative, use relative cropping
    if (new_size.x <= 0)
      new_size.x = mRaw->dim.x / (double_width ? 2 : 1) - cam->cropPos.x + new_size.x;
    else
      new_size.x /= (double_width ? 2 : 1);

    if (new_size.y <= 0)
      new_size.y = mRaw->dim.y - cam->cropPos.y + new_size.y;
  }

  bool rotate = hints.find("fuji_rotate") != hints.end();
  rotate = rotate & fujiRotate;

  // Rotate 45 degrees - could be multithreaded.
  if (rotate && !this->uncorrectedRawValues) {
    // Calculate the 45 degree rotated size;
    uint32 rotatedsize;
    uint32 rotationPos;
    if (alt_layout) {
      rotatedsize = new_size.y+new_size.x/2;
      rotationPos = new_size.x/2 - 1;
    }
    else {
      rotatedsize = new_size.x+new_size.y/2;
      rotationPos = new_size.x - 1;
    }

    iPoint2D final_size(rotatedsize, rotatedsize-1);
    RawImage rotated = RawImage::create(final_size, TYPE_USHORT16, 1);
    rotated->clearArea(iRectangle2D(iPoint2D(0,0), rotated->dim));
    rotated->metadata = mRaw->metadata;
    rotated->metadata.fujiRotationPos = rotationPos;

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

  if (mRootIFD->hasEntryRecursive(FUJI_WB_GRBLEVELS)) {
    TiffEntry *wb = mRootIFD->getEntryRecursive(FUJI_WB_GRBLEVELS);
    if (wb->count == 3) {
      const uint32 *tmp = wb->getIntArray();
      mRaw->metadata.wbCoeffs[0] = (float)tmp[1];
      mRaw->metadata.wbCoeffs[1] = (float)tmp[0];
      mRaw->metadata.wbCoeffs[2] = (float)tmp[2];
    }
  } else if (mRootIFD->hasEntryRecursive(FUJIOLDWB)) {
    TiffEntry *wb = mRootIFD->getEntryRecursive(FUJIOLDWB);
    if (wb->count == 8) {
      const ushort16 *tmp = wb->getShortArray();
      mRaw->metadata.wbCoeffs[0] = (float)tmp[1];
      mRaw->metadata.wbCoeffs[1] = (float)tmp[0];
      mRaw->metadata.wbCoeffs[2] = (float)tmp[3];
    }
  }
}


} // namespace RawSpeed
