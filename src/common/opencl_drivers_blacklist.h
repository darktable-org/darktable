/*
    This file is part of darktable,
    Copyright (C) 2016-2026 darktable developers.

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
#include <strings.h>

// FIXME: in the future, we may want to also take DRIVER_VERSION into account
static const gchar *bad_opencl_drivers[] =
{
  // clang-format off

  "beignet",
  "pocl",
  "clover",
  "amd-app",
/*
  Neo was originally blacklisted due to improper cache invalidation, but this has been fixed.
  Per Issue 20104, enabling neo for Windows.
*/
#if defined _WIN32
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

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

