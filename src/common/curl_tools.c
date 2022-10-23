/*
    This file is part of darktable,
    Copyright (C) 2019-2020 darktable developers.

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

    part of this file is based on nikon_curve.h from UFraw
    Copyright 2004-2008 by Shawn Freeman, Udi Fuchs
*/

#include "common/darktable.h"
#include "common/curl_tools.h"
#include "common/file_location.h"
#include "control/control.h"

void dt_curl_init(CURL *curl, gboolean verbose)
{
  curl_easy_reset(curl);

  char useragent[64];
  snprintf(useragent, sizeof(useragent), "darktable/%s", darktable_package_version);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, useragent);

  char datadir[PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));
  gchar *crtfilename = g_build_filename(datadir, "..", "curl", "curl-ca-bundle.crt", NULL);
  if(g_file_test(crtfilename, G_FILE_TEST_EXISTS)) curl_easy_setopt(curl, CURLOPT_CAINFO, crtfilename);
  g_free(crtfilename);

  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

  if(verbose)
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

