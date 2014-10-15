#ifndef CIFF_IFD_H
#define CIFF_IFD_H

#include "FileMap.h"
#include "CiffEntry.h"
#include "CiffParserException.h"

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


class CiffIFD
{
public:
  CiffIFD(FileMap* f, uint32 start, uint32 end);
  virtual ~CiffIFD(void);
  vector<CiffIFD*> mSubIFD;
  map<CiffTag, CiffEntry*> mEntry;
  vector<CiffIFD*> getIFDsWithTag(CiffTag tag);
  CiffEntry* getEntry(CiffTag tag);
  bool hasEntry(CiffTag tag);
  bool hasEntryRecursive(CiffTag tag);
  CiffEntry* getEntryRecursive(CiffTag tag);
  CiffEntry* getEntryRecursiveWhere(CiffTag tag, uint32 isValue);
  CiffEntry* getEntryRecursiveWhere(CiffTag tag, string isValue);
  vector<CiffIFD*> getIFDsWithTagWhere(CiffTag tag, string isValue);
  vector<CiffIFD*> getIFDsWithTagWhere(CiffTag tag, uint32 isValue);
  FileMap* getFileMap() {return mFile;};
protected:
  FileMap *mFile;
};

} // namespace RawSpeed

#endif
