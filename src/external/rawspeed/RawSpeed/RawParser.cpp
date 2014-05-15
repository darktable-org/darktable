#include "StdAfx.h"
#include "RawParser.h"
#include "TiffParserException.h"
#include "TiffParser.h"
#include "MrwDecoder.h"


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


RawParser::RawParser(FileMap* inputData): mInput(inputData) {
}


RawParser::~RawParser(void) {
}

RawDecoder* RawParser::getDecoder() {
  // If the file starts with "\0MRM" it's a MRW
  if (MrwDecoder::isMRW(mInput)) {
    try {
      return new MrwDecoder(mInput);
    } catch (RawDecoderException) {}
  } else {
    try {
      TiffParser p(mInput);
      p.parseData();
      return p.getDecoder();
    } catch (TiffParserException) {}
  }
  throw RawDecoderException("No decoder found. Sorry.");
  return NULL;
}

} // namespace RawSpeed
