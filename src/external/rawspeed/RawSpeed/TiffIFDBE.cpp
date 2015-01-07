#include "StdAfx.h"
#include "TiffIFDBE.h"
#include "TiffEntryBE.h"
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

TiffIFDBE::TiffIFDBE() {
  endian = big;
}

TiffIFDBE::TiffIFDBE(FileMap* f, uint32 offset) {
  mFile = f;
  endian = big;
  int entries;
  CHECKSIZE(offset);

  const unsigned char* data = f->getData(offset);
  entries = (unsigned short)data[0] << 8 | (unsigned short)data[1];    // Directory entries in this IFD

  CHECKSIZE(offset + 2 + entries*4);
  for (int i = 0; i < entries; i++) {
    TiffEntryBE *t = new TiffEntryBE(f, offset + 2 + i*12, offset);

    if (t->tag == SUBIFDS || t->tag == EXIFIFDPOINTER || t->tag == DNGPRIVATEDATA || t->tag == MAKERNOTE) {   // subIFD tag
      if (t->tag == DNGPRIVATEDATA) {
        try {
          TiffIFD *maker_ifd = parseDngPrivateData(t);
          mSubIFD.push_back(maker_ifd);
          delete(t);
        } catch (TiffParserException) {
          // Unparsable private data are added as entries
          mEntry[t->tag] = t;
        }
      } else if (t->tag == MAKERNOTE || t->tag == 0x2e) {
        try {
          mSubIFD.push_back(parseMakerNote(f, t->getDataOffset(), endian));
          delete(t);
        } catch (TiffParserException) {
          // Unparsable makernotes are added as entries
          mEntry[t->tag] = t;
        }
      } else {
        const unsigned int* sub_offsets = t->getIntArray();
        try {
          for (uint32 j = 0; j < t->count; j++) {
            mSubIFD.push_back(new TiffIFDBE(f, sub_offsets[j]));
          }
          delete(t);
        } catch (TiffParserException) {
          // Unparsable subifds are added as entries
          mEntry[t->tag] = t;
        }
      }
    } else {  // Store as entry
      mEntry[t->tag] = t;
    }
  }
  data = f->getDataWrt(offset + 2 + entries * 12);
  nextIFD = (unsigned int)data[0] << 24 | (unsigned int)data[1] << 16 | (unsigned int)data[2] << 8 | (unsigned int)data[3];
}


TiffIFDBE::~TiffIFDBE(void) {
}

} // namespace RawSpeed
