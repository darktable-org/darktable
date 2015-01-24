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
#ifndef COMMON_H
#define COMMON_H


#if !defined(__unix__) && !defined(__APPLE__) && !defined(__MINGW32__) 
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#define MIN(a,b) min(a,b)
#define MAX(a,b) max(a,b)
typedef unsigned __int64 uint64;
// MSVC may not have NAN
#ifndef NAN
  static const unsigned long __nan[2] = {0xffffffff, 0x7fffffff};
  #define NAN (*(const float *) __nan)
#endif
#else // On linux
#define _ASSERTE(a) void(a)
#define _RPT0(a,b) 
#define _RPT1(a,b,c) 
#define _RPT2(a,b,c,d) 
#define _RPT3(a,b,c,d,e) 
#define _RPT4(a,b,c,d,e,f) 
#define __inline inline
#define _strdup(a) strdup(a)
#ifndef MIN
#define MIN(a, b)  lmin(a,b)
#endif
#ifndef MAX
#define MAX(a, b)  lmax(a,b)
#endif
typedef unsigned long long uint64;
#ifndef __MINGW32__
void* _aligned_malloc(size_t bytes, size_t alignment);
#define _aligned_free(a) do { free(a); } while (0)
typedef char* LPCWSTR;
#endif
#endif // __unix__

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#define get2BE(data,pos) ((((ushort16)(data)[pos]) << 8) | \
                           ((ushort16)(data)[pos+1]))

#define get2LE(data,pos) ((((ushort16)(data)[pos+1]) << 8) | \
                           ((ushort16)(data)[pos]))

#define get4BE(data,pos) ((((uint32)(data)[pos]) << 24) | \
                          (((uint32)(data)[pos+1]) << 16) | \
                          (((uint32)(data)[pos+2]) << 8) | \
                           ((uint32)(data)[pos+3]))

#define get4LE(data,pos) ((((uint32)(data)[pos+3]) << 24) | \
                          (((uint32)(data)[pos+2]) << 16) | \
                          (((uint32)(data)[pos+1]) << 8) | \
                           ((uint32)(data)[pos]))

#define get8LE(data,pos) ((((uint64)(data)[pos+7]) << 56) | \
                          (((uint64)(data)[pos+6]) << 48) | \
                          (((uint64)(data)[pos+5]) << 40) | \
                          (((uint64)(data)[pos+4]) << 32) | \
                          (((uint64)(data)[pos+3]) << 24) | \
                          (((uint64)(data)[pos+2]) << 16) | \
                          (((uint64)(data)[pos+1]) << 8)  | \
                           ((uint64)(data)[pos]))

int rawspeed_get_number_of_processor_cores();


namespace RawSpeed {

typedef signed char char8;
typedef unsigned char uchar8;
typedef unsigned int uint32;
typedef signed int int32;
typedef unsigned short ushort16;
typedef signed short short16;

typedef enum Endianness {
  big, little, unknown
} Endianness;

const int DEBUG_PRIO_ERROR = 0x10;
const int DEBUG_PRIO_WARNING = 0x100;
const int DEBUG_PRIO_INFO = 0x1000;
const int DEBUG_PRIO_EXTRA = 0x10000;

void writeLog(int priority, const char *format, ...);

inline void BitBlt(uchar8* dstp, int dst_pitch, const uchar8* srcp, int src_pitch, int row_size, int height) {
  if (height == 1 || (dst_pitch == src_pitch && src_pitch == row_size)) {
    memcpy(dstp, srcp, row_size*height);
    return;
  }
  for (int y=height; y>0; --y) {
    memcpy(dstp, srcp, row_size);
    dstp += dst_pitch;
    srcp += src_pitch;
  }
}
inline bool isPowerOfTwo (int val) {
  return (val & (~val+1)) == val;
}

inline int lmin(int p0, int p1) {
  return p1 + ((p0 - p1) & ((p0 - p1) >> 31));
}
inline int lmax(int p0, int p1) {
  return p0 - ((p0 - p1) & ((p0 - p1) >> 31));
}

inline uint32 getThreadCount()
{
#ifdef WIN32
  return pthread_num_processors_np();
#else
  return rawspeed_get_number_of_processor_cores();
#endif
}

inline Endianness getHostEndianness() {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  return little;
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  return big;
#else
  ushort16 testvar = 0xfeff; 
  uint32 firstbyte = ((uchar8 *)&testvar)[0];
  if (firstbyte == 0xff)
    return little;
  else if (firstbyte == 0xfe)
    return big;
  else
    _ASSERTE(FALSE);

  // Return something to make compilers happy
  return unknown;
#endif
}

#if defined(__GNUC__) && (PIPE_CC_GCC_VERSION >= 403) && defined(__BYTE_ORDER__) 
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ && __alignof__ (int) == 1
#define LE_PLATFORM_HAS_BSWAP
#define PLATFORM_BSWAP32(A) __builtin_bswap32(A)
#endif
#endif

#ifdef _MSC_VER
#include <intrin.h>
#define LE_PLATFORM_HAS_BSWAP
#define PLATFORM_BSWAP32(A) _byteswap_ulong(A)
#endif

inline uint32 clampbits(int x, uint32 n) { 
  uint32 _y_temp; 
  if( (_y_temp=x>>n) ) 
    x = ~_y_temp >> (32-n); 
  return x;
}

/* This is faster - at least when compiled on visual studio 32 bits */
inline int other_abs(int x) { int const mask = x >> 31; return (x + mask) ^ mask;}

/* Remove all spaces at the end of a string */

inline void TrimSpaces(string& str) {
  // Trim Both leading and trailing spaces
  size_t startpos = str.find_first_not_of(" \t"); // Find the first character position after excluding leading blank spaces
  size_t endpos = str.find_last_not_of(" \t"); // Find the first character position from reverse af

  // if all spaces or empty return an empty string
  if ((string::npos == startpos) || (string::npos == endpos)) {
    str = "";
  } else
    str = str.substr(startpos, endpos - startpos + 1);
}


inline vector<string> split_string(string input, char c = ' ') {
  vector<string> result;
  const char *str = input.c_str();

  while(1) {
    const char *begin = str;

    while(*str != c && *str)
      str++;

    result.push_back(string(begin, str));

    if(0 == *str++)
      break;
  }

  return result;
}

typedef enum {
  BitOrder_Plain,  /* Memory order */
  BitOrder_Jpeg,   /* Input is added to stack byte by byte, and output is lifted from top */
  BitOrder_Jpeg16, /* Same as above, but 16 bits at the time */
  BitOrder_Jpeg32, /* Same as above, but 32 bits at the time */
} BitOrder;

} // namespace RawSpeed

#endif
