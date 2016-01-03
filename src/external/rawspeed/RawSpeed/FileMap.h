#ifndef FILE_MAP_H
#define FILE_MAP_H

#include "FileIOException.h"
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

#include "IOException.h"

namespace RawSpeed {

/*************************************************************************
 * This is the basic file map
 *
 * It allows access to a file.
 * Base implementation is for a complete file that is already in memory.
 * This can also be done as a MemMap 
 * 
 *****************************/
class FileMap
{
public:
  // Allocates the data array itself
  FileMap(uint32 _size);
  // Data already allocated, if possible allocate 16 extra bytes.
  FileMap(uchar8* _data, uint32 _size);
  // A subset reusing the same data and starting at offset
  FileMap(FileMap *f, uint32 offset);
  // A subset reusing the same data and starting at offset, with size bytes
  FileMap(FileMap *f, uint32 offset, uint32 size);
  ~FileMap(void);
  const uchar8* getData(uint32 offset, uint32 count);
  uchar8* getDataWrt(uint32 offset, uint32 count) {return (uchar8 *)getData(offset,count);}
  uint32 getSize() {return size;}
  bool isValid(uint32 offset) {return offset<=size;}
  FileMap* clone();
  /* For testing purposes */
  void corrupt(int errors);
  FileMap* cloneRandomSize();
private:
 uchar8* data;
 uint32 size;
 bool mOwnAlloc;
};

} // namespace RawSpeed

#endif
