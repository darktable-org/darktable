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

#include "common/colorspaces.h"
#include "common/colormatrices.c"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/file_location.h"
#include "common/math.h"
#include "common/matrices.h"
#include "common/srgb_tone_curve_values.h"
#include "common/utility.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/imageop.h"

#include <strings.h>

#ifdef USE_COLORDGTK
#include "colord-gtk.h"
#endif

#if 0
#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#include <CoreServices/CoreServices.h>
#endif

static const cmsCIEXYZ d65 = {0.95045471, 1.00000000, 1.08905029};

//D65 (sRGB, AdobeRGB, Rec2020)
static const cmsCIExyY D65xyY = {0.312700492, 0.329000939, 1.0};

//D60
//static const cmsCIExyY d60 = {0.32168, 0.33767, 1.0};

//D50 (ProPhoto RGB)
static const cmsCIExyY D50xyY = {0.3457, 0.3585, 1.0};

// D65:
static const cmsCIExyYTRIPLE sRGB_Primaries = {
  {0.6400, 0.3300, 1.0}, // red
  {0.3000, 0.6000, 1.0}, // green
  {0.1500, 0.0600, 1.0}  // blue
};

// D65:
static const cmsCIExyYTRIPLE Rec2020_Primaries = {
  {0.7080, 0.2920, 1.0}, // red
  {0.1700, 0.7970, 1.0}, // green
  {0.1310, 0.0460, 1.0}  // blue
};

// D65:
static const cmsCIExyYTRIPLE Rec709_Primaries = {
  {0.6400, 0.3300, 1.0}, // red
  {0.3000, 0.6000, 1.0}, // green
  {0.1500, 0.0600, 1.0}  // blue
};

// D65:
static const cmsCIExyYTRIPLE Adobe_Primaries = {
  {0.6400, 0.3300, 1.0}, // red
  {0.2100, 0.7100, 1.0}, // green
  {0.1500, 0.0600, 1.0}  // blue
};

// D65:
static const cmsCIExyYTRIPLE P3_Primaries = {
  {0.680, 0.320, 1.0}, // red
  {0.265, 0.690, 1.0}, // green
  {0.150, 0.060, 1.0}  // blue
};

// https://en.wikipedia.org/wiki/ProPhoto_RGB_color_space
// D50:
static const cmsCIExyYTRIPLE ProPhoto_Primaries = {
  /*       x,        y,       Y */
  { 0.734699, 0.265301, 1.0000 }, /* red   */
  { 0.159597, 0.840403, 1.0000 }, /* green */
  { 0.036598, 0.000105, 1.0000 }, /* blue  */
};

cmsCIEXYZTRIPLE Rec709_Primaries_Prequantized;

static const dt_colorspaces_color_profile_t *_get_profile(dt_colorspaces_t *self,
                                                          dt_colorspaces_color_profile_type_t type,
                                                          const char *filename,
                                                          dt_colorspaces_profile_direction_t direction);

static int dt_colorspaces_get_matrix_from_profile(cmsHPROFILE prof, dt_colormatrix_t matrix, float *lutr, float *lutg,
                                                  float *lutb, const int lutsize, const int input)
{
  // create an OpenCL processable matrix + tone curves from an cmsHPROFILE:
  // NOTE: may be invoked with matrix and LUT pointers set to null to find
  // out if the profile can be created at all.

  // check this first:
  if(!prof || !cmsIsMatrixShaper(prof)) return 1;

  // there are some profiles that contain both a color LUT for some specific
  // intent and a generic matrix. in some cases the matrix might be
  // deliberately wrong with swapped blue and red channels in order to easily
  // detect if a color managed software is applying the LUT or the matrix.
  // thus, if this profile contains LUT for any intent, it might also contain
  // swapped matrix, so the only right way to handle it is to let LCMS apply it.
  const int UsedDirection = input ? LCMS_USED_AS_INPUT : LCMS_USED_AS_OUTPUT;

  if(cmsIsCLUT(prof, INTENT_PERCEPTUAL, UsedDirection)
     || cmsIsCLUT(prof, INTENT_RELATIVE_COLORIMETRIC, UsedDirection)
     || cmsIsCLUT(prof, INTENT_ABSOLUTE_COLORIMETRIC, UsedDirection)
     || cmsIsCLUT(prof, INTENT_SATURATION, UsedDirection))
    return 1;

  cmsToneCurve *red_curve = cmsReadTag(prof, cmsSigRedTRCTag);
  cmsToneCurve *green_curve = cmsReadTag(prof, cmsSigGreenTRCTag);
  cmsToneCurve *blue_curve = cmsReadTag(prof, cmsSigBlueTRCTag);

  cmsCIEXYZ *red_color = cmsReadTag(prof, cmsSigRedColorantTag);
  cmsCIEXYZ *green_color = cmsReadTag(prof, cmsSigGreenColorantTag);
  cmsCIEXYZ *blue_color = cmsReadTag(prof, cmsSigBlueColorantTag);

  if(!red_curve || !green_curve || !blue_curve || !red_color || !green_color || !blue_color) return 2;

  dt_colormatrix_t matrix_tmp = { { red_color->X, green_color->X, blue_color->X },
                                  { red_color->Y, green_color->Y, blue_color->Y },
                                  { red_color->Z, green_color->Z,  blue_color->Z } };

  // some camera ICC profiles claim to have color locations for red, green and blue base colors defined,
  // but in fact these are all set to zero. we catch this case here.
  float sum = 0.0f;
  for(int k1 = 0; k1 < 3; k1++)
    for(int k2 = 0; k2 < 3; k2++)
      sum += matrix_tmp[k1][k2];
  if(sum == 0.0f) return 3;

  if(input && lutr && lutg && lutb)
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
    dt_colormatrix_t tmp;
    memcpy(tmp, matrix_tmp, sizeof(dt_colormatrix_t));
    if(mat3SSEinv(matrix_tmp, tmp))
      return 3;
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

    if(lutr && lutg && lutb)
    {
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
    }

    cmsFreeToneCurve(rev_red);
    cmsFreeToneCurve(rev_green);
    cmsFreeToneCurve(rev_blue);
  }

  if(matrix)
    memcpy(matrix, matrix_tmp, sizeof(dt_colormatrix_t));

  return 0;
}

int dt_colorspaces_get_matrix_from_input_profile(cmsHPROFILE prof, dt_colormatrix_t matrix, float *lutr, float *lutg,
                                                 float *lutb, const int lutsize)
{
  return dt_colorspaces_get_matrix_from_profile(prof, matrix, lutr, lutg, lutb, lutsize, 1);
}

int dt_colorspaces_get_matrix_from_output_profile(cmsHPROFILE prof, dt_colormatrix_t matrix, float *lutr, float *lutg,
                                                  float *lutb, const int lutsize)
{
  return dt_colorspaces_get_matrix_from_profile(prof, matrix, lutr, lutg, lutb, lutsize, 0);
}

static cmsHPROFILE dt_colorspaces_create_lab_profile()
{
  return cmsCreateLab4Profile(cmsD50_xyY());
}

static void _compute_prequantized_primaries(const cmsCIExyY* whitepoint,
                                            const cmsCIExyYTRIPLE* primaries,
                                            cmsCIEXYZTRIPLE *primaries_prequantized)
{
  cmsHPROFILE profile = cmsCreateRGBProfile(whitepoint, primaries, NULL);

  cmsCIEXYZ *R = cmsReadTag(profile, cmsSigRedColorantTag);
  cmsCIEXYZ *G = cmsReadTag(profile, cmsSigGreenColorantTag);
  cmsCIEXYZ *B = cmsReadTag(profile, cmsSigBlueColorantTag);

  primaries_prequantized->Red.X   = (double)R->X;
  primaries_prequantized->Red.Y   = (double)R->Y;
  primaries_prequantized->Red.Z   = (double)R->Z;

  primaries_prequantized->Green.X = (double)G->X;
  primaries_prequantized->Green.Y = (double)G->Y;
  primaries_prequantized->Green.Z = (double)G->Z;

  primaries_prequantized->Blue.X  = (double)B->X;
  primaries_prequantized->Blue.Y  = (double)B->Y;
  primaries_prequantized->Blue.Z  = (double)B->Z;

  cmsCloseProfile(profile);
}

static cmsHPROFILE _create_lcms_profile(const char *desc, const char *dmdd,
                                        const cmsCIExyY *whitepoint, const cmsCIExyYTRIPLE *primaries, cmsToneCurve *trc,
                                        gboolean v2)
{
  cmsMLU *mlu1 = cmsMLUalloc(NULL, 1);
  cmsMLU *mlu2 = cmsMLUalloc(NULL, 1);
  cmsMLU *mlu3 = cmsMLUalloc(NULL, 1);
  cmsMLU *mlu4 = cmsMLUalloc(NULL, 1);

  cmsToneCurve *out_curves[3] = { trc, trc, trc };
  cmsHPROFILE profile = cmsCreateRGBProfile(whitepoint, primaries, out_curves);

  if(v2) cmsSetProfileVersion(profile, 2.4);

  cmsSetHeaderFlags(profile, cmsEmbeddedProfileTrue);

  cmsMLUsetASCII(mlu1, "en", "US", "Public Domain");
  cmsWriteTag(profile, cmsSigCopyrightTag, mlu1);

  cmsMLUsetASCII(mlu2, "en", "US", desc);
  cmsWriteTag(profile, cmsSigProfileDescriptionTag, mlu2);

  cmsMLUsetASCII(mlu3, "en", "US", dmdd);
  cmsWriteTag(profile, cmsSigDeviceModelDescTag, mlu3);

  cmsMLUsetASCII(mlu4, "en", "US", "darktable");
  cmsWriteTag(profile, cmsSigDeviceMfgDescTag, mlu4);

  cmsMLUfree(mlu1);
  cmsMLUfree(mlu2);
  cmsMLUfree(mlu3);
  cmsMLUfree(mlu4);

  return profile;
}

// https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.2100-2-201807-I!!PDF-F.pdf
// Perceptual Quantization / SMPTE standard ST.2084
static double _PQ_fct(double x)
{
  static const double M1 = 2610.0 / 16384.0;
  static const double M2 = (2523.0 / 4096.0) * 128.0;
  static const double C1 = 3424.0 / 4096.0;
  static const double C2 = (2413.0 / 4096.0) * 32.0;
  static const double C3 = (2392.0 / 4096.0) * 32.0;

  if(x == 0.0) return 0.0;
  const double sign = x;
  x = fabs(x);

  const double xpo = pow(x, 1.0 / M2);
  const double num = MAX(xpo - C1, 0.0);
  const double den = C2 - C3 * xpo;
  const double res = pow(num / den, 1.0 / M1);

  return copysign(res, sign);
}

// https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.2100-2-201807-I!!PDF-F.pdf
// Hybrid Log-Gamma
static double _HLG_fct(double x)
{
  static const double Beta  = 0.04;
  static const double RA    = 5.591816309728916; // 1.0 / A where A = 0.17883277
  static const double B     = 0.28466892; // 1.0 - 4.0 * A
  static const double C     = 0.5599107295; // 0,5 –aln(4a)

  double e = MAX(x * (1.0 - Beta) + Beta, 0.0);

  if(e == 0.0) return 0.0;

  const double sign = e;
  e = fabs(e);

  double res = 0.0;

  if(e <= 0.5)
  {
    res = e * e / 3.0;
  }
  else
  {
    res = (exp((e - C) * RA) + B) / 12.0;
  }

  return copysign(res, sign);
}

static cmsToneCurve* _colorspaces_create_transfer(int32_t size, double (*fct)(double))
{
  float *values = g_malloc(sizeof(float) * size);

  for(int32_t i = 0; i < size; ++i)
  {
    const double x = (float)i / (size - 1);
    const double y = MIN(fct(x), 1.0f);
    values[i] = (float)y;
  }

  cmsToneCurve* result = cmsBuildTabulatedToneCurveFloat(NULL, size, values);
  g_free(values);
  return result;
}

static cmsHPROFILE _colorspaces_create_srgb_profile(gboolean v2)
{
  cmsFloat64Number srgb_parameters[5] = { 2.4, 1.0 / 1.055,  0.055 / 1.055, 1.0 / 12.92, 0.04045 };
  cmsToneCurve *transferFunction = cmsBuildParametricToneCurve(NULL, 4, srgb_parameters);

  cmsHPROFILE profile = _create_lcms_profile("sRGB", "sRGB",
                                             &D65xyY, &sRGB_Primaries, transferFunction, v2);

  cmsFreeToneCurve(transferFunction);

  return profile;
}

static cmsHPROFILE dt_colorspaces_create_srgb_profile()
{
  return _colorspaces_create_srgb_profile(TRUE);
}

static cmsHPROFILE dt_colorspaces_create_srgb_profile_v4()
{
  return _colorspaces_create_srgb_profile(FALSE);
}

static cmsHPROFILE dt_colorspaces_create_brg_profile()
{
  cmsFloat64Number srgb_parameters[5] = { 2.4, 1.0 / 1.055,  0.055 / 1.055, 1.0 / 12.92, 0.04045 };
  cmsToneCurve *transferFunction = cmsBuildParametricToneCurve(NULL, 4, srgb_parameters);

  cmsCIExyYTRIPLE BRG_Primaries = { sRGB_Primaries.Blue, sRGB_Primaries.Red, sRGB_Primaries.Green };

  cmsHPROFILE profile = _create_lcms_profile("BRG", "BRG",
                                             &D65xyY, &BRG_Primaries, transferFunction, TRUE);

  cmsFreeToneCurve(transferFunction);

  return profile;
}

static cmsHPROFILE dt_colorspaces_create_gamma_rec709_rgb_profile(void)
{
  cmsFloat64Number srgb_parameters[5] = { 1/0.45, 1.0 / 1.099,  0.099 / 1.099, 1.0 / 4.5, 0.081 };
  cmsToneCurve *transferFunction = cmsBuildParametricToneCurve(NULL, 4, srgb_parameters);

  cmsHPROFILE profile = _create_lcms_profile("Gamma Rec709 RGB", "Gamma Rec709 RGB",
                                             &D65xyY, &Rec709_Primaries, transferFunction, TRUE);

  cmsFreeToneCurve(transferFunction);

  return profile;
}

// Create the ICC virtual profile for adobe rgb space
static cmsHPROFILE dt_colorspaces_create_adobergb_profile(void)
{
  // AdobeRGB's "2.2" gamma is technically defined as 2 + 51/256
  cmsToneCurve *transferFunction = cmsBuildGamma(NULL, 2.19921875);

  cmsHPROFILE profile = _create_lcms_profile("Adobe RGB (compatible)", "Adobe RGB",
                                             &D65xyY, &Adobe_Primaries, transferFunction, TRUE);

  cmsFreeToneCurve(transferFunction);

  return profile;
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

  Gamma[0] = Gamma[1] = Gamma[2] = cmsBuildGamma(NULL, 1.0);

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

  Gamma[0] = Gamma[1] = Gamma[2] = cmsBuildGamma(NULL, 1.0);

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

  Gamma[0] = Gamma[1] = Gamma[2] = cmsBuildGamma(NULL, 1.0);

  hp = cmsCreateRGBProfile(&WP, &XYZPrimaries, Gamma);
  cmsFreeToneCurve(Gamma[0]);
  if(hp == NULL) return NULL;

  char name[512];
  snprintf(name, sizeof(name), "darktable profiled %s", makermodel);
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

static cmsHPROFILE dt_colorspaces_create_xyz_profile(void)
{
  cmsHPROFILE hXYZ = cmsCreateXYZProfile();
  cmsSetPCS(hXYZ, cmsSigXYZData);
  cmsSetHeaderRenderingIntent(hXYZ, INTENT_PERCEPTUAL);

  if(hXYZ == NULL) return NULL;

  cmsSetProfileVersion(hXYZ, 2.1);
  cmsMLU *mlu0 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu0, "en", "US", "(dt internal)");
  cmsMLU *mlu1 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu1, "en", "US", "linear XYZ");
  cmsMLU *mlu2 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu2, "en", "US", "darktable linear XYZ");
  cmsWriteTag(hXYZ, cmsSigDeviceMfgDescTag, mlu0);
  cmsWriteTag(hXYZ, cmsSigDeviceModelDescTag, mlu1);
  // this will only be displayed when the embedded profile is read by for example GIMP
  cmsWriteTag(hXYZ, cmsSigProfileDescriptionTag, mlu2);
  cmsMLUfree(mlu0);
  cmsMLUfree(mlu1);
  cmsMLUfree(mlu2);

  return hXYZ;
}

static cmsHPROFILE dt_colorspaces_create_linear_rec709_rgb_profile(void)
{
  cmsToneCurve *transferFunction = cmsBuildGamma(NULL, 1.0);

  cmsHPROFILE profile = _create_lcms_profile("Linear Rec709 RGB", "Linear Rec709 RGB",
                                             &D65xyY, &Rec709_Primaries, transferFunction, TRUE);

  cmsFreeToneCurve(transferFunction);

  return profile;
}

static cmsHPROFILE dt_colorspaces_create_linear_rec2020_rgb_profile(void)
{
  cmsToneCurve *transferFunction = cmsBuildGamma(NULL, 1.0);

  cmsHPROFILE profile = _create_lcms_profile("Linear Rec2020 RGB", "Linear Rec2020 RGB",
                                             &D65xyY, &Rec2020_Primaries, transferFunction, TRUE);

  cmsFreeToneCurve(transferFunction);

  return profile;
}

static cmsHPROFILE dt_colorspaces_create_pq_rec2020_rgb_profile(void)
{
  cmsToneCurve *transferFunction = _colorspaces_create_transfer(4096, _PQ_fct);

  cmsHPROFILE profile = _create_lcms_profile("PQ Rec2020 RGB", "PQ Rec2020 RGB",
                                             &D65xyY, &Rec2020_Primaries, transferFunction, TRUE);

  cmsFreeToneCurve(transferFunction);

  return profile;
}

static cmsHPROFILE dt_colorspaces_create_hlg_rec2020_rgb_profile(void)
{
  cmsToneCurve *transferFunction = _colorspaces_create_transfer(4096, _HLG_fct);

  cmsHPROFILE profile = _create_lcms_profile("HLG Rec2020 RGB", "HLG Rec2020 RGB",
                                             &D65xyY, &Rec2020_Primaries, transferFunction, TRUE);

  cmsFreeToneCurve(transferFunction);

  return profile;
}

static cmsHPROFILE dt_colorspaces_create_pq_p3_rgb_profile(void)
{
  cmsToneCurve *transferFunction = _colorspaces_create_transfer(4096, _PQ_fct);

  cmsHPROFILE profile = _create_lcms_profile("PQ P3 RGB", "PQ P3 RGB",
                                             &D65xyY, &P3_Primaries, transferFunction, TRUE);

  cmsFreeToneCurve(transferFunction);

  return profile;
}

static cmsHPROFILE dt_colorspaces_create_hlg_p3_rgb_profile(void)
{
  cmsToneCurve *transferFunction = _colorspaces_create_transfer(4096, _HLG_fct);

  cmsHPROFILE profile = _create_lcms_profile("HLG P3 RGB", "HLG P3 RGB",
                                             &D65xyY, &P3_Primaries, transferFunction, TRUE);

  cmsFreeToneCurve(transferFunction);

  return profile;
}

static cmsHPROFILE dt_colorspaces_create_linear_prophoto_rgb_profile(void)
{
  cmsToneCurve *transferFunction = cmsBuildGamma(NULL, 1.0);

  cmsHPROFILE profile = _create_lcms_profile("Linear ProPhoto RGB", "Linear ProPhoto RGB",
                                             &D50xyY,  &ProPhoto_Primaries, transferFunction, TRUE);

  cmsFreeToneCurve(transferFunction);

  return profile;
}

static cmsHPROFILE dt_colorspaces_create_linear_infrared_profile(void)
{
  cmsToneCurve *transferFunction = cmsBuildGamma(NULL, 1.0);

  // linear rgb with r and b swapped:
  cmsCIExyYTRIPLE BGR_Primaries = { sRGB_Primaries.Blue, sRGB_Primaries.Green, sRGB_Primaries.Red };

  cmsHPROFILE profile = _create_lcms_profile("Linear Infrared BGR", "darktable Linear Infrared BGR",
                                             &D65xyY, &BGR_Primaries, transferFunction, FALSE);

  cmsFreeToneCurve(transferFunction);

  return profile;
}

const dt_colorspaces_color_profile_t *dt_colorspaces_get_work_profile(const int imgid)
{
  // find the colorin module -- the pointer stays valid until darktable shuts down
  static const dt_iop_module_so_t *colorin = NULL;
  if(colorin == NULL)
  {
    for(const GList *modules = darktable.iop; modules; modules = g_list_next(modules))
    {
      const dt_iop_module_so_t *module = (const dt_iop_module_so_t *)(modules->data);
      if(dt_iop_module_is(module, "colorin"))
      {
        colorin = module;
        break;
      }
    }
  }

  const dt_colorspaces_color_profile_t *p = NULL;

  if(colorin && colorin->get_p)
  {
    // get the profile assigned from colorin
    // FIXME: does this work when using JPEG thumbs and the image was never opened?
    sqlite3_stmt *stmt;
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT op_params FROM main.history WHERE imgid=?1 AND operation='colorin' ORDER BY num DESC LIMIT 1", -1,
      &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      // use introspection to get the profile name from the binary params blob
      const void *params = sqlite3_column_blob(stmt, 0);
      dt_colorspaces_color_profile_type_t *type = colorin->get_p(params, "type_work");
      char *filename = colorin->get_p(params, "filename_work");

      if(type && filename) p = dt_colorspaces_get_profile(*type, filename,
                                                          DT_PROFILE_DIRECTION_WORK);
    }
    sqlite3_finalize(stmt);
  }

  // if all else fails -> fall back to linear Rec2020 RGB
  if(!p) p = dt_colorspaces_get_profile(DT_COLORSPACE_LIN_REC2020, "", DT_PROFILE_DIRECTION_WORK);

  return p;
}

const dt_colorspaces_color_profile_t *dt_colorspaces_get_output_profile(const int imgid,
                                                                        dt_colorspaces_color_profile_type_t over_type,
                                                                        const char *over_filename)
{
  // find the colorout module -- the pointer stays valid until darktable shuts down
  static const dt_iop_module_so_t *colorout = NULL;
  if(colorout == NULL)
  {
    for(const GList *modules = darktable.iop; modules; modules = g_list_next(modules))
    {
      const dt_iop_module_so_t *module = (const dt_iop_module_so_t *)(modules->data);
      if(dt_iop_module_is(module, "colorout"))
      {
        colorout = module;
        break;
      }
    }
  }

  const dt_colorspaces_color_profile_t *p = NULL;

  if(over_type != DT_COLORSPACE_NONE)
  {
    // return the profile specified in export.
    // we have that in here to get rid of the if() check in all places calling this function.
    p = dt_colorspaces_get_profile(over_type, over_filename, DT_PROFILE_DIRECTION_OUT | DT_PROFILE_DIRECTION_DISPLAY);
  }
  else if(colorout && colorout->get_p)
  {
    // get the profile assigned from colorout
    // FIXME: does this work when using JPEG thumbs and the image was never opened?
    sqlite3_stmt *stmt;
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT op_params FROM main.history WHERE imgid=?1 AND operation='colorout' ORDER BY num DESC LIMIT 1", -1,
      &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      // use introspection to get the profile name from the binary params blob
      const void *params = sqlite3_column_blob(stmt, 0);
      dt_colorspaces_color_profile_type_t *type = colorout->get_p(params, "type");
      char *filename = colorout->get_p(params, "filename");

      if(type && filename) p = dt_colorspaces_get_profile(*type, filename,
                                                          DT_PROFILE_DIRECTION_OUT | DT_PROFILE_DIRECTION_DISPLAY);
    }
    sqlite3_finalize(stmt);
  }

  // if all else fails -> fall back to sRGB
  if(!p) p = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "", DT_PROFILE_DIRECTION_OUT);

  return p;
}

#if 0
static void dt_colorspaces_create_cmatrix(float cmatrix[4][3], float mat[3][3])
{
  // sRGB D65, the linear part:
  static const dt_colormatrix_t rgb_to_xyz = { { 0.4124564f, 0.3575761f, 0.1804375f, 0.0f },
                                        { 0.2126729f, 0.7151522f, 0.0721750f, 0.0f },
                                        { 0.0193339f, 0.1191920f, 0.9503041f, 0.0f } };

  for(int c = 0; c < 3; c++)
  {
    for(int j = 0; j < 3; j++)
    {
      mat[c][j] = 0.0f;
      for(int k = 0; k < 3; k++)
      {
        mat[c][j] += rgb_to_xyz[k][j] * cmatrix[c][k];
      }
    }
  }
}
#endif

static cmsHPROFILE dt_colorspaces_create_xyzmatrix_profile(const float mat[3][3])
{
  // mat: cam -> xyz
  dt_aligned_pixel_t x, y;
  for(int k = 0; k < 3; k++)
  {
    const float norm = mat[0][k] + mat[1][k] + mat[2][k];
    x[k] = mat[0][k] / norm;
    y[k] = mat[1][k] / norm;
  }
  cmsCIExyYTRIPLE CameraPrimaries = { { x[0], y[0], 1.0 }, { x[1], y[1], 1.0 }, { x[2], y[2], 1.0 } };
  cmsHPROFILE profile;

  cmsCIExyY D65;
  cmsXYZ2xyY(&D65, &d65);

  cmsToneCurve *Gamma[3];
  Gamma[0] = Gamma[1] = Gamma[2] = cmsBuildGamma(NULL, 1.0);
  profile = cmsCreateRGBProfile(&D65, &CameraPrimaries, Gamma);
  cmsFreeToneCurve(Gamma[0]);
  if(profile == NULL) return NULL;

  cmsSetProfileVersion(profile, 2.1);
  cmsMLU *mlu0 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu0, "en", "US", "(dt internal)");
  cmsMLU *mlu1 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu1, "en", "US", "color matrix built-in");
  cmsMLU *mlu2 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu2, "en", "US", "color matrix built-in");
  cmsWriteTag(profile, cmsSigDeviceMfgDescTag, mlu0);
  cmsWriteTag(profile, cmsSigDeviceModelDescTag, mlu1);
  // this will only be displayed when the embedded profile is read by for example GIMP
  cmsWriteTag(profile, cmsSigProfileDescriptionTag, mlu2);
  cmsMLUfree(mlu0);
  cmsMLUfree(mlu1);
  cmsMLUfree(mlu2);

  return profile;
}

cmsHPROFILE dt_colorspaces_create_xyzimatrix_profile(float mat[3][3])
{
  // mat: xyz -> cam
  float imat[3][3];
  mat3inv((float *)imat, (float *)mat);
  return dt_colorspaces_create_xyzmatrix_profile(imat);
}

static cmsHPROFILE _ensure_rgb_profile(cmsHPROFILE profile)
{
  if(profile && cmsGetColorSpace(profile) == cmsSigGrayData)
  {
    cmsToneCurve *trc = cmsReadTag(profile, cmsSigGrayTRCTag);
    cmsCIEXYZ *wtpt = cmsReadTag(profile, cmsSigMediaWhitePointTag);
    cmsCIEXYZ *bkpt = cmsReadTag(profile, cmsSigMediaBlackPointTag);
    cmsCIEXYZ *chad = cmsReadTag(profile, cmsSigChromaticAdaptationTag);

    cmsMLU *cprt = cmsReadTag(profile, cmsSigCopyrightTag);
    cmsMLU *desc = cmsReadTag(profile, cmsSigProfileDescriptionTag);
    cmsMLU *dmnd = cmsReadTag(profile, cmsSigDeviceMfgDescTag);
    cmsMLU *dmdd = cmsReadTag(profile, cmsSigDeviceModelDescTag);

    cmsHPROFILE rgb_profile = cmsCreateProfilePlaceholder(0);

    cmsSetDeviceClass(rgb_profile, cmsSigDisplayClass);
    cmsSetColorSpace(rgb_profile, cmsSigRgbData);
    cmsSetPCS(rgb_profile, cmsSigXYZData);

    cmsWriteTag(rgb_profile, cmsSigCopyrightTag, cprt);
    cmsWriteTag(rgb_profile, cmsSigProfileDescriptionTag, desc);
    cmsWriteTag(rgb_profile, cmsSigDeviceMfgDescTag, dmnd);
    cmsWriteTag(rgb_profile, cmsSigDeviceModelDescTag, dmdd);

    cmsWriteTag(rgb_profile, cmsSigMediaBlackPointTag, bkpt);
    cmsWriteTag(rgb_profile, cmsSigMediaWhitePointTag, wtpt);
    cmsWriteTag(rgb_profile, cmsSigChromaticAdaptationTag, chad);
    cmsSetColorSpace(rgb_profile, cmsSigRgbData);
    cmsSetPCS(rgb_profile, cmsSigXYZData);

    // TODO: we still use prequantized primaries here, we will probably want to rework this
    // part to create a profile using cmsCreateRGBProfile() as done in _create_lcms_profile().
    cmsWriteTag(rgb_profile, cmsSigRedColorantTag, (void *)&Rec709_Primaries_Prequantized.Red);
    cmsWriteTag(rgb_profile, cmsSigGreenColorantTag, (void *)&Rec709_Primaries_Prequantized.Green);
    cmsWriteTag(rgb_profile, cmsSigBlueColorantTag, (void *)&Rec709_Primaries_Prequantized.Blue);

    cmsWriteTag(rgb_profile, cmsSigRedTRCTag, (void *)trc);
    cmsLinkTag(rgb_profile, cmsSigGreenTRCTag, cmsSigRedTRCTag);
    cmsLinkTag(rgb_profile, cmsSigBlueTRCTag, cmsSigRedTRCTag);

    cmsCloseProfile(profile);
    profile = rgb_profile;
  }

  return profile;
}

cmsHPROFILE dt_colorspaces_get_rgb_profile_from_mem(uint8_t *data, uint32_t size)
{
  cmsHPROFILE profile = _ensure_rgb_profile(cmsOpenProfileFromMem(data, size));

  return profile;
}

void dt_colorspaces_cleanup_profile(cmsHPROFILE p)
{
  if(!p) return;
  cmsCloseProfile(p);
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

static dt_colorspaces_color_profile_t *_create_profile(dt_colorspaces_color_profile_type_t type,
                                                       cmsHPROFILE profile, const char *name, int in_pos,
                                                       int out_pos, int display_pos, int category_pos,
                                                       int work_pos, int display2_pos)
{
  dt_colorspaces_color_profile_t *prof;
  prof = (dt_colorspaces_color_profile_t *)calloc(1, sizeof(dt_colorspaces_color_profile_t));
  prof->type = type;
  g_strlcpy(prof->name, name, sizeof(prof->name));
  prof->profile = profile;
  prof->in_pos = in_pos;
  prof->out_pos = out_pos;
  prof->display_pos = display_pos;
  prof->category_pos = category_pos;
  prof->work_pos = work_pos;
  prof->display2_pos = display2_pos;
  return prof;
}

// this function is basically thread safe, at least when not called on the global darktable.color_profiles
static void _update_display_transforms(dt_colorspaces_t *self)
{
  if(self->transform_srgb_to_display) cmsDeleteTransform(self->transform_srgb_to_display);
  self->transform_srgb_to_display = NULL;

  if(self->transform_adobe_rgb_to_display) cmsDeleteTransform(self->transform_adobe_rgb_to_display);
  self->transform_adobe_rgb_to_display = NULL;

  const dt_colorspaces_color_profile_t *display_dt_profile = _get_profile(self, self->display_type,
                                                                          self->display_filename,
                                                                          DT_PROFILE_DIRECTION_DISPLAY);
  if(!display_dt_profile) return;
  cmsHPROFILE display_profile = display_dt_profile->profile;
  if(!display_profile) return;

  self->transform_srgb_to_display = cmsCreateTransform(_get_profile(self, DT_COLORSPACE_SRGB, "",
                                                                    DT_PROFILE_DIRECTION_DISPLAY)->profile,
                                                       TYPE_RGBA_8,
                                                       display_profile,
                                                       TYPE_BGRA_8,
                                                       self->display_intent,
                                                       0);

  self->transform_adobe_rgb_to_display = cmsCreateTransform(_get_profile(self, DT_COLORSPACE_ADOBERGB, "",
                                                                         DT_PROFILE_DIRECTION_DISPLAY)->profile,
                                                            TYPE_RGBA_8,
                                                            display_profile,
                                                            TYPE_BGRA_8,
                                                            self->display_intent,
                                                            0);
}

static void _update_display2_transforms(dt_colorspaces_t *self)
{
  if(self->transform_srgb_to_display2) cmsDeleteTransform(self->transform_srgb_to_display2);
  self->transform_srgb_to_display2 = NULL;

  if(self->transform_adobe_rgb_to_display2) cmsDeleteTransform(self->transform_adobe_rgb_to_display2);
  self->transform_adobe_rgb_to_display2 = NULL;

  const dt_colorspaces_color_profile_t *display2_dt_profile
      = _get_profile(self, self->display2_type, self->display2_filename, DT_PROFILE_DIRECTION_DISPLAY2);
  if(!display2_dt_profile) return;
  cmsHPROFILE display2_profile = display2_dt_profile->profile;
  if(!display2_profile) return;

  self->transform_srgb_to_display2
      = cmsCreateTransform(_get_profile(self, DT_COLORSPACE_SRGB, "", DT_PROFILE_DIRECTION_DISPLAY2)->profile,
                           TYPE_RGBA_8, display2_profile, TYPE_BGRA_8, self->display2_intent, 0);

  self->transform_adobe_rgb_to_display2
      = cmsCreateTransform(_get_profile(self, DT_COLORSPACE_ADOBERGB, "", DT_PROFILE_DIRECTION_DISPLAY2)->profile,
                           TYPE_RGBA_8, display2_profile, TYPE_BGRA_8, self->display2_intent, 0);
}

// update cached transforms for color management of thumbnails
// make sure that darktable.color_profiles->xprofile_lock is held when calling this!
void dt_colorspaces_update_display_transforms()
{
  _update_display_transforms(darktable.color_profiles);
}

void dt_colorspaces_update_display2_transforms()
{
  _update_display2_transforms(darktable.color_profiles);
}

// make sure that darktable.color_profiles->xprofile_lock is held when calling this!
static void _update_display_profile(guchar *tmp_data, gsize size, char *name, size_t name_size)
{
  g_free(darktable.color_profiles->xprofile_data);
  darktable.color_profiles->xprofile_data = tmp_data;
  darktable.color_profiles->xprofile_size = size;

  cmsHPROFILE profile = cmsOpenProfileFromMem(tmp_data, size);
  if(profile)
  {
    for(GList *iter = darktable.color_profiles->profiles; iter; iter = g_list_next(iter))
    {
      dt_colorspaces_color_profile_t *p = (dt_colorspaces_color_profile_t *)iter->data;
      if(p->type == DT_COLORSPACE_DISPLAY)
      {
        if(p->profile) dt_colorspaces_cleanup_profile(p->profile);
        p->profile = profile;
        if(name)
          dt_colorspaces_get_profile_name(profile, "en", "US", name, name_size);

        // update cached transforms for color management of thumbnails
        dt_colorspaces_update_display_transforms();

        break;
      }
    }
  }
}

static void _update_display2_profile(guchar *tmp_data, gsize size, char *name, size_t name_size)
{
  g_free(darktable.color_profiles->xprofile_data2);
  darktable.color_profiles->xprofile_data2 = tmp_data;
  darktable.color_profiles->xprofile_size2 = size;

  cmsHPROFILE profile = cmsOpenProfileFromMem(tmp_data, size);
  if(profile)
  {
    for(GList *iter = darktable.color_profiles->profiles; iter; iter = g_list_next(iter))
    {
      dt_colorspaces_color_profile_t *p = (dt_colorspaces_color_profile_t *)iter->data;
      if(p->type == DT_COLORSPACE_DISPLAY2)
      {
        if(p->profile) dt_colorspaces_cleanup_profile(p->profile);
        p->profile = profile;
        if(name) dt_colorspaces_get_profile_name(profile, "en", "US", name, name_size);

        // update cached transforms for color management of thumbnails
        dt_colorspaces_update_display2_transforms();

        break;
      }
    }
  }
}

static void cms_error_handler(cmsContext ContextID, cmsUInt32Number ErrorCode, const char *text)
{
  dt_print(DT_DEBUG_ALWAYS, "[lcms2] error %d: %s\n", ErrorCode, text);
}

static gint _sort_profiles(gconstpointer a, gconstpointer b)
{
  const dt_colorspaces_color_profile_t *profile_a = (dt_colorspaces_color_profile_t *)a;
  const dt_colorspaces_color_profile_t *profile_b = (dt_colorspaces_color_profile_t *)b;

  gchar *name_a = g_utf8_casefold(profile_a->name, -1);
  gchar *name_b = g_utf8_casefold(profile_b->name, -1);

  gint result = g_strcmp0(name_a, name_b);

  g_free(name_a);
  g_free(name_b);

  return result;
}

static GList *load_profile_from_dir(const char *subdir)
{
  GList *temp_profiles = NULL;
  const gchar *d_name;
  char datadir[PATH_MAX] = { 0 };
  char confdir[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(confdir, sizeof(confdir));
  dt_loc_get_datadir(datadir, sizeof(datadir));
  char *lang = getenv("LANG");
  if(!lang) lang = "en_US";

  char *dirname = g_build_filename(confdir, "color", subdir, NULL);
  if(!g_file_test(dirname, G_FILE_TEST_IS_DIR))
  {
    g_free(dirname);
    dirname = g_build_filename(datadir, "color", subdir, NULL);
  }
  GDir *dir = g_dir_open(dirname, 0, NULL);
  if(dir)
  {
    while((d_name = g_dir_read_name(dir)))
    {
      char *filename = g_build_filename(dirname, d_name, NULL);
      const char *cc = filename + strlen(filename);
      for(; *cc != '.' && cc > filename; cc--)
        ;
      if(!g_ascii_strcasecmp(cc, ".icc") || !g_ascii_strcasecmp(cc, ".icm"))
      {
        size_t end;
        char *icc_content = dt_read_file(filename, &end);
        if(!icc_content) goto icc_loading_done;

        // TODO: add support for grayscale profiles, then remove _ensure_rgb_profile() from here
        cmsHPROFILE tmpprof = _ensure_rgb_profile(cmsOpenProfileFromMem(icc_content, sizeof(char) * end));
        if(tmpprof)
        {
          dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)calloc(1, sizeof(dt_colorspaces_color_profile_t));
          dt_colorspaces_get_profile_name(tmpprof, lang, lang + 3, prof->name, sizeof(prof->name));

          g_strlcpy(prof->filename, filename, sizeof(prof->filename));
          prof->type = DT_COLORSPACE_FILE;
          prof->profile = tmpprof;
          // these will be set after sorting!
          prof->in_pos = -1;
          prof->out_pos = -1;
          prof->display_pos = -1;
          prof->display2_pos = -1;
          prof->category_pos = -1;
          prof->work_pos = -1;
          temp_profiles = g_list_prepend(temp_profiles, prof);
        }

icc_loading_done:
        if(icc_content) free(icc_content);
      }
      g_free(filename);
    }
    g_dir_close(dir);
    temp_profiles = g_list_sort(temp_profiles, _sort_profiles);
  }
  g_free(dirname);
  return temp_profiles;
}

dt_colorspaces_t *dt_colorspaces_init()
{
  cmsSetLogErrorHandler(cms_error_handler);

  dt_colorspaces_t *res = (dt_colorspaces_t *)calloc(1, sizeof(dt_colorspaces_t));

  _compute_prequantized_primaries(&D65xyY, &Rec709_Primaries, &Rec709_Primaries_Prequantized);

  pthread_rwlock_init(&res->xprofile_lock, NULL);

  int in_pos = -1,
      out_pos = -1,
      display_pos = -1,
      display2_pos = -1,
      category_pos = -1,
      work_pos = -1;

  // init the category profile with NULL profile, the actual profile must be retrieved dynamically by the caller
  res->profiles = g_list_append(res->profiles, _create_profile(DT_COLORSPACE_WORK, NULL, _("work profile"), -1, -1,
                                                               -1, ++category_pos, -1, -1));

  res->profiles = g_list_append(res->profiles, _create_profile(DT_COLORSPACE_EXPORT, NULL, _("export profile"), -1,
                                                               -1, -1, ++category_pos, -1, -1));

  res->profiles
      = g_list_append(res->profiles, _create_profile(DT_COLORSPACE_SOFTPROOF, NULL, _("softproof profile"), -1, -1,
                                                     -1, ++category_pos, -1, -1));

  // init the display profile with srgb so some stupid code that runs before the real profile could be fetched has something to work with
  res->profiles = g_list_append(
      res->profiles, _create_profile(DT_COLORSPACE_DISPLAY, dt_colorspaces_create_srgb_profile(),
                                     _("system display profile"), -1, -1, ++display_pos, ++category_pos, -1, -1));
  res->profiles = g_list_append(
      res->profiles, _create_profile(DT_COLORSPACE_DISPLAY2, dt_colorspaces_create_srgb_profile(),
                                     _("system display profile (second window)"), -1, -1, -1, ++category_pos, -1, ++display2_pos));
  // we want a v4 with parametric curve for input and a v2 with point trc for output
  // see http://ninedegreesbelow.com/photography/lcms-make-icc-profiles.html#profile-variants-and-versions
  // TODO: what about display?
  res->profiles
      = g_list_append(res->profiles, _create_profile(DT_COLORSPACE_SRGB, dt_colorspaces_create_srgb_profile_v4(),
                                                     _("sRGB"), ++in_pos, -1, -1, -1, -1, -1));

  res->profiles
      = g_list_append(res->profiles, _create_profile(DT_COLORSPACE_SRGB, dt_colorspaces_create_srgb_profile(),
                                                     _("sRGB (web-safe)"), -1, ++out_pos, ++display_pos,
                                                     ++category_pos, ++work_pos, ++display2_pos));

  res->profiles = g_list_append(res->profiles,
                                _create_profile(DT_COLORSPACE_ADOBERGB, dt_colorspaces_create_adobergb_profile(),
                                                _("Adobe RGB (compatible)"), ++in_pos, ++out_pos, ++display_pos,
                                                ++category_pos, ++work_pos, ++display2_pos));

  res->profiles = g_list_append(
      res->profiles, _create_profile(DT_COLORSPACE_LIN_REC709, dt_colorspaces_create_linear_rec709_rgb_profile(),
                                     _("linear Rec709 RGB"), ++in_pos, ++out_pos, ++display_pos, ++category_pos,
                                     ++work_pos, ++display2_pos));

  res->profiles = g_list_append(res->profiles, _create_profile(DT_COLORSPACE_REC709, dt_colorspaces_create_gamma_rec709_rgb_profile(),
                                     _("Rec709 RGB"), ++in_pos, ++out_pos, -1, -1,
                                     ++work_pos, -1));

  res->profiles = g_list_append(
      res->profiles, _create_profile(DT_COLORSPACE_LIN_REC2020, dt_colorspaces_create_linear_rec2020_rgb_profile(),
                                     _("linear Rec2020 RGB"), ++in_pos, ++out_pos, ++display_pos, ++category_pos,
                                     ++work_pos, ++display2_pos));

  res->profiles = g_list_append(
      res->profiles, _create_profile(DT_COLORSPACE_PQ_REC2020, dt_colorspaces_create_pq_rec2020_rgb_profile(),
                                     _("PQ Rec2020 RGB"), ++in_pos, ++out_pos, ++display_pos, ++category_pos,
                                     ++work_pos, ++display2_pos));

  res->profiles = g_list_append(
      res->profiles, _create_profile(DT_COLORSPACE_HLG_REC2020, dt_colorspaces_create_hlg_rec2020_rgb_profile(),
                                     _("HLG Rec2020 RGB"), ++in_pos, ++out_pos, ++display_pos, ++category_pos,
                                     ++work_pos, ++display2_pos));

  res->profiles = g_list_append(
      res->profiles, _create_profile(DT_COLORSPACE_PQ_P3, dt_colorspaces_create_pq_p3_rgb_profile(),
                                     _("PQ P3 RGB"), ++in_pos, ++out_pos, ++display_pos, ++category_pos,
                                     ++work_pos, ++display2_pos));

  res->profiles = g_list_append(
      res->profiles, _create_profile(DT_COLORSPACE_HLG_P3, dt_colorspaces_create_hlg_p3_rgb_profile(),
                                     _("HLG P3 RGB"), ++in_pos, ++out_pos, ++display_pos, ++category_pos,
                                     ++work_pos, ++display2_pos));

  res->profiles = g_list_append(
     res->profiles, _create_profile(DT_COLORSPACE_PROPHOTO_RGB, dt_colorspaces_create_linear_prophoto_rgb_profile(),
                                    _("linear ProPhoto RGB"), ++in_pos, ++out_pos, ++display_pos, ++category_pos,
                                    ++work_pos, ++display2_pos));

  res->profiles = g_list_append(
      res->profiles,
      _create_profile(DT_COLORSPACE_XYZ, dt_colorspaces_create_xyz_profile(), _("linear XYZ"), ++in_pos,
                      dt_conf_get_bool("allow_lab_output") ? ++out_pos : -1, -1, -1, -1, -1));

  res->profiles = g_list_append(
      res->profiles, _create_profile(DT_COLORSPACE_LAB, dt_colorspaces_create_lab_profile(), _("Lab"), ++in_pos,
                                     dt_conf_get_bool("allow_lab_output") ? ++out_pos : -1, -1, -1, -1, -1));

  res->profiles = g_list_append(
      res->profiles, _create_profile(DT_COLORSPACE_INFRARED, dt_colorspaces_create_linear_infrared_profile(),
                                     _("linear infrared BGR"), ++in_pos, -1, -1, -1, -1, -1));

  res->profiles
      = g_list_append(res->profiles, _create_profile(DT_COLORSPACE_BRG, dt_colorspaces_create_brg_profile(),
                                                     _("BRG (for testing)"), ++in_pos, ++out_pos, ++display_pos,
                                                     -1, -1, ++display2_pos));

  // init display profile and softproof/gama checking from conf
  res->display_type = dt_conf_get_int("ui_last/color/display_type");
  res->display2_type = dt_conf_get_int("ui_last/color/display2_type");
  res->softproof_type = dt_conf_get_int("ui_last/color/softproof_type");
  res->histogram_type = dt_conf_get_int("ui_last/color/histogram_type");
  const char *tmp = dt_conf_get_string_const("ui_last/color/display_filename");
  g_strlcpy(res->display_filename, tmp, sizeof(res->display_filename));
  tmp = dt_conf_get_string_const("ui_last/color/display2_filename");
  g_strlcpy(res->display2_filename, tmp, sizeof(res->display2_filename));
  tmp = dt_conf_get_string_const("ui_last/color/softproof_filename");
  g_strlcpy(res->softproof_filename, tmp, sizeof(res->softproof_filename));
  tmp = dt_conf_get_string_const("ui_last/color/histogram_filename");
  g_strlcpy(res->histogram_filename, tmp, sizeof(res->histogram_filename));
  res->display_intent = dt_conf_get_int("ui_last/color/display_intent");
  res->display2_intent = dt_conf_get_int("ui_last/color/display2_intent");
  res->softproof_intent = dt_conf_get_int("ui_last/color/softproof_intent");
  res->mode = dt_conf_get_int("ui_last/color/mode");

  // sanity checks to ensure the profile filenames are present

  if((unsigned int)res->display_type >= DT_COLORSPACE_LAST
     || (res->display_type == DT_COLORSPACE_FILE
         && (!res->display_filename[0] || !g_file_test(res->display_filename, G_FILE_TEST_IS_REGULAR))))
    res->display_type = DT_COLORSPACE_DISPLAY;

  if((unsigned int)res->display2_type >= DT_COLORSPACE_LAST
     || (res->display2_type == DT_COLORSPACE_FILE
         && (!res->display2_filename[0] || !g_file_test(res->display2_filename, G_FILE_TEST_IS_REGULAR))))
    res->display2_type = DT_COLORSPACE_DISPLAY2;

  if((unsigned int)res->softproof_type >= DT_COLORSPACE_LAST
     || (res->softproof_type == DT_COLORSPACE_FILE
         && (!res->softproof_filename[0] || !g_file_test(res->softproof_filename, G_FILE_TEST_IS_REGULAR))))
    res->softproof_type = DT_COLORSPACE_SRGB;

  if((unsigned int)res->histogram_type >= DT_COLORSPACE_LAST
     || (res->histogram_type == DT_COLORSPACE_FILE
         && (!res->histogram_filename[0] || !g_file_test(res->histogram_filename, G_FILE_TEST_IS_REGULAR))))
    res->histogram_type = DT_COLORSPACE_SRGB;

  // temporary list of profiles to be added, we keep this separate to be able to sort it before adding
  GList *temp_profiles;

  // read {userconfig,datadir}/color/in/*.icc, in this order.
  temp_profiles = load_profile_from_dir("in");
  for(GList *iter = temp_profiles; iter; iter = g_list_next(iter))
  {
    dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)iter->data;
    prof->in_pos = ++in_pos;
  }
  res->profiles = g_list_concat(res->profiles, temp_profiles);

  // read {conf,data}dir/color/out/*.icc
  temp_profiles = load_profile_from_dir("out");
  for(GList *iter = temp_profiles; iter; iter = g_list_next(iter))
  {
    dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)iter->data;
    // FIXME: do want to filter out non-RGB profiles for cases besides histogram profile? colorin is OK with RGB or XYZ, print is OK with anything which LCMS likes, otherwise things are more choosey
    const cmsColorSpaceSignature color_space = cmsGetColorSpace(prof->profile);
    // The histogram profile is used for histogram, clipping indicators and the global color picker.
    // Some of these also assume a matrix profile. LUT profiles don't make much sense in these applications
    // so filter out any profile that doesn't implement the relative colorimetric intent as a matrix (+ TRC).
    // For discussion, see e.g.
    // https://github.com/darktable-org/darktable/issues/7660#issuecomment-760143437
    // For the working profile we also require a matrix profile.
    const gboolean is_valid_matrix_profile
        = dt_colorspaces_get_matrix_from_output_profile(prof->profile, NULL, NULL, NULL, NULL, 0) == 0
          && dt_colorspaces_get_matrix_from_input_profile(prof->profile, NULL, NULL, NULL, NULL, 0) == 0;
    prof->out_pos = ++out_pos;
    prof->display_pos = ++display_pos;
    prof->display2_pos = ++display2_pos;
    if(is_valid_matrix_profile)
    {
      prof->category_pos = ++category_pos;
      prof->work_pos = ++work_pos;
    }
    else
    {
      dt_print(DT_DEBUG_DEV,
               "output profile `%s' color space `%c%c%c%c' not supported for work or histogram profile\n",
               prof->name, (char)(color_space >> 24), (char)(color_space >> 16), (char)(color_space >> 8),
               (char)(color_space));

      if(res->histogram_type == prof->type
         && (prof->type != DT_COLORSPACE_FILE
             || dt_colorspaces_is_profile_equal(prof->filename, res->histogram_filename)))
      {
        // bad histogram profile selected, we must reset it to sRGB
        const char *name = dt_colorspaces_get_name(prof->type, prof->filename);
        dt_control_log(_("profile `%s' not usable as histogram profile. it has been replaced by sRGB!"), name);
        dt_print(DT_DEBUG_ALWAYS,
                "[colorspaces] profile `%s' not usable as histogram profile. it has been replaced by sRGB!\n",
                name);
        res->histogram_type = DT_COLORSPACE_SRGB;
        res->histogram_filename[0] = '\0';
      }
    }
  }
  res->profiles = g_list_concat(res->profiles, temp_profiles);


  if((unsigned int)res->mode > DT_PROFILE_GAMUTCHECK) res->mode = DT_PROFILE_NORMAL;

  _update_display_transforms(res);
  _update_display2_transforms(res);

  return res;
}

void dt_colorspaces_cleanup(dt_colorspaces_t *self)
{
  // remember display profile and softproof/gama checking from conf
  dt_conf_set_int("ui_last/color/display_type", self->display_type);
  dt_conf_set_int("ui_last/color/display2_type", self->display2_type);
  dt_conf_set_int("ui_last/color/softproof_type", self->softproof_type);
  dt_conf_set_int("ui_last/color/histogram_type", self->histogram_type);
  dt_conf_set_string("ui_last/color/display_filename", self->display_filename);
  dt_conf_set_string("ui_last/color/display2_filename", self->display2_filename);
  dt_conf_set_string("ui_last/color/softproof_filename", self->softproof_filename);
  dt_conf_set_string("ui_last/color/histogram_filename", self->histogram_filename);
  dt_conf_set_int("ui_last/color/display_intent", self->display_intent);
  dt_conf_set_int("ui_last/color/display2_intent", self->display2_intent);
  dt_conf_set_int("ui_last/color/softproof_intent", self->softproof_intent);
  dt_conf_set_int("ui_last/color/mode", self->mode);

  if(self->transform_srgb_to_display) cmsDeleteTransform(self->transform_srgb_to_display);
  self->transform_srgb_to_display = NULL;

  if(self->transform_adobe_rgb_to_display) cmsDeleteTransform(self->transform_adobe_rgb_to_display);
  self->transform_adobe_rgb_to_display = NULL;

  if(self->transform_srgb_to_display2) cmsDeleteTransform(self->transform_srgb_to_display2);
  self->transform_srgb_to_display2 = NULL;

  if(self->transform_adobe_rgb_to_display2) cmsDeleteTransform(self->transform_adobe_rgb_to_display2);
  self->transform_adobe_rgb_to_display2 = NULL;

  for(GList *iter = self->profiles; iter; iter = g_list_next(iter))
  {
    dt_colorspaces_color_profile_t *p = (dt_colorspaces_color_profile_t *)iter->data;
    dt_colorspaces_cleanup_profile(p->profile);
  }
  g_list_free_full(self->profiles, free);

  pthread_rwlock_destroy(&self->xprofile_lock);
  g_free(self->colord_profile_file);
  g_free(self->xprofile_data);

  g_free(self->colord_profile_file2);
  g_free(self->xprofile_data2);

  free(self);
}

const char *dt_colorspaces_get_name(dt_colorspaces_color_profile_type_t type,
                                    const char *filename)
{
  switch(type)
  {
     case DT_COLORSPACE_NONE:
       return NULL;
     case DT_COLORSPACE_FILE:
       return filename;
     case DT_COLORSPACE_SRGB:
       return _("sRGB");
     case DT_COLORSPACE_ADOBERGB:
       return _("Adobe RGB (compatible)");
     case DT_COLORSPACE_LIN_REC709:
       return _("linear Rec709 RGB");
     case DT_COLORSPACE_LIN_REC2020:
       return _("linear Rec2020 RGB");
     case DT_COLORSPACE_XYZ:
       return _("linear XYZ");
     case DT_COLORSPACE_LAB:
       return _("Lab");
     case DT_COLORSPACE_INFRARED:
       return _("linear infrared BGR");
     case DT_COLORSPACE_DISPLAY:
       return _("system display profile");
     case DT_COLORSPACE_EMBEDDED_ICC:
       return _("embedded ICC profile");
     case DT_COLORSPACE_EMBEDDED_MATRIX:
       return _("embedded matrix");
     case DT_COLORSPACE_STANDARD_MATRIX:
       return _("standard color matrix");
     case DT_COLORSPACE_ENHANCED_MATRIX:
       return _("enhanced color matrix");
     case DT_COLORSPACE_VENDOR_MATRIX:
       return _("vendor color matrix");
     case DT_COLORSPACE_ALTERNATE_MATRIX:
       return _("alternate color matrix");
     case DT_COLORSPACE_BRG:
       return _("BRG (experimental)");
     case DT_COLORSPACE_EXPORT:
       return _("export profile");
     case DT_COLORSPACE_SOFTPROOF:
       return _("softproof profile");
     case DT_COLORSPACE_WORK:
       return _("work profile");
     case DT_COLORSPACE_DISPLAY2:
       return _("system display profile (second window)");
     case DT_COLORSPACE_REC709:
       return _("Rec709 RGB");
     case DT_COLORSPACE_PROPHOTO_RGB:
       return _("linear ProPhoto RGB");
     case DT_COLORSPACE_PQ_REC2020:
       return _("PQ Rec2020");
     case DT_COLORSPACE_HLG_REC2020:
       return _("HLG Rec2020");
     case DT_COLORSPACE_PQ_P3:
       return _("PQ P3");
     case DT_COLORSPACE_HLG_P3:
       return _("HLG P3");
     case DT_COLORSPACE_LAST:
       break;
  }

  return NULL;
}

#ifdef USE_COLORDGTK
static void dt_colorspaces_get_display_profile_colord_callback(GObject *source, GAsyncResult *res, gpointer user_data)
{
  const dt_colorspaces_color_profile_type_t profile_type
      = (dt_colorspaces_color_profile_type_t)GPOINTER_TO_INT(user_data);

  pthread_rwlock_wrlock(&darktable.color_profiles->xprofile_lock);

  int profile_changed = 0;
  CdWindow *window = CD_WINDOW(source);
  GError *error = NULL;
  CdProfile *profile = cd_window_get_profile_finish(window, res, &error);
  if(error == NULL && profile != NULL)
  {
    const gchar *filename = cd_profile_get_filename(profile);
    if(filename)
    {
      if((profile_type == DT_COLORSPACE_DISPLAY2
          && g_strcmp0(filename, darktable.color_profiles->colord_profile_file2))
         || (profile_type != DT_COLORSPACE_DISPLAY2
             && g_strcmp0(filename, darktable.color_profiles->colord_profile_file)))
      {
        /* the profile has changed (either because the user changed the colord settings or because we are on a
         * different screen now) */
        // update darktable.color_profiles->colord_profile_file
        if(profile_type == DT_COLORSPACE_DISPLAY2)
        {
          g_free(darktable.color_profiles->colord_profile_file2);
          darktable.color_profiles->colord_profile_file2 = g_strdup(filename);
        }
        else
        {
          g_free(darktable.color_profiles->colord_profile_file);
          darktable.color_profiles->colord_profile_file = g_strdup(filename);
        }
        // read the file
        guchar *tmp_data = NULL;
        gsize size;
        g_file_get_contents(filename, (gchar **)&tmp_data, &size, NULL);
        if(profile_type == DT_COLORSPACE_DISPLAY2)
        {
          profile_changed = size > 0 && (darktable.color_profiles->xprofile_size2 != size
                                         || memcmp(darktable.color_profiles->xprofile_data2, tmp_data, size) != 0);
        }
        else
        {
          profile_changed = size > 0 && (darktable.color_profiles->xprofile_size != size
                                         || memcmp(darktable.color_profiles->xprofile_data, tmp_data, size) != 0);
        }
        if(profile_changed)
        {
          if(profile_type == DT_COLORSPACE_DISPLAY2)
            _update_display2_profile(tmp_data, size, NULL, 0);
          else
            _update_display_profile(tmp_data, size, NULL, 0);
          dt_print(DT_DEBUG_CONTROL,
                   "[color profile] colord gave us a new screen profile: '%s' (size: %zu)\n", filename, size);
        }
        else
        {
          g_free(tmp_data);
        }
      }
    }
  }
  if(profile) g_object_unref(profile);
  g_object_unref(window);

  pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);

  if(profile_changed) DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_CHANGED);
}
#endif

#if defined GDK_WINDOWING_X11
static int _gtk_get_monitor_num(GdkMonitor *monitor)
{
  GdkDisplay *display;
  int n_monitors, i;

  display = gdk_monitor_get_display(monitor);
  n_monitors = gdk_display_get_n_monitors(display);
  for(i = 0; i < n_monitors; i++)
  {
    if(gdk_display_get_monitor(display, i) == monitor) return i;
  }

  return -1;
}
#endif

// Get the display ICC profile of the monitor associated with the widget.
// For X display, uses the ICC profile specifications version 0.2 from
// http://burtonini.com/blog/computers/xicc
// Based on code from Gimp's modules/cdisplay_lcms.c
void dt_colorspaces_set_display_profile(const dt_colorspaces_color_profile_type_t profile_type)
{
  if(!dt_control_running()) return;
  // make sure that no one gets a broken profile
  // FIXME: benchmark if the try is really needed when moving/resizing the window. Maybe we can just lock it
  // and block
  if(pthread_rwlock_trywrlock(&darktable.color_profiles->xprofile_lock))
    return; // we are already updating the profile. Or someone is reading right now. Too bad we can't
            // distinguish that. Whatever ...

  guint8 *buffer = NULL;
  gint buffer_size = 0;
  gchar *profile_source = NULL;

#if defined GDK_WINDOWING_X11

  // we will use the xatom no matter what configured when compiled without colord
  gboolean use_xatom = TRUE;
#if defined USE_COLORDGTK
  gboolean use_colord = TRUE;
  const char *display_profile_source = (profile_type == DT_COLORSPACE_DISPLAY2)
                                      ? dt_conf_get_string_const("ui_last/display2_profile_source")
                                      : dt_conf_get_string_const("ui_last/display_profile_source");
  if(display_profile_source)
  {
    if(!strcmp(display_profile_source, "xatom"))
      use_colord = FALSE;
    else if(!strcmp(display_profile_source, "colord"))
      use_xatom = FALSE;
  }
#endif

  /* let's have a look at the xatom, just in case ... */
  if(use_xatom)
  {
    GtkWidget *widget = (profile_type == DT_COLORSPACE_DISPLAY2) ? darktable.develop->second_window.second_wnd
                                                                 : dt_ui_center(darktable.gui->ui);
    GdkWindow *window = gtk_widget_get_window(widget);
    GdkScreen *screen = gtk_widget_get_screen(widget);
    if(screen == NULL) screen = gdk_screen_get_default();

    GdkDisplay *display = gtk_widget_get_display(widget);
    int monitor = _gtk_get_monitor_num(gdk_display_get_monitor_at_window(display, window));

    char *atom_name;
    if(monitor > 0)
      atom_name = g_strdup_printf("_ICC_PROFILE_%d", monitor);
    else
      atom_name = g_strdup("_ICC_PROFILE");

    profile_source = g_strdup_printf("xatom %s", atom_name);

    GdkAtom type = GDK_NONE;
    gint format = 0;
    gdk_property_get(gdk_screen_get_root_window(screen), gdk_atom_intern(atom_name, FALSE), GDK_NONE, 0,
                     64 * 1024 * 1024, FALSE, &type, &format, &buffer_size, &buffer);
    g_free(atom_name);
  }

#ifdef USE_COLORDGTK
  /* also try to get the profile from colord. this will set the value asynchronously! */
  if(use_colord)
  {
    CdWindow *window = cd_window_new();
    GtkWidget *center_widget = (profile_type == DT_COLORSPACE_DISPLAY2)
                                   ? darktable.develop->second_window.second_wnd
                                   : dt_ui_center(darktable.gui->ui);
    cd_window_get_profile(window, center_widget, NULL, dt_colorspaces_get_display_profile_colord_callback,
                          GINT_TO_POINTER(profile_type));
  }
#endif

#elif defined GDK_WINDOWING_QUARTZ
#if 0
  GtkWidget *widget = (profile_type == DT_COLORSPACE_DISPLAY2) ? darktable.develop->second_window.second_wnd : dt_ui_center(darktable.gui->ui);
  GdkScreen *screen = gtk_widget_get_screen(widget);
  if(screen == NULL) screen = gdk_screen_get_default();
  int monitor = gdk_screen_get_monitor_at_window(screen, gtk_widget_get_window(widget));

  CGDirectDisplayID ids[monitor + 1];
  uint32_t total_ids;
  CMProfileRef prof = NULL;
  if(CGGetOnlineDisplayList(monitor + 1, &ids[0], &total_ids) == kCGErrorSuccess && total_ids == monitor + 1)
    CMGetProfileByAVID(ids[monitor], &prof);
  if(prof != NULL)
  {
    CFDataRef data;
    data = CMProfileCopyICCData(NULL, prof);
    CMCloseProfile(prof);

    UInt8 *tmp_buffer = (UInt8 *)g_malloc(CFDataGetLength(data));
    CFDataGetBytes(data, CFRangeMake(0, CFDataGetLength(data)), tmp_buffer);

    buffer = (guint8 *)tmp_buffer;
    buffer_size = CFDataGetLength(data);

    CFRelease(data);
  }
  profile_source = g_strdup("osx color profile api");
#endif
#elif defined G_OS_WIN32
  HDC hdc = GetDC(NULL);
  if(hdc != NULL)
  {
    DWORD len = 0;
    GetICMProfile(hdc, &len, NULL);
    wchar_t *wpath = g_new(wchar_t, len);

    if(GetICMProfileW(hdc, &len, wpath))
    {
      gchar *path = g_utf16_to_utf8(wpath, -1, NULL, NULL, NULL);
      if(path)
      {
        gsize size;
        g_file_get_contents(path, (gchar **)&buffer, &size, NULL);
        buffer_size = size;
        g_free(path);
      }
    }
    g_free(wpath);
    ReleaseDC(NULL, hdc);
  }
  profile_source = g_strdup("windows color profile api");
#endif

  int profile_changed = 0;
  if(profile_type == DT_COLORSPACE_DISPLAY2)
  {
    profile_changed
        = buffer_size > 0 && (darktable.color_profiles->xprofile_size2 != buffer_size
                              || memcmp(darktable.color_profiles->xprofile_data2, buffer, buffer_size) != 0);
  }
  else
  {
    profile_changed
        = buffer_size > 0 && (darktable.color_profiles->xprofile_size != buffer_size
                              || memcmp(darktable.color_profiles->xprofile_data, buffer, buffer_size) != 0);
  }
  if(profile_changed)
  {
    char name[512] = { 0 };
    if(profile_type == DT_COLORSPACE_DISPLAY2)
      _update_display2_profile(buffer, buffer_size, name, sizeof(name));
    else
      _update_display_profile(buffer, buffer_size, name, sizeof(name));
    dt_print(DT_DEBUG_CONTROL, "[color profile] we got a new screen profile `%s' from the %s (size: %d)\n",
             *name ? name : "(unknown)", profile_source, buffer_size);
  }
  else
  {
    g_free(buffer);
  }
  pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
  if(profile_changed) DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_CHANGED);
  g_free(profile_source);
}

static gboolean _colorspaces_is_base_name(const char *profile)
{
  const char *f = profile;
  while(*f != '\0')
  {
    if(*f == '/' || *f == '\\') return FALSE;
    f++;
  }
  return TRUE;
}

static const char *_colorspaces_get_base_name(const char *profile)
{
  const char* f = profile + strlen(profile);
  for(; f >= profile; f--)
  {
    if(*f == '/' || *f == '\\')
      return ++f;   // path separator found - return the filename only, without the leading separator
  }
  return f;         // no separator found - consider profile_name to be a "base" one
}

gboolean dt_colorspaces_is_profile_equal(const char *fullname, const char *filename)
{
  // for backward compatibility we need to also ensure that we check
  // for basename, indeed filename parameter may be in fact just a
  // basename as recorded in an iop.
  return _colorspaces_is_base_name(filename)
    ? !strcmp(_colorspaces_get_base_name(fullname), filename)
    : !strcmp(_colorspaces_get_base_name(fullname), _colorspaces_get_base_name(filename));
}

dt_colorspaces_color_profile_type_t dt_colorspaces_cicp_to_type(const dt_colorspaces_cicp_t *cicp, const char *filename)
{
  switch(cicp->color_primaries)
  {
    /* Give up immediately if unspecified */
    case DT_CICP_COLOR_PRIMARIES_UNSPECIFIED:
      if(cicp->transfer_characteristics == DT_CICP_TRANSFER_CHARACTERISTICS_UNSPECIFIED
         && cicp->matrix_coefficients == DT_CICP_MATRIX_COEFFICIENTS_UNSPECIFIED)
        return DT_COLORSPACE_NONE;
      break; /* unspecified */

    /* REC709 */
    case DT_CICP_COLOR_PRIMARIES_REC709:

      switch(cicp->transfer_characteristics)
      {
        /* SRGB */
        case DT_CICP_TRANSFER_CHARACTERISTICS_SRGB:

          switch(cicp->matrix_coefficients)
          {
            case DT_CICP_MATRIX_COEFFICIENTS_IDENTITY: /* support RGB (4:4:4 or lossless) */
            case DT_CICP_MATRIX_COEFFICIENTS_SYCC:
            case DT_CICP_MATRIX_COEFFICIENTS_REC601: /* support equivalents just in case of mistagging */
            case DT_CICP_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL: /* support incorrectly tagged files */
            case DT_CICP_MATRIX_COEFFICIENTS_UNSPECIFIED:
              return DT_COLORSPACE_SRGB;
            default:
              break;
          }

          break; /* SRGB */

        /* REC709 */
        case DT_CICP_TRANSFER_CHARACTERISTICS_REC709:
        case DT_CICP_TRANSFER_CHARACTERISTICS_REC601:      /* support equivalents just in case of mistagging */
        case DT_CICP_TRANSFER_CHARACTERISTICS_REC2020_10B: /* support equivalents just in case of mistagging */
        case DT_CICP_TRANSFER_CHARACTERISTICS_REC2020_12B: /* support equivalents just in case of mistagging */

          switch(cicp->matrix_coefficients)
          {
            case DT_CICP_MATRIX_COEFFICIENTS_IDENTITY: /* support RGB (4:4:4 or lossless) */
            case DT_CICP_MATRIX_COEFFICIENTS_REC709:
            case DT_CICP_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL:
            case DT_CICP_MATRIX_COEFFICIENTS_UNSPECIFIED:
              return DT_COLORSPACE_REC709;
            default:
              break;
          }

          break; /* REC709 */

        /* LINEAR REC709 */
        case DT_CICP_TRANSFER_CHARACTERISTICS_LINEAR:

          switch(cicp->matrix_coefficients)
          {
            case DT_CICP_MATRIX_COEFFICIENTS_IDENTITY: /* support RGB (4:4:4 or lossless) */
            case DT_CICP_MATRIX_COEFFICIENTS_REC709:
            case DT_CICP_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL:
            case DT_CICP_MATRIX_COEFFICIENTS_UNSPECIFIED:
              return DT_COLORSPACE_LIN_REC709;
            default:
              break;
          }

          break; /* LINEAR REC709 */

        default:
          break;
      }

      break; /* REC709 */

    /* REC2020 */
    case DT_CICP_COLOR_PRIMARIES_REC2020:

      switch(cicp->transfer_characteristics)
      {
        /* LINEAR REC2020 */
        case DT_CICP_TRANSFER_CHARACTERISTICS_LINEAR:

          switch(cicp->matrix_coefficients)
          {
            case DT_CICP_MATRIX_COEFFICIENTS_IDENTITY: /* support RGB (4:4:4 or lossless) */
            case DT_CICP_MATRIX_COEFFICIENTS_REC2020_NCL:
            case DT_CICP_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL:
            case DT_CICP_MATRIX_COEFFICIENTS_UNSPECIFIED:
              return DT_COLORSPACE_LIN_REC2020;
            default:
              break;
          }

          break; /* LINEAR REC2020 */

        /* PQ REC2020 */
        case DT_CICP_TRANSFER_CHARACTERISTICS_PQ:

          switch(cicp->matrix_coefficients)
          {
            case DT_CICP_MATRIX_COEFFICIENTS_IDENTITY: /* support RGB (4:4:4 or lossless) */
            case DT_CICP_MATRIX_COEFFICIENTS_REC2020_NCL:
            case DT_CICP_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL:
            case DT_CICP_MATRIX_COEFFICIENTS_UNSPECIFIED:
              return DT_COLORSPACE_PQ_REC2020;
            default:
              break;
          }

          break; /* PQ REC2020 */

        /* HLG REC2020 */
        case DT_CICP_TRANSFER_CHARACTERISTICS_HLG:

          switch(cicp->matrix_coefficients)
          {
            case DT_CICP_MATRIX_COEFFICIENTS_IDENTITY: /* support RGB (4:4:4 or lossless) */
            case DT_CICP_MATRIX_COEFFICIENTS_REC2020_NCL:
            case DT_CICP_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL:
            case DT_CICP_MATRIX_COEFFICIENTS_UNSPECIFIED:
              return DT_COLORSPACE_HLG_REC2020;
            default:
              break;
          }

          break; /* HLG REC2020 */

        default:
          break;
      }

      break; /* REC2020 */

    /* P3 */
    case DT_CICP_COLOR_PRIMARIES_P3:

      switch(cicp->transfer_characteristics)
      {
        /* PQ P3 */
        case DT_CICP_TRANSFER_CHARACTERISTICS_PQ:

          switch(cicp->matrix_coefficients)
          {
            case DT_CICP_MATRIX_COEFFICIENTS_IDENTITY: /* support RGB (4:4:4 or lossless) */
            case DT_CICP_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL:
            case DT_CICP_MATRIX_COEFFICIENTS_UNSPECIFIED:
              return DT_COLORSPACE_PQ_P3;
            default:
              break;
          }

          break; /* PQ P3 */

        /* HLG P3 */
        case DT_CICP_TRANSFER_CHARACTERISTICS_HLG:

          switch(cicp->matrix_coefficients)
          {
            case DT_CICP_MATRIX_COEFFICIENTS_IDENTITY: /* support RGB (4:4:4 or lossless) */
            case DT_CICP_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL:
            case DT_CICP_MATRIX_COEFFICIENTS_UNSPECIFIED:
              return DT_COLORSPACE_HLG_P3;
            default:
              break;
          }

          break; /* HLG P3 */

        default:
          break;
      }

      break; /* P3 */

    /* XYZ */
    case DT_CICP_COLOR_PRIMARIES_XYZ:

      switch(cicp->transfer_characteristics)
      {
        /* LINEAR XYZ */
        case DT_CICP_TRANSFER_CHARACTERISTICS_LINEAR:

          switch(cicp->matrix_coefficients)
          {
            case DT_CICP_MATRIX_COEFFICIENTS_IDENTITY:
            case DT_CICP_MATRIX_COEFFICIENTS_UNSPECIFIED:
              return DT_COLORSPACE_XYZ;
            default:
              break;
          }

          break; /* LINEAR XYZ */

        default:
          break;
      }

      break; /* XYZ */

    default:
      break;
  }

  if(filename != NULL)
    dt_print(DT_DEBUG_IMAGEIO, "[colorin] unsupported CICP color profile for `%s': %d/%d/%d\n", filename,
             cicp->color_primaries, cicp->transfer_characteristics, cicp->matrix_coefficients);

  return DT_COLORSPACE_NONE;
}

static const dt_colorspaces_color_profile_t *_get_profile(dt_colorspaces_t *self,
                                                          dt_colorspaces_color_profile_type_t type,
                                                          const char *filename,
                                                          dt_colorspaces_profile_direction_t direction)
{
  for(GList *iter = self->profiles; iter; iter = g_list_next(iter))
  {
    dt_colorspaces_color_profile_t *p = (dt_colorspaces_color_profile_t *)iter->data;
    if(((direction & DT_PROFILE_DIRECTION_IN && p->in_pos > -1)
        || (direction & DT_PROFILE_DIRECTION_OUT && p->out_pos > -1)
        || (direction & DT_PROFILE_DIRECTION_WORK && p->work_pos > -1)
        || (direction & DT_PROFILE_DIRECTION_DISPLAY && p->display_pos > -1)
        || (direction & DT_PROFILE_DIRECTION_DISPLAY2 && p->display2_pos > -1))
       && (p->type == type
           && (type != DT_COLORSPACE_FILE || dt_colorspaces_is_profile_equal(p->filename, filename))))
    {
      return p;
    }
  }

  return NULL;
}

const dt_colorspaces_color_profile_t *dt_colorspaces_get_profile(dt_colorspaces_color_profile_type_t type,
                                                                 const char *filename,
                                                                 dt_colorspaces_profile_direction_t direction)
{
  return _get_profile(darktable.color_profiles, type, filename, direction);
}

// Copied from dcraw's pseudoinverse()
static void dt_colorspaces_pseudoinverse(double (*in)[3], double (*out)[3], int size)
{
  double work[3][6];

  for(int i = 0; i < 3; i++) {
    for(int j = 0; j < 6; j++)
      work[i][j] = j == i+3;
    for(int j = 0; j < 3; j++)
      for(int k = 0; k < size; k++)
        work[i][j] += in[k][i] * in[k][j];
  }
  for(int i = 0; i < 3; i++) {
    double num = work[i][i];
    for(int j = 0; j < 6; j++)
      work[i][j] /= num;
    for(int k = 0; k < 3; k++) {
      if(k==i) continue;
      num = work[k][i];
      for(int j = 0; j < 6; j++)
        work[k][j] -= work[i][j] * num;
    }
  }
  for(int i = 0; i < size; i++)
    for(int j = 0; j < 3; j++)
    {
      out[i][j] = 0.0f;
      for(int k = 0; k < 3; k++)
        out[i][j] += work[j][k+3] * in[i][k];
    }
}

int dt_colorspaces_conversion_matrices_xyz(const float adobe_XYZ_to_CAM[4][3], float in_XYZ_to_CAM[9], double XYZ_to_CAM[4][3], double CAM_to_XYZ[3][4])
{
  if(!isnan(in_XYZ_to_CAM[0]))
  {
    for(int i = 0; i < 9; i++)
        XYZ_to_CAM[i/3][i%3] = (double) in_XYZ_to_CAM[i];
    for(int i = 0; i < 3; i++)
      XYZ_to_CAM[3][i] = 0.0f;
  }
  else
  {
    if(isnan(adobe_XYZ_to_CAM[0][0]))
      return FALSE;

    for(int i = 0; i < 4; i++)
      for(int j = 0; j < 3; j++)
        XYZ_to_CAM[i][j] = (double)adobe_XYZ_to_CAM[i][j];
  }

  // Invert the matrix
  double inverse[4][3];
  dt_colorspaces_pseudoinverse (XYZ_to_CAM, inverse, 4);
  for(int i = 0; i < 3; i++)
    for(int j = 0; j < 4; j++)
      CAM_to_XYZ[i][j] = inverse[j][i];

  return TRUE;
}

// Converted from dcraw's cam_xyz_coeff()
int dt_colorspaces_conversion_matrices_rgb(const float adobe_XYZ_to_CAM[4][3],
                                           double out_RGB_to_CAM[4][3], double out_CAM_to_RGB[3][4],
                                           const float *embedded_matrix,
                                           double mul[4])
{
  double RGB_to_CAM[4][3];

  float XYZ_to_CAM[4][3];
  XYZ_to_CAM[0][0] = NAN;

  if(embedded_matrix == NULL || isnan(embedded_matrix[0]))
  {
    for(int k=0; k<4; k++)
      for(int i=0; i<3; i++)
        XYZ_to_CAM[k][i] = adobe_XYZ_to_CAM[k][i];
  }
  else
  {
    // keep in sync with reload_defaults from colorin.c
    // embedded matrix is used with higher priority than standard one
    XYZ_to_CAM[0][0] = embedded_matrix[0];
    XYZ_to_CAM[0][1] = embedded_matrix[1];
    XYZ_to_CAM[0][2] = embedded_matrix[2];

    XYZ_to_CAM[1][0] = embedded_matrix[3];
    XYZ_to_CAM[1][1] = embedded_matrix[4];
    XYZ_to_CAM[1][2] = embedded_matrix[5];

    XYZ_to_CAM[2][0] = embedded_matrix[6];
    XYZ_to_CAM[2][1] = embedded_matrix[7];
    XYZ_to_CAM[2][2] = embedded_matrix[8];
  }

  if(isnan(XYZ_to_CAM[0][0]))
    return FALSE;

  const double RGB_to_XYZ[3][3] = {
  // sRGB D65
    { 0.412453, 0.357580, 0.180423 },
    { 0.212671, 0.715160, 0.072169 },
    { 0.019334, 0.119193, 0.950227 },
  };

  // Multiply RGB matrix
  for(int i = 0; i < 4; i++)
    for(int j = 0; j < 3; j++)
    {
      RGB_to_CAM[i][j] = 0.0f;
      for(int k = 0; k < 3; k++)
        RGB_to_CAM[i][j] += XYZ_to_CAM[i][k] * RGB_to_XYZ[k][j];
    }

  // Normalize cam_rgb so that cam_rgb * (1,1,1) is (1,1,1,1)
  for(int i = 0; i < 4; i++) {
    double num = 0.0f;
    for(int j = 0; j < 3; j++)
      num += RGB_to_CAM[i][j];
    for(int j = 0; j < 3; j++)
       RGB_to_CAM[i][j] /= num;
    if(mul) mul[i] = 1.0f / num;
  }

  if(out_RGB_to_CAM)
    for(int i = 0; i < 4; i++)
      for(int j = 0; j < 3; j++)
        out_RGB_to_CAM[i][j] = RGB_to_CAM[i][j];

  if(out_CAM_to_RGB)
  {
    // Invert the matrix
    double inverse[4][3];
    dt_colorspaces_pseudoinverse (RGB_to_CAM, inverse, 4);
    for(int i = 0; i < 3; i++)
      for(int j = 0; j < 4; j++)
        out_CAM_to_RGB[i][j] = inverse[j][i];
  }

  return TRUE;
}

void dt_colorspaces_cygm_apply_coeffs_to_rgb(float *out, const float *in, int num, double RGB_to_CAM[4][3],
                                             double CAM_to_RGB[3][4], dt_aligned_pixel_t coeffs)
{
  // Create the CAM to RGB with applied WB matrix
  double CAM_to_RGB_WB[3][4];
  for(int a=0; a<3; a++)
    for(int b=0; b<4; b++)
      CAM_to_RGB_WB[a][b] = CAM_to_RGB[a][b] * coeffs[b];

  // Create the RGB->RGB+WB matrix
  double RGB_to_RGB_WB[3][3];
  for(int a=0; a<3; a++)
    for(int b=0; b<3; b++) {
      RGB_to_RGB_WB[a][b] = 0.0f;
      for(int c=0; c<4; c++)
        RGB_to_RGB_WB[a][b] += CAM_to_RGB_WB[a][c] * RGB_to_CAM[c][b];
    }

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(in, out, num, RGB_to_RGB_WB) schedule(static)
#endif
  for(int i = 0; i < num; i++)
  {
    const float *inpos = &in[i*4];
    float *outpos = &out[i*4];
    outpos[0]=outpos[1]=outpos[2] = 0.0f;
    for(int a=0; a<3; a++)
      for(int b=0; b<3; b++)
        outpos[a] += RGB_to_RGB_WB[a][b] * inpos[b];
  }
}

void dt_colorspaces_cygm_to_rgb(float *out, int num, double CAM_to_RGB[3][4])
{
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(out, num, CAM_to_RGB) schedule(static)
#endif
  for(int i = 0; i < num; i++)
  {
    float *in = &out[i*4];
    dt_aligned_pixel_t o = {0.0f,0.0f,0.0f};
    for(int c = 0; c < 3; c++)
      for(int k = 0; k < 4; k++)
        o[c] += CAM_to_RGB[c][k] * in[k];
    for(int c = 0; c < 3; c++)
      in[c] = o[c];
  }
}

void dt_colorspaces_rgb_to_cygm(float *out, int num, double RGB_to_CAM[4][3])
{
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(out, num, RGB_to_CAM) schedule(static)
#endif
  for(int i = 0; i < num; i++)
  {
    float *in = &out[i*3];
    dt_aligned_pixel_t o = {0.0f,0.0f,0.0f,0.0f};
    for(int c = 0; c < 4; c++)
      for(int k = 0; k < 3; k++)
        o[c] += RGB_to_CAM[c][k] * in[k];
    for(int c = 0; c < 4; c++)
      in[c] = o[c];
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
