/*
 *    This file is part of darktable,
 *    Copyright (C) 2017-2021 darktable developers.
 *
 *    darktable is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    darktable is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>
#include <gmodule.h>

#include "config.h"
#include "common/file_location.h"
#include "common/module.h"

GList *dt_module_load_modules(const char *subdir, size_t module_size,
                              int (*load_module_so)(void *module, const char *libname, const char *plugin_name),
                              void (*init_module)(void *module),
                              gint (*sort_modules)(gconstpointer a, gconstpointer b))
{
  GList *plugin_list = NULL;
  char plugindir[PATH_MAX] = { 0 };
  const gchar *dir_name;
  dt_loc_get_plugindir(plugindir, sizeof(plugindir));
  g_strlcat(plugindir, subdir, sizeof(plugindir));
  GDir *dir = g_dir_open(plugindir, 0, NULL);
  if(!dir) return NULL;
  const int name_offset = strlen(SHARED_MODULE_PREFIX),
            name_end = strlen(SHARED_MODULE_PREFIX) + strlen(SHARED_MODULE_SUFFIX);
  while((dir_name = g_dir_read_name(dir)))
  {
    // get lib*.so
    if(!g_str_has_prefix(dir_name, SHARED_MODULE_PREFIX)) continue;
    if(!g_str_has_suffix(dir_name, SHARED_MODULE_SUFFIX)) continue;
    char *plugin_name = g_strndup(dir_name + name_offset, strlen(dir_name) - name_end);
    void *module = calloc(1, module_size);
    gchar *libname = g_module_build_path(plugindir, plugin_name);
    int res = load_module_so(module, libname, plugin_name);
    g_free(libname);
    g_free(plugin_name);
    if(res)
    {
      free(module);
      continue;
    }
    plugin_list = g_list_prepend(plugin_list, module);

    if(init_module) init_module(module);
  }
  g_dir_close(dir);

  if(sort_modules)
    plugin_list = g_list_sort(plugin_list, sort_modules);
  else
    plugin_list = g_list_reverse(plugin_list);  // list was built in reverse order, so un-reverse it

 return plugin_list;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

