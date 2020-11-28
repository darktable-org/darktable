/*
    This file is part of darktable,
    Copyright (C) 2009-2020 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <assert.h>
#include <math.h>
#include <stdlib.h>
//#include <string.h>

#include "common/darktable.h"
#include "common/box_filters.h"
#if defined(__SSE__)
#include <xmmintrin.h>
#endif

static void blur_horizontal_1ch(float *buf, const int height, const int width, const int radius, float *scanlines)
{
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(radius, height, width, scanlines)   \
  shared(buf) \
  schedule(static)
#endif
  for(int y = 0; y < height; y++)
  {
    float L = 0;
    int hits = 0;
    size_t index = (size_t)y * width;
    float *scanline = scanlines + dt_get_thread_num() * width;
    // add up the left half of the window
    for (int x = 0; x < radius && x < width ; x++)
    {
      L += buf[index+x];
      hits++;
    }
    // process the blur up to the point where we start removing values
    int x;
    for (x = 0; x <= radius && x < width; x++)
    {
      const int np = x + radius;
      if(np < width)
      {
        L += buf[index + np];
        hits++;
      }
      scanline[x] = L / hits;
    }
    // process the blur for the bulk of the scan line
    for(; x + radius < width; x++)
    {
      const int op = x - radius - 1;
      const int np = x + radius;
      L -= buf[index + op];
      L += buf[index + np];
      scanline[x] = L / hits;
    }
    // process the right end where we have no more values to add to the running sum
    for(; x < width; x++)
    {
      const int op = x - radius - 1;
      L -= buf[index + op];
      hits--;
      scanline[x] = L / hits;
    }
    // copy blurred values back to original location in buffer
    for(x = 0; x < width; x++)
      buf[index + x] = scanline[x];
  }
  return;
}

#ifdef __SSE2__
static void blur_vertical_1ch_sse(float *buf, const int height, const int width, const int radius, float *scanline)
{
  __m128 L = { 0, 0, 0, 0 };
  __m128 hits = { 0, 0, 0, 0 };
  const __m128 one = { 1.0f, 1.0f, 1.0f, 1.0f };
  // add up the top half of the window
  for (size_t y = 0; y < radius && y < height; y++)
  {
    size_t index = y * width;
    L += _mm_loadu_ps(buf + index);	// use unaligned load since width is not necessarily a multiple of 4
    hits += one;
  }
  // process the blur up to the point where we start removing values
  for (size_t y = 0; y <= radius && y < height; y++)
  {
    const int np = y + radius;
    if(np < height)
    {
      L += _mm_loadu_ps(buf+np*width);
      hits += one;
    }
    _mm_store_ps(scanline+4*y, L / hits);
  }
  // process the blur for the rest of the scan line
  for (size_t y = radius+1 ; y < height; y++)
  {
    const int op = y - radius - 1;
    const int np = y + radius;
    L -= _mm_loadu_ps(buf+op*width);
    hits -= one;
    if(np < height)
    {
      L += _mm_loadu_ps(buf+np*width);
      hits += one;
    }
    _mm_store_ps(scanline+4*y, L / hits);
  }

  // copy blurred values back to original location in buffer
  for (size_t y = 0; y < height; y++)
  {
    // use unaligned store since width is not necessarily a multiple of four
    // use the faster aligned load since we've ensured that scanline is aligned
    _mm_storeu_ps(buf + y*width, _mm_load_ps(scanline + 4*y));
  }
  return;
}
#endif /* __SSE2__ */

// invoked inside an OpenMP parallel for, so no need to parallelize
static void blur_vertical_4wide(float *buf, const int height, const int width, const int radius, float *scanline)
{
#ifdef __SSE2__
  if (darktable.codepath.SSE2)
  {
    blur_vertical_1ch_sse(buf, height, width, radius, scanline);
    return;
  }
#endif /* __SSE2__ */

  float L[4] = { 0, 0, 0, 0 };
  int hits = 0;
  // add up the left half of the window
  for (size_t y = 0; y < radius && y < height; y++)
  {
    size_t index = y * width;
    L[0] += buf[index];
    L[1] += buf[index + 1];
    L[2] += buf[index + 2];
    L[3] += buf[index + 3];
    hits++;
  }
  // process the blur up to the point where we start removing values
  for (size_t y = 0; y <= radius && y < height; y++)
  {
    const int np = y + radius;
    const int npoffset = np * width;
    if(np < height)
    {
      L[0] += buf[npoffset];
      L[1] += buf[npoffset+1];
      L[2] += buf[npoffset+2];
      L[3] += buf[npoffset+3];
      hits++;
    }
    scanline[4*y] = L[0] / hits;
    scanline[4*y+1] = L[1] / hits;
    scanline[4*y+2] = L[2] / hits;
    scanline[4*y+3] = L[3] / hits;
  }
  // process the blur for the rest of the scan line
  for (size_t y = radius+1; y < height; y++)
  {
    const int op = y - radius - 1;
    const int np = y + radius;
    const int opoffset = op * width;
    const int npoffset = np * width;
    if(op >= 0)
    {
      L[0] -= buf[opoffset];
      L[1] -= buf[opoffset+1];
      L[2] -= buf[opoffset+2];
      L[3] -= buf[opoffset+3];
      hits--;
    }
    if(np < height)
    {
      L[0] += buf[npoffset];
      L[1] += buf[npoffset+1];
      L[2] += buf[npoffset+2];
      L[3] += buf[npoffset+3];
      hits++;
    }
    scanline[4*y] = L[0] / hits;
    scanline[4*y+1] = L[1] / hits;
    scanline[4*y+2] = L[2] / hits;
    scanline[4*y+3] = L[3] / hits;
  }

  // copy blurred values back to original location in buffer
  for (size_t y = 0; y < height; y++)
  {
    buf[y * width] = scanline[4*y];
    buf[y * width + 1] = scanline[4*y+1];
    buf[y * width + 2] = scanline[4*y+2];
    buf[y * width + 3] = scanline[4*y+3];
  }
  return;
}

static void blur_vertical_1ch(float *buf, const int height, const int width, const int radius, float *scanlines)
{
  /* vertical pass on L channel */
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(radius, height, width, scanlines)   \
  shared(buf) \
  schedule(static)
#endif
  for(int x = 0; x < width-4; x += 4)
  {
    float *scanline = scanlines + 4 * dt_get_thread_num() * height;
    blur_vertical_4wide(buf + x, height, width, radius, scanline);
  }
  const int opoffs = -(radius + 1) * width;
  const int npoffs = radius*width;
  for(int x = 4*(width/4); x < width; x++)
  {
    float L = 0;
    int hits = 0;
    size_t index = (size_t)x - radius * width;
    float *scanline = scanlines + 4 * dt_get_thread_num() * height;
    for(int y = -radius; y < height; y++)
    {
      const int op = y - radius - 1;
      const int np = y + radius;
      if(op >= 0)
      {
        L -= buf[index + opoffs];
        hits--;
      }
      if(np < height)
      {
        L += buf[index + npoffs];
        hits++;
      }
      if(y >= 0) scanline[y] = L / hits;
      index += width;
    }

    for(int y = 0; y < height; y++)
      buf[(size_t)y * width + x] = scanline[y];
  }
  return;
}

void dt_box_mean(float *const buf, const int height, const int width, const int ch,
                 const int radius, const int iterations)
{
  assert(ch == 1); //TODO: implement 4ch version
  const int size = width > height ? width : height;
  float *scanlines = dt_alloc_align(64, 4 * size * sizeof(float) * dt_get_num_threads());

  for(int iteration = 0; iteration < iterations; iteration++)
  {
    blur_horizontal_1ch(buf, height, width, radius, scanlines);
    blur_vertical_1ch(buf, height, width, radius, scanlines);
  }

  dt_free_align(scanlines);
}
