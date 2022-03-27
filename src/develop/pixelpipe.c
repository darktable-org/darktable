/*
    This file is part of darktable,
    Copyright (C) 2009-2020 darktable developers.

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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "develop/pixelpipe_hb.c"

const char *dt_pixelpipe_name(dt_dev_pixelpipe_type_t pipe)
{
  switch(pipe)
  {
    case DT_DEV_PIXELPIPE_NONE: return "NONE";
    case DT_DEV_PIXELPIPE_EXPORT: return "EXPORT";
    case DT_DEV_PIXELPIPE_FULL: return "FULL";
    case DT_DEV_PIXELPIPE_PREVIEW: return "PREVIEW";
    case DT_DEV_PIXELPIPE_PREVIEW2: return "PREVIEW2";
    case DT_DEV_PIXELPIPE_THUMBNAIL: return "THUMBNAIL";
    case DT_DEV_PIXELPIPE_ANY: return "ANY";
    default: return "(unknown)";
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

