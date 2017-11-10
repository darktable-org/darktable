/*
    This file is part of darktable,
    copyright (c) 2017 Ronny Kahl.

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

#include <glib.h>
#include <sqlite3.h>
#include <stdint.h>

typedef enum dt_rev_geocode_status_t
{
  DT_REV_GEOCODE_STATUS_SUCCESS,
  DT_REV_GEOCODE_STATUS_FAIL,
  DT_REV_GEOCODE_STATUS_CONNECT_ERROR,
  DT_REV_GEOCODE_STATUS_REMOVED,
  DT_REV_GEOCODE_STATUS_NOTHINGTODO
} dt_rev_geocode_status_t;

/** Perform a reverse geocode (find location name). \param[in] imgid the image id to reverse geocode
 *  \return if the lookup has been performed. */
dt_rev_geocode_status_t dt_rev_geocode(gint imgid, gboolean perform_lookup);

/** Perform a reverse geocode (find location name) on geotagged images with no location set or vice versa.
 *  Usually called during startup. */
void dt_rev_geocode_startup();

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
