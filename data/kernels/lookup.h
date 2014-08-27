/*
 *  This file is part of darktable,
 *  copyright (c) 2009--2013 johannes hanika.
 *  copyright (c) 2014 Ulrich Pegelow.
 *  copyright (c) 2014 LebedevRI.
 *
 *  darktable is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  darktable is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

float
lookup_unbounded(read_only image2d_t lut, const float x, global const float *a)
{
  // in case the tone curve is marked as linear, return the fast
  // path to linear unbounded (does not clip x at 1)
  if(a[0] >= 0.0f)
  {
    if(x < 1.0f/a[0])
    {
      const int xi = clamp(x*65535.0f, 0.0f, 65535.0f);
      const int2 p = (int2)((xi & 0xff), (xi >> 8));
      return read_imagef(lut, sampleri, p).x;
    }
    else return a[1] * native_powr(x*a[0], a[2]);
  }
  else return x;
}

float
lookup_unbounded_twosided(read_only image2d_t lut, const float x, global const float *a)
{
  // in case the tone curve is marked as linear, return the fast
  // path to linear unbounded (does not clip x at 1)
  if(a[0] >= 0.0f)
  {
    const float ar = 1.0f/a[0];
    const float al = 1.0f - 1.0f/a[3];
    if(x < ar && x >= al)
    {
      // lut lookup
      const int xi = clamp(x*65535.0f, 0.0f, 65535.0f);
      const int2 p = (int2)((xi & 0xff), (xi >> 8));
      return read_imagef(lut, sampleri, p).x;
    }
    else
    {
      // two-sided extrapolation (with inverted x-axis for left side)
      const float xx = (x >= ar) ? x : 1.0f - x;
      global const float *aa = (x >= ar) ? a : a + 3;
      return aa[1] * native_powr(xx*aa[0], aa[2]);
    }
  }
  else return x;
}

float
lookup(read_only image2d_t lut, const float x)
{
  int xi = clamp(x*65535.0f, 0.0f, 65535.0f);
  int2 p = (int2)((xi & 0xff), (xi >> 8));
  return read_imagef(lut, sampleri, p).x;
}
