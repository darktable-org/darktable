/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.

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

/* kernel for the sharpen plugin */
kernel void
sharpen (read_only image2d_t in, write_only image2d_t out, constant float *m, const int rad,
    const float sharpen, const float thrs)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int wd = 2*rad+1;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  float sum = 0.0f;
  for(int j=0;j<wd;j++) for(int i=0;i<wd;i++)
  {
    float px = read_imagef(in, sampleri, (float2)(x+(i - rad), y+(j - rad))).x;
    const float w = m[j]*m[i];
    sum += w*px;
  }
  float d = pixel.x - sum;
  float amount = sharpen * copysign(max(0.0f, fabs(d) - thrs), d);
  write_imagef (out, (int2)(x, y), (float4)(max(0.0f, pixel.x + amount), pixel.y, pixel.z, 1.0f));
}

kernel void
whitebalance_1ui(read_only image2d_t in, write_only image2d_t out, constant float *coeffs,
    const unsigned int filters, const int rx, const int ry)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const uint4 pixel = read_imageui(in, sampleri, (int2)(x, y));
  write_imagef (out, (int2)(x, y), (float4)(pixel.x * coeffs[FC(ry+y, rx+x, filters)], 0.0f, 0.0f, 0.0f));
}

kernel void
whitebalance_4f(read_only image2d_t in, write_only image2d_t out, constant float *coeffs)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  write_imagef (out, (int2)(x, y), (float4)(pixel.x * coeffs[0], pixel.y * coeffs[1], pixel.z * coeffs[2], pixel.w));
}

/* kernel for the exposure plugin. should work transparently with float4 and float image2d. */
kernel void
exposure (read_only image2d_t in, write_only image2d_t out, const float black, const float scale)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

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
highlights (read_only image2d_t in, write_only image2d_t out, const int mode, const float clip,
            const float blendL, const float blendC, const float blendh)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

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
lookup(read_only image2d_t lut, const float x)
{
  // in case the tone curve is marked as linear, return the fast
  // path to linear unbounded (does not clip x at 1)
  const float f = read_imagef(lut, sampleri, 0).x;
  if(f >= 0.0f)
  {
    const int xi = clamp(x*65535.0f, 0.0f, 65535.0f);
    const int2 p = (int2)((xi & 0xff), (xi >> 8));
    return read_imagef(lut, sampleri, p).x;
  }
  else return x;
}

/* kernel for the basecurve plugin. */
kernel void
basecurve (read_only image2d_t in, write_only image2d_t out, read_only image2d_t table, constant float *a)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  // use lut or extrapolation:
  if(pixel.x < 1.0f) pixel.x = lookup(table, pixel.x);
  else               pixel.x = a[0] * pow(pixel.x, a[1]);
  if(pixel.y < 1.0f) pixel.y = lookup(table, pixel.y);
  else               pixel.y = a[0] * pow(pixel.y, a[1]);
  if(pixel.x < 1.0f) pixel.z = lookup(table, pixel.z);
  else               pixel.z = a[0] * pow(pixel.z, a[1]);
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
colorin (read_only image2d_t in, write_only image2d_t out, constant float *mat,
         read_only image2d_t lutr, read_only image2d_t lutg, read_only image2d_t lutb, const int map_blues)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));

  float cam[3], XYZ[3], Lab[3];
  cam[0] = lookup(lutr, pixel.x);
  cam[1] = lookup(lutg, pixel.y);
  cam[2] = lookup(lutb, pixel.z);

  if(map_blues)
  {
    // manual gamut mapping. these values cause trouble when converting back from Lab to sRGB:
    const float YY = cam[0]+cam[1]+cam[2];
    const float zz = cam[2]/YY;
    // lower amount and higher bound_z make the effect smaller.
    // the effect is weakened the darker input values are, saturating at bound_Y
    const float bound_z = 0.5f, bound_Y = 0.5f;
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

/* kernel for the tonecurve plugin. */
kernel void
tonecurve (read_only image2d_t in, write_only image2d_t out, read_only image2d_t table, constant float *a)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  const float L_in = pixel.x/100.0f;
  // use lut or extrapolation:
  const float L = (L_in < 1.0f) ? lookup(table, L_in) : (a[0] * pow(L_in, a[1]));
  if(L_in > 0.01f)
  {
    pixel.y *= L/L_in;
    pixel.z *= L/L_in;
  }
  else
  {
    pixel.y *= L/0.01f;
    pixel.z *= L/0.01f;
  }
  pixel.x = L;
  write_imagef (out, (int2)(x, y), pixel);
}

#if 0
/* kernel for the colorcorrection plugin. */
__kernel void
colorcorrection (read_only image2d_t in, write_only image2d_t out, float saturation, float a_scale, float a_base, float b_scale, float b_base)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  pixel.y = saturation*(pixel.y + pixel.x * a_scale + a_base);
  pixel.z = saturation*(pixel.z + pixel.x * b_scale + b_base);
  write_imagef (out, (int2)(x, y), pixel);
}

// TODO: 2 crop and rotate
__kernel void
clipping (read_only image2d_t in, write_only image2d_t out)
{
// only crop, no rot fast and sharp path:
  if(!d->flags && d->angle == 0.0 && d->keystone > 1 && roi_in->width == roi_out->width && roi_in->height == roi_out->height)
  {
    float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
    write_imagef (out, (int2)(x, y), pixel);
  }
  else
  {
    out = ((float *)o)+ch*roi_out->width*j + ch*i;
    float pi[2], po[2];

    pi[0] = roi_out->x + roi_out->scale*d->cix + i + .5;
    pi[1] = roi_out->y + roi_out->scale*d->ciy + j + .5;
    // transform this point using matrix m
    if(d->flip) {pi[1] -= d->tx*roi_out->scale; pi[0] -= d->ty*roi_out->scale;}
    else        {pi[0] -= d->tx*roi_out->scale; pi[1] -= d->ty*roi_out->scale;}
    pi[0] /= roi_out->scale; pi[1] /= roi_out->scale;
    backtransform(pi, po, d->m, d->k, d->keystone);
    po[0] *= roi_in->scale; po[1] *= roi_in->scale;
    po[0] += d->tx*roi_in->scale;  po[1] += d->ty*roi_in->scale;
    // transform this point to roi_in
    po[0] -= roi_in->x; po[1] -= roi_in->y;

    const int ii = (int)po[0], jj = (int)po[1];
    float4 pixel = read_imagef(in, samplerf, (int2)(ii, jj));
    write_imagef (out, (int2)(x, y), pixel);
  }
}
#endif

#if 0
void
Lab_to_XYZ(float *lab, float *xyz)
{
	const float epsilon = 0.008856f, kappa = 903.3f;
	xyz[1] = (lab[0]<=kappa*epsilon) ?
		(lab[0]/kappa) : (native_powr((lab[0]+16.0f)/116.0f, 3.0f));
	const float fy = (xyz[1]<=epsilon) ? ((kappa*xyz[1]+16.0f)/116.0f) : ((lab[0]+16.0f)/116.0f);
	const float fz = fy - lab[2]/200.0f;
	const float fx = lab[1]/500.0f + fy;
	xyz[2] = (native_powr(fz, 3.0f)<=epsilon) ? ((116.0f*fz-16.0f)/kappa) : (native_powr(fz, 3.0f));
	xyz[0] = (native_powr(fx, 3.0f)<=epsilon) ? ((116.0f*fx-16.0f)/kappa) : (native_powr(fx, 3.0f));
  xyz[0] *= 0.9642;
  xyz[2] *= 0.8249;
}
#else
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
#endif

/* kernel for the plugin colorout, fast matrix + shaper path only */
kernel void
colorout (read_only image2d_t in, write_only image2d_t out, constant float *mat, 
          read_only image2d_t lutr, read_only image2d_t lutg, read_only image2d_t lutb)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  float XYZ[3], rgb[3];
  Lab_to_XYZ((float *)&pixel, XYZ);
  for(int i=0;i<3;i++)
  {
    rgb[i] = 0.0f;
    for(int j=0;j<3;j++) rgb[i] += mat[3*i+j]*XYZ[j];
  }
  pixel.x = lookup(lutr, rgb[0]);
  pixel.y = lookup(lutg, rgb[1]);
  pixel.z = lookup(lutb, rgb[2]);
  write_imagef (out, (int2)(x, y), pixel);
}

