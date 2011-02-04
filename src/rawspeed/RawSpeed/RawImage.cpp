#include "StdAfx.h"
#include "RawImage.h"
#include "RawDecoder.h"  // For exceptions
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
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace RawSpeed {

RawImageData::RawImageData(void):
    dim(0, 0), bpp(0), isCFA(true),
    blackLevel(-1), whitePoint(65536),
    dataRefCount(0), data(0), cpp(1) {
  pthread_mutex_init(&mymutex, NULL);
  subsampling.x = subsampling.y = 1;
}

RawImageData::RawImageData(iPoint2D _dim, uint32 _bpc, uint32 _cpp) :
    dim(_dim), bpp(_bpc),
    blackLevel(-1), whitePoint(65536),
    dataRefCount(0), data(0), cpp(cpp) {
  subsampling.x = subsampling.y = 1;
  createData();
  pthread_mutex_init(&mymutex, NULL);
}

RawImageData::~RawImageData(void) {
  _ASSERTE(dataRefCount == 0);
  if (data)
    _aligned_free(data);
  data = 0;
  mOffset = iPoint2D(0, 0);
  pthread_mutex_destroy(&mymutex);
}


void RawImageData::createData() {
  if (dim.x > 65535 || dim.y > 65535)
    ThrowRDE("RawImageData: Dimensions too large for allocation.");
  if (dim.x <= 0 || dim.y <= 0)
    ThrowRDE("RawImageData: Dimension of one sides is less than 1 - cannot allocate image.");
  if (data)
    ThrowRDE("RawImageData: Duplicate data allocation in createData.");
  pitch = (((dim.x * bpp) + 15) / 16) * 16;
  data = (uchar8*)_aligned_malloc(pitch * dim.y, 16);
  if (!data)
    ThrowRDE("RawImageData::createData: Memory Allocation failed.");
}

void RawImageData::destroyData() {
  if (data)
    _aligned_free(data);
  data = 0;
}

void RawImageData::setCpp(uint32 val) {
  if (data)
    ThrowRDE("RawImageData: Attempted to set Components per pixel after data allocation");
  if (val > 4)
    ThrowRDE("RawImageData: Only up to 4 components per pixel is support - attempted to set: %d", val);
  bpp /= cpp;
  cpp = val;
  bpp *= val;
}

uchar8* RawImageData::getData() {
  if (!data)
    ThrowRDE("RawImageData::getData - Data not yet allocated.");
  return &data[mOffset.y*pitch+mOffset.x*bpp];
}

uchar8* RawImageData::getData(uint32 x, uint32 y) {
  if ((int)x >= dim.x)
    ThrowRDE("RawImageData::getData - X Position outside image requested.");
  if ((int)y >= dim.y) {
    ThrowRDE("RawImageData::getData - Y Position outside image requested.");
  }

  x += mOffset.x;
  y += mOffset.y;

  if (!data)
    ThrowRDE("RawImageData::getData - Data not yet allocated.");

  return &data[y*pitch+x*bpp];
}

uchar8* RawImageData::getDataUncropped(uint32 x, uint32 y) {
  if ((int)x >= dim.x+mOffset.x)
    ThrowRDE("RawImageData::getDataUncropped - X Position outside image requested.");
  if ((int)y >= dim.y+mOffset.y) {
    ThrowRDE("RawImageData::getDataUncropped - Y Position outside image requested.");
  }

  if (!data)
    ThrowRDE("RawImageData::getDataUncropped - Data not yet allocated.");

  return &data[y*pitch+x*bpp];
}

void RawImageData::subFrame(iPoint2D offset, iPoint2D new_size) {
  if (!new_size.isThisInside(dim - offset)) {
    printf("WARNING: RawImageData::subFrame - Attempted to create new subframe larger than original size. Crop skipped.\n");
    return;
  }
  if (offset.x < 0 || offset.y < 0) {
    printf("WARNING: RawImageData::subFrame - Negative crop offset. Crop skipped.\n");
    return;
  }

  mOffset += offset;
  dim = new_size;
}

void RawImageData::calculateBlackAreas() {
  int* histogram = (int*)malloc(4*65536*sizeof(int));
  memset(histogram, 0, 4*65536*sizeof(int));
  int totalpixels = 0;

  for (uint32 i = 0; i < blackAreas.size(); i++) {
    BlackArea area = blackAreas[i];
    /* Process horizontal area */
    if (!area.isVertical) {
      for (uint32 y = area.offset; y < area.offset+area.size; y++) {
        ushort16 *pixel = (ushort16*)getDataUncropped(mOffset.x, y);
        int* localhist = &histogram[(y&1)*(65536*2)];
        for (int x = mOffset.x; x < dim.x; x++) {
          localhist[((x&1)<<16) + *pixel]++;
        }
      }
      totalpixels += area.size * dim.x;
    }

    /* Process vertical area */
    if (area.isVertical) {
      for (int y = mOffset.y; y < dim.y; y++) {
        ushort16 *pixel = (ushort16*)getDataUncropped(area.offset, y);
        int* localhist = &histogram[(y&1)*(65536*2)];
        for (uint32 x = area.offset; x < area.size; x++) {
          localhist[((x&1)<<16) + *pixel]++;
        }
      }
    }
    totalpixels += area.size * dim.y;
  }

  if (!totalpixels) {
    for (int i = 0 ; i < 4; i++)
      blackLevelSeparate[i] = blackLevel;
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
  free(histogram);
}

void RawImageData::scaleBlackWhite() {
  const int skipBorder = 150;
  int gw = (dim.x - skipBorder) * cpp;
  if (blackLevel < 0 || whitePoint == 65536) {  // Estimate
    int b = 65536;
    int m = 0;
    for (int row = skipBorder*cpp;row < (dim.y - skipBorder);row++) {
      ushort16 *pixel = (ushort16*)getData(skipBorder, row);
      for (int col = skipBorder ; col < gw ; col++) {
        b = MIN(*pixel, b);
        m = MAX(*pixel, m);
        pixel++;
      }
    }
    if (blackLevel < 0)
      blackLevel = b;
    if (whitePoint == 65536)
      whitePoint = m;
    printf("Estimated black:%d, Estimated white: %d\n", blackLevel, whitePoint);
  }

  calculateBlackAreas();
  scaleValues();
}

#if _MSC_VER > 1399 || defined(__SSE2__)

void RawImageData::scaleValues() {
  bool use_sse2;
#ifdef _MSC_VER 
  int info[4];
  __cpuid(info, 1);
  use_sse2 = !!(info[3]&(1 << 26));
#else
  use_sse2 = TRUE;
#endif

  // Check SSE2
  if (use_sse2) {

    __m128i sseround;
    __m128i ssesub2;
    __m128i ssesign;
    uint32* sub_mul = (uint32*)_aligned_malloc(16*4*2, 16);
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

    for (int y = 0; y < dim.y; y++) {
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
    for (int y = 0; y < dim.y; y++) {
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

void RawImageData::scaleValues() {
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
  for (int y = 0; y < dim.y; y++) {
    ushort16 *pixel = (ushort16*)getData(0, y);
    int *mul_local = &mul[2*(y&1)];
    int *sub_local = &sub[2*(y&1)];
    for (int x = 0 ; x < gw; x++) {
      pixel[x] = clampbits(((pixel[x] - sub_local[x&1]) * mul_local[x&1] + 8192) >> 14, 16);
    }
  }
}

#endif


RawImage::RawImage(RawImageData* p) : p_(p) {
  pthread_mutex_lock(&p_->mymutex);
  ++p_->dataRefCount;
  pthread_mutex_unlock(&p_->mymutex);
}

RawImage::RawImage(const RawImage& p) : p_(p.p_) {
  pthread_mutex_lock(&p_->mymutex);
  ++p_->dataRefCount;
  pthread_mutex_unlock(&p_->mymutex);
}

RawImage::~RawImage() {
  pthread_mutex_lock(&p_->mymutex);
  if (--p_->dataRefCount == 0) {
    pthread_mutex_unlock(&p_->mymutex);
    delete p_;
    return;
  }
  pthread_mutex_unlock(&p_->mymutex);
}

RawImageData* RawImage::operator->() {
  return p_;
}

RawImageData& RawImage::operator*() {
  return *p_;
}

RawImage& RawImage::operator=(const RawImage & p) {
  RawImageData* const old = p_;
  p_ = p.p_;
  ++p_->dataRefCount;
  if (--old->dataRefCount == 0) delete old;
  return *this;
}

} // namespace RawSpeed
