/*
    This file is part of darktable,
    copyright (c) 2009--2012 johannes hanika.

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

const sampler_t sampleri =  CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST;
const sampler_t samplerf =  CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_LINEAR;

int
FC(const int row, const int col, const unsigned int filters)
{
  return filters >> ((((row) << 1 & 14) + ((col) & 1)) << 1) & 3;
}


kernel void
whitebalance_1ui(read_only image2d_t in, write_only image2d_t out, const int width, const int height, global float *coeffs,
    const unsigned int filters, const int rx, const int ry)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const uint4 pixel = read_imageui(in, sampleri, (int2)(x, y));
  write_imagef (out, (int2)(x, y), (float4)(pixel.x * coeffs[FC(ry+y, rx+x, filters)], 0.0f, 0.0f, 0.0f));
}

kernel void
whitebalance_4f(read_only image2d_t in, write_only image2d_t out, const int width, const int height, global float *coeffs)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  write_imagef (out, (int2)(x, y), (float4)(pixel.x * coeffs[0], pixel.y * coeffs[1], pixel.z * coeffs[2], pixel.w));
}

/* kernel for the exposure plugin. should work transparently with float4 and float image2d. */
kernel void
exposure (read_only image2d_t in, write_only image2d_t out, const int width, const int height, const float black, const float scale)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;
  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  pixel = (pixel - black)*scale;
  write_imagef (out, (int2)(x, y), pixel);
}

/* helpers for the highlights plugin: convert to lch. */

constant float xyz_rgb[9] = {  /* XYZ from RGB */
	  0.412453, 0.357580, 0.180423,
	  0.212671, 0.715160, 0.072169,
	  0.019334, 0.119193, 0.950227};
constant float rgb_xyz[9] = {  /* RGB from XYZ */
	  3.24048, -1.53715, -0.498536,
	  -0.969255, 1.87599, 0.0415559,
	  0.0556466, -0.204041, 1.05731};

void
rgb_to_lch (float *rgb, float *lch)
{
	float xyz[3], lab[3];
	xyz[0] = xyz[1] = xyz[2] = 0.0f;
	for (int c=0; c<3; c++)
		for (int cc=0; cc<3; cc++)
			xyz[cc] += xyz_rgb[3*cc+c] * rgb[c];
	for (int c=0; c<3; c++)
		xyz[c] = xyz[c] > 0.008856f ? native_powr(xyz[c], 1.0f/3.0f) : 7.787f*xyz[c] + 16.0f/116.0f;
	lab[0] = 116.0f * xyz[1] - 16.0f;
	lab[1] = 500.0f * (xyz[0] - xyz[1]);
	lab[2] = 200.0f * (xyz[1] - xyz[2]);

	lch[0] = lab[0];
	lch[1] = native_sqrt(lab[1]*lab[1]+lab[2]*lab[2]);
	lch[2] = atan2(lab[2], lab[1]);
}

// convert CIE-LCh to linear RGB
void
lch_to_rgb(float *lch, float *rgb)
{
	float xyz[3], lab[3];
	const float epsilon = 0.008856f, kappa = 903.3f;
	lab[0] = lch[0];
	lab[1] = lch[1] * native_cos(lch[2]);
	lab[2] = lch[1] * native_sin(lch[2]);
	xyz[1] = (lab[0]<=kappa*epsilon) ?
		(lab[0]/kappa) : (native_powr((lab[0]+16.0f)/116.0f, 3.0f));
	const float fy = (xyz[1]<=epsilon) ? ((kappa*xyz[1]+16.0f)/116.0f) : ((lab[0]+16.0f)/116.0f);
	const float fz = fy - lab[2]/200.0f;
	const float fx = lab[1]/500.0f + fy;
	xyz[2] = (native_powr(fz, 3.0f)<=epsilon) ? ((116.0f*fz-16.0f)/kappa) : (native_powr(fz, 3.0f));
	xyz[0] = (native_powr(fx, 3.0f)<=epsilon) ? ((116.0f*fx-16.0f)/kappa) : (native_powr(fx, 3.0f));

	for (int c=0; c<3; c++)
	{
		float tmpf = 0.0f;
		for (int cc=0; cc<3; cc++)
			tmpf += rgb_xyz[3*c+cc] * xyz[cc];
		rgb[c] = fmax(tmpf, 0.0f);
	}
}

/* kernel for the highlights plugin. */
kernel void
highlights (read_only image2d_t in, write_only image2d_t out, const int width, const int height,
            const int mode, const float clip, const float blendL, const float blendC, const float blendh)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  float4 inc, lchi, lchc, lch;
  switch(mode)
  {
    case 1: // DT_IOP_HIGHLIGHTS_LCH
      inc.x = fmin(clip, pixel.x);
      inc.y = fmin(clip, pixel.y);
      inc.z = fmin(clip, pixel.z);
      rgb_to_lch((float *)&pixel, (float *)&lchi);
      rgb_to_lch((float *)&inc, (float *)&lchc);
      lch.x = lchc.x + blendL * (lchi.x - lchc.x);
      lch.y = lchc.y + blendC * (lchi.y - lchc.y);
      lch.z = lchc.z + blendh * (lchi.z - lchc.z);
      lch_to_rgb((float *)&lch, (float *)&pixel);
      break;
    default: // 0, DT_IOP_HIGHLIGHTS_CLIP
      pixel.x = fmin(clip, pixel.x);
      pixel.y = fmin(clip, pixel.y);
      pixel.z = fmin(clip, pixel.z);
      break;
  }
  write_imagef (out, (int2)(x, y), pixel);
}

float
lookup_unbounded(read_only image2d_t lut, const float x, global float *a)
{
  // in case the tone curve is marked as linear, return the fast
  // path to linear unbounded (does not clip x at 1)
  if(a[0] >= 0.0f)
  {
    if(x < 1.0f)
    {
      const int xi = clamp(x*65535.0f, 0.0f, 65535.0f);
      const int2 p = (int2)((xi & 0xff), (xi >> 8));
      return read_imagef(lut, sampleri, p).x;
    }
    else return a[0] * native_powr(x, a[1]);
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

/* kernel for the basecurve plugin. */
kernel void
basecurve (read_only image2d_t in, write_only image2d_t out, const int width, const int height,
           read_only image2d_t table, global float *a)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  // use lut or extrapolation:
  pixel.x = lookup_unbounded(table, pixel.x, a);
  pixel.y = lookup_unbounded(table, pixel.y, a);
  pixel.z = lookup_unbounded(table, pixel.z, a);
  write_imagef (out, (int2)(x, y), pixel);
}


void
XYZ_to_Lab(float *xyz, float *lab)
{
  xyz[0] *= (1.0f/0.9642f);
  xyz[2] *= (1.0f/0.8242f);
	for (int c=0; c<3; c++)
		xyz[c] = xyz[c] > 0.008856f ? native_powr(xyz[c], 1.0f/3.0f) : 7.787f*xyz[c] + 16.0f/116.0f;
	lab[0] = 116.0f * xyz[1] - 16.0f;
	lab[1] = 500.0f * (xyz[0] - xyz[1]);
	lab[2] = 200.0f * (xyz[1] - xyz[2]);
}

/* kernel for the plugin colorin */
kernel void
colorin (read_only image2d_t in, write_only image2d_t out, const int width, const int height,
         global float *mat, read_only image2d_t lutr, read_only image2d_t lutg, read_only image2d_t lutb,
         const int map_blues,
         global float *a)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));

  float cam[3], XYZ[3], Lab[3];
  cam[0] = lookup_unbounded(lutr, pixel.x, a);
  cam[1] = lookup_unbounded(lutg, pixel.y, a+2);
  cam[2] = lookup_unbounded(lutb, pixel.z, a+4);

  if(map_blues)
  {
    // manual gamut mapping. these values cause trouble when converting back from Lab to sRGB:
    const float YY = cam[0]+cam[1]+cam[2];
    const float zz = cam[2]/YY;
    // lower amount and higher bound_z make the effect smaller.
    // the effect is weakened the darker input values are, saturating at bound_Y
    const float bound_z = 0.5f, bound_Y = 0.8f;
    const float amount = 0.11f;
    if (zz > bound_z)
    {
      const float t = (zz - bound_z)/(1.0f-bound_z) * fmin(1.0f, YY/bound_Y);
      cam[1] += t*amount;
      cam[2] -= t*amount;
    }
  }
  // now convert camera to XYZ using the color matrix
  for(int j=0;j<3;j++)
  {
    XYZ[j] = 0.0f;
    for(int i=0;i<3;i++) XYZ[j] += mat[3*j+i] * cam[i];
  }
  XYZ_to_Lab(XYZ, (float *)&pixel);
  write_imagef (out, (int2)(x, y), pixel);
}

/* kernel for the tonecurve plugin version 2 */
kernel void
tonecurve (read_only image2d_t in, write_only image2d_t out, const int width, const int height,
           read_only image2d_t table_L, read_only image2d_t table_a, read_only image2d_t table_b,
           const int autoscale_ab, global float *a)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  const float L_in = pixel.x/100.0f;
  // use lut or extrapolation:
  const float L = lookup_unbounded(table_L, L_in, a);
  if (autoscale_ab == 0)
  {
    const float a_in = (pixel.y + 128.0f) / 256.0f;
    const float b_in = (pixel.z + 128.0f) / 256.0f;
    pixel.y = lookup(table_a, a_in);
    pixel.z = lookup(table_b, b_in);
  }
  else if(pixel.x > 0.01f)
  {
    pixel.y *= L/pixel.x;
    pixel.z *= L/pixel.x;
  }
  else
  {
    pixel.y *= L/0.01f;
    pixel.z *= L/0.01f;
  }
  pixel.x = L;
  write_imagef (out, (int2)(x, y), pixel);
}


/* kernel for the colorcorrection plugin. */
__kernel void
colorcorrection (read_only image2d_t in, write_only image2d_t out, const int width, const int height,
                 const float saturation, const float a_scale, const float a_base, 
                 const float b_scale, const float b_base)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  pixel.y = saturation*(pixel.y + pixel.x * a_scale + a_base);
  pixel.z = saturation*(pixel.z + pixel.x * b_scale + b_base);
  write_imagef (out, (int2)(x, y), pixel);
}


void
mul_mat_vec_2(const float4 m, const float2 *p, float2 *o)
{
  (*o).x = (*p).x*m.x + (*p).y*m.y;
  (*o).y = (*p).x*m.z + (*p).y*m.w;
}

void
backtransform(float2 *p, float2 *o, const float4 m, const float2 t)
{
  (*p).y /= (1.0f + (*p).x*t.x);
  (*p).x /= (1.0f + (*p).y*t.y);
  mul_mat_vec_2(m, p, o);
}


/* kernel for clip&rotate */
__kernel void
clip_rotate(read_only image2d_t in, write_only image2d_t out, const int width, const int height, 
            const int in_width, const int in_height,
            const int2 roi_in, const int2 roi_out, const float scale_in, const float scale_out,
            const int flip, const float2 ci, const float2 t, const float2 k, const float4 mat)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float2 pi, po;
  
  pi.x = roi_out.x + scale_out * ci.x + x + 0.5f;
  pi.y = roi_out.y + scale_out * ci.y + y + 0.5f;

  pi.x -= flip ? t.y * scale_out : t.x * scale_out;
  pi.y -= flip ? t.x * scale_out : t.y * scale_out;

  pi /= scale_out;
  backtransform(&pi, &po, mat, k);
  po *= scale_in;

  po.x += t.x * scale_in;
  po.y += t.y * scale_in;

  po.x -= roi_in.x;
  po.y -= roi_in.y;

  const int ii = (int)po.x;
  const int jj = (int)po.y;

  float4 o;

  if (ii >=0 && jj >= 0 && ii <= in_width-2 && jj <= in_height-2)
    o = read_imagef(in, samplerf, po);
  else
    o = (float4)0.0f;

  write_imagef (out, (int2)(x, y), o);
}


/* we use this exp approximation to maintain full identity with cpu path */
float 
fast_expf(const float x)
{
  // meant for the range [-100.0f, 0.0f]. largest error ~ -0.06 at 0.0f.
  // will get _a_lot_ worse for x > 0.0f (9000 at 10.0f)..
  const int i1 = 0x3f800000u;
  // e^x, the comment would be 2^x
  const int i2 = 0x402DF854u;//0x40000000u;
  // const int k = CLAMPS(i1 + x * (i2 - i1), 0x0u, 0x7fffffffu);
  // without max clamping (doesn't work for large x, but is faster):
  const int k0 = i1 + x * (i2 - i1);
  const int k = k0 > 0 ? k0 : 0;
  const float f = *(const float *)&k;
  return f;
}


/* kernel for monochrome */
__kernel void
monochrome(read_only image2d_t in, write_only image2d_t out, const int width, const int height, 
           const float a, const float b, const float size)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  pixel.x *= fast_expf(-clamp(((pixel.y - a)*(pixel.y - a) + (pixel.z - b)*(pixel.z - b))/(2.0f * size), 0.0f, 1.0f));
  pixel.y = pixel.z = 0.0f;
  write_imagef (out, (int2)(x, y), pixel);
}


float
lab_f_inv(float x)
{
  const float epsilon = 0.206896551f; 
  const float kappa   = 24389.0f/27.0f;
  if(x > epsilon) return x*x*x;
  else return (116.0f*x - 16.0f)/kappa;
}

void
Lab_to_XYZ(float *Lab, float *XYZ)
{
  const float d50[3] = { 0.9642f, 1.0f, 0.8249f };
  const float fy = (Lab[0] + 16.0f)/116.0f;
  const float fx = Lab[1]/500.0f + fy;
  const float fz = fy - Lab[2]/200.0f;
  XYZ[0] = d50[0]*lab_f_inv(fx);
  XYZ[1] = d50[1]*lab_f_inv(fy);
  XYZ[2] = d50[2]*lab_f_inv(fz);
}

/* kernel for the plugin colorout, fast matrix + shaper path only */
kernel void
colorout (read_only image2d_t in, write_only image2d_t out, const int width, const int height,
          global float *mat, read_only image2d_t lutr, read_only image2d_t lutg, read_only image2d_t lutb,
          global float *a)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  float XYZ[3], rgb[3];
  Lab_to_XYZ((float *)&pixel, XYZ);
  for(int i=0;i<3;i++)
  {
    rgb[i] = 0.0f;
    for(int j=0;j<3;j++) rgb[i] += mat[3*i+j]*XYZ[j];
  }
  pixel.x = lookup_unbounded(lutr, rgb[0], a);
  pixel.y = lookup_unbounded(lutg, rgb[1], a+2);
  pixel.z = lookup_unbounded(lutb, rgb[2], a+4);
  write_imagef (out, (int2)(x, y), pixel);
}

