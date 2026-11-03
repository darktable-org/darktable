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

int main(void)
{
  cmsHPROFILE transport = dt_display_transport_create_profile();
  _require(transport != NULL, "cannot create darktable transport profile");
  const double transport_xy[3][2] = { { 0.708, 0.292 }, { 0.170, 0.797 }, { 0.131, 0.046 } };
  _check_encoding(transport, transport_xy);
  cmsCloseProfile(transport);

  cmsHPROFILE ui = dt_display_ui_create_profile();
  _require(ui != NULL, "cannot create darktable UI profile");
  const double ui_xy[3][2] = { { 0.640, 0.330 }, { 0.300, 0.600 }, { 0.150, 0.060 } };
  _check_encoding(ui, ui_xy);
  cmsCloseProfile(ui);
  return EXIT_SUCCESS;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
