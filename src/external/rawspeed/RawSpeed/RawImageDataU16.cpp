#include "StdAfx.h"
#include "RawImage.h"
#include "RawDecoder.h"  // For exceptions
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
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace RawSpeed {

RawImageDataU16::RawImageDataU16(void)
{
  dataType = TYPE_USHORT16;
  bpp = 2;
}

RawImageDataU16::RawImageDataU16(iPoint2D _dim, uint32 _cpp) :
RawImageData(_dim, 2, _cpp)
{
  dataType = TYPE_USHORT16;
}


void RawImageDataU16::calculateBlackAreas() {
  int* histogram = (int*)malloc(4*65536*sizeof(int));
  memset(histogram, 0, 4*65536*sizeof(int));
  int totalpixels = 0;

  for (uint32 i = 0; i < blackAreas.size(); i++) {
    BlackArea area = blackAreas[i];

    /* Make sure area sizes are multiple of two, 
       so we have the same amount of pixels for each CFA group */
    area.size = area.size - (area.size&1);

    /* If offset is negative (relative to right or bottom border) calculate
       the offset from the left or top border */
    if(area.offset < 0)
      area.offset += area.isVertical ? uncropped_dim.x : uncropped_dim.y;

    /* Process horizontal area */
    if (!area.isVertical) {
      if ((int)area.offset+(int)area.size > uncropped_dim.y)
        ThrowRDE("RawImageData::calculateBlackAreas: Offset + size is larger than height of image");
      for (uint32 y = area.offset; y < area.offset+area.size; y++) {
        ushort16 *pixel = (ushort16*)getDataUncropped(mOffset.x, y);
        int* localhist = &histogram[(y&1)*(65536*2)];
        for (int x = mOffset.x; x < dim.x+mOffset.x; x++) {
          localhist[((x&1)<<16) + *pixel]++;
        }
      }
      totalpixels += area.size * dim.x;
    }

    /* Process vertical area */
    if (area.isVertical) {
      if ((int)area.offset+(int)area.size > uncropped_dim.x)
        ThrowRDE("RawImageData::calculateBlackAreas: Offset + size is larger than width of image");
      for (int y = mOffset.y; y < dim.y+mOffset.y; y++) {
        ushort16 *pixel = (ushort16*)getDataUncropped(area.offset, y);
        int* localhist = &histogram[(y&1)*(65536*2)];
        for (uint32 x = area.offset; x < area.size+area.offset; x++) {
          localhist[((x&1)<<16) + *pixel]++;
        }
      }
      totalpixels += area.size * dim.y;
    }
  }

  if (!totalpixels) {
    for (int i = 0 ; i < 4; i++)
      blackLevelSeparate[i] = blackLevel;
    free(histogram);
    return;
  }

  /* Calculate median value of black areas for each component */
  /* Adjust the number of total pixels so it is the same as the median of each histogram */
  totalpixels /= 4*2;

  for (int i = 0 ; i < 4; i++) {
    int* localhist = &histogram[i*65536];
    int acc_pixels = localhist[0];
    int pixel_value = 0;
    while (acc_pixels <= totalpixels && pixel_value < 65535) {
      pixel_value++;
      acc_pixels += localhist[pixel_value];
    }
    blackLevelSeparate[i] = pixel_value;
  }

  /* If this is not a CFA image, we do not use separate blacklevels, use average */
  if (!isCFA) {
    int total = 0;
    for (int i = 0 ; i < 4; i++)
      total+=blackLevelSeparate[i];
    for (int i = 0 ; i < 4; i++)
      blackLevelSeparate[i] = (total+2)>>2;
  }
  free(histogram);
}

void RawImageDataU16::scaleBlackWhite() {
  const int skipBorder = 250;
  int gw = (dim.x - skipBorder) * cpp;
  if ((blackAreas.empty() && blackLevelSeparate[0] < 0 && blackLevel < 0) || whitePoint >= 65536) {  // Estimate
    int b = 65536;
    int m = 0;
    for (int row = skipBorder; row < (dim.y - skipBorder);row++) {
      ushort16 *pixel = (ushort16*)getData(skipBorder, row);
      for (int col = skipBorder ; col < gw ; col++) {
        b = MIN(*pixel, b);
        m = MAX(*pixel, m);
        pixel++;
      }
    }
    if (blackLevel < 0)
      blackLevel = b;
    if (whitePoint >= 65536)
      whitePoint = m;
    writeLog(DEBUG_PRIO_INFO, "ISO:%d, Estimated black:%d, Estimated white: %d\n", metadata.isoSpeed, blackLevel, whitePoint);
  }

  /* Skip, if not needed */
  if ((blackAreas.size() == 0 && blackLevel == 0 && whitePoint == 65535 && blackLevelSeparate[0] < 0) || dim.area() <= 0)
    return;

  /* If filter has not set separate blacklevel, compute or fetch it */
  if (blackLevelSeparate[0] < 0)
    calculateBlackAreas();

  startWorker(RawImageWorker::SCALE_VALUES, true);
}

#if _MSC_VER > 1399 || defined(__SSE2__)

void RawImageDataU16::scaleValues(int start_y, int end_y) {
  bool use_sse2;
#ifdef _MSC_VER 
  int info[4];
  __cpuid(info, 1);
  use_sse2 = !!(info[3]&(1 << 26));
#else
  use_sse2 = TRUE;
#endif

  int depth_values = whitePoint - blackLevelSeparate[0];
  float app_scale = 65535.0f / depth_values;

  // Scale in 30.2 fp
  int full_scale_fp = (int)(app_scale * 4.0f);
  // Half Scale in 18.14 fp
  int half_scale_fp = (int)(app_scale * 4095.0f);

  // Check SSE2
  if (use_sse2 && app_scale < 63) {

    __m128i sseround;
    __m128i ssesub2;
    __m128i ssesign;
    __m128i rand_mul;
    __m128i rand_mask;
    __m128i sse_full_scale_fp;
    __m128i sse_half_scale_fp;

    uint32* sub_mul = (uint32*)_aligned_malloc(16*4*2, 16);
    if (!sub_mul)
	  ThrowRDE("Out of memory, failed to allocate 128 bytes");
    uint32 gw = pitch / 16;
    // 10 bit fraction
    uint32 mul = (int)(1024.0f * 65535.0f / (float)(whitePoint - blackLevelSeparate[mOffset.x&1]));  
    mul |= ((int)(1024.0f * 65535.0f / (float)(whitePoint - blackLevelSeparate[(mOffset.x+1)&1])))<<16;
    uint32 b = blackLevelSeparate[mOffset.x&1] | (blackLevelSeparate[(mOffset.x+1)&1]<<16);

    for (int i = 0; i< 4; i++) {
      sub_mul[i] = b;     // Subtract even lines
      sub_mul[4+i] = mul;   // Multiply even lines
    }

    mul = (int)(1024.0f * 65535.0f / (float)(whitePoint - blackLevelSeparate[2+(mOffset.x&1)]));
    mul |= ((int)(1024.0f * 65535.0f / (float)(whitePoint - blackLevelSeparate[2+((mOffset.x+1)&1)])))<<16;
    b = blackLevelSeparate[2+(mOffset.x&1)] | (blackLevelSeparate[2+((mOffset.x+1)&1)]<<16);

    for (int i = 0; i< 4; i++) {
      sub_mul[8+i] = b;   // Subtract odd lines
      sub_mul[12+i] = mul;  // Multiply odd lines
    }

    sseround = _mm_set_epi32(512, 512, 512, 512);
    ssesub2 = _mm_set_epi32(32768, 32768, 32768, 32768);
    ssesign = _mm_set_epi32(0x80008000, 0x80008000, 0x80008000, 0x80008000);
    sse_full_scale_fp = _mm_set1_epi32(full_scale_fp|(full_scale_fp<<16));
    sse_half_scale_fp = _mm_set1_epi32(half_scale_fp >> 4);

    if (mDitherScale) {
      rand_mul = _mm_set1_epi32(0x4d9f1d32);
    } else {
      rand_mul = _mm_set1_epi32(0);
    }
    rand_mask = _mm_set1_epi32(0x00ff00ff);  // 8 random bits

    for (int y = start_y; y < end_y; y++) {
      __m128i sserandom;
      if (mDitherScale) {
          sserandom = _mm_set_epi32(dim.x*1676+y*18000, dim.x*2342+y*34311, dim.x*4272+y*12123, dim.x*1234+y*23464);
      } else {
        sserandom = _mm_setzero_si128();
      }
      __m128i* pixel = (__m128i*) & data[(mOffset.y+y)*pitch];
      __m128i ssescale, ssesub;
      if (((y+mOffset.y)&1) == 0) { 
        ssesub = _mm_load_si128((__m128i*)&sub_mul[0]);
        ssescale = _mm_load_si128((__m128i*)&sub_mul[4]);
      } else {
        ssesub = _mm_load_si128((__m128i*)&sub_mul[8]);
        ssescale = _mm_load_si128((__m128i*)&sub_mul[12]);
      }

      for (uint32 x = 0 ; x < gw; x++) {
        __m128i pix_high;
        __m128i temp;
        _mm_prefetch((char*)(pixel+1), _MM_HINT_T0);
        __m128i pix_low = _mm_load_si128(pixel);
        // Subtract black
        pix_low = _mm_subs_epu16(pix_low, ssesub);
        // Multiply the two unsigned shorts and combine it to 32 bit result
        pix_high = _mm_mulhi_epu16(pix_low, ssescale);
        temp = _mm_mullo_epi16(pix_low, ssescale);
        pix_low = _mm_unpacklo_epi16(temp, pix_high);
        pix_high = _mm_unpackhi_epi16(temp, pix_high);
        // Add rounder
        pix_low = _mm_add_epi32(pix_low, sseround);
        pix_high = _mm_add_epi32(pix_high, sseround);

        sserandom = _mm_xor_si128(_mm_mulhi_epi16(sserandom, rand_mul), _mm_mullo_epi16(sserandom, rand_mul));
        __m128i rand_masked = _mm_and_si128(sserandom, rand_mask);  // Get 8 random bits
        rand_masked = _mm_mullo_epi16(rand_masked, sse_full_scale_fp);
        
        __m128i zero = _mm_setzero_si128();
        __m128i rand_lo = _mm_sub_epi32(sse_half_scale_fp, _mm_unpacklo_epi16(rand_masked,zero));
        __m128i rand_hi = _mm_sub_epi32(sse_half_scale_fp, _mm_unpackhi_epi16(rand_masked,zero));

        pix_low = _mm_add_epi32(pix_low, rand_lo);
        pix_high = _mm_add_epi32(pix_high, rand_hi);

        // Shift down
        pix_low = _mm_srai_epi32(pix_low, 10);
        pix_high = _mm_srai_epi32(pix_high, 10);
        // Subtract to avoid clipping
        pix_low = _mm_sub_epi32(pix_low, ssesub2);
        pix_high = _mm_sub_epi32(pix_high, ssesub2);
        // Pack
        pix_low = _mm_packs_epi32(pix_low, pix_high);
        // Shift sign off
        pix_low = _mm_xor_si128(pix_low, ssesign);
        _mm_store_si128(pixel, pix_low);
        pixel++;
      }
    }
    _aligned_free(sub_mul);
  } else {
    // Not SSE2
    int gw = dim.x * cpp;
    int mul[4];
    int sub[4];
    for (int i = 0; i < 4; i++) {
      int v = i;
      if ((mOffset.x&1) != 0)
        v ^= 1;
      if ((mOffset.y&1) != 0)
        v ^= 2;
      mul[i] = (int)(16384.0f * 65535.0f / (float)(whitePoint - blackLevelSeparate[v]));
      sub[i] = blackLevelSeparate[v];
    }
    for (int y = start_y; y < end_y; y++) {
      int v = dim.x + y * 36969;
      ushort16 *pixel = (ushort16*)getData(0, y);
      int *mul_local = &mul[2*(y&1)];
      int *sub_local = &sub[2*(y&1)];
      for (int x = 0 ; x < gw; x++) {
        int rand;
        if (mDitherScale) {
          v = 18000 *(v & 65535) + (v >> 16);
          rand = half_scale_fp - (full_scale_fp * (v&2047));
        } else {
          rand = 0;
        }
        pixel[x] = clampbits(((pixel[x] - sub_local[x&1]) * mul_local[x&1] + 8192 + rand) >> 14, 16);
      }
    }
  }
}

#else

void RawImageDataU16::scaleValues(int start_y, int end_y) {
  int gw = dim.x * cpp;
  int mul[4];
  int sub[4];
  int depth_values = whitePoint - blackLevelSeparate[0];
  float app_scale = 65535.0f / depth_values;

  // Scale in 30.2 fp
  int full_scale_fp = (int)(app_scale * 4.0f);
  // Half Scale in 18.14 fp
  int half_scale_fp = (int)(app_scale * 4095.0f);

  for (int i = 0; i < 4; i++) {
    int v = i;
    if ((mOffset.x&1) != 0)
      v ^= 1;
    if ((mOffset.y&1) != 0)
      v ^= 2;
    mul[i] = (int)(16384.0f * 65535.0f / (float)(whitePoint - blackLevelSeparate[v]));
    sub[i] = blackLevelSeparate[v];
  }
  for (int y = start_y; y < end_y; y++) {
    int v = dim.x + y * 36969;
    ushort16 *pixel = (ushort16*)getData(0, y);
    int *mul_local = &mul[2*(y&1)];
    int *sub_local = &sub[2*(y&1)];
    for (int x = 0 ; x < gw; x++) {
      int rand;
      if (mDitherScale) {
        v = 18000 *(v & 65535) + (v >> 16);
        rand = half_scale_fp - (full_scale_fp * (v&2047));
      } else {
        rand = 0;
      }
      pixel[x] = clampbits(((pixel[x] - sub_local[x&1]) * mul_local[x&1] + 8192 + rand) >> 14, 16);
    }
  }
}

#endif

/* This performs a 4 way interpolated pixel */
/* The value is interpolated from the 4 closest valid pixels in */
/* the horizontal and vertical direction. Pixels found further away */
/* are weighed less */

void RawImageDataU16::fixBadPixel( uint32 x, uint32 y, int component )
{
  int values[4];
  int dist[4];
  int weight[4];

  values[0] = values[1] = values[2] = values[3] = -1;
  dist[0] = dist[1] = dist[2] = dist[3] = 0;
  uchar8* bad_line = &mBadPixelMap[y*mBadPixelMapPitch];
  int step = isCFA ? 2 : 1;

  // Find pixel to the left
  int x_find = (int)x - step;
  int curr = 0;
  while (x_find >= 0 && values[curr] < 0) {
    if (0 == ((bad_line[x_find>>3] >> (x_find&7)) & 1)) {
      values[curr] = ((ushort16*)getData(x_find, y))[component];
      dist[curr] = (int)x-x_find;
    }
    x_find -= step;
  }
  // Find pixel to the right
  x_find = (int)x + step;
  curr = 1;
  while (x_find < uncropped_dim.x && values[curr] < 0) {
    if (0 == ((bad_line[x_find>>3] >> (x_find&7)) & 1)) {
      values[curr] = ((ushort16*)getData(x_find, y))[component];
      dist[curr] = x_find-(int)x;
    }
    x_find += step;
  }

  bad_line = &mBadPixelMap[x>>3];
  // Find pixel upwards
  int y_find = (int)y - step;
  curr = 2;
  while (y_find >= 0 && values[curr] < 0) {
    if (0 == ((bad_line[y_find*mBadPixelMapPitch] >> (x&7)) & 1)) {
      values[curr] = ((ushort16*)getData(x, y_find))[component];
      dist[curr] = (int)y-y_find;
    }
    y_find -= step;
  }
  // Find pixel downwards
  y_find = (int)y + step;
  curr = 3;
  while (y_find < uncropped_dim.y && values[curr] < 0) {
    if (0 == ((bad_line[y_find*mBadPixelMapPitch] >> (x&7)) & 1)) {
      values[curr] = ((ushort16*)getData(x, y_find))[component];
      dist[curr] = y_find-(int)y;
    }
    y_find += step;
  }

  // Find x weights
  int total_dist_x = dist[0] + dist[1];

  int total_shifts = 7;
  if (total_dist_x) {
    weight[0] = dist[0] ? (total_dist_x - dist[0]) * 256 / total_dist_x : 0;
    weight[1] = 256 - weight[0];
    total_shifts++;
  }

  // Find y weights
  int total_dist_y = dist[2] + dist[3];
  if (total_dist_y) {
    weight[2] = dist[2] ? (total_dist_x - dist[2]) * 256 / total_dist_y : 0;
    weight[3] = 256-weight[2];
    total_shifts++;
  }

  int total_pixel = 0;
  for (int i = 0; i < 4; i++)
    if (values[i] >= 0)
      total_pixel += values[i] * weight[i];

  total_pixel >>= total_shifts;
  ushort16* pix = (ushort16*)getDataUncropped(x, y);
  pix[component] = clampbits(total_pixel, 16);

  /* Process other pixels - could be done inline, since we have the weights */
  if (cpp > 1 && component == 0)
    for (int i = 1; i < (int)cpp; i++)
      fixBadPixel(x,y,i);
}

// TODO: Could be done with SSE2
void RawImageDataU16::doLookup( int start_y, int end_y )
{
  if (table->ntables == 1) {
    ushort16* t = table->getTable(0);
    if (table->dither) {
      int gw = uncropped_dim.x * cpp;
      uint32* t = (uint32*)table->getTable(0);
      for (int y = start_y; y < end_y; y++) {
        uint32 v = (uncropped_dim.x + y * 13) ^ 0x45694584;
        ushort16 *pixel = (ushort16*)getDataUncropped(0, y);
        for (int x = 0 ; x < gw; x++) {
          ushort16 p = *pixel;
          uint32 lookup = t[p];
          uint32 base = lookup & 0xffff;
          uint32 delta = lookup >> 16;
          v = 15700 *(v & 65535) + (v >> 16);
          uint32 pix = base + (((delta * (v&2047) + 1024)) >> 12);
          *pixel = pix;
          pixel++;
        }
      }
      return;
    }

    int gw = uncropped_dim.x * cpp;
    for (int y = start_y; y < end_y; y++) {
      ushort16 *pixel = (ushort16*)getDataUncropped(0, y);
      for (int x = 0 ; x < gw; x++) {
        *pixel = t[*pixel];
        pixel ++;
      }
    }
    return;
  } 
  ThrowRDE("Table lookup with multiple components not implemented");
}


// setWithLookUp will set a single pixel by using the lookup table if supplied,
// You must supply the destination where the value should be written, and a pointer to
// a value that will be used to store a random counter that can be reused between calls.
void RawImageDataU16::setWithLookUp(ushort16 value, uchar8* dst, uint32* random) {
  ushort16* dest = (ushort16*)dst;
  if (table == NULL) {
    *dest = value;
    return;
  }
  if (table->dither) {
    uint32* t = (uint32*)table->tables;
    uint32 lookup = t[value];
    uint32 base = lookup & 0xffff;
    uint32 delta = lookup >> 16;
    uint32 r = *random;
    
    uint32 pix = base + ((delta * (r&2047) + 1024) >> 12);
    *random = 15700 *(r & 65535) + (r >> 16);
    *dest = pix;
    return;
  }
  ushort16* t = (ushort16*)table->tables;
  *dest = t[value];
}


} // namespace RawSpeed
