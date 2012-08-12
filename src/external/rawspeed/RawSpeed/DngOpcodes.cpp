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
    //uint32 flags = getULong(&data[bytes_used+8]);
    uint32 expected_size = getULong(&data[bytes_used+12]);
    bytes_used += 16;
    uint32 opcode_used = 0;
    switch (code)
    {
      case 6:
        mOpcodes.push_back(new OpcodeTrimBounds(&data[bytes_used], entry_size - bytes_used, &opcode_used));
      case 7:
        mOpcodes.push_back(new OpcodeMapTable(&data[bytes_used], entry_size - bytes_used, &opcode_used));
      case 8:
        mOpcodes.push_back(new OpcodeMapPolynomial(&data[bytes_used], entry_size - bytes_used, &opcode_used));
        break;
      default:
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

} // namespace RawSpeed 
