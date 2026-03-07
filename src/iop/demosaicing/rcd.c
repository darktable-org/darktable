/*
    This file is part of darktable,
    Copyright (C) 2010-2026 darktable developers.

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

/*
* RATIO CORRECTED DEMOSAICING
* Luis Sanz Rodríguez (luis.sanz.rodriguez(at)gmail(dot)com)
*
* Release 2.3 @ 171125
*
* Original code from https://github.com/LuisSR/RCD-Demosaicing
* Licensed under the GNU GPL version 3
*/

/*
* The tiling coding was done by Ingo Weyrich (heckflosse67@gmx.de) from rawtherapee
*/

/*
* Luis Sanz Rodríguez significantly optimised the v 2.3 code and simplified the directional
* coefficients in an exact, shorter and more performant formula.
* In cooperation with Ingo Weyrich and Luis Sanz Rodríguez this has been tuned for performance.
* Hanno Schwalm (hanno@schwalm-bremen.de)
*/

/* Some notes about the algorithm
* 1. The calculated data at the tiling borders RCD_BORDER must be 10 to be stable.
* 2. The tilesize has a significant influence on performance, the default is a good guess for modern
*    x86/64 machines, tested on Xeon E-2288G, i5-8250U.
*/

/* We don't want to use the -Ofast option in dt as it effects are not well specified and there have been issues
   leading to crashes.
   But we can use the 'fast-math' option in code sections if input data and algorithms are well defined.

   We have defined input data and make sure there are no divide-by-zero or overflows by chosen eps
   Reordering of instructions might lead to a slight loss of presision whigh is not significant here.
   Not necessary in this code section
     threadsafe handling of errno
     signed zero handling
     handling of math interrupts
     handling of rounding
     handling of overflows

   The 'fp-contract=fast' option enables fused multiply&add if available
*/

#ifdef __GNUC__
  #pragma GCC push_options
  #pragma GCC optimize ("fp-contract=fast", "finite-math-only", "no-math-errno")
#endif

#define RCD_BORDER 10 // avoid tile-overlap errors
#define RCD_TILEVALID (DT_RCD_TILESIZE - 2 * RCD_BORDER)
#define w1 DT_RCD_TILESIZE
#define w2 (2 * DT_RCD_TILESIZE)
#define w3 (3 * DT_RCD_TILESIZE)
#define w4 (4 * DT_RCD_TILESIZE)

#define eps 1e-5f              // Tolerance to avoid dividing by zero
#define epssq 1e-10f

// We might have negative data in input and also want to normalise
static inline float _safe_in(float a, float scale)
{
  return fmaxf(0.0f, a) * scale;
}

DT_OMP_DECLARE_SIMD(aligned(in, out : 64))
static void demosaic_box3(float *const restrict out,
                          const float *const restrict in,
                          const int width,
                          const int height,
                          const uint32_t filters,
                          const uint8_t(*const xtrans)[6])
{
  DT_OMP_FOR()
  for(int row = 0; row < height; row++)
  {
    for(int col = 0; col < width; col++)
    {
      dt_aligned_pixel_t sum = { 0.0f, 0.0f, 0.0f, 0.0f };
      dt_aligned_pixel_t cnt = { 0.0f, 0.0f, 0.0f, 0.0f };
      for(int y = row-1; y < row+2; y++)
      {
        for(int x = col-1; x < col+2; x++)
        {
          if(x >= 0 && y >= 0 && x < width && y < height)
          {
            const int color = (filters == 9u) ? FCNxtrans(y, x, xtrans) : FC(y, x, filters);
            sum[color] += MAX(0.0f, in[(size_t)width*y +x]);
            cnt[color] += 1.0f;
          }
        }
      }
      for_each_channel(c)
        out[((size_t)row * width + col)*4 + c] = sum[c] / MAX(1.0f, cnt[c]);
    }
  }
}

DT_OMP_DECLARE_SIMD(aligned(in, out : 64))
static void rcd_demosaic(float *const restrict out,
                         const float *const restrict in,
                         const int width,
                         const int height,
                         const uint32_t filters,
                         const float scaler)
{
  demosaic_ppg(out, in, width, height, filters, 0.0f, RCD_BORDER);
  if(width < 2*RCD_BORDER || height < 2*RCD_BORDER)
    return;

  const float revscaler = 1.0f / scaler;

  const int num_vertical = 1 + (height - 2 * RCD_BORDER -1) / RCD_TILEVALID;
  const int num_horizontal = 1 + (width - 2 * RCD_BORDER -1) / RCD_TILEVALID;

  DT_OMP_PRAGMA(parallel firstprivate(width, height, filters, out, in, scaler, revscaler))
  {
    // ensure that border elements which are read but never actually set below are zeroed out so use calloc
    float *const VH_Dir = dt_calloc_align_float((size_t) DT_RCD_TILESIZE * DT_RCD_TILESIZE);
    float *const PQ_Dir = dt_alloc_align_float((size_t) DT_RCD_TILESIZE * DT_RCD_TILESIZE / 2);
    float *const cfa =    dt_alloc_align_float((size_t) DT_RCD_TILESIZE * DT_RCD_TILESIZE);
    float *const P_CDiff_Hpf = dt_alloc_align_float((size_t) DT_RCD_TILESIZE * DT_RCD_TILESIZE / 2);
    float *const Q_CDiff_Hpf = dt_alloc_align_float((size_t) DT_RCD_TILESIZE * DT_RCD_TILESIZE / 2);

    float (*const rgb)[DT_RCD_TILESIZE * DT_RCD_TILESIZE] = (void *)dt_alloc_align_float((size_t)3 * DT_RCD_TILESIZE * DT_RCD_TILESIZE);

    // No overlapping use so re-use same buffer
    float *const lpf = PQ_Dir;

    DT_OMP_PRAGMA(for schedule(simd:static) collapse(2))
    for(int tile_vertical = 0; tile_vertical < num_vertical; tile_vertical++)
    {
      for(int tile_horizontal = 0; tile_horizontal < num_horizontal; tile_horizontal++)
      {
        const int rowStart = tile_vertical * RCD_TILEVALID;
        const int rowEnd = MIN(rowStart + DT_RCD_TILESIZE, height);

        const int colStart = tile_horizontal * RCD_TILEVALID;
        const int colEnd = MIN(colStart + DT_RCD_TILESIZE, width);

        const int tileRows = MIN(rowEnd - rowStart, DT_RCD_TILESIZE);
        const int tileCols = MIN(colEnd - colStart, DT_RCD_TILESIZE);

        if(rowStart + DT_RCD_TILESIZE > height || colStart + DT_RCD_TILESIZE > width)
        {
          // VH_Dir is only filled for(4,4)..(height-4,width-4), but the refinement code reads (3,3)...(h-3,w-3),
          // so we need to ensure that the border is zeroed for partial tiles to get consistent results
          memset(VH_Dir, 0, sizeof(*VH_Dir) * DT_RCD_TILESIZE * DT_RCD_TILESIZE);
          // TODO: figure out what part of rgb is being accessed without initialization on partial tiles
          memset(rgb, 0, sizeof(float) * 3 * DT_RCD_TILESIZE * DT_RCD_TILESIZE);
        }
        // Step 0: fill data and make sure data are not negative.
        for(int row = rowStart; row < rowEnd; row++)
        {
          const int c0 = FC(row, colStart, filters);
          const int c1 = FC(row, colStart + 1, filters);
          for(int col = colStart, indx = (row - rowStart) * DT_RCD_TILESIZE, in_indx = row * width + colStart; col < colEnd; col++, indx++, in_indx++)
          {
            cfa[indx] = rgb[c0][indx] = rgb[c1][indx] = _safe_in(in[in_indx], revscaler);
          }
        }

        // STEP 1: Find vertical and horizontal interpolation directions
        float bufferV[3][DT_RCD_TILESIZE - 8];
        // Step 1.1: Calculate the square of the vertical and horizontal color difference high pass filter
        for(int row = 3; row < MIN(tileRows - 3, 5); row++ )
        {
          for(int col = 4, indx = row * DT_RCD_TILESIZE + col; col < tileCols - 4; col++, indx++ )
          {
            bufferV[row - 3][col - 4] = sqrf((cfa[indx - w3] - cfa[indx - w1] - cfa[indx + w1] + cfa[indx + w3]) - 3.0f * (cfa[indx - w2] + cfa[indx + w2]) + 6.0f * cfa[indx]);
          }
        }

        // Step 1.2: Obtain the vertical and horizontal directional discrimination strength
        float DT_ALIGNED_PIXEL bufferH[DT_RCD_TILESIZE];
        // We start with V0, V1 and V2 pointing to row -1, row and row +1
        // After row is processed V0 must point to the old V1, V1 must point to the old V2 and V2 must point to the old V0
        // because the old V0 is not used anymore and will be filled with row + 1 data in next iteration
        float* V0 = bufferV[0];
        float* V1 = bufferV[1];
        float* V2 = bufferV[2];
        for(int row = 4; row < tileRows - 4; row++ )
        {
          for(int col = 3, indx = row * DT_RCD_TILESIZE + col; col < tileCols - 3; col++, indx++)
          {
            bufferH[col - 3] = sqrf((cfa[indx -  3] - cfa[indx -  1] - cfa[indx +  1] + cfa[indx +  3]) - 3.0f * (cfa[indx -  2] + cfa[indx +  2]) + 6.0f * cfa[indx]);
          }
          for(int col = 4, indx = (row + 1) * DT_RCD_TILESIZE + col; col < tileCols - 4; col++, indx++)
          {
            V2[col - 4] = sqrf((cfa[indx - w3] - cfa[indx - w1] - cfa[indx + w1] + cfa[indx + w3]) - 3.0f * (cfa[indx - w2] + cfa[indx + w2]) + 6.0f * cfa[indx]);
          }
          for(int col = 4, indx = row * DT_RCD_TILESIZE + col; col < tileCols - 4; col++, indx++ )
          {
            const float V_Stat = fmaxf(epssq,      V0[col - 4] +      V1[col - 4] +      V2[col - 4]);
            const float H_Stat = fmaxf(epssq, bufferH[col - 4] + bufferH[col - 3] + bufferH[col - 2]);
            VH_Dir[indx] = V_Stat / ( V_Stat + H_Stat );
          }
          // rolling the line pointers
          float* tmp = V0; V0 = V1; V1 = V2; V2 = tmp;
        }

        // STEP 2: Calculate the low pass filter
        // Step 2.1: Low pass filter incorporating green, red and blue local samples from the raw data
        for(int row = 2; row < tileRows - 2; row++)
        {
          for(int col = 2 + (FC(row, 0, filters) & 1), indx = row * DT_RCD_TILESIZE + col, lp_indx = indx / 2; col < tileCols - 2; col += 2, indx +=2, lp_indx++)
          {
            lpf[lp_indx] = cfa[indx]
                        + 0.5f * (cfa[indx - w1]     + cfa[indx + w1] +     cfa[indx - 1] +      cfa[indx + 1])
                       + 0.25f * (cfa[indx - w1 - 1] + cfa[indx - w1 + 1] + cfa[indx + w1 - 1] + cfa[indx + w1 + 1]);
          }
        }

        // STEP 3: Populate the green channel
        // Step 3.1: Populate the green channel at blue and red CFA positions
        for(int row = 4; row < tileRows - 4; row++)
        {
          for(int col = 4 + (FC(row, 0, filters) & 1), indx = row * DT_RCD_TILESIZE + col, lpindx = indx / 2; col < tileCols - 4; col += 2, indx += 2, lpindx++)
          {
            const float cfai = cfa[indx];

            // Cardinal gradients
            const float N_Grad = eps + fabsf(cfa[indx - w1] - cfa[indx + w1]) + fabsf(cfai - cfa[indx - w2]) + fabsf(cfa[indx - w1] - cfa[indx - w3]) + fabsf(cfa[indx - w2] - cfa[indx - w4]);
            const float S_Grad = eps + fabsf(cfa[indx - w1] - cfa[indx + w1]) + fabsf(cfai - cfa[indx + w2]) + fabsf(cfa[indx + w1] - cfa[indx + w3]) + fabsf(cfa[indx + w2] - cfa[indx + w4]);
            const float W_Grad = eps + fabsf(cfa[indx -  1] - cfa[indx +  1]) + fabsf(cfai - cfa[indx -  2]) + fabsf(cfa[indx -  1] - cfa[indx -  3]) + fabsf(cfa[indx -  2] - cfa[indx -  4]);
            const float E_Grad = eps + fabsf(cfa[indx -  1] - cfa[indx +  1]) + fabsf(cfai - cfa[indx +  2]) + fabsf(cfa[indx +  1] - cfa[indx +  3]) + fabsf(cfa[indx +  2] - cfa[indx +  4]);

            // Cardinal pixel estimations
            const float lpfi = lpf[lpindx];
            const float N_Est = cfa[indx - w1] * (lpfi + lpfi) / (eps + lpfi + lpf[lpindx - w1]);
            const float S_Est = cfa[indx + w1] * (lpfi + lpfi) / (eps + lpfi + lpf[lpindx + w1]);
            const float W_Est = cfa[indx -  1] * (lpfi + lpfi) / (eps + lpfi + lpf[lpindx -  1]);
            const float E_Est = cfa[indx +  1] * (lpfi + lpfi) / (eps + lpfi + lpf[lpindx +  1]);

            // Vertical and horizontal estimations
            const float V_Est = (S_Grad * N_Est + N_Grad * S_Est) / (N_Grad + S_Grad);
            const float H_Est = (W_Grad * E_Est + E_Grad * W_Est) / (E_Grad + W_Grad);

            // G@B and G@R interpolation
            // Refined vertical and horizontal local discrimination
            const float VH_Central_Value = VH_Dir[indx];
            const float VH_Neighbourhood_Value = 0.25f * (VH_Dir[indx - w1 - 1] + VH_Dir[indx - w1 + 1] + VH_Dir[indx + w1 - 1] + VH_Dir[indx + w1 + 1]);
            const float VH_Disc = (fabsf(0.5f - VH_Central_Value) < fabsf(0.5f - VH_Neighbourhood_Value)) ? VH_Neighbourhood_Value : VH_Central_Value;

            rgb[1][indx] = interpolatef(VH_Disc, H_Est, V_Est);
          }
        }

        // STEP 4: Populate the red and blue channels

        // Step 4.0: Calculate the square of the P/Q diagonals color difference high pass filter
        for(int row = 3; row < tileRows - 3; row++)
        {
          for(int col = 3, indx = row * DT_RCD_TILESIZE + col, indx2 = indx / 2; col < tileCols - 3; col+=2, indx+=2, indx2++)
          {
            P_CDiff_Hpf[indx2] = sqrf((cfa[indx - w3 - 3] - cfa[indx - w1 - 1] - cfa[indx + w1 + 1] + cfa[indx + w3 + 3]) - 3.0f * (cfa[indx - w2 - 2] + cfa[indx + w2 + 2]) + 6.0f * cfa[indx]);
            Q_CDiff_Hpf[indx2] = sqrf((cfa[indx - w3 + 3] - cfa[indx - w1 + 1] - cfa[indx + w1 - 1] + cfa[indx + w3 - 3]) - 3.0f * (cfa[indx - w2 + 2] + cfa[indx + w2 - 2]) + 6.0f * cfa[indx]);
          }
        }
        // Step 4.1: Obtain the P/Q diagonals directional discrimination strength
        for(int row = 4; row < tileRows - 4; row++)
        {
          for(int col = 4 + (FC(row, 0, filters) & 1), indx = row * DT_RCD_TILESIZE + col, indx2 = indx / 2, indx3 = (indx - w1 - 1) / 2, indx4 = (indx + w1 - 1) / 2; col < tileCols - 4; col += 2, indx += 2, indx2++, indx3++, indx4++ )
          {
            const float P_Stat = fmaxf(epssq, P_CDiff_Hpf[indx3]     + P_CDiff_Hpf[indx2] + P_CDiff_Hpf[indx4 + 1]);
            const float Q_Stat = fmaxf(epssq, Q_CDiff_Hpf[indx3 + 1] + Q_CDiff_Hpf[indx2] + Q_CDiff_Hpf[indx4]);
            PQ_Dir[indx2] = P_Stat / (P_Stat + Q_Stat);
          }
        }

        // Step 4.2: Populate the red and blue channels at blue and red CFA positions
        for(int row = 4; row < tileRows - 4; row++)
        {
          for(int col = 4 + (FC(row, 0, filters) & 1), indx = row * DT_RCD_TILESIZE + col, c = 2 - FC(row, col, filters), pqindx = indx / 2, pqindx2 = (indx - w1 - 1) / 2, pqindx3 = (indx + w1 - 1) / 2; col < tileCols - 4; col += 2, indx += 2, pqindx++, pqindx2++, pqindx3++)
          {
            // Refined P/Q diagonal local discrimination
            const float PQ_Central_Value   = PQ_Dir[pqindx];
            const float PQ_Neighbourhood_Value = 0.25f * (PQ_Dir[pqindx2] + PQ_Dir[pqindx2 + 1] + PQ_Dir[pqindx3] + PQ_Dir[pqindx3 + 1]);

            const float PQ_Disc = (fabsf(0.5f - PQ_Central_Value) < fabsf(0.5f - PQ_Neighbourhood_Value)) ? PQ_Neighbourhood_Value : PQ_Central_Value;

            // Diagonal gradients
            const float NW_Grad = eps + fabsf(rgb[c][indx - w1 - 1] - rgb[c][indx + w1 + 1]) + fabsf(rgb[c][indx - w1 - 1] - rgb[c][indx - w3 - 3]) + fabsf(rgb[1][indx] - rgb[1][indx - w2 - 2]);
            const float NE_Grad = eps + fabsf(rgb[c][indx - w1 + 1] - rgb[c][indx + w1 - 1]) + fabsf(rgb[c][indx - w1 + 1] - rgb[c][indx - w3 + 3]) + fabsf(rgb[1][indx] - rgb[1][indx - w2 + 2]);
            const float SW_Grad = eps + fabsf(rgb[c][indx - w1 + 1] - rgb[c][indx + w1 - 1]) + fabsf(rgb[c][indx + w1 - 1] - rgb[c][indx + w3 - 3]) + fabsf(rgb[1][indx] - rgb[1][indx + w2 - 2]);
            const float SE_Grad = eps + fabsf(rgb[c][indx - w1 - 1] - rgb[c][indx + w1 + 1]) + fabsf(rgb[c][indx + w1 + 1] - rgb[c][indx + w3 + 3]) + fabsf(rgb[1][indx] - rgb[1][indx + w2 + 2]);

            // Diagonal colour differences
            const float NW_Est = rgb[c][indx - w1 - 1] - rgb[1][indx - w1 - 1];
            const float NE_Est = rgb[c][indx - w1 + 1] - rgb[1][indx - w1 + 1];
            const float SW_Est = rgb[c][indx + w1 - 1] - rgb[1][indx + w1 - 1];
            const float SE_Est = rgb[c][indx + w1 + 1] - rgb[1][indx + w1 + 1];

            // P/Q estimations
            const float P_Est = (NW_Grad * SE_Est + SE_Grad * NW_Est) / (NW_Grad + SE_Grad);
            const float Q_Est = (NE_Grad * SW_Est + SW_Grad * NE_Est) / (NE_Grad + SW_Grad);

            // R@B and B@R interpolation
            rgb[c][indx] = rgb[1][indx] + interpolatef(PQ_Disc, Q_Est, P_Est);
          }
        }

        // Step 4.3: Populate the red and blue channels at green CFA positions
        for(int row = 4; row < tileRows - 4; row++)
        {
          for(int col = 4 + (FC(row, 1, filters) & 1), indx = row * DT_RCD_TILESIZE + col; col < tileCols - 4; col += 2, indx +=2)
          {
            // Refined vertical and horizontal local discrimination
            const float VH_Central_Value = VH_Dir[indx];
            const float VH_Neighbourhood_Value = 0.25f * (VH_Dir[indx - w1 - 1] + VH_Dir[indx - w1 + 1] + VH_Dir[indx + w1 - 1] + VH_Dir[indx + w1 + 1]);
            const float VH_Disc = (fabsf(0.5f - VH_Central_Value) < fabsf(0.5f - VH_Neighbourhood_Value) ) ? VH_Neighbourhood_Value : VH_Central_Value;
            const float rgb1 = rgb[1][indx];
            const float N1 = eps + fabsf(rgb1 - rgb[1][indx - w2]);
            const float S1 = eps + fabsf(rgb1 - rgb[1][indx + w2]);
            const float W1 = eps + fabsf(rgb1 - rgb[1][indx -  2]);
            const float E1 = eps + fabsf(rgb1 - rgb[1][indx +  2]);

            const float rgb1mw1 = rgb[1][indx - w1];
            const float rgb1pw1 = rgb[1][indx + w1];
            const float rgb1m1 =  rgb[1][indx - 1];
            const float rgb1p1 =  rgb[1][indx + 1];

            for(int c = 0; c <= 2; c += 2)
            {
              const float SNabs = fabs(rgb[c][indx - w1] - rgb[c][indx + w1]);
              const float EWabs = fabs(rgb[c][indx -  1] - rgb[c][indx +  1]);

              // Cardinal gradients
              const float N_Grad = N1 + SNabs + fabsf(rgb[c][indx - w1] - rgb[c][indx - w3]);
              const float S_Grad = S1 + SNabs + fabsf(rgb[c][indx + w1] - rgb[c][indx + w3]);
              const float W_Grad = W1 + EWabs + fabsf(rgb[c][indx -  1] - rgb[c][indx -  3]);
              const float E_Grad = E1 + EWabs + fabsf(rgb[c][indx +  1] - rgb[c][indx +  3]);

              // Cardinal colour differences
              const float N_Est = rgb[c][indx - w1] - rgb1mw1;
              const float S_Est = rgb[c][indx + w1] - rgb1pw1;
              const float W_Est = rgb[c][indx -  1] - rgb1m1;
              const float E_Est = rgb[c][indx +  1] - rgb1p1;

              // Vertical and horizontal estimations
              const float V_Est = (N_Grad * S_Est + S_Grad * N_Est) / (N_Grad + S_Grad);
              const float H_Est = (E_Grad * W_Est + W_Grad * E_Est) / (E_Grad + W_Grad);

              // R@G and B@G interpolation
              rgb[c][indx] = rgb1 + interpolatef(VH_Disc, H_Est, V_Est);
            }
          }
        }

        for(int row = rowStart + RCD_BORDER; row < rowEnd - RCD_BORDER; row++)
        {
          for(int col = colStart + RCD_BORDER, idx = (row - rowStart) * DT_RCD_TILESIZE + col - colStart, o_idx = (row * width + col) * 4;
              col < colEnd - RCD_BORDER;
              col++, o_idx += 4, idx++)
          {
            out[o_idx]   = scaler * fmaxf(0.0f, rgb[0][idx]);
            out[o_idx+1] = scaler * fmaxf(0.0f, rgb[1][idx]);
            out[o_idx+2] = scaler * fmaxf(0.0f, rgb[2][idx]);
            out[o_idx+3] = 0.0f;
          }
        }
      }
    }
    dt_free_align(cfa);
    dt_free_align(rgb);
    dt_free_align(VH_Dir);
    dt_free_align(PQ_Dir);
    dt_free_align(P_CDiff_Hpf);
    dt_free_align(Q_CDiff_Hpf);
  }
}

// revert rcd specific aggressive optimizing
#ifdef __GNUC__
  #pragma GCC pop_options
#endif

#ifdef HAVE_OPENCL
static cl_int process_rcd_cl(dt_iop_module_t *self,
                             dt_dev_pixelpipe_iop_t *piece,
                             cl_mem dev_in,
                             cl_mem dev_out,
                             const int width,
                             const int height,
                             const uint32_t filters)
{
  const dt_iop_demosaic_global_data_t *gd = self->global_data;

  const int devid = piece->pipe->devid;

  cl_mem dev_tmp = NULL;
  cl_mem cfa = NULL;
  cl_mem rgb0 = NULL;
  cl_mem rgb1 = NULL;
  cl_mem rgb2 = NULL;
  cl_mem VH_dir = NULL;
  cl_mem PQ_dir = NULL;
  cl_mem VP_diff = NULL;
  cl_mem HQ_diff = NULL;

  cl_int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;


  dev_tmp = dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
  if(dev_tmp == NULL) goto error;

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_border_interpolate, width, height,
        CLARG(dev_in), CLARG(dev_tmp), CLARG(width), CLARG(height), CLARG(filters));
  if(err != CL_SUCCESS) goto error;

  {
    dt_opencl_local_buffer_t locopt
        = (dt_opencl_local_buffer_t){ .xoffset = 2*3, .xfactor = 1, .yoffset = 2*3, .yfactor = 1,
                                      .cellsize = sizeof(float) * 1, .overhead = 0,
                                      .sizex = 64, .sizey = 64 };

    err = dt_opencl_local_buffer_opt(devid, gd->kernel_ppg_green, &locopt);
    if(err != CL_SUCCESS) goto error;

    size_t sizes[3] = { ROUNDUP(width, locopt.sizex), ROUNDUP(height, locopt.sizey), 1 };
    size_t local[3] = { locopt.sizex, locopt.sizey, 1 };
    err = dt_opencl_enqueue_kernel_2d_local_args(devid, gd->kernel_ppg_green, sizes, local,
        CLARG(dev_in), CLARG(dev_tmp), CLARG(width), CLARG(height), CLARG(filters),
        CLLOCAL(sizeof(float) * (locopt.sizex + 2*3) * (locopt.sizey + 2*3)), CLARGINT(32));
    if(err != CL_SUCCESS) goto error;
  }

  {
    dt_opencl_local_buffer_t locopt
        = (dt_opencl_local_buffer_t){ .xoffset = 2*1, .xfactor = 1, .yoffset = 2*1, .yfactor = 1,
                                      .cellsize = 4 * sizeof(float), .overhead = 0,
                                      .sizex = 64, .sizey = 64 };

    err = dt_opencl_local_buffer_opt(devid, gd->kernel_ppg_redblue, &locopt);
    if(err != CL_SUCCESS) goto error;

    size_t sizes[3] = { ROUNDUP(width, locopt.sizex), ROUNDUP(height, locopt.sizey), 1 };
    size_t local[3] = { locopt.sizex, locopt.sizey, 1 };
    err = dt_opencl_enqueue_kernel_2d_local_args(devid, gd->kernel_ppg_redblue, sizes, local,
      CLARG(dev_tmp), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(filters),
      CLLOCAL(sizeof(float) * 4 * (locopt.sizex + 2) * (locopt.sizey + 2)), CLARGINT(16));
    if(err != CL_SUCCESS) goto error;
  }
  dt_opencl_release_mem_object(dev_tmp);
  dev_tmp = NULL;

  const size_t bsize = sizeof(float) * width * height;
  err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
  cfa = dt_opencl_alloc_device_buffer(devid, bsize);
  VH_dir = dt_opencl_alloc_device_buffer(devid, bsize);
  PQ_dir = dt_opencl_alloc_device_buffer(devid, bsize);
  VP_diff = dt_opencl_alloc_device_buffer(devid, bsize);
  HQ_diff = dt_opencl_alloc_device_buffer(devid, bsize);
  rgb0 = dt_opencl_alloc_device_buffer(devid, bsize);
  rgb1 = dt_opencl_alloc_device_buffer(devid, bsize);
  rgb2 = dt_opencl_alloc_device_buffer(devid, bsize);
  if(cfa == NULL || VH_dir == NULL || PQ_dir == NULL || VP_diff == NULL || HQ_diff == NULL || rgb0 == NULL || rgb1 == NULL || rgb2 == NULL)
    goto error;

  // populate data
  const float scaler = ceilf(dt_iop_get_processed_maximum(piece));
  const float revscaler = 1.0f / scaler;
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_rcd_populate, width, height,
        CLARG(dev_in), CLARG(cfa), CLARG(rgb0), CLARG(rgb1), CLARG(rgb2), CLARG(width), CLARG(height),
        CLARG(filters), CLARG(revscaler));
  if(err != CL_SUCCESS) goto error;

  // Step 1.1: Calculate a squared vertical and horizontal high pass filter on color differences
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_rcd_step_1_1, width, height,
        CLARG(cfa), CLARG(VP_diff), CLARG(HQ_diff), CLARG(width), CLARG(height));
  if(err != CL_SUCCESS) goto error;

  // Step 1.2: Calculate vertical and horizontal local discrimination
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_rcd_step_1_2, width, height,
        CLARG(VH_dir), CLARG(VP_diff), CLARG(HQ_diff), CLARG(width), CLARG(height));
  if(err != CL_SUCCESS) goto error;

  // Step 2.1: Low pass filter incorporating green, red and blue local samples from the raw data
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_rcd_step_2_1, width / 2, height,
        CLARG(PQ_dir), CLARG(cfa), CLARG(width), CLARG(height), CLARG(filters));
  if(err != CL_SUCCESS) goto error;

  // Step 3.1: Populate the green channel at blue and red CFA positions
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_rcd_step_3_1, width / 2, height,
      CLARG(PQ_dir), CLARG(cfa), CLARG(rgb1), CLARG(VH_dir), CLARG(width), CLARG(height), CLARG(filters));
  if(err != CL_SUCCESS) goto error;

    // Step 4.1: Calculate a squared P/Q diagonals high pass filter on color differences
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_rcd_step_4_1, width / 2, height,
      CLARG(cfa), CLARG(VP_diff), CLARG(HQ_diff), CLARG(width), CLARG(height), CLARG(filters));
  if(err != CL_SUCCESS) goto error;

    // Step 4.2: Calculate P/Q diagonal local discrimination
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_rcd_step_4_2, width / 2, height,
        CLARG(PQ_dir), CLARG(VP_diff), CLARG(HQ_diff), CLARG(width), CLARG(height), CLARG(filters));
  if(err != CL_SUCCESS) goto error;

  // Step 4.3: Populate the red and blue channels at blue and red CFA positions
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_rcd_step_5_1, width / 2, height,
        CLARG(PQ_dir), CLARG(rgb0), CLARG(rgb1), CLARG(rgb2), CLARG(width), CLARG(height), CLARG(filters));
  if(err != CL_SUCCESS) goto error;

  // Step 5.2: Populate the red and blue channels at green CFA positions
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_rcd_step_5_2, width / 2, height,
        CLARG(VH_dir), CLARG(rgb0), CLARG(rgb1), CLARG(rgb2), CLARG(width), CLARG(height), CLARG(filters));
  if(err != CL_SUCCESS) goto error;

  // write output
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_rcd_write_output, width, height,
        CLARG(dev_out), CLARG(rgb0), CLARG(rgb1), CLARG(rgb2), CLARG(width), CLARG(height), CLARG(scaler), CLARGINT(RCD_BORDER));

error:
  dt_opencl_release_mem_object(dev_tmp);
  dt_opencl_release_mem_object(cfa);
  dt_opencl_release_mem_object(rgb0);
  dt_opencl_release_mem_object(rgb1);
  dt_opencl_release_mem_object(rgb2);
  dt_opencl_release_mem_object(VH_dir);
  dt_opencl_release_mem_object(PQ_dir);
  dt_opencl_release_mem_object(VP_diff);
  dt_opencl_release_mem_object(HQ_diff);

  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL, "[opencl_demosaic] rcd problem '%s'", cl_errstr(err));
  return err;
}

#endif

#undef RCD_BORDER
#undef RCD_TILEVALID
#undef w1
#undef w2
#undef w3
#undef w4
#undef eps
#undef epssq

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
