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

  RawImageDataFloat::RawImageDataFloat(void) {
    bpp = 4;
    dataType = TYPE_FLOAT32;
  }

  RawImageDataFloat::RawImageDataFloat(iPoint2D _dim, uint32 _cpp) :
  RawImageData(_dim, 4, _cpp)
  {
    dataType = TYPE_FLOAT32;
  }


  void RawImageDataFloat::calculateBlackAreas() {
    float accPixels[4] = {0,0,0,0};
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
          float *pixel = (float*)getDataUncropped(mOffset.x, y);
          for (int x = mOffset.x; x < dim.x+mOffset.x; x++) {
            accPixels[((y&1)<<1)|(x&1)] += *pixel++;
          }
        }
        totalpixels += area.size * dim.x;
      }

      /* Process vertical area */
      if (area.isVertical) {
        if ((int)area.offset+(int)area.size > uncropped_dim.x)
          ThrowRDE("RawImageData::calculateBlackAreas: Offset + size is larger than width of image");
        for (int y = mOffset.y; y < dim.y+mOffset.y; y++) {
          float *pixel = (float*)getDataUncropped(area.offset, y);
          for (uint32 x = area.offset; x < area.size+area.offset; x++) {
            accPixels[((y&1)<<1)|(x&1)] += *pixel++;
          }
        }
        totalpixels += area.size * dim.y;
      }
    }

    if (!totalpixels) {
      for (int i = 0 ; i < 4; i++)
        blackLevelSeparate[i] = blackLevel;
      return;
    }

    /* Calculate median value of black areas for each component */
    /* Adjust the number of total pixels so it is the same as the median of each histogram */
    totalpixels /= 4;

    for (int i = 0 ; i < 4; i++) {
      blackLevelSeparate[i] = (int)(65535.0f * accPixels[i]/totalpixels);
    }

    /* If this is not a CFA image, we do not use separate blacklevels, use average */
    if (!isCFA) {
      int total = 0;
      for (int i = 0 ; i < 4; i++)
        total+=blackLevelSeparate[i];
      for (int i = 0 ; i < 4; i++)
        blackLevelSeparate[i] = (total+2)>>2;
    }
  }

  void RawImageDataFloat::scaleBlackWhite() {
    const int skipBorder = 150;
    int gw = (dim.x - skipBorder) * cpp;
    if ((blackAreas.empty() && blackLevelSeparate[0] < 0 && blackLevel < 0) || whitePoint == 65536) {  // Estimate
      float b = 100000000;
      float m = -10000000;
      for (int row = skipBorder*cpp;row < (dim.y - skipBorder);row++) {
        float *pixel = (float*)getData(skipBorder, row);
        for (int col = skipBorder ; col < gw ; col++) {
          b = MIN(*pixel, b);
          m = MAX(*pixel, m);
          pixel++;
        }
      }
      if (blackLevel < 0)
        blackLevel = (int)b;
      if (whitePoint == 65536)
        whitePoint = (int)m;
      writeLog(DEBUG_PRIO_INFO, "Estimated black:%d, Estimated white: %d\n", blackLevel, whitePoint);
    }

    /* If filter has not set separate blacklevel, compute or fetch it */
    if (blackLevelSeparate[0] < 0)
      calculateBlackAreas();

    startWorker(RawImageWorker::SCALE_VALUES, true);
}

#if 0 // _MSC_VER > 1399 || defined(__SSE2__)

  void RawImageDataFloat::scaleValues(int start_y, int end_y) {
    bool use_sse2;
#ifdef _MSC_VER 
    int info[4];
    __cpuid(info, 1);
    use_sse2 = !!(info[3]&(1 << 26));
#else
    use_sse2 = TRUE;
#endif

    float app_scale = 65535.0f / (whitePoint - blackLevelSeparate[0]);
    // Check SSE2
    if (use_sse2 && app_scale < 63) {

      __m128i sseround;
      __m128i ssesub2;
      __m128i ssesign;
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

      for (int y = start_y; y < end_y; y++) {
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
        ushort16 *pixel = (ushort16*)getData(0, y);
        int *mul_local = &mul[2*(y&1)];
        int *sub_local = &sub[2*(y&1)];
        for (int x = 0 ; x < gw; x++) {
          pixel[x] = clampbits(((pixel[x] - sub_local[x&1]) * mul_local[x&1] + 8192) >> 14, 16);
        }
      }
    }
  }

#else

  void RawImageDataFloat::scaleValues(int start_y, int end_y) {
    int gw = dim.x * cpp;
    float mul[4];
    float sub[4];
    for (int i = 0; i < 4; i++) {
      int v = i;
      if ((mOffset.x&1) != 0)
        v ^= 1;
      if ((mOffset.y&1) != 0)
        v ^= 2;
      mul[i] = 65535.0f / (float)(whitePoint - blackLevelSeparate[v]);
      sub[i] = (float)blackLevelSeparate[v];
    }
    for (int y = start_y; y < end_y; y++) {
      float *pixel = (float*)getData(0, y);
      float *mul_local = &mul[2*(y&1)];
      float *sub_local = &sub[2*(y&1)];
      for (int x = 0 ; x < gw; x++) {
        pixel[x] = (pixel[x] - sub_local[x&1]) * mul_local[x&1];
      }
    }
  }

#endif

  /* This performs a 4 way interpolated pixel */
  /* The value is interpolated from the 4 closest valid pixels in */
  /* the horizontal and vertical direction. Pixels found further away */
  /* are weighed less */

void RawImageDataFloat::fixBadPixel( uint32 x, uint32 y, int component )
{
  float values[4];
  float dist[4];
  float weight[4];

  values[0] = values[1] = values[2] = values[3] = -1;
  dist[0] = dist[1] = dist[2] = dist[3] = 0;
  uchar8* bad_line = &mBadPixelMap[y*mBadPixelMapPitch];

  // Find pixel to the left
  int x_find = (int)x - 2;
  int curr = 0;
  while (x_find >= 0 && values[curr] < 0) {
    if (0 == ((bad_line[x_find>>3] >> (x_find&7)) & 1)) {
      values[curr] = ((float*)getData(x_find, y))[component];
      dist[curr] = float((int)x-x_find);
    }
    x_find-=2;
  }
  // Find pixel to the right
  x_find = (int)x + 2;
  curr = 1;
  while (x_find < uncropped_dim.x && values[curr] < 0) {
    if (0 == ((bad_line[x_find>>3] >> (x_find&7)) & 1)) {
      values[curr] = ((float*)getData(x_find, y))[component];
      dist[curr] = float(x_find-(int)x);
    }
    x_find+=2;
  }

  bad_line = &mBadPixelMap[x>>3];
  // Find pixel upwards
  int y_find = (int)y - 2;
  curr = 2;
  while (y_find >= 0 && values[curr] < 0) {
    if (0 == ((bad_line[y_find*mBadPixelMapPitch] >> (x&7)) & 1)) {
      values[curr] = ((float*)getData(x, y_find))[component];
      dist[curr] = float((int)y-y_find);
    }
    y_find-=2;
  }
  // Find pixel downwards
  y_find = (int)y + 2;
  curr = 3;
  while (y_find < uncropped_dim.y && values[curr] < 0) {
    if (0 == ((bad_line[y_find*mBadPixelMapPitch] >> (x&7)) & 1)) {
      values[curr] = ((float*)getData(x, y_find))[component];
      dist[curr] = float(y_find-(int)y);
    }
    y_find+=2;
  }
  // Find x weights
  float total_dist_x = dist[0] + dist[1];

  float total_div = 0.000001f;
  if (total_dist_x) {
    weight[0] = dist[0] > 0.0f ? (total_dist_x - dist[0]) / total_dist_x : 0;
    weight[1] = 1.0f - weight[0];
    total_div += 1;
  }

  // Find y weights
  float total_dist_y = dist[2] + dist[3];
  if (total_dist_y) {
    weight[2] = dist[2] > 0.0f ? (total_dist_x - dist[2]) / total_dist_y : 0;
    weight[3] = 1.0f - weight[2];
    total_div += 1;
  }


  float total_pixel = 0;
  for (int i = 0; i < 4; i++)
    if (values[i] >= 0)
      total_pixel += values[i] * dist[i];

  total_pixel /= total_div;
  float* pix = (float*)getDataUncropped(x, y);
  pix[component] = total_pixel;

  /* Process other pixels - could be done inline, since we have the weights */
  if (cpp > 1 && component == 0)
    for (int i = 1; i < (int)cpp; i++)
      fixBadPixel(x,y,i);

}


void RawImageDataFloat::doLookup( int start_y, int end_y ) {
  ThrowRDE("Float point lookup tables not implemented");
}

void RawImageDataFloat::setWithLookUp(ushort16 value, uchar8* dst, uint32* random) {
  float* dest = (float*)dst;
  if (table == NULL) {
    *dest = (float)value * (1.0f/65535);
    return;
  }

  ThrowRDE("Float point lookup tables not implemented");
}


} // namespace RawSpeed
