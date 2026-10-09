/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

#include "develop/masks/probe_image.h"

#include "common/darktable.h"
#include "common/math.h"

#include <math.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// deterministic integer hash noise
//
// everything random-looking in the probe comes from here: an integer hash,
// not rand(), so that the probe is the same on every machine and a
// verification failure reproduces anywhere
// ---------------------------------------------------------------------------

static inline uint32_t _hash_u32(uint32_t x)
{
  // the finalizer of MurmurHash3: good avalanche, no state, no libc
  x ^= x >> 16;
  x *= 0x85ebca6bu;
  x ^= x >> 13;
  x *= 0xc2b2ae35u;
  x ^= x >> 16;
  return x;
}

static inline uint32_t _hash3(const int32_t x, const int32_t y, const uint32_t seed)
{
  return _hash_u32(((uint32_t)x * 0x9e3779b1u) ^ ((uint32_t)y * 0x85ebca77u) ^ (seed * 0xc2b2ae3du));
}

/** hashed value in [0,1) at integer lattice point (x,y) */
static inline float _value_at(const int32_t x, const int32_t y, const uint32_t seed)
{
  return (float)(_hash3(x, y, seed) >> 8) * (1.0f / 16777216.0f);
}

static inline float _smoothstep(const float t)
{
  return t * t * (3.0f - 2.0f * t);
}

/** bilinear value noise with smoothstep interpolation, lattice period `scale` */
static inline float _value_noise(const float x, const float y, const uint32_t seed)
{
  const float fx = floorf(x), fy = floorf(y);
  const int32_t ix = (int32_t)fx, iy = (int32_t)fy;
  const float tx = _smoothstep(x - fx), ty = _smoothstep(y - fy);

  const float v00 = _value_at(ix, iy, seed);
  const float v10 = _value_at(ix + 1, iy, seed);
  const float v01 = _value_at(ix, iy + 1, seed);
  const float v11 = _value_at(ix + 1, iy + 1, seed);

  const float a = v00 + (v10 - v00) * tx;
  const float b = v01 + (v11 - v01) * tx;
  return a + (b - a) * ty;
}

/** fractional Brownian motion: `octaves` doublings of frequency, halvings of
    amplitude, roughly in [0, 1] with a mean near 0.5. It gives the detail mask,
    a wavelet decomposition, something at every scale it looks at, where noise
    of one frequency would fill one band only */
static inline float _fbm(const float x,
                         const float y,
                         const int octaves,
                         const uint32_t seed)
{
  float sum = 0.0f, amp = 0.5f, norm = 0.0f;
  float fx = x, fy = y;
  for(int o = 0; o < octaves; o++)
  {
    sum += amp * _value_noise(fx, fy, seed + (uint32_t)o * 0x1000193u);
    norm += amp;
    amp *= 0.5f;
    fx *= 2.0f;
    fy *= 2.0f;
  }
  return norm > 0.0f ? sum / norm : 0.0f;
}

// ---------------------------------------------------------------------------
// low-discrepancy sequences
//
// to walk blue and the exposure ladder across tiles: unlike a hash, a
// low-discrepancy sequence spreads well over the first few tiles already,
// which is all a small mask covers
// ---------------------------------------------------------------------------

/** van der Corput radical inverse in base 2 */
static inline float _radical_inverse_2(uint32_t n)
{
  n = (n << 16) | (n >> 16);
  n = ((n & 0x55555555u) << 1) | ((n & 0xaaaaaaaau) >> 1);
  n = ((n & 0x33333333u) << 2) | ((n & 0xccccccccu) >> 2);
  n = ((n & 0x0f0f0f0fu) << 4) | ((n & 0xf0f0f0f0u) >> 4);
  n = ((n & 0x00ff00ffu) << 8) | ((n & 0xff00ff00u) >> 8);
  return (float)n * 2.3283064365386963e-10f; // / 2^32
}

/** van der Corput radical inverse in base 3 */
static inline float _radical_inverse_3(uint32_t n)
{
  float inv = 1.0f / 3.0f, r = 0.0f, f = inv;
  while(n)
  {
    r += (float)(n % 3u) * f;
    n /= 3u;
    f *= inv;
  }
  return r;
}

// ---------------------------------------------------------------------------
// the probe
// ---------------------------------------------------------------------------

/** the tile edge in pixels, which trades the two kinds of coverage against
    each other: a tile sweeps red and green, finer when larger, but blue and
    exposure change between tiles only, so a small shape sees fewer slices of
    the cube when tiles are larger. At 16 a window an eighth of the image wide
    spans about seven tiles each way (the local coverage test needs that), and
    the noise fills in a 16-step sweep. Clamped for small images, so that there
    is always more than one tile */
static inline int _tile_size(const int width, const int height)
{
  const int smaller = MIN(width, height);
  int t = 16;
  if(smaller < 8 * t) t = MAX(4, smaller / 8);
  return t;
}

static void _generate(float *const buf, const int width, const int height)
{
  if(!buf || width <= 0 || height <= 0) return;

  const int tile = _tile_size(width, height);
  const int ntx = (width + tile - 1) / tile;

  // the noise lattice is fixed in fractions of the image, not in pixels, so
  // that a probe of any size has the same structure
  const float nscale = 24.0f;

  DT_OMP_FOR()
  for(int y = 0; y < height; y++)
  {
    const int ty = y / tile;
    const float v = (float)(y % tile) / (float)tile; // [0,1) within tile

    for(int x = 0; x < width; x++)
    {
      const int tx = x / tile;
      const float u = (float)(x % tile) / (float)tile;

      const uint32_t n = (uint32_t)(ty * ntx + tx);

      // the base: each tile sweeps a full red and green slice of the linear
      // RGB cube, at a blue level walking a base-2 radical inverse across
      // tiles, so a few neighboring tiles span blue too
      float rgb[3] = { u, v, _radical_inverse_2(n + 1u) };

      // texture at every scale the detail mask looks at, seeded per channel
      // so that it moves through hue too, not only along the neutral axis
      const float nx = (float)x / (float)width * nscale;
      const float ny = (float)y / (float)height * nscale;
      for(int c = 0; c < 3; c++)
        rgb[c] += 0.18f * (_fbm(nx, ny, 5, 0x51ed270bu + (uint32_t)c * 0x9e3779b9u) - 0.5f);

      // hard edges. The tile grid already has some, where red and green jump
      // back to 0, but of one scale, periodic and axis-aligned, and guided
      // filtering is sensitive to edge orientation and spacing. So these cells
      // differ in all three: their sizes are coprime to the tile size, so that
      // their borders never meet the tiles' (do not make them multiples of
      // it), they are offset from the origin, and each is split along a
      // hashed angle. Four levels, so that off-axis edges are about as common
      // as the grid's axis-aligned ones, each factor gentle, as up to four
      // multiply on one pixel
      static const int _cell_bias[4] = { 5, 7, 11, 13 };
      for(int level = 0; level < 4; level++)
      {
        const int cell = tile * (level + 1) + _cell_bias[level]; // coprime to tile
        const int cx = (x + 13 * level) / cell;
        const int cy = (y + 29 * level) / cell;
        const uint32_t h = _hash3(cx, cy, 0xa511e9b3u + (uint32_t)level);

        // half-plane through the cell center, at a hashed angle
        const float angle = (float)(h >> 8) * (DT_2PI_F / 16777216.0f);
        const float dx = (float)x - ((float)cx * cell + cell * 0.5f);
        const float dy = (float)y - ((float)cy * cell + cell * 0.5f);
        if(dx * cosf(angle) + dy * sinf(angle) > 0.0f)
        {
          const float k = (h & 2u) ? 0.6f : 1.5f;
          for(int c = 0; c < 3; c++) rgb[c] *= k;
        }
      }

      // a saturation ladder: the base reaches the cube's corners only at
      // tile corners, where the noise pulls them back to neutral, so the most
      // saturated colors would be missing. A quarter of the tiles are pushed
      // away from their mean, to and past the gamut boundary; the clamp at
      // the end of the loop puts a push past it on the boundary
      const float s2 = _radical_inverse_2(n * 3u + 7u); // decorrelated from blue
      if(s2 > 0.75f)
      {
        const float mean = (rgb[0] + rgb[1] + rgb[2]) / 3.0f;
        const float k = 1.0f + (s2 - 0.75f) * (3.0f / 0.25f); // up to 4x
        for(int c = 0; c < 3; c++) rgb[c] = mean + k * (rgb[c] - mean);
      }

      // an exposure ladder over [-6, +2] EV on half the tiles. Up, because
      // boost factors let a blendif slider select values well above 1 in the
      // scene-referred working space; down, because the base is dark only
      // where all three channels are near 0, at a tile corner, where the noise
      // lifts it again
      const float e3 = _radical_inverse_3(n + 1u);
      if(e3 > 0.5f)
      {
        const float ev = (e3 - 0.5f) * (8.0f / 0.5f) - 6.0f; // [-6, +2] EV
        const float gain = exp2f(ev);
        for(int c = 0; c < 3; c++) rgb[c] *= gain;
      }

      float *const px = buf + ((size_t)y * width + x) * 4;
      for(int c = 0; c < 3; c++) px[c] = MAX(0.0f, rgb[c]);
      px[3] = 0.0f;
    }
  }
}

float *dt_masks_probe_new(const int width, const int height)
{
  if(width <= 0 || height <= 0) return NULL;
  float *const buf = dt_alloc_align_float((size_t)width * height * 4);
  if(!buf) return NULL;
  _generate(buf, width, height);
  return buf;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
