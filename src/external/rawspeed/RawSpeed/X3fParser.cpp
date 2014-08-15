#include "StdAfx.h"
#include "X3fParser.h"
#include "X3fDecoder.h"
#include "ByteStreamSwap.h"
/*
RawSpeed - RAW file decoder.

Copyright (C) 2009-2013 Klaus Post

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

X3fParser::X3fParser(FileMap* file) {
  decoder = NULL;
  bytes = NULL;
  mFile = file;
  uint32 size = file->getSize();
  if (size<104+128)
    ThrowRDE("X3F file too small");

  if (getHostEndianness() == little)
    bytes = new ByteStream(file->getData(0), size);
  else
    bytes = new ByteStreamSwap(file->getData(0), size);
  try {
    try {
      // Read signature
      if (bytes->getUInt() != 0x62564f46)
        ThrowRDE("X3F Decoder: Not an X3f file (Signature)");

      uint32 version = bytes->getUInt();
      if (version < 0x00020000)
        ThrowRDE("X3F Decoder: File version too old");

      // Skip identifier + mark bits
      bytes->skipBytes(16+4);

      bytes->setAbsoluteOffset(0);
      decoder = new X3fDecoder(file);
      readDirectory();
    } catch (IOException e) {
      ThrowRDE("X3F Decoder: IO Error while reading header: %s", e.what());
    }
  } catch (RawDecoderException e) {
    freeObjects();
    throw e;
  }
}

void X3fParser::freeObjects() {
  if (bytes)
    delete bytes;
  if (decoder)
    delete decoder;
  decoder = NULL;
  bytes = NULL;
}

X3fParser::~X3fParser(void)
{
  freeObjects();
}


static string getIdAsString(ByteStream *bytes) {
  uchar8 id[5];
  for (int i = 0; i < 4; i++)
    id[i] = bytes->getByte();
  id[4] = 0;
  return string((const char*)id);
}


void X3fParser::readDirectory()
{
  bytes->setAbsoluteOffset(mFile->getSize()-4);
  uint32 dir_off = bytes->getUInt();
  bytes->setAbsoluteOffset(dir_off);

  // Check signature
  if (0 != getIdAsString(bytes).compare("SECd"))
    ThrowRDE("X3F Decoder: Unable to locate directory");

  uint32 version = bytes->getUInt();
  if (version < 0x00020000)
    ThrowRDE("X3F Decoder: File version too old (directory)");

  uint32 n_entries = bytes->getUInt();
  for (uint32 i = 0; i < n_entries; i++) {
    X3fDirectory dir(bytes);
    decoder->mDirectory.push_back(dir);
    bytes->pushOffset();
    if (0 == dir.id.compare("IMA2") || 0 == dir.id.compare("IMAG")){
      decoder->mImages.push_back(X3fImage(bytes, dir.offset, dir.length));
    }
    if (0 == dir.id.compare("PROP")){
      decoder->mProperties.addProperties(bytes, dir.offset, dir.length);
    }
    bytes->popOffset();
  }
}

RawDecoder* X3fParser::getDecoder()
{
  if (NULL == decoder)
    ThrowRDE("X3fParser: No decoder found!");
  RawDecoder *ret = decoder;
  decoder = NULL;
  return ret;
}

X3fDirectory::X3fDirectory( ByteStream *bytes )
{
    offset = bytes->getUInt();
    length = bytes->getUInt();
    id = getIdAsString(bytes);
}


X3fImage::X3fImage( ByteStream *bytes, uint32 offset, uint32 length )
{
  bytes->setAbsoluteOffset(offset);
  string id = getIdAsString(bytes);
  if (id.compare("SECi"))
    ThrowRDE("X3fImage:Unknown Image signature");

  uint32 version = bytes->getUInt();
  if (version < 0x00020000)
    ThrowRDE("X3F Decoder: File version too old (image)");

  type = bytes->getUInt();
  format = bytes->getUInt();
  width = bytes->getUInt();
  height = bytes->getUInt();
  pitchB = bytes->getUInt();
  dataOffset = bytes->getOffset();
  dataSize = length - (dataOffset-offset);
  if (pitchB == dataSize)
    pitchB = 0;
}


/*
* ConvertUTF16toUTF8 function only Copyright:
*
* Copyright 2001-2004 Unicode, Inc.
* 
* Disclaimer
* 
* This source code is provided as is by Unicode, Inc. No claims are
* made as to fitness for any particular purpose. No warranties of any
* kind are expressed or implied. The recipient agrees to determine
* applicability of information provided. If this file has been
* purchased on magnetic or optical media from Unicode, Inc., the
* sole remedy for any claim will be exchange of defective media
* within 90 days of receipt.
* 
* Limitations on Rights to Redistribute This Code
* 
* Unicode, Inc. hereby grants the right to freely use the information
* supplied in this file in the creation of products supporting the
* Unicode Standard, and to make copies of this file in any form
* for internal or external distribution as long as this notice
* remains attached.
*/

typedef unsigned int    UTF32;  /* at least 32 bits */
typedef unsigned short  UTF16;  /* at least 16 bits */
typedef unsigned char   UTF8;   /* typically 8 bits */
typedef unsigned char   Boolean; /* 0 or 1 */


/* Some fundamental constants */
#define UNI_REPLACEMENT_CHAR (UTF32)0x0000FFFD
#define UNI_MAX_BMP (UTF32)0x0000FFFF
#define UNI_MAX_UTF16 (UTF32)0x0010FFFF
#define UNI_MAX_UTF32 (UTF32)0x7FFFFFFF
#define UNI_MAX_LEGAL_UTF32 (UTF32)0x0010FFFF

#define UNI_MAX_UTF8_BYTES_PER_CODE_POINT 4

#define UNI_UTF16_BYTE_ORDER_MARK_NATIVE  0xFEFF
#define UNI_UTF16_BYTE_ORDER_MARK_SWAPPED 0xFFFE

#define UNI_SUR_HIGH_START  (UTF32)0xD800
#define UNI_SUR_HIGH_END    (UTF32)0xDBFF
#define UNI_SUR_LOW_START   (UTF32)0xDC00
#define UNI_SUR_LOW_END     (UTF32)0xDFFF

static const int halfShift  = 10; /* used for shifting by 10 bits */
static const UTF32 halfBase = 0x0010000UL;
static const UTF8 firstByteMark[7] = { 0x00, 0x00, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC };

static bool ConvertUTF16toUTF8 (const UTF16** sourceStart, const UTF16* sourceEnd,  UTF8** targetStart, UTF8* targetEnd) 
{
  bool success = TRUE;
  const UTF16* source = *sourceStart;
  UTF8* target = *targetStart;
  while (source < sourceEnd) {
    UTF32 ch;
    unsigned short bytesToWrite = 0;
    const UTF32 byteMask = 0xBF;
    const UTF32 byteMark = 0x80; 
    const UTF16* oldSource = source; /* In case we have to back up because of target overflow. */
    ch = *source++;
    /* If we have a surrogate pair, convert to UTF32 first. */
    if (ch >= UNI_SUR_HIGH_START && ch <= UNI_SUR_HIGH_END) {
      /* If the 16 bits following the high surrogate are in the source buffer... */
      if (source < sourceEnd) {
        UTF32 ch2 = *source;
        /* If it's a low surrogate, convert to UTF32. */
        if (ch2 >= UNI_SUR_LOW_START && ch2 <= UNI_SUR_LOW_END) {
          ch = ((ch - UNI_SUR_HIGH_START) << halfShift)
            + (ch2 - UNI_SUR_LOW_START) + halfBase;
          ++source;
#if 0
        } else if (flags == strictConversion) { /* it's an unpaired high surrogate */
          --source; /* return to the illegal value itself */
          success = FALSE;
          break;
#endif
        }
      } else { /* We don't have the 16 bits following the high surrogate. */
        --source; /* return to the high surrogate */
        success = FALSE;
        break;
      }
    }
    /* Figure out how many bytes the result will require */
    if (ch < (UTF32)0x80) {      bytesToWrite = 1;
    } else if (ch < (UTF32)0x800) {     bytesToWrite = 2;
    } else if (ch < (UTF32)0x10000) {   bytesToWrite = 3;
    } else if (ch < (UTF32)0x110000) {  bytesToWrite = 4;
    } else {                            bytesToWrite = 3;
    ch = UNI_REPLACEMENT_CHAR;
    }

    target += bytesToWrite;
    if (target > targetEnd) {
      source = oldSource; /* Back up source pointer! */
      target -= bytesToWrite; success = FALSE; break;
    }
    switch (bytesToWrite) { /* note: everything falls through. */
            case 4: *--target = (UTF8)((ch | byteMark) & byteMask); ch >>= 6;
            case 3: *--target = (UTF8)((ch | byteMark) & byteMask); ch >>= 6;
            case 2: *--target = (UTF8)((ch | byteMark) & byteMask); ch >>= 6;
            case 1: *--target =  (UTF8)(ch | firstByteMark[bytesToWrite]);
    }
    target += bytesToWrite;
  }
  // Function modified to retain source + target positions
  //  *sourceStart = source;
  //  *targetStart = target;
  return success;
}

string X3fPropertyCollection::getString( ByteStream *bytes ) {
  uint32 max_len = bytes->getRemainSize() / 2;
  const UTF16* start = (const UTF16*)bytes->getData();
  const UTF16* src_end = start;
  uint32 i = 0;
  for (; i < max_len && start == src_end; i++) {
    if (start[i] == 0) {
      src_end = &start[i];
    }
  }
  if (start != src_end) {
    UTF8* dest = new UTF8[i * 4 + 1];
    memset(dest, 0, i * 4 + 1);
    if (ConvertUTF16toUTF8(&start, src_end, &dest, &dest[i * 4 - 1])) {
      string ret((const char*)dest);
      delete[] dest;
      return ret;
    }
    delete[] dest;
  }
  return "";
}

void X3fPropertyCollection::addProperties( ByteStream *bytes, uint32 offset, uint32 length )
{
  bytes->setAbsoluteOffset(offset);
  string id = getIdAsString(bytes);
  if (id.compare("SECp"))
    ThrowRDE("X3fImage:Unknown Property signature");

  uint32 version = bytes->getUInt();
  if (version < 0x00020000)
    ThrowRDE("X3F Decoder: File version too old (properties)");

  uint32 entries = bytes->getUInt();
  if (!entries)
    return;

  if (0 != bytes->getUInt())
    ThrowRDE("X3F Decoder: Unknown property character encoding");

  // Skip 4 reserved bytes
  bytes->skipBytes(4);

  // Skip size (not used ATM)
  bytes->skipBytes(4);

  if (entries > 1000)
    ThrowRDE("X3F Decoder: Unreasonable number of properties: %u", entries);

  uint32 data_start = bytes->getOffset() + entries*8;
  for (uint32 i = 0; i < entries; i++) {
    uint32 key_pos = bytes->getUInt();
    uint32 value_pos = bytes->getUInt();
    bytes->pushOffset();
    try {
      bytes->setAbsoluteOffset(key_pos * 2 + data_start);
      string key = getString(bytes);
      bytes->setAbsoluteOffset(value_pos * 2 + data_start);
      string val = getString(bytes);
      props[key] = val;
    } catch (IOException) {}
    bytes->popOffset();
  }
}

} // namespace RawSpeed
