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
#include "common/image_compression.h"
#include "common/darktable.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef union
{
  float f;
  uint32_t i;
} dt_image_float_int_t;

void dt_image_uncompress(const uint8_t *in, float *out, const int32_t width, const int32_t height)
{
  dt_image_float_int_t L[16];
  float chrom[4][3];
  const dt_aligned_pixel_t fac = { 4., 2., 4. };
  uint16_t L16[16];
  int32_t n_zeroes, Lbias;
  uint8_t r[4], b[4];
  const uint8_t *block = in;
  for(int j = 0; j < height; j += 4)
  {
    for(int i = 0; i < width; i += 4)
    {
      // luma
      Lbias = (block[0] >> 3) << 10;
      n_zeroes = block[0] & 0x7;
      const int shift = 14 - n_zeroes - 4 + 1;

      for(int k = 0; k < 8; k++)
      {
        L16[2 * k] = ((int)(block[1 + k] >> 4) << shift) + Lbias;
        L16[2 * k + 1] = ((int)(block[1 + k] & 0xf) << shift) + Lbias;
      }
      for(int k = 0; k < 16; k++)
      {
        L[k].i = (((int)(L16[k]) >> 10) - (15 - 127)) << (23);
        L[k].i |= (L16[k] & 0x3ff) << 13;
      }
      // chroma
      r[0] = block[9] >> 1;
      b[0] = ((block[9] & 0x01) << 6) | (block[10] >> 2);
      r[1] = ((block[10] & 0x03) << 5) | (block[11] >> 3);
      b[1] = ((block[11] & 0x07) << 4) | (block[12] >> 4);
      r[2] = ((block[12] & 0x0f) << 3) | (block[13] >> 5);
      b[2] = ((block[13] & 0x1f) << 2) | (block[14] >> 6);
      r[3] = ((block[14] & 0x3f) << 1) | (block[15] >> 7);
      b[3] = block[15] & 0x7f;

      for(int q = 0; q < 4; q++)
      {
        chrom[q][0] = r[q] * (1. / 127.);
        chrom[q][2] = b[q] * (1. / 127.);
        chrom[q][1] = 1. - chrom[q][0] - chrom[q][2];
      }
      for(int k = 0; k < 16; k++)
        for(int c = 0; c < 3; c++)
          out[3 * (i + (k & 3) + width * (j + (k >> 2))) + c] = L[k].f * fac[c]
                                                                * chrom[((k >> 3) << 1) | ((k & 3) >> 1)][c];
      block += 16 * sizeof(uint8_t);
    }
  }
}

void dt_image_compress(const float *in, uint8_t *out, const int32_t width, const int32_t height)
{
  dt_image_float_int_t L[16];
  int16_t Lmin, Lmax, n_zeroes, L16[16];
  uint8_t *block = out, r[4], b[4];
  for(int j = 0; j < height; j += 4)
  {
    for(int i = 0; i < width; i += 4)
    {
      Lmin = 0x7fff;
      for(int q = 0; q < 4; q++)
      {
        dt_aligned_pixel_t chrom = { 0, 0, 0 };
        for(int pj = 0; pj < 2; pj++)
        {
          for(int pi = 0; pi < 2; pi++)
          {
            const int io = (pi + ((q & 1) << 1)), jo = (pj + (q & 2));
            const int ii = i + io, jj = j + jo;

            L[io + 4 * jo].f = (in[3 * (ii + width * jj) + 0] + 2 * in[3 * (ii + width * jj) + 1]
                                + in[3 * (ii + width * jj) + 2]) * .25;
            for(int k = 0; k < 3; k++) chrom[k] += L[io + 4 * jo].f * in[3 * (ii + width * jj) + k];
            L16[io + 4 * jo] = (L[io + 4 * jo].i >> 13) & 0x3ff;
            int e = ((L[io + 4 * jo].i >> (23)) - (127 - 15));
            e = e > 0 ? e : 0;
            e = e > 30 ? 30 : e;
            L16[io + 4 * jo] |= e << 10;
            Lmin = Lmin < L16[io + 4 * jo] ? Lmin : L16[io + 4 * jo];
          }
        }
        const float norm = 1. / (chrom[0] + 2 * chrom[1] + chrom[2]);
        r[q] = (int)(127. * (chrom[0] * norm));
        b[q] = (int)(127. * (chrom[2] * norm));
      }
      // store luma
      Lmin &= ~0x3ff;
      block[0] = (Lmin >> 10) << 3; // Lbias
      Lmax = 0;
      for(int k = 0; k < 16; k++)
      {
        L16[k] -= Lmin;
        Lmax = Lmax > L16[k] ? Lmax : L16[k];
      }
      n_zeroes = 0;
      for(int k = 1 << 14; (k & Lmax) == 0 && n_zeroes < 7; k >>= 1) n_zeroes++;
      block[0] |= n_zeroes;
      const int shift = 14 - n_zeroes - 4 + 1;
      const int off = (1 << shift) >> 1;
      for(int k = 0; k < 8; k++)
      {
        L16[2 * k] = ((int)L16[2 * k] + off) >> shift;
        L16[2 * k] = L16[2 * k] > 0xf ? 0xf : L16[2 * k];
        L16[2 * k + 1] = ((int)L16[2 * k + 1] + off) >> shift;
        L16[2 * k + 1] = L16[2 * k + 1] > 0xf ? 0xf : L16[2 * k + 1];
        block[k + 1] = L16[2 * k + 1] | (L16[2 * k] << 4);
      }
      // store chroma
      block[9] = (r[0] << 1) | (b[0] >> 6);
      block[10] = (b[0] << 2) | (r[1] >> 5);
      block[11] = (r[1] << 3) | (b[1] >> 4);
      block[12] = (b[1] << 4) | (r[2] >> 3);
      block[13] = (r[2] << 5) | (b[2] >> 2);
      block[14] = (b[2] << 6) | (r[3] >> 1);
      block[15] = (r[3] << 7) | (b[3] >> 0);
      block += 16 * sizeof(uint8_t);
    }
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

