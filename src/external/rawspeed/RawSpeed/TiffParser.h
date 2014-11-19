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

#ifndef TIFF_PARSER_H
#define TIFF_PARSER_H

#include "FileMap.h"
#include "TiffIFD.h"
#include "TiffIFDBE.h"
#include "TiffParserException.h"
#include "RawDecoder.h"


namespace RawSpeed {

class TiffParser 
{
public:
  TiffParser(FileMap* input);
  virtual ~TiffParser(void);

  virtual void parseData();
  virtual RawDecoder* getDecoder();
  Endianness tiff_endian;
  /* Returns the Root IFD - this object still retains ownership */
  TiffIFD* RootIFD() const { return mRootIFD; }
  /* Merges root of other TIFF into this - clears the root of the other */
  void MergeIFD(TiffParser* other_tiff);
  RawSpeed::Endianness getHostEndian() const { return host_endian; }
protected:
  FileMap *mInput;
  TiffIFD* mRootIFD;
  Endianness host_endian;
};

} // namespace RawSpeed

#endif
