/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.
    copyright (c) 2011 Henrik Andersson.
    copyright (c) 2012 tobias ellinghaus.
    copyright (c) 2016 Roman Lebedev.

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

#include "develop/imageop_math.h"
#include <assert.h> // for assert
#ifdef __SSE__
#include <emmintrin.h> // for _mm_set_epi32, _mm_add_epi32
#endif
#include <glib.h> // for MIN, MAX, CLAMP, inline
#include <math.h> // for round, floorf, fmaxf
#ifdef __SSE__
#include <xmmintrin.h> // for _mm_set_ps, _mm_mul_ps, _mm_set...
#endif
#include "common/darktable.h"        // for darktable, darktable_t, dt_code...
#include "common/imageio.h"          // for FILTERS_ARE_4BAYER
#include "common/interpolation.h"    // for dt_interpolation_new, dt_interp...
#include "develop/imageop.h"         // for dt_iop_roi_t

void dt_iop_flip_and_zoom_8(const uint8_t *in, int32_t iw, int32_t ih, uint8_t *out, int32_t ow, int32_t oh,
                            const dt_image_orientation_t orientation, uint32_t *width, uint32_t *height)
{
  // init strides:
  const uint32_t iwd = (orientation & ORIENTATION_SWAP_XY) ? ih : iw;
  const uint32_t iht = (orientation & ORIENTATION_SWAP_XY) ? iw : ih;
  // DO NOT UPSCALE !!!
  const float scale = fmaxf(1.0, fmaxf(iwd / (float)ow, iht / (float)oh));
  const uint32_t wd = *width = MIN(ow, iwd / scale);
  const uint32_t ht = *height = MIN(oh, iht / scale);
  const int bpp = 4; // bytes per pixel
  int32_t ii = 0, jj = 0;
  int32_t si = 1, sj = iw;
  if(orientation & ORIENTATION_FLIP_X)
  {
    jj = ih - jj - 1;
    sj = -sj;
  }
  if(orientation & ORIENTATION_FLIP_Y)
  {
    ii = iw - ii - 1;
    si = -si;
  }
  if(orientation & ORIENTATION_SWAP_XY)
  {
    int t = sj;
    sj = si;
    si = t;
  }
  const int32_t half_pixel = .5f * scale;
  const int32_t offm = half_pixel * bpp * MIN(MIN(0, si), MIN(sj, si + sj));
  const int32_t offM = half_pixel * bpp * MAX(MAX(0, si), MAX(sj, si + sj));
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(in, out, jj, ii, sj, si, iw, ih)
#endif
  for(uint32_t j = 0; j < ht; j++)
  {
    uint8_t *out2 = out + bpp * wd * j;
    const uint8_t *in2 = in + bpp * (iw * jj + ii + sj * (int32_t)(scale * j));
    float stepi = 0.0f;
    for(uint32_t i = 0; i < wd; i++)
    {
      const uint8_t *in3 = in2 + ((int32_t)stepi) * si * bpp;
      // this should always be within the bounds of in[], due to the way
      // wd/ht are constructed by always just rounding down. half_pixel should never
      // add up to one pixel difference.
      // we have this check with the hope the branch predictor will get rid of it:
      if(in3 + offm >= in && in3 + offM < in + bpp * iw * ih)
      {
        for(int k = 0; k < 3; k++)
          out2[k] = // in3[k];
              CLAMP(((int32_t)in3[bpp * half_pixel * sj + k] + (int32_t)in3[bpp * half_pixel * (si + sj) + k]
                     + (int32_t)in3[bpp * half_pixel * si + k] + (int32_t)in3[k])
                        / 4,
                    0, 255);
      }
      out2 += bpp;
      stepi += scale;
    }
  }
}

void dt_iop_clip_and_zoom_8(const uint8_t *i, int32_t ix, int32_t iy, int32_t iw, int32_t ih, int32_t ibw,
                            int32_t ibh, uint8_t *o, int32_t ox, int32_t oy, int32_t ow, int32_t oh,
                            int32_t obw, int32_t obh)
{
  const float scalex = iw / (float)ow;
  const float scaley = ih / (float)oh;
  int32_t ix2 = MAX(ix, 0);
  int32_t iy2 = MAX(iy, 0);
  int32_t ox2 = MAX(ox, 0);
  int32_t oy2 = MAX(oy, 0);
  int32_t oh2 = MIN(MIN(oh, (ibh - iy2) / scaley), obh - oy2);
  int32_t ow2 = MIN(MIN(ow, (ibw - ix2) / scalex), obw - ox2);
  assert((int)(ix2 + ow2 * scalex) <= ibw);
  assert((int)(iy2 + oh2 * scaley) <= ibh);
  assert(ox2 + ow2 <= obw);
  assert(oy2 + oh2 <= obh);
  assert(ix2 >= 0 && iy2 >= 0 && ox2 >= 0 && oy2 >= 0);
  float x = ix2, y = iy2;
  for(int s = 0; s < oh2; s++)
  {
    int idx = ox2 + obw * (oy2 + s);
    for(int t = 0; t < ow2; t++)
    {
      for(int k = 0; k < 3; k++)
        o[4 * idx + k] = // i[3*(ibw* (int)y +             (int)x             ) + k)];
            CLAMP(((int32_t)i[(4 * (ibw * (int32_t)y + (int32_t)(x + .5f * scalex)) + k)]
                   + (int32_t)i[(4 * (ibw * (int32_t)(y + .5f * scaley) + (int32_t)(x + .5f * scalex)) + k)]
                   + (int32_t)i[(4 * (ibw * (int32_t)(y + .5f * scaley) + (int32_t)(x)) + k)]
                   + (int32_t)i[(4 * (ibw * (int32_t)y + (int32_t)(x)) + k)])
                      / 4,
                  0, 255);
      x += scalex;
      idx++;
    }
    y += scaley;
    x = ix2;
  }
}

// apply clip and zoom on parts of a supplied full image.
// roi_in and roi_out define which part to work on.
void dt_iop_clip_and_zoom(float *out, const float *const in, const dt_iop_roi_t *const roi_out,
                          const dt_iop_roi_t *const roi_in, const int32_t out_stride, const int32_t in_stride)
{
  const struct dt_interpolation *itor = dt_interpolation_new(DT_INTERPOLATION_USERPREF);
  dt_interpolation_resample(itor, out, roi_out, out_stride * 4 * sizeof(float), in, roi_in,
                            in_stride * 4 * sizeof(float));
}

// apply clip and zoom on the image region supplied in the input buffer.
// roi_in and roi_out describe which part of the full image this relates to.
void dt_iop_clip_and_zoom_roi(float *out, const float *const in, const dt_iop_roi_t *const roi_out,
                              const dt_iop_roi_t *const roi_in, const int32_t out_stride,
                              const int32_t in_stride)
{
  const struct dt_interpolation *itor = dt_interpolation_new(DT_INTERPOLATION_USERPREF);
  dt_interpolation_resample_roi(itor, out, roi_out, out_stride * 4 * sizeof(float), in, roi_in,
                                in_stride * 4 * sizeof(float));
}

#ifdef HAVE_OPENCL
// apply clip and zoom on parts of a supplied full image.
// roi_in and roi_out define which part to work on.
int dt_iop_clip_and_zoom_cl(int devid, cl_mem dev_out, cl_mem dev_in, const dt_iop_roi_t *const roi_out,
                            const dt_iop_roi_t *const roi_in)
{
  const struct dt_interpolation *itor = dt_interpolation_new(DT_INTERPOLATION_USERPREF);
  return dt_interpolation_resample_cl(itor, devid, dev_out, roi_out, dev_in, roi_in);
}

// apply clip and zoom on the image region supplied in the input buffer.
// roi_in and roi_out describe which part of the full image this relates to.
int dt_iop_clip_and_zoom_roi_cl(int devid, cl_mem dev_out, cl_mem dev_in, const dt_iop_roi_t *const roi_out,
                                const dt_iop_roi_t *const roi_in)
{
  const struct dt_interpolation *itor = dt_interpolation_new(DT_INTERPOLATION_USERPREF);
  return dt_interpolation_resample_roi_cl(itor, devid, dev_out, roi_out, dev_in, roi_in);
}

#endif

void dt_iop_clip_and_zoom_mosaic_half_size_plain(uint16_t *const out, const uint16_t *const in,
                                                 const dt_iop_roi_t *const roi_out,
                                                 const dt_iop_roi_t *const roi_in, const int32_t out_stride,
                                                 const int32_t in_stride, const uint32_t filters)
{
  // adjust to pixel region and don't sample more than scale/2 nbs!
  // pixel footprint on input buffer, radius:
  const float px_footprint = 1.f / roi_out->scale;

  // move to origin point 01 of a 2x2 CFA block
  // (RGGB=0112 or CYGM=0132)
  int trggbx = 0, trggby = 0;
  if(FC(trggby, trggbx + 1, filters) != 1) trggbx++;
  if(FC(trggby, trggbx, filters) != 0)
  {
    trggbx = (trggbx + 1) & 1;
    trggby++;
  }
  const int rggbx = trggbx, rggby = trggby;

  // Create a reverse lookup of FC(): for each CFA color, a list of
  // offsets from start of a 2x2 block at which to find that
  // color. First index is color, second is to the list of offsets,
  // preceded by the number of offsets.
  int clut[4][3] = {{0}};
  for(int y = 0; y < 2; ++y)
    for(int x = 0; x < 2; ++x)
    {
      const int c = FC(y + rggby, x + rggbx, filters);
      assert(clut[c][0] < 2);
      clut[c][++clut[c][0]] = x + y * in_stride;
    }

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(clut) schedule(static)
#endif
  for(int y = 0; y < roi_out->height; y++)
  {
    uint16_t *outc = out + out_stride * y;

    const float fy = (y + roi_out->y) * px_footprint;
    const int miny = (CLAMPS((int)floorf(fy - px_footprint), 0, roi_in->height-3) & ~1u) + rggby;
    const int maxy = MIN(roi_in->height-1, (int)ceilf(fy + px_footprint));

    float fx = roi_out->x * px_footprint;
    for(int x = 0; x < roi_out->width; x++, fx += px_footprint, outc++)
    {
      const int minx = (CLAMPS((int)floorf(fx - px_footprint), 0, roi_in->width-3) & ~1u) + rggbx;
      const int maxx = MIN(roi_in->width-1, (int)ceilf(fx + px_footprint));

      const int c = FC(y, x, filters);
      int num = 0;
      uint32_t col = 0;

      for(int yy = miny; yy < maxy; yy += 2)
        for(int xx = minx; xx < maxx; xx += 2)
        {
          col += in[clut[c][1] + xx + in_stride * yy];
          num++;
          if (clut[c][0] == 2)
          { // G in RGGB CFA
            col += in[clut[c][2] + xx + in_stride * yy];
            num++;
          }
        }
      *outc = col / num;
    }
  }
}

#if defined(__SSE__)
void dt_iop_clip_and_zoom_mosaic_half_size_sse2(uint16_t *const out, const uint16_t *const in,
                                                const dt_iop_roi_t *const roi_out,
                                                const dt_iop_roi_t *const roi_in, const int32_t out_stride,
                                                const int32_t in_stride, const uint32_t filters)
{
  // adjust to pixel region and don't sample more than scale/2 nbs!
  // pixel footprint on input buffer, radius:
  const float px_footprint = 1.f / roi_out->scale;
  // how many 2x2 blocks can be sampled inside that area
  const int samples = round(px_footprint / 2);

  // move p to point to an rggb block:
  int trggbx = 0, trggby = 0;
  if(FC(trggby, trggbx + 1, filters) != 1) trggbx++;
  if(FC(trggby, trggbx, filters) != 0)
  {
    trggbx = (trggbx + 1) & 1;
    trggby++;
  }
  const int rggbx = trggbx, rggby = trggby;

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int y = 0; y < roi_out->height; y++)
  {
    uint16_t *outc = out + out_stride * y;

    float fy = (y + roi_out->y) * px_footprint;
    int py = (int)fy & ~1;
    const float dy = (fy - py) / 2;
    py = MIN(((roi_in->height - 6) & ~1u), py) + rggby;

    int maxj = MIN(((roi_in->height - 5) & ~1u) + rggby, py + 2 * samples);

    for(int x = 0; x < roi_out->width; x++)
    {
      __m128 col = _mm_setzero_ps();

      float fx = (x + roi_out->x) * px_footprint;
      int px = (int)fx & ~1;
      const float dx = (fx - px) / 2;
      px = MIN(((roi_in->width - 6) & ~1u), px) + rggbx;

      int maxi = MIN(((roi_in->width - 5) & ~1u) + rggbx, px + 2 * samples);

      float p1, p2, p3, p4;
      float num = 0;

      // upper left 2x2 block of sampling region
      p1 = in[px + in_stride * py];
      p2 = in[px + 1 + in_stride * py];
      p3 = in[px + in_stride * (py + 1)];
      p4 = in[px + 1 + in_stride * (py + 1)];
      col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps((1 - dx) * (1 - dy)), _mm_set_ps(p4, p3, p2, p1)));

      // left 2x2 block border of sampling region
      for(int j = py + 2; j <= maxj; j += 2)
      {
        p1 = in[px + in_stride * j];
        p2 = in[px + 1 + in_stride * j];
        p3 = in[px + in_stride * (j + 1)];
        p4 = in[px + 1 + in_stride * (j + 1)];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(1 - dx), _mm_set_ps(p4, p3, p2, p1)));
      }

      // upper 2x2 block border of sampling region
      for(int i = px + 2; i <= maxi; i += 2)
      {
        p1 = in[i + in_stride * py];
        p2 = in[i + 1 + in_stride * py];
        p3 = in[i + in_stride * (py + 1)];
        p4 = in[i + 1 + in_stride * (py + 1)];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(1 - dy), _mm_set_ps(p4, p3, p2, p1)));
      }

      // 2x2 blocks in the middle of sampling region
      for(int j = py + 2; j <= maxj; j += 2)
        for(int i = px + 2; i <= maxi; i += 2)
        {
          p1 = in[i + in_stride * j];
          p2 = in[i + 1 + in_stride * j];
          p3 = in[i + in_stride * (j + 1)];
          p4 = in[i + 1 + in_stride * (j + 1)];
          col = _mm_add_ps(col, _mm_set_ps(p4, p3, p2, p1));
        }

      if(maxi == px + 2 * samples && maxj == py + 2 * samples)
      {
        // right border
        for(int j = py + 2; j <= maxj; j += 2)
        {
          p1 = in[maxi + 2 + in_stride * j];
          p2 = in[maxi + 3 + in_stride * j];
          p3 = in[maxi + 2 + in_stride * (j + 1)];
          p4 = in[maxi + 3 + in_stride * (j + 1)];
          col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dx), _mm_set_ps(p4, p3, p2, p1)));
        }

        // upper right
        p1 = in[maxi + 2 + in_stride * py];
        p2 = in[maxi + 3 + in_stride * py];
        p3 = in[maxi + 2 + in_stride * (py + 1)];
        p4 = in[maxi + 3 + in_stride * (py + 1)];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dx * (1 - dy)), _mm_set_ps(p4, p3, p2, p1)));

        // lower border
        for(int i = px + 2; i <= maxi; i += 2)
        {
          p1 = in[i + in_stride * (maxj + 2)];
          p2 = in[i + 1 + in_stride * (maxj + 2)];
          p3 = in[i + in_stride * (maxj + 3)];
          p4 = in[i + 1 + in_stride * (maxj + 3)];
          col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dy), _mm_set_ps(p4, p3, p2, p1)));
        }

        // lower left 2x2 block
        p1 = in[px + in_stride * (maxj + 2)];
        p2 = in[px + 1 + in_stride * (maxj + 2)];
        p3 = in[px + in_stride * (maxj + 3)];
        p4 = in[px + 1 + in_stride * (maxj + 3)];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps((1 - dx) * dy), _mm_set_ps(p4, p3, p2, p1)));

        // lower right 2x2 block
        p1 = in[maxi + 2 + in_stride * (maxj + 2)];
        p2 = in[maxi + 3 + in_stride * (maxj + 2)];
        p3 = in[maxi + 2 + in_stride * (maxj + 3)];
        p4 = in[maxi + 3 + in_stride * (maxj + 3)];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dx * dy), _mm_set_ps(p4, p3, p2, p1)));

        num = (samples + 1) * (samples + 1);
      }
      else if(maxi == px + 2 * samples)
      {
        // right border
        for(int j = py + 2; j <= maxj; j += 2)
        {
          p1 = in[maxi + 2 + in_stride * j];
          p2 = in[maxi + 3 + in_stride * j];
          p3 = in[maxi + 2 + in_stride * (j + 1)];
          p4 = in[maxi + 3 + in_stride * (j + 1)];
          col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dx), _mm_set_ps(p4, p3, p2, p1)));
        }

        // upper right
        p1 = in[maxi + 2 + in_stride * py];
        p2 = in[maxi + 3 + in_stride * py];
        p3 = in[maxi + 2 + in_stride * (py + 1)];
        p4 = in[maxi + 3 + in_stride * (py + 1)];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dx * (1 - dy)), _mm_set_ps(p4, p3, p2, p1)));

        num = ((maxj - py) / 2 + 1 - dy) * (samples + 1);
      }
      else if(maxj == py + 2 * samples)
      {
        // lower border
        for(int i = px + 2; i <= maxi; i += 2)
        {
          p1 = in[i + in_stride * (maxj + 2)];
          p2 = in[i + 1 + in_stride * (maxj + 2)];
          p3 = in[i + in_stride * (maxj + 3)];
          p4 = in[i + 1 + in_stride * (maxj + 3)];
          col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dy), _mm_set_ps(p4, p3, p2, p1)));
        }

        // lower left 2x2 block
        p1 = in[px + in_stride * (maxj + 2)];
        p2 = in[px + 1 + in_stride * (maxj + 2)];
        p3 = in[px + in_stride * (maxj + 3)];
        p4 = in[px + 1 + in_stride * (maxj + 3)];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps((1 - dx) * dy), _mm_set_ps(p4, p3, p2, p1)));

        num = ((maxi - px) / 2 + 1 - dx) * (samples + 1);
      }
      else
      {
        num = ((maxi - px) / 2 + 1 - dx) * ((maxj - py) / 2 + 1 - dy);
      }

      num = 1.0f / num;
      col = _mm_mul_ps(col, _mm_set1_ps(num));

      float fcol[4] __attribute__((aligned(16)));
      _mm_store_ps(fcol, col);

      const int c = (2 * ((y + rggby) % 2) + ((x + rggbx) % 2));
      *outc = (uint16_t)(fcol[c]);
      outc++;
    }
  }
  _mm_sfence();
}
#endif

void dt_iop_clip_and_zoom_mosaic_half_size(uint16_t *const out, const uint16_t *const in,
                                           const dt_iop_roi_t *const roi_out, const dt_iop_roi_t *const roi_in,
                                           const int32_t out_stride, const int32_t in_stride,
                                           const uint32_t filters)
{
  if(1)//(darktable.codepath.OPENMP_SIMD)
    return dt_iop_clip_and_zoom_mosaic_half_size_plain(out, in, roi_out, roi_in, out_stride, in_stride, filters);
#if defined(__SSE__)
  else if(darktable.codepath.SSE2)
    return dt_iop_clip_and_zoom_mosaic_half_size_sse2(out, in, roi_out, roi_in, out_stride, in_stride, filters);
#endif
  else
    dt_unreachable_codepath();
}

void dt_iop_clip_and_zoom_mosaic_half_size_f_plain(float *const out, const float *const in,
                                                   const dt_iop_roi_t *const roi_out,
                                                   const dt_iop_roi_t *const roi_in, const int32_t out_stride,
                                                   const int32_t in_stride, const uint32_t filters)
{
  // adjust to pixel region and don't sample more than scale/2 nbs!
  // pixel footprint on input buffer, radius:
  const float px_footprint = 1.f / roi_out->scale;
  // how many 2x2 blocks can be sampled inside that area
  const int samples = round(px_footprint / 2);

  // move p to point to an rggb block:
  int trggbx = 0, trggby = 0;
  if(FC(trggby, trggbx + 1, filters) != 1) trggbx++;
  if(FC(trggby, trggbx, filters) != 0)
  {
    trggbx = (trggbx + 1) & 1;
    trggby++;
  }
  const int rggbx = trggbx, rggby = trggby;

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int y = 0; y < roi_out->height; y++)
  {
    float *outc = out + out_stride * y;

    float fy = (y + roi_out->y) * px_footprint;
    int py = (int)fy & ~1;
    const float dy = (fy - py) / 2;
    py = MIN(((roi_in->height - 6) & ~1u), py) + rggby;

    int maxj = MIN(((roi_in->height - 5) & ~1u) + rggby, py + 2 * samples);

    for(int x = 0; x < roi_out->width; x++)
    {
      float col[4] = { 0, 0, 0, 0 };

      float fx = (x + roi_out->x) * px_footprint;
      int px = (int)fx & ~1;
      const float dx = (fx - px) / 2;
      px = MIN(((roi_in->width - 6) & ~1u), px) + rggbx;

      int maxi = MIN(((roi_in->width - 5) & ~1u) + rggbx, px + 2 * samples);

      float p[4];
      float num = 0;

      // upper left 2x2 block of sampling region
      p[0] = in[px + in_stride * py];
      p[1] = in[px + 1 + in_stride * py];
      p[2] = in[px + in_stride * (py + 1)];
      p[3] = in[px + 1 + in_stride * (py + 1)];
      for(int c = 0; c < 4; c++) col[c] += ((1 - dx) * (1 - dy)) * p[c];

      // left 2x2 block border of sampling region
      for(int j = py + 2; j <= maxj; j += 2)
      {
        p[0] = in[px + in_stride * j];
        p[1] = in[px + 1 + in_stride * j];
        p[2] = in[px + in_stride * (j + 1)];
        p[3] = in[px + 1 + in_stride * (j + 1)];
        for(int c = 0; c < 4; c++) col[c] += (1 - dx) * p[c];
      }

      // upper 2x2 block border of sampling region
      for(int i = px + 2; i <= maxi; i += 2)
      {
        p[0] = in[i + in_stride * py];
        p[1] = in[i + 1 + in_stride * py];
        p[2] = in[i + in_stride * (py + 1)];
        p[3] = in[i + 1 + in_stride * (py + 1)];
        for(int c = 0; c < 4; c++) col[c] += (1 - dy) * p[c];
      }

      // 2x2 blocks in the middle of sampling region
      for(int j = py + 2; j <= maxj; j += 2)
        for(int i = px + 2; i <= maxi; i += 2)
        {
          p[0] = in[i + in_stride * j];
          p[1] = in[i + 1 + in_stride * j];
          p[2] = in[i + in_stride * (j + 1)];
          p[3] = in[i + 1 + in_stride * (j + 1)];
          for(int c = 0; c < 4; c++) col[c] += p[c];
        }

      if(maxi == px + 2 * samples && maxj == py + 2 * samples)
      {
        // right border
        for(int j = py + 2; j <= maxj; j += 2)
        {
          p[0] = in[maxi + 2 + in_stride * j];
          p[1] = in[maxi + 3 + in_stride * j];
          p[2] = in[maxi + 2 + in_stride * (j + 1)];
          p[3] = in[maxi + 3 + in_stride * (j + 1)];
          for(int c = 0; c < 4; c++) col[c] += dx * p[c];
        }

        // upper right
        p[0] = in[maxi + 2 + in_stride * py];
        p[1] = in[maxi + 3 + in_stride * py];
        p[2] = in[maxi + 2 + in_stride * (py + 1)];
        p[3] = in[maxi + 3 + in_stride * (py + 1)];
        for(int c = 0; c < 4; c++) col[c] += (dx * (1 - dy)) * p[c];

        // lower border
        for(int i = px + 2; i <= maxi; i += 2)
        {
          p[0] = in[i + in_stride * (maxj + 2)];
          p[1] = in[i + 1 + in_stride * (maxj + 2)];
          p[2] = in[i + in_stride * (maxj + 3)];
          p[3] = in[i + 1 + in_stride * (maxj + 3)];
          for(int c = 0; c < 4; c++) col[c] += dy * p[c];
        }

        // lower left 2x2 block
        p[0] = in[px + in_stride * (maxj + 2)];
        p[1] = in[px + 1 + in_stride * (maxj + 2)];
        p[2] = in[px + in_stride * (maxj + 3)];
        p[3] = in[px + 1 + in_stride * (maxj + 3)];
        for(int c = 0; c < 4; c++) col[c] += ((1 - dx) * dy) * p[c];

        // lower right 2x2 block
        p[0] = in[maxi + 2 + in_stride * (maxj + 2)];
        p[1] = in[maxi + 3 + in_stride * (maxj + 2)];
        p[2] = in[maxi + 2 + in_stride * (maxj + 3)];
        p[3] = in[maxi + 3 + in_stride * (maxj + 3)];
        for(int c = 0; c < 4; c++) col[c] += (dx * dy) * p[c];

        num = (samples + 1) * (samples + 1);
      }
      else if(maxi == px + 2 * samples)
      {
        // right border
        for(int j = py + 2; j <= maxj; j += 2)
        {
          p[0] = in[maxi + 2 + in_stride * j];
          p[1] = in[maxi + 3 + in_stride * j];
          p[2] = in[maxi + 2 + in_stride * (j + 1)];
          p[3] = in[maxi + 3 + in_stride * (j + 1)];
          for(int c = 0; c < 4; c++) col[c] += dx * p[c];
        }

        // upper right
        p[0] = in[maxi + 2 + in_stride * py];
        p[1] = in[maxi + 3 + in_stride * py];
        p[2] = in[maxi + 2 + in_stride * (py + 1)];
        p[3] = in[maxi + 3 + in_stride * (py + 1)];
        for(int c = 0; c < 4; c++) col[c] += (dx * (1 - dy)) * p[c];

        num = ((maxj - py) / 2 + 1 - dy) * (samples + 1);
      }
      else if(maxj == py + 2 * samples)
      {
        // lower border
        for(int i = px + 2; i <= maxi; i += 2)
        {
          p[0] = in[i + in_stride * (maxj + 2)];
          p[1] = in[i + 1 + in_stride * (maxj + 2)];
          p[2] = in[i + in_stride * (maxj + 3)];
          p[3] = in[i + 1 + in_stride * (maxj + 3)];
          for(int c = 0; c < 4; c++) col[c] += dy * p[c];
        }

        // lower left 2x2 block
        p[0] = in[px + in_stride * (maxj + 2)];
        p[1] = in[px + 1 + in_stride * (maxj + 2)];
        p[2] = in[px + in_stride * (maxj + 3)];
        p[3] = in[px + 1 + in_stride * (maxj + 3)];
        for(int c = 0; c < 4; c++) col[c] += ((1 - dx) * dy) * p[c];

        num = ((maxi - px) / 2 + 1 - dx) * (samples + 1);
      }
      else
      {
        num = ((maxi - px) / 2 + 1 - dx) * ((maxj - py) / 2 + 1 - dy);
      }

      const int c = (2 * ((y + rggby) % 2) + ((x + rggbx) % 2));
      *outc = col[c] / num;
      outc++;
    }
  }
}

#if defined(__SSE__)
void dt_iop_clip_and_zoom_mosaic_half_size_f_sse2(float *const out, const float *const in,
                                                  const dt_iop_roi_t *const roi_out,
                                                  const dt_iop_roi_t *const roi_in, const int32_t out_stride,
                                                  const int32_t in_stride, const uint32_t filters)
{
  // adjust to pixel region and don't sample more than scale/2 nbs!
  // pixel footprint on input buffer, radius:
  const float px_footprint = 1.f / roi_out->scale;
  // how many 2x2 blocks can be sampled inside that area
  const int samples = round(px_footprint / 2);

  // move p to point to an rggb block:
  int trggbx = 0, trggby = 0;
  if(FC(trggby, trggbx + 1, filters) != 1) trggbx++;
  if(FC(trggby, trggbx, filters) != 0)
  {
    trggbx = (trggbx + 1) & 1;
    trggby++;
  }
  const int rggbx = trggbx, rggby = trggby;

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int y = 0; y < roi_out->height; y++)
  {
    float *outc = out + out_stride * y;

    float fy = (y + roi_out->y) * px_footprint;
    int py = (int)fy & ~1;
    const float dy = (fy - py) / 2;
    py = MIN(((roi_in->height - 6) & ~1u), py) + rggby;

    int maxj = MIN(((roi_in->height - 5) & ~1u) + rggby, py + 2 * samples);

    for(int x = 0; x < roi_out->width; x++)
    {
      __m128 col = _mm_setzero_ps();

      float fx = (x + roi_out->x) * px_footprint;
      int px = (int)fx & ~1;
      const float dx = (fx - px) / 2;
      px = MIN(((roi_in->width - 6) & ~1u), px) + rggbx;

      int maxi = MIN(((roi_in->width - 5) & ~1u) + rggbx, px + 2 * samples);

      float p1, p2, p3, p4;
      float num = 0;

      // upper left 2x2 block of sampling region
      p1 = in[px + in_stride * py];
      p2 = in[px + 1 + in_stride * py];
      p3 = in[px + in_stride * (py + 1)];
      p4 = in[px + 1 + in_stride * (py + 1)];
      col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps((1 - dx) * (1 - dy)), _mm_set_ps(p4, p3, p2, p1)));

      // left 2x2 block border of sampling region
      for(int j = py + 2; j <= maxj; j += 2)
      {
        p1 = in[px + in_stride * j];
        p2 = in[px + 1 + in_stride * j];
        p3 = in[px + in_stride * (j + 1)];
        p4 = in[px + 1 + in_stride * (j + 1)];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(1 - dx), _mm_set_ps(p4, p3, p2, p1)));
      }

      // upper 2x2 block border of sampling region
      for(int i = px + 2; i <= maxi; i += 2)
      {
        p1 = in[i + in_stride * py];
        p2 = in[i + 1 + in_stride * py];
        p3 = in[i + in_stride * (py + 1)];
        p4 = in[i + 1 + in_stride * (py + 1)];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(1 - dy), _mm_set_ps(p4, p3, p2, p1)));
      }

      // 2x2 blocks in the middle of sampling region
      for(int j = py + 2; j <= maxj; j += 2)
        for(int i = px + 2; i <= maxi; i += 2)
        {
          p1 = in[i + in_stride * j];
          p2 = in[i + 1 + in_stride * j];
          p3 = in[i + in_stride * (j + 1)];
          p4 = in[i + 1 + in_stride * (j + 1)];
          col = _mm_add_ps(col, _mm_set_ps(p4, p3, p2, p1));
        }

      if(maxi == px + 2 * samples && maxj == py + 2 * samples)
      {
        // right border
        for(int j = py + 2; j <= maxj; j += 2)
        {
          p1 = in[maxi + 2 + in_stride * j];
          p2 = in[maxi + 3 + in_stride * j];
          p3 = in[maxi + 2 + in_stride * (j + 1)];
          p4 = in[maxi + 3 + in_stride * (j + 1)];
          col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dx), _mm_set_ps(p4, p3, p2, p1)));
        }

        // upper right
        p1 = in[maxi + 2 + in_stride * py];
        p2 = in[maxi + 3 + in_stride * py];
        p3 = in[maxi + 2 + in_stride * (py + 1)];
        p4 = in[maxi + 3 + in_stride * (py + 1)];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dx * (1 - dy)), _mm_set_ps(p4, p3, p2, p1)));

        // lower border
        for(int i = px + 2; i <= maxi; i += 2)
        {
          p1 = in[i + in_stride * (maxj + 2)];
          p2 = in[i + 1 + in_stride * (maxj + 2)];
          p3 = in[i + in_stride * (maxj + 3)];
          p4 = in[i + 1 + in_stride * (maxj + 3)];
          col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dy), _mm_set_ps(p4, p3, p2, p1)));
        }

        // lower left 2x2 block
        p1 = in[px + in_stride * (maxj + 2)];
        p2 = in[px + 1 + in_stride * (maxj + 2)];
        p3 = in[px + in_stride * (maxj + 3)];
        p4 = in[px + 1 + in_stride * (maxj + 3)];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps((1 - dx) * dy), _mm_set_ps(p4, p3, p2, p1)));

        // lower right 2x2 block
        p1 = in[maxi + 2 + in_stride * (maxj + 2)];
        p2 = in[maxi + 3 + in_stride * (maxj + 2)];
        p3 = in[maxi + 2 + in_stride * (maxj + 3)];
        p4 = in[maxi + 3 + in_stride * (maxj + 3)];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dx * dy), _mm_set_ps(p4, p3, p2, p1)));

        num = (samples + 1) * (samples + 1);
      }
      else if(maxi == px + 2 * samples)
      {
        // right border
        for(int j = py + 2; j <= maxj; j += 2)
        {
          p1 = in[maxi + 2 + in_stride * j];
          p2 = in[maxi + 3 + in_stride * j];
          p3 = in[maxi + 2 + in_stride * (j + 1)];
          p4 = in[maxi + 3 + in_stride * (j + 1)];
          col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dx), _mm_set_ps(p4, p3, p2, p1)));
        }

        // upper right
        p1 = in[maxi + 2 + in_stride * py];
        p2 = in[maxi + 3 + in_stride * py];
        p3 = in[maxi + 2 + in_stride * (py + 1)];
        p4 = in[maxi + 3 + in_stride * (py + 1)];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dx * (1 - dy)), _mm_set_ps(p4, p3, p2, p1)));

        num = ((maxj - py) / 2 + 1 - dy) * (samples + 1);
      }
      else if(maxj == py + 2 * samples)
      {
        // lower border
        for(int i = px + 2; i <= maxi; i += 2)
        {
          p1 = in[i + in_stride * (maxj + 2)];
          p2 = in[i + 1 + in_stride * (maxj + 2)];
          p3 = in[i + in_stride * (maxj + 3)];
          p4 = in[i + 1 + in_stride * (maxj + 3)];
          col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dy), _mm_set_ps(p4, p3, p2, p1)));
        }

        // lower left 2x2 block
        p1 = in[px + in_stride * (maxj + 2)];
        p2 = in[px + 1 + in_stride * (maxj + 2)];
        p3 = in[px + in_stride * (maxj + 3)];
        p4 = in[px + 1 + in_stride * (maxj + 3)];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps((1 - dx) * dy), _mm_set_ps(p4, p3, p2, p1)));

        num = ((maxi - px) / 2 + 1 - dx) * (samples + 1);
      }
      else
      {
        num = ((maxi - px) / 2 + 1 - dx) * ((maxj - py) / 2 + 1 - dy);
      }

      num = 1.0f / num;
      col = _mm_mul_ps(col, _mm_set1_ps(num));

      float fcol[4] __attribute__((aligned(16)));
      _mm_store_ps(fcol, col);

      const int c = (2 * ((y + rggby) % 2) + ((x + rggbx) % 2));
      *outc = fcol[c];
      outc++;
    }
  }
  _mm_sfence();
}
#endif

void dt_iop_clip_and_zoom_mosaic_half_size_f(float *const out, const float *const in,
                                             const dt_iop_roi_t *const roi_out, const dt_iop_roi_t *const roi_in,
                                             const int32_t out_stride, const int32_t in_stride,
                                             const uint32_t filters)
{
  if(darktable.codepath.OPENMP_SIMD)
    return dt_iop_clip_and_zoom_mosaic_half_size_f_plain(out, in, roi_out, roi_in, out_stride, in_stride, filters);
#if defined(__SSE__)
  else if(darktable.codepath.SSE2)
    return dt_iop_clip_and_zoom_mosaic_half_size_f_sse2(out, in, roi_out, roi_in, out_stride, in_stride, filters);
#endif
  else
    dt_unreachable_codepath();
}

/**
 * downscales and clips a Fujifilm X-Trans mosaiced buffer (in) to the given region of interest (r_*)
 * and writes it to out.
 */
void dt_iop_clip_and_zoom_mosaic_third_size_xtrans(uint16_t *const out, const uint16_t *const in,
                                                   const dt_iop_roi_t *const roi_out,
                                                   const dt_iop_roi_t *const roi_in, const int32_t out_stride,
                                                   const int32_t in_stride, const uint8_t (*const xtrans)[6])
{
  const float px_footprint = 1.f / roi_out->scale;
  // Use box filter of width px_footprint*2+1 centered on the current
  // sample (rounded to nearest input pixel) to anti-alias. Higher MP
  // images need larger filters to avoid artifacts.
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int y = 0; y < roi_out->height; y++)
  {
    uint16_t *outc = out + out_stride * y;

    const float fy = (y + roi_out->y) * px_footprint;
    const int miny = MAX(0, (int)roundf(fy - px_footprint));
    const int maxy = MIN(roi_in->height-1, (int)roundf(fy + px_footprint));

    float fx = roi_out->x * px_footprint;
    for(int x = 0; x < roi_out->width; x++, fx += px_footprint, outc++)
    {
      const int minx = MAX(0, (int)roundf(fx - px_footprint));
      const int maxx = MIN(roi_in->width-1, (int)roundf(fx + px_footprint));

      const int c = FCxtrans(y, x, roi_out, xtrans);
      int num = 0;
      uint32_t col = 0;

      for(int yy = miny; yy <= maxy; ++yy)
        for(int xx = minx; xx <= maxx; ++xx)
          if(FCxtrans(yy, xx, roi_in, xtrans) == c)
          {
            col += in[xx + in_stride * yy];
            num++;
          }
      *outc = col / num;
    }
  }
}

void dt_iop_clip_and_zoom_mosaic_third_size_xtrans_f(float *const out, const float *const in,
                                                     const dt_iop_roi_t *const roi_out,
                                                     const dt_iop_roi_t *const roi_in, const int32_t out_stride,
                                                     const int32_t in_stride, const uint8_t (*const xtrans)[6])
{
  const float px_footprint = 1.f / roi_out->scale;
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int y = 0; y < roi_out->height; y++)
  {
    float *outc = out + out_stride * y;

    const float fy = (y + roi_out->y) * px_footprint;
    const int miny = MAX(0, (int)roundf(fy - px_footprint));
    const int maxy = MIN(roi_in->height-1, (int)roundf(fy + px_footprint));

    float fx = roi_out->x * px_footprint;
    for(int x = 0; x < roi_out->width; x++, fx += px_footprint, outc++)
    {
      const int minx = MAX(0, (int)roundf(fx - px_footprint));
      const int maxx = MIN(roi_in->width-1, (int)roundf(fx + px_footprint));

      const int c = FCxtrans(y, x, roi_out, xtrans);
      int num = 0;
      float col = 0.f;

      for(int yy = miny; yy <= maxy; ++yy)
        for(int xx = minx; xx <= maxx; ++xx)
          if(FCxtrans(yy, xx, roi_in, xtrans) == c)
          {
            col += in[xx + in_stride * yy];
            num++;
          }
      *outc = col / (float)num;
    }
  }
}

void dt_iop_clip_and_zoom_demosaic_passthrough_monochrome_f_plain(float *out, const float *const in,
                                                                  const dt_iop_roi_t *const roi_out,
                                                                  const dt_iop_roi_t *const roi_in,
                                                                  const int32_t out_stride,
                                                                  const int32_t in_stride)
{
  // adjust to pixel region and don't sample more than scale/2 nbs!
  // pixel footprint on input buffer, radius:
  const float px_footprint = 1.f / roi_out->scale;
  // how many pixels can be sampled inside that area
  const int samples = round(px_footprint);

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(out) schedule(static)
#endif
  for(int y = 0; y < roi_out->height; y++)
  {
    float *outc = out + 4 * (out_stride * y);

    float fy = (y + roi_out->y) * px_footprint;
    int py = (int)fy;
    const float dy = fy - py;
    py = MIN(((roi_in->height - 3)), py);

    int maxj = MIN(((roi_in->height - 2)), py + samples);

    for(int x = 0; x < roi_out->width; x++)
    {
      float col = 0.0f;

      float fx = (x + roi_out->x) * px_footprint;
      int px = (int)fx;
      const float dx = fx - px;
      px = MIN(((roi_in->width - 3)), px);

      int maxi = MIN(((roi_in->width - 2)), px + samples);

      float p;
      float num = 0;

      // upper left pixel of sampling region
      p = in[px + in_stride * py];
      col += ((1 - dx) * (1 - dy)) * p;

      // left pixel border of sampling region
      for(int j = py + 1; j <= maxj; j++)
      {
        p = in[px + in_stride * j];
        col += (1 - dx) * p;
      }

      // upper pixel border of sampling region
      for(int i = px + 1; i <= maxi; i++)
      {
        p = in[i + in_stride * py];
        col += (1 - dy) * p;
      }

      // pixels in the middle of sampling region
      for(int j = py + 1; j <= maxj; j++)
        for(int i = px + 1; i <= maxi; i++)
        {
          p = in[i + in_stride * j];
          col += p;
        }

      if(maxi == px + samples && maxj == py + samples)
      {
        // right border
        for(int j = py + 1; j <= maxj; j++)
        {
          p = in[maxi + 1 + in_stride * j];
          col += dx * p;
        }

        // upper right
        p = in[maxi + 1 + in_stride * py];
        col += (dx * (1 - dy)) * p;

        // lower border
        for(int i = px + 1; i <= maxi; i++)
        {
          p = in[i + in_stride * (maxj + 1)];
          col += dy * p;
        }

        // lower left pixel
        p = in[px + in_stride * (maxj + 1)];
        col += ((1 - dx) * dy) * p;

        // lower right pixel
        p = in[maxi + 1 + in_stride * (maxj + 1)];
        col += (dx * dy) * p;

        num = (samples + 1) * (samples + 1);
      }
      else if(maxi == px + samples)
      {
        // right border
        for(int j = py + 1; j <= maxj; j++)
        {
          p = in[maxi + 1 + in_stride * j];
          col += dx * p;
        }

        // upper right
        p = in[maxi + 1 + in_stride * py];
        col += (dx * (1 - dy)) * p;

        num = ((maxj - py) / 2 + 1 - dy) * (samples + 1);
      }
      else if(maxj == py + samples)
      {
        // lower border
        for(int i = px + 1; i <= maxi; i++)
        {
          p = in[i + in_stride * (maxj + 1)];
          col += dy * p;
        }

        // lower left pixel
        p = in[px + in_stride * (maxj + 1)];
        col += ((1 - dx) * dy) * p;

        num = ((maxi - px) / 2 + 1 - dx) * (samples + 1);
      }
      else
      {
        num = ((maxi - px) / 2 + 1 - dx) * ((maxj - py) / 2 + 1 - dy);
      }

      const float pix = col / num;
      outc[0] = pix;
      outc[1] = pix;
      outc[2] = pix;
      outc[3] = 0.0f;
      outc += 4;
    }
  }
}

#if defined(__SSE__)
void dt_iop_clip_and_zoom_demosaic_passthrough_monochrome_f_sse2(float *out, const float *const in,
                                                                 const dt_iop_roi_t *const roi_out,
                                                                 const dt_iop_roi_t *const roi_in,
                                                                 const int32_t out_stride,
                                                                 const int32_t in_stride)
{
  // adjust to pixel region and don't sample more than scale/2 nbs!
  // pixel footprint on input buffer, radius:
  const float px_footprint = 1.f / roi_out->scale;
  // how many pixels can be sampled inside that area
  const int samples = round(px_footprint);

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(out) schedule(static)
#endif
  for(int y = 0; y < roi_out->height; y++)
  {
    float *outc = out + 4 * (out_stride * y);

    float fy = (y + roi_out->y) * px_footprint;
    int py = (int)fy;
    const float dy = fy - py;
    py = MIN(((roi_in->height - 3)), py);

    int maxj = MIN(((roi_in->height - 2)), py + samples);

    for(int x = 0; x < roi_out->width; x++)
    {
      __m128 col = _mm_setzero_ps();

      float fx = (x + roi_out->x) * px_footprint;
      int px = (int)fx;
      const float dx = fx - px;
      px = MIN(((roi_in->width - 3)), px);

      int maxi = MIN(((roi_in->width - 2)), px + samples);

      float p;
      float num = 0;

      // upper left pixel of sampling region
      p = in[px + in_stride * py];
      col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps((1 - dx) * (1 - dy)), _mm_set_ps(0.0f, p, p, p)));

      // left pixel border of sampling region
      for(int j = py + 1; j <= maxj; j++)
      {
        p = in[px + in_stride * j];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(1 - dx), _mm_set_ps(0.0f, p, p, p)));
      }

      // upper pixel border of sampling region
      for(int i = px + 1; i <= maxi; i++)
      {
        p = in[i + in_stride * py];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(1 - dy), _mm_set_ps(0.0f, p, p, p)));
      }

      // pixels in the middle of sampling region
      for(int j = py + 1; j <= maxj; j++)
        for(int i = px + 1; i <= maxi; i++)
        {
          p = in[i + in_stride * j];
          col = _mm_add_ps(col, _mm_set_ps(0.0f, p, p, p));
        }

      if(maxi == px + samples && maxj == py + samples)
      {
        // right border
        for(int j = py + 1; j <= maxj; j++)
        {
          p = in[maxi + 1 + in_stride * j];
          col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dx), _mm_set_ps(0.0f, p, p, p)));
        }

        // upper right
        p = in[maxi + 1 + in_stride * py];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dx * (1 - dy)), _mm_set_ps(0.0f, p, p, p)));

        // lower border
        for(int i = px + 1; i <= maxi; i++)
        {
          p = in[i + in_stride * (maxj + 1)];
          col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dy), _mm_set_ps(0.0f, p, p, p)));
        }

        // lower left pixel
        p = in[px + in_stride * (maxj + 1)];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps((1 - dx) * dy), _mm_set_ps(0.0f, p, p, p)));

        // lower right pixel
        p = in[maxi + 1 + in_stride * (maxj + 1)];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dx * dy), _mm_set_ps(0.0f, p, p, p)));

        num = (samples + 1) * (samples + 1);
      }
      else if(maxi == px + samples)
      {
        // right border
        for(int j = py + 1; j <= maxj; j++)
        {
          p = in[maxi + 1 + in_stride * j];
          col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dx), _mm_set_ps(0.0f, p, p, p)));
        }

        // upper right
        p = in[maxi + 1 + in_stride * py];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dx * (1 - dy)), _mm_set_ps(0.0f, p, p, p)));

        num = ((maxj - py) / 2 + 1 - dy) * (samples + 1);
      }
      else if(maxj == py + samples)
      {
        // lower border
        for(int i = px + 1; i <= maxi; i++)
        {
          p = in[i + in_stride * (maxj + 1)];
          col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dy), _mm_set_ps(0.0f, p, p, p)));
        }

        // lower left pixel
        p = in[px + in_stride * (maxj + 1)];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps((1 - dx) * dy), _mm_set_ps(0.0f, p, p, p)));

        num = ((maxi - px) / 2 + 1 - dx) * (samples + 1);
      }
      else
      {
        num = ((maxi - px) / 2 + 1 - dx) * ((maxj - py) / 2 + 1 - dy);
      }

      num = 1.0f / num;
      col = _mm_mul_ps(col, _mm_set_ps(0.0f, num, num, num));
      _mm_stream_ps(outc, col);
      outc += 4;
    }
  }
  _mm_sfence();
}
#endif

void dt_iop_clip_and_zoom_demosaic_passthrough_monochrome_f(float *out, const float *const in,
                                                            const dt_iop_roi_t *const roi_out,
                                                            const dt_iop_roi_t *const roi_in,
                                                            const int32_t out_stride, const int32_t in_stride)
{
  if(darktable.codepath.OPENMP_SIMD)
    return dt_iop_clip_and_zoom_demosaic_passthrough_monochrome_f_plain(out, in, roi_out, roi_in, out_stride,
                                                                        in_stride);
#if defined(__SSE__)
  else if(darktable.codepath.SSE2)
    return dt_iop_clip_and_zoom_demosaic_passthrough_monochrome_f_sse2(out, in, roi_out, roi_in, out_stride,
                                                                       in_stride);
#endif
  else
    dt_unreachable_codepath();
}

#if 0 // gets rid of pink artifacts, but doesn't do sub-pixel sampling, so shows some staircasing artifacts.
void
dt_iop_clip_and_zoom_demosaic_half_size_f(
  float *out,
  const float *const in,
  const dt_iop_roi_t *const roi_out,
  const dt_iop_roi_t *const roi_in,
  const int32_t out_stride,
  const int32_t in_stride,
  const uint32_t filters,
  const float clip)
{
  // adjust to pixel region and don't sample more than scale/2 nbs!
  // pixel footprint on input buffer, radius:
  const float px_footprint = 1.f/roi_out->scale;
  // how many 2x2 blocks can be sampled inside that area
  const int samples = round(px_footprint/2);

  // move p to point to an rggb block:
  int trggbx = 0, trggby = 0;
  if(FC(trggby, trggbx+1, filters) != 1) trggbx ++;
  if(FC(trggby, trggbx,   filters) != 0)
  {
    trggbx = (trggbx + 1)&1;
    trggby ++;
  }
  const int rggbx = trggbx, rggby = trggby;

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(out) schedule(static)
#endif
  for(int y=0; y<roi_out->height; y++)
  {
    float *outc = out + 4*(out_stride*y);

    float fy = (y + roi_out->y)*px_footprint;
    int py = (int)fy & ~1;
    py = MIN(((roi_in->height-4) & ~1u), py) + rggby;

    int maxj = MIN(((roi_in->height-3)&~1u)+rggby, py+2*samples);

    float fx = roi_out->x*px_footprint;

    for(int x=0; x<roi_out->width; x++)
    {
      __m128 col = _mm_setzero_ps();

      fx += px_footprint;
      int px = (int)fx & ~1;
      px = MIN(((roi_in->width -4) & ~1u), px) + rggbx;

      int maxi = MIN(((roi_in->width -3)&~1u)+rggbx, px+2*samples);

      int num = 0;

      const int idx = px + in_stride*py;
      const float pc = MAX(MAX(in[idx], in[idx+1]), MAX(in[idx + in_stride], in[idx+1 + in_stride]));

      // 2x2 blocks in the middle of sampling region
      __m128 sum = _mm_setzero_ps();

      for(int j=py; j<=maxj; j+=2)
        for(int i=px; i<=maxi; i+=2)
        {
          const float p1 = in[i   + in_stride*j];
          const float p2 = in[i+1 + in_stride*j];
          const float p3 = in[i   + in_stride*(j + 1)];
          const float p4 = in[i+1 + in_stride*(j + 1)];

          if (!((pc >= clip) ^ (MAX(MAX(p1,p2),MAX(p3,p4)) >= clip)))
          {
            sum = _mm_add_ps(sum, _mm_set_ps(0,p4,p3+p2,p1));
            num++;
          }
        }

      col = _mm_mul_ps(sum, _mm_div_ps(_mm_set_ps(0.0f,1.0f,0.5f,1.0f),_mm_set1_ps(num)));
      _mm_stream_ps(outc, col);
      outc += 4;
    }
  }
  _mm_sfence();
}

#else
// very fast and smooth, but doesn't handle highlights:

void dt_iop_clip_and_zoom_demosaic_half_size_f_plain(float *out, const float *const in,
                                                     const dt_iop_roi_t *const roi_out,
                                                     const dt_iop_roi_t *const roi_in, const int32_t out_stride,
                                                     const int32_t in_stride, const uint32_t filters)
{
  // adjust to pixel region and don't sample more than scale/2 nbs!
  // pixel footprint on input buffer, radius:
  const float px_footprint = 1.f / roi_out->scale;
  // how many 2x2 blocks can be sampled inside that area
  const int samples = round(px_footprint / 2);

  // move p to point to an rggb block:
  int trggbx = 0, trggby = 0;
  if(FC(trggby, trggbx + 1, filters) != 1) trggbx++;
  if(FC(trggby, trggbx, filters) != 0)
  {
    trggbx = (trggbx + 1) & 1;
    trggby++;
  }
  const int rggbx = trggbx, rggby = trggby;

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(out) schedule(static)
#endif
  for(int y = 0; y < roi_out->height; y++)
  {
    float *outc = out + 4 * (out_stride * y);

    float fy = (y + roi_out->y) * px_footprint;
    int py = (int)fy & ~1;
    const float dy = (fy - py) / 2;
    py = MIN(((roi_in->height - 6) & ~1u), py) + rggby;

    int maxj = MIN(((roi_in->height - 5) & ~1u) + rggby, py + 2 * samples);

    for(int x = 0; x < roi_out->width; x++)
    {
      float col[4] = { 0, 0, 0, 0 };

      float fx = (x + roi_out->x) * px_footprint;
      int px = (int)fx & ~1;
      const float dx = (fx - px) / 2;
      px = MIN(((roi_in->width - 6) & ~1u), px) + rggbx;

      int maxi = MIN(((roi_in->width - 5) & ~1u) + rggbx, px + 2 * samples);

      float p[3];
      float num = 0;

      // upper left 2x2 block of sampling region
      p[0] = in[px + in_stride * py];
      p[1] = in[px + 1 + in_stride * py] + in[px + in_stride * (py + 1)];
      p[2] = in[px + 1 + in_stride * (py + 1)];
      for(int c = 0; c < 3; c++) col[c] += ((1 - dx) * (1 - dy)) * p[c];

      // left 2x2 block border of sampling region
      for(int j = py + 2; j <= maxj; j += 2)
      {
        p[0] = in[px + in_stride * j];
        p[1] = in[px + 1 + in_stride * j] + in[px + in_stride * (j + 1)];
        p[2] = in[px + 1 + in_stride * (j + 1)];
        for(int c = 0; c < 3; c++) col[c] += (1 - dx) * p[c];
      }

      // upper 2x2 block border of sampling region
      for(int i = px + 2; i <= maxi; i += 2)
      {
        p[0] = in[i + in_stride * py];
        p[1] = in[i + 1 + in_stride * py] + in[i + in_stride * (py + 1)];
        p[2] = in[i + 1 + in_stride * (py + 1)];
        for(int c = 0; c < 3; c++) col[c] += (1 - dy) * p[c];
      }

      // 2x2 blocks in the middle of sampling region
      for(int j = py + 2; j <= maxj; j += 2)
        for(int i = px + 2; i <= maxi; i += 2)
        {
          p[0] = in[i + in_stride * j];
          p[1] = in[i + 1 + in_stride * j] + in[i + in_stride * (j + 1)];
          p[2] = in[i + 1 + in_stride * (j + 1)];
          for(int c = 0; c < 3; c++) col[c] += p[c];
        }

      if(maxi == px + 2 * samples && maxj == py + 2 * samples)
      {
        // right border
        for(int j = py + 2; j <= maxj; j += 2)
        {
          p[0] = in[maxi + 2 + in_stride * j];
          p[1] = in[maxi + 3 + in_stride * j] + in[maxi + 2 + in_stride * (j + 1)];
          p[2] = in[maxi + 3 + in_stride * (j + 1)];
          for(int c = 0; c < 3; c++) col[c] += dx * p[c];
        }

        // upper right
        p[0] = in[maxi + 2 + in_stride * py];
        p[1] = in[maxi + 3 + in_stride * py] + in[maxi + 2 + in_stride * (py + 1)];
        p[2] = in[maxi + 3 + in_stride * (py + 1)];
        for(int c = 0; c < 3; c++) col[c] += (dx * (1 - dy)) * p[c];

        // lower border
        for(int i = px + 2; i <= maxi; i += 2)
        {
          p[0] = in[i + in_stride * (maxj + 2)];
          p[1] = in[i + 1 + in_stride * (maxj + 2)] + in[i + in_stride * (maxj + 3)];
          p[2] = in[i + 1 + in_stride * (maxj + 3)];
          for(int c = 0; c < 3; c++) col[c] += dy * p[c];
        }

        // lower left 2x2 block
        p[0] = in[px + in_stride * (maxj + 2)];
        p[1] = in[px + 1 + in_stride * (maxj + 2)] + in[px + in_stride * (maxj + 3)];
        p[2] = in[px + 1 + in_stride * (maxj + 3)];
        for(int c = 0; c < 3; c++) col[c] += ((1 - dx) * dy) * p[c];

        // lower right 2x2 block
        p[0] = in[maxi + 2 + in_stride * (maxj + 2)];
        p[1] = in[maxi + 3 + in_stride * (maxj + 2)] + in[maxi + 2 + in_stride * (maxj + 3)];
        p[2] = in[maxi + 3 + in_stride * (maxj + 3)];
        for(int c = 0; c < 3; c++) col[c] += (dx * dy) * p[c];

        num = (samples + 1) * (samples + 1);
      }
      else if(maxi == px + 2 * samples)
      {
        // right border
        for(int j = py + 2; j <= maxj; j += 2)
        {
          p[0] = in[maxi + 2 + in_stride * j];
          p[1] = in[maxi + 3 + in_stride * j] + in[maxi + 2 + in_stride * (j + 1)];
          p[2] = in[maxi + 3 + in_stride * (j + 1)];
          for(int c = 0; c < 3; c++) col[c] += dx * p[c];
        }

        // upper right
        p[0] = in[maxi + 2 + in_stride * py];
        p[1] = in[maxi + 3 + in_stride * py] + in[maxi + 2 + in_stride * (py + 1)];
        p[2] = in[maxi + 3 + in_stride * (py + 1)];
        for(int c = 0; c < 3; c++) col[c] += (dx * (1 - dy)) * p[c];

        num = ((maxj - py) / 2 + 1 - dy) * (samples + 1);
      }
      else if(maxj == py + 2 * samples)
      {
        // lower border
        for(int i = px + 2; i <= maxi; i += 2)
        {
          p[0] = in[i + in_stride * (maxj + 2)];
          p[1] = in[i + 1 + in_stride * (maxj + 2)] + in[i + in_stride * (maxj + 3)];
          p[2] = in[i + 1 + in_stride * (maxj + 3)];
          for(int c = 0; c < 3; c++) col[c] += dy * p[c];
        }

        // lower left 2x2 block
        p[0] = in[px + in_stride * (maxj + 2)];
        p[1] = in[px + 1 + in_stride * (maxj + 2)] + in[px + in_stride * (maxj + 3)];
        p[2] = in[px + 1 + in_stride * (maxj + 3)];
        for(int c = 0; c < 3; c++) col[c] += ((1 - dx) * dy) * p[c];

        num = ((maxi - px) / 2 + 1 - dx) * (samples + 1);
      }
      else
      {
        num = ((maxi - px) / 2 + 1 - dx) * ((maxj - py) / 2 + 1 - dy);
      }

      outc[0] = col[0] / num;
      outc[1] = (col[1] / num) / 2.0f;
      outc[2] = col[2] / num;
      outc[3] = 0.0f;
      outc += 4;
    }
  }
}

#if defined(__SSE__)
void dt_iop_clip_and_zoom_demosaic_half_size_f_sse2(float *out, const float *const in,
                                                    const dt_iop_roi_t *const roi_out,
                                                    const dt_iop_roi_t *const roi_in, const int32_t out_stride,
                                                    const int32_t in_stride, const uint32_t filters)
{
  // adjust to pixel region and don't sample more than scale/2 nbs!
  // pixel footprint on input buffer, radius:
  const float px_footprint = 1.f / roi_out->scale;
  // how many 2x2 blocks can be sampled inside that area
  const int samples = round(px_footprint / 2);

  // move p to point to an rggb block:
  int trggbx = 0, trggby = 0;
  if(FC(trggby, trggbx + 1, filters) != 1) trggbx++;
  if(FC(trggby, trggbx, filters) != 0)
  {
    trggbx = (trggbx + 1) & 1;
    trggby++;
  }
  const int rggbx = trggbx, rggby = trggby;

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(out) schedule(static)
#endif
  for(int y = 0; y < roi_out->height; y++)
  {
    float *outc = out + 4 * (out_stride * y);

    float fy = (y + roi_out->y) * px_footprint;
    int py = (int)fy & ~1;
    const float dy = (fy - py) / 2;
    py = MIN(((roi_in->height - 6) & ~1u), py) + rggby;

    int maxj = MIN(((roi_in->height - 5) & ~1u) + rggby, py + 2 * samples);

    for(int x = 0; x < roi_out->width; x++)
    {
      __m128 col = _mm_setzero_ps();

      float fx = (x + roi_out->x) * px_footprint;
      int px = (int)fx & ~1;
      const float dx = (fx - px) / 2;
      px = MIN(((roi_in->width - 6) & ~1u), px) + rggbx;

      int maxi = MIN(((roi_in->width - 5) & ~1u) + rggbx, px + 2 * samples);

      float p1, p2, p4;
      float num = 0;

      // upper left 2x2 block of sampling region
      p1 = in[px + in_stride * py];
      p2 = in[px + 1 + in_stride * py] + in[px + in_stride * (py + 1)];
      p4 = in[px + 1 + in_stride * (py + 1)];
      col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps((1 - dx) * (1 - dy)), _mm_set_ps(0.0f, p4, p2, p1)));

      // left 2x2 block border of sampling region
      for(int j = py + 2; j <= maxj; j += 2)
      {
        p1 = in[px + in_stride * j];
        p2 = in[px + 1 + in_stride * j] + in[px + in_stride * (j + 1)];
        p4 = in[px + 1 + in_stride * (j + 1)];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(1 - dx), _mm_set_ps(0.0f, p4, p2, p1)));
      }

      // upper 2x2 block border of sampling region
      for(int i = px + 2; i <= maxi; i += 2)
      {
        p1 = in[i + in_stride * py];
        p2 = in[i + 1 + in_stride * py] + in[i + in_stride * (py + 1)];
        p4 = in[i + 1 + in_stride * (py + 1)];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(1 - dy), _mm_set_ps(0.0f, p4, p2, p1)));
      }

      // 2x2 blocks in the middle of sampling region
      for(int j = py + 2; j <= maxj; j += 2)
        for(int i = px + 2; i <= maxi; i += 2)
        {
          p1 = in[i + in_stride * j];
          p2 = in[i + 1 + in_stride * j] + in[i + in_stride * (j + 1)];
          p4 = in[i + 1 + in_stride * (j + 1)];
          col = _mm_add_ps(col, _mm_set_ps(0.0f, p4, p2, p1));
        }

      if(maxi == px + 2 * samples && maxj == py + 2 * samples)
      {
        // right border
        for(int j = py + 2; j <= maxj; j += 2)
        {
          p1 = in[maxi + 2 + in_stride * j];
          p2 = in[maxi + 3 + in_stride * j] + in[maxi + 2 + in_stride * (j + 1)];
          p4 = in[maxi + 3 + in_stride * (j + 1)];
          col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dx), _mm_set_ps(0.0f, p4, p2, p1)));
        }

        // upper right
        p1 = in[maxi + 2 + in_stride * py];
        p2 = in[maxi + 3 + in_stride * py] + in[maxi + 2 + in_stride * (py + 1)];
        p4 = in[maxi + 3 + in_stride * (py + 1)];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dx * (1 - dy)), _mm_set_ps(0.0f, p4, p2, p1)));

        // lower border
        for(int i = px + 2; i <= maxi; i += 2)
        {
          p1 = in[i + in_stride * (maxj + 2)];
          p2 = in[i + 1 + in_stride * (maxj + 2)] + in[i + in_stride * (maxj + 3)];
          p4 = in[i + 1 + in_stride * (maxj + 3)];
          col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dy), _mm_set_ps(0.0f, p4, p2, p1)));
        }

        // lower left 2x2 block
        p1 = in[px + in_stride * (maxj + 2)];
        p2 = in[px + 1 + in_stride * (maxj + 2)] + in[px + in_stride * (maxj + 3)];
        p4 = in[px + 1 + in_stride * (maxj + 3)];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps((1 - dx) * dy), _mm_set_ps(0.0f, p4, p2, p1)));

        // lower right 2x2 block
        p1 = in[maxi + 2 + in_stride * (maxj + 2)];
        p2 = in[maxi + 3 + in_stride * (maxj + 2)] + in[maxi + 2 + in_stride * (maxj + 3)];
        p4 = in[maxi + 3 + in_stride * (maxj + 3)];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dx * dy), _mm_set_ps(0.0f, p4, p2, p1)));

        num = (samples + 1) * (samples + 1);
      }
      else if(maxi == px + 2 * samples)
      {
        // right border
        for(int j = py + 2; j <= maxj; j += 2)
        {
          p1 = in[maxi + 2 + in_stride * j];
          p2 = in[maxi + 3 + in_stride * j] + in[maxi + 2 + in_stride * (j + 1)];
          p4 = in[maxi + 3 + in_stride * (j + 1)];
          col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dx), _mm_set_ps(0.0f, p4, p2, p1)));
        }

        // upper right
        p1 = in[maxi + 2 + in_stride * py];
        p2 = in[maxi + 3 + in_stride * py] + in[maxi + 2 + in_stride * (py + 1)];
        p4 = in[maxi + 3 + in_stride * (py + 1)];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dx * (1 - dy)), _mm_set_ps(0.0f, p4, p2, p1)));

        num = ((maxj - py) / 2 + 1 - dy) * (samples + 1);
      }
      else if(maxj == py + 2 * samples)
      {
        // lower border
        for(int i = px + 2; i <= maxi; i += 2)
        {
          p1 = in[i + in_stride * (maxj + 2)];
          p2 = in[i + 1 + in_stride * (maxj + 2)] + in[i + in_stride * (maxj + 3)];
          p4 = in[i + 1 + in_stride * (maxj + 3)];
          col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dy), _mm_set_ps(0.0f, p4, p2, p1)));
        }

        // lower left 2x2 block
        p1 = in[px + in_stride * (maxj + 2)];
        p2 = in[px + 1 + in_stride * (maxj + 2)] + in[px + in_stride * (maxj + 3)];
        p4 = in[px + 1 + in_stride * (maxj + 3)];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps((1 - dx) * dy), _mm_set_ps(0.0f, p4, p2, p1)));

        num = ((maxi - px) / 2 + 1 - dx) * (samples + 1);
      }
      else
      {
        num = ((maxi - px) / 2 + 1 - dx) * ((maxj - py) / 2 + 1 - dy);
      }

      num = 1.0f / num;
      col = _mm_mul_ps(col, _mm_set_ps(0.0f, num, 0.5f * num, num));
      _mm_stream_ps(outc, col);
      outc += 4;
    }
  }
  _mm_sfence();
}
#endif
#endif

void dt_iop_clip_and_zoom_demosaic_half_size_f(float *out, const float *const in,
                                               const dt_iop_roi_t *const roi_out, const dt_iop_roi_t *const roi_in,
                                               const int32_t out_stride, const int32_t in_stride,
                                               const uint32_t filters)
{
  if(darktable.codepath.OPENMP_SIMD)
    return dt_iop_clip_and_zoom_demosaic_half_size_f_plain(out, in, roi_out, roi_in, out_stride, in_stride,
                                                           filters);
#if defined(__SSE__)
  else if(darktable.codepath.SSE2)
    return dt_iop_clip_and_zoom_demosaic_half_size_f_sse2(out, in, roi_out, roi_in, out_stride, in_stride, filters);
#endif
  else
    dt_unreachable_codepath();
}

void dt_iop_clip_and_zoom_demosaic_third_size_xtrans_f(float *out, const float *const in,
                                                       const dt_iop_roi_t *const roi_out,
                                                       const dt_iop_roi_t *const roi_in,
                                                       const int32_t out_stride, const int32_t in_stride,
                                                       const uint8_t (*const xtrans)[6])
{
  const float px_footprint = 1.f / roi_out->scale;
  const int samples = MAX(1, (int)floorf(px_footprint / 3));

  // A slightly different algorithm than
  // dt_iop_clip_and_zoom_demosaic_half_size_f() which aligns to 2x2
  // Bayer grid and hence most pull additional data from all edges
  // which don't align with CFA. Instead align to a 3x3 pattern (which
  // is semi-regular in X-Trans CFA). This code doesn't worry about
  // fractional pixel offset of top/left of pattern nor oversampling
  // by non-integer number of samples.

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(out) schedule(static)
#endif
  for(int y = 0; y < roi_out->height; y++)
  {
    float *outc = out + 4 * (out_stride * y);
    const int py = CLAMPS((int)round((y + roi_out->y - 0.5f) * px_footprint), 0, roi_in->height - 3);
    const int ymax = MIN(roi_in->height - 3, py + 3 * samples);

    for(int x = 0; x < roi_out->width; x++, outc += 4)
    {
      float col[3] = { 0.0f };
      int num = 0;
      const int px = CLAMPS((int)round((x + roi_out->x - 0.5f) * px_footprint), 0, roi_in->width - 3);
      const int xmax = MIN(roi_in->width - 3, px + 3 * samples);
      for(int yy = py; yy <= ymax; yy += 3)
        for(int xx = px; xx <= xmax; xx += 3)
        {
          for(int j = 0; j < 3; ++j)
            for(int i = 0; i < 3; ++i)
              col[FCxtrans(yy + j, xx + i, roi_in, xtrans)] += in[xx + i + in_stride * (yy + j)];
          num++;
        }

      // X-Trans RGB weighting averages to 2:5:2 for each 3x3 cell
      outc[0] = col[0] / (num * 2);
      outc[1] = col[1] / (num * 5);
      outc[2] = col[2] / (num * 2);
    }
  }
}

void dt_iop_RGB_to_YCbCr(const float *rgb, float *yuv)
{
  yuv[0] = 0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2];
  yuv[1] = -0.147 * rgb[0] - 0.289 * rgb[1] + 0.437 * rgb[2];
  yuv[2] = 0.615 * rgb[0] - 0.515 * rgb[1] - 0.100 * rgb[2];
}

void dt_iop_YCbCr_to_RGB(const float *yuv, float *rgb)
{
  rgb[0] = yuv[0] + 1.140 * yuv[2];
  rgb[1] = yuv[0] - 0.394 * yuv[1] - 0.581 * yuv[2];
  rgb[2] = yuv[0] + 2.028 * yuv[1];
}

static inline void mat4inv(const float X[][4], float R[][4])
{
  const float det = X[0][3] * X[1][2] * X[2][1] * X[3][0] - X[0][2] * X[1][3] * X[2][1] * X[3][0]
                    - X[0][3] * X[1][1] * X[2][2] * X[3][0] + X[0][1] * X[1][3] * X[2][2] * X[3][0]
                    + X[0][2] * X[1][1] * X[2][3] * X[3][0] - X[0][1] * X[1][2] * X[2][3] * X[3][0]
                    - X[0][3] * X[1][2] * X[2][0] * X[3][1] + X[0][2] * X[1][3] * X[2][0] * X[3][1]
                    + X[0][3] * X[1][0] * X[2][2] * X[3][1] - X[0][0] * X[1][3] * X[2][2] * X[3][1]
                    - X[0][2] * X[1][0] * X[2][3] * X[3][1] + X[0][0] * X[1][2] * X[2][3] * X[3][1]
                    + X[0][3] * X[1][1] * X[2][0] * X[3][2] - X[0][1] * X[1][3] * X[2][0] * X[3][2]
                    - X[0][3] * X[1][0] * X[2][1] * X[3][2] + X[0][0] * X[1][3] * X[2][1] * X[3][2]
                    + X[0][1] * X[1][0] * X[2][3] * X[3][2] - X[0][0] * X[1][1] * X[2][3] * X[3][2]
                    - X[0][2] * X[1][1] * X[2][0] * X[3][3] + X[0][1] * X[1][2] * X[2][0] * X[3][3]
                    + X[0][2] * X[1][0] * X[2][1] * X[3][3] - X[0][0] * X[1][2] * X[2][1] * X[3][3]
                    - X[0][1] * X[1][0] * X[2][2] * X[3][3] + X[0][0] * X[1][1] * X[2][2] * X[3][3];
  R[0][0] = (X[1][2] * X[2][3] * X[3][1] - X[1][3] * X[2][2] * X[3][1] + X[1][3] * X[2][1] * X[3][2]
             - X[1][1] * X[2][3] * X[3][2] - X[1][2] * X[2][1] * X[3][3] + X[1][1] * X[2][2] * X[3][3])
            / det;
  R[1][0] = (X[1][3] * X[2][2] * X[3][0] - X[1][2] * X[2][3] * X[3][0] - X[1][3] * X[2][0] * X[3][2]
             + X[1][0] * X[2][3] * X[3][2] + X[1][2] * X[2][0] * X[3][3] - X[1][0] * X[2][2] * X[3][3])
            / det;
  R[2][0] = (X[1][1] * X[2][3] * X[3][0] - X[1][3] * X[2][1] * X[3][0] + X[1][3] * X[2][0] * X[3][1]
             - X[1][0] * X[2][3] * X[3][1] - X[1][1] * X[2][0] * X[3][3] + X[1][0] * X[2][1] * X[3][3])
            / det;
  R[3][0] = (X[1][2] * X[2][1] * X[3][0] - X[1][1] * X[2][2] * X[3][0] - X[1][2] * X[2][0] * X[3][1]
             + X[1][0] * X[2][2] * X[3][1] + X[1][1] * X[2][0] * X[3][2] - X[1][0] * X[2][1] * X[3][2])
            / det;

  R[0][1] = (X[0][3] * X[2][2] * X[3][1] - X[0][2] * X[2][3] * X[3][1] - X[0][3] * X[2][1] * X[3][2]
             + X[0][1] * X[2][3] * X[3][2] + X[0][2] * X[2][1] * X[3][3] - X[0][1] * X[2][2] * X[3][3])
            / det;
  R[1][1] = (X[0][2] * X[2][3] * X[3][0] - X[0][3] * X[2][2] * X[3][0] + X[0][3] * X[2][0] * X[3][2]
             - X[0][0] * X[2][3] * X[3][2] - X[0][2] * X[2][0] * X[3][3] + X[0][0] * X[2][2] * X[3][3])
            / det;
  R[2][1] = (X[0][3] * X[2][1] * X[3][0] - X[0][1] * X[2][3] * X[3][0] - X[0][3] * X[2][0] * X[3][1]
             + X[0][0] * X[2][3] * X[3][1] + X[0][1] * X[2][0] * X[3][3] - X[0][0] * X[2][1] * X[3][3])
            / det;
  R[3][1] = (X[0][1] * X[2][2] * X[3][0] - X[0][2] * X[2][1] * X[3][0] + X[0][2] * X[2][0] * X[3][1]
             - X[0][0] * X[2][2] * X[3][1] - X[0][1] * X[2][0] * X[3][2] + X[0][0] * X[2][1] * X[3][2])
            / det;

  R[0][2] = (X[0][2] * X[1][3] * X[3][1] - X[0][3] * X[1][2] * X[3][1] + X[0][3] * X[1][1] * X[3][2]
             - X[0][1] * X[1][3] * X[3][2] - X[0][2] * X[1][1] * X[3][3] + X[0][1] * X[1][2] * X[3][3])
            / det;
  R[1][2] = (X[0][3] * X[1][2] * X[3][0] - X[0][2] * X[1][3] * X[3][0] - X[0][3] * X[1][0] * X[3][2]
             + X[0][0] * X[1][3] * X[3][2] + X[0][2] * X[1][0] * X[3][3] - X[0][0] * X[1][2] * X[3][3])
            / det;
  R[2][2] = (X[0][1] * X[1][3] * X[3][0] - X[0][3] * X[1][1] * X[3][0] + X[0][3] * X[1][0] * X[3][1]
             - X[0][0] * X[1][3] * X[3][1] - X[0][1] * X[1][0] * X[3][3] + X[0][0] * X[1][1] * X[3][3])
            / det;
  R[3][2] = (X[0][2] * X[1][1] * X[3][0] - X[0][1] * X[1][2] * X[3][0] - X[0][2] * X[1][0] * X[3][1]
             + X[0][0] * X[1][2] * X[3][1] + X[0][1] * X[1][0] * X[3][2] - X[0][0] * X[1][1] * X[3][2])
            / det;

  R[0][3] = (X[0][3] * X[1][2] * X[2][1] - X[0][2] * X[1][3] * X[2][1] - X[0][3] * X[1][1] * X[2][2]
             + X[0][1] * X[1][3] * X[2][2] + X[0][2] * X[1][1] * X[2][3] - X[0][1] * X[1][2] * X[2][3])
            / det;
  R[1][3] = (X[0][2] * X[1][3] * X[2][0] - X[0][3] * X[1][2] * X[2][0] + X[0][3] * X[1][0] * X[2][2]
             - X[0][0] * X[1][3] * X[2][2] - X[0][2] * X[1][0] * X[2][3] + X[0][0] * X[1][2] * X[2][3])
            / det;
  R[2][3] = (X[0][3] * X[1][1] * X[2][0] - X[0][1] * X[1][3] * X[2][0] - X[0][3] * X[1][0] * X[2][1]
             + X[0][0] * X[1][3] * X[2][1] + X[0][1] * X[1][0] * X[2][3] - X[0][0] * X[1][1] * X[2][3])
            / det;
  R[3][3] = (X[0][1] * X[1][2] * X[2][0] - X[0][2] * X[1][1] * X[2][0] + X[0][2] * X[1][0] * X[2][1]
             - X[0][0] * X[1][2] * X[2][1] - X[0][1] * X[1][0] * X[2][2] + X[0][0] * X[1][1] * X[2][2])
            / det;
}

static void mat4mulv(float *dst, float mat[][4], const float *const v)
{
  for(int k = 0; k < 4; k++)
  {
    float x = 0.0f;
    for(int i = 0; i < 4; i++) x += mat[k][i] * v[i];
    dst[k] = x;
  }
}

void dt_iop_estimate_cubic(const float *const x, const float *const y, float *a)
{
  // we want to fit a spline
  // [y]   [x^3 x^2 x^1 1] [a^3]
  // |y| = |x^3 x^2 x^1 1| |a^2|
  // |y|   |x^3 x^2 x^1 1| |a^1|
  // [y]   [x^3 x^2 x^1 1] [ 1 ]
  // and do that by inverting the matrix X:

  const float X[4][4] = { { x[0] * x[0] * x[0], x[0] * x[0], x[0], 1.0f },
                          { x[1] * x[1] * x[1], x[1] * x[1], x[1], 1.0f },
                          { x[2] * x[2] * x[2], x[2] * x[2], x[2], 1.0f },
                          { x[3] * x[3] * x[3], x[3] * x[3], x[3], 1.0f } };
  float X_inv[4][4];
  mat4inv(X, X_inv);
  mat4mulv(a, X_inv, y);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
