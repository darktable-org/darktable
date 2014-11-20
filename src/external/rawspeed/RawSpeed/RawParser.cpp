#include "StdAfx.h"
#include "RawParser.h"
#include "TiffParserException.h"
#include "TiffParser.h"
#include "CiffParserException.h"
#include "CiffParser.h"
#include "X3fParser.h"
#include "ByteStreamSwap.h"
#include "TiffEntryBE.h"
#include "MrwDecoder.h"
#include "NakedDecoder.h"

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

namespace RawSpeed {


RawParser::RawParser(FileMap* inputData): mInput(inputData) {
}


RawParser::~RawParser(void) {
}

RawDecoder* RawParser::getDecoder() {
  const unsigned char* data = mInput->getData(0);
  // We need some data.
  // For now it is 104 bytes for RAF images.
  if (mInput->getSize() <=  104)
    ThrowRDE("File too small");

  // MRW images are easy to check for, let's try that first
  if (MrwDecoder::isMRW(mInput)) {
    try {
      return new MrwDecoder(mInput);
    } catch (RawDecoderException) {
    }
  }

  // FUJI has pointers to IFD's at fixed byte offsets
  // So if camera is FUJI, we cannot use ordinary TIFF parser
  if (0 == memcmp(&data[0], "FUJIFILM", 8)) {
    // First IFD typically JPEG and EXIF
    uint32 first_ifd = data[87] | (data[86]<<8) | (data[85]<<16) | (data[84]<<24);
    first_ifd += 12;
    if (mInput->getSize() <=  first_ifd)
      ThrowRDE("File too small (FUJI first IFD)");

    // RAW IFD on newer, pointer to raw data on older models, so we try parsing first
    // And adds it as data if parsin fails
    uint32 second_ifd = (uint32)data[103] | (data[102]<<8) | (data[101]<<16) | (data[100]<<24);
    if (mInput->getSize() <=  second_ifd)
      second_ifd = 0;

    // RAW information IFD on older
    uint32 third_ifd = data[95] | (data[94]<<8) | (data[93]<<16) | (data[92]<<24);
    if (mInput->getSize() <=  third_ifd)
      third_ifd = 0;

    // Open the IFDs and merge them
    try {
      FileMap *m1 = new FileMap(mInput->getDataWrt(first_ifd), mInput->getSize()-first_ifd);
      FileMap *m2 = NULL;
      TiffParser p(m1);
      p.parseData();
      if (second_ifd) {
        m2 = new FileMap(mInput->getDataWrt(second_ifd), mInput->getSize()-second_ifd);
        try {
          TiffParser p2(m2);
          p2.parseData();
          p.MergeIFD(&p2);
        } catch (TiffParserException e) {
          delete m2;
          m2 = NULL;
       }
      }

      TiffIFD *new_ifd = new TiffIFD(mInput);
      p.RootIFD()->mSubIFD.push_back(new_ifd);

      if (third_ifd) {
        try {
          ParseFuji(third_ifd, new_ifd);
        } catch (TiffParserException e) {
        }
      }
      // Make sure these aren't leaked.
      RawDecoder *d = p.getDecoder();
      d->ownedObjects.push_back(m1);
      if (m2)
        d->ownedObjects.push_back(m2);

      if (!m2 && second_ifd) {
        TiffEntry *entry = new TiffEntry(FUJI_STRIPOFFSETS, TIFF_LONG, 1);
        entry->setData(&second_ifd, 4);
        new_ifd->mEntry[entry->tag] = entry;
        entry = new TiffEntry(FUJI_STRIPBYTECOUNTS, TIFF_LONG, 1);
        uint32 max_size = mInput->getSize()-second_ifd;
        entry->setData(&max_size, 4);
        new_ifd->mEntry[entry->tag] = entry;
      }
      return d;
    } catch (TiffParserException) {}
    ThrowRDE("No decoder found. Sorry.");
  }

  // Ordinary TIFF images
  try {
    TiffParser p(mInput);
    p.parseData();
    return p.getDecoder();
  } catch (TiffParserException) {}

  try {
    X3fParser parser(mInput);
    return parser.getDecoder();
  } catch (RawDecoderException) {
  }

  // CIFF images
  try {
    CiffParser p(mInput);
    p.parseData();
    return p.getDecoder();
  } catch (CiffParserException &e) {
  }

  // File could not be decoded, so do one last ditch effort based on file size
  if (NakedDecoder::couldBeNakedRaw(mInput)) {
    try {
      return new NakedDecoder(mInput);
    } catch (RawDecoderException) {
    }
  }

  ThrowRDE("No decoder found. Sorry.");
  return NULL;
}

/* Parse FUJI information */
/* It is a simpler form of Tiff IFD, so we add them as TiffEntries */
void RawParser::ParseFuji(uint32 offset, TiffIFD *target_ifd)
{
  try {
    ByteStreamSwap bytes(mInput->getData(offset), mInput->getSize()-offset);
    uint32 entries = bytes.getUInt();

    if (entries > 255)
      ThrowTPE("ParseFuji: Too many entries");

    for (uint32 i = 0; i < entries; i++) {
      ushort16 tag = bytes.getShort();
      ushort16 length = bytes.getShort();
      TiffEntry *t;

      // Set types of known tags
      switch (tag) {
        case 0x100:
        case 0x121:
        case 0x2ff0:
          t = new TiffEntryBE((TiffTag)tag, TIFF_SHORT, length/2, bytes.getData());
          break;

        case 0xc000:
          // This entry seem to have swapped endianness:
          t = new TiffEntry((TiffTag)tag, TIFF_LONG, length/4, bytes.getData());
          break;

        default:
          t = new TiffEntry((TiffTag)tag, TIFF_UNDEFINED, length, bytes.getData());
      }

      target_ifd->mEntry[t->tag] = t;
      bytes.skipBytes(length);
    }
  } catch (IOException e) {
    ThrowTPE("ParseFuji: IO error occurred during parsing. Skipping the rest");
  }

}

} // namespace RawSpeed
