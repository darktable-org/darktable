#include "StdAfx.h"
#include "DngOpcodes.h"
/* 
RawSpeed - RAW file decoder.

Copyright (C) 2012 Klaus Post

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

DngOpcodes::DngOpcodes(TiffEntry *entry)
{
  host = getHostEndianness();
  const uchar8* data = entry->getData();
  uint32 entry_size = entry->count;
  uint32 opcode_count = getULong(&data[0]);

  int bytes_used = 4;
  for (uint32 i = 0; i < opcode_count; i++) {
    uint32 code = getULong(&data[bytes_used]);
    //uint32 version = getULong(&data[bytes_used+4]);
    uint32 flags = getULong(&data[bytes_used+8]);
    uint32 expected_size = getULong(&data[bytes_used+12]);
    bytes_used += 16;
    uint32 opcode_used = 0;
    switch (code)
    {
      case 4:
        mOpcodes.push_back(new OpcodeFixBadPixelsConstant(&data[bytes_used], entry_size - bytes_used, &opcode_used));
        break;
      case 5:
        mOpcodes.push_back(new OpcodeFixBadPixelsList(&data[bytes_used], entry_size - bytes_used, &opcode_used));
        break;
      case 6:
        mOpcodes.push_back(new OpcodeTrimBounds(&data[bytes_used], entry_size - bytes_used, &opcode_used));
        break;
      case 7:
        mOpcodes.push_back(new OpcodeMapTable(&data[bytes_used], entry_size - bytes_used, &opcode_used));
        break;
      case 8:
        mOpcodes.push_back(new OpcodeMapPolynomial(&data[bytes_used], entry_size - bytes_used, &opcode_used));
        break;
      case 10:
        mOpcodes.push_back(new OpcodeDeltaPerRow(&data[bytes_used], entry_size - bytes_used, &opcode_used));
        break;
      case 11:
        mOpcodes.push_back(new OpcodeDeltaPerCol(&data[bytes_used], entry_size - bytes_used, &opcode_used));
        break;
      case 12:
        mOpcodes.push_back(new OpcodeScalePerRow(&data[bytes_used], entry_size - bytes_used, &opcode_used));
        break;
      case 13:
        mOpcodes.push_back(new OpcodeScalePerCol(&data[bytes_used], entry_size - bytes_used, &opcode_used));
        break;
      default:
        // Throw Error if not marked as optional
        if (!(flags & 1))
          ThrowRDE("DngOpcodes: Unsupported Opcode: %d", code);
    }
    if (opcode_used != expected_size)
      ThrowRDE("DngOpcodes: Inconsistent length of opcode");
    bytes_used += opcode_used;
    if (bytes_used > (int)entry_size)
      ThrowRDE("DngOpcodes: More codes than entry size (should be caught earlier)");
  }
}

DngOpcodes::~DngOpcodes(void)
{
  size_t codes = mOpcodes.size();
  for (uint32 i = 0; i < codes; i++)
    delete mOpcodes[i];
  mOpcodes.clear();
}

/* TODO: Apply in separate threads */
RawImage& DngOpcodes::applyOpCodes( RawImage &img )
{
  size_t codes = mOpcodes.size();
  for (uint32 i = 0; i < codes; i++)
  {
    DngOpcode* code = mOpcodes[i];
    RawImage img_out = code->createOutput(img);
    iRectangle2D fullImage(0,0,img->dim.x, img->dim.y);

    if (!code->mAoi.isThisInside(fullImage))
      ThrowRDE("DngOpcodes: Area of interest not inside image!");
    if (code->mAoi.hasPositiveArea()) {
      code->apply(img, img_out, code->mAoi.getTop(), code->mAoi.getBottom());
      img = img_out;
    }
  }
  return img;
}

/***************** OpcodeFixBadPixelsConstant   ****************/

OpcodeFixBadPixelsConstant::OpcodeFixBadPixelsConstant(const uchar8* parameters, int param_max_bytes, uint32 *bytes_used )
{
  if (param_max_bytes < 8)
    ThrowRDE("OpcodeFixBadPixelsConstant: Not enough data to read parameters, only %d bytes left.", param_max_bytes);
  mValue = getLong(&parameters[0]);
  // Bayer Phase not used
  *bytes_used = 8;
  mFlags = MultiThreaded;
}

RawImage& OpcodeFixBadPixelsConstant::createOutput( RawImage &in )
{
  // These limitations are present within the DNG SDK as well.
  if (in->getDataType() != TYPE_USHORT16)
    ThrowRDE("OpcodeFixBadPixelsConstant: Only 16 bit images supported");

  if (in->getCpp() > 1)
    ThrowRDE("OpcodeFixBadPixelsConstant: This operation is only supported with 1 component");

  return in;
}

void OpcodeFixBadPixelsConstant::apply( RawImage &in, RawImage &out, int startY, int endY )
{
  iPoint2D crop = in->getCropOffset();
  uint32 offset = crop.x | (crop.y << 16);
  vector<uint32> bad_pos;
  for (int y = startY; y < endY; y ++) {
    ushort16* src = (ushort16*)out->getData(0, y);
    for (int x = 0; x < in->dim.x; x++) {
      if (src[x]== mValue) {
        bad_pos.push_back(offset + ((uint32)x | (uint32)y<<16));
      }
    }
  }
  if (!bad_pos.empty()) {
    pthread_mutex_lock(&out->mBadPixelMutex);
    out->mBadPixelPositions.insert(out->mBadPixelPositions.end(), bad_pos.begin(), bad_pos.end());
    pthread_mutex_unlock(&out->mBadPixelMutex);
  }

}

/***************** OpcodeFixBadPixelsList   ****************/

OpcodeFixBadPixelsList::OpcodeFixBadPixelsList( const uchar8* parameters, int param_max_bytes, uint32 *bytes_used )
{
  if (param_max_bytes < 12)
    ThrowRDE("OpcodeFixBadPixelsList: Not enough data to read parameters, only %d bytes left.", param_max_bytes);
  // Skip phase - we don't care
  int BadPointCount = getLong(&parameters[4]);
  int BadRectCount = getLong(&parameters[8]);
  bytes_used[0] = 12; 
  if (12 + BadPointCount * 8 + BadRectCount * 16 > param_max_bytes)
    ThrowRDE("OpcodeFixBadPixelsList: Ran out parameter space, only %d bytes left.", param_max_bytes);

  // Read points
  for (int i = 0; i < BadPointCount; i++) {
    uint32 BadPointRow = (uint32)getLong(&parameters[bytes_used[0]]);
    uint32 BadPointCol = (uint32)getLong(&parameters[bytes_used[0]+4]);
    bytes_used[0] += 8;
    bad_pos.push_back(BadPointRow | (BadPointCol << 16));
  }

  // Read rects
  for (int i = 0; i < BadRectCount; i++) {
    uint32 BadRectTop = (uint32)getLong(&parameters[bytes_used[0]]);
    uint32 BadRectLeft = (uint32)getLong(&parameters[bytes_used[0]+4]);
    uint32 BadRectBottom = (uint32)getLong(&parameters[bytes_used[0]]);
    uint32 BadRectRight = (uint32)getLong(&parameters[bytes_used[0]+4]);
    bytes_used[0] += 16;
    if (BadRectTop < BadRectBottom && BadRectLeft < BadRectRight) {
      for (uint32 y = BadRectLeft; y <= BadRectRight; y++) {
        for (uint32 x = BadRectTop; x <= BadRectBottom; x++) {
          bad_pos.push_back(x | (y << 16));
        }
      }
    }
  }
}

void OpcodeFixBadPixelsList::apply( RawImage &in, RawImage &out, int startY, int endY )
{
  iPoint2D crop = in->getCropOffset();
  uint32 offset = crop.x | (crop.y << 16);
  for (vector<uint32>::iterator i=bad_pos.begin(); i != bad_pos.end(); i++) {
    uint32 pos = offset + (*i);
    out->mBadPixelPositions.push_back(pos);
  }
}

 /***************** OpcodeTrimBounds   ****************/

OpcodeTrimBounds::OpcodeTrimBounds(const uchar8* parameters, int param_max_bytes, uint32 *bytes_used )
{
  if (param_max_bytes < 16)
    ThrowRDE("OpcodeTrimBounds: Not enough data to read parameters, only %d bytes left.", param_max_bytes);
  mTop = getLong(&parameters[0]);
  mLeft = getLong(&parameters[4]);
  mBottom = getLong(&parameters[8]);
  mRight = getLong(&parameters[12]);
  *bytes_used = 16;
}

void OpcodeTrimBounds::apply( RawImage &in, RawImage &out, int startY, int endY )
{
  iRectangle2D crop(mLeft, mTop, mRight-mLeft, mBottom-mTop);
  out->subFrame(crop);
}

/***************** OpcodeMapTable   ****************/

OpcodeMapTable::OpcodeMapTable(const uchar8* parameters, int param_max_bytes, uint32 *bytes_used )
{
  if (param_max_bytes < 36)
    ThrowRDE("OpcodeMapTable: Not enough data to read parameters, only %d bytes left.", param_max_bytes);
  mAoi.setAbsolute(getLong(&parameters[4]), getLong(&parameters[0]), getLong(&parameters[12]), getLong(&parameters[8]));
  mFirstPlane = getLong(&parameters[16]);
  mPlanes = getLong(&parameters[20]);
  mRowPitch = getLong(&parameters[24]);
  mColPitch = getLong(&parameters[28]);
  if (mFirstPlane < 0)
    ThrowRDE("OpcodeMapPolynomial: Negative first plane");
  if (mPlanes <= 0)
    ThrowRDE("OpcodeMapPolynomial: Negative number of planes");
  if (mRowPitch <= 0 || mColPitch <= 0)
    ThrowRDE("OpcodeMapPolynomial: Invalid Pitch");

  int tablesize = getLong(&parameters[32]);
  *bytes_used = 36;

  if (tablesize > 65536)
    ThrowRDE("OpcodeMapTable: A map with more than 65536 entries not allowed");

  if (param_max_bytes < 36 + (tablesize*2))
    ThrowRDE("OpcodeMapPolynomial: Not enough data to read parameters, only %d bytes left.", param_max_bytes);

  for (int i = 0; i <= 65535; i++)
  {
    int location = min(tablesize-1, i);
    mLookup[i] = getUshort(&parameters[36+2*location]);
  }

  *bytes_used += tablesize*2;
  mFlags = MultiThreaded | PureLookup;
}


RawImage& OpcodeMapTable::createOutput( RawImage &in )
{
  if (in->getDataType() != TYPE_USHORT16)
    ThrowRDE("OpcodeMapTable: Only 16 bit images supported");

  if (mFirstPlane > (int)in->getCpp())
    ThrowRDE("OpcodeMapTable: Not that many planes in actual image");

  if (mFirstPlane+mPlanes > (int)in->getCpp())
    ThrowRDE("OpcodeMapTable: Not that many planes in actual image");

  return in;
}

void OpcodeMapTable::apply( RawImage &in, RawImage &out, int startY, int endY )
{
  int cpp = out->getCpp();
  for (int y = startY; y < endY; y += mRowPitch) {
    ushort16 *src = (ushort16*)out->getData(mAoi.getLeft(), y);
    // Add offset, so this is always first plane
    src+=mFirstPlane;
    for (int x = 0; x < mAoi.getWidth(); x += mColPitch) {
      for (int p = 0; p < mPlanes; p++)
      {
        src[x*cpp+p] = mLookup[src[x*cpp+p]];
      }
    }
  }
}



 /***************** OpcodeMapPolynomial   ****************/

OpcodeMapPolynomial::OpcodeMapPolynomial(const uchar8* parameters, int param_max_bytes, uint32 *bytes_used )
{
  if (param_max_bytes < 36)
    ThrowRDE("OpcodeMapPolynomial: Not enough data to read parameters, only %d bytes left.", param_max_bytes);
  mAoi.setAbsolute(getLong(&parameters[4]), getLong(&parameters[0]), getLong(&parameters[12]), getLong(&parameters[8]));
  mFirstPlane = getLong(&parameters[16]);
  mPlanes = getLong(&parameters[20]);
  mRowPitch = getLong(&parameters[24]);
  mColPitch = getLong(&parameters[28]);
  if (mFirstPlane < 0)
    ThrowRDE("OpcodeMapPolynomial: Negative first plane");
  if (mPlanes <= 0)
    ThrowRDE("OpcodeMapPolynomial: Negative number of planes");
  if (mRowPitch <= 0 || mColPitch <= 0)
    ThrowRDE("OpcodeMapPolynomial: Invalid Pitch");

  mDegree = getLong(&parameters[32]);
  *bytes_used = 36;
  if (mDegree > 8)
    ThrowRDE("OpcodeMapPolynomial: A polynomial with more than 8 degrees not allowed");
  if (param_max_bytes < 36 + (mDegree*8))
    ThrowRDE("OpcodeMapPolynomial: Not enough data to read parameters, only %d bytes left.", param_max_bytes);
  for (int i = 0; i <= mDegree; i++)
    mCoefficient[i] = getDouble(&parameters[36+8*i]);
  *bytes_used += 8*mDegree+8;
  mFlags = MultiThreaded | PureLookup;
}


RawImage& OpcodeMapPolynomial::createOutput( RawImage &in )
{
  if (in->getDataType() != TYPE_USHORT16)
    ThrowRDE("OpcodeMapPolynomial: Only 16 bit images supported");

  if (mFirstPlane > (int)in->getCpp())
    ThrowRDE("OpcodeMapPolynomial: Not that many planes in actual image");

  if (mFirstPlane+mPlanes > (int)in->getCpp())
    ThrowRDE("OpcodeMapPolynomial: Not that many planes in actual image");

  // Create lookup
  for (int i = 0; i < 65536; i++)
  {
    double in_val = (double)i/65536.0;
    double val = mCoefficient[0];
    for (int j = 1; j <= mDegree; j++)
      val += mCoefficient[j] * pow(in_val, (double)(j));
    mLookup[i] = clampbits((int)(val*65535.5), 16);
  }
  return in;
}

void OpcodeMapPolynomial::apply( RawImage &in, RawImage &out, int startY, int endY )
{
  int cpp = out->getCpp();
  for (int y = startY; y < endY; y += mRowPitch) {
    ushort16 *src = (ushort16*)out->getData(mAoi.getLeft(), y);
    // Add offset, so this is always first plane
    src+=mFirstPlane;
    for (int x = 0; x < mAoi.getWidth(); x += mColPitch) {
      for (int p = 0; p < mPlanes; p++)
      {
        src[x*cpp+p] = mLookup[src[x*cpp+p]];
      }
    }
  }
}

/***************** OpcodeDeltaPerRow   ****************/

OpcodeDeltaPerRow::OpcodeDeltaPerRow(const uchar8* parameters, int param_max_bytes, uint32 *bytes_used )
{
  if (param_max_bytes < 36)
    ThrowRDE("OpcodeDeltaPerRow: Not enough data to read parameters, only %d bytes left.", param_max_bytes);
  mAoi.setAbsolute(getLong(&parameters[4]), getLong(&parameters[0]), getLong(&parameters[12]), getLong(&parameters[8]));
  mFirstPlane = getLong(&parameters[16]);
  mPlanes = getLong(&parameters[20]);
  mRowPitch = getLong(&parameters[24]);
  mColPitch = getLong(&parameters[28]);
  if (mFirstPlane < 0)
    ThrowRDE("OpcodeDeltaPerRow: Negative first plane");
  if (mPlanes <= 0)
    ThrowRDE("OpcodeDeltaPerRow: Negative number of planes");
  if (mRowPitch <= 0 || mColPitch <= 0)
    ThrowRDE("OpcodeDeltaPerRow: Invalid Pitch");

  mCount = getLong(&parameters[32]);
  *bytes_used = 36;
  if (param_max_bytes < 36 + (mCount*4))
    ThrowRDE("OpcodeDeltaPerRow: Not enough data to read parameters, only %d bytes left.", param_max_bytes);
  if (mAoi.getHeight() != mCount)
    ThrowRDE("OpcodeDeltaPerRow: Element count (%d) does not match height of area (%d).", mCount, mAoi.getHeight());

  for (int i = 0; i <= mCount; i++)
    mDelta[i] = getFloat(&parameters[36+4*i]);
  *bytes_used += 4*mCount;
  mFlags = MultiThreaded;
}


RawImage& OpcodeDeltaPerRow::createOutput( RawImage &in )
{
  if (mFirstPlane > (int)in->getCpp())
    ThrowRDE("OpcodeDeltaPerRow: Not that many planes in actual image");

  if (mFirstPlane+mPlanes > (int)in->getCpp())
    ThrowRDE("OpcodeDeltaPerRow: Not that many planes in actual image");

  return in;
}

void OpcodeDeltaPerRow::apply( RawImage &in, RawImage &out, int startY, int endY )
{
  if (in->getDataType() == TYPE_USHORT16) {
    int cpp = out->getCpp();
    for (int y = startY; y < endY; y += mRowPitch) {
      ushort16 *src = (ushort16*)out->getData(mAoi.getLeft(), y);
      // Add offset, so this is always first plane
      src+=mFirstPlane;
      int delta = (int)(65535.0f * mDelta[y]);
      for (int x = 0; x < mAoi.getWidth(); x += mColPitch) {
        for (int p = 0; p < mPlanes; p++)
        {
          src[x*cpp+p] = clampbits(16,delta + src[x*cpp+p]);
        }
      }
    }
  } else {
    int cpp = out->getCpp();
    for (int y = startY; y < endY; y += mRowPitch) {
      float *src = (float*)out->getData(mAoi.getLeft(), y);
      // Add offset, so this is always first plane
      src+=mFirstPlane;
      float delta = mDelta[y];
      for (int x = 0; x < mAoi.getWidth(); x += mColPitch) {
        for (int p = 0; p < mPlanes; p++)
        {
          src[x*cpp+p] = delta + src[x*cpp+p];
        }
      }
    }
  }
}

/***************** OpcodeDeltaPerCol   ****************/

OpcodeDeltaPerCol::OpcodeDeltaPerCol(const uchar8* parameters, int param_max_bytes, uint32 *bytes_used )
{
  if (param_max_bytes < 36)
    ThrowRDE("OpcodeDeltaPerCol: Not enough data to read parameters, only %d bytes left.", param_max_bytes);
  mAoi.setAbsolute(getLong(&parameters[4]), getLong(&parameters[0]), getLong(&parameters[12]), getLong(&parameters[8]));
  mFirstPlane = getLong(&parameters[16]);
  mPlanes = getLong(&parameters[20]);
  mRowPitch = getLong(&parameters[24]);
  mColPitch = getLong(&parameters[28]);
  if (mFirstPlane < 0)
    ThrowRDE("OpcodeDeltaPerCol: Negative first plane");
  if (mPlanes <= 0)
    ThrowRDE("OpcodeDeltaPerCol: Negative number of planes");
  if (mRowPitch <= 0 || mColPitch <= 0)
    ThrowRDE("OpcodeDeltaPerCol: Invalid Pitch");

  mCount = getLong(&parameters[32]);
  *bytes_used = 36;
  if (param_max_bytes < 36 + (mCount*4))
    ThrowRDE("OpcodeDeltaPerCol: Not enough data to read parameters, only %d bytes left.", param_max_bytes);
  if (mAoi.getWidth() != mCount)
    ThrowRDE("OpcodeDeltaPerRow: Element count (%d) does not match width of area (%d).", mCount, mAoi.getWidth());

  for (int i = 0; i <= mCount; i++)
    mDelta[i] = getFloat(&parameters[36+4*i]);
  *bytes_used += 4*mCount;
  mFlags = MultiThreaded;
  mDeltaX = NULL;
}

OpcodeDeltaPerCol::~OpcodeDeltaPerCol( void )
{
  if (mDeltaX)
    delete[] mDeltaX;
  mDeltaX = NULL;
}


RawImage& OpcodeDeltaPerCol::createOutput( RawImage &in )
{
  if (mFirstPlane > (int)in->getCpp())
    ThrowRDE("OpcodeDeltaPerCol: Not that many planes in actual image");

  if (mFirstPlane+mPlanes > (int)in->getCpp())
    ThrowRDE("OpcodeDeltaPerCol: Not that many planes in actual image");

  if (in->getDataType() == TYPE_USHORT16) {
    if (mDeltaX)
      delete[] mDeltaX;
    int w = mAoi.getWidth();
    mDeltaX = new int[w];
    for (int i = 0; i < w; i++)
      mDeltaX[i] = (int)(65535.0f * mDelta[i] + 0.5f);
  }
  return in;
}

void OpcodeDeltaPerCol::apply( RawImage &in, RawImage &out, int startY, int endY )
{
  if (in->getDataType() == TYPE_USHORT16) {
    int cpp = out->getCpp();
    for (int y = startY; y < endY; y += mRowPitch) {
      ushort16 *src = (ushort16*)out->getData(mAoi.getLeft(), y);
      // Add offset, so this is always first plane
      src+=mFirstPlane;
      for (int x = 0; x < mAoi.getWidth(); x += mColPitch) {
        for (int p = 0; p < mPlanes; p++)
        {
          src[x*cpp+p] = clampbits(16, mDeltaX[x] + src[x*cpp+p]);
        }
      }
    }
  } else {
    int cpp = out->getCpp();
    for (int y = startY; y < endY; y += mRowPitch) {
      float *src = (float*)out->getData(mAoi.getLeft(), y);
      // Add offset, so this is always first plane
      src+=mFirstPlane;
      for (int x = 0; x < mAoi.getWidth(); x += mColPitch) {
        for (int p = 0; p < mPlanes; p++)
        {
          src[x*cpp+p] = mDelta[x] + src[x*cpp+p];
        }
      }
    }
  }
}

/***************** OpcodeScalePerRow   ****************/

OpcodeScalePerRow::OpcodeScalePerRow(const uchar8* parameters, int param_max_bytes, uint32 *bytes_used )
{
  if (param_max_bytes < 36)
    ThrowRDE("OpcodeScalePerRow: Not enough data to read parameters, only %d bytes left.", param_max_bytes);
  mAoi.setAbsolute(getLong(&parameters[4]), getLong(&parameters[0]), getLong(&parameters[12]), getLong(&parameters[8]));
  mFirstPlane = getLong(&parameters[16]);
  mPlanes = getLong(&parameters[20]);
  mRowPitch = getLong(&parameters[24]);
  mColPitch = getLong(&parameters[28]);
  if (mFirstPlane < 0)
    ThrowRDE("OpcodeScalePerRow: Negative first plane");
  if (mPlanes <= 0)
    ThrowRDE("OpcodeScalePerRow: Negative number of planes");
  if (mRowPitch <= 0 || mColPitch <= 0)
    ThrowRDE("OpcodeScalePerRow: Invalid Pitch");

  mCount = getLong(&parameters[32]);
  *bytes_used = 36;
  if (param_max_bytes < 36 + (mCount*4))
    ThrowRDE("OpcodeScalePerRow: Not enough data to read parameters, only %d bytes left.", param_max_bytes);
  if (mAoi.getHeight() != mCount)
    ThrowRDE("OpcodeScalePerRow: Element count (%d) does not match height of area (%d).", mCount, mAoi.getHeight());

  for (int i = 0; i <= mCount; i++)
    mDelta[i] = getFloat(&parameters[36+4*i]);
  *bytes_used += 4*mCount;
  mFlags = MultiThreaded;
}


RawImage& OpcodeScalePerRow::createOutput( RawImage &in )
{
  if (mFirstPlane > (int)in->getCpp())
    ThrowRDE("OpcodeScalePerRow: Not that many planes in actual image");

  if (mFirstPlane+mPlanes > (int)in->getCpp())
    ThrowRDE("OpcodeScalePerRow: Not that many planes in actual image");

  return in;
}

void OpcodeScalePerRow::apply( RawImage &in, RawImage &out, int startY, int endY )
{
  if (in->getDataType() == TYPE_USHORT16) {
    int cpp = out->getCpp();
    for (int y = startY; y < endY; y += mRowPitch) {
      ushort16 *src = (ushort16*)out->getData(mAoi.getLeft(), y);
      // Add offset, so this is always first plane
      src+=mFirstPlane;
      int delta = (int)(1024.0f * mDelta[y]);
      for (int x = 0; x < mAoi.getWidth(); x += mColPitch) {
        for (int p = 0; p < mPlanes; p++)
        {
          src[x*cpp+p] = clampbits(16,(delta * src[x*cpp+p] + 512) >> 10);
        }
      }
    }
  } else {
    int cpp = out->getCpp();
    for (int y = startY; y < endY; y += mRowPitch) {
      float *src = (float*)out->getData(mAoi.getLeft(), y);
      // Add offset, so this is always first plane
      src+=mFirstPlane;
      float delta = mDelta[y];
      for (int x = 0; x < mAoi.getWidth(); x += mColPitch) {
        for (int p = 0; p < mPlanes; p++)
        {
          src[x*cpp+p] = delta * src[x*cpp+p];
        }
      }
    }
  }
}

/***************** OpcodeScalePerCol   ****************/

OpcodeScalePerCol::OpcodeScalePerCol(const uchar8* parameters, int param_max_bytes, uint32 *bytes_used )
{
  if (param_max_bytes < 36)
    ThrowRDE("OpcodeScalePerCol: Not enough data to read parameters, only %d bytes left.", param_max_bytes);
  mAoi.setAbsolute(getLong(&parameters[4]), getLong(&parameters[0]), getLong(&parameters[12]), getLong(&parameters[8]));
  mFirstPlane = getLong(&parameters[16]);
  mPlanes = getLong(&parameters[20]);
  mRowPitch = getLong(&parameters[24]);
  mColPitch = getLong(&parameters[28]);
  if (mFirstPlane < 0)
    ThrowRDE("OpcodeScalePerCol: Negative first plane");
  if (mPlanes <= 0)
    ThrowRDE("OpcodeScalePerCol: Negative number of planes");
  if (mRowPitch <= 0 || mColPitch <= 0)
    ThrowRDE("OpcodeScalePerCol: Invalid Pitch");

  mCount = getLong(&parameters[32]);
  *bytes_used = 36;
  if (param_max_bytes < 36 + (mCount*4))
    ThrowRDE("OpcodeScalePerCol: Not enough data to read parameters, only %d bytes left.", param_max_bytes);
  if (mAoi.getWidth() != mCount)
    ThrowRDE("OpcodeScalePerCol: Element count (%d) does not match width of area (%d).", mCount, mAoi.getWidth());

  for (int i = 0; i <= mCount; i++)
    mDelta[i] = getFloat(&parameters[36+4*i]);
  *bytes_used += 4*mCount;
  mFlags = MultiThreaded;
  mDeltaX = NULL;
}

OpcodeScalePerCol::~OpcodeScalePerCol( void )
{
  if (mDeltaX)
    delete[] mDeltaX;
  mDeltaX = NULL;
}


RawImage& OpcodeScalePerCol::createOutput( RawImage &in )
{
  if (mFirstPlane > (int)in->getCpp())
    ThrowRDE("OpcodeScalePerCol: Not that many planes in actual image");

  if (mFirstPlane+mPlanes > (int)in->getCpp())
    ThrowRDE("OpcodeScalePerCol: Not that many planes in actual image");

  if (in->getDataType() == TYPE_USHORT16) {
    if (mDeltaX)
      delete[] mDeltaX;
    int w = mAoi.getWidth();
    mDeltaX = new int[w];
    for (int i = 0; i < w; i++)
      mDeltaX[i] = (int)(1024.0f * mDelta[i]);
  }
  return in;
}

void OpcodeScalePerCol::apply( RawImage &in, RawImage &out, int startY, int endY )
{
  if (in->getDataType() == TYPE_USHORT16) {
    int cpp = out->getCpp();
    for (int y = startY; y < endY; y += mRowPitch) {
      ushort16 *src = (ushort16*)out->getData(mAoi.getLeft(), y);
      // Add offset, so this is always first plane
      src+=mFirstPlane;
      for (int x = 0; x < mAoi.getWidth(); x += mColPitch) {
        for (int p = 0; p < mPlanes; p++)
        {
          src[x*cpp+p] = clampbits(16, (mDeltaX[x] * src[x*cpp+p] + 512) >> 10);
        }
      }
    }
  } else {
    int cpp = out->getCpp();
    for (int y = startY; y < endY; y += mRowPitch) {
      float *src = (float*)out->getData(mAoi.getLeft(), y);
      // Add offset, so this is always first plane
      src+=mFirstPlane;
      for (int x = 0; x < mAoi.getWidth(); x += mColPitch) {
        for (int p = 0; p < mPlanes; p++)
        {
          src[x*cpp+p] = mDelta[x] * src[x*cpp+p];
        }
      }
    }
  }
}


} // namespace RawSpeed 
