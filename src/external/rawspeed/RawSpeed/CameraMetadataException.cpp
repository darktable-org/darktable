#include "StdAfx.h"
#include "CameraMetadataException.h"
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

void ThrowCME(const char* fmt, ...) {
  va_list val;
  va_start(val, fmt);
  char buf[8192];
#if defined(__unix__) || defined(__MINGW32__)
  vsnprintf(buf, 8192, fmt, val);
#else
  vsprintf_s(buf, 8192, fmt, val);
#endif
  va_end(val);
  _RPT1(0, "EXCEPTION: %s\n", buf);
  throw CameraMetadataException(buf);
}

CameraMetadataException::CameraMetadataException(const string _msg): runtime_error(_msg) {
  _RPT1(0, "CameraMetadata Exception: %s\n", _msg.c_str());
}

} // namespace RawSpeed
