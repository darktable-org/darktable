/*
    This file is part of darktable,
    copyright (c) 2011 henrik andersson

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

struct dt_gpx_t;

/* loads and parses a gpx track file */
struct dt_gpx_t *dt_gpx_new(const gchar *filename);
void dt_gpx_destroy(struct dt_gpx_t *);

/* fetch the lon,lat coords for time t, if within time range
  of gpx record return TRUE, FALSE is returned if out of time frame
  and closest record of lon,lat is filled */
gboolean dt_gpx_get_location(struct dt_gpx_t *, GTimeVal *timestamp, gdouble *lon, gdouble *lat, gdouble *ele);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
