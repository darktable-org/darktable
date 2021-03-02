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

inline float sqrf(float a)
{
  return (a * a);
}

inline float lab_f(float x)
{
  const float epsilon = 216.0f / 24389.0f;
  const float kappa = 24389.0f / 27.0f;
  return (x > epsilon) ? native_powr(x, (float)(1.0f/3.0f)) : (kappa * x + (float)16.0f) / ((float)116.0f);
}

inline float calcBlendFactor(float val, float threshold)
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
__kernel void rcd_write_output (__write_only image2d_t out, global float *rgb0, global float *rgb1, global float *rgb2, const int w, const int height, const float scale)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= w || row >= height) return;
  const int idx = mad24(row, w, col);

  write_imagef (out, (int2)(col, row), (float4)(scale * rgb0[idx], scale * rgb1[idx], scale * rgb2[idx], 0.0f));
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
  const int col = 4 + get_global_id(0);
  const int row = 4 + get_global_id(1);
  if((row > height - 5) || (col > w - 5)) return;
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
  const int row = 4 + get_global_id(1);
  const int col = 4 + (FC(row, 0, filters) & 1) + 2 *get_global_id(0);
  if((col > w - 4) || (row > height - 4)) return;
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

__kernel void dual_luminance_mask(global float *luminance, __read_only image2d_t in, const int w, const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if((col >= w) || (row >= height)) return;
  const int idx = mad24(row, w, col);

  float4 val = read_imagef(in, sampleri, (int2)(col, row));
  luminance[idx] = lab_f(0.333333333f * (val.x + val.y + val.z));    
}

__kernel void dual_calc_blend(global float *luminance, global float *mask, const int w, const int height, const float threshold)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if((col >= w) || (row >= height)) return;

  const int idx = mad24(row, w, col);
  if((col < 2) || (row < 2) || (col >= w - 2) || (row >= height - 2)) 
  {
    mask[idx] = 0.0f;
  }
  else
  {
    const int w2 = w * 2;
    const float scale = 1.0f / 16.0f;
    const float contrast = scale * native_sqrt(sqrf(luminance[idx+1] - luminance[idx-1]) + sqrf(luminance[idx +  w] - luminance[idx -  w]) +
                                         sqrf(luminance[idx+2] - luminance[idx-2]) + sqrf(luminance[idx + w2] - luminance[idx - w2]));
    mask[idx] = calcBlendFactor(contrast, threshold);
  }
}

__kernel void dual_blend_both(__read_only image2d_t high, __read_only image2d_t low, __write_only image2d_t out, const int w, const int height, global float *mask, const int showmask)
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
  write_imagef(out, (int2)(col, row), data);
}

__kernel void dual_fast_blur(global float *src, global float *out, const int w, const int height, const float c42, const float c41, const float c40,
                             const float c33, const float c32, const float c31, const float c30, const float c22, const float c21,
                             const float c20, const float c11, const float c10, const float c00)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if((col >= w) || (row >= height)) return;
  const int i = mad24(row, w, col);

  if((col < 5) || (row < 5) || (col > w - 5) || (row > height - 5)) 
  {
    out[i] = 0.0f;
  }
  else
  {
    const int w2 = 2 * w;
    const int w3 = 3 * w;
    const int w4 = 4 * w;
    const float val = c42 * (src[i - w4 - 2] + src[i - w4 + 2] + src[i - w2 - 4] + src[i - w2 + 4] + src[i + w2 - 4] + src[i + w2 + 4] + src[i + w4 - 2] + src[i + w4 + 2]) +
                      c41 * (src[i - w4 - 1] + src[i - w4 + 1] + src[i -  w - 4] + src[i -  w + 4] + src[i +  w - 4] + src[i +  w + 4] + src[i + w4 - 1] + src[i + w4 + 1]) +
                      c40 * (src[i - w4] + src[i - 4] + src[i + 4] + src[i + w4]) +
                      c33 * (src[i - w3 - 3] + src[i - w3 + 3] + src[i + w3 - 3] + src[i + w3 + 3]) +
                      c32 * (src[i - w3 - 2] + src[i - w3 + 2] + src[i - w2 - 3] + src[i - w2 + 3] + src[i + w2 - 3] + src[i + w2 + 3] + src[i + w3 - 2] + src[i + w3 + 2]) +
                      c31 * (src[i - w3 - 1] + src[i - w3 + 1] + src[i -  w - 3] + src[i -  w + 3] + src[i +  w - 3] + src[i +  w + 3] + src[i + w3 - 1] + src[i + w3 + 1]) +
                      c30 * (src[i - w3] + src[i - 3] + src[i + 3] + src[i + w3]) +
                      c22 * (src[i - w2 - 2] + src[i - w2 + 2] + src[i + w2 - 2] + src[i + w2 + 2]) +
                      c21 * (src[i - w2 - 1] + src[i - w2 + 1] + src[i -  w - 2] + src[i -  w + 2] + src[i +  w - 2] + src[i +  w + 2] + src[i + w2 - 1] + src[i + w2 + 1]) +
                      c20 * (src[i - w2] + src[i - 2] + src[i + 2] + src[i + w2]) +
                      c11 * (src[i -  w - 1] + src[i -  w + 1] + src[i +  w - 1] + src[i +  w + 1]) +
                      c10 * (src[i -  w] + src[i - 1] + src[i + 1] + src[i +  w]) +
                      c00 * src[i];
    out[i] = ICLAMP(val, 0.0f, 1.0f);
  }
}

kernel void rcd_border(global float *cfa, write_only image2d_t out, const int width, const int height, const unsigned int filters, const int border, const float scale)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  int avgwindow = 1;

  if(x>=border && x<width-border && y>=border && y<height-border) return;

  float4 o;
  const float4 scaler = scale;
  float sum[4] = { 0.0f };
  int count[4] = { 0 };

  for (int j=y-avgwindow; j<=y+avgwindow; j++) for (int i=x-avgwindow; i<=x+avgwindow; i++)
  {
    if (j>=0 && i>=0 && j<height && i<width)
    {
      int f = FC(j,i,filters);
      sum[f] += cfa[mad24(j, width, i)];
      count[f]++;
    }
  }

  float i = cfa[mad24(y, width, x)];
  o.x = count[0] > 0 ? sum[0]/count[0] : i;
  o.y = count[1]+count[3] > 0 ? (sum[1]+sum[3])/(count[1]+count[3]) : i;
  o.z = count[2] > 0 ? sum[2]/count[2] : i;

  int f = FC(y,x,filters);

  if     (f == 0) o.x = i;
  else if(f == 1) o.y = i;
  else if(f == 2) o.z = i;
  else            o.y = i;

  write_imagef (out, (int2)(x, y), o * scaler);
}

