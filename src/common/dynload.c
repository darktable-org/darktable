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
#include <stdio.h>
#include <stdlib.h>

#ifdef __APPLE__
#include <sys/malloc.h>
#endif

#include "common/dynload.h"

/* check if gmodules is supported on this platform */
int dt_gmodule_supported(void)
{
  int success = g_module_supported();

  return success;
}


/* dynamically load library */
dt_gmodule_t *dt_gmodule_open(const char *library)
{
  dt_gmodule_t *module = NULL;
  GModule *gmodule;
  char *name;

  name = g_module_build_path(NULL, library);

  gmodule = g_module_open(name, G_MODULE_BIND_LAZY);

  if (gmodule != NULL)
  {
    module = (dt_gmodule_t *)malloc(sizeof(dt_gmodule_t));
    module->gmodule = gmodule;
    module->library = g_module_name(gmodule);
  }

  return module;
}


/* get pointer to symbol */
int dt_gmodule_symbol(dt_gmodule_t *module, const char *name, void (** pointer)(void))
{
  int success = g_module_symbol(module->gmodule, name, (gpointer)pointer);

  return success;
}






