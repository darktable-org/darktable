/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

#include "common/display_transport.h"

static cmsHPROFILE _create_profile(const cmsCIExyYTRIPLE *primaries)
{
  const cmsCIExyY white = { 0.3127, 0.3290, 1.0 };
  // gamma22 is supported by compositors which do not accept the sRGB transfer function
  cmsToneCurve *curve = cmsBuildGamma(NULL, 2.2);
  if(!curve) return NULL;
  cmsToneCurve *curves[3] = { curve, curve, curve };
  cmsHPROFILE profile = cmsCreateRGBProfile(&white, primaries, curves);
  cmsFreeToneCurve(curve);
  return profile;
}

cmsHPROFILE dt_display_transport_create_profile(void)
{
  const cmsCIExyYTRIPLE primaries = {
    { 0.708, 0.292, 1.0 },
    { 0.170, 0.797, 1.0 },
    { 0.131, 0.046, 1.0 }
  };
  return _create_profile(&primaries);
}

cmsHPROFILE dt_display_ui_create_profile(void)
{
  const cmsCIExyYTRIPLE primaries = {
    { 0.640, 0.330, 1.0 },
    { 0.300, 0.600, 1.0 },
    { 0.150, 0.060, 1.0 }
  };
  return _create_profile(&primaries);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
