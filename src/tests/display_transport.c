/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable. If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/display_transport.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void _require(const int condition, const char *message)
{
  if(!condition)
  {
    fprintf(stderr, "display transport: %s\n", message);
    exit(EXIT_FAILURE);
  }
}

static cmsHPROFILE _rgb_profile(const cmsCIExyYTRIPLE *primaries,
                               const double gamma[3])
{
  const cmsCIExyY white = { 0.3127, 0.3290, 1.0 };
  cmsToneCurve *curves[3];
  for(int c = 0; c < 3; c++)
  {
    curves[c] = cmsBuildGamma(NULL, gamma[c]);
    _require(curves[c] != NULL, "cannot create reference tone curve");
  }
  cmsHPROFILE profile = cmsCreateRGBProfile(&white, primaries, curves);
  for(int c = 0; c < 3; c++) cmsFreeToneCurve(curves[c]);
  _require(profile != NULL, "cannot create reference RGB profile");
  return profile;
}

static cmsHTRANSFORM _transform(cmsHPROFILE source,
                               const cmsUInt32Number source_format,
                               cmsHPROFILE destination,
                               const cmsUInt32Number destination_format)
{
  cmsHTRANSFORM transform = cmsCreateTransform(source, source_format, destination,
                                             destination_format, INTENT_RELATIVE_COLORIMETRIC,
                                             cmsFLAGS_NOOPTIMIZE | cmsFLAGS_NOCACHE);
  _require(transform != NULL, "cannot create reference transform");
  return transform;
}

static void _check_encoding(cmsHPROFILE profile, const double xy[3][2])
{
  const cmsCIExyY white_d65 = { 0.3127, 0.3290, 1.0 };
  cmsCIEXYZ d65;
  cmsxyY2XYZ(&d65, &white_d65);
  const cmsTagSignature colorants[3] = {
    cmsSigRedColorantTag, cmsSigGreenColorantTag, cmsSigBlueColorantTag
  };
  for(int c = 0; c < 3; c++)
  {
    const cmsCIEXYZ *primary_d50 = cmsReadTag(profile, colorants[c]);
    _require(primary_d50 != NULL, "missing display profile colorant");
    cmsCIEXYZ primary_d65;
    _require(cmsAdaptToIlluminant(&primary_d65, cmsD50_XYZ(), &d65, primary_d50),
             "cannot adapt display profile primary to D65");
    cmsCIExyY primary;
    cmsXYZ2xyY(&primary, &primary_d65);
    _require(fabs(primary.x - xy[c][0]) < 0.0001
             && fabs(primary.y - xy[c][1]) < 0.0001,
             "display profile primaries do not match the Wayland description");
  }

  cmsHPROFILE xyz = cmsCreateXYZProfile();
  _require(xyz != NULL, "cannot create reference XYZ profile");
  cmsHTRANSFORM transform = _transform(profile, TYPE_RGB_FLT, xyz, TYPE_XYZ_DBL);
  const float gray_levels[] = { 0.0f, 0.02f, 0.18f, 0.5f, 0.75f, 1.0f };
  const cmsCIEXYZ *d50 = cmsD50_XYZ();
  for(size_t k = 0; k < sizeof(gray_levels) / sizeof(gray_levels[0]); k++)
  {
    const float gray[3] = { gray_levels[k], gray_levels[k], gray_levels[k] };
    cmsCIEXYZ result;
    cmsDoTransform(transform, gray, &result, 1);
    const double luminance = pow(gray_levels[k], 2.2);
    _require(fabs(result.X - d50->X * luminance) < 0.0001
             && fabs(result.Y - luminance) < 0.0001
             && fabs(result.Z - d50->Z * luminance) < 0.0001,
             "display profile white or gamma does not match D65 gamma 2.2");
  }
  cmsDeleteTransform(transform);
  cmsCloseProfile(xyz);
}

static cmsHPROFILE _proof_profile(cmsHPROFILE original)
{
  cmsUInt32Number size = 0;
  _require(cmsSaveProfileToMem(original, NULL, &size), "cannot measure proof profile");
  void *data = malloc(size);
  _require(data != NULL, "cannot allocate proof profile");
  _require(cmsSaveProfileToMem(original, data, &size), "cannot serialize proof profile");
  cmsHPROFILE proof = cmsOpenProfileFromMem(data, size);
  free(data);
  _require(proof != NULL, "cannot reopen proof profile");
  return proof;
}

static double _delta_e(cmsHTRANSFORM display_lab,
                       const float left[3], const float right[3])
{
  cmsCIELab lab_left, lab_right;
  cmsDoTransform(display_lab, left, &lab_left, 1);
  cmsDoTransform(display_lab, right, &lab_right, 1);
  const double difference = cmsCIE2000DeltaE(&lab_left, &lab_right, 1.0, 1.0, 1.0);
  _require(isfinite(difference), "display transform produced a non-finite color difference");
  return difference;
}

static float _clip(const float value)
{
  return fmaxf(0.0f, fminf(1.0f, value));
}

static void _check_ui_composition(cmsHPROFILE transport, cmsHPROFILE ui)
{
  cmsHPROFILE srgb = cmsCreate_sRGBProfile();
  cmsHPROFILE lab = cmsCreateLab4Profile(NULL);
  _require(srgb != NULL && lab != NULL, "cannot create UI reference profiles");
  cmsHTRANSFORM srgb_ui = _transform(srgb, TYPE_RGB_FLT, ui, TYPE_RGB_FLT);
  cmsHTRANSFORM ui_srgb = _transform(ui, TYPE_RGB_FLT, srgb, TYPE_RGB_FLT);
  cmsHTRANSFORM srgb_transport = _transform(srgb, TYPE_RGB_FLT, transport, TYPE_RGB_FLT);
  cmsHTRANSFORM transport_ui = _transform(transport, TYPE_RGB_FLT, ui, TYPE_RGB_FLT);
  cmsHTRANSFORM transport_ui_8bit = _transform(transport, TYPE_BGRA_8, ui, TYPE_BGRA_8);
  cmsHTRANSFORM srgb_lab = _transform(srgb, TYPE_RGB_FLT, lab, TYPE_Lab_DBL);
  // the piece-wise knee and near-black samples distinguish true sRGB from the gamma22 parent
  const float levels[] = { 0.0f, 0.02f, 0.04045f, 0.08f, 0.18f, 0.5f, 0.75f, 1.0f };
  const size_t count = sizeof(levels) / sizeof(levels[0]);
  double max_float = 0.0, max_ui_8bit = 0.0, max_native_8bit = 0.0;
  double max_wrong_encoding = 0.0;
  for(size_t r = 0; r < count; r++)
    for(size_t g = 0; g < count; g++)
      for(size_t b = 0; b < count; b++)
      {
        const float rgb[3] = { levels[r], levels[g], levels[b] };
        float encoded[3], native[3], reconstructed[3];
        cmsDoTransform(srgb_ui, rgb, encoded, 1);
        for(int c = 0; c < 3; c++)
        {
          const double linear = rgb[c] <= 0.04045f
            ? rgb[c] / 12.92 : pow((rgb[c] + 0.055) / 1.055, 2.4);
          _require(fabs(encoded[c] - pow(linear, 1.0 / 2.2)) < 0.0001,
                   "UI conversion replaced the true sRGB diagnostic source encoding");
        }
        cmsDoTransform(ui_srgb, encoded, reconstructed, 1);
        max_float = fmax(max_float, _delta_e(srgb_lab, rgb, reconstructed));
        for(int c = 0; c < 3; c++) encoded[c] = roundf(_clip(encoded[c]) * 255.0f) / 255.0f;
        cmsDoTransform(ui_srgb, encoded, reconstructed, 1);
        max_ui_8bit = fmax(max_ui_8bit, _delta_e(srgb_lab, rgb, reconstructed));

        cmsDoTransform(srgb_transport, rgb, native, 1);
        cmsDoTransform(transport_ui, native, encoded, 1);
        cmsDoTransform(ui_srgb, encoded, reconstructed, 1);
        max_float = fmax(max_float, _delta_e(srgb_lab, rgb, reconstructed));
        unsigned char native_bytes[4] = { 0, 0, 0, 255 }, ui_bytes[4] = { 0 };
        for(int c = 0; c < 3; c++) native_bytes[2 - c] = lroundf(_clip(native[c]) * 255.0f);
        cmsDoTransform(transport_ui_8bit, native_bytes, ui_bytes, 1);
        for(int c = 0; c < 3; c++) encoded[c] = ui_bytes[2 - c] / 255.0f;
        cmsDoTransform(ui_srgb, encoded, reconstructed, 1);
        max_native_8bit = fmax(max_native_8bit, _delta_e(srgb_lab, rgb, reconstructed));

        // copying true sRGB bytes into the gamma22 parent changes shadow and midtone colors
        cmsDoTransform(ui_srgb, rgb, reconstructed, 1);
        max_wrong_encoding = fmax(max_wrong_encoding, _delta_e(srgb_lab, rgb, reconstructed));
      }
  printf("BT709 gamma22 UI: max deltaE00 float %.6f, UI 8-bit %.6f, "
         "native and UI 8-bit %.6f, wrong encoding %.6f\n",
         max_float, max_ui_8bit, max_native_8bit, max_wrong_encoding);
  _require(max_float < 0.03, "native image and UI float conversions disagree");
  _require(max_ui_8bit < 1.0, "8-bit UI conversion exceeded deltaE00 tolerance");
  _require(max_native_8bit < 2.0, "8-bit native fallback to UI exceeded deltaE00 tolerance");
  _require(max_wrong_encoding > 0.5, "UI reference failed to expose an sRGB/gamma22 mismatch");
  cmsDeleteTransform(srgb_lab);
  cmsDeleteTransform(transport_ui_8bit);
  cmsDeleteTransform(transport_ui);
  cmsDeleteTransform(srgb_transport);
  cmsDeleteTransform(ui_srgb);
  cmsDeleteTransform(srgb_ui);
  cmsCloseProfile(lab);
  cmsCloseProfile(srgb);
}

static void _check_composition(cmsHPROFILE source,
                               cmsHPROFILE transport,
                               cmsHPROFILE display,
                               const char *name,
                               const int check_negative_control)
{
  cmsHPROFILE lab = cmsCreateLab4Profile(NULL);
  cmsHPROFILE srgb = cmsCreate_sRGBProfile();
  _require(lab != NULL && srgb != NULL, "cannot create reference Lab and sRGB profiles");
  // v2 serialization quantizes the proof TRCs, preventing lcms from eliding the gamut roundtrip
  cmsSetProfileVersion(srgb, 2.4);
  cmsHPROFILE proof = _proof_profile(srgb);
  cmsHTRANSFORM source_lab = _transform(source, TYPE_RGB_FLT, lab, TYPE_Lab_FLT);
  cmsHTRANSFORM lab_transport = _transform(lab, TYPE_Lab_FLT, transport, TYPE_RGB_FLT);
  cmsHTRANSFORM lab_display = _transform(lab, TYPE_Lab_FLT, display, TYPE_RGB_FLT);
  cmsHTRANSFORM transport_display = _transform(transport, TYPE_RGB_FLT, display, TYPE_RGB_FLT);
  cmsHTRANSFORM display_lab = _transform(display, TYPE_RGB_FLT, lab, TYPE_Lab_DBL);
  cmsHTRANSFORM lab_srgb = _transform(lab, TYPE_Lab_FLT, srgb, TYPE_RGB_FLT);
  cmsHTRANSFORM srgb_display = _transform(srgb, TYPE_RGB_FLT, display, TYPE_RGB_FLT);
  const cmsUInt32Number flags = cmsFLAGS_SOFTPROOFING | cmsFLAGS_NOCACHE
                               | cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_NOOPTIMIZE;
  cmsHTRANSFORM proof_transport = cmsCreateProofingTransform
    (lab, TYPE_Lab_FLT, transport, TYPE_RGB_FLT, proof,
     INTENT_RELATIVE_COLORIMETRIC, INTENT_RELATIVE_COLORIMETRIC, flags);
  cmsHTRANSFORM proof_display = cmsCreateProofingTransform
    (lab, TYPE_Lab_FLT, display, TYPE_RGB_FLT, proof,
     INTENT_RELATIVE_COLORIMETRIC, INTENT_RELATIVE_COLORIMETRIC, flags);
  _require(proof_transport != NULL && proof_display != NULL, "cannot create proofing transforms");
  cmsHTRANSFORM gamut_transport = cmsCreateProofingTransform
    (lab, TYPE_Lab_FLT, transport, TYPE_RGB_FLT, proof,
     INTENT_RELATIVE_COLORIMETRIC, INTENT_RELATIVE_COLORIMETRIC, flags | cmsFLAGS_GAMUTCHECK);
  cmsHTRANSFORM gamut_display = cmsCreateProofingTransform
    (lab, TYPE_Lab_FLT, display, TYPE_RGB_FLT, proof,
     INTENT_RELATIVE_COLORIMETRIC, INTENT_RELATIVE_COLORIMETRIC, flags | cmsFLAGS_GAMUTCHECK);
  _require(gamut_transport != NULL && gamut_display != NULL, "cannot create gamut-check transforms");

  // lcms 2.17 uses configured alarm colors for float output
  cmsUInt16Number alarm_codes[cmsMAXCHANNELS];
  cmsGetAlarmCodes(alarm_codes);
  const int legacy_alarm = cmsGetEncodedCMMversion() < 2170;
  float alarm[3];
  for(int c = 0; c < 3; c++)
    alarm[c] = legacy_alarm ? -1.0f : alarm_codes[c] / 65535.0f;

  double max_float = 0.0, max_float_interior = 0.0, max_8bit = 0.0;
  double max_proof = 0.0, max_proof_8bit = 0.0;
  double max_srgb_loss = 0.0, max_wrong_description = 0.0, max_proof_effect = 0.0;
  unsigned int outside_srgb = 0, gamut_alarms = 0;
  for(int r = 0; r <= 8; r++)
    for(int g = 0; g <= 8; g++)
      for(int b = 0; b <= 8; b++)
      {
        const float rgb[3] = { r / 8.0f, g / 8.0f, b / 8.0f };
        float pcs[3], direct[3], encoded[3], reconstructed[3];
        cmsDoTransform(source_lab, rgb, pcs, 1);
        cmsDoTransform(lab_display, pcs, direct, 1);
        cmsDoTransform(lab_transport, pcs, encoded, 1);
        cmsDoTransform(transport_display, encoded, reconstructed, 1);
        max_float = fmax(max_float, _delta_e(display_lab, direct, reconstructed));
        if(encoded[0] > 0.01f && encoded[0] < 0.99f
           && encoded[1] > 0.01f && encoded[1] < 0.99f
           && encoded[2] > 0.01f && encoded[2] < 0.99f)
          max_float_interior = fmax(max_float_interior, _delta_e(display_lab, direct, reconstructed));
        for(int c = 0; c < 3; c++) encoded[c] = roundf(_clip(encoded[c]) * 255.0f) / 255.0f;
        cmsDoTransform(transport_display, encoded, reconstructed, 1);
        max_8bit = fmax(max_8bit, _delta_e(display_lab, direct, reconstructed));

        // device RGB described as transport RGB applies the display conversion twice
        cmsDoTransform(transport_display, direct, reconstructed, 1);
        max_wrong_description = fmax(max_wrong_description, _delta_e(display_lab, direct, reconstructed));
        cmsDoTransform(lab_srgb, pcs, encoded, 1);
        int outside = 0;
        for(int c = 0; c < 3; c++)
        {
          outside |= encoded[c] < -0.0001f || encoded[c] > 1.0001f;
          encoded[c] = _clip(encoded[c]);
        }
        outside_srgb += outside;
        cmsDoTransform(srgb_display, encoded, reconstructed, 1);
        max_srgb_loss = fmax(max_srgb_loss, _delta_e(display_lab, direct, reconstructed));

        cmsDoTransform(lab_display, pcs, reconstructed, 1);
        cmsDoTransform(proof_display, pcs, direct, 1);
        max_proof_effect = fmax(max_proof_effect, _delta_e(display_lab, direct, reconstructed));
        cmsDoTransform(proof_transport, pcs, encoded, 1);
        cmsDoTransform(transport_display, encoded, reconstructed, 1);
        max_proof = fmax(max_proof, _delta_e(display_lab, direct, reconstructed));
        for(int c = 0; c < 3; c++) encoded[c] = roundf(_clip(encoded[c]) * 255.0f) / 255.0f;
        cmsDoTransform(transport_display, encoded, reconstructed, 1);
        max_proof_8bit = fmax(max_proof_8bit, _delta_e(display_lab, direct, reconstructed));

        cmsDoTransform(gamut_display, pcs, direct, 1);
        cmsDoTransform(gamut_transport, pcs, encoded, 1);
        const int display_alarm = fabsf(direct[0] - alarm[0]) < 1e-6f
                                  && fabsf(direct[1] - alarm[1]) < 1e-6f
                                  && fabsf(direct[2] - alarm[2]) < 1e-6f;
        const int transport_alarm = fabsf(encoded[0] - alarm[0]) < 1e-6f
                                    && fabsf(encoded[1] - alarm[1]) < 1e-6f
                                    && fabsf(encoded[2] - alarm[2]) < 1e-6f;
        _require(display_alarm == transport_alarm, "transport changed the softproof gamut warning");
        gamut_alarms += transport_alarm;
      }

  printf("%s: max deltaE00 float %.6f, 8-bit %.6f, proof %.6f, proof 8-bit %.6f, "
         "sRGB clip %.6f, wrong description %.6f, proof effect %.6f; "
         "%u/729 colors outside sRGB, %u gamut alarms\n",
         name, max_float, max_8bit, max_proof, max_proof_8bit,
         max_srgb_loss, max_wrong_description, max_proof_effect, outside_srgb, gamut_alarms);
  // the P3 red primary lies just outside Rec2020; gamma22 clips its small negative blue component
  _require(max_float < (check_negative_control ? 0.25 : 2.0),
           "float transport changed the displayed color");
  _require(max_float_interior < 0.03, "float transport changed colors inside its gamut");
  _require(max_8bit < 2.0, "8-bit transport exceeded deltaE00 tolerance");
  _require(max_proof < 0.03, "transport changed the softproof result");
  _require(max_proof_8bit < 2.0, "8-bit transport changed the softproof result");
  _require(outside_srgb > 0, "reference samples failed to exercise colors outside sRGB");
  _require(gamut_alarms > 0, "reference samples failed to exercise softproof gamut warnings");
  if(check_negative_control)
  {
    _require(max_srgb_loss > 2.0, "reference display failed to expose sRGB clipping");
    _require(max_wrong_description > 2.0, "reference display failed to expose double conversion");
    _require(max_proof_effect > 2.0, "reference proof failed to change colors outside sRGB");
  }

  cmsDeleteTransform(gamut_display);
  cmsDeleteTransform(gamut_transport);
  cmsDeleteTransform(proof_display);
  cmsDeleteTransform(proof_transport);
  cmsDeleteTransform(srgb_display);
  cmsDeleteTransform(lab_srgb);
  cmsDeleteTransform(display_lab);
  cmsDeleteTransform(transport_display);
  cmsDeleteTransform(lab_display);
  cmsDeleteTransform(lab_transport);
  cmsDeleteTransform(source_lab);
  cmsCloseProfile(proof);
  cmsCloseProfile(srgb);
  cmsCloseProfile(lab);
}

int main(int argc, char **argv)
{
  _require(argc <= 2, "usage: display-transport-test [display.icc]");
  cmsHPROFILE transport = dt_display_transport_create_profile();
  _require(transport != NULL, "cannot create darktable transport profile");
  const double transport_xy[3][2] = { { 0.708, 0.292 }, { 0.170, 0.797 }, { 0.131, 0.046 } };
  _check_encoding(transport, transport_xy);
  cmsHPROFILE ui = dt_display_ui_create_profile();
  _require(ui != NULL, "cannot create darktable UI profile");
  const double ui_xy[3][2] = { { 0.640, 0.330 }, { 0.300, 0.600 }, { 0.150, 0.060 } };
  _check_encoding(ui, ui_xy);
  _check_ui_composition(transport, ui);

  const cmsCIExyYTRIPLE p3_primaries = {
    { 0.680, 0.320, 1.0 }, { 0.265, 0.690, 1.0 }, { 0.150, 0.060, 1.0 }
  };
  const cmsCIExyYTRIPLE adobe_primaries = {
    { 0.640, 0.330, 1.0 }, { 0.210, 0.710, 1.0 }, { 0.150, 0.060, 1.0 }
  };
  const double gamma[3] = { 563.0 / 256.0, 563.0 / 256.0, 563.0 / 256.0 };
  const double asymmetric_gamma[3] = { 2.1, 2.3, 2.5 };
  const double srgb_parameters[5] = { 2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045 };
  cmsToneCurve *srgb_curve = cmsBuildParametricToneCurve(NULL, 4, srgb_parameters);
  _require(srgb_curve != NULL, "cannot create reference sRGB tone curve");
  cmsToneCurve *p3_curves[3] = { srgb_curve, srgb_curve, srgb_curve };
  const cmsCIExyY white = { 0.3127, 0.3290, 1.0 };
  cmsHPROFILE p3 = cmsCreateRGBProfile(&white, &p3_primaries, p3_curves);
  cmsFreeToneCurve(srgb_curve);
  _require(p3 != NULL, "cannot create reference Display P3 profile");
  cmsHPROFILE adobe = _rgb_profile(&adobe_primaries, gamma);
  const cmsCIExyYTRIPLE display_primaries = {
    { 0.690, 0.310, 1.0 }, { 0.210, 0.730, 1.0 }, { 0.145, 0.055, 1.0 }
  };
  cmsHPROFILE display = _rgb_profile(&display_primaries, asymmetric_gamma);
  _check_composition(p3, transport, display, "synthetic display, P3", 1);
  _check_composition(adobe, transport, display, "synthetic display, Adobe RGB", 1);
  cmsCloseProfile(display);
  if(argc == 2)
  {
    display = cmsOpenProfileFromFile(argv[1], "r");
    _require(display != NULL, "cannot open supplied display ICC profile");
    _require(cmsGetColorSpace(display) == cmsSigRgbData, "supplied display profile must use RGB");
    _check_composition(p3, transport, display, "supplied ICC, P3", 0);
    _check_composition(adobe, transport, display, "supplied ICC, Adobe RGB", 0);
    cmsCloseProfile(display);
  }
  cmsCloseProfile(adobe);
  cmsCloseProfile(p3);
  cmsCloseProfile(ui);
  cmsCloseProfile(transport);
  return EXIT_SUCCESS;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
