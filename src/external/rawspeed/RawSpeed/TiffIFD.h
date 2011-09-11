#ifndef TIFF_IFD_H
#define TIFF_IFD_H

#include "FileMap.h"
#include "TiffEntry.h"
#include "TiffParserException.h"

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


class TiffIFD
{
public:
  TiffIFD();
  TiffIFD(FileMap* f, uint32 offset);
  virtual ~TiffIFD(void);
  vector<TiffIFD*> mSubIFD;
  map<TiffTag, TiffEntry*> mEntry;
  int getNextIFD() {return nextIFD;}
  vector<TiffIFD*> getIFDsWithTag(TiffTag tag);
  TiffEntry* getEntry(TiffTag tag);
  bool hasEntry(TiffTag tag);
  bool hasEntryRecursive(TiffTag tag);
  TiffEntry* getEntryRecursive(TiffTag tag);
  TiffIFD* parseDngPrivateData(TiffEntry *t);
  TiffIFD* parseMakerNote(FileMap *f, uint32 offset, Endianness parent_end);
  Endianness endian;
protected:
  int nextIFD;
};

inline bool isTiffSameAsHost(const ushort16* tifftag) {
  Endianness host = getHostEndianness();
  if (tifftag[0] == 0x4949)
    return little == host;
  if (tifftag[0] == 0x4d4d)
    return big == host;
  ThrowTPE("Unknown Tiff Byteorder :%x", tifftag[0]);
  return false;
}

inline Endianness getTiffEndianness(const ushort16* tifftag) {
  if (tifftag[0] == 0x4949)
    return little;
  if (tifftag[0] == 0x4d4d)
    return big;
  return unknown;
}

} // namespace RawSpeed

#endif
