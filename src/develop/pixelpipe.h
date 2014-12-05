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
#ifndef DT_DEV_PIXELPIPE_H
#define DT_DEV_PIXELPIPE_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

typedef enum dt_dev_pixelpipe_type_t
{
  DT_DEV_PIXELPIPE_NONE = 0,
  DT_DEV_PIXELPIPE_EXPORT = 1 << 0,
  DT_DEV_PIXELPIPE_FULL = 1 << 1,
  DT_DEV_PIXELPIPE_PREVIEW = 1 << 2,
  DT_DEV_PIXELPIPE_THUMBNAIL = 1 << 3,
  DT_DEV_PIXELPIPE_ANY = DT_DEV_PIXELPIPE_EXPORT | DT_DEV_PIXELPIPE_FULL | DT_DEV_PIXELPIPE_PREVIEW
                         | DT_DEV_PIXELPIPE_THUMBNAIL
} dt_dev_pixelpipe_type_t;

typedef void dt_iop_params_t;

#ifdef HAVE_GEGL
#include "develop/pixelpipe_gegl.h"
#else
#include "develop/pixelpipe_hb.h"
#endif

#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
