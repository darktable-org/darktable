#ifndef TIFF_ENTRY_H
#define TIFF_ENTRY_H

#include "TiffParserException.h"
#include "FileMap.h"

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

const uint32 datasizes[] = {0,1,1,2,4,8,1,1,2,4, 8, 4, 8, 4};
                      // 0-1-2-3-4-5-6-7-8-9-10-11-12-13
const uint32 datashifts[] = {0,0,0,1,2,3,0,0,1,2, 3, 2, 3, 2};

#ifdef CHECKSIZE
#undef CHECKSIZE
#endif

#define CHECKSIZE(A) if (A > f->getSize() || A < 1) ThrowTPE("Error reading TIFF Entry structure size. File Corrupt")

// 0-1-2-3-4-5-6-7-8-9-10-11-12-13
/*
 * Tag data type information.
 *
 * Note: RATIONALs are the ratio of two 32-bit integer values.
 */
typedef	enum {
	TIFF_NOTYPE	= 0,	/* placeholder */
	TIFF_BYTE	= 1,	/* 8-bit unsigned integer */
	TIFF_ASCII	= 2,	/* 8-bit bytes w/ last byte null */
	TIFF_SHORT	= 3,	/* 16-bit unsigned integer */
	TIFF_LONG	= 4,	/* 32-bit unsigned integer */
	TIFF_RATIONAL	= 5,	/* 64-bit unsigned fraction */
	TIFF_SBYTE	= 6,	/* !8-bit signed integer */
	TIFF_UNDEFINED	= 7,	/* !8-bit untyped data */
	TIFF_SSHORT	= 8,	/* !16-bit signed integer */
	TIFF_SLONG	= 9,	/* !32-bit signed integer */
	TIFF_SRATIONAL	= 10,	/* !64-bit signed fraction */
	TIFF_FLOAT	= 11,	/* !32-bit IEEE floating point */
	TIFF_DOUBLE	= 12	/* !64-bit IEEE floating point */
} TiffDataType;


class TiffEntry
{
public:
  TiffEntry();
  TiffEntry(TiffTag tag, TiffDataType type, uint32 count, const uchar8* data = NULL);
  TiffEntry(FileMap* f, uint32 offset, uint32 up_offset);
  virtual ~TiffEntry(void);
  virtual uint32 getInt();
  float getFloat();
  virtual ushort16 getShort();
  virtual const uint32* getIntArray();
  virtual const ushort16* getShortArray();
  virtual const short16* getSignedShortArray();
  string getString();
  uchar8 getByte();
  const uchar8* getData() {return data;};
  uchar8* getDataWrt();;
  virtual void setData(const void *data, uint32 byte_count );
  int getElementSize();
  int getElementShift();
// variables:
  TiffTag tag;
  TiffDataType type;
  uint32 count;
  uint32 getDataOffset() const { return data_offset; }
  bool isFloat();
  bool isInt();
  bool isString();
  void offsetFromParent() {data_offset += parent_offset; parent_offset = 0; fetchData(); }
  uint32 parent_offset;
protected:
  void fetchData();
  string getValueAsString();
  uchar8* own_data;
  const uchar8* data;
  uint32 data_offset;
  FileMap *file;
#ifdef _DEBUG
  int debug_intVal;
  float debug_floatVal;
#endif
};

} // namespace RawSpeed

#endif
