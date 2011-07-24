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



#if defined(__MINGW32__)
#define UNICODE
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif

#define WIN32_LEAN_AND_MEAN		// Exclude rarely-used stuff from Windows headers
#include <stdio.h>
#ifndef __unix__
#include <tchar.h>
#include <io.h>
#include <Windows.h>
#ifndef __MINGW32__
#include <crtdbg.h>
#else
#include <stdexcept>
#endif
#include <malloc.h>
#else // if unix
#ifdef _XOPEN_SOURCE
#if (_XOPEN_SOURCE < 600)
#undef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600  // for posix_memalign()
#endif // _XOPEN_SOURCE < 600
#else
#define _XOPEN_SOURCE 600  // for posix_memalign()
#endif //_XOPEN_SOURCE
#include <stdlib.h>
#include <stdarg.h>
#include <stdexcept>
#include <exception>
#include <string.h>
#include <assert.h>
#endif // __unix__
#include <math.h>
#include "pthread.h"
// STL
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <list>
using namespace std;

//My own
#include "TiffTag.h"
#include "Common.h"
#include "Point.h"


