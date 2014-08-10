#include "StdAfx.h"
#include "CiffIFD.h"
#include "CiffParser.h"
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

#ifdef CHECKSIZE
#undef CHECKSIZE
#endif

#define CHECKSIZE(A) if (A > size) ThrowCPE("Error reading CIFF structure (invalid size). File Corrupt")

CiffIFD::CiffIFD(FileMap* f, uint32 start, uint32 end) {
  mFile = f;
  uint32 size = f->getSize();
  CHECKSIZE(start);
  CHECKSIZE(end);

  uint32 valuedata_size = *(uint32 *) f->getData(end-4);
  ushort16 dircount = *(ushort16 *) f->getData(start+valuedata_size);

//  fprintf(stderr, "Found %d entries between %d and %d after %d data bytes\n", 
//                  dircount, start, end, valuedata_size);

  for (uint32 i = 0; i < dircount; i++) {
    CiffEntry *t = new CiffEntry(f, start, start+valuedata_size+2+i*10);

    if (t->type == CIFF_SUB1 || t->type == CIFF_SUB2) {
      try {
        mSubIFD.push_back(new CiffIFD(f, t->data_offset, t->data_offset+t->count));
        delete(t);
      } catch (CiffParserException &e) {
        // Unparsable subifds are added as entries
        mEntry[t->tag] = t;
      }
    } else {
      mEntry[t->tag] = t;
    }
  }
}


CiffIFD::~CiffIFD(void) {
  for (map<CiffTag, CiffEntry*>::iterator i = mEntry.begin(); i != mEntry.end(); ++i) {
    delete((*i).second);
  }
  mEntry.clear();
  for (vector<CiffIFD*>::iterator i = mSubIFD.begin(); i != mSubIFD.end(); ++i) {
    delete(*i);
  }
  mSubIFD.clear();
}

bool CiffIFD::hasEntryRecursive(CiffTag tag) {
  if (mEntry.find(tag) != mEntry.end())
    return TRUE;
  for (vector<CiffIFD*>::iterator i = mSubIFD.begin(); i != mSubIFD.end(); ++i) {
    if ((*i)->hasEntryRecursive(tag))
      return TRUE;
  }
  return false;
}

vector<CiffIFD*> CiffIFD::getIFDsWithTag(CiffTag tag) {
  vector<CiffIFD*> matchingIFDs;
  if (mEntry.find(tag) != mEntry.end()) {
    matchingIFDs.push_back(this);
  }
  for (vector<CiffIFD*>::iterator i = mSubIFD.begin(); i != mSubIFD.end(); ++i) {
    vector<CiffIFD*> t = (*i)->getIFDsWithTag(tag);
    for (uint32 j = 0; j < t.size(); j++) {
      matchingIFDs.push_back(t[j]);
    }
  }
  return matchingIFDs;
}

vector<CiffIFD*> CiffIFD::getIFDsWithTagWhere(CiffTag tag, uint32 isValue) {
  vector<CiffIFD*> matchingIFDs;
  if (mEntry.find(tag) != mEntry.end()) {
    CiffEntry* entry = mEntry[tag];
    if (entry->isInt() && entry->getInt() == isValue)
      matchingIFDs.push_back(this);
  }
  for (vector<CiffIFD*>::iterator i = mSubIFD.begin(); i != mSubIFD.end(); ++i) {
    vector<CiffIFD*> t = (*i)->getIFDsWithTag(tag);
    for (uint32 j = 0; j < t.size(); j++) {
      matchingIFDs.push_back(t[j]);
    }
  }
  return matchingIFDs;
}

vector<CiffIFD*> CiffIFD::getIFDsWithTagWhere(CiffTag tag, string isValue) {
  vector<CiffIFD*> matchingIFDs;
  if (mEntry.find(tag) != mEntry.end()) {
    CiffEntry* entry = mEntry[tag];
    if (entry->isString() && 0 == entry->getString().compare(isValue))
      matchingIFDs.push_back(this);
  }
  for (vector<CiffIFD*>::iterator i = mSubIFD.begin(); i != mSubIFD.end(); ++i) {
    vector<CiffIFD*> t = (*i)->getIFDsWithTag(tag);
    for (uint32 j = 0; j < t.size(); j++) {
      matchingIFDs.push_back(t[j]);
    }
  }
  return matchingIFDs;
}

CiffEntry* CiffIFD::getEntryRecursive(CiffTag tag) {
  if (mEntry.find(tag) != mEntry.end()) {
    return mEntry[tag];
  }
  for (vector<CiffIFD*>::iterator i = mSubIFD.begin(); i != mSubIFD.end(); ++i) {
    CiffEntry* entry = (*i)->getEntryRecursive(tag);
    if (entry)
      return entry;
  }
  return NULL;
}

CiffEntry* CiffIFD::getEntryRecursiveWhere(CiffTag tag, uint32 isValue) {
  if (mEntry.find(tag) != mEntry.end()) {
    CiffEntry* entry = mEntry[tag];
    if (entry->isInt() && entry->getInt() == isValue)
      return entry;
  }
  for (vector<CiffIFD*>::iterator i = mSubIFD.begin(); i != mSubIFD.end(); ++i) {
    CiffEntry* entry = (*i)->getEntryRecursive(tag);
    if (entry)
      return entry;
  }
  return NULL;
}

CiffEntry* CiffIFD::getEntryRecursiveWhere(CiffTag tag, string isValue) {
  if (mEntry.find(tag) != mEntry.end()) {
    CiffEntry* entry = mEntry[tag];
    if (entry->isString() && 0 == entry->getString().compare(isValue))
      return entry;
  }
  for (vector<CiffIFD*>::iterator i = mSubIFD.begin(); i != mSubIFD.end(); ++i) {
    CiffEntry* entry = (*i)->getEntryRecursive(tag);
    if (entry)
      return entry;
  }
  return NULL;
}

CiffEntry* CiffIFD::getEntry(CiffTag tag) {
  if (mEntry.find(tag) != mEntry.end()) {
    return mEntry[tag];
  }
  ThrowCPE("CiffIFD: CIFF Parser entry 0x%x not found.", tag);
  return 0;
}


bool CiffIFD::hasEntry(CiffTag tag) {
  return mEntry.find(tag) != mEntry.end();
}

} // namespace RawSpeed
