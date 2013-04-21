#include "StdAfx.h"
#include "CameraSensorInfo.h"
/*
RawSpeed - RAW file decoder.

Copyright (C) 2011 Klaus Post

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

CameraSensorInfo::CameraSensorInfo(int black_level, int white_level, int min_iso, int max_iso) :
mBlackLevel(black_level),
mWhiteLevel(white_level),
mMinIso(min_iso), 
mMaxIso(max_iso)
{
}

CameraSensorInfo::~CameraSensorInfo(void)
{
}

bool CameraSensorInfo::isIsoWithin( int iso )
{
  return (iso >= mMinIso && iso <= mMaxIso) || (iso >= mMinIso && 0 == mMaxIso);
}

bool CameraSensorInfo::isDefault()
{
  return (0 == mMinIso && 0 == mMaxIso);
}

} // namespace RawSpeed
