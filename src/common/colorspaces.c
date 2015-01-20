/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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

#include "common/darktable.h"
#include "control/conf.h"
#include "control/control.h"
#include "common/colormatrices.c"
#include "common/colorspaces.h"
#include "common/debug.h"
#include "common/srgb_tone_curve_values.h"
#include "develop/imageop.h"


/** inverts the given 3x3 matrix */
static int mat3inv(float *const dst, const float *const src)
{
#define A(y, x) src[(y - 1) * 3 + (x - 1)]
#define B(y, x) dst[(y - 1) * 3 + (x - 1)]

  const float det = A(1, 1) * (A(3, 3) * A(2, 2) - A(3, 2) * A(2, 3))
                    - A(2, 1) * (A(3, 3) * A(1, 2) - A(3, 2) * A(1, 3))
                    + A(3, 1) * (A(2, 3) * A(1, 2) - A(2, 2) * A(1, 3));

  const float epsilon = 1e-7f;
  if(fabsf(det) < epsilon) return 1;

  const float invDet = 1.f / det;

  B(1, 1) = invDet * (A(3, 3) * A(2, 2) - A(3, 2) * A(2, 3));
  B(1, 2) = -invDet * (A(3, 3) * A(1, 2) - A(3, 2) * A(1, 3));
  B(1, 3) = invDet * (A(2, 3) * A(1, 2) - A(2, 2) * A(1, 3));

  B(2, 1) = -invDet * (A(3, 3) * A(2, 1) - A(3, 1) * A(2, 3));
  B(2, 2) = invDet * (A(3, 3) * A(1, 1) - A(3, 1) * A(1, 3));
  B(2, 3) = -invDet * (A(2, 3) * A(1, 1) - A(2, 1) * A(1, 3));

  B(3, 1) = invDet * (A(3, 2) * A(2, 1) - A(3, 1) * A(2, 2));
  B(3, 2) = -invDet * (A(3, 2) * A(1, 1) - A(3, 1) * A(1, 2));
  B(3, 3) = invDet * (A(2, 2) * A(1, 1) - A(2, 1) * A(1, 2));
#undef A
#undef B
  return 0;
}

static void mat3mulv(float *dst, const float *const mat, const float *const v)
{
  for(int k = 0; k < 3; k++)
  {
    float x = 0.0f;
    for(int i = 0; i < 3; i++) x += mat[3 * k + i] * v[i];
    dst[k] = x;
  }
}

static void mat3mul(float *dst, const float *const m1, const float *const m2)
{
  for(int k = 0; k < 3; k++)
  {
    for(int i = 0; i < 3; i++)
    {
      float x = 0.0f;
      for(int j = 0; j < 3; j++) x += m1[3 * k + j] * m2[3 * j + i];
      dst[3 * k + i] = x;
    }
  }
}

static int dt_colorspaces_get_matrix_from_profile(cmsHPROFILE prof, float *matrix, float *lutr, float *lutg,
                                                  float *lutb, const int lutsize, const int input)
{
  // create an OpenCL processable matrix + tone curves from an cmsHPROFILE:

  // check this first:
  if(!cmsIsMatrixShaper(prof)) return 1;

  cmsToneCurve *red_curve = cmsReadTag(prof, cmsSigRedTRCTag);
  cmsToneCurve *green_curve = cmsReadTag(prof, cmsSigGreenTRCTag);
  cmsToneCurve *blue_curve = cmsReadTag(prof, cmsSigBlueTRCTag);

  cmsCIEXYZ *red_color = cmsReadTag(prof, cmsSigRedColorantTag);
  cmsCIEXYZ *green_color = cmsReadTag(prof, cmsSigGreenColorantTag);
  cmsCIEXYZ *blue_color = cmsReadTag(prof, cmsSigBlueColorantTag);

  if(!red_curve || !green_curve || !blue_curve || !red_color || !green_color || !blue_color) return 2;

  matrix[0] = red_color->X;
  matrix[1] = green_color->X;
  matrix[2] = blue_color->X;
  matrix[3] = red_color->Y;
  matrix[4] = green_color->Y;
  matrix[5] = blue_color->Y;
  matrix[6] = red_color->Z;
  matrix[7] = green_color->Z;
  matrix[8] = blue_color->Z;

  // some camera ICC profiles claim to have color locations for red, green and blue base colors defined,
  // but in fact these are all set to zero. we catch this case here.
  float sum = 0.0f;
  for(int k = 0; k < 9; k++) sum += matrix[k];
  if(sum == 0.0f) return 3;

  if(input)
  {
    // mark as linear, if they are:
    if(cmsIsToneCurveLinear(red_curve))
      lutr[0] = -1.0f;
    else
      for(int k = 0; k < lutsize; k++) lutr[k] = cmsEvalToneCurveFloat(red_curve, k / (lutsize - 1.0f));
    if(cmsIsToneCurveLinear(green_curve))
      lutg[0] = -1.0f;
    else
      for(int k = 0; k < lutsize; k++) lutg[k] = cmsEvalToneCurveFloat(green_curve, k / (lutsize - 1.0f));
    if(cmsIsToneCurveLinear(blue_curve))
      lutb[0] = -1.0f;
    else
      for(int k = 0; k < lutsize; k++) lutb[k] = cmsEvalToneCurveFloat(blue_curve, k / (lutsize - 1.0f));
  }
  else
  {
    // invert profile->XYZ matrix for output profiles
    float tmp[9];
    memcpy(tmp, matrix, sizeof(float) * 9);
    if(mat3inv(matrix, tmp)) return 3;
    // also need to reverse gamma, to apply reverse before matrix multiplication:
    cmsToneCurve *rev_red = cmsReverseToneCurveEx(0x8000, red_curve);
    cmsToneCurve *rev_green = cmsReverseToneCurveEx(0x8000, green_curve);
    cmsToneCurve *rev_blue = cmsReverseToneCurveEx(0x8000, blue_curve);
    if(!rev_red || !rev_green || !rev_blue)
    {
      cmsFreeToneCurve(rev_red);
      cmsFreeToneCurve(rev_green);
      cmsFreeToneCurve(rev_blue);
      return 4;
    }
    // pass on tonecurves, in case lutsize > 0:
    if(cmsIsToneCurveLinear(red_curve))
      lutr[0] = -1.0f;
    else
      for(int k = 0; k < lutsize; k++) lutr[k] = cmsEvalToneCurveFloat(rev_red, k / (lutsize - 1.0f));
    if(cmsIsToneCurveLinear(green_curve))
      lutg[0] = -1.0f;
    else
      for(int k = 0; k < lutsize; k++) lutg[k] = cmsEvalToneCurveFloat(rev_green, k / (lutsize - 1.0f));
    if(cmsIsToneCurveLinear(blue_curve))
      lutb[0] = -1.0f;
    else
      for(int k = 0; k < lutsize; k++) lutb[k] = cmsEvalToneCurveFloat(rev_blue, k / (lutsize - 1.0f));
    cmsFreeToneCurve(rev_red);
    cmsFreeToneCurve(rev_green);
    cmsFreeToneCurve(rev_blue);
  }
  return 0;
}

int dt_colorspaces_get_matrix_from_input_profile(cmsHPROFILE prof, float *matrix, float *lutr, float *lutg,
                                                 float *lutb, const int lutsize)
{
  return dt_colorspaces_get_matrix_from_profile(prof, matrix, lutr, lutg, lutb, lutsize, 1);
}

int dt_colorspaces_get_matrix_from_output_profile(cmsHPROFILE prof, float *matrix, float *lutr, float *lutg,
                                                  float *lutb, const int lutsize)
{
  return dt_colorspaces_get_matrix_from_profile(prof, matrix, lutr, lutg, lutb, lutsize, 0);
}

cmsHPROFILE dt_colorspaces_create_lab_profile()
{
  return cmsCreateLab4Profile(cmsD50_xyY());
}

cmsHPROFILE dt_colorspaces_create_srgb_profile()
{
  cmsHPROFILE hsRGB;

  cmsCIEXYZTRIPLE Colorants = { { 0.436066, 0.222488, 0.013916 },
                                { 0.385147, 0.716873, 0.097076 },
                                { 0.143066, 0.060608, 0.714096 } };

  cmsCIEXYZ black = { 0, 0, 0 };
  cmsCIEXYZ D65 = { 0.95045, 1, 1.08905 };
  cmsToneCurve *transferFunction;

  transferFunction
      = cmsBuildTabulatedToneCurve16(NULL, dt_srgb_tone_curve_values_n, dt_srgb_tone_curve_values);

  hsRGB = cmsCreateProfilePlaceholder(0);

  cmsSetProfileVersion(hsRGB, 2.1);

  cmsMLU *mlu0 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu0, "en", "US", "Public Domain");
  cmsMLU *mlu1 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu1, "en", "US", "sRGB");
  cmsMLU *mlu2 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu2, "en", "US", "Darktable");
  cmsMLU *mlu3 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu3, "en", "US", "sRGB");
  // this will only be displayed when the embedded profile is read by for example GIMP
  cmsWriteTag(hsRGB, cmsSigCopyrightTag, mlu0);
  cmsWriteTag(hsRGB, cmsSigProfileDescriptionTag, mlu1);
  cmsWriteTag(hsRGB, cmsSigDeviceMfgDescTag, mlu2);
  cmsWriteTag(hsRGB, cmsSigDeviceModelDescTag, mlu3);
  cmsMLUfree(mlu0);
  cmsMLUfree(mlu1);
  cmsMLUfree(mlu2);
  cmsMLUfree(mlu3);

  cmsSetDeviceClass(hsRGB, cmsSigDisplayClass);
  cmsSetColorSpace(hsRGB, cmsSigRgbData);
  cmsSetPCS(hsRGB, cmsSigXYZData);

  cmsWriteTag(hsRGB, cmsSigMediaWhitePointTag, &D65);
  cmsWriteTag(hsRGB, cmsSigMediaBlackPointTag, &black);

  cmsWriteTag(hsRGB, cmsSigRedColorantTag, (void *)&Colorants.Red);
  cmsWriteTag(hsRGB, cmsSigGreenColorantTag, (void *)&Colorants.Green);
  cmsWriteTag(hsRGB, cmsSigBlueColorantTag, (void *)&Colorants.Blue);

  cmsWriteTag(hsRGB, cmsSigRedTRCTag, (void *)transferFunction);
  cmsLinkTag(hsRGB, cmsSigGreenTRCTag, cmsSigRedTRCTag);
  cmsLinkTag(hsRGB, cmsSigBlueTRCTag, cmsSigRedTRCTag);

  return hsRGB;
}

// Create the ICC virtual profile for adobe rgb space
cmsHPROFILE dt_colorspaces_create_adobergb_profile(void)
{
  cmsHPROFILE hAdobeRGB;

  cmsCIEXYZTRIPLE Colorants = { { 0.609741, 0.311111, 0.019470 },
                                { 0.205276, 0.625671, 0.060867 },
                                { 0.149185, 0.063217, 0.744568 } };

  cmsCIEXYZ black = { 0, 0, 0 };
  cmsCIEXYZ D65 = { 0.95045, 1, 1.08905 };
  cmsToneCurve *transferFunction;

  // AdobeRGB's "2.2" gamma is technically defined as 2 + 51/256
  transferFunction = cmsBuildGamma(NULL, 2.19921875);

  hAdobeRGB = cmsCreateProfilePlaceholder(0);

  cmsSetProfileVersion(hAdobeRGB, 2.1);

  cmsMLU *mlu0 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu0, "en", "US", "Public Domain");
  cmsMLU *mlu1 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu1, "en", "US", "Adobe RGB (compatible)");
  cmsMLU *mlu2 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu2, "en", "US", "Darktable");
  cmsMLU *mlu3 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu3, "en", "US", "Adobe RGB");
  // this will only be displayed when the embedded profile is read by for example GIMP
  cmsWriteTag(hAdobeRGB, cmsSigCopyrightTag, mlu0);
  cmsWriteTag(hAdobeRGB, cmsSigProfileDescriptionTag, mlu1);
  cmsWriteTag(hAdobeRGB, cmsSigDeviceMfgDescTag, mlu2);
  cmsWriteTag(hAdobeRGB, cmsSigDeviceModelDescTag, mlu3);
  cmsMLUfree(mlu0);
  cmsMLUfree(mlu1);
  cmsMLUfree(mlu2);
  cmsMLUfree(mlu3);

  cmsSetDeviceClass(hAdobeRGB, cmsSigDisplayClass);
  cmsSetColorSpace(hAdobeRGB, cmsSigRgbData);
  cmsSetPCS(hAdobeRGB, cmsSigXYZData);

  cmsWriteTag(hAdobeRGB, cmsSigMediaWhitePointTag, &D65);
  cmsWriteTag(hAdobeRGB, cmsSigMediaBlackPointTag, &black);

  cmsWriteTag(hAdobeRGB, cmsSigRedColorantTag, (void *)&Colorants.Red);
  cmsWriteTag(hAdobeRGB, cmsSigGreenColorantTag, (void *)&Colorants.Green);
  cmsWriteTag(hAdobeRGB, cmsSigBlueColorantTag, (void *)&Colorants.Blue);

  cmsWriteTag(hAdobeRGB, cmsSigRedTRCTag, (void *)transferFunction);
  cmsWriteTag(hAdobeRGB, cmsSigGreenTRCTag, (void *)transferFunction);
  cmsWriteTag(hAdobeRGB, cmsSigBlueTRCTag, (void *)transferFunction);

  return hAdobeRGB;
}

static cmsToneCurve *build_linear_gamma(void)
{
  double Parameters[2];

  Parameters[0] = 1.0;
  Parameters[1] = 0;

  return cmsBuildParametricToneCurve(0, 1, Parameters);
}

static float cbrt_5f(float f)
{
  uint32_t *p = (uint32_t *)&f;
  *p = *p / 3 + 709921077;
  return f;
}

static float cbrta_halleyf(const float a, const float R)
{
  const float a3 = a * a * a;
  const float b = a * (a3 + R + R) / (a3 + a3 + R);
  return b;
}

static float lab_f(const float x)
{
  const float epsilon = 216.0f / 24389.0f;
  const float kappa = 24389.0f / 27.0f;
  if(x > epsilon)
  {
    // approximate cbrtf(x):
    const float a = cbrt_5f(x);
    return cbrta_halleyf(a, x);
  }
  else
    return (kappa * x + 16.0f) / 116.0f;
}

void dt_XYZ_to_Lab(const float *XYZ, float *Lab)
{
  const float d50[3] = { 0.9642, 1.0, 0.8249 };
  const float f[3] = { lab_f(XYZ[0] / d50[0]), lab_f(XYZ[1] / d50[1]), lab_f(XYZ[2] / d50[2]) };
  Lab[0] = 116.0f * f[1] - 16.0f;
  Lab[1] = 500.0f * (f[0] - f[1]);
  Lab[2] = 200.0f * (f[1] - f[2]);
}

static float lab_f_inv(const float x)
{
  const float epsilon = 0.20689655172413796; // cbrtf(216.0f/24389.0f);
  const float kappa = 24389.0f / 27.0f;
  if(x > epsilon)
    return x * x * x;
  else
    return (116.0f * x - 16.0f) / kappa;
}

void dt_Lab_to_XYZ(const float *Lab, float *XYZ)
{
  const float d50[3] = { 0.9642, 1.0, 0.8249 };
  const float fy = (Lab[0] + 16.0f) / 116.0f;
  const float fx = Lab[1] / 500.0f + fy;
  const float fz = fy - Lab[2] / 200.0f;
  XYZ[0] = d50[0] * lab_f_inv(fx);
  XYZ[1] = d50[1] * lab_f_inv(fy);
  XYZ[2] = d50[2] * lab_f_inv(fz);
}


int dt_colorspaces_get_darktable_matrix(const char *makermodel, float *matrix)
{
  dt_profiled_colormatrix_t *preset = NULL;
  for(int k = 0; k < dt_profiled_colormatrix_cnt; k++)
  {
    if(!strcasecmp(makermodel, dt_profiled_colormatrices[k].makermodel))
    {
      preset = dt_profiled_colormatrices + k;
      break;
    }
  }
  if(!preset) return -1;

  const float wxyz = preset->white[0] + preset->white[1] + preset->white[2];
  const float rxyz = preset->rXYZ[0] + preset->rXYZ[1] + preset->rXYZ[2];
  const float gxyz = preset->gXYZ[0] + preset->gXYZ[1] + preset->gXYZ[2];
  const float bxyz = preset->bXYZ[0] + preset->bXYZ[1] + preset->bXYZ[2];

  const float xn = preset->white[0] / wxyz;
  const float yn = preset->white[1] / wxyz;
  const float xr = preset->rXYZ[0] / rxyz;
  const float yr = preset->rXYZ[1] / rxyz;
  const float xg = preset->gXYZ[0] / gxyz;
  const float yg = preset->gXYZ[1] / gxyz;
  const float xb = preset->bXYZ[0] / bxyz;
  const float yb = preset->bXYZ[1] / bxyz;

  const float primaries[9] = { xr, xg, xb, yr, yg, yb, 1.0f - xr - yr, 1.0f - xg - yg, 1.0f - xb - yb };

  float result[9];
  if(mat3inv(result, primaries)) return -1;

  const float whitepoint[3] = { xn / yn, 1.0f, (1.0f - xn - yn) / yn };
  float coeff[3];

  // get inverse primary whitepoint
  mat3mulv(coeff, result, whitepoint);


  float tmp[9] = { coeff[0] * xr, coeff[1] * xg, coeff[2] * xb, coeff[0] * yr, coeff[1] * yg, coeff[2] * yb,
                   coeff[0] * (1.0f - xr - yr), coeff[1] * (1.0f - xg - yg), coeff[2] * (1.0f - xb - yb) };

  // input whitepoint[] in XYZ with Y normalized to 1.0f
  const float dn[3]
      = { preset->white[0] / (float)preset->white[1], 1.0f, preset->white[2] / (float)preset->white[1] };
  const float lam_rigg[9] = { 0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296 };
  const float d50[3] = { 0.9642, 1.0, 0.8249 };


  // adapt to d50
  float chad_inv[9];
  if(mat3inv(chad_inv, lam_rigg)) return -1;

  float cone_src_rgb[3], cone_dst_rgb[3];
  mat3mulv(cone_src_rgb, lam_rigg, dn);
  mat3mulv(cone_dst_rgb, lam_rigg, d50);

  const float cone[9]
      = { cone_dst_rgb[0] / cone_src_rgb[0], 0.0f, 0.0f, 0.0f, cone_dst_rgb[1] / cone_src_rgb[1], 0.0f, 0.0f,
          0.0f, cone_dst_rgb[2] / cone_src_rgb[2] };

  float tmp2[9];
  float bradford[9];
  mat3mul(tmp2, cone, lam_rigg);
  mat3mul(bradford, chad_inv, tmp2);

  mat3mul(matrix, bradford, tmp);
  return 0;
}

cmsHPROFILE dt_colorspaces_create_alternate_profile(const char *makermodel)
{
  dt_profiled_colormatrix_t *preset = NULL;
  for(int k = 0; k < dt_alternate_colormatrix_cnt; k++)
  {
    if(!strcmp(makermodel, dt_alternate_colormatrices[k].makermodel))
    {
      preset = dt_alternate_colormatrices + k;
      break;
    }
  }
  if(!preset) return NULL;

  const float wxyz = preset->white[0] + preset->white[1] + preset->white[2];
  const float rxyz = preset->rXYZ[0] + preset->rXYZ[1] + preset->rXYZ[2];
  const float gxyz = preset->gXYZ[0] + preset->gXYZ[1] + preset->gXYZ[2];
  const float bxyz = preset->bXYZ[0] + preset->bXYZ[1] + preset->bXYZ[2];
  cmsCIExyY WP = { preset->white[0] / wxyz, preset->white[1] / wxyz, 1.0 };
  cmsCIExyYTRIPLE XYZPrimaries = { { preset->rXYZ[0] / rxyz, preset->rXYZ[1] / rxyz, 1.0 },
                                   { preset->gXYZ[0] / gxyz, preset->gXYZ[1] / gxyz, 1.0 },
                                   { preset->bXYZ[0] / bxyz, preset->bXYZ[1] / bxyz, 1.0 } };
  cmsToneCurve *Gamma[3];
  cmsHPROFILE hp;

  Gamma[0] = Gamma[1] = Gamma[2] = build_linear_gamma();

  hp = cmsCreateRGBProfile(&WP, &XYZPrimaries, Gamma);
  cmsFreeToneCurve(Gamma[0]);
  if(hp == NULL) return NULL;

  char name[512];
  snprintf(name, sizeof(name), "darktable alternate %s", makermodel);
  cmsSetProfileVersion(hp, 2.1);
  cmsMLU *mlu0 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu0, "en", "US", "(dt internal)");
  cmsMLU *mlu1 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu1, "en", "US", name);
  cmsMLU *mlu2 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu2, "en", "US", name);
  cmsWriteTag(hp, cmsSigDeviceMfgDescTag, mlu0);
  cmsWriteTag(hp, cmsSigDeviceModelDescTag, mlu1);
  // this will only be displayed when the embedded profile is read by for example GIMP
  cmsWriteTag(hp, cmsSigProfileDescriptionTag, mlu2);
  cmsMLUfree(mlu0);
  cmsMLUfree(mlu1);
  cmsMLUfree(mlu2);

  return hp;
}

cmsHPROFILE dt_colorspaces_create_vendor_profile(const char *makermodel)
{
  dt_profiled_colormatrix_t *preset = NULL;
  for(int k = 0; k < dt_vendor_colormatrix_cnt; k++)
  {
    if(!strcmp(makermodel, dt_vendor_colormatrices[k].makermodel))
    {
      preset = dt_vendor_colormatrices + k;
      break;
    }
  }
  if(!preset) return NULL;

  const float wxyz = preset->white[0] + preset->white[1] + preset->white[2];
  const float rxyz = preset->rXYZ[0] + preset->rXYZ[1] + preset->rXYZ[2];
  const float gxyz = preset->gXYZ[0] + preset->gXYZ[1] + preset->gXYZ[2];
  const float bxyz = preset->bXYZ[0] + preset->bXYZ[1] + preset->bXYZ[2];
  cmsCIExyY WP = { preset->white[0] / wxyz, preset->white[1] / wxyz, 1.0 };
  cmsCIExyYTRIPLE XYZPrimaries = { { preset->rXYZ[0] / rxyz, preset->rXYZ[1] / rxyz, 1.0 },
                                   { preset->gXYZ[0] / gxyz, preset->gXYZ[1] / gxyz, 1.0 },
                                   { preset->bXYZ[0] / bxyz, preset->bXYZ[1] / bxyz, 1.0 } };
  cmsToneCurve *Gamma[3];
  cmsHPROFILE hp;

  Gamma[0] = Gamma[1] = Gamma[2] = build_linear_gamma();

  hp = cmsCreateRGBProfile(&WP, &XYZPrimaries, Gamma);
  cmsFreeToneCurve(Gamma[0]);
  if(hp == NULL) return NULL;

  char name[512];
  snprintf(name, sizeof(name), "darktable vendor %s", makermodel);
  cmsSetProfileVersion(hp, 2.1);
  cmsMLU *mlu0 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu0, "en", "US", "(dt internal)");
  cmsMLU *mlu1 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu1, "en", "US", name);
  cmsMLU *mlu2 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu2, "en", "US", name);
  cmsWriteTag(hp, cmsSigDeviceMfgDescTag, mlu0);
  cmsWriteTag(hp, cmsSigDeviceModelDescTag, mlu1);
  // this will only be displayed when the embedded profile is read by for example GIMP
  cmsWriteTag(hp, cmsSigProfileDescriptionTag, mlu2);
  cmsMLUfree(mlu0);
  cmsMLUfree(mlu1);
  cmsMLUfree(mlu2);

  return hp;
}

cmsHPROFILE dt_colorspaces_create_darktable_profile(const char *makermodel)
{
  dt_profiled_colormatrix_t *preset = NULL;
  for(int k = 0; k < dt_profiled_colormatrix_cnt; k++)
  {
    if(!strcasecmp(makermodel, dt_profiled_colormatrices[k].makermodel))
    {
      preset = dt_profiled_colormatrices + k;
      break;
    }
  }
  if(!preset) return NULL;

  const float wxyz = preset->white[0] + preset->white[1] + preset->white[2];
  const float rxyz = preset->rXYZ[0] + preset->rXYZ[1] + preset->rXYZ[2];
  const float gxyz = preset->gXYZ[0] + preset->gXYZ[1] + preset->gXYZ[2];
  const float bxyz = preset->bXYZ[0] + preset->bXYZ[1] + preset->bXYZ[2];
  cmsCIExyY WP = { preset->white[0] / wxyz, preset->white[1] / wxyz, 1.0 };
  cmsCIExyYTRIPLE XYZPrimaries = { { preset->rXYZ[0] / rxyz, preset->rXYZ[1] / rxyz, 1.0 },
                                   { preset->gXYZ[0] / gxyz, preset->gXYZ[1] / gxyz, 1.0 },
                                   { preset->bXYZ[0] / bxyz, preset->bXYZ[1] / bxyz, 1.0 } };
  cmsToneCurve *Gamma[3];
  cmsHPROFILE hp;

  Gamma[0] = Gamma[1] = Gamma[2] = build_linear_gamma();

  hp = cmsCreateRGBProfile(&WP, &XYZPrimaries, Gamma);
  cmsFreeToneCurve(Gamma[0]);
  if(hp == NULL) return NULL;

  char name[512];
  snprintf(name, sizeof(name), "Darktable profiled %s", makermodel);
  cmsSetProfileVersion(hp, 2.1);
  cmsMLU *mlu0 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu0, "en", "US", "(dt internal)");
  cmsMLU *mlu1 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu1, "en", "US", name);
  cmsMLU *mlu2 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu2, "en", "US", name);
  cmsWriteTag(hp, cmsSigDeviceMfgDescTag, mlu0);
  cmsWriteTag(hp, cmsSigDeviceModelDescTag, mlu1);
  // this will only be displayed when the embedded profile is read by for example GIMP
  cmsWriteTag(hp, cmsSigProfileDescriptionTag, mlu2);
  cmsMLUfree(mlu0);
  cmsMLUfree(mlu1);
  cmsMLUfree(mlu2);

  return hp;
}

cmsHPROFILE dt_colorspaces_create_xyz_profile(void)
{
  cmsHPROFILE hXYZ = cmsCreateXYZProfile();
  // revert some settings which prevent us from using XYZ as output profile:
  cmsSetDeviceClass(hXYZ, cmsSigDisplayClass);
  cmsSetColorSpace(hXYZ, cmsSigRgbData);
  cmsSetPCS(hXYZ, cmsSigXYZData);
  cmsSetHeaderRenderingIntent(hXYZ, INTENT_PERCEPTUAL);

  if(hXYZ == NULL) return NULL;

  cmsSetProfileVersion(hXYZ, 2.1);
  cmsMLU *mlu0 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu0, "en", "US", "(dt internal)");
  cmsMLU *mlu1 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu1, "en", "US", "linear XYZ");
  cmsMLU *mlu2 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu2, "en", "US", "Darktable linear XYZ");
  cmsWriteTag(hXYZ, cmsSigDeviceMfgDescTag, mlu0);
  cmsWriteTag(hXYZ, cmsSigDeviceModelDescTag, mlu1);
  // this will only be displayed when the embedded profile is read by for example GIMP
  cmsWriteTag(hXYZ, cmsSigProfileDescriptionTag, mlu2);
  cmsMLUfree(mlu0);
  cmsMLUfree(mlu1);
  cmsMLUfree(mlu2);

  return hXYZ;
}

cmsHPROFILE dt_colorspaces_create_linear_rec709_rgb_profile(void)
{
  cmsHPROFILE hRec709RGB;

  cmsCIEXYZTRIPLE Colorants = { { 0.436066, 0.222488, 0.013916 },
                                { 0.385147, 0.716873, 0.097076 },
                                { 0.143066, 0.060608, 0.714096 } };

  cmsCIEXYZ black = { 0, 0, 0 };
  cmsCIEXYZ D65 = { 0.95045, 1, 1.08905 };
  cmsToneCurve *transferFunction;

  transferFunction = build_linear_gamma();

  hRec709RGB = cmsCreateProfilePlaceholder(0);

  cmsSetProfileVersion(hRec709RGB, 2.1);

  cmsMLU *mlu0 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu0, "en", "US", "Public Domain");
  cmsMLU *mlu1 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu1, "en", "US", "Linear Rec709 RGB");
  cmsMLU *mlu2 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu2, "en", "US", "Darktable");
  cmsMLU *mlu3 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu3, "en", "US", "Linear Rec709 RGB");
  // this will only be displayed when the embedded profile is read by for example GIMP
  cmsWriteTag(hRec709RGB, cmsSigCopyrightTag, mlu0);
  cmsWriteTag(hRec709RGB, cmsSigProfileDescriptionTag, mlu1);
  cmsWriteTag(hRec709RGB, cmsSigDeviceMfgDescTag, mlu2);
  cmsWriteTag(hRec709RGB, cmsSigDeviceModelDescTag, mlu3);
  cmsMLUfree(mlu0);
  cmsMLUfree(mlu1);
  cmsMLUfree(mlu2);
  cmsMLUfree(mlu3);

  cmsSetDeviceClass(hRec709RGB, cmsSigDisplayClass);
  cmsSetColorSpace(hRec709RGB, cmsSigRgbData);
  cmsSetPCS(hRec709RGB, cmsSigXYZData);

  cmsWriteTag(hRec709RGB, cmsSigMediaWhitePointTag, &D65);
  cmsWriteTag(hRec709RGB, cmsSigMediaBlackPointTag, &black);

  cmsWriteTag(hRec709RGB, cmsSigRedColorantTag, (void *)&Colorants.Red);
  cmsWriteTag(hRec709RGB, cmsSigGreenColorantTag, (void *)&Colorants.Green);
  cmsWriteTag(hRec709RGB, cmsSigBlueColorantTag, (void *)&Colorants.Blue);

  cmsWriteTag(hRec709RGB, cmsSigRedTRCTag, (void *)transferFunction);
  cmsWriteTag(hRec709RGB, cmsSigGreenTRCTag, (void *)transferFunction);
  cmsWriteTag(hRec709RGB, cmsSigBlueTRCTag, (void *)transferFunction);

  return hRec709RGB;
}

cmsHPROFILE dt_colorspaces_create_linear_rec2020_rgb_profile(void)
{
  cmsHPROFILE hRec2020RGB;

  cmsCIEXYZTRIPLE Colorants = { { 0.673492, 0.279037, -0.001938 },
                                { 0.165665, 0.675354, 0.029984 },
                                { 0.125046, 0.045609, 0.796860 } };

  cmsCIEXYZ black = { 0, 0, 0 };
  cmsCIEXYZ D65 = { 0.95045, 1, 1.08905 };
  cmsToneCurve *transferFunction;

  transferFunction = build_linear_gamma();

  hRec2020RGB = cmsCreateProfilePlaceholder(0);

  cmsSetProfileVersion(hRec2020RGB, 2.1);

  cmsMLU *mlu0 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu0, "en", "US", "Public Domain");
  cmsMLU *mlu1 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu1, "en", "US", "Linear Rec2020 RGB");
  cmsMLU *mlu2 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu2, "en", "US", "Darktable");
  cmsMLU *mlu3 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu3, "en", "US", "Linear Rec2020 RGB");
  // this will only be displayed when the embedded profile is read by for example GIMP
  cmsWriteTag(hRec2020RGB, cmsSigCopyrightTag, mlu0);
  cmsWriteTag(hRec2020RGB, cmsSigProfileDescriptionTag, mlu1);
  cmsWriteTag(hRec2020RGB, cmsSigDeviceMfgDescTag, mlu2);
  cmsWriteTag(hRec2020RGB, cmsSigDeviceModelDescTag, mlu3);
  cmsMLUfree(mlu0);
  cmsMLUfree(mlu1);
  cmsMLUfree(mlu2);
  cmsMLUfree(mlu3);

  cmsSetDeviceClass(hRec2020RGB, cmsSigDisplayClass);
  cmsSetColorSpace(hRec2020RGB, cmsSigRgbData);
  cmsSetPCS(hRec2020RGB, cmsSigXYZData);

  cmsWriteTag(hRec2020RGB, cmsSigMediaWhitePointTag, &D65);
  cmsWriteTag(hRec2020RGB, cmsSigMediaBlackPointTag, &black);

  cmsWriteTag(hRec2020RGB, cmsSigRedColorantTag, (void *)&Colorants.Red);
  cmsWriteTag(hRec2020RGB, cmsSigGreenColorantTag, (void *)&Colorants.Green);
  cmsWriteTag(hRec2020RGB, cmsSigBlueColorantTag, (void *)&Colorants.Blue);

  cmsWriteTag(hRec2020RGB, cmsSigRedTRCTag, (void *)transferFunction);
  cmsWriteTag(hRec2020RGB, cmsSigGreenTRCTag, (void *)transferFunction);
  cmsWriteTag(hRec2020RGB, cmsSigBlueTRCTag, (void *)transferFunction);

  return hRec2020RGB;
}

cmsHPROFILE dt_colorspaces_create_linear_infrared_profile(void)
{
  // linear rgb with r and b swapped:
  cmsCIExyY D65;
  cmsCIExyYTRIPLE Rec709Primaries
      = { { 0.1500, 0.0600, 1.0 }, { 0.3000, 0.6000, 1.0 }, { 0.6400, 0.3300, 1.0 } };
  cmsToneCurve *Gamma[3];
  cmsHPROFILE hsRGB;

  cmsWhitePointFromTemp(&D65, 6504.0);
  Gamma[0] = Gamma[1] = Gamma[2] = build_linear_gamma();

  hsRGB = cmsCreateRGBProfile(&D65, &Rec709Primaries, Gamma);
  cmsFreeToneCurve(Gamma[0]);
  if(hsRGB == NULL) return NULL;

  cmsSetProfileVersion(hsRGB, 2.1);
  cmsMLU *mlu0 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu0, "en", "US", "(dt internal)");
  cmsMLU *mlu1 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu1, "en", "US", "linear infrared bgr");
  cmsMLU *mlu2 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu2, "en", "US", "Darktable Linear Infrared BGR");
  cmsWriteTag(hsRGB, cmsSigDeviceMfgDescTag, mlu0);
  cmsWriteTag(hsRGB, cmsSigDeviceModelDescTag, mlu1);
  // this will only be displayed when the embedded profile is read by for example GIMP
  cmsWriteTag(hsRGB, cmsSigProfileDescriptionTag, mlu2);
  cmsMLUfree(mlu0);
  cmsMLUfree(mlu1);
  cmsMLUfree(mlu2);

  return hsRGB;
}

int dt_colorspaces_find_profile(char *filename, size_t filename_len, const char *profile, const char *inout)
{
  char datadir[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(datadir, sizeof(datadir));
  snprintf(filename, filename_len, "%s/color/%s/%s", datadir, inout, profile);
  if(!g_file_test(filename, G_FILE_TEST_IS_REGULAR))
  {
    dt_loc_get_datadir(datadir, sizeof(datadir));
    snprintf(filename, filename_len, "%s/color/%s/%s", datadir, inout, profile);
    if(!g_file_test(filename, G_FILE_TEST_IS_REGULAR)) return 1;
  }
  return 0;
}

char *dt_colorspaces_get_output_profile_name(const int imgid)
{
  // find the colorout module -- the pointer stays valid until darktable shuts down
  static dt_iop_module_so_t *colorout = NULL;
  if(colorout == NULL)
  {
    GList *modules = g_list_first(darktable.iop);
    while(modules)
    {
      dt_iop_module_so_t *module = (dt_iop_module_so_t *)(modules->data);
      if(!strcmp(module->op, "colorout"))
      {
        colorout = module;
        break;
      }
      modules = g_list_next(modules);
    }
  }

  char profile[PATH_MAX] = { 0 };
  profile[0] = '\0';
  // db lookup colorout params, and dt_conf_() for override
  gchar *overprofile = dt_conf_get_string("plugins/lighttable/export/iccprofile");
  if(colorout && colorout->get_p && (!overprofile || !strcmp(overprofile, "image")))
  {
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(
        dt_database_get(darktable.db),
        "SELECT op_params FROM history WHERE imgid=?1 AND operation='colorout' ORDER BY num DESC LIMIT 1", -1,
        &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      // use introspection to get the profile name from the binary params blob
      const void *params = sqlite3_column_blob(stmt, 0);
      char *iccprofile = colorout->get_p(params, "iccprofile");
      g_strlcpy(profile, iccprofile, sizeof(profile));
    }
    sqlite3_finalize(stmt);
  }
  if(!overprofile && profile[0] == '\0')
  {
    g_strlcpy(profile, "sRGB", sizeof(profile));
  }
  else if(profile[0] == '\0')
  {
    g_strlcpy(profile, overprofile, sizeof(profile));
  }

  g_free(overprofile);

  return g_strdup(profile);
}

cmsHPROFILE dt_colorspaces_create_output_profile(const int imgid)
{
  char *profile = dt_colorspaces_get_output_profile_name(imgid);

  cmsHPROFILE output = NULL;

  if(!strcmp(profile, "sRGB"))
    output = dt_colorspaces_create_srgb_profile();
  else if(!strcmp(profile, "linear_rec709_rgb") || !strcmp(profile, "linear_rgb"))
    output = dt_colorspaces_create_linear_rec709_rgb_profile();
  else if(!strcmp(profile, "linear_rec2020_rgb"))
    output = dt_colorspaces_create_linear_rec2020_rgb_profile();
  else if(!strcmp(profile, "XYZ"))
    output = dt_colorspaces_create_xyz_profile();
  else if(!strcmp(profile, "adobergb"))
    output = dt_colorspaces_create_adobergb_profile();
  else if(!strcmp(profile, "X profile"))
  {
    pthread_rwlock_rdlock(&darktable.control->xprofile_lock);
    if(darktable.control->xprofile_data)
      output = cmsOpenProfileFromMem(darktable.control->xprofile_data, darktable.control->xprofile_size);
    pthread_rwlock_unlock(&darktable.control->xprofile_lock);
  }
  else
  {
    // else: load file name
    char filename[PATH_MAX] = { 0 };
    dt_colorspaces_find_profile(filename, sizeof(filename), profile, "out");
    output = cmsOpenProfileFromFile(filename, "r");
  }
  if(!output) output = dt_colorspaces_create_srgb_profile();
  g_free(profile);
  return output;
}

cmsHPROFILE dt_colorspaces_create_cmatrix_profile(float cmatrix[3][4])
{
  float mat[3][3];
  // sRGB D65, the linear part:
  const float rgb_to_xyz[3][3] = { { 0.4124564, 0.3575761, 0.1804375 },
                                   { 0.2126729, 0.7151522, 0.0721750 },
                                   { 0.0193339, 0.1191920, 0.9503041 } };

  for(int c = 0; c < 3; c++)
    for(int j = 0; j < 3; j++)
    {
      mat[c][j] = 0;
      for(int k = 0; k < 3; k++) mat[c][j] += rgb_to_xyz[c][k] * cmatrix[k][j];
    }
  return dt_colorspaces_create_xyzmatrix_profile(mat);
}

cmsHPROFILE dt_colorspaces_create_xyzimatrix_profile(float mat[3][3])
{
  // mat: xyz -> cam
  float imat[3][3];
  mat3inv((float *)imat, (float *)mat);
  return dt_colorspaces_create_xyzmatrix_profile(imat);
}

cmsHPROFILE dt_colorspaces_create_xyzmatrix_profile(float mat[3][3])
{
  // mat: cam -> xyz
  cmsCIExyY D65;
  float x[3], y[3];
  for(int k = 0; k < 3; k++)
  {
    const float norm = mat[0][k] + mat[1][k] + mat[2][k];
    x[k] = mat[0][k] / norm;
    y[k] = mat[1][k] / norm;
  }
  cmsCIExyYTRIPLE CameraPrimaries = { { x[0], y[0], 1.0 }, { x[1], y[1], 1.0 }, { x[2], y[2], 1.0 } };
  cmsHPROFILE cmat;

  cmsWhitePointFromTemp(&D65, 6504.0);

  cmsToneCurve *Gamma[3];
  Gamma[0] = Gamma[1] = Gamma[2] = build_linear_gamma();
  cmat = cmsCreateRGBProfile(&D65, &CameraPrimaries, Gamma);
  if(cmat == NULL) return NULL;
  cmsFreeToneCurve(Gamma[0]);

  cmsSetProfileVersion(cmat, 2.1);
  cmsMLU *mlu0 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu0, "en", "US", "(dt internal)");
  cmsMLU *mlu1 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu1, "en", "US", "color matrix built-in");
  cmsMLU *mlu2 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu2, "en", "US", "color matrix built-in");
  cmsWriteTag(cmat, cmsSigDeviceMfgDescTag, mlu0);
  cmsWriteTag(cmat, cmsSigDeviceModelDescTag, mlu1);
  // this will only be displayed when the embedded profile is read by for example GIMP
  cmsWriteTag(cmat, cmsSigProfileDescriptionTag, mlu2);
  cmsMLUfree(mlu0);
  cmsMLUfree(mlu1);
  cmsMLUfree(mlu2);

  return cmat;
}

void dt_colorspaces_cleanup_profile(cmsHPROFILE p)
{
  cmsCloseProfile(p);
}

void dt_colorspaces_get_makermodel(char *makermodel, size_t makermodel_len, const char *const maker,
                                   const char *const model)
{
  // if first word in maker == first word in model, use just model.
  const char *c, *d;
  char *e;
  c = maker;
  d = model;
  int match = 1;
  const char *end = maker + strlen(maker);
  while(*c != ' ' && c < end)
    if(*(c++) != *(d++))
    {
      match = 0;
      break;
    }
  if(match)
  {
    snprintf(makermodel, makermodel_len, "%s", model);
  }
  else if(!strcmp(maker, "KONICA MINOLTA")
          && (!strncmp(model, "MAXXUM", 6) || !strncmp(model, "DYNAX", 5) || !strncmp(model, "ALPHA", 5)))
  {
    // Use the dcraw name name for the Konica Minolta 5D/7D (without Konica)
    int numoffset = !strncmp(model, "MAXXUM", 6) ? 7 : 6;
    snprintf(makermodel, makermodel_len, "MINOLTA DYNAX %s", model + numoffset);
  }
  else if(!strncmp(maker, "Konica Minolta", 14) || !strncmp(maker, "KONICA MINOLTA", 14))
  {
    // Identify Konica Minolta cameras as just Minolta internally
    snprintf(makermodel, makermodel_len, "Minolta %s", model);
  }
  else
  {
    // else need to append first word of the maker:
    c = maker;
    d = model;
    const char *end = maker + strlen(maker);
    for(e = makermodel; c < end && *c != ' '; c++, e++) *e = *c;
    // separate with space
    *(e++) = ' ';
    // and continue with model.
    // need FinePix gone from some fuji cameras to match dcraws description:
    if(!strncmp(model, "FinePix", 7))
      snprintf(e, makermodel_len - (d - maker), "%s", model + 8);
    else
      snprintf(e, makermodel_len - (d - maker), "%s", model);
  }
  // strip trailing spaces
  e = makermodel + strlen(makermodel) - 1;
  while(e > makermodel && *e == ' ') e--;
  e[1] = '\0';
}

void dt_colorspaces_get_makermodel_split(char *makermodel, size_t makermodel_len, char **modelo,
                                         const char *const maker, const char *const model)
{
  dt_colorspaces_get_makermodel(makermodel, makermodel_len, maker, model);
  *modelo = makermodel;
  const char *end = makermodel + strlen(makermodel);
  for(; **modelo != ' ' && *modelo < end; (*modelo)++)
    ;
  **modelo = '\0';
  (*modelo)++;
}

void dt_colorspaces_get_profile_name(cmsHPROFILE p, const char *language, const char *country, char *name,
                                     size_t len)
{
  cmsUInt32Number size;
  gchar *buf = NULL;
  wchar_t *wbuf = NULL;
  gchar *utf8 = NULL;

  size = cmsGetProfileInfoASCII(p, cmsInfoDescription, language, country, NULL, 0);
  if(size == 0) goto error;

  buf = (char *)calloc(size + 1, sizeof(char));
  size = cmsGetProfileInfoASCII(p, cmsInfoDescription, language, country, buf, size);
  if(size == 0) goto error;

  // most unix like systems should work with this, but at least Windows doesn't
  if(sizeof(wchar_t) != 4 || g_utf8_validate(buf, -1, NULL))
    g_strlcpy(name, buf, len); // better a little weird than totally borked
  else
  {
    wbuf = (wchar_t *)calloc(size + 1, sizeof(wchar_t));
    size = cmsGetProfileInfo(p, cmsInfoDescription, language, country, wbuf, sizeof(wchar_t) * size);
    if(size == 0) goto error;
    utf8 = g_ucs4_to_utf8((gunichar *)wbuf, -1, NULL, NULL, NULL);
    if(!utf8) goto error;
    g_strlcpy(name, utf8, len);
  }

  free(buf);
  free(wbuf);
  g_free(utf8);
  return;

error:
  if(buf)
    g_strlcpy(name, buf, len); // better a little weird than totally borked
  else
    *name = '\0'; // nothing to do here
  free(buf);
  free(wbuf);
  g_free(utf8);
}

void rgb2hsl(const float rgb[3], float *h, float *s, float *l)
{
  const float r = rgb[0], g = rgb[1], b = rgb[2];
  float pmax = fmax(r, fmax(g, b));
  float pmin = fmin(r, fmin(g, b));
  float delta = (pmax - pmin);

  float hv = 0, sv = 0, lv = (pmin + pmax) / 2.0;

  if(pmax != pmin)
  {
    sv = lv < 0.5 ? delta / (pmax + pmin) : delta / (2.0 - pmax - pmin);

    if(pmax == r)
      hv = (g - b) / delta;
    else if(pmax == g)
      hv = 2.0 + (b - r) / delta;
    else if(pmax == b)
      hv = 4.0 + (r - g) / delta;
    hv /= 6.0;
    if(hv < 0.0)
      hv += 1.0;
    else if(hv > 1.0)
      hv -= 1.0;
  }
  *h = hv;
  *s = sv;
  *l = lv;
}

static inline float hue2rgb(float m1, float m2, float hue)
{
  if(hue < 0.0)
    hue += 1.0;
  else if(hue > 1.0)
    hue -= 1.0;

  if(hue < 1.0 / 6.0)
    return (m1 + (m2 - m1) * hue * 6.0);
  else if(hue < 1.0 / 2.0)
    return m2;
  else if(hue < 2.0 / 3.0)
    return (m1 + (m2 - m1) * ((2.0 / 3.0) - hue) * 6.0);
  else
    return m1;
}

void hsl2rgb(float rgb[3], float h, float s, float l)
{
  float m1, m2;
  if(s == 0)
  {
    rgb[0] = rgb[1] = rgb[2] = l;
    return;
  }
  m2 = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
  m1 = (2.0 * l - m2);
  rgb[0] = hue2rgb(m1, m2, h + (1.0 / 3.0));
  rgb[1] = hue2rgb(m1, m2, h);
  rgb[2] = hue2rgb(m1, m2, h - (1.0 / 3.0));
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
