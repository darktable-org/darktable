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
interpolation_func_bicubic(float t)
{
  float r;
  t = fabs(t);

  r = (t >= 2.0f) ? 0.0f : ((t > 1.0f) ? (0.5f*(t*(-t*t + 5.0f*t - 8.0f) + 4.0f)) : (0.5f*(t*(3.0f*t*t - 5.0f*t) + 2.0f)));

  return r;
}

#define DT_LANCZOS_EPSILON (1e-9f)

#if 0
float
interpolation_func_lanczos(float width, float t)
{
float ta = fabs(t);

float r = (ta > width) ? 0.0f : ((ta < DT_LANCZOS_EPSILON) ? 1.0f : width*native_sin(M_PI_F*t)*native_sin(M_PI_F*t/width)/(M_PI_F*M_PI_F*t*t));

return r;
}
#else
float
sinf_fast(float t)
{
  const float a = 4.0f/(M_PI_F*M_PI_F);
  const float p = 0.225f;

  t = a*t*(M_PI_F - fabs(t));

  return p*(t*fabs(t) - t) + t;
}

float
interpolation_func_lanczos(float width, float t)
{
  /* Compute a value for sinf(pi.t) in [-pi pi] for which the value will be
   * correct */
  int a = (int)t;
  float r = t - (float)a;

  // Compute the correct sign for sinf(pi.r)
  union { float f; unsigned int i; } sign;
  sign.i = ((a&1)<<31) | 0x3f800000;

  return (DT_LANCZOS_EPSILON + width*sign.f*sinf_fast(M_PI_F*r)*sinf_fast(M_PI_F*t/width))/(DT_LANCZOS_EPSILON + M_PI_F*M_PI_F*t*t);
}
#endif

float4
interpolation_compute_pixel_bilinear_4f(
  read_only image2d_t in,
  const int in_width, const int in_height,
  float2 po)
{
  float4 o = (po.x >=0 && po.y >= 0 && po.x <= in_width-2 && po.y <= in_height-2) ? read_imagef(in, samplerf, po) : (float4)0.0f;

  return o;
}

float4
interpolation_compute_pixel_bicubic_4f(
  read_only image2d_t in,
  const int in_width, const int in_height,
  float2 po)
{
  const int kwidth = 2;

  float4 pixel = (float4)0.0f;
  float weight = 0.0f;

  for(int jj = 1 - kwidth; jj <= kwidth; jj++)
    for(int ii = 1 - kwidth; ii <= kwidth; ii++)
    {
      const int i = po.x + ii;
      const int j = po.y + jj;

      float wx = interpolation_func_bicubic((float)i - po.x);
      float wy = interpolation_func_bicubic((float)j - po.y);
      float w = (i < 0 || j < 0 || i >= in_width || j >= in_height) ? 0.0f : wx * wy;

      pixel += read_imagef(in, sampleri, (int2)(i, j)) * w;
      weight += w;
    }

    pixel = weight > 0.0f ? pixel / weight : (float4)0.0f;

    return pixel;
}

float4
interpolation_compute_pixel_lanczos2_4f(
  read_only image2d_t in,
  const int in_width, const int in_height,
  float2 po)
{
  const int kwidth = 2;

  float4 pixel = (float4)0.0f;
  float weight = 0.0f;

  for(int jj = 1 - kwidth; jj <= kwidth; jj++)
    for(int ii = 1 - kwidth; ii <= kwidth; ii++)
    {
      const int i = po.x + ii;
      const int j = po.y + jj;

      float wx = interpolation_func_lanczos(2, (float)i - po.x);
      float wy = interpolation_func_lanczos(2, (float)j - po.y);
      float w = (i < 0 || j < 0 || i >= in_width || j >= in_height) ? 0.0f : wx * wy;

      pixel += read_imagef(in, sampleri, (int2)(i, j)) * w;
      weight += w;
    }

  pixel = weight > 0.0f ? pixel / weight : (float4)0.0f;

  return pixel;
}

float4
interpolation_compute_pixel_lanczos3_4f(
  read_only image2d_t in,
  const int in_width, const int in_height,
  float2 po)
{
  const int kwidth = 3;

  float4 pixel = (float4)0.0f;
  float weight = 0.0f;

  for(int jj = 1 - kwidth; jj <= kwidth; jj++)
    for(int ii = 1 - kwidth; ii <= kwidth; ii++)
    {
      const int i = po.x + ii;
      const int j = po.y + jj;

      float wx = interpolation_func_lanczos(3, (float)i - po.x);
      float wy = interpolation_func_lanczos(3, (float)j - po.y);
      float w = (i < 0 || j < 0 || i >= in_width || j >= in_height) ? 0.0f : wx * wy;

      pixel += read_imagef(in, sampleri, (int2)(i, j)) * w;
      weight += w;
    }

  pixel = weight > 0.0f ? pixel / weight : (float4)0.0f;

  return pixel;
}
