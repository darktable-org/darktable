#include "StdAfx.h"
#include "TiffIFD.h"
#include "TiffParser.h"
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

#ifdef CHECKSIZE
#undef CHECKSIZE
#endif

#define CHECKSIZE(A) if (A >= size) ThrowTPE("Error reading TIFF structure (invalid size). File Corrupt")

TiffIFD::TiffIFD() {
  nextIFD = 0;
  endian = little;
}

TiffIFD::TiffIFD(FileMap* f, uint32 offset) {
  uint32 size = f->getSize();
  uint32 entries;
  endian = little;
  CHECKSIZE(offset);

  entries = *(unsigned short*)f->getData(offset);    // Directory entries in this IFD

  CHECKSIZE(offset + 2 + entries*4);
  for (uint32 i = 0; i < entries; i++) {
    TiffEntry *t = new TiffEntry(f, offset + 2 + i*12);

    if (t->tag == SUBIFDS || t->tag == EXIFIFDPOINTER || t->tag == DNGPRIVATEDATA || t->tag == MAKERNOTE || t->tag == 0x2e) {   // subIFD tag
      if (t->tag == DNGPRIVATEDATA) {
        try {
          TiffIFD *maker_ifd = parseDngPrivateData(t);
          mSubIFD.push_back(maker_ifd);
          delete(t);
        } catch (TiffParserException e) {
          // Unparsable private data are added as entries
          mEntry[t->tag] = t;
        }
      } else if (t->tag == MAKERNOTE || t->tag == 0x2e) {
        try {
          mSubIFD.push_back(parseMakerNote(f, t->getDataOffset(), endian));
          delete(t);
        } catch (TiffParserException e) {
          // Unparsable makernotes are added as entries
          mEntry[t->tag] = t;
        }
      } else {
        const unsigned int* sub_offsets = t->getIntArray();
        try {
          for (uint32 j = 0; j < t->count; j++) {
            mSubIFD.push_back(new TiffIFD(f, sub_offsets[j]));
          }
          delete(t);
        } catch (TiffParserException e) {
          // Unparsable subifds are added as entries
          mEntry[t->tag] = t;
        }
      }
    } else {  // Store as entry
      mEntry[t->tag] = t;
    }
  }
  nextIFD = *(int*)f->getData(offset + 2 + entries * 12);
}

TiffIFD* TiffIFD::parseDngPrivateData(TiffEntry *t) {
  /*
  1. Six bytes containing the zero-terminated string "Adobe". (The DNG specification calls for the DNGPrivateData tag to start with an ASCII string identifying the creator/format).
  2. 4 bytes: an ASCII string ("MakN" for a Makernote),  indicating what sort of data is being stored here. Note that this is not zero-terminated.
  3. A four-byte count (number of data bytes following); this is the length of the original MakerNote data. (This is always in "most significant byte first" format).
  4. 2 bytes: the byte-order indicator from the original file (the usual 'MM'/4D4D or 'II'/4949).
  5. 4 bytes: the original file offset for the MakerNote tag data (stored according to the byte order given above).
  6. The contents of the MakerNote tag. This is a simple byte-for-byte copy, with no modification.
  */
  uint32 size = t->count;
  const uchar8 *data = t->getData();
  string id((const char*)data);
  if (id.compare("Adobe"))
    ThrowTPE("Not Adobe Private data");

  data+=6;
  if (!(data[0] == 'M' && data[1] == 'a' && data[2] == 'k' &&data[3] == 'N' ))
    ThrowTPE("Not Makernote");

  data+=4;
  uint32 count;
  if (big == getHostEndianness())
    count = *(uint32*)data;
  else
    count = (unsigned int)data[0] << 24 | (unsigned int)data[1] << 16 | (unsigned int)data[2] << 8 | (unsigned int)data[3];

  data+=4;
  CHECKSIZE(count);
  Endianness makernote_endian = unknown;
  if (data[0] == 0x49 && data[1] == 0x49)
    makernote_endian = little;
  else if (data[0] == 0x4D && data[1] == 0x4D)
    makernote_endian = big;
  else
    ThrowTPE("Cannot determine endianess of DNG makernote");

  data+=2;
  uint32 org_offset;

  if (big == getHostEndianness())
    org_offset = *(uint32*)data;
  else
    org_offset = (unsigned int)data[0] << 24 | (unsigned int)data[1] << 16 | (unsigned int)data[2] << 8 | (unsigned int)data[3];

  data+=4;
  /* We don't parse original makernotes that are placed after 300MB mark in the original file */
  if (org_offset+count > 300*1024*1024)
    ThrowTPE("Adobe Private data: original offset of makernote is past 300MB offset");

  /* Create fake tiff with original offsets */
  uchar8* maker_data = new uchar8[org_offset+count];
  memcpy(&maker_data[org_offset],data, count);
  FileMap *maker_map = new FileMap(maker_data, org_offset+count);

  TiffIFD *maker_ifd;
  try {
    maker_ifd = parseMakerNote(maker_map, org_offset, makernote_endian);
  } catch (TiffParserException e) {
    delete[] maker_data;
    delete maker_map;
    throw e;
  }
  delete[] maker_data;
  delete maker_map;
  return maker_ifd;
}

/* This will attempt to parse makernotes and return it as an IFD */
TiffIFD* TiffIFD::parseMakerNote(FileMap *f, uint32 offset, Endianness parent_end)
{
  uint32 size = f->getSize();
  CHECKSIZE(offset + 20);
  TiffIFD *maker_ifd = NULL;
  const uchar8* data = f->getData(offset);

  // Pentax makernote starts with AOC\0 - If it's there, skip it
  if (data[0] == 0x41 && data[1] == 0x4f && data[2] == 0x43 && data[3] == 0)
  {
    data +=4;
    offset +=4;
  }

  // Panasonic has the word Exif at byte 6, a complete Tiff header starts at byte 12
  // This TIFF is 0 offset based
  if (data[6] == 0x45 && data[7] == 0x78 && data[8] == 0x69 && data[9] == 0x66)
  {
    parent_end = getTiffEndianness((const ushort16*)&data[12]);
    if (parent_end == unknown)
      ThrowTPE("Cannot determine Panasonic makernote endianness");
    data +=20;
    offset +=20;
  }

  // Some have MM or II to indicate endianness - read that
  if (data[0] == 0x49 && data[1] == 0x49) {
    offset +=2;
    parent_end = little;
  } else if (data[0] == 0x4D && data[1] == 0x4D) {
    parent_end = big;
    offset +=2;
  }

  // Attempt to parse the rest as an IFD
  if (parent_end == getHostEndianness())
    maker_ifd = new TiffIFD(f, offset);
  else
    maker_ifd = new TiffIFDBE(f, offset);

  // If the structure cannot be read, a TiffParserException will be thrown.
  return maker_ifd;
}

TiffIFD::~TiffIFD(void) {
  for (map<TiffTag, TiffEntry*>::iterator i = mEntry.begin(); i != mEntry.end(); ++i) {
    delete((*i).second);
  }
  mEntry.clear();
  for (vector<TiffIFD*>::iterator i = mSubIFD.begin(); i != mSubIFD.end(); ++i) {
    delete(*i);
  }
  mSubIFD.clear();
}

bool TiffIFD::hasEntryRecursive(TiffTag tag) {
  if (mEntry.find(tag) != mEntry.end())
    return TRUE;
  for (vector<TiffIFD*>::iterator i = mSubIFD.begin(); i != mSubIFD.end(); ++i) {
    if ((*i)->hasEntryRecursive(tag))
      return TRUE;
  }
  return false;
}

vector<TiffIFD*> TiffIFD::getIFDsWithTag(TiffTag tag) {
  vector<TiffIFD*> matchingIFDs;
  if (mEntry.find(tag) != mEntry.end()) {
    matchingIFDs.push_back(this);
  }
  for (vector<TiffIFD*>::iterator i = mSubIFD.begin(); i != mSubIFD.end(); ++i) {
    vector<TiffIFD*> t = (*i)->getIFDsWithTag(tag);
    for (uint32 j = 0; j < t.size(); j++) {
      matchingIFDs.push_back(t[j]);
    }
  }

  return matchingIFDs;
}

TiffEntry* TiffIFD::getEntryRecursive(TiffTag tag) {
  if (mEntry.find(tag) != mEntry.end()) {
    return mEntry[tag];
  }
  for (vector<TiffIFD*>::iterator i = mSubIFD.begin(); i != mSubIFD.end(); ++i) {
    TiffEntry* entry = (*i)->getEntryRecursive(tag);
    if (entry)
      return entry;
  }
  return NULL;
}

TiffEntry* TiffIFD::getEntry(TiffTag tag) {
  if (mEntry.find(tag) != mEntry.end()) {
    return mEntry[tag];
  }
  ThrowTPE("TiffIFD: TIFF Parser entry 0x%x not found.", tag);
  return 0;
}


bool TiffIFD::hasEntry(TiffTag tag) {
  return mEntry.find(tag) != mEntry.end();
}

} // namespace RawSpeed
