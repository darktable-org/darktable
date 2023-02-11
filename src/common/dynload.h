/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

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

#ifdef HAVE_OPENCL

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifndef __APPLE__
#include <glib.h>
#include <gmodule.h>
#endif //!__APPLE__

typedef struct dt_gmodule_t
{
#ifndef __APPLE__
  GModule *gmodule;
#else
  void *gmodule;
#endif
  char *library;
} dt_gmodule_t;


/* check if gmodules is supported on this platform */
int dt_gmodule_supported(void);

/* dynamically load library */
dt_gmodule_t *dt_gmodule_open(const char *);

/* get pointer to function */
int dt_gmodule_symbol(dt_gmodule_t *, const char *, void (**)(void));

#endif // HAVE_OPENCL

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
