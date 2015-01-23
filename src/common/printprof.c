/*
    This file is part of darktable,
    copyright (c) 2014-2015 pascal obry

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

    NOTE: this code is highly inspired from the tifficc tool from Little-CMS
*/

#include <glib.h>
#include <unistd.h>
#include "lcms2_plugin.h"
#include "common/printprof.h"
#include "common/colorspaces.h"

int dt_apply_printer_profile(int imgid, void **in, uint32_t width, uint32_t height, int bpp, const char *profile, int intent)
{
  cmsHPROFILE hInProfile, hOutProfile;
  cmsHTRANSFORM hTransform;

  if(!g_file_test(profile, G_FILE_TEST_IS_REGULAR))
    return 1;

  void *out = (void *)malloc(width*height*3);

  hInProfile = dt_colorspaces_create_output_profile(imgid);
  hOutProfile = cmsOpenProfileFromFile(profile, "r");

  hTransform = cmsCreateTransform
    (hInProfile,  (bpp==8 ? TYPE_BGR_8 : TYPE_BGR_16),
     hOutProfile, TYPE_BGR_8,
     intent, 0);

  cmsCloseProfile(hInProfile);
  cmsCloseProfile(hOutProfile);

  if (bpp == 8)
  {
    const uint8_t *ptr_in = (uint8_t *)*in;
    uint8_t *ptr_out = (uint8_t *)out;

    for (int k=0; k<height; k++)
      cmsDoTransform(hTransform, (const void *)&ptr_in[k*width*3], (void *)&ptr_out[k*width*3], width);
  }
  else
  {
    const uint16_t *ptr_in = (uint16_t *)*in;
    uint8_t *ptr_out = (uint8_t *)out;

    for (int k=0; k<height; k++)
      cmsDoTransform(hTransform, (const void *)&ptr_in[k*width*3], (void *)&ptr_out[k*width*3], width);
  }

  cmsDeleteTransform(hTransform);

  free(*in);
  *in = out;

  return 0;
}
