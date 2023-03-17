/*
    This file is part of darktable,
    Copyright (C) 2016-2021 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <glib.h>

#undef OPTIONAL
#undef REQUIRED
#undef DEFAULT

#undef FULL_API_H

#ifdef INCLUDE_API_FROM_MODULE_LOAD
  #define OPTIONAL(return_type, function_name, ...) \
      if(!g_module_symbol(module->module, #function_name, (gpointer) & (module->function_name))) \
          module->function_name = NULL
  #define REQUIRED(return_type, function_name, ...) \
      if(!g_module_symbol(module->module, #function_name, (gpointer) & (module->function_name))) \
          goto api_h_error
  #define DEFAULT(return_type, function_name, ...) \
      if(!g_module_symbol(module->module, #function_name, (gpointer) & (module->function_name))) \
          module->function_name = default_ ## function_name

  dt_print(DT_DEBUG_CONTROL, "[" INCLUDE_API_FROM_MODULE_LOAD "] loading `%s' from %s\n", module_name, libname);
  module->module = g_module_open(libname, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
  if(!module->module) goto api_h_error;
  int (*version)();
  if(!g_module_symbol(module->module, "dt_module_dt_version", (gpointer) & (version))) goto api_h_error;
  if(version() != dt_version())
  {
    dt_print(DT_DEBUG_ALWAYS,
            "[" INCLUDE_API_FROM_MODULE_LOAD "] `%s' is compiled for another version of dt (module %d (%s) != dt %d (%s)) !\n",
            libname, abs(version()), version() < 0 ? "debug" : "opt", abs(dt_version()),
            dt_version() < 0 ? "debug" : "opt");
    goto api_h_error;
  }
  if(!g_module_symbol(module->module, "dt_module_mod_version", (gpointer) & (module->version))) goto api_h_error;

  goto skip_error;
api_h_error:
  dt_print(DT_DEBUG_ALWAYS, "[" INCLUDE_API_FROM_MODULE_LOAD "] failed to open `%s': %s\n", module_name, g_module_error());
  if(module->module) g_module_close(module->module);
  module->module = NULL;
  return 1;
skip_error:
  #undef INCLUDE_API_FROM_MODULE_LOAD
#elif defined(INCLUDE_API_FROM_MODULE_H)
  #define OPTIONAL(return_type, function_name, ...) return_type (*function_name)(__VA_ARGS__)
  #define REQUIRED(return_type, function_name, ...) return_type (*function_name)(__VA_ARGS__)
  #define DEFAULT(return_type, function_name, ...) return_type (*function_name)(__VA_ARGS__)
  int (*version)();
  #undef INCLUDE_API_FROM_MODULE_H
#elif defined(INCLUDE_API_FROM_MODULE_LOAD_BY_SO)
  #define OPTIONAL(return_type, function_name, ...) module->function_name = so->function_name
  #define REQUIRED(return_type, function_name, ...) module->function_name = so->function_name
  #define DEFAULT(return_type, function_name, ...) module->function_name = so->function_name
  #undef INCLUDE_API_FROM_MODULE_LOAD_BY_SO
#else
  #define FULL_API_H
  #define OPTIONAL(return_type, function_name, ...) return_type function_name(__VA_ARGS__)
  #define REQUIRED(return_type, function_name, ...) return_type function_name(__VA_ARGS__)
  #define DEFAULT(return_type, function_name, ...) return_type function_name(__VA_ARGS__)
  #ifdef __cplusplus
  extern "C" {
  #endif
  // these 2 functions are defined by DT_MODULE() macro.
  #pragma GCC visibility push(default)
  // returns the version of dt's module interface at the time this module was build
  int dt_module_dt_version();
  // returns the version of this module
  int dt_module_mod_version();
  #pragma GCC visibility pop
  #ifdef __cplusplus
  }
  #endif
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
