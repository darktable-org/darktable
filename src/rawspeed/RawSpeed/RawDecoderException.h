#ifndef RAW_DECODER_EXCEPTION_H
#define RAW_DECODER_EXCEPTION_H

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

void ThrowRDE(const char* fmt, ...);

class RawDecoderException : public std::runtime_error
{
public:
  RawDecoderException(const string _msg) : runtime_error(_msg) {
    _RPT1(0, "RawDecompressor Exception: %s\n", _msg.c_str());
  }
};

} // namespace RawSpeed

#endif
