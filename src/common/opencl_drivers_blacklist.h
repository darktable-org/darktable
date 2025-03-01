/*
    This file is part of darktable,
    Copyright (C) 2016-2025 darktable developers.

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

#include <string.h>

// FIXME: in the future, we may want to also take DRIVER_VERSION into account
static const gchar *bad_opencl_drivers[] =
{
  // clang-format off

  "beignet",
  "pocl",
/*
  Neo was originally blacklisted due to improper cache invalidation, but this has been fixed.
  During the discussion of that issue in pull request 2033, it was hinted that Neo may be still be
  problematic on Windows, so keep it blacklisted there for now

  TODO:  Determine if Windows failures were due to the same cache invalidation issue.
*/
#if defined _WIN32
  "neo",
  "d3d12",
#endif
  NULL

  // clang-format on
};

// returns TRUE if blacklisted
static gboolean _opencl_check_driver_blacklist(const char *device_version)
{
  gchar *device = g_ascii_strdown(device_version, -1);

  for(int i = 0; bad_opencl_drivers[i]; i++)
  {
    if(!g_strrstr(device, bad_opencl_drivers[i])) continue;

    // oops, found in black list
    g_free(device);
    return TRUE;
  }

  // did not find in the black list, guess it's ok.
  g_free(device);
  return FALSE;
}

/*
   darktable OpenCL runtime exceptions
   1. We test for a number of problematic or advantage situations and leave a flag about that
      in the dt_opencl_device_t struct
   2. gboolean dt_opencl_exception(const int devid, const uint32_t mask)
      allows to check for such conditions while processing the pixelpipe and
      possibly using different code or fallbacks
   3. An example can be found in demosaic testing for DT_OPENCL_AMD_APP while using the
      DT_IOP_DEMOSAIC_PPG demosaicer, as that OpenCL code fails for unknown reasons we do
      a fallback to DT_IOP_DEMOSAIC_RCD (which is better anyway).
   4. We could also fallback to CPU code path in such exceptions, in that case we should return
      with DT_OPENCL_DT_EXCEPTION as the error code, that would be reported in the -d opencl log
*/

static void _write_test_exceptions(dt_opencl_device_t *device)
{
  if(!strncasecmp(device->device_version, "OpenCL 2.0 AMD-APP", 18))
  {
    device->exceptions |= DT_OPENCL_AMD_APP;
    dt_print_nts(DT_DEBUG_OPENCL, "   CL EXCEPTION:             DT_OPENCL_AMD_APP\n");
  }
  if(!strncasecmp(device->platform, "NVIDIA CUDA", 11))
  {
    device->exceptions |= DT_OPENCL_ONLY_CUDA;
    dt_print_nts(DT_DEBUG_OPENCL, "   CL EXCEPTION:             DT_OPENCL_ONLY_CUDA\n");
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

