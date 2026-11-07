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

// LMMSE demosaic (src/iop/demosaicing/lmmse.c) as whole-image passes.
//
// The CPU code works on 136x136 tiles, the image plus a 4 pixel border.
// Here every buffer covers the image plus that border (pw x ph, the image at
// an offset of 4), so that the passes see the same neighbors as a tile at
// the image edges, and keep the same margins to the padded edge. The border
// holds zeros where the CPU tile buffer is never written.
//
// None of the CPU loops reads a value that the same loop writes, so each one
// is a single pass. The only difference to the CPU is at the seams between
// its tiles, where the CPU passes run into the unwritten tile border; here
// they read the neighboring image data.

#include "common.h"

bool _area(int col, int row, int m, int pw, int ph)
{
  return col >= m && col < pw - m && row >= m && row < ph - m;
}

// the CPU gamma tables of 65536 entries, which clip to 0 and 1
float _gamma_in(float x)
{
  if(x < 0.0f) return 0.0f;
  if(x * 65535.0f > 65534.99f) return 1.0f;
  return x <= 0.001867f ? x * 17.0f : 1.044445f * pow(x, 1.0f / 2.4f) - 0.044445f;
}

float _gamma_out(float x)
{
  if(x < 0.0f) return 0.0f;
  if(x * 65535.0f > 65534.99f) return 1.0f;
  return x <= 0.031746f ? x / 17.0f : pow((x + 0.044445f) / 1.044445f, 2.4f);
}

float _median3(float x0, float x1, float x2)
{
  return max(min(x0, x1), min(x2, max(x0, x1)));
}

void _cas(float *a, float *b)
{
  const float x = *a;
  const bool c = *a > *b;
  *a = c ? *b : *a;
  *b = c ? x : *b;
}

float _median9(float *s)
{
  _cas(&s[1], &s[2]);
  _cas(&s[4], &s[5]);
  _cas(&s[7], &s[8]);
  _cas(&s[0], &s[1]);
  _cas(&s[3], &s[4]);
  _cas(&s[6], &s[7]);
  _cas(&s[1], &s[2]);
  _cas(&s[4], &s[5]);
  _cas(&s[7], &s[8]);
  _cas(&s[0], &s[3]);
  _cas(&s[5], &s[8]);
  _cas(&s[4], &s[7]);
  _cas(&s[3], &s[6]);
  _cas(&s[1], &s[4]);
  _cas(&s[2], &s[5]);
  _cas(&s[4], &s[7]);
  _cas(&s[4], &s[2]);
  _cas(&s[6], &s[4]);
  _cas(&s[4], &s[2]);
  return s[4];
}

// the gamma corrected raw data
kernel void lmmse_init(
  read_only image2d_t in, global float *cfa,
                const int width, const int height, const int pw, const int ph,
                const float revscaler)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= pw || row >= ph) return;
  const int x = col - 4;
  const int y = row - 4;
  const bool inside = x >= 0 && x < width && y >= 0 && y < height;
  cfa[row * pw + col] = inside ? _gamma_in(revscaler * Areadsingle(in, x, y)) : 0.0f;
}

// G-R(B), horizontal and vertical
kernel void lmmse_diff(
  global float *cfa, global float *hdiff, global float *vdiff,
                const int pw, const int ph, const unsigned int filters)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= pw || row >= ph) return;
  const int i = row * pw + col;
  if(!_area(col, row, 2, pw, ph))
  {
    hdiff[i] = 0.0f;
    vdiff[i] = 0.0f;
    return;
  }
  const int w1 = pw, w2 = 2 * pw;
  const float c0 = cfa[i];
  float h, v;
  if((FC(row, col, filters) & 1) == 0)
  { // at R(B)
    const float v0 = 0.0625f * (cfa[i - w1 - 1] + cfa[i - w1 + 1] + cfa[i + w1 - 1] + cfa[i + w1 + 1]) + 0.25f * c0;
    h = -0.25f * (cfa[i - 2] + cfa[i + 2]) + 0.5f * (cfa[i - 1] + c0 + cfa[i + 1]);
    const float Y0 = v0 + 0.5f * h;
    h = (c0 > 1.75f * Y0) ? _median3(h, cfa[i - 1], cfa[i + 1]) : clamp(h, 0.0f, 1.0f);
    h -= c0;

    v = -0.25f * (cfa[i - w2] + cfa[i + w2]) + 0.5f * (cfa[i - w1] + c0 + cfa[i + w1]);
    const float Y1 = v0 + 0.5f * v;
    v = (c0 > 1.75f * Y1) ? _median3(v, cfa[i - w1], cfa[i + w1]) : clamp(v, 0.0f, 1.0f);
    v -= c0;
  }
  else
  { // at G
    h = 0.25f * (cfa[i - 2] + cfa[i + 2]) - 0.5f * (cfa[i - 1] + c0 + cfa[i + 1]);
    v = 0.25f * (cfa[i - w2] + cfa[i + w2]) - 0.5f * (cfa[i - w1] + c0 + cfa[i + w1]);
    h = clamp(h, -1.0f, 0.0f) + c0;
    v = clamp(v, -1.0f, 0.0f) + c0;
  }
  hdiff[i] = h;
  vdiff[i] = v;
}

// low pass filter on the color differences
kernel void lmmse_lowpass(
  global float *hdiff, global float *vdiff, global float *hlp, global float *vlp,
                   const int pw, const int ph,
                   const float h0, const float h1, const float h2, const float h3, const float h4)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= pw || row >= ph) return;
  const int i = row * pw + col;
  if(!_area(col, row, 4, pw, ph))
  {
    hlp[i] = 0.0f;
    vlp[i] = 0.0f;
    return;
  }
  const int w1 = pw, w2 = 2 * pw, w3 = 3 * pw, w4 = 4 * pw;
  hlp[i] = h0 * hdiff[i] + h1 * (hdiff[i - 1] + hdiff[i + 1]) + h2 * (hdiff[i - 2] + hdiff[i + 2])
         + h3 * (hdiff[i - 3] + hdiff[i + 3]) + h4 * (hdiff[i - 4] + hdiff[i + 4]);
  vlp[i] = h0 * vdiff[i] + h1 * (vdiff[i - w1] + vdiff[i + w1]) + h2 * (vdiff[i - w2] + vdiff[i + w2])
         + h3 * (vdiff[i - w3] + vdiff[i + w3]) + h4 * (vdiff[i - w4] + vdiff[i + w4]);
}

// the LMMSE estimate along one direction, d is 1 or the row pitch
void _estimate(global float *diff, global float *lp, int i, int d, float *x, float *var)
{
  float p[9];
  float mu = 0.0f;
  for(int k = 0; k < 9; k++)
  {
    p[k] = lp[i + (k - 4) * d];
    mu += p[k];
  }
  mu /= 9.0f;
  float vx = 1e-7f;
  for(int k = 0; k < 9; k++) vx += fsquare(p[k] - mu);
  float vn = 1e-7f;
  for(int k = 0; k < 9; k++) vn += fsquare(p[k] - diff[i + (k - 4) * d]);
  *x = (diff[i] * vx + lp[i] * vn) / (vx + vn);
  *var = vx * vn / (vx + vn);
}

// interpolated G-R(B) at R(B)
kernel void lmmse_interp(
  global float *hdiff, global float *vdiff, global float *hlp, global float *vlp, global float *interp,
                  const int pw, const int ph, const unsigned int filters)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= pw || row >= ph) return;
  const int i = row * pw + col;
  if(!_area(col, row, 4, pw, ph) || (FC(row, col, filters) & 1) != 0)
  {
    interp[i] = 0.0f;
    return;
  }
  float xh, vh, xv, vv;
  _estimate(hdiff, hlp, i, 1, &xh, &vh);
  _estimate(vdiff, vlp, i, pw, &xv, &vv);
  interp[i] = (xh * vv + xv * vh) / (vh + vv);
}

// the CFA values into their planes, and green at R/B. The planes take the
// buffers of hdiff, vdiff and hlp, which hold zeros at the padded edge
kernel void lmmse_planes(
  global float *cfa, global float *interp, global float *red, global float *green, global float *blue,
                  const int width, const int height, const int pw, const int ph, const unsigned int filters)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= pw || row >= ph) return;
  const int i = row * pw + col;
  const int x = col - 4;
  const int y = row - 4;
  const bool inside = x >= 0 && x < width && y >= 0 && y < height;
  const int c = FC(row, col, filters);
  const float v = inside ? cfa[i] : 0.0f;
  if(c == 1)
    green[i] = v;
  else
  {
    green[i] = inside ? v + interp[i] : 0.0f;
    if(c == 0) red[i] = v;
    else blue[i] = v;
  }
}

// bilinear R/B at G: h from its row, v from its column
void _rb_at_g(global float *h, global float *v, global float *green, int i, int w1)
{
  const float g = green[i];
  h[i] = g + 0.5f * (h[i - 1] - green[i - 1] + h[i + 1] - green[i + 1]);
  v[i] = g + 0.5f * (v[i - w1] - green[i - w1] + v[i + w1] - green[i + w1]);
}

kernel void lmmse_rb_at_g(
  global float *red, global float *green, global float *blue,
                   const int pw, const int ph, const unsigned int filters)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(!_area(col, row, 1, pw, ph) || (FC(row, col, filters) & 1) == 0) return;
  const int i = row * pw + col;
  if(FC(row, col + 1, filters) == 0)
    _rb_at_g(red, blue, green, i, pw);
  else
    _rb_at_g(blue, red, green, i, pw);
}

// bilinear B at R and R at B
float _rb_at_rb(global float *o, global float *green, int i, int w1)
{
  return green[i] + 0.25f * (o[i - w1] - green[i - w1] + o[i - 1] - green[i - 1]
                             + o[i + 1] - green[i + 1] + o[i + w1] - green[i + w1]);
}

kernel void lmmse_rb_at_rb(
  global float *red, global float *green, global float *blue,
                    const int pw, const int ph, const unsigned int filters)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(!_area(col, row, 1, pw, ph)) return;
  const int c = FC(row, col, filters);
  if((c & 1) != 0) return;
  const int i = row * pw + col;
  if(c == 0) blue[i] = _rb_at_rb(blue, green, i, pw);
  else red[i] = _rb_at_rb(red, green, i, pw);
}

// 3x3 medians of R-G and B-G
kernel void lmmse_median(
  global float *red, global float *green, global float *blue, global float *corr_r, global float *corr_b,
                  const int pw, const int ph)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(!_area(col, row, 1, pw, ph)) return;
  const int i = row * pw + col;
  float r[9], b[9];
  for(int dy = -1, k = 0; dy <= 1; dy++)
    for(int dx = -1; dx <= 1; dx++, k++)
    {
      const int j = i + dy * pw + dx;
      r[k] = red[j] - green[j];
      b[k] = blue[j] - green[j];
    }
  corr_r[i] = _median9(r);
  corr_b[i] = _median9(b);
}

// the colors from the medians. The CPU loop stops one row early at the
// bottom, kept as is
kernel void lmmse_median_apply(
  global float *red, global float *green, global float *blue, global float *corr_r, global float *corr_b,
                        const int pw, const int ph, const unsigned int filters)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col < 6 || col >= pw - 6 || row < 6 || row >= ph - 7) return;
  const int i = row * pw + col;
  const int c = FC(row, col, filters);
  const float g = green[i];
  if((c & 1) != 0)
  {
    red[i] = g + corr_r[i];
    blue[i] = g + corr_b[i];
  }
  else if(c == 0)
  {
    const float b = g + corr_b[i];
    blue[i] = b;
    green[i] = 0.5f * (red[i] - corr_r[i] + b - corr_b[i]);
  }
  else
  {
    const float r = g + corr_r[i];
    red[i] = r;
    green[i] = 0.5f * (r - corr_r[i] + blue[i] - corr_b[i]);
  }
}

// the CFA values back into their planes
kernel void lmmse_fill(
  global float *cfa, global float *red, global float *green, global float *blue,
                const int pw, const int ph, const unsigned int filters)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(!_area(col, row, 4, pw, ph)) return;
  const int i = row * pw + col;
  const int c = FC(row, col, filters);
  if(c == 0) red[i] = cfa[i];
  else if(c == 2) blue[i] = cfa[i];
  else green[i] = cfa[i];
}

// the refinement: the weights from the gradients of a at i-2..i+2 and of b
// at i-1..i+1, horizontally and vertically, applied to p - green
float _refine(global float *p, global float *a, global float *b, global float *green, int i, int w1)
{
  const int w2 = 2 * w1;
  const float dL = 1.0f / (1.0f + fabs(a[i - 2] - a[i]) + fabs(b[i + 1] - b[i - 1]));
  const float dR = 1.0f / (1.0f + fabs(a[i + 2] - a[i]) + fabs(b[i + 1] - b[i - 1]));
  const float dU = 1.0f / (1.0f + fabs(a[i - w2] - a[i]) + fabs(b[i + w1] - b[i - w1]));
  const float dD = 1.0f / (1.0f + fabs(a[i + w2] - a[i]) + fabs(b[i + w1] - b[i - w1]));
  return ((green[i - 1] - p[i - 1]) * dL + (green[i + 1] - p[i + 1]) * dR
          + (green[i - w1] - p[i - w1]) * dU + (green[i + w1] - p[i + w1]) * dD)
         / (dL + dR + dU + dD);
}

// green at R/B
kernel void lmmse_refine_g(
  global float *red, global float *green, global float *blue,
                    const int pw, const int ph, const unsigned int filters)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(!_area(col, row, 8, pw, ph)) return;
  const int c = FC(row, col, filters);
  if((c & 1) != 0) return;
  const int i = row * pw + col;
  if(c == 0) green[i] = red[i] + _refine(red, red, green, green, i, pw);
  else green[i] = blue[i] + _refine(blue, blue, green, green, i, pw);
}

// R and B at G
kernel void lmmse_refine_rb_at_g(
  global float *red, global float *green, global float *blue,
                          const int pw, const int ph, const unsigned int filters)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(!_area(col, row, 8, pw, ph) || (FC(row, col, filters) & 1) == 0) return;
  const int i = row * pw + col;
  const float g = green[i];
  const float r = g - _refine(red, green, red, green, i, pw);
  const float b = g - _refine(blue, green, blue, green, i, pw);
  red[i] = r;
  blue[i] = b;
}

// B at R and R at B
kernel void lmmse_refine_rb_at_rb(
  global float *red, global float *green, global float *blue,
                           const int pw, const int ph, const unsigned int filters)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(!_area(col, row, 8, pw, ph)) return;
  const int c = FC(row, col, filters);
  if((c & 1) != 0) return;
  const int i = row * pw + col;
  if(c == 0) blue[i] = green[i] - _refine(blue, red, green, green, i, pw);
  else red[i] = green[i] - _refine(red, blue, green, green, i, pw);
}

// gamma back and the output, inside the 4 pixel border that PPG fills
kernel void lmmse_output(
  global float *red, global float *green, global float *blue, write_only image2d_t out,
                  const int width, const int height, const int pw, const float scaler)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x < 4 || x >= width - 4 || y < 4 || y >= height - 4) return;
  const int i = (y + 4) * pw + x + 4;
  write_imagef(out, (int2)(x, y), (float4)(max(0.0f, scaler * _gamma_out(red[i])),
                               max(0.0f, scaler * _gamma_out(green[i])),
                               max(0.0f, scaler * _gamma_out(blue[i])), 0.0f));
}
