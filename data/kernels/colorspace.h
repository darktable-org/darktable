/*
    This file is part of darktable,
    copyright (c) 2009--2012 johannes hanika.
    copyright (c) 2011--2013 Ulrich Pegelow

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

#pragma once

#include "common.h"

static inline float4 matrix_dot(const float4 vector, const float4 matrix[3])
{
  float4 output;
  const float4 vector_copy = { vector.x, vector.y, vector.z, 0.f };
  output.x = dot(vector_copy, matrix[0]);
  output.y = dot(vector_copy, matrix[1]);
  output.z = dot(vector_copy, matrix[2]);
  output.w = vector.w;
  return output;
}


static inline float4 matrix_product(const float4 xyz, constant const float *const matrix)
{
  const float R = matrix[0] * xyz.x + matrix[1] * xyz.y + matrix[2] * xyz.z;
  const float G = matrix[3] * xyz.x + matrix[4] * xyz.y + matrix[5] * xyz.z;
  const float B = matrix[6] * xyz.x + matrix[7] * xyz.y + matrix[8] * xyz.z;
  const float a = xyz.w;
  return (float4)(R, G, B, a);
}

// same as above but with 4×float padded matrix
static inline float4 matrix_product_float4(const float4 xyz, constant const float *const matrix)
{
  const float R = matrix[0] * xyz.x + matrix[1] * xyz.y + matrix[2]  * xyz.z;
  const float G = matrix[4] * xyz.x + matrix[5] * xyz.y + matrix[6]  * xyz.z;
  const float B = matrix[8] * xyz.x + matrix[9] * xyz.y + matrix[10] * xyz.z;
  const float a = xyz.w;
  return (float4)(R, G, B, a);
}

static inline float4 Lab_2_LCH(float4 Lab)
{
  float H = atan2(Lab.z, Lab.y);

  H = (H > 0.0f) ? H / (2.0f*M_PI_F) : 1.0f - fabs(H) / (2.0f*M_PI_F);

  const float L = Lab.x;
  const float C = sqrt(Lab.y*Lab.y + Lab.z*Lab.z);

  return (float4)(L, C, H, Lab.w);
}


static inline float4 LCH_2_Lab(float4 LCH)
{
  const float L = LCH.x;
  const float a = cos(2.0f*M_PI_F*LCH.z) * LCH.y;
  const float b = sin(2.0f*M_PI_F*LCH.z) * LCH.y;

  return (float4)(L, a, b, LCH.w);
}


static inline float4 lab_f(float4 x)
{
  const float4 epsilon = 216.0f / 24389.0f;
  const float4 kappa = 24389.0f / 27.0f;
  return (x > epsilon) ? native_powr(x, (float4)(1.0f/3.0f)) : (kappa * x + (float4)16.0f) / ((float4)116.0f);
}


static inline float4 XYZ_to_Lab(float4 xyz)
{
  float4 lab;
  const float4 d50 = (float4)(0.9642f, 1.0f, 0.8249f, 1.0f);
  xyz = lab_f(xyz / d50);
  lab.x = 116.0f * xyz.y - 16.0f;
  lab.y = 500.0f * (xyz.x - xyz.y);
  lab.z = 200.0f * (xyz.y - xyz.z);

  return lab;
}


static inline float4 lab_f_inv(float4 x)
{
  const float4 epsilon = 0.206896551f;
  const float4 kappa   = 24389.0f / 27.0f;
  return (x > epsilon) ? x*x*x : ((float4)116.0f * x - (float4)16.0f)/kappa;
}


static inline float4 Lab_to_XYZ(float4 Lab)
{
  const float4 d50 = (float4)(0.9642f, 1.0f, 0.8249f, 0.0f);
  float4 f;
  f.y = (Lab.x + 16.0f)/116.0f;
  f.x = Lab.y/500.0f + f.y;
  f.z = f.y - Lab.z/200.0f;
  return d50 * lab_f_inv(f);
}

static inline float4 prophotorgb_to_XYZ(float4 rgb)
{
  const float rgb_to_xyz[3][3] = { // prophoto rgb
    {0.7976749f, 0.1351917f, 0.0313534f},
    {0.2880402f, 0.7118741f, 0.0000857f},
    {0.0000000f, 0.0000000f, 0.8252100f},
  };
  float4 XYZ = (float4)(0.0f, 0.0f, 0.0f, 0.0f);
  XYZ.x += rgb_to_xyz[0][0] * rgb.x;
  XYZ.x += rgb_to_xyz[0][1] * rgb.y;
  XYZ.x += rgb_to_xyz[0][2] * rgb.z;
  XYZ.y += rgb_to_xyz[1][0] * rgb.x;
  XYZ.y += rgb_to_xyz[1][1] * rgb.y;
  XYZ.y += rgb_to_xyz[1][2] * rgb.z;
  XYZ.z += rgb_to_xyz[2][0] * rgb.x;
  XYZ.z += rgb_to_xyz[2][1] * rgb.y;
  XYZ.z += rgb_to_xyz[2][2] * rgb.z;
  return XYZ;
}

static inline float4 XYZ_to_prophotorgb(float4 XYZ)
{
  const float xyz_to_rgb[3][3] = { // prophoto rgb d50
    { 1.3459433f, -0.2556075f, -0.0511118f},
    {-0.5445989f,  1.5081673f,  0.0205351f},
    { 0.0000000f,  0.0000000f,  1.2118128f},
  };
  float4 rgb = (float4)(0.0f, 0.0f, 0.0f, 0.0f);
  rgb.x += xyz_to_rgb[0][0] * XYZ.x;
  rgb.x += xyz_to_rgb[0][1] * XYZ.y;
  rgb.x += xyz_to_rgb[0][2] * XYZ.z;
  rgb.y += xyz_to_rgb[1][0] * XYZ.x;
  rgb.y += xyz_to_rgb[1][1] * XYZ.y;
  rgb.y += xyz_to_rgb[1][2] * XYZ.z;
  rgb.z += xyz_to_rgb[2][0] * XYZ.x;
  rgb.z += xyz_to_rgb[2][1] * XYZ.y;
  rgb.z += xyz_to_rgb[2][2] * XYZ.z;
  return rgb;
}

static inline float4 Lab_to_prophotorgb(float4 Lab)
{
  const float4 XYZ = Lab_to_XYZ(Lab);
  return XYZ_to_prophotorgb(XYZ);
}

static inline float4 prophotorgb_to_Lab(float4 rgb)
{
  const float4 XYZ = prophotorgb_to_XYZ(rgb);
  return XYZ_to_Lab(XYZ);
}

static inline float4 RGB_2_HSL(const float4 RGB)
{
  float H, S, L;

  // assumes that each channel is scaled to [0; 1]
  const float R = RGB.x;
  const float G = RGB.y;
  const float B = RGB.z;

  const float var_Min = fmin(R, fmin(G, B));
  const float var_Max = fmax(R, fmax(G, B));
  const float del_Max = var_Max - var_Min;

  L = (var_Max + var_Min) / 2.0f;

  if (del_Max < 1e-6f)
  {
    H = 0.0f;
    S = 0.0f;
  }
  else
  {
    if (L < 0.5f) S = del_Max / (var_Max + var_Min);
    else          S = del_Max / (2.0f - var_Max - var_Min);

    const float del_R = (((var_Max - R) / 6.0f) + (del_Max / 2.0f)) / del_Max;
    const float del_G = (((var_Max - G) / 6.0f) + (del_Max / 2.0f)) / del_Max;
    const float del_B = (((var_Max - B) / 6.0f) + (del_Max / 2.0f)) / del_Max;

    if      (R == var_Max) H = del_B - del_G;
    else if (G == var_Max) H = (1.0f / 3.0f) + del_R - del_B;
    else if (B == var_Max) H = (2.0f / 3.0f) + del_G - del_R;

    if (H < 0.0f) H += 1.0f;
    if (H > 1.0f) H -= 1.0f;
  }

  return (float4)(H, S, L, RGB.w);
}



static inline float Hue_2_RGB(float v1, float v2, float vH)
{
  if (vH < 0.0f) vH += 1.0f;
  if (vH > 1.0f) vH -= 1.0f;
  if ((6.0f * vH) < 1.0f) return (v1 + (v2 - v1) * 6.0f * vH);
  if ((2.0f * vH) < 1.0f) return (v2);
  if ((3.0f * vH) < 2.0f) return (v1 + (v2 - v1) * ((2.0f / 3.0f) - vH) * 6.0f);
  return (v1);
}



static inline float4 HSL_2_RGB(const float4 HSL)
{
  float R, G, B;

  const float H = HSL.x;
  const float S = HSL.y;
  const float L = HSL.z;

  float var_1, var_2;

  if (S < 1e-6f)
  {
    R = B = G = L;
  }
  else
  {
    if (L < 0.5f) var_2 = L * (1.0f + S);
    else          var_2 = (L + S) - (S * L);

    var_1 = 2.0f * L - var_2;

    R = Hue_2_RGB(var_1, var_2, H + (1.0f / 3.0f));
    G = Hue_2_RGB(var_1, var_2, H);
    B = Hue_2_RGB(var_1, var_2, H - (1.0f / 3.0f));
  }

  // returns RGB scaled to [0; 1] for each channel
  return (float4)(R, G, B, HSL.w);
}

static inline float4 RGB_2_HSV(const float4 RGB)
{
  float4 HSV;

  const float minv = fmin(RGB.x, fmin(RGB.y, RGB.z));
  const float maxv = fmax(RGB.x, fmax(RGB.y, RGB.z));
  const float delta = maxv - minv;

  HSV.z = maxv;
  HSV.w = RGB.w;

  if(fabs(maxv) > 1e-6f && fabs(delta) > 1e-6f)
  {
    HSV.y = delta / maxv;
  }
  else
  {
    HSV.x = 0.0f;
    HSV.y = 0.0f;
    return HSV;
  }

  if (RGB.x == maxv)
   HSV.x = (RGB.y - RGB.z) / delta;
  else if (RGB.y == maxv)
   HSV.x = 2.0f + (RGB.z - RGB.x) / delta;
  else
   HSV.x = 4.0f + (RGB.x - RGB.y) / delta;

  HSV.x /= 6.0f;

  if(HSV.x < 0)
    HSV.x += 1.0f;

  return HSV;
}

static inline float4 HSV_2_RGB(const float4 HSV)
{
  float4 RGB;

  if (fabs(HSV.y) < 1e-6f)
  {
    RGB.x = RGB.y = RGB.z = HSV.z;
    RGB.w = HSV.w;
    return RGB;
  }

  const int i = floor(6.0f*HSV.x);
  const float v = HSV.z;
  const float w = HSV.w;
  const float p = v * (1.0f - HSV.y);
  const float q = v * (1.0f - HSV.y * (6.0f*HSV.x - i));
  const float t = v * (1.0f - HSV.y * (1.0f - (6.0f*HSV.x - i)));

  switch (i)
  {
    case 0:
      RGB = (float4)(v, t, p, w);
      break;
    case 1:
      RGB = (float4)(q, v, p, w);
      break;
    case 2:
      RGB = (float4)(p, v, t, w);
      break;
    case 3:
      RGB = (float4)(p, q, v, w);
      break;
    case 4:
      RGB = (float4)(t, p, v, w);
      break;
    case 5:
    default:
      RGB = (float4)(v, p, q, w);
      break;
  }
  return RGB;
}


// XYZ -> sRGB matrix, D65
static inline float4 XYZ_to_sRGB(float4 XYZ)
{
  float4 sRGB;

  sRGB.x =  3.1338561f * XYZ.x - 1.6168667f * XYZ.y - 0.4906146f * XYZ.z;
  sRGB.y = -0.9787684f * XYZ.x + 1.9161415f * XYZ.y + 0.0334540f * XYZ.z;
  sRGB.z =  0.0719453f * XYZ.x - 0.2289914f * XYZ.y + 1.4052427f * XYZ.z;
  sRGB.w = XYZ.w;

  return sRGB;
}


// sRGB -> XYZ matrix, D65
static inline float4 sRGB_to_XYZ(float4 sRGB)
{
  float4 XYZ;

  XYZ.x = 0.4360747f * sRGB.x + 0.3850649f * sRGB.y + 0.1430804f * sRGB.z;
  XYZ.y = 0.2225045f * sRGB.x + 0.7168786f * sRGB.y + 0.0606169f * sRGB.z;
  XYZ.z = 0.0139322f * sRGB.x + 0.0971045f * sRGB.y + 0.7141733f * sRGB.z;
  XYZ.w = sRGB.w;

  return XYZ;
}


static inline float4 XYZ_to_JzAzBz(float4 XYZ_D65)
{
  const float4 M[3] = { { 0.41478972f, 0.579999f, 0.0146480f, 0.0f },
                        { -0.2015100f, 1.120649f, 0.0531008f, 0.0f },
                        { -0.0166008f, 0.264800f, 0.6684799f, 0.0f } };

  const float4 A[3] = { { 0.5f, 0.5f, 0.0f, 0.0f },
                        { 3.524000f, -4.066708f, 0.542708f, 0.0f },
                        { 0.199076f, 1.096799f, -1.295875f, 0.0f } };

  float4 temp1, temp2;
  // XYZ -> X'Y'Z
  temp1.x = 1.15f * XYZ_D65.x - 0.15f * XYZ_D65.z;
  temp1.y = 0.66f * XYZ_D65.y + 0.34f * XYZ_D65.x;
  temp1.z = XYZ_D65.z;
  temp1.w = 0.f;
  // X'Y'Z -> LMS
  temp2.x = dot(M[0], temp1);
  temp2.y = dot(M[1], temp1);
  temp2.z = dot(M[2], temp1);
  temp2.w = 0.f;
  // LMS -> L'M'S'
  temp2 = native_powr(fmax(temp2 / 10000.f, 0.0f), 0.159301758f);
  temp2 = native_powr((0.8359375f + 18.8515625f * temp2) / (1.0f + 18.6875f * temp2), 134.034375f);
  // L'M'S' -> Izazbz
  temp1.x = dot(A[0], temp2);
  temp1.y = dot(A[1], temp2);
  temp1.z = dot(A[2], temp2);
  // Iz -> Jz
  temp1.x = fmax(0.44f * temp1.x / (1.0f - 0.56f * temp1.x) - 1.6295499532821566e-11f, 0.f);
  return temp1;
}


static inline float4 JzAzBz_2_XYZ(const float4 JzAzBz)
{
  const float b = 1.15f;
  const float g = 0.66f;
  const float c1 = 0.8359375f; // 3424 / 2^12
  const float c2 = 18.8515625f; // 2413 / 2^7
  const float c3 = 18.6875f; // 2392 / 2^7
  const float n_inv = 1.0f / 0.159301758f; // 2610 / 2^14
  const float p_inv = 1.0f / 134.034375f; // 1.7 x 2523 / 2^5
  const float d = -0.56f;
  const float d0 = 1.6295499532821566e-11f;
  const float4 MI[3] = { {  1.9242264357876067f, -1.0047923125953657f,  0.0376514040306180f, 0.0f },
                         {  0.3503167620949991f,  0.7264811939316552f, -0.0653844229480850f, 0.0f },
                         { -0.0909828109828475f, -0.3127282905230739f,  1.5227665613052603f, 0.0f } };
  const float4 AI[3] = { {  1.0f,  0.1386050432715393f,  0.0580473161561189f, 0.0f },
                         {  1.0f, -0.1386050432715393f, -0.0580473161561189f, 0.0f },
                         {  1.0f, -0.0960192420263190f, -0.8118918960560390f, 0.0f } };

  float4 XYZ, LMS, IzAzBz;
  // Jz -> Iz
  IzAzBz = JzAzBz;
  IzAzBz.x += d0;
  IzAzBz.x = fmax(IzAzBz.x / (1.0f + d - d * IzAzBz.x), 0.f);
  // IzAzBz -> L'M'S'
  LMS.x = dot(AI[0], IzAzBz);
  LMS.y = dot(AI[1], IzAzBz);
  LMS.z = dot(AI[2], IzAzBz);
  LMS.w = 0.f;
  // L'M'S' -> LMS
  LMS = native_powr(fmax(LMS, 0.0f), p_inv);
  LMS = 10000.f * native_powr(fmax((c1 - LMS) / (c3 * LMS - c2), 0.0f), n_inv);
  // LMS -> X'Y'Z
  XYZ.x = dot(MI[0], LMS);
  XYZ.y = dot(MI[1], LMS);
  XYZ.z = dot(MI[2], LMS);
  XYZ.w = 0.f;
  // X'Y'Z -> XYZ_D65
  float4 XYZ_D65;
  XYZ_D65.x = (XYZ.x + (b - 1.0f) * XYZ.z) / b;
  XYZ_D65.y = (XYZ.y + (g - 1.0f) * XYZ_D65.x) / g;
  XYZ_D65.z = XYZ.z;
  XYZ_D65.w = JzAzBz.w;
  return XYZ_D65;
}


static inline float4 JzAzBz_to_JzCzhz(float4 JzAzBz)
{
  const float h = atan2(JzAzBz.z, JzAzBz.y) / (2.0f * M_PI_F);
  float4 JzCzhz;
  JzCzhz.x = JzAzBz.x;
  JzCzhz.y = native_sqrt(JzAzBz.y * JzAzBz.y + JzAzBz.z * JzAzBz.z);
  JzCzhz.z = (h >= 0.0f) ? h : 1.0f + h;
  JzCzhz.w = JzAzBz.w;
  return JzCzhz;
}


// Convert CIE 1931 2° XYZ D65 to CIE 2006 LMS D65 (cone space)
/*
* The CIE 1931 XYZ 2° observer D65 is converted to CIE 2006 LMS D65 using the approximation by
* Richard A. Kirk, Chromaticity coordinates for graphic arts based on CIE 2006 LMS
* with even spacing of Munsell colours
* https://doi.org/10.2352/issn.2169-2629.2019.27.38
*/

static inline float4 XYZ_to_LMS(const float4 XYZ)
{
  const float4 XYZ_D65_to_LMS_2006_D65[3]
    = { { 0.257085f, 0.859943f, -0.031061f, 0.f },
        { -0.394427f, 1.175800f, 0.106423f, 0.f },
        { 0.064856f, -0.076250f, 0.559067f, 0.f } };

  return matrix_dot(XYZ, XYZ_D65_to_LMS_2006_D65);
}


static inline float4 LMS_to_XYZ(const float4 LMS)
{
  const float4 LMS_2006_D65_to_XYZ_D65[3]
    = { { 1.80794659f, -1.29971660f, 0.34785879f, 0.f },
        { 0.61783960f, 0.39595453f, -0.04104687f, 0.f },
        { -0.12546960f, 0.20478038f, 1.74274183f, 0.f } };

  return matrix_dot(LMS, LMS_2006_D65_to_XYZ_D65);
}


/*
* Convert from CIE 2006 LMS D65 to Filmlight RGB defined in
* Richard A. Kirk, Chromaticity coordinates for graphic arts based on CIE 2006 LMS
* with even spacing of Munsell colours
* https://doi.org/10.2352/issn.2169-2629.2019.27.38
*/

static inline float4 gradingRGB_to_LMS(const float4 RGB)
{
  const float4 filmlightRGB_D65_to_LMS_D65[3]
    = { { 0.95f, 0.38f, 0.00f, 0.f },
        { 0.05f, 0.62f, 0.03f, 0.f },
        { 0.00f, 0.00f, 0.97f, 0.f } };

  return matrix_dot(RGB, filmlightRGB_D65_to_LMS_D65);
}

static inline float4 LMS_to_gradingRGB(const float4 LMS)
{
  const float4 LMS_D65_to_filmlightRGB_D65[3]
    = { {  1.0877193f, -0.66666667f,  0.02061856f, 0.f },
        { -0.0877193f,  1.66666667f, -0.05154639f, 0.f },
        {         0.f,          0.f,  1.03092784f, 0.f } };

  return matrix_dot(LMS, LMS_D65_to_filmlightRGB_D65);
}


/*
* Re-express the Filmlight RGB triplet as Yrg luminance/chromacity coordinates
*/

static inline float4 LMS_to_Yrg(const float4 LMS)
{
  // compute luminance
  const float Y = 0.68990272f * LMS.x + 0.34832189f * LMS.y;

  // normalize LMS
  const float a = LMS.x + LMS.y + LMS.z;
  const float4 lms = (a == 0.f) ? 0.f : LMS / a;

  // convert to Filmlight rgb (normalized)
  const float4 rgb = LMS_to_gradingRGB(lms);

  return (float4)(Y, rgb.x, rgb.y, LMS.w);
}


static inline float4 Yrg_to_LMS(const float4 Yrg)
{
  const float Y = Yrg.x;

  // reform rgb (normalized) from chroma
  const float r = Yrg.y;
  const float g = Yrg.z;
  const float b = 1.f - r - g;
  const float4 rgb = { r, g, b, 0.f };

  // convert to lms (normalized)
  const float4 lms = gradingRGB_to_LMS(rgb);

  // denormalize to LMS
  const float denom = (0.68990272f * lms.x + 0.34832189f * lms.y);
  const float a = (denom == 0.f) ? 0.f : Y / denom;
  return lms * a;
}


/*
 * Re-express Filmlight Yrg in polar coordinates Ych
 *
 * Note that we don't explicitly store the hue angle
 * but rather just the cosine and sine of the angle.
 * This is because we don't need the hue angle anywhere
 * and this way we can avoid calculating expensive
 * trigonometric functions.
 */

static inline float4 Yrg_to_Ych(const float4 Yrg)
{
  const float Y = Yrg.x;
  // Subtract white point. These are the r, g coordinates of
  // sRGB (D50 adapted) (1, 1, 1) taken through
  // XYZ D50 -> CAT16 D50->D65 adaptation -> LMS 2006
  // -> grading RGB conversion.
  const float r = Yrg.y - 0.21902143f;
  const float g = Yrg.z - 0.54371398f;
  const float c = dt_fast_hypot(g, r);
  const float cos_h = c != 0.f ? r / c : 1.f;
  const float sin_h = c != 0.f ? g / c : 0.f;
  return (float4)(Y, c, cos_h, sin_h);
}


static inline float4 Ych_to_Yrg(const float4 Ych)
{
  const float Y = Ych.x;
  const float c = Ych.y;
  const float cos_h = Ych.z;
  const float sin_h = Ych.w;
  const float r = c * cos_h + 0.21902143f;
  const float g = c * sin_h + 0.54371398f;
  return (float4)(Y, r, g, 0.f);
}


static inline float4 dt_xyY_to_uvY(const float4 xyY)
{
  // This is the linear part of the chromaticity transform from CIE L*u*v* e.g. u'v'.
  // See https://en.wikipedia.org/wiki/CIELUV
  // It rescales the chromaticity diagram xyY in a more perceptual way,
  // but it is still not hue-linear and not perfectly perceptual.
  // As such, it is the only radiometricly-accurate representation of hue non-linearity in human vision system.
  // Use it for "hue preserving" (as much as possible) gamut mapping in scene-referred space
  const float denominator = -2.f * xyY.x + 12.f * xyY.y + 3.f;
  float4 uvY;
  uvY.x = 4.f * xyY.x / denominator; // u'
  uvY.y = 9.f * xyY.y / denominator; // v'
  uvY.z = xyY.z;                     // Y
  uvY.w = xyY.w;
  return uvY;
}


static inline float4 dt_uvY_to_xyY(const float4 uvY)
{
  // This is the linear part of chromaticity transform from CIE L*u*v* e.g. u'v'.
  // See https://en.wikipedia.org/wiki/CIELUV
  // It rescales the chromaticity diagram xyY in a more perceptual way,
  // but it is still not hue-linear and not perfectly perceptual.
  // As such, it is the only radiometricly-accurate representation of hue non-linearity in human vision system.
  // Use it for "hue preserving" (as much as possible) gamut mapping in scene-referred space
  const float denominator = 6.0f * uvY.x - 16.f * uvY.y + 12.0f;
  float4 xyY;
  xyY.x = 9.f * uvY.x / denominator; // x
  xyY.y = 4.f * uvY.y / denominator; // y
  xyY.z = uvY.z;                     // Y
  xyY.w = uvY.w;
  return xyY;
}

static inline float4 dt_XYZ_to_xyY(const float4 XYZ)
{
  float4 xyY;
  const float sum = XYZ.x + XYZ.y + XYZ.z;
  xyY.xy = XYZ.xy / sum;
  xyY.z = XYZ.y;
  xyY.w = XYZ.w;
  return xyY;
}

static inline float4 dt_xyY_to_XYZ(const float4 xyY)
{
  float4 XYZ;
  XYZ.x = xyY.z * xyY.x / xyY.y;
  XYZ.y = xyY.z;
  XYZ.z = xyY.z * (1.f - xyY.x - xyY.y) / xyY.y;
  XYZ.w = xyY.w;
  return XYZ;
}

// port src/common/chromatic_adaptation.h

static inline float4 convert_XYZ_to_bradford_LMS(const float4 XYZ)
{
  // Warning : needs XYZ normalized with Y - you need to downscale before
  const float4 XYZ_to_Bradford_LMS[3] = { {  0.8951f,  0.2664f, -0.1614f, 0.f },
                                          { -0.7502f,  1.7135f,  0.0367f, 0.f },
                                          {  0.0389f, -0.0685f,  1.0296f, 0.f } };

  return matrix_dot(XYZ, XYZ_to_Bradford_LMS);
}

static inline float4 convert_bradford_LMS_to_XYZ(const float4 LMS)
{
  // Warning : output XYZ normalized with Y - you need to upscale later
  const float4 Bradford_LMS_to_XYZ[3] = { {  0.9870f, -0.1471f,  0.1600f, 0.f },
                                          {  0.4323f,  0.5184f,  0.0493f, 0.f },
                                          { -0.0085f,  0.0400f,  0.9685f, 0.f } };

  return matrix_dot(LMS, Bradford_LMS_to_XYZ);
}

static inline float4 convert_XYZ_to_CAT16_LMS(const float4 XYZ)
{
  // Warning : needs XYZ normalized with Y - you need to downscale before
  const float4 XYZ_to_CAT16_LMS[3] = { {  0.401288f, 0.650173f, -0.051461f, 0.f },
                                       { -0.250268f, 1.204414f,  0.045854f, 0.f },
                                       { -0.002079f, 0.048952f,  0.953127f, 0.f } };

  return matrix_dot(XYZ, XYZ_to_CAT16_LMS);
}

static inline float4 convert_CAT16_LMS_to_XYZ(const float4 LMS)
{
  // Warning : output XYZ normalized with Y - you need to upscale later
  const float4 CAT16_LMS_to_XYZ[3] = { {  1.862068f, -1.011255f,  0.149187f, 0.f },
                                       {  0.38752f ,  0.621447f, -0.008974f, 0.f },
                                       { -0.015841f, -0.034123f,  1.049964f, 0.f } };

  return matrix_dot(LMS, CAT16_LMS_to_XYZ);
}

static inline void bradford_adapt_D50(float4 *lms_in,
                                      const float4 origin_illuminant,
                                      const float p, const int full)
{
  // Bradford chromatic adaptation from origin to target D50 illuminant in LMS space
  // p = powf(origin_illuminant[2] / D50[2], 0.0834f) needs to be precomputed for performance,
  // since it is independent from current pixel values
  // origin illuminant need also to be precomputed to LMS

  // Precomputed D50 primaries in Bradford LMS for ICC transforms
  const float4 D50 = { 0.996078f, 1.020646f, 0.818155f, 0.f };

  if(full)
  {
    float4 temp = *lms_in / origin_illuminant;

    // use linear Bradford if B is negative
    temp.z = (temp.z > 0.f) ? native_powr(temp.z, p) : temp.z;

    *lms_in = D50 * temp;
  }
  else
    *lms_in *= D50 / origin_illuminant;
}

static inline void CAT16_adapt_D50(float4 *lms_in,
                                   const float4 origin_illuminant,
                                   const float D, const int full)
{
  // CAT16 chromatic adaptation from origin to target D50 illuminant in LMS space
  // D is the coefficient of adaptation, depending of the surround lighting
  // origin illuminant need also to be precomputed to LMS

  // Precomputed D50 primaries in CAT16 LMS for ICC transforms
  const float4 D50 = { 0.994535f, 1.000997f, 0.833036f, 0.f };

  if(full) *lms_in *= D50 / origin_illuminant;
  else *lms_in *= (D * D50 / origin_illuminant + 1.f - D);
}

static inline void XYZ_adapt_D50(float4 *lms_in,
                                 const float4 origin_illuminant)
{
  // XYZ chromatic adaptation from origin to target D65 illuminant in XYZ space
  // origin illuminant need also to be precomputed to XYZ

  // Precomputed D50 primaries in XYZ for camera WB adjustment
  const float4 D50 = { 0.9642119944211994f, 1.0f, 0.8251882845188288f, 0.f };
  *lms_in *= D50 / origin_illuminant;
}

static inline float4 gamut_check_Yrg(float4 Ych)
{
  // Do a test conversion to Yrg
  float4 Yrg = Ych_to_Yrg(Ych);

  // Gamut-clip in Yrg at constant hue and luminance
  // e.g. find the max chroma value that fits in gamut at the current hue
  const float D65_r = 0.21902143f;
  const float D65_g = 0.54371398f;
  float max_c = Ych.y;
  const float cos_h = Ych.z;
  const float sin_h = Ych.w;

  if(Yrg.y < 0.f)
  {
    max_c = fmin(-D65_r / cos_h, max_c);
  }
  if(Yrg.z < 0.f)
  {
    max_c = fmin(-D65_g / sin_h, max_c);
  }
  if(Yrg.y + Yrg.z > 1.f)
  {
    max_c = fmin((1.f - D65_r - D65_g) / (cos_h + sin_h), max_c);
  }

  // Overwrite chroma with the sanitized value and
  Ych.y = max_c;

  return Ych;
}


/** The following is darktable Uniform Color Space 2022
 * © Aurélien Pierre
 * https://eng.aurelienpierre.com/2022/02/color-saturation-control-for-the-21th-century/
 *
 * Use this space for color-grading in a perceptual framework.
 * The CAM terms have been removed for performance.
 **/

static inline float Y_to_dt_UCS_L_star(const float Y)
{
  // WARNING: L_star needs to be < 2.098883786377, meaning Y needs to be < 3.875766378407574e+19
  const float Y_hat = native_powr(Y, 0.631651345306265f);
  return 2.098883786377f * Y_hat / (Y_hat + 1.12426773749357f);
}

static inline float dt_UCS_L_star_to_Y(const float L_star)
{
  // WARNING: L_star needs to be < 2.098883786377, meaning Y needs to be < 3.875766378407574e+19
  return native_powr((1.12426773749357f * L_star / (2.098883786377f - L_star)), 1.5831518565279648f);
}

static inline void xyY_to_dt_UCS_UV(const float4 xyY, float UV_star_prime[2])
{
  float4 x_factors = { -0.783941002840055f,  0.745273540913283f, 0.318707282433486f, 0.f };
  float4 y_factors = {  0.277512987809202f, -0.205375866083878f, 2.16743692732158f,  0.f };
  float4 offsets   = {  0.153836578598858f, -0.165478376301988f, 0.291320554395942f, 0.f };

  float4 UVD = x_factors * xyY.x + y_factors * xyY.y + offsets;
  UVD.xy /= UVD.z;

  float UV_star[2] = { 0.f };
  const float factors[2]     = { 1.39656225667f, 1.4513954287f };
  const float half_values[2] = { 1.49217352929f, 1.52488637914f };
  for(int c = 0; c < 2; c++)
    UV_star[c] = factors[c] * ((float *)&UVD)[c] / (fabs(((float *)&UVD)[c]) + half_values[c]);

  // The following is equivalent to a 2D matrix product
  UV_star_prime[0] = -1.124983854323892f * UV_star[0] - 0.980483721769325f * UV_star[1];
  UV_star_prime[1] =  1.86323315098672f  * UV_star[0] + 1.971853092390862f * UV_star[1];
}


static inline float4 xyY_to_dt_UCS_JCH(const float4 xyY, const float L_white)
{
  /*
    input :
      * xyY in normalized CIE XYZ for the 2° 1931 observer adapted for D65
      * L_white the lightness of white as dt UCS L* lightness
      * cz = 1 for standard pre-print proofing conditions with average surround and n = 20 %
              (background = middle grey, white = perfect diffuse white)
    range : xy in [0; 1], Y normalized for perfect diffuse white = 1
  */

  float UV_star_prime[2];
  xyY_to_dt_UCS_UV(xyY, UV_star_prime);

  // Y upper limit is calculated from the L star upper limit.
  const float DT_UCS_Y_UPPER_LIMIT = 13237757000.f;
  const float L_star = Y_to_dt_UCS_L_star(clamp(xyY.z, 0.f, DT_UCS_Y_UPPER_LIMIT));
  const float M2 = UV_star_prime[0] * UV_star_prime[0] + UV_star_prime[1] * UV_star_prime[1]; // square of colorfulness M

  // should be JCH[0] = powf(L_star / L_white), cz) but we treat only the case where cz = 1
  float4 JCH;
  JCH.x = L_star / L_white;
  JCH.y = 15.932993652962535f * native_powr(L_star, 0.6523997524738018f) * native_powr(M2, 0.6007557017508491f) / L_white;
  JCH.z = atan2(UV_star_prime[1], UV_star_prime[0]);
  return JCH;

}

static inline float4 dt_UCS_JCH_to_xyY(const float4 JCH, const float L_white)
{
  /*
    input :
      * xyY in normalized CIE XYZ for the 2° 1931 observer adapted for D65
      * L_white the lightness of white as dt UCS L* lightness
      * cz = 1 for standard pre-print proofing conditions with average surround and n = 20 %
              (background = middle grey, white = perfect diffuse white)
    range : xy in [0; 1], Y normalized for perfect diffuse white = 1
  */

  // should be L_star = powf(JCH[0], 1.f / cz) * L_white but we treat only the case where cz = 1
  // L_star upper limit is 2.098883786377 truncated to 32-bit float and last decimal removed.
  // By clipping L_star to this limit, we ensure dt_UCS_L_star_to_Y() doesn't divide by zero.
  const float DT_UCS_L_STAR_UPPER_LIMIT = 2.098883f;
  const float L_star = clamp(JCH.x * L_white, 0.f, DT_UCS_L_STAR_UPPER_LIMIT);
  const float M = L_star != 0.f
    ? native_powr(JCH.y * L_white / (15.932993652962535f * native_powr(L_star, 0.6523997524738018f)), 0.8322850678616855f)
    : 0.f;

  const float U_star_prime = M * native_cos(JCH.z);
  const float V_star_prime = M * native_sin(JCH.z);

  // The following is equivalent to a 2D matrix product
  const float UV_star[2] = { -5.037522385190711f * U_star_prime - 2.504856328185843f * V_star_prime,
                              4.760029407436461f * U_star_prime + 2.874012963239247f * V_star_prime };

  float UV[2] = {0.f};
  const float factors[2]     = { 1.39656225667f, 1.4513954287f };
  const float half_values[2] = { 1.49217352929f,1.52488637914f };
  for(int c = 0; c < 2; c++)
    UV[c] = -half_values[c] * UV_star[c] / (fabs(UV_star[c]) - factors[c]);

  const float4 U_factors = {  0.167171472114775f,   -0.150959086409163f,    0.940254742367256f,  0.f };
  const float4 V_factors = {  0.141299802443708f,   -0.155185060382272f,    1.000000000000000f,  0.f };
  const float4 offsets   = { -0.00801531300850582f, -0.00843312433578007f, -0.0256325967652889f, 0.f };

  float4 xyD = U_factors * UV[0] + V_factors * UV[1] + offsets;

  float4 xyY;
  xyY.x = xyD.x / xyD.z;
  xyY.y = xyD.y / xyD.z;
  xyY.z = dt_UCS_L_star_to_Y(L_star);
  return xyY;
}


static inline float4 dt_UCS_JCH_to_HSB(const float4 JCH)
{
  float4 HSB;
  HSB.z = JCH.x * (native_powr(JCH.y, 1.33654221029386f) + 1.f);
  HSB.y = (HSB.z > 0.f) ? JCH.y / HSB.z : 0.f;
  HSB.x = JCH.z;
  return HSB;
}


static inline float4 dt_UCS_HSB_to_JCH(const float4 HSB)
{
  float4 JCH;
  JCH.z = HSB.x;
  JCH.y = HSB.y * HSB.z;
  JCH.x = HSB.z / (native_powr(JCH.y, 1.33654221029386f) + 1.f);
  return JCH;
}


static inline float4 dt_UCS_JCH_to_HCB(const float4 JCH)
{
  float4 HCB;
  HCB.z = JCH.x * (native_powr(JCH.y, 1.33654221029386f) + 1.f);
  HCB.y = JCH.y;
  HCB.x = JCH.z;
  return HCB;
}


static inline float4 dt_UCS_HCB_to_JCH(const float4 HCB)
{
  float4 JCH;
  JCH.z = HCB.x;
  JCH.y = HCB.y;
  JCH.x = HCB.z / (native_powr(HCB.y, 1.33654221029386f) + 1.f);
  return JCH;
}
