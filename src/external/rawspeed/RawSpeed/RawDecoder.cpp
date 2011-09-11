#include "StdAfx.h"
#include "RawDecoder.h"
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

	RawDecoder::RawDecoder(FileMap* file) : mRaw(RawImage::create()), mFile(file) {
  decoderVersion = 0;
  failOnUnknown = FALSE;
}

RawDecoder::~RawDecoder(void) {
  for (uint32 i = 0 ; i < errors.size(); i++) {
    free((void*)errors[i]);
  }
  errors.clear();
}

void RawDecoder::decodeUncompressed(TiffIFD *rawIFD, bool MSBOrder) {
  uint32 nslices = rawIFD->getEntry(STRIPOFFSETS)->count;
  const uint32 *offsets = rawIFD->getEntry(STRIPOFFSETS)->getIntArray();
  const uint32 *counts = rawIFD->getEntry(STRIPBYTECOUNTS)->getIntArray();
  uint32 yPerSlice = rawIFD->getEntry(ROWSPERSTRIP)->getInt();
  uint32 width = rawIFD->getEntry(IMAGEWIDTH)->getInt();
  uint32 height = rawIFD->getEntry(IMAGELENGTH)->getInt();
  uint32 bitPerPixel = rawIFD->getEntry(BITSPERSAMPLE)->getInt();

  vector<RawSlice> slices;
  uint32 offY = 0;

  for (uint32 s = 0; s < nslices; s++) {
    RawSlice slice;
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
    ThrowRDE("RAW Decoder: No valid slices found. File probably truncated.");

  mRaw->dim = iPoint2D(width, offY);
  mRaw->createData();
  mRaw->whitePoint = (1<<bitPerPixel)-1;

  offY = 0;
  for (uint32 i = 0; i < slices.size(); i++) {
    RawSlice slice = slices[i];
    ByteStream in(mFile->getData(slice.offset), slice.count);
    iPoint2D size(width, slice.h);
    iPoint2D pos(0, offY);
    bitPerPixel = (int)((uint64)(slice.count * 8) / (slice.h * width));
    try {
      readUncompressedRaw(in, size, pos, width*bitPerPixel / 8, bitPerPixel, MSBOrder);
    } catch (RawDecoderException e) {
      if (i>0)
        errors.push_back(_strdup(e.what()));
      else
        throw;
    } catch (IOException e) {
      if (i>0)
        errors.push_back(_strdup(e.what()));
      else
        ThrowRDE("RAW decoder: IO error occurred in first slice, unable to decode more. Error is: %s", e.what());
    }
    offY += slice.h;
  }
}

void RawDecoder::readUncompressedRaw(ByteStream &input, iPoint2D& size, iPoint2D& offset, int inputPitch, int bitPerPixel, bool MSBOrder) {
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
  if (bitPerPixel > 16 && mRaw->getDataType() == TYPE_USHORT16)
    ThrowRDE("readUncompressedRaw: Unsupported bit depth");

  uint32 skipBits = inputPitch - w * bitPerPixel / 8;  // Skip per line
  if (offset.y > mRaw->dim.y)
    ThrowRDE("readUncompressedRaw: Invalid y offset");
  if (offset.x + size.x > mRaw->dim.x)
    ThrowRDE("readUncompressedRaw: Invalid x offset");

  uint32 y = offset.y;
  h = MIN(h + (uint32)offset.y, (uint32)mRaw->dim.y);

  if (mRaw->getDataType() == TYPE_FLOAT32)
  {
    if (bitPerPixel != 32)
      ThrowRDE("readUncompressedRaw: Only 32 bit float point supported");
    BitBlt(&data[offset.x*sizeof(float)*cpp+y*outPitch], outPitch,
        input.getData(), inputPitch, w*mRaw->getBpp(), h - y);
    return;
  }

  if (MSBOrder) {
    BitPumpMSB bits(&input);
    w *= cpp;
    for (; y < h; y++) {
      ushort16* dest = (ushort16*) & data[offset.x*sizeof(ushort16)*cpp+y*outPitch];
      bits.checkPos();
      for (uint32 x = 0 ; x < w; x++) {
        uint32 b = bits.getBits(bitPerPixel);
        dest[x] = b;
      }
      bits.skipBits(skipBits);
    }

  } else {

    if (bitPerPixel == 16 && getHostEndianness() == little)  {
      BitBlt(&data[offset.x*sizeof(ushort16)*cpp+y*outPitch], outPitch,
             input.getData(), inputPitch, w*mRaw->getBpp(), h - y);
      return;
    }
    if (bitPerPixel == 12 && (int)w == inputPitch * 8 / 12 && getHostEndianness() == little)  {
      Decode12BitRaw(input, w, h);
      return;
    }
    BitPumpPlain bits(&input);
    w *= cpp;
    for (; y < h; y++) {
      ushort16* dest = (ushort16*) & data[offset.x*sizeof(ushort16)+y*outPitch];
      bits.checkPos();
      for (uint32 x = 0 ; x < w; x++) {
        uint32 b = bits.getBits(bitPerPixel);
        dest[x] = b;
      }
      bits.skipBits(skipBits);
    }
  }
}

void RawDecoder::Decode12BitRaw(ByteStream &input, uint32 w, uint32 h) {
  uchar8* data = mRaw->getData();
  uint32 pitch = mRaw->pitch;
  const uchar8 *in = input.getData();
  if (input.getRemainSize() < ((w*12/8)*h)) {
    if ((uint32)input.getRemainSize() > (w*12/8))
      h = input.getRemainSize() / (w*12/8) - 1;
    else
      ThrowIOE("readUncompressedRaw: Not enough data to decode a single line. Image file truncated.");
  }
  for (uint32 y = 0; y < h; y++) {
    ushort16* dest = (ushort16*) & data[y*pitch];
    for (uint32 x = 0 ; x < w; x += 2) {
      uint32 g1 = *in++;
      uint32 g2 = *in++;
      dest[x] = g1 | ((g2 & 0xf) << 8);
      uint32 g3 = *in++;
      dest[x+1] = (g2 >> 4) | (g3 << 4);
    }
  }
}

bool RawDecoder::checkCameraSupported(CameraMetaData *meta, string make, string model, string mode) {
  TrimSpaces(make);
  TrimSpaces(model);
  Camera* cam = meta->getCamera(make, model, mode);
  if (!cam) {
    if (mode.length() == 0)
      printf("Unable to find camera in database: %s %s %s\n", make.c_str(), model.c_str(), mode.c_str());

     if (failOnUnknown)
       ThrowRDE("Camera not supported, and not allowed to guess. Sorry.");

    // Assume the camera can be decoded, but return false, so decoders can see that we are unsure.
    return false;    
  }

  if (!cam->supported)
    ThrowRDE("Camera not supported (explicit). Sorry.");

  if (cam->decoderVersion > decoderVersion)
    ThrowRDE("Camera not supported in this version. Update RawSpeed for support.");

  hints = cam->hints;
  return true;
}

void RawDecoder::setMetaData(CameraMetaData *meta, string make, string model, string mode) {
  TrimSpaces(make);
  TrimSpaces(model);
  Camera *cam = meta->getCamera(make, model, mode);
  if (!cam) {
    printf("Unable to find camera in database: %s %s %s\nPlease upload file to ftp.rawstudio.org, thanks!\n", make.c_str(), model.c_str(), mode.c_str());
    return;
  }

  iPoint2D new_size = cam->cropSize;

  // If crop size is negative, use relative cropping
  if (new_size.x <= 0)
    new_size.x = mRaw->dim.x - cam->cropPos.x + new_size.x;

  if (new_size.y <= 0)
    new_size.y = mRaw->dim.y - cam->cropPos.y + new_size.y;

  mRaw->subFrame(cam->cropPos, new_size);
  mRaw->cfa = cam->cfa;

  // Shift CFA to match crop
  if (cam->cropPos.x & 1)
    mRaw->cfa.shiftLeft();
  if (cam->cropPos.y & 1)
    mRaw->cfa.shiftDown();

  mRaw->blackLevel = cam->black;
  mRaw->whitePoint = cam->white;
  mRaw->blackAreas = cam->blackAreas;

}


void *RawDecoderDecodeThread(void *_this) {
  RawDecoderThread* me = (RawDecoderThread*)_this;
  try {
    me->parent->decodeThreaded(me);
  } catch (RawDecoderException ex) {
    me->error = _strdup(ex.what());
  } catch (IOException ex) {
    me->error = _strdup(ex.what());
  }

  pthread_exit(NULL);
  return 0;
}

void RawDecoder::startThreads() {
  uint32 threads;
  threads = getThreadCount(); 
  int y_offset = 0;
  int y_per_thread = (mRaw->dim.y + threads - 1) / threads;
  RawDecoderThread *t = new RawDecoderThread[threads];

  pthread_attr_t attr;

  /* Initialize and set thread detached attribute */
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

  for (uint32 i = 0; i < threads; i++) {
    t[i].start_y = y_offset;
    t[i].end_y = MIN(y_offset + y_per_thread, mRaw->dim.y);
    t[i].parent = this;
    pthread_create(&t[i].threadid, &attr, RawDecoderDecodeThread, &t[i]);
    y_offset = t[i].end_y;
  }

  void *status;
  for (uint32 i = 0; i < threads; i++) {
    pthread_join(t[i].threadid, &status);
    if (t[i].error) {
      errors.push_back(t[i].error);
    }
  }
  if (errors.size() >= threads)
    ThrowRDE("RawDecoder::startThreads: All threads reported errors. Cannot load image.");

  delete[] t;
}

void RawDecoder::decodeThreaded(RawDecoderThread * t) {
  ThrowRDE("Internal Error: This class does not support threaded decoding");
}

} // namespace RawSpeed