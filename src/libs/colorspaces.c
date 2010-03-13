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

#include <lcms.h>

static LPGAMMATABLE
build_srgb_gamma(void)
{
  double Parameters[5];

  Parameters[0] = 2.4;
  Parameters[1] = 1. / 1.055;
  Parameters[2] = 0.055 / 1.055;
  Parameters[3] = 1. / 12.92;
  Parameters[4] = 0.04045;    // d

  return cmsBuildParametricGamma(1024, 4, Parameters);
}

cmsHPROFILE LCMSEXPORT 
create_srgb_profile(void)
{
  cmsCIExyY       D65;
  cmsCIExyYTRIPLE Rec709Primaries = {
                                   {0.6400, 0.3300, 1.0},
                                   {0.3000, 0.6000, 1.0},
                                   {0.1500, 0.0600, 1.0}
                                   };
  LPGAMMATABLE Gamma22[3];
  cmsHPROFILE  hsRGB;
 
  cmsWhitePointFromTemp(6504, &D65);
  Gamma22[0] = Gamma22[1] = Gamma22[2] = build_srgb_gamma();
           
  hsRGB = cmsCreateRGBProfile(&D65, &Rec709Primaries, Gamma22);
  cmsFreeGamma(Gamma22[0]);
  if (hsRGB == NULL) return NULL;
      
  cmsAddTag(hsRGB, icSigDeviceMfgDescTag,      (LPVOID) "(dt internal)");
  cmsAddTag(hsRGB, icSigDeviceModelDescTag,    (LPVOID) "sRGB");

  // This will only be displayed when the embedded profile is read by for example GIMP
  cmsAddTag(hsRGB, icSigProfileDescriptionTag, (LPVOID) "Darktable sRGB");
        
  return hsRGB;
}

static LPGAMMATABLE 
build_adobergb_gamma(void)
{
  // this is wrong, this should be a TRC not a table gamma
  double Parameters[2];

  Parameters[0] = 2.2;
  Parameters[1] = 0;

  return cmsBuildParametricGamma(1024, 1, Parameters);
}

// Create the ICC virtual profile for sRGB space
cmsHPROFILE LCMSEXPORT 
create_adobergb_profile(void)
{
  cmsCIExyY       D65;
  cmsCIExyYTRIPLE AdobePrimaries = {
                                   {0.6400, 0.3300, 1.0},
                                   {0.2100, 0.7100, 1.0},
                                   {0.1500, 0.0600, 1.0}
                                   };
  LPGAMMATABLE Gamma22[3];
  cmsHPROFILE  hAdobeRGB;

  cmsWhitePointFromTemp(6504, &D65);
  Gamma22[0] = Gamma22[1] = Gamma22[2] = build_adobergb_gamma();

  hAdobeRGB = cmsCreateRGBProfile(&D65, &AdobePrimaries, Gamma22);
  cmsFreeGamma(Gamma22[0]);
  if (hAdobeRGB == NULL) return NULL;

  cmsAddTag(hAdobeRGB, icSigDeviceMfgDescTag,      (LPVOID) "(dt internal)");
  cmsAddTag(hAdobeRGB, icSigDeviceModelDescTag,    (LPVOID) "AdobeRGB");

  // This will only be displayed when the embedded profile is read by for example GIMP
  cmsAddTag(hAdobeRGB, icSigProfileDescriptionTag, (LPVOID) "Darktable AdobeRGB");

  return hAdobeRGB;
}

