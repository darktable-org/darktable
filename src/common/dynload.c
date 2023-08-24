/*
    This file is part of darktable,
    Copyright (C) 2011-2023 darktable developers.

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

#ifdef HAVE_OPENCL

#include <stdlib.h>
#ifndef __APPLE__
#include <stdio.h>
#include <string.h>
#else //!__APPLE__
#include <dlfcn.h>
#endif //!__APPLE__

#include "common/dynload.h"
#include "common/darktable.h"

#ifndef __APPLE__
/* check if gmodules is supported on this platform */
gboolean dt_gmodule_supported(void)
{
  return g_module_supported();
}


/* dynamically load library */
dt_gmodule_t *dt_gmodule_open(const char *library)
{
  dt_gmodule_t *module = NULL;
  GModule *gmodule;
  char *name;

  if(strchr(library, '/') == NULL)
  {
    name = g_module_build_path(NULL, library);
  }
  else
  {
    name = g_strdup(library);
  }

  gmodule = g_module_open(name, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);

  if(gmodule != NULL)
  {
    module = (dt_gmodule_t *)malloc(sizeof(dt_gmodule_t));
    module->gmodule = gmodule;
    module->library = name;
  }
  else
    g_free(name);

  return module;
}


/* get pointer to symbol and return flag in case off success */
gboolean dt_gmodule_symbol(dt_gmodule_t *module, const char *name, void (**pointer)(void))
{
  // As in g_module_symbol() reference the returned pointer might be NULL
  // so we also check for a non-NULL pointer for returned success 
  const gboolean symbol = g_module_symbol(module->gmodule, name, (gpointer)pointer);
  const gboolean valid = *pointer != NULL;
  if(!(symbol && valid))
    dt_print(DT_DEBUG_OPENCL, "[opencl init] missing symbol `%s` in library`\n", name);
  return symbol && valid ? TRUE : FALSE;
}
#else //!__APPLE__
/* check if gmodules is supported on this platform */
gboolean dt_gmodule_supported(void)
{
  return TRUE;
}

/* dynamically load library */
dt_gmodule_t *dt_gmodule_open(const char *library)
{
  // logic here is simplified since it's used only specifically for OpenCL
  dt_gmodule_t *module = NULL;
  void *gmodule = dlopen(library, RTLD_LAZY | RTLD_LOCAL);

  if(gmodule != NULL)
  {
    module = (dt_gmodule_t *)malloc(sizeof(dt_gmodule_t));
    module->gmodule = gmodule;
    module->library = g_strdup(library);
  }

  return module;
}

/* get pointer to symbol */
gboolean dt_gmodule_symbol(dt_gmodule_t *module, const char *name, void (**pointer)(void))
{
  *pointer = dlsym(module->gmodule, name);
  const gboolean valid = *pointer != NULL;
  if(!valid)
    dt_print(DT_DEBUG_OPENCL, "[opencl init] missing symbol `%s` in library`\n", name);

  return valid;
}
#endif //!__APPLE__
#endif //HAVE_OPENCL

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

