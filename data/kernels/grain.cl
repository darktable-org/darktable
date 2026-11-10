/*
  This file is part of darktable,
  Copyright (C) 2026 darktable developers.

  darktable is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  darktable is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common.h"

// the simplex noise of src/iop/grain.c. Its coordinates reach ~1e6, too
// large for float, but the noise repeats every 768 units along x and y, so
// the host reduces them in double and passes each one as integer + fraction.
// The integer parts are summed as ints here, which keeps the fractions exact.

#define GRAIN_LIGHTNESS_STRENGTH_SCALE 0.15f
#define GRAIN_LUT_SIZE 128

constant int permutation[256]
    = { 151, 160, 137, 91,  90,  15,  131, 13,  201, 95,  96,  53,  194, 233, 7,   225, 140, 36,  103, 30,
        69,  142, 8,   99,  37,  240, 21,  10,  23,  190, 6,   148, 247, 120, 234, 75,  0,   26,  197, 62,
        94,  252, 219, 203, 117, 35,  11,  32,  57,  177, 33,  88,  237, 149, 56,  87,  174, 20,  125, 136,
        171, 168, 68,  175, 74,  165, 71,  134, 139, 48,  27,  166, 77,  146, 158, 231, 83,  111, 229, 122,
        60,  211, 133, 230, 220, 105, 92,  41,  55,  46,  245, 40,  244, 102, 143, 54,  65,  25,  63,  161,
        1,   216, 80,  73,  209, 76,  132, 187, 208, 89,  18,  169, 200, 196, 135, 130, 116, 188, 159, 86,
        164, 100, 109, 198, 173, 186, 3,   64,  52,  217, 226, 250, 124, 123, 5,   202, 38,  147, 118, 126,
        255, 82,  85,  212, 207, 206, 59,  227, 47,  16,  58,  17,  182, 189, 28,  42,  223, 183, 170, 213,
        119, 248, 152, 2,   44,  154, 163, 70,  221, 153, 101, 155, 167, 43,  172, 9,   129, 22,  39,  253,
        19,  98,  108, 110, 79,  113, 224, 232, 178, 185, 112, 104, 218, 246, 97,  228, 251, 34,  242, 193,
        238, 210, 144, 12,  191, 179, 162, 241, 81,  51,  145, 235, 249, 14,  239, 107, 49,  192, 214, 31,
        181, 199, 106, 157, 184, 84,  204, 176, 115, 121, 50,  45,  127, 4,   150, 254, 138, 236, 205, 93,
        222, 114, 67,  29,  24,  72,  243, 141, 128, 195, 78,  66,  215, 61,  156, 180 };

constant float grad3[12][3]
  = { { 1, 1, 0 }, { -1, 1, 0 }, { 1, -1, 0 }, { -1, -1, 0 },
      { 1, 0, 1 }, { -1, 0, 1 }, { 1, 0, -1 }, { -1, 0, -1 },
      { 0, 1, 1 }, { 0, -1, 1 }, { 0, 1, -1 }, { 0, -1, -1 } };

static inline int _perm(const int i)
{
  return permutation[i & 255];
}

static inline float _corner(const int gi, const float x, const float y, const float z)
{
  const float t = 0.6f - x * x - y * y - z * z;
  if(t < 0.0f) return 0.0f;
  const float t2 = t * t;
  return t2 * t2 * (grad3[gi][0] * x + grad3[gi][1] * y + grad3[gi][2] * z);
}

// _simplex_noise() of grain.c for xin = xi + xf, yin = yi + yf, zin = zi,
// with xi, yi, zi >= 0 and xf, yf in [0, 1)
static float _simplex_noise(const int xi, const float xf,
                            const int yi, const float yf,
                            const int zi)
{
  // s = (xin + yin + zin) / 3, split into q + sf
  const int sum = xi + yi + zi;
  const int q = sum / 3;
  const float sf = ((float)(sum - 3 * q) + xf + yf) / 3.0f;
  const int i = xi + q + (int)floor(xf + sf);
  const int j = yi + q + (int)floor(yf + sf);
  const int k = zi + q + (int)floor(sf);

  // t = (i + j + k) / 6, split into t6 + tf
  const int tsum = i + j + k;
  const int t6 = tsum / 6;
  const float tf = (float)(tsum - 6 * t6) / 6.0f;
  const float x0 = (float)(xi + t6 - i) + xf + tf;
  const float y0 = (float)(yi + t6 - j) + yf + tf;
  const float z0 = (float)(zi + t6 - k) + tf;

  int i1, j1, k1, i2, j2, k2;
  if(x0 >= y0)
  {
    if(y0 >= z0)      { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
    else if(x0 >= z0) { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 0; k2 = 1; }
    else              { i1 = 0; j1 = 0; k1 = 1; i2 = 1; j2 = 0; k2 = 1; }
  }
  else
  {
    if(y0 < z0)       { i1 = 0; j1 = 0; k1 = 1; i2 = 0; j2 = 1; k2 = 1; }
    else if(x0 < z0)  { i1 = 0; j1 = 1; k1 = 0; i2 = 0; j2 = 1; k2 = 1; }
    else              { i1 = 0; j1 = 1; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
  }

  const float G3 = 1.0f / 6.0f;
  const float x1 = x0 - i1 + G3;
  const float y1 = y0 - j1 + G3;
  const float z1 = z0 - k1 + G3;
  const float x2 = x0 - i2 + 2.0f * G3;
  const float y2 = y0 - j2 + 2.0f * G3;
  const float z2 = z0 - k2 + 2.0f * G3;
  const float x3 = x0 - 1.0f + 3.0f * G3;
  const float y3 = y0 - 1.0f + 3.0f * G3;
  const float z3 = z0 - 1.0f + 3.0f * G3;

  const int ii = i & 255;
  const int jj = j & 255;
  const int kk = k & 255;
  const int gi0 = _perm(ii + _perm(jj + _perm(kk))) % 12;
  const int gi1 = _perm(ii + i1 + _perm(jj + j1 + _perm(kk + k1))) % 12;
  const int gi2 = _perm(ii + i2 + _perm(jj + j2 + _perm(kk + k2))) % 12;
  const int gi3 = _perm(ii + 1 + _perm(jj + 1 + _perm(kk + 1))) % 12;

  return 32.0f * (_corner(gi0, x0, y0, z0) + _corner(gi1, x1, y1, z1)
                  + _corner(gi2, x2, y2, z2) + _corner(gi3, x3, y3, z3));
}

// a + b of two reduced coordinates, as integer part and fraction in [0, 1)
static inline void _add_coord(const float2 a, const float2 b, int *ci, float *cf)
{
  float f = a.y + b.y;
  int n = (int)a.x + (int)b.x;
  if(f >= 1.0f)
  {
    f -= 1.0f;
    n++;
  }
  *ci = n;
  *cf = f;
}

static float _lut_lookup(global const float *grain_lut, const float x, const float y)
{
  const float _x = clamp((x + 0.5f) * (GRAIN_LUT_SIZE - 1), 0.0f, (float)(GRAIN_LUT_SIZE - 1));
  const float _y = clamp(y * (GRAIN_LUT_SIZE - 1), 0.0f, (float)(GRAIN_LUT_SIZE - 1));

  const int _x0 = _x < GRAIN_LUT_SIZE - 2 ? _x : GRAIN_LUT_SIZE - 2;
  const int _y0 = _y < GRAIN_LUT_SIZE - 2 ? _y : GRAIN_LUT_SIZE - 2;
  const int _x1 = _x0 + 1;
  const int _y1 = _y0 + 1;

  const float x_diff = _x - _x0;
  const float y_diff = _y - _y0;

  const float l00 = grain_lut[_y0 * GRAIN_LUT_SIZE + _x0];
  const float l01 = grain_lut[_y0 * GRAIN_LUT_SIZE + _x1];
  const float l10 = grain_lut[_y1 * GRAIN_LUT_SIZE + _x0];
  const float l11 = grain_lut[_y1 * GRAIN_LUT_SIZE + _x1];

  const float xy0 = (1.0f - y_diff) * l00 + l10 * y_diff;
  const float xy1 = (1.0f - y_diff) * l01 + l11 * y_diff;
  return xy0 * (1.0f - x_diff) + xy1 * x_diff;
}

// cols[3 * x + octave] and rows[3 * y + octave] hold the reduced pixel
// coordinates, xoffs[3 * l + octave] and yoffs[3 * l + octave] those of the
// sample offsets (hash included), each as (integer part, fraction)
kernel void
grain(read_only image2d_t in,
      write_only image2d_t out,
      const int width,
      const int height,
      global const float2 *cols,
      global const float2 *rows,
      global const float2 *xoffs,
      global const float2 *yoffs,
      const int samples,
      const float strength,
      global const float *grain_lut)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float4 pixel = readpixel(in, x, y);

  // weights of the three octaves, matched to scans of real grain
  const float a[3] = { 0.2340f, 0.7850f, 1.2150f };
  const float wsample = samples > 1 ? 1.0f / samples : 1.0f;

  float noise = 0.0f;
  for(int l = 0; l < samples; l++)
  {
    float total = 0.0f;
    for(int o = 0; o < 3; o++)
    {
      int xi, yi;
      float xf, yf;
      _add_coord(cols[3 * x + o], xoffs[3 * l + o], &xi, &xf);
      _add_coord(rows[3 * y + o], yoffs[3 * l + o], &yi, &yf);
      total += _simplex_noise(xi, xf, yi, yf, o) * a[o];
    }
    noise += wsample * total;
  }

  const float delta = _lut_lookup(grain_lut, noise * strength * GRAIN_LIGHTNESS_STRENGTH_SCALE,
                                  pixel.x / 100.0f);
  write_imagef(out, (int2)(x, y), (float4)(pixel.x + delta, pixel.y, pixel.z, pixel.w));
}
