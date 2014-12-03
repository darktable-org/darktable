/*
    This file is part of darktable,
    copyright (c) 2011 Ulrich Pegelow

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

#ifndef DT_DYNLOAD_H
#define DT_DYNLOAD_H

#ifdef HAVE_OPENCL

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib.h>
#include <gmodule.h>

typedef struct dt_gmodule_t
{
  GModule *gmodule;
  char *library;
} dt_gmodule_t;


/* check if gmodules is supported on this platform */
int dt_gmodule_supported(void);

/* dynamically load library */
dt_gmodule_t *dt_gmodule_open(const char *);

/* get pointer to function */
int dt_gmodule_symbol(dt_gmodule_t *, const char *, void (**)(void));


#endif
#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
