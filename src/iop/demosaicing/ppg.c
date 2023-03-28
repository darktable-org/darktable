/*
    This file is part of darktable,
    Copyright (C) 2010-2023 darktable developers.

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

#ifdef _OPENMP
  #pragma omp declare simd aligned(in, out)
#endif
static void demosaic_ppg(
        float *const out,
        const float *const in,
        const dt_iop_roi_t *const roi_out,
        const dt_iop_roi_t *const roi_in,
        const uint32_t filters,
        const float thrs)
{
  // these may differ a little, if you're unlucky enough to split a bayer block with cropping or similar.
  // we never want to access the input out of bounds though:
  assert(roi_in->width >= roi_out->width);
  assert(roi_in->height >= roi_out->height);
  // border interpolate
  float sum[8];
  for(int j = 0; j < roi_out->height; j++)
    for(int i = 0; i < roi_out->width; i++)
    {
      if(i == 3 && j >= 3 && j < roi_out->height - 3) i = roi_out->width - 3;
      if(i == roi_out->width) break;
      memset(sum, 0, sizeof(float) * 8);
      for(int y = j - 1; y != j + 2; y++)
        for(int x = i - 1; x != i + 2; x++)
        {
          const int yy = y + roi_out->y, xx = x + roi_out->x;
          if((yy >= 0) && (xx >= 0) && (yy < roi_in->height) && (xx < roi_in->width))
          {
            const int f = FC(y, x, filters);
            sum[f] += in[(size_t)yy * roi_in->width + xx];
            sum[f + 4]++;
          }
        }
      const int f = FC(j, i, filters);
      for(int c = 0; c < 3; c++)
      {
        if(c != f && sum[c + 4] > 0.0f)
          out[4 * ((size_t)j * roi_out->width + i) + c] = fmaxf(0.0f, sum[c] / sum[c + 4]);
        else
          out[4 * ((size_t)j * roi_out->width + i) + c]
              = fmaxf(0.0f, in[((size_t)j + roi_out->y) * roi_in->width + i + roi_out->x]);
      }
    }
  const int median = thrs > 0.0f;
  // if(median) fbdd_green(out, in, roi_out, roi_in, filters);
  const float *input = in;
  if(median)
  {
    float *med_in = (float *)dt_alloc_align_float((size_t)roi_in->height * roi_in->width);
    pre_median(med_in, in, roi_in, filters, 1, thrs);
    input = med_in;
  }
// for all pixels except those in the 3 pixel border:
// interpolate green from input into out float array, or copy color.
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(filters, out, roi_in, roi_out) \
  shared(input) \
  schedule(static)
#endif
  for(int j = 3; j < roi_out->height - 3; j++)
  {
    float *buf = out + (size_t)4 * roi_out->width * j + 4 * 3;
    const float *buf_in = input + (size_t)roi_in->width * (j + roi_out->y) + 3 + roi_out->x;
    for(int i = 3; i < roi_out->width - 3; i++)
    {
      const int c = FC(j, i, filters);
      dt_aligned_pixel_t color;
      const float pc = buf_in[0];
      // if(__builtin_expect(c == 0 || c == 2, 1))
      if(c == 0 || c == 2)
      {
        color[c] = pc;
        // get stuff (hopefully from cache)
        const float pym = buf_in[-roi_in->width * 1];
        const float pym2 = buf_in[-roi_in->width * 2];
        const float pym3 = buf_in[-roi_in->width * 3];
        const float pyM = buf_in[+roi_in->width * 1];
        const float pyM2 = buf_in[+roi_in->width * 2];
        const float pyM3 = buf_in[+roi_in->width * 3];
        const float pxm = buf_in[-1];
        const float pxm2 = buf_in[-2];
        const float pxm3 = buf_in[-3];
        const float pxM = buf_in[+1];
        const float pxM2 = buf_in[+2];
        const float pxM3 = buf_in[+3];

        const float guessx = (pxm + pc + pxM) * 2.0f - pxM2 - pxm2;
        const float diffx = (fabsf(pxm2 - pc) + fabsf(pxM2 - pc) + fabsf(pxm - pxM)) * 3.0f
                            + (fabsf(pxM3 - pxM) + fabsf(pxm3 - pxm)) * 2.0f;
        const float guessy = (pym + pc + pyM) * 2.0f - pyM2 - pym2;
        const float diffy = (fabsf(pym2 - pc) + fabsf(pyM2 - pc) + fabsf(pym - pyM)) * 3.0f
                            + (fabsf(pyM3 - pyM) + fabsf(pym3 - pym)) * 2.0f;
        if(diffx > diffy)
        {
          // use guessy
          const float m = fminf(pym, pyM);
          const float M = fmaxf(pym, pyM);
          color[1] = fmaxf(fminf(guessy * .25f, M), m);
        }
        else
        {
          const float m = fminf(pxm, pxM);
          const float M = fmaxf(pxm, pxM);
          color[1] = fmaxf(fminf(guessx * .25f, M), m);
        }
      }
      else
        color[1] = pc;

      color[3] = 0.0f;

      for_each_channel(k,aligned(buf,color:16)
        dt_omp_nontemporal(buf)) buf[k] = fmaxf(0.0f, color[k]);

      buf += 4;
      buf_in++;
    }
  }

// for all pixels except the outermost row/column:
// interpolate colors using out as input into float out array
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(filters, out, roi_out) \
  schedule(static)
#endif
  for(int j = 1; j < roi_out->height - 1; j++)
  {
    float *buf = out + (size_t)4 * roi_out->width * j + 4;
    for(int i = 1; i < roi_out->width - 1; i++)
    {
      // also prefetch direct nbs top/bottom
      const int c = FC(j, i, filters);
      dt_aligned_pixel_t color = { buf[0], buf[1], buf[2], buf[3] };

      // fill all four pixels with correctly interpolated stuff: r/b for green1/2
      // b for r and r for b
      if(__builtin_expect(c & 1, 1)) // c == 1 || c == 3)
      {
        // calculate red and blue for green pixels:
        // need 4-nbhood:
        const float *nt = buf - 4 * roi_out->width;
        const float *nb = buf + 4 * roi_out->width;
        const float *nl = buf - 4;
        const float *nr = buf + 4;
        if(FC(j, i + 1, filters) == 0) // red nb in same row
        {
          color[2] = (nt[2] + nb[2] + 2.0f * color[1] - nt[1] - nb[1]) * .5f;
          color[0] = (nl[0] + nr[0] + 2.0f * color[1] - nl[1] - nr[1]) * .5f;
        }
        else
        {
          // blue nb
          color[0] = (nt[0] + nb[0] + 2.0f * color[1] - nt[1] - nb[1]) * .5f;
          color[2] = (nl[2] + nr[2] + 2.0f * color[1] - nl[1] - nr[1]) * .5f;
        }
      }
      else
      {
        // get 4-star-nbhood:
        const float *ntl = buf - 4 - 4 * roi_out->width;
        const float *ntr = buf + 4 - 4 * roi_out->width;
        const float *nbl = buf - 4 + 4 * roi_out->width;
        const float *nbr = buf + 4 + 4 * roi_out->width;

        if(c == 0)
        {
          // red pixel, fill blue:
          const float diff1 = fabsf(ntl[2] - nbr[2]) + fabsf(ntl[1] - color[1]) + fabsf(nbr[1] - color[1]);
          const float guess1 = ntl[2] + nbr[2] + 2.0f * color[1] - ntl[1] - nbr[1];
          const float diff2 = fabsf(ntr[2] - nbl[2]) + fabsf(ntr[1] - color[1]) + fabsf(nbl[1] - color[1]);
          const float guess2 = ntr[2] + nbl[2] + 2.0f * color[1] - ntr[1] - nbl[1];
          if(diff1 > diff2)
            color[2] = guess2 * .5f;
          else if(diff1 < diff2)
            color[2] = guess1 * .5f;
          else
            color[2] = (guess1 + guess2) * .25f;
        }
        else // c == 2, blue pixel, fill red:
        {
          const float diff1 = fabsf(ntl[0] - nbr[0]) + fabsf(ntl[1] - color[1]) + fabsf(nbr[1] - color[1]);
          const float guess1 = ntl[0] + nbr[0] + 2.0f * color[1] - ntl[1] - nbr[1];
          const float diff2 = fabsf(ntr[0] - nbl[0]) + fabsf(ntr[1] - color[1]) + fabsf(nbl[1] - color[1]);
          const float guess2 = ntr[0] + nbl[0] + 2.0f * color[1] - ntr[1] - nbl[1];
          if(diff1 > diff2)
            color[0] = guess2 * .5f;
          else if(diff1 < diff2)
            color[0] = guess1 * .5f;
          else
            color[0] = (guess1 + guess2) * .25f;
        }
      }

      for_each_channel(k,aligned(buf,color:16)
        dt_omp_nontemporal(buf)) buf[k] = fmaxf(0.0f, color[k]);

      buf += 4;
    }
  }
  // _mm_sfence();
  if(median) dt_free_align((float *)input);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

