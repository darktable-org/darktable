/*
    This file is part of darktable,
    rcd_cl implemented Hanno Schwalm (hanno@schwalm-bremen.de)

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

#include "common.h"

static inline float sqrf(float a)
{
  return (a * a);
}

static inline float calcBlendFactor(float val, float threshold)
{
    // sigmoid function
    // result is in ]0;1] range
    // inflexion point is at (x, y) (threshold, 0.5)
    return 1.0f / (1.0f + native_exp(16.0f - (16.0f / threshold) * val));
}

// Populate cfa and rgb data by normalized input
__kernel void rcd_populate (__read_only image2d_t in, global float *cfa, global float *rgb0, global float *rgb1, global float *rgb2, const int w, const int height, const unsigned int filters, const float scale)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= w || row >= height) return;
  const float val = scale * fmax(0.0f, read_imagef(in, sampleri, (int2)(col, row)).x);
  const int color = FC(row, col, filters);

  global float *rgbcol = rgb0;
  if(color == 1) rgbcol = rgb1;
  else if(color == 2) rgbcol = rgb2;

  const int idx = mad24(row, w, col);
  cfa[idx] = rgbcol[idx] = val;
}

// Write back-normalized data in rgb channels to output 
__kernel void rcd_write_output (__write_only image2d_t out, global float *rgb0, global float *rgb1, global float *rgb2, const int w, const int height, const float scale, const int border)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(!(col >= border && col < w - border && row >= border && row < height - border)) return;
  const int idx = mad24(row, w, col);

  write_imagef(out, (int2)(col, row), (float4)(fmax(scale * rgb0[idx], 0.0f), fmax(scale * rgb1[idx], 0.0f), fmax(scale * rgb2[idx], 0.0f), 0.0f));
}

// Step 1.1: Calculate a squared vertical and horizontal high pass filter on color differences
__kernel void rcd_step_1_1 (global float *cfa, global float *v_diff, global float *h_diff, const int w, const int height)
{
  const int col = 3 + get_global_id(0);
  const int row = 3 + get_global_id(1);
  if((row > height - 4) || (col > w - 4)) return;
  const int idx = mad24(row, w, col);
  const int w2 = 2 * w;
  const int w3 = 3 * w;

  v_diff[idx] = sqrf(cfa[idx - w3] - 3.0f * cfa[idx - w2] - cfa[idx - w] + 6.0f * cfa[idx] - cfa[idx + w] - 3.0f * cfa[idx + w2] + cfa[idx + w3]);
  h_diff[idx] = sqrf(cfa[idx -  3] - 3.0f * cfa[idx -  2] - cfa[idx - 1] + 6.0f * cfa[idx] - cfa[idx + 1] - 3.0f * cfa[idx +  2] + cfa[idx +  3]);
}

// Step 1.2: Calculate vertical and horizontal local discrimination
__kernel void rcd_step_1_2 (global float *VH_dir, global float *v_diff, global float *h_diff, const int w, const int height)
{
  const int col = 2 + get_global_id(0);
  const int row = 2 + get_global_id(1);
  if((row > height - 3) || (col > w - 3)) return;
  const int idx = mad24(row, w, col);
  const float eps = 1e-10f;

  const float V_Stat = fmax(eps, v_diff[idx - w] + v_diff[idx] + v_diff[idx + w]);
  const float H_Stat = fmax(eps, h_diff[idx - 1] + h_diff[idx] + h_diff[idx + 1]);
  VH_dir[idx] = V_Stat / (V_Stat + H_Stat);
}

// Step 2.1: Low pass filter incorporating green, red and blue local samples from the raw data
__kernel void rcd_step_2_1(global float *lpf, global float *cfa, const int w, const int height, const unsigned int filters)
{
  const int row = 2 + get_global_id(1);
  const int col = 2 + (FC(row, 0, filters) & 1) + 2 *get_global_id(0);
  if((col > w - 2) || (row > height - 2)) return;
  const int idx = mad24(row, w, col);

  lpf[idx / 2] = cfa[idx]
     + 0.5f * (cfa[idx - w    ] + cfa[idx + w    ] + cfa[idx     - 1] + cfa[idx     + 1])
    + 0.25f * (cfa[idx - w - 1] + cfa[idx - w + 1] + cfa[idx + w - 1] + cfa[idx + w + 1]);
}

// Step 3.1: Populate the green channel at blue and red CFA positions
__kernel void rcd_step_3_1(global float *lpf, global float *cfa, global float *rgb1, global float *VH_Dir, const int w, const int height, const unsigned int filters)
{
  const int row = 4 + get_global_id(1);
  const int col = 4 + (FC(row, 0, filters) & 1) + 2 * get_global_id(0);
  if((col > w - 5) || (row > height - 5)) return;
  const int idx = mad24(row, w, col);
  const int lidx = idx / 2;
  const int w2 = 2 * w;
  const int w3 = 3 * w;
  const int w4 = 4 * w;
  const float eps = 1e-5f;

  // Refined vertical and horizontal local discrimination
  const float VH_Central_Value   = VH_Dir[idx];
  const float VH_Neighbourhood_Value = 0.25f * (VH_Dir[idx - w - 1] + VH_Dir[idx - w + 1] + VH_Dir[idx + w - 1] + VH_Dir[idx + w + 1]);
  const float VH_Disc = (fabs( 0.5f - VH_Central_Value ) < fabs(0.5f - VH_Neighbourhood_Value)) ? VH_Neighbourhood_Value : VH_Central_Value;

  const float cfai = cfa[idx];
  // Cardinal gradients
  const float N_Grad = eps + fabs(cfa[idx - w] - cfa[idx + w]) + fabs(cfai - cfa[idx - w2]) + fabs(cfa[idx - w] - cfa[idx - w3]) + fabs(cfa[idx - w2] - cfa[idx - w4]);
  const float S_Grad = eps + fabs(cfa[idx + w] - cfa[idx - w]) + fabs(cfai - cfa[idx + w2]) + fabs(cfa[idx + w] - cfa[idx + w3]) + fabs(cfa[idx + w2] - cfa[idx + w4]);
  const float W_Grad = eps + fabs(cfa[idx - 1] - cfa[idx + 1]) + fabs(cfai - cfa[idx -  2]) + fabs(cfa[idx - 1] - cfa[idx -  3]) + fabs(cfa[idx -  2] - cfa[idx -  4]);
  const float E_Grad = eps + fabs(cfa[idx + 1] - cfa[idx - 1]) + fabs(cfai - cfa[idx +  2]) + fabs(cfa[idx + 1] - cfa[idx +  3]) + fabs(cfa[idx +  2] - cfa[idx +  4]);

  const float lfpi = lpf[lidx];
  // Cardinal pixel estimations
  const float N_Est = cfa[idx - w] * (lfpi + lfpi) / (eps + lfpi + lpf[lidx - w]);
  const float S_Est = cfa[idx + w] * (lfpi + lfpi) / (eps + lfpi + lpf[lidx + w]);
  const float W_Est = cfa[idx - 1] * (lfpi + lfpi) / (eps + lfpi + lpf[lidx - 1]);
  const float E_Est = cfa[idx + 1] * (lfpi + lfpi) / (eps + lfpi + lpf[lidx + 1]);

  // Vertical and horizontal estimations
  const float V_Est = (S_Grad * N_Est + N_Grad * S_Est) / (N_Grad + S_Grad);
  const float H_Est = (W_Grad * E_Est + E_Grad * W_Est) / (E_Grad + W_Grad);

  // G@B and G@R interpolation
  rgb1[idx] = mix(V_Est, H_Est, VH_Disc);
} 

// Step 4.0: Calculate the square of the P/Q diagonals color difference high pass filter
__kernel void rcd_step_4_1(global float *cfa, global float *p_diff, global float *q_diff, const int w, const int height, const unsigned int filters)
{
  const int row = 3 + get_global_id(1);
  const int col = 3 + 2 * get_global_id(0);
  if((col > w - 4) || (row > height - 4)) return;
  const int idx = mad24(row, w, col);
  const int idx2 = idx / 2;
  const int w2 = 2 * w;
  const int w3 = 3 * w;

  p_diff[idx2] = sqrf((cfa[idx - w3 - 3] - cfa[idx - w - 1] - cfa[idx + w + 1] + cfa[idx + w3 + 3]) - 3.0f * (cfa[idx - w2 - 2] + cfa[idx + w2 + 2]) + 6.0f * cfa[idx]);
  q_diff[idx2] = sqrf((cfa[idx - w3 + 3] - cfa[idx - w + 1] - cfa[idx + w - 1] + cfa[idx + w3 - 3]) - 3.0f * (cfa[idx - w2 + 2] + cfa[idx + w2 - 2]) + 6.0f * cfa[idx]);
}

// Step 4.1: Calculate P/Q diagonals local discrimination strength
__kernel void rcd_step_4_2(global float *PQ_dir, global float *p_diff, global float *q_diff, const int w, const int height, const unsigned int filters)
{
  const int row = 2 + get_global_id(1);
  const int col = 2 + (FC(row, 0, filters) & 1) + 2 *get_global_id(0);
  if((col > w - 3) || (row > height - 3)) return;
  const int idx = mad24(row, w, col);
  const int idx2 = idx / 2;
  const int idx3 = (idx - w - 1) / 2;
  const int idx4 = (idx + w - 1) / 2;  
  const float eps = 1e-10f;

  const float P_Stat = fmax(eps, p_diff[idx3]     + p_diff[idx2] + p_diff[idx4 + 1]);
  const float Q_Stat = fmax(eps, q_diff[idx3 + 1] + q_diff[idx2] + q_diff[idx4]);
  PQ_dir[idx2] = P_Stat / (P_Stat + Q_Stat);
}

// Step 4.2: Populate the red and blue channels at blue and red CFA positions
__kernel void rcd_step_5_1(global float *PQ_dir, global float *rgb0, global float *rgb1, global float *rgb2, const int w, const int height, const unsigned int filters)
{
  const int row = 4 + get_global_id(1);
  const int col = 4 + (FC(row, 0, filters) & 1) + 2 * get_global_id(0);
  if((col > w - 4) || (row > height - 4)) return;

  const int color = 2 - FC(row, col, filters);

  global float *rgbc = rgb0;
  if(color == 1) rgbc = rgb1;
  else if(color == 2) rgbc = rgb2;
 
  const int idx = mad24(row, w, col);
  const int pqidx = idx / 2;
  const int pqidx2 = (idx - w - 1) / 2;
  const int pqidx3 = (idx + w - 1) / 2;
  const int w2 = 2 * w;
  const int w3 = 3 * w;
  const float eps = 1e-5f;

  const float PQ_Central_Value   = PQ_dir[pqidx];
  const float PQ_Neighbourhood_Value = 0.25f * (PQ_dir[pqidx2] + PQ_dir[pqidx2 + 1] + PQ_dir[pqidx3] + PQ_dir[pqidx3 + 1]);
  const float PQ_Disc = (fabs(0.5f - PQ_Central_Value) < fabs(0.5f - PQ_Neighbourhood_Value)) ? PQ_Neighbourhood_Value : PQ_Central_Value;

  const float NW_Grad = eps + fabs(rgbc[idx - w - 1] - rgbc[idx + w + 1]) + fabs(rgbc[idx - w - 1] - rgbc[idx - w3 - 3]) + fabs(rgb1[idx] - rgb1[idx - w2 - 2]);
  const float NE_Grad = eps + fabs(rgbc[idx - w + 1] - rgbc[idx + w - 1]) + fabs(rgbc[idx - w + 1] - rgbc[idx - w3 + 3]) + fabs(rgb1[idx] - rgb1[idx - w2 + 2]);
  const float SW_Grad = eps + fabs(rgbc[idx - w + 1] - rgbc[idx + w - 1]) + fabs(rgbc[idx + w - 1] - rgbc[idx + w3 - 3]) + fabs(rgb1[idx] - rgb1[idx + w2 - 2]);
  const float SE_Grad = eps + fabs(rgbc[idx - w - 1] - rgbc[idx + w + 1]) + fabs(rgbc[idx + w + 1] - rgbc[idx + w3 + 3]) + fabs(rgb1[idx] - rgb1[idx + w2 + 2]);

  const float NW_Est = rgbc[idx - w - 1] - rgb1[idx - w - 1];
  const float NE_Est = rgbc[idx - w + 1] - rgb1[idx - w + 1];
  const float SW_Est = rgbc[idx + w - 1] - rgb1[idx + w - 1];
  const float SE_Est = rgbc[idx + w + 1] - rgb1[idx + w + 1];

  const float P_Est = (NW_Grad * SE_Est + SE_Grad * NW_Est) / (NW_Grad + SE_Grad);
  const float Q_Est = (NE_Grad * SW_Est + SW_Grad * NE_Est) / (NE_Grad + SW_Grad);

  rgbc[idx]= rgb1[idx] + mix(P_Est, Q_Est, PQ_Disc);
}

// Step 4.3: Populate the red and blue channels at green CFA positions
__kernel void rcd_step_5_2(global float *VH_dir, global float *rgb0, global float *rgb1, global float *rgb2, const int w, const int height, const unsigned int filters)
{
  const int row = 4 + get_global_id(1);
  const int col = 4 + (FC(row, 1, filters) & 1) + 2 * get_global_id(0);
  if((col > w - 4) || (row > height - 4)) return;

  const int idx = mad24(row, w, col);
  const int w2 = 2 * w;
  const int w3 = 3 * w;
  const float eps = 1e-5f;

  // Refined vertical and horizontal local discrimination
  const float VH_Central_Value   = VH_dir[idx];
  const float VH_Neighbourhood_Value = 0.25f * (VH_dir[idx - w - 1] + VH_dir[idx - w + 1] + VH_dir[idx + w - 1] + VH_dir[idx + w + 1]);
  const float VH_Disc = (fabs(0.5f - VH_Central_Value) < fabs(0.5f - VH_Neighbourhood_Value)) ? VH_Neighbourhood_Value : VH_Central_Value;

  const float rgbi1 = rgb1[idx];
  const float N1 = eps + fabs(rgbi1 - rgb1[idx - w2]);
  const float S1 = eps + fabs(rgbi1 - rgb1[idx + w2]);
  const float W1 = eps + fabs(rgbi1 - rgb1[idx -  2]);
  const float E1 = eps + fabs(rgbi1 - rgb1[idx +  2]);

  const float rgb1mw1 = rgb1[idx - w];
  const float rgb1pw1 = rgb1[idx + w];
  const float rgb1m1 =  rgb1[idx - 1];
  const float rgb1p1 =  rgb1[idx + 1];


  for(int c = 0; c <= 2; c += 2)
  {
    global float *rgbc = (c == 0) ? rgb0 : rgb2;

    const float SNabs = fabs(rgbc[idx - w] - rgbc[idx + w]);
    const float EWabs = fabs(rgbc[idx - 1] - rgbc[idx + 1]);
 
    // Cardinal gradients
    const float N_Grad = N1 + SNabs + fabs(rgbc[idx - w] - rgbc[idx - w3]);
    const float S_Grad = S1 + SNabs + fabs(rgbc[idx + w] - rgbc[idx + w3]);
    const float W_Grad = W1 + EWabs + fabs(rgbc[idx - 1] - rgbc[idx -  3]);
    const float E_Grad = E1 + EWabs + fabs(rgbc[idx + 1] - rgbc[idx +  3]);

    // Cardinal colour differences
    const float N_Est = rgbc[idx - w] - rgb1mw1;
    const float S_Est = rgbc[idx + w] - rgb1pw1;
    const float W_Est = rgbc[idx - 1] - rgb1m1;
    const float E_Est = rgbc[idx + 1] - rgb1p1;

    // Vertical and horizontal estimations
    const float V_Est = (N_Grad * S_Est + S_Grad * N_Est) / (N_Grad + S_Grad);
    const float H_Est = (E_Grad * W_Est + W_Grad * E_Est) / (E_Grad + W_Grad);

    // R@G and B@G interpolation
    rgbc[idx] = rgb1[idx] + mix(V_Est, H_Est, VH_Disc);
  }
}

__kernel void calc_Y0_mask(global float *mask, __read_only image2d_t in, const int w, const int height, const float red, const float green, const float blue)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if((col >= w) || (row >= height)) return;
  const int idx = mad24(row, w, col);

  float4 pt = read_imagef(in, sampleri, (int2)(col, row));
  const float val = ICLAMP(pt.x / red, 0.0f, 1.0f)
                  + ICLAMP(pt.y / green, 0.0f, 1.0f)
                  + ICLAMP(pt.z / blue, 0.0f, 1.0f);
  mask[idx] = native_sqrt(val / 3.0f);
}

__kernel void calc_scharr_mask(global float *in, global float *out, const int w, const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if((col >= w) || (row >= height)) return;

  const int oidx = mad24(row, w, col);

  int incol = col < 1 ? 1 : col;
  incol = col > w - 2 ? w - 2 : incol;
  int inrow = row < 1 ? 1 : row;
  inrow = row > height - 2 ? height - 2 : inrow;

  const int idx = mad24(inrow, w, incol); 

  // scharr operator
  const float gx = 47.0f * (in[idx-w-1] - in[idx-w+1])
                + 162.0f * (in[idx-1]   - in[idx+1])
                 + 47.0f * (in[idx+w-1] - in[idx+w+1]);
  const float gy = 47.0f * (in[idx-w-1] - in[idx+w-1])
                + 162.0f * (in[idx-w]   - in[idx+w])
                 + 47.0f * (in[idx-w+1] - in[idx+w+1]);
  const float gradient_magnitude = native_sqrt(sqrf(gx / 256.0f) + sqrf(gy / 256.0f));
  out[oidx] = gradient_magnitude / 16.0f;
}


__kernel void write_scharr_mask(global float *in, __write_only image2d_t out, const int w, const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if((col >= w) || (row >= height)) return;

  const int oidx = mad24(row, w, col);

  int incol = col < 1 ? 1 : col;
  incol = col > w - 2 ? w - 2 : incol;
  int inrow = row < 1 ? 1 : row;
  inrow = row > height - 2 ? height - 2 : inrow;

  const int idx = mad24(inrow, w, incol); 

  // scharr operator
  const float gx = 47.0f * (in[idx-w-1] - in[idx-w+1])
                + 162.0f * (in[idx-1]   - in[idx+1])
                 + 47.0f * (in[idx+w-1] - in[idx+w+1]);
  const float gy = 47.0f * (in[idx-w-1] - in[idx+w-1])
                + 162.0f * (in[idx-w]   - in[idx+w])
                 + 47.0f * (in[idx-w+1] - in[idx+w+1]);
  const float gradient_magnitude = native_sqrt(sqrf(gx / 256.0f) + sqrf(gy / 256.0f));
  write_imagef(out, (int2)(col, row), gradient_magnitude / 16.0f);
}


__kernel void calc_detail_blend(global float *in, global float *out, const int w, const int height, const float threshold, const int detail)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if((col >= w) || (row >= height)) return;

  const int idx = mad24(row, w, col); 

  const float blend = ICLAMP(calcBlendFactor(in[idx], threshold), 0.0f, 1.0f);
  out[idx] = detail ? blend : 1.0f - blend;
}

__kernel void readin_mask(global float *mask, __read_only image2d_t in, const int w, const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if((col >= w) || (row >= height)) return;

  const int idx = mad24(row, w, col);
  const float val = read_imagef(in, sampleri, (int2)(col, row)).x;
  mask[idx] = val;
}

__kernel void writeout_mask(global const float *mask, __write_only image2d_t out, const int w, const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if((col >= w) || (row >= height)) return;
  const int idx = mad24(row, w, col);

  const float val = mask[idx];
  write_imagef(out, (int2)(col, row), val);  
}

__kernel void write_blended_dual(__read_only image2d_t high, __read_only image2d_t low, __write_only image2d_t out, const int w, const int height, global float *mask, const int showmask)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if((col >= w) || (row >= height)) return;
  const int idx = mad24(row, w, col);

  float4 data;

  if(showmask)
  {
    data = mask[idx];
  }
  else
  {
    const float4 high_val = read_imagef(high, sampleri, (int2)(col, row));
    const float4 low_val = read_imagef(low, sampleri, (int2)(col, row));
    const float4 blender = mask[idx];
    data = mix(low_val, high_val, blender);
  }
  write_imagef(out, (int2)(col, row), fmax(data, 0.0f));
}

__kernel void fastblur_mask_9x9(global float *src, global float *out, const int w, const int height, global const float *kern)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if((col >= w) || (row >= height)) return;

  const int oidx = mad24(row, w, col);
  int incol = col < 4 ? 4 : col;
  incol = col > w - 5 ? w - 5 : incol;
  int inrow = row < 4 ? 4 : row;
  inrow = row > height - 5 ? height - 5 : inrow;
  const int i = mad24(inrow, w, incol); 

  const int w2 = 2 * w;
  const int w3 = 3 * w;
  const int w4 = 4 * w;
  const float val = kern[12] * (src[i - w4 - 2] + src[i - w4 + 2] + src[i - w2 - 4] + src[i - w2 + 4] + src[i + w2 - 4] + src[i + w2 + 4] + src[i + w4 - 2] + src[i + w4 + 2]) +
                    kern[11] * (src[i - w4 - 1] + src[i - w4 + 1] + src[i -  w - 4] + src[i -  w + 4] + src[i +  w - 4] + src[i +  w + 4] + src[i + w4 - 1] + src[i + w4 + 1]) +
                    kern[10] * (src[i - w4] + src[i - 4] + src[i + 4] + src[i + w4]) +
                    kern[9] * (src[i - w3 - 3] + src[i - w3 + 3] + src[i + w3 - 3] + src[i + w3 + 3]) +
                    kern[8] * (src[i - w3 - 2] + src[i - w3 + 2] + src[i - w2 - 3] + src[i - w2 + 3] + src[i + w2 - 3] + src[i + w2 + 3] + src[i + w3 - 2] + src[i + w3 + 2]) +
                    kern[7] * (src[i - w3 - 1] + src[i - w3 + 1] + src[i -  w - 3] + src[i -  w + 3] + src[i +  w - 3] + src[i +  w + 3] + src[i + w3 - 1] + src[i + w3 + 1]) +
                    kern[6] * (src[i - w3] + src[i - 3] + src[i + 3] + src[i + w3]) +
                    kern[5] * (src[i - w2 - 2] + src[i - w2 + 2] + src[i + w2 - 2] + src[i + w2 + 2]) +
                    kern[4] * (src[i - w2 - 1] + src[i - w2 + 1] + src[i -  w - 2] + src[i -  w + 2] + src[i +  w - 2] + src[i +  w + 2] + src[i + w2 - 1] + src[i + w2 + 1]) +
                    kern[3] * (src[i - w2] + src[i - 2] + src[i + 2] + src[i + w2]) +
                    kern[2] * (src[i -  w - 1] + src[i -  w + 1] + src[i +  w - 1] + src[i +  w + 1]) +
                    kern[1] * (src[i -  w] + src[i - 1] + src[i + 1] + src[i +  w]) +
                    kern[0] * src[i];
  out[oidx] = ICLAMP(val, 0.0f, 1.0f);
}

kernel void rcd_border_green(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
                    const unsigned int filters, local float *buffer, const int border)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int xlsz = get_local_size(0);
  const int ylsz = get_local_size(1);
  const int xlid = get_local_id(0);
  const int ylid = get_local_id(1);
  const int xgid = get_group_id(0);
  const int ygid = get_group_id(1);

  // individual control variable in this work group and the work group size
  const int l = mad24(ylid, xlsz, xlid);
  const int lsz = mul24(xlsz, ylsz);

  // stride and maximum capacity of local buffer
  // cells of 1*float per pixel with a surrounding border of 3 cells
  const int stride = xlsz + 2*3;
  const int maxbuf = mul24(stride, ylsz + 2*3);

  // coordinates of top left pixel of buffer
  // this is 3 pixel left and above of the work group origin
  const int xul = mul24(xgid, xlsz) - 3;
  const int yul = mul24(ygid, ylsz) - 3;

  // populate local memory buffer
  for(int n = 0; n <= maxbuf/lsz; n++)
  {
    const int bufidx = mad24(n, lsz, l);
    if(bufidx >= maxbuf) continue;
    const int xx = xul + bufidx % stride;
    const int yy = yul + bufidx / stride;
    buffer[bufidx] = fmax(0.0f, read_imagef(in, sampleri, (int2)(xx, yy)).x);
  }

  // center buffer around current x,y-Pixel
  buffer += mad24(ylid + 3, stride, xlid + 3);

  barrier(CLK_LOCAL_MEM_FENCE);

  if(x >= width - 3 || x < 3 || y >= height - 3 || y < 3) return;
  if(x >= border && x < width - border && y >= border && y < height - border) return;

  // process all non-green pixels
  const int row = y;
  const int col = x;
  const int c = FC(row, col, filters);
  float4 color = 0.0f; // output color

  const float pc = buffer[0];

  if     (c == 0) color.x = pc; // red
  else if(c == 1) color.y = pc; // green1
  else if(c == 2) color.z = pc; // blue
  else            color.y = pc; // green2

  // fill green layer for red and blue pixels:
  if(c == 0 || c == 2)
  {
    // look up horizontal and vertical neighbours, sharpened weight:
    const float pym  = buffer[-1 * stride];
    const float pym2 = buffer[-2 * stride];
    const float pym3 = buffer[-3 * stride];
    const float pyM  = buffer[ 1 * stride];
    const float pyM2 = buffer[ 2 * stride];
    const float pyM3 = buffer[ 3 * stride];
    const float pxm  = buffer[-1];
    const float pxm2 = buffer[-2];
    const float pxm3 = buffer[-3];
    const float pxM  = buffer[ 1];
    const float pxM2 = buffer[ 2];
    const float pxM3 = buffer[ 3];
    const float guessx = (pxm + pc + pxM) * 2.0f - pxM2 - pxm2;
    const float diffx  = (fabs(pxm2 - pc) +
                          fabs(pxM2 - pc) +
                          fabs(pxm  - pxM)) * 3.0f +
                         (fabs(pxM3 - pxM) + fabs(pxm3 - pxm)) * 2.0f;
    const float guessy = (pym + pc + pyM) * 2.0f - pyM2 - pym2;
    const float diffy  = (fabs(pym2 - pc) +
                          fabs(pyM2 - pc) +
                          fabs(pym  - pyM)) * 3.0f +
                         (fabs(pyM3 - pyM) + fabs(pym3 - pym)) * 2.0f;
    if(diffx > diffy)
    {
      // use guessy
      const float m = fmin(pym, pyM);
      const float M = fmax(pym, pyM);
      color.y = fmax(fmin(guessy*0.25f, M), m);
    }
    else
    {
      const float m = fmin(pxm, pxM);
      const float M = fmax(pxm, pxM);
      color.y = fmax(fmin(guessx*0.25f, M), m);
    }
  }
  write_imagef (out, (int2)(x, y), fmax(color, 0.0f));
}
kernel void rcd_border_redblue(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
                      const unsigned int filters, local float4 *buffer, const int border)
{
  // image in contains full green and sparse r b
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int xlsz = get_local_size(0);
  const int ylsz = get_local_size(1);
  const int xlid = get_local_id(0);
  const int ylid = get_local_id(1);
  const int xgid = get_group_id(0);
  const int ygid = get_group_id(1);

  // individual control variable in this work group and the work group size
  const int l = mad24(ylid, xlsz, xlid);
  const int lsz = mul24(xlsz, ylsz);

  // stride and maximum capacity of local buffer
  // cells of float4 per pixel with a surrounding border of 1 cell
  const int stride = xlsz + 2;
  const int maxbuf = mul24(stride, ylsz + 2);

  // coordinates of top left pixel of buffer
  // this is 1 pixel left and above of the work group origin
  const int xul = mul24(xgid, xlsz) - 1;
  const int yul = mul24(ygid, ylsz) - 1;

  // populate local memory buffer
  for(int n = 0; n <= maxbuf/lsz; n++)
  {
    const int bufidx = mad24(n, lsz, l);
    if(bufidx >= maxbuf) continue;
    const int xx = xul + bufidx % stride;
    const int yy = yul + bufidx / stride;
    buffer[bufidx] = fmax(0.0f, read_imagef(in, sampleri, (int2)(xx, yy)));
  }

  // center buffer around current x,y-Pixel
  buffer += mad24(ylid + 1, stride, xlid + 1);

  barrier(CLK_LOCAL_MEM_FENCE);

  if(x >= width || y >= height) return;
  if(x >= border && x < width - border && y >= border && y < height - border) return;

  const int row = y;
  const int col = x;
  const int c = FC(row, col, filters);
  float4 color = buffer[0];
  if(row > 0 && col > 0 && col < width - 1 && row < height - 1)
  { 
    if(c == 1 || c == 3)
    { // calculate red and blue for green pixels:
      // need 4-nbhood:
      float4 nt = buffer[-stride];
      float4 nb = buffer[ stride];
      float4 nl = buffer[-1];
      float4 nr = buffer[ 1];
      if(FC(row, col+1, filters) == 0) // red nb in same row
      {
        color.z = (nt.z + nb.z + 2.0f*color.y - nt.y - nb.y)*0.5f;
        color.x = (nl.x + nr.x + 2.0f*color.y - nl.y - nr.y)*0.5f;
      }
      else
      { // blue nb
        color.x = (nt.x + nb.x + 2.0f*color.y - nt.y - nb.y)*0.5f;
        color.z = (nl.z + nr.z + 2.0f*color.y - nl.y - nr.y)*0.5f;
      }
    }
    else
    {
      // get 4-star-nbhood:
      float4 ntl = buffer[-stride - 1];
      float4 ntr = buffer[-stride + 1];
      float4 nbl = buffer[ stride - 1];
      float4 nbr = buffer[ stride + 1];

      if(c == 0)
      { // red pixel, fill blue:
        const float diff1  = fabs(ntl.z - nbr.z) + fabs(ntl.y - color.y) + fabs(nbr.y - color.y);
        const float guess1 = ntl.z + nbr.z + 2.0f*color.y - ntl.y - nbr.y;
        const float diff2  = fabs(ntr.z - nbl.z) + fabs(ntr.y - color.y) + fabs(nbl.y - color.y);
        const float guess2 = ntr.z + nbl.z + 2.0f*color.y - ntr.y - nbl.y;
        if     (diff1 > diff2) color.z = guess2 * 0.5f;
        else if(diff1 < diff2) color.z = guess1 * 0.5f;
        else color.z = (guess1 + guess2)*0.25f;
      }
      else // c == 2, blue pixel, fill red:
      {
        const float diff1  = fabs(ntl.x - nbr.x) + fabs(ntl.y - color.y) + fabs(nbr.y - color.y);
        const float guess1 = ntl.x + nbr.x + 2.0f*color.y - ntl.y - nbr.y;
        const float diff2  = fabs(ntr.x - nbl.x) + fabs(ntr.y - color.y) + fabs(nbl.y - color.y);
        const float guess2 = ntr.x + nbl.x + 2.0f*color.y - ntr.y - nbl.y;
        if     (diff1 > diff2) color.x = guess2 * 0.5f;
        else if(diff1 < diff2) color.x = guess1 * 0.5f;
        else color.x = (guess1 + guess2)*0.25f;
      }
    }
  }
  write_imagef (out, (int2)(x, y), fmax(color, 0.0f));
}

