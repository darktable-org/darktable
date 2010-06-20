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
#include "iop/colorout.h"
#include "control/conf.h"
#include "control/control.h"
#include "common/colormatrices.c"

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

cmsHPROFILE
dt_colorspaces_create_srgb_profile(void)
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

// Create the ICC virtual profile for adobe rgb space
cmsHPROFILE
dt_colorspaces_create_adobergb_profile(void)
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

static LPGAMMATABLE
build_linear_gamma(void)
{
  double Parameters[2];

  Parameters[0] = 1.0;
  Parameters[1] = 0;

  return cmsBuildParametricGamma(1024, 1, Parameters);
}

cmsHPROFILE
dt_colorspaces_create_darktable_profile(const char *makermodel)
{
  dt_profiled_colormatrix_t *preset = NULL;
  for(int k=0;k<dt_profiled_colormatrix_cnt;k++)
  {
    if(!strcmp(makermodel, dt_profiled_colormatrices[k].makermodel))
    {
      preset = dt_profiled_colormatrices + k;
      break;
    }
  }
  if(!preset) return NULL;

  const float wxyz = preset->white[0]+preset->white[1]+preset->white[2];
  const float rxyz = preset->rXYZ[0] +preset->rXYZ[1] +preset->rXYZ[2];
  const float gxyz = preset->gXYZ[0] +preset->gXYZ[1] +preset->gXYZ[2];
  const float bxyz = preset->bXYZ[0] +preset->bXYZ[1] +preset->bXYZ[2];
  cmsCIExyY       WP = {preset->white[0]/wxyz, preset->white[1]/wxyz, 1.0};
  cmsCIExyYTRIPLE XYZPrimaries   = {
                                   {preset->rXYZ[0]/rxyz, preset->rXYZ[1]/rxyz, 1.0},
                                   {preset->gXYZ[0]/gxyz, preset->gXYZ[1]/gxyz, 1.0},
                                   {preset->bXYZ[0]/bxyz, preset->bXYZ[1]/bxyz, 1.0}
                                   };
  LPGAMMATABLE Gamma[3];
  cmsHPROFILE  hp;
 
  Gamma[0] = Gamma[1] = Gamma[2] = build_linear_gamma();
           
  hp = cmsCreateRGBProfile(&WP, &XYZPrimaries, Gamma);
  cmsFreeGamma(Gamma[0]);
  if (hp == NULL) return NULL;
      
  char name[512];
  snprintf(name, 512, "Darktable profiled %s", makermodel);
  cmsAddTag(hp, icSigDeviceMfgDescTag,      (LPVOID) "(dt internal)");
  cmsAddTag(hp, icSigDeviceModelDescTag,    (LPVOID) name);
  cmsAddTag(hp, icSigProfileDescriptionTag, (LPVOID) name);

  return hp;
}

cmsHPROFILE
dt_colorspaces_create_xyz_profile(void)
{
  cmsCIExyY       D65;
  cmsCIExyYTRIPLE XYZPrimaries   = {
                                   {1.0, 0.0, 1.0},
                                   {0.0, 1.0, 1.0},
                                   {0.0, 0.0, 1.0}
                                   };
  LPGAMMATABLE Gamma[3];
  cmsHPROFILE  hXYZ;
 
  cmsWhitePointFromTemp(6504, &D65);
  Gamma[0] = Gamma[1] = Gamma[2] = build_linear_gamma();
           
  hXYZ = cmsCreateRGBProfile(&D65, &XYZPrimaries, Gamma);
  cmsFreeGamma(Gamma[0]);
  if (hXYZ == NULL) return NULL;
      
  cmsAddTag(hXYZ, icSigDeviceMfgDescTag,      (LPVOID) "(dt internal)");
  cmsAddTag(hXYZ, icSigDeviceModelDescTag,    (LPVOID) "linear XYZ");

  // This will only be displayed when the embedded profile is read by for example GIMP
  cmsAddTag(hXYZ, icSigProfileDescriptionTag, (LPVOID) "Darktable linear XYZ");
        
  return hXYZ;
}

cmsHPROFILE
dt_colorspaces_create_linear_rgb_profile(void)
{
  cmsCIExyY       D65;
  cmsCIExyYTRIPLE Rec709Primaries = {
                                   {0.6400, 0.3300, 1.0},
                                   {0.3000, 0.6000, 1.0},
                                   {0.1500, 0.0600, 1.0}
                                   };
  LPGAMMATABLE Gamma[3];
  cmsHPROFILE  hsRGB;
 
  cmsWhitePointFromTemp(6504, &D65);
  Gamma[0] = Gamma[1] = Gamma[2] = build_linear_gamma();
           
  hsRGB = cmsCreateRGBProfile(&D65, &Rec709Primaries, Gamma);
  cmsFreeGamma(Gamma[0]);
  if (hsRGB == NULL) return NULL;
      
  cmsAddTag(hsRGB, icSigDeviceMfgDescTag,      (LPVOID) "(dt internal)");
  cmsAddTag(hsRGB, icSigDeviceModelDescTag,    (LPVOID) "linear rgb");

  // This will only be displayed when the embedded profile is read by for example GIMP
  cmsAddTag(hsRGB, icSigProfileDescriptionTag, (LPVOID) "Darktable linear RGB");
        
  return hsRGB;
}

cmsHPROFILE
dt_colorspaces_create_linear_infrared_profile(void)
{
  // linear rgb with r and b swapped:
  cmsCIExyY       D65;
  cmsCIExyYTRIPLE Rec709Primaries = {
                                   {0.1500, 0.0600, 1.0},
                                   {0.3000, 0.6000, 1.0},
                                   {0.6400, 0.3300, 1.0}
                                   };
  LPGAMMATABLE Gamma[3];
  cmsHPROFILE  hsRGB;
 
  cmsWhitePointFromTemp(6504, &D65);
  Gamma[0] = Gamma[1] = Gamma[2] = build_linear_gamma();
           
  hsRGB = cmsCreateRGBProfile(&D65, &Rec709Primaries, Gamma);
  cmsFreeGamma(Gamma[0]);
  if (hsRGB == NULL) return NULL;
      
  cmsAddTag(hsRGB, icSigDeviceMfgDescTag,      (LPVOID) "(dt internal)");
  cmsAddTag(hsRGB, icSigDeviceModelDescTag,    (LPVOID) "linear infrared bgr");

  // This will only be displayed when the embedded profile is read by for example GIMP
  cmsAddTag(hsRGB, icSigProfileDescriptionTag, (LPVOID) "Darktable linear infrared BGR");
        
  return hsRGB;
}

cmsHPROFILE
dt_colorspaces_create_output_profile(const int imgid)
{
  char profile[1024];
  profile[0] = '\0';
  // db lookup colorout params, and dt_conf_() for override
  gchar *overprofile = dt_conf_get_string("plugins/lighttable/export/iccprofile");
  if(!overprofile || !strcmp(overprofile, "image"))
  {
    const dt_iop_colorout_params_t *params;
    // sqlite:
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(darktable.db, "select op_params from history where imgid=?1 and operation='colorout'", -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, imgid);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      params = sqlite3_column_blob(stmt, 0);
      strncpy(profile, params->iccprofile, 1024);
    }
    sqlite3_finalize(stmt);
  }
  if(!overprofile && profile[0] == '\0')
    strncpy(profile, "sRGB", 1024);
  if(profile[0] == '\0')
    strncpy(profile, overprofile, 1024);
  g_free(overprofile);

  cmsHPROFILE output = NULL;

  if(!strcmp(profile, "sRGB"))
    output = dt_colorspaces_create_srgb_profile();
  if(!strcmp(profile, "linear_rgb"))
    output = dt_colorspaces_create_linear_rgb_profile();
  else if(!strcmp(profile, "XYZ"))
    output = dt_colorspaces_create_xyz_profile();
  else if(!strcmp(profile, "adobergb"))
    output = dt_colorspaces_create_adobergb_profile();
  else if(!strcmp(profile, "X profile") && darktable.control->xprofile_data)
    output = cmsOpenProfileFromMem(darktable.control->xprofile_data, darktable.control->xprofile_size);
  else
  { // else: load file name
    char datadir[1024];
    char filename[1024];
    dt_get_datadir(datadir, 1024);
    snprintf(filename, 1024, "%s/color/out/%s", datadir, profile);
    output = cmsOpenProfileFromFile(filename, "r");
  }
  if(!output) output = dt_colorspaces_create_srgb_profile();
  return output;
}

