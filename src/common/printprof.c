/*
    This file is part of darktable,
    Copyright (C) 2014-2020 darktable developers.

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

#include "common/printprof.h"
#include "common/colorspaces.h"
#include "lcms2.h"
#include <glib.h>
#include <unistd.h>

static cmsUInt32Number ComputeOutputFormatDescriptor (cmsUInt32Number dwInput, int OutColorSpace, int bps)
{
    int IsPlanar  = T_PLANAR(dwInput);
    int Channels  = 3;
    int IsFlt     = 0;
    return (FLOAT_SH(IsFlt)|COLORSPACE_SH(OutColorSpace)|PLANAR_SH(IsPlanar)|CHANNELS_SH(Channels)|BYTES_SH(bps));
}

static cmsUInt32Number ComputeFormatDescriptor (int OutColorSpace, int bps)
{
  int IsPlanar  = 0;
  int Channels  = 3;
  int IsFlt = 0;
  return (FLOAT_SH(IsFlt)|COLORSPACE_SH(OutColorSpace)|PLANAR_SH(IsPlanar)|CHANNELS_SH(Channels)|BYTES_SH(bps));
}

int dt_apply_printer_profile(void **in, uint32_t width, uint32_t height, int bpp, cmsHPROFILE hInProfile,
                             cmsHPROFILE hOutProfile, int intent, gboolean black_point_compensation)
{
  cmsHTRANSFORM hTransform;
  cmsUInt32Number wInput, wOutput;
  int OutputColorSpace;

  if(!hOutProfile || !hInProfile)
    return 1;

  wInput = ComputeFormatDescriptor (PT_RGB, (bpp==8?1:2));

  OutputColorSpace = _cmsLCMScolorSpace(cmsGetColorSpace(hOutProfile));
  wOutput = ComputeOutputFormatDescriptor(wInput, OutputColorSpace, 1);

  hTransform = cmsCreateTransform
    (hInProfile,  wInput,
     hOutProfile, wOutput,
     intent,
     black_point_compensation ? cmsFLAGS_BLACKPOINTCOMPENSATION : 0);

  if(!hTransform)
  {
    fprintf(stderr, "error printer profile may be corrupted\n");
    return 1;
  }

  void *out = (void *)malloc((size_t)3 * width * height);

  if(bpp == 8)
  {
    const uint8_t *ptr_in = (uint8_t *)*in;
    uint8_t *ptr_out = (uint8_t *)out;

#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(ptr_in, ptr_out, hTransform, height, width)
#endif
    for(int k=0; k<height; k++)
      cmsDoTransform(hTransform, (const void *)&ptr_in[k*width*3], (void *)&ptr_out[k*width*3], width);
  }
  else
  {
    const uint16_t *ptr_in = (uint16_t *)*in;
    uint8_t *ptr_out = (uint8_t *)out;

#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(ptr_in, ptr_out, hTransform, height, width)
#endif
    for(int k=0; k<height; k++)
      cmsDoTransform(hTransform, (const void *)&ptr_in[k*width*3], (void *)&ptr_out[k*width*3], width);
  }

  cmsDeleteTransform(hTransform);

  free(*in);
  *in = out;

  return 0;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
